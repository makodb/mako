# Phase 2 report

Plan: [../modification-plan.md](../modification-plan.md), Phase 2 ("Side
effects become returned actions"), reported at stopping point 0.7 point 5.
Raw results: `$RESULTS/p2/` (not in git). Testing per plan 0.10: Tier 1 at
the phase end, no performance run.

## 1. What changed

| Commit | What |
|---|---|
| `3ab039029` | M10: the 22 `raft_verify` calls and 7 explicit panics become `assert!`; the unused `raft_verify` kernel is deleted |
| `4a8d2dc81` | M6: `RaftEntry` caches its command's `has_value`, `is_tpc_commit`, `kind` and `payload_bytes`, asked once through one new kernel |
| `2d657ae4c` | M1/M3/M4: a core decision pushes its effects into a `CoreOutput` (apply range, timer reset, the leader's no-op, the role setting) and the shell carries them out in order before the guard drops; `setIsLeader`/`stepDown` become `RaftCore::set_is_leader`/`step_down`; the timer reset's clock and sample are parameters; the configuration and peer table move into `RaftCore` |
| `a85a19993` | F6: the role setting's log entry and the leader-change callback run after `mtx_` is released, in transition order, each once; lab case 14 |

Per-item detail: [../diff-ledger.md](../diff-ledger.md), "Phase 2".

## 2. Tier 1

Three lanes (`$RESULTS/p2/tier1/`):

| Suite | Result |
|---|---|
| raftLabTest (rust lane, 30 cases) | pass, 266 s |
| shard1ReplicationRaft | pass, 57 s |
| shard2ReplicationRaft | pass, 67 s |
| shard1ReplicationSimpleRaft | pass, 41 s |
| shard2ReplicationSimpleRaft | pass, 52 s |
| raftLabTestHybrid (30 cases) | pass, 267 s |
| raftLabTestCpp (30 cases) | first attempt: **failed to compile**; after the fix: pass, 340 s |

No retries in any log. Lab case 14 (the leader-change callback) passes on
every lane.

The cpp lane's first build failed on one call in heartbeat phase 2,
`server.core.step_down(.., &mut out)`: for a method of a type another module
defines, reached through a reference parameter, the transpiler wrote the
local as a pointer (`&out`) where the method takes a reference. A call to a
function of the same module translates correctly, so the step-down goes
through a small same-module function now (B13's class of transpiler limit).
The fix was folded into the M1/M3/M4 commit, which was re-created (it and
F6 have new SHAs); the code is otherwise identical, so only the cpp lab was
re-run, after a rebuild that passed every source gate.

## 3. Equivalence (plan A.4, as amended by 0.10)

The lab passing is the check until Phase 3.

## 4. Plan corrections

1. **F6 as written would reorder callbacks across threads.** The plan moves
   the leader-change callback after `mtx_` is released, "in emitted order".
   That order holds within one call, but two threads changing role back to
   back could deliver their callbacks crossed, and `RaftWorker` keeps the
   value that arrives last, so it could end up believing this server leads
   when it does not. The implementation queues each transition's notice
   under `mtx_` (so the queue's order is the transitions') and fires the
   queue in order with no lock held; whichever thread finds it idle drains
   it. Lab case 14 checks that each replica's notices alternate and end on
   its role.
2. **Registration raced the callback** (bugs-found B11, latent):
   `RegisterLeaderChangeCallback` wrote the slot with no lock. With the
   callback outside `mtx_` the window would have widened; registration and
   every read of the slot now take the notice queue's lock, and the callback
   runs from a copy.
3. **No separate wake action.** The plan lists `WakeReplication` among
   `become_leader()`'s actions. The only core path that woke replication was
   the leader's no-op, whose shell executor still wakes it, and a lab build
   skips both together as before; a separate action would have split them.
4. **Logging stays in the core functions.** M7 (core logging becomes a
   no-op, the shell logs) is in the plan's kind list but in no phase's
   table; the core calls still log through `rusty::raft_log_*`, which Phase 6
   must remove from the Verus-checked crate.
5. **Timer resets run at the end of their critical section** (in push
   order), not at the point they were pushed: nothing in a section reads the
   timer after a reset (checked), and the clock is read microseconds later.
   The follower's InstallSnapshot is the exception: its resets run right
   after the role change, ahead of the (possibly long) install, as before.

Not done: the plan's Phase 2 "dry run" (lifting `raft_commit_advance` and
`heartbeat_apply_append_reply` into the Verus spike crate). It informs
Phase 6 and is folded into it.

## 5. Bugs found

B11 (the callback slot written with no lock; fixed with F6) and B13 (the
transpiler cannot redeclare a name in one scope; a `let` shadowing the
`stopped` parameter was renamed before any cpp build). See
[../bugs-found.md](../bugs-found.md).
