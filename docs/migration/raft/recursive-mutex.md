# Removing RaftServer's recursive mutex

`RaftServer::mtx_` was a `std::recursive_mutex`. It is a plain `std::mutex`
now, wrapped in `RaftCheckedMutex`, which aborts with a diagnostic if one
thread ever re-enters it.

This is the record of why, how it was verified, what went wrong on the way,
and what residual risk is left. It is written because the change looks
trivial in the diff (one type name) and is not: a recursive mutex tolerates
two quite different things, and removing it makes one of them a deadlock and
the other one loud.

## Why remove it at all

Three reasons, in increasing order of importance.

**It has no Rust spelling.** This is the reason that forced the schedule but
the least interesting one. Rust's `Mutex<T>` has no recursive form, so the
end state this migration is aimed at -- `Mutex<RaftState>` owning the
consensus cluster, with the lock guard handing out the only reference to it
-- cannot be reached while re-entrant acquisition is legal anywhere.

**It hides re-entrancy rather than preventing it.** A recursive mutex makes
"function A holds the lock and calls function B which takes the lock" work.
It does not make it *correct*. The inner critical section observes state that
the outer one was in the middle of mutating, and the outer one resumes with
state the inner one changed. Under a recursive mutex that is silent; the only
symptom is a bug somewhere else, later.

**The one that actually matters here: `recursive_mutex` tracks ownership BY
THREAD, and this runtime schedules fibers that share a thread.** If a critical
section ever suspends -- an RPC send, a `Fiber::sleep`, a wait on an event --
the poll thread runs another fiber. If that fiber calls anything that takes
`mtx_`, `recursive_mutex` sees the same `thread::id` and **grants the lock**.
That is not re-entrancy. That is two logically concurrent fibers inside a
section that is supposed to be exclusive, with no error, no deadlock, and no
way to notice. A plain mutex turns that case into a self-deadlock, which is
loud.

Zero critical sections suspend today, so this was latent rather than live.
But "latent" is exactly the state a recursive mutex is good at maintaining,
and the conversion was going to add and move critical sections.

## How the nesting was actually found

Static analysis was wrong three times, so the nesting was **measured**. A
`thread_local` depth counter was added to every acquisition, logging each
re-entry with its call site, and the full suite was run.

| stage | nested acquisitions observed |
|---|---|
| baseline | **36,327** |
| after splitting four functions | 79 |
| after fixing the transitive callers | 0 |

The baseline breakdown was `IsLeader` 36,099, `resetTimer` 196, `CompactLog`
30, `PublishAppliedIndex` 2. The remaining 79 were all `resetTimer`, reached
through `setIsLeader`, `doVote` and `stepDown`.

`CompactLog` is the one a static scan never finds. It nests only from
`CreateSnapshotLocked`, which takes no lock itself -- its contract is
caller-holds -- so a "is a lock held at this point" scan sees nothing at the
call site.

**And the probe was wrong too.** Reaching zero instrumented nestings,
demoting to `std::mutex` and running the suite produced a deadlock at case 16
of 25. The probe had only instrumented single-line `lock_guard` acquisitions,
so two `unique_lock` acquisitions of `mtx_` (in `RequestVoteImpl` and
`StartImpl`) were invisible to it.

That is the argument for demoting the mutex in the same change rather than
afterwards: **a deadlock is a better oracle than any scan.** With the
recursive mutex still in place, a missed nesting stays silent forever. With
it gone, the first missed one stops the test.

**The real culprit was `test.cc`, not `server.cc`.** Twenty-three sites where
the lab test holds `server->mtx_` and then calls an accessor that re-acquires
it -- `GetSnapshotIndex`, `GetSnapshotTerm`, `SetSnapshotThreshold`,
`SetSnapshotManager`. Harmless under recursion, instant self-deadlock without
it. The first systematic scan missed every one because its negative lookbehind
excluded `>`, so no `server->Method()` call matched at all. **Test code
participates in the lock discipline and has to be enumerated together with
production code**, not after it.

## The shape that replaced it

