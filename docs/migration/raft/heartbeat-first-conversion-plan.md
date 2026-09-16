# Making `RaftServer` a Rust struct

Rewritten 2026-09-16, replacing the heartbeat-first plan entirely. That document
aimed at a pure DSL `HeartbeatDecision` called from a C++ driver; the target is
now the inverse, and most of it has been executed, so the old text described
neither where we are nor where we are going.

Same evidence discipline as before. **MEASURED** means a command was run
against the tree at `7544d881a`; **INFERRED** is reasoning from those
measurements; **UNTESTED** is a hypothesis with a named check. The discipline
has earned its keep: this migration has now falsified four claims from the
original plan, two from its successor, and three of my own, every one of them
on contact with the transpiler rather than in review.

## 0. Where we actually are

Eleven commits of conversion have landed. What is Rust today:

| type | what it owns |
|---|---|
| `ElectionTimerLoop` | the election timer's whole `while`, its campaign branch and vote wait |
| `HeartbeatDriver` | the heartbeat loop, its lifecycle, and PHASE 0-3 sequencing |
| `HeartbeatRoundScope` | term, generation, membership, commit index for one round |
| `PeerTable` | all peer progress, dense-ordinal, plus the majority-commit selection |
| `AuthorityLedger` | read-index generations, and PHASE 3's whole settle scan |
| `PendingTable` | in-flight AppendEntries, carrying two wire types opaquely |
| `ReplicationWakeGate`, `HeartbeatAuthority`, `FollowerProgress` | earlier work |

Four mechanics are settled and should not be re-litigated:

- **Opaque carry works, with no gate relaxation.** `*mut core::ffi::c_void`
  emits as `void*`; `unsafe extern "C"` blocks emit as `extern "C"`
  declarations. A DSL type can hold a `shared_ptr<AppendEntriesResponse>` or a
  `janus::Command` through a `rusty::` alias and hand it to a kernel.
- **Suspension is spellable**, and has been since the wake gate landed:
  `waiter.wait_timeout(us)` inside a DSL body. `Fiber::create_run` and
  `Fiber::sleep` likewise.
- **Cross-carrier DSL calls work**, via alias-only shim namespaces
  (`quorum_hpp`, `server_h`) matching the module path the emitter reaches for.
- **Logging works** via `log_line(Log::INFO, 0i32, core::ptr::null(),
  format!(...))`, documented at `src/rrr_log.h:20-24`. Its only cost is the
  lost level short-circuit.

## 1. The two facts that shape everything below

**MEASURED: `impl RaftServer` is stubbed out.** A DSL `impl` on a hand-written
C++ type emits `#if 0  // patcher: orphan-impl block stubbed`. So there is no
incremental method migration. RaftServer becomes a DSL struct in ONE change
covering all 79 of its methods, or it stays C++. Every conversion listed in §0
was possible only because each was a *new whole type*. (`scripts/raft_dsl.sh`
now fails on that marker, so this cannot happen silently.)

**MEASURED: the mutex guards by convention, and the convention is not clean.**
Of 77 members:

```
touched ONLY under mtx_      10     <- what a Mutex<T> could own today
touched both in and out      24     <- the actual problem
touched ONLY outside mtx_    20
not touched in server.cc     23
```

Rust's `Mutex<T>` **owns** what it guards; `mtx_` guards its siblings by
convention. You cannot say "this bare field is protected by that separate
mutex". So the conversion is blocked not by the member types -- 42 of 77 are
scalars or atomics and 10 are already DSL types -- but by those 24 members with
accesses on both sides of the lock.

## 2. What is NOT the blocker

Worth stating, because each was believed to be at some point:

- **Not the member types.** 52 of 77 are trivially convertible.
- **Not the wire types.** `PendingTable` already carries two opaquely.
- **Not `&mut self` needing exclusive access.** That was my error. The correct
  Rust shape for shared mutable state is `&self` plus interior mutability --
  `Mutex` and atomics -- and RaftServer is already half-way there: `looping_`,
  `stop_`, `rpc_ready_` and `apply_thread_running_` are atomics today, which
  are `&self`-safe and honest.
- **Not the apply thread's existence.** MEASURED: `apply_thread_` is a member
  (`server.h:2231`), spawned with a `[this]` capture (`server.cc:1441`), and
  guaranteed joined before the object dies -- in `PrepareForShutdown`
  (`1344`) and in the destructor (`3953`). That is precisely
  `thread::scope` semantics, not `thread::spawn` semantics: the guaranteed join
  is what makes lending a borrow sound, which is why Rust's scoped threads
  accept non-`'static` borrows and `spawn` does not. `rusty::thread::Scope`
  exists (`thread.hpp:446`, `scope()` at 581). So `&self` sharing across the
  apply thread is expressible without wrapping everything in `Arc`.

