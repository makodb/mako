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

`build_rust_base` (`verus-p0`) against `build_rust` (`25aacb728`), alternated
round by round on a quiet machine (load 0.5 at the start), rounds and bounds
from [../gate-params.md](../gate-params.md) (`$RESULTS/p3/`):

| Point | What | Median (Phase 3 vs Phase 0) | Bound | Sign test p | Verdict |
|---|---|---|---|---|---|
| G1 | 4 KB at 240/s, latency (10 rounds) | p50 +0.15%, p99 +0.74% | +2%, +5% | 0.754, 0.344 | pass |
| G2 | 4 KB unthrottled, throughput (25) | **−5.46%** (Phase 3 lower in 18 of 25) | −5.4% | **0.043** | **FAIL** |
| G3 | 286 KB × 6 partitions at 190/s, latency (25) | p50 +0.36%, p99 +0.88% | +2.4%, +6% | 0.690, 0.108 | pass |
| G4 | 286 KB × 6 partitions unthrottled, throughput (11) | −0.61% | −2% | 0.549 | pass |
| G5 | 1 MiB at 55/s, latency (20) | p50 −0.30%, p99 +0.09% | +2%, +5% | 0.824, 1.000 | pass |
| G6 | 1 MiB unthrottled, throughput (12) | −0.46% | −2% | 1.000 | pass |
| G7 | leader killed, 20 per build | new leader median +3.5%, p90 +4.1%; first commit the same | 10%, 10.2% | MWU 0.308, 0.299 | pass |

**G2 fails**: small-entry maximum throughput is 5.46% lower, just past the
widened bound and significant at p = 0.043. Nothing else moved: the
production shape (G3, G4) and large entries (G5, G6) are within noise. Per
plan 0.7 point 2 forward progress stops here (Phase 4 waits); the
investigation follows, measurement only (0.6), starting with G2 on the
phase-end commits since the last checkpoint (0.10). Section 3.1 records it.

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