The acquire-wrapper / `...Locked` split, which this codebase already used by
hand in `CreateSnapshotLocked` and `ElectionLastLogTermLocked`. Eight
functions were split: `IsLeader`, `resetTimer`, `CompactLog`,
`PublishAppliedIndex`, `GetSnapshotIndex`, `GetSnapshotTerm`,
`SetSnapshotThreshold`, `SetSnapshotManager`.

Each keeps an acquiring entry point for callers that hold nothing, and gains
a `...Locked` body for callers that already hold `mtx_`:

```cpp
uint64_t GetSnapshotIndexLocked() const { return state_.snapidx_; }   // caller-holds
uint64_t GetSnapshotIndex()            { lock; return GetSnapshotIndexLocked(); }
```

The pair is not boilerplate. It is the contract made syntactic: a caller
picks the name that matches what it already holds, and picking wrong is a
deadlock at the first execution rather than a subtle interleaving at some
later one. Seventeen methods in `server.h` now carry an explicit
`CALLER MUST HOLD mtx_` contract.

After the Rust conversion the same split survives, with the guard spelled in
Rust:

```rust
pub fn GetSnapshotIndexLocked(&self) -> u64 { self.state_.snapidx_ }

pub fn GetSnapshotIndex(&mut self) -> u64 {
    let _lock = RaftLockGuard::new(&mut self.mtx_);
    self.GetSnapshotIndexLocked()
}
```

`RaftLockGuard` acquires in `new` and releases in `Drop`, which reproduces
`std::lock_guard`'s scope exactly -- early returns included. There are 33
such acquisitions in the Rust today and 2 remaining `std::lock_guard` ones in
C++, plus 24 in `test.cc`.

## The always-on check

Splitting the functions closed every path *inside* RaftServer. It could not
close the two that leave it, so the mutex itself now reports the failure.

```cpp
class RaftCheckedMutex {
 public:
  void lock() {
    const std::thread::id self = std::this_thread::get_id();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ReportReentry();                       // [[noreturn]], prints then aborts
    }
    inner_.lock();
    owner_.store(self, std::memory_order_relaxed);
  }
  bool try_lock();   // same check
  void unlock() { owner_.store(std::thread::id{}, ...); inner_.unlock(); }
 private:
  std::mutex inner_{};
  std::atomic<std::thread::id> owner_{};
};

static_assert(std::atomic<std::thread::id>::is_always_lock_free, ...);
```

Four things about this are deliberate:

- **It is always on, not debug-only.** The failure it catches is a hang with
  no stack, no log line and no test that can reach it. A check that is
  compiled out of release builds would be absent from exactly the
  configuration where the bug appears.
- **The cost is one relaxed atomic load and one relaxed store per
  acquisition**, on a word that is already in this core's cache because the
  same thread wrote it last. The `static_assert` exists so that a platform
  where `atomic<thread::id>` is not lock-free -- where the check would take
  an internal lock and cost far more than intended -- fails the build instead
  of quietly paying for it.
- **The relaxed ordering is sufficient.** `owner_` is only ever compared
  against the reading thread's own id. A stale value written by another
  thread can only be *some other* thread's id or the empty id, neither of
  which matches, so a false positive is impossible; and the only writer of
  the value that could match is this thread itself, whose writes it observes
  in program order. The mutex's own acquire/release still orders the data.
- **The message names the bug rather than the symptom.** It says which
  callbacks are the likely cause and which methods they must not reach.

## What it is guarding: the two escapes

Every path inside RaftServer was checked, and a static walk still finds no
function holding `mtx_` that reaches another taking it. The gap is the two
application-provided `std::function` hooks, both invoked **with `mtx_`
held**:

```
MaybeCreateSnapshot -> CreateSnapshotLocked                -> create_sm_snapshot_cb_
OnInstallSnapshot   -> PrepareStateMachineSnapshotLocked   -> prepare_sm_snapshot_cb_
```

An embedder callback that reaches `GetAppliedIndex`, `GetSnapshotIndex`,
`IsLeader`, `Start` or any other acquirer deadlocks where the recursive mutex
tolerated it. `create_sm_snapshot_cb_` additionally runs on the **apply
thread**, not the poll thread.

Nothing documented this before; `SetStateMachineSnapshotCallbacks` now
carries the contract at the registration point, which is where an embedder
looks.

