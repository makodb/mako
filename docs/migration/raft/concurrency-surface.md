# What the unconverted RaftServer C++ is actually defending against

Measured at `95f9277c5`, over `src/deptran/raft/server.{h,cc}`. Generated
C++ excluded throughout.

## Thread inventory

RaftServer creates exactly ONE OS thread.

| context | what runs there | how it is created |
|---|---|---|
| apply thread | the apply drain loop and the state machine | `std::thread apply_thread_`, `server.cc:1464` |
| poll thread | heartbeat loop, election timer loop, all RPC handlers | rrr fibers on the communicator's single `PollThread` (`server.cc:1638`) |
| caller threads | `Setup`, the snapshot/config accessors, shutdown | the application and test harness |

Everything in the poll-thread row is a COOPERATIVE FIBER. Those cannot run
in parallel with one another; they interleave only where one suspends.

## How much of it is literally synchronisation

```
literal lock/unlock statements       42
literal atomic operations           138
literal thread lifecycle operations    8
                                    ---
                                    188   of ~2,540 hand-written code lines = 7.4%
```

## How much must be REASONED about as concurrent

Lines sitting inside a critical section:

```
mtx_                              1,189
state_machine_apply_mtx_            385
apply_queue_mtx_                     33
startup_mtx_                          6
any mutex (deduped)               1,260   = ~50% of the remaining C++
```

## The cause is far smaller than the blast radius

`mtx_` is a genuine cross-thread lock: the apply thread takes it. But the
apply thread's reach is tiny. From the thread lambda, the transitive callee
set inside RaftServer is SIX methods, of which three take `mtx_`:

```
CompactLog, MaybeCreateSnapshot, PublishAppliedIndex     18 code lines
plus one inline lock_guard(mtx_) in the thread lambda itself
```

Against that, 23 methods totalling 1,596 code lines take `mtx_` from the
poll thread or a caller thread.

So roughly 1,600 lines are written defensively because of ~18 lines on the
other side of the boundary.

## Fibers contribute no locking at all

Of the 29 `mtx_` critical sections in server.cc, the number containing a
fiber suspension point is ZERO. (A scan flags one, at the `unique_lock` in
`OnAppendEntries`; the match is `usleep(25*1000)` inside a `/* */` comment
at `server.cc:5247`, not live code.)

This is the answer to "fibers or threads": it is threads. Cooperative fibers
on one poll thread can only interleave where one of them suspends, and no
critical section suspends, so none of this locking defends against fiber
interleaving. It exists entirely because the apply thread and the caller
threads can preempt at any instruction.

## What that implies for the conversion

`rusty::Mutex<T>` owns what it guards, which is why Step D of the conversion
plan has been blocked on making the guarded set honest. The measurement
above says the hard part is much smaller than the line count suggests:

- If the apply thread's three `mtx_` call sites were moved behind their own
  lock, or their few fields made atomic, `mtx_` would become
  poll-thread-only. At that point it is defending against nothing --
  no suspension points, no parallelism -- and the poll-thread critical
  sections could be dropped rather than translated.
- The caller-thread accessors (`GetSnapshotIndex`, `SetSnapshotThreshold`,
  `Disconnect`, `PrepareForShutdown`, ...) are the remaining genuine
  cross-thread edge. They are infrequent, and several already have
  `...Locked` variants from the recursive-mutex work.
- `state_machine_apply_mtx_` (385 lines) is a real apply-thread/poll-thread
  edge and is separate from `mtx_`; it guards the state machine, not Raft
  state, and is not in `rusty::Mutex<RaftConsensusState>`'s way.

UNTESTED: that the apply thread's three sites can be moved without
introducing a lock-ordering problem with `state_machine_apply_mtx_`, whose
documented order is apply-gate then Raft-state.
