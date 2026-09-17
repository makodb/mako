# Step D stated in full: what `rusty::Mutex<T>` actually requires

Measured at `fc023f0a6` over `src/deptran/raft/server.{h,cc}`, excluding the
DSL source blocks, the generated C++, comments, and `...Locked` bodies (whose
callers hold the lock by contract).

## 1. What the change is

Today:

```cpp
class RaftServer {
  std::mutex mtx_;              // guards... what, exactly?
  RaftConsensusState state_;    // ...this, and 36 other members
  RaftLog raft_log_;
  PeerTable peers_;
  ...
};
```

The association between the mutex and the data it protects exists only in
comments and habit. Nothing stops any line of code from reading `state_`
without holding `mtx_`, and nothing reports it if one does.

After:

```rust
pub struct RaftState { /* the guarded members, as fields */ }
// in RaftServer:
rusty::Mutex<RaftState> state_;
```

`Mutex<T>` CONTAINS the data. The only way to reach a field is `lock()`,
which hands back a guard. Touching the data without the lock is not
discouraged, it is unspellable -- the field is not reachable any other way.
That is the entire value of the step: a convention becomes a type.

## 2. The precondition

`Mutex<T>` must own EXACTLY the set `mtx_` guards.

- Own too little, and the members left outside are still guarded by
  convention. The step bought nothing for them, and worse, the code now
  looks safe.
- Own too much, and code that legitimately touches a member without the
  lock has to start taking it, which changes both behaviour and cost.

So the whole of Step D reduces to one question: **what does `mtx_` guard?**

## 3. What it guards, measured

`mtx_` critical sections touch **37 members**. `RaftConsensusState` has
**20 fields**. The gap is 17 members, and they are not one problem but four.

### Group 1 -- already atomic, must NOT move in (7)

```
stop_  looping_  rpc_ready_  apply_thread_running_
disconnected_  snapshot_manager_configured_  snapshot_trigger_index_
```

These appear inside critical sections incidentally; they are atomics and are
read and written outside the lock all over. They are already thread-safe on
their own terms. Moving them into the mutex would be wrong -- it would force
a lock acquisition where an atomic load does today.

### Group 2 -- the real work (4)

```
raft_log_          68 in-lock accesses,  3 out
peers_             29 in-lock,           6 out
current_config_     5 in-lock,           6 out
peer_sites_         1 in-lock,           6 out
```

These are genuinely guarded state that must become fields of the locked
struct. `raft_log_` and `peers_` are already DSL types, which is what makes
this tractable at all -- before the log conversion, `raft_log_` was a
`map<slotid_t, shared_ptr<RaftData>>` with no Rust spelling, and this step
could not have been done.

`current_config_` is a `std::set<siteid_t>` written exactly once during
Setup. `peer_sites_` is a `std::vector<siteid_t>` derived from it. Both are
immutable after startup, so they could equally be moved OUT of the guarded
set and marked immutable rather than moved in. That is a judgement call, not
a measurement.

### Group 3 -- has its own lock (3)

```
apply_queue_  apply_queue_epoch_  apply_queue_mtx_
```

These belong to the apply-queue subsystem and are guarded by
`apply_queue_mtx_`, not by `mtx_`. They appear inside `mtx_` regions only
because the two locks nest there. They stay where they are.

### Group 4 -- opaque handles, effectively immutable (2)

```
snapshot_manager_  async_callback_lifetime_
```

`shared_ptr`s set during setup. `snapshot_manager_` has 9 in-lock accesses
and 0 out-of-lock ones, so it is the one member in the whole list that is
cleanly and exclusively guarded.

## 4. The 20 fields already in the struct

11 of them appear to be touched outside the lock, which would be alarming if
it were true. It is not. Every one of those accesses is inside
`setIsLeader` or `stepDown`, and both are documented as caller-holds-lock:

> `// Must be called with mtx_ held. Every caller reaches here from inside
> RequestVoteImpl, OnAppendEntries, OnRequestVote (via doVote),
> OnInstallSnapshot or stepDown, all of which hold mtx_.`

Checked against the code: all 9 call sites of `setIsLeader` and all 5 of
`stepDown` hold `mtx_`. The one exception the comment names is real and
benign -- `setIsLeader(false)` under `RAFT_TEST_CORO` in RaftServer's own
constructor, where no other thread can observe the object yet.

They simply lack the `...Locked` suffix the rest of the codebase uses.

That leaves exactly **two** genuinely unlocked reads of guarded state, both
on the apply thread and both only for logging:

```
server.cc:1553  Log_info("... applied {} entries, state_.execute_index_={} ...")
server.cc:1591  Log_info("... IDLE state_.execute_index_={} state_.commit_index_={} ...")
```

## 5. So the work is

1. Rename `setIsLeader` / `stepDown` to `...Locked`, or give them a guard
   parameter. Mechanical, compiler-checked.
2. Fix the two apply-thread log lines -- read under the lock, or drop them.
3. Decide `current_config_` and `peer_sites_`: move into the struct, or
   move out of the guarded set as immutable-after-setup.
4. Move `raft_log_` and `peers_` into the struct.
5. Wrap it: `rusty::Mutex<RaftState>`.
6. Every `lock_guard(mtx_)` + `state_.x` becomes `state_.lock().x`.

Steps 1-4 are C++ refactors with no DSL in them, each independently
committable and compiler-checked. Only step 5 changes the type.

## 6. The open decision, stated honestly

Should the apply thread's edge be narrowed first?

The apply thread takes `mtx_` at three call sites totalling 18 lines
(`CompactLog`, `MaybeCreateSnapshot`, `PublishAppliedIndex`) plus one inline
acquisition in the thread body. Against that, 23 methods and ~1,600 lines
take `mtx_` from the poll thread.

**A correction to what I said earlier.** I described narrowing that edge as
making `mtx_` "poll-thread-only", and that is wrong. `GetSnapshotIndex`,
`SetSnapshotThreshold`, `Disconnect`, `PrepareForShutdown`, `GetLeaderHint`
and `IsLeader` are called from outside RaftServer -- 28 sites in `test.cc`,
5 in `raft_worker.cc`, and one each in `testconf.cc` and
`raft_main_helper.cc`. Those callers are not the poll thread. So even with
the apply thread's three sites removed, `mtx_` would remain a genuine
cross-thread lock and could not be deleted.

Narrowing the edge is therefore a smaller win than I implied: it would
shrink what crosses threads, but not change the fact that a lock is needed,
and it would not let the poll-thread critical sections be dropped.

UNTESTED, and the reason to be careful if it is attempted anyway: the three
apply-thread sites interact with `state_machine_apply_mtx_`, whose
documented acquisition order is apply-gate then Raft-state. Giving them a
new lock introduces a third order to get right.