**The suites cannot exercise a violation.** The only in-tree registrations
are in `test.cc` and are pure. Their passing was never evidence for this
path, and the comment on the method says so. That is precisely why the check
is in the mutex rather than in a test.

## The one genuine near-miss: the InstallSnapshot completion callback

There is a third path, and it is the one that came closest to biting.

`RaftCommo::SendInstallSnapshot` normally invokes its completion callback
from the reactor when the reply lands, with `mtx_` not held. But when
`PeerForSite` returns null (`commo.cc:167-170`) it invokes the callback
**inline, on the caller's stack** -- and that caller is heartbeat PHASE 1,
which holds `mtx_`. A recursive mutex tolerated the re-entry. A plain one
would self-deadlock.

What saves it is where the lock sits, not luck:

```cpp
[...](uint64_t follower_term) {
  std::lock_guard<std::mutex> lifetime_lock(callback_lifetime->mutex);
  auto* server = callback_lifetime->server;
  if (server == nullptr) return;

  // THE LOCK IS TAKEN BELOW THIS CHECK, NOT ABOVE IT.
  if (!raft_server_install_snapshot_reply_is_available(follower_term)) {
    Log_warn(...);
    return;                       // <-- the inline path always returns here
  }
  server->InstallSnapshotReplyAccepted(...);   // <-- this is what takes mtx_
}
```

The inline path always passes `follower_term == 0`, so it takes the
availability branch and returns having touched no Raft state at all.
Acquiring *after* the check means the synchronous context never reaches the
lock, and every path that does reach it is the asynchronous one.

This is a real invariant held together by statement order, so it is worth
noting that the Rust conversion made it slightly harder to break by accident:
everything past the availability check is now
`RaftServerBase::InstallSnapshotReplyAccepted`, which takes `mtx_` itself.
The ordering the comment describes is expressed by *where the call sits*
rather than by where a guard happens to be declared, and moving the guard
earlier is no longer a one-line edit.

## Is there a liveness risk?

This was asked directly, and the honest answer has two parts.

**No new deadlock cycle.** A deadlock needs either a cycle across two or more
locks, or self-re-entry. `mtx_` is never held while acquiring another Raft
lock in the opposite order -- the global order is
`state_machine_apply_mtx_` -> `mtx_` -> `apply_queue_mtx_`, and every site
that takes more than one takes them in that order. Self-re-entry is what the
check catches, and it aborts rather than hangs.

**A recursive mutex was never protecting liveness anyway.** It converts a
would-be deadlock into a silent correctness hazard. Removing it does not
create a hang that was not already a bug; it converts a class of latent
bugs into immediate, diagnosable failures. The two ways that plays out:

- a nesting inside RaftServer -- all found and removed, verified by
  instrumentation to zero and then by the suite passing with a plain mutex;
- a nesting through an embedder callback -- unreachable by any test, now
  reported by the mutex with a message naming the cause.

The residual risk is a **fiber suspension inside a critical section**, which
would now self-deadlock rather than silently admit a second fiber. No
critical section suspends today. If one is ever added, it deadlocks
immediately and visibly, which is the outcome this change is for.

## Commits

| commit | what it did |
|---|---|
| `340bd3d37` | dissolved `TxLogServer`'s implementation inheritance, moving `mtx_` down into RaftServer |
| `26e9107bd` | recorded why the mutex was right before it was wrong |
| `09be9181d` | removed the recursive mutex: eight function splits, the instrumentation, the `test.cc` sites |
| `fc023f0a6` | documented the re-entrancy contract the demotion created, at the registration point |
| `bda58df50` | made re-entry abort with a message instead of hanging (`RaftCheckedMutex`) |

## What would retire this class

`RaftCheckedMutex` is a guard rail around a discipline that is currently
maintained by hand. The structural fix is `Mutex<RaftState>`: make the lock
*own* the consensus cluster, so the only way to reach `state_` is through a
guard, and "held or not held" stops being a contract in a comment and becomes
the type of the reference you have. At that point the `...Locked` suffix
pairs collapse into taking a `&mut RaftState` parameter, the check has
nothing left to catch from inside the server, and only the embedder-callback
escape remains -- which is a documentation problem, not a locking one.
