# Phase 3 report

Plan: [../modification-plan.md](../modification-plan.md), Phase 3 ("Cut the
fibers", milestone 1), reported at stopping points 0.7 points 2 and 5. Raw
results: `$RESULTS/p3/` (not in git). Testing per plan 0.10: Tier 1 at the
phase end, then the first performance checkpoint (G1-G6 and G7, the Phase 0
build against this one).

## 1. What changed

| Commit | What |
|---|---|
| `937a34cd8` | The heartbeat round as three core calls (`heartbeat_tick`, `heartbeat_on_reply`, `heartbeat_round_end`); F7: each payload built and stamped from handles copied out under the guard, after it drops |
| `496db6cd2` | The inbound RequestVote and AppendEntries handlers take the core; the payload enters as a `WireBatch` |
| `25aacb728` | The campaign as two core calls (`start_election`, `election_settle`); the vote tally counted by the core from the replies each lane's wait gathered; the election timer's gather a core function |

Per-item detail: [../diff-ledger.md](../diff-ledger.md), "Phase 3". The
path-by-path analysis Phase 8 starts from:
[../coupling-table.md](../coupling-table.md).

## 2. Tier 1

Three lanes (`$RESULTS/p3/tier1/`), first attempt, no retries:

| Suite | Result |
|---|---|
| raftLabTest (rust lane, 30 cases) | pass, 266 s |
| shard1ReplicationRaft | pass, 57 s |
| shard2ReplicationRaft | pass, 67 s |
| shard1ReplicationSimpleRaft | pass, 41 s |
| shard2ReplicationSimpleRaft | pass, 52 s |
| raftLabTestHybrid (30 cases) | pass, 264 s |
| raftLabTestCpp (30 cases) | pass |

## 3. Performance checkpoint (plan 0.10)

(filled in when the run ends)

## 4. Equivalence

Plan A.4 item 3 asks for a replay recorder at `step()` and a byte-for-byte
self-replay. Not done in this phase, by design: a faithful replay needs every
core mutation to go through recorded calls, and some still bypass them (the
proposal's append inside `Start`, `PublishAppliedIndex`, Setup, the snapshot
paths). Phase 4's `with_core` makes every access a call; the recorder goes in
there, and the self-replay check runs from Phase 4 on. Until then the lab
passing is the check.

## 5. V3 settled

The coupling table's §3.2: a "no" vote reaches only the campaign's local
count, and through it only the "no quorum" branch, whose one marked effect is
the role leaving Candidate (`LStepAside`, no guard). A refusal's term is at
most the campaign's, so it cannot trigger the higher-term branch. No marked
field depends on the "no" count; F2 stays unneeded, and stop point 7's
fallback is not reached.

## 6. Plan deviations

- **The vote tally is fed at settlement, not reply by reply.** The plan's
  shell polls `core.decided()` every 200 µs during the wait. Each lane keeps
  its own rule for ending the wait (raft-rt's 200 µs poll of its tally; the
  C++ event's wakeup), and the replies are handed to the core when the
  campaign settles, so no lane's timing changes. The core's count decides
  the outcome; the lane's only ends the wait (the two use the same rule).
- **The two new lab cases are not written** ("reply landing during
  `round_end`", "vote reply after the deadline"). The lab has no way to hold
  a reply past a round's deadline or past the 1 s vote wait (its slowest link
  delays 27 ms). By construction, a late append reply is processed by the
  next round's collection loop (its slot stays occupied until then), and a
  late vote reply never reaches the core (the campaign's replies are read
  once, at settlement). Case 10 (unreliable network) exercises late append
  replies.
- **InstallSnapshot sends move ahead of the round's AppendEntries sends**
  (still under the guard, in follower order among themselves). Snapshots are
  off in the verified configuration.

## 7. Bugs found

None new in the code. The F1 fix now covers the C++ lanes too (B3), since the
core counts each voter once.