## 3. The plan

Each step is independently committable, independently revertible, and
verifiable with the existing net. Steps A-C are C++ refactors with no DSL in
them at all; D and E are the conversion.

### Step A -- make the guarded set honest

For each of the 24 members touched on both sides of `mtx_`, decide which it is:

1. safe because it is immutable after `Setup` (`current_config_` is the model
   case: MEASURED, exactly one write, `server.cc:1671`),
2. safe because it is already an atomic,
3. safe because the access is on the poll thread and the member is only
   mutated there,
4. **an actual race**.

Bring (4) under the lock; annotate (1)-(3) at the declaration. The outcome is a
provable four-way partition of all 77 members: immutable-after-setup, atomic,
mutex-guarded, fiber-local.

UNTESTED: that category (4) is non-empty. It would be surprising if all 24 were
benign, and this step is worth doing for that reason alone, independent of any
Rust.

**Verify.** Compilation plus the standard suite. Any member moved under the
lock is a behaviour change and needs the full seven suites.

### Step B -- group the guarded members into one struct

Mechanical once A is done: the mutex-guarded partition becomes
`struct RaftConsensusState`, and `mtx_` guards exactly it. Still C++, still a
`recursive_mutex`. Ownership now matches guarding, which is the precondition
for a faithful `Mutex<T>`.

**Verify.** Compilation is most of it; the rename surface is large but
compiler-checked.

### Step C -- flatten the recursive mutex

MEASURED: 31 acquisition sites across 25 functions, and `rusty::Mutex` is
strictly non-recursive -- a re-entry probe hangs -- with no recursive
equivalent anywhere in the runtime. The nested re-acquisitions must go.

With B done they are enumerable rather than scattered: every acquisition names
the state struct, so the call graph between acquisitions is readable. The
technique is Tranche 4b's -- prove by caller enumeration that the lock is
already held, and split the callee into a `...Locked` variant.

The known hard case is the synchronous `SendInstallSnapshot` failure callback
(`commo.cc:167` invokes it inline when `PeerForSite` returns null), which
re-enters at depth 3 via `stepDown` -> `resetTimer`. Making that callback
always-async is the fix, and it is independently worth doing because it is also
what stops PHASE 1 from being able to hold a borrow.

**Verify.** All seven suites. A missed nesting is a deadlock that neither the
bench nor `raftLabTest` is guaranteed to reproduce.

### Step D -- the state struct becomes DSL, behind `rusty::Mutex`

Now honest: `rusty::Mutex<RaftConsensusState>` owns what it guards, and every
accessor is `&self` with interior mutability. This is the step where the borrow
check starts checking something true about RaftServer, as it already does for
`HeartbeatRoundState`.

### Step E -- `RaftServer` itself

All 79 methods at once, because §1 leaves no alternative. By this point the
members are: the DSL state struct, the atomics, the immutable-after-setup
scalars, the DSL component types from §0, and a handful of opaque carriers
(the `std::thread`, the rrr handles, `snapshot_manager_`).

On `TxLogServer`: try implementing it directly first. rrr's one-field Shim
(Pattern 2) exists for removing an interface obligation, but MEASURED, that
obligation is now three pure virtuals and zero state (`scheduler.h:95-99`). A
shim would be indirection for nothing unless the direct attempt actually
fights. Decide with evidence, not in advance.

## 4. Verification, unchanged

`scripts/raft_dsl.sh --check` green; `raftLabTest` 25/25; the seven replication
suites after anything touching the replication path; log integrity at 1 and 6
partitions; the flap test; and performance at the throttled point for latency
and the saturation point for throughput.

Note what the gate does NOT do: it runs no C++ compiler, so green does not mean
the emitted C++ builds. It does now fail on silently dropped statements
(`// TODO:` and `#if 0  // patcher:` inside a GEN region), which is the one
failure mode that previously passed everything.

Docker is not installed on the development machine; `./ci/ci.sh` is run
directly, with `LIBRARY_PATH` and `LD_LIBRARY_PATH` set to the mako-deps lib
directory. CLAUDE.md's Docker instruction is inaccurate.

## 5. What could still go wrong

**Step A finds nothing.** Then the 24 mixed members are all benign, B is
smaller than budgeted, and the plan gets easier. This is the good outcome and
it costs one careful pass to learn.

**Step C finds a nesting that cannot be removed.** Then `rusty::Mutex` is out
and the state struct stays behind a C++ `recursive_mutex`, reached from Rust
through kernels -- which still works, and is what every conversion in §0 does
today. D degrades rather than failing.

**Step E is one very large commit.** Unavoidable given §1. The mitigation is
that A-D shrink it: by then the hard members are already types, and E is mostly
mechanical. It should still be attempted only when A-D are green and the tree
is clean.
