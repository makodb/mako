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

### 3.1 The G2 investigation (measurement only)

No source changed. The tools were the Phase 0 trace kit
(`MAKO_RAFT_TRACE_FILE`, M0), the per-follower applied counts already in
every bench record, a `/proc` sampler of which NUMA node each process's
threads ran on, and G2 re-run on each phase's end commit (Phase 1
`590370719` and Phase 2 `a85a19993`, built in their own worktrees). Raw data:
`$RESULTS/p3/g2-trace/`, `g2-bisect-trace/`, `g2-bisect/`.

**Result: the loss is a change in how often the leader skips a follower, and
it arrives with Phase 2. The cost of a round grew by about 1% (validation
below), inside G2's original 2% budget.**

**How a G2 round works.** At 4 KB with no rate limit, every heartbeat round
sends each follower up to 256 entries. The leader stops collecting replies
after the first pass over its in-flight slots that leaves it with a
majority (the early-quorum exit in `heartbeat_collect_body`). A follower
whose reply has not arrived by then still has its request outstanding when
the next round starts, and a follower with a request outstanding is skipped
in that round (per-follower stop-and-wait). Its reply is processed in the
next round's collection, and the round after that sends to it again. Both
rules are Phase 0's, unchanged by Phases 1-3.

So a reply that comes in a little after the other follower's costs that
follower a round. A round that sends to one follower is much shorter than one
that sends to two. In the traced runs (`round_kinds.txt`) a one-follower round
took 4.8-6.5 ms and a two-follower round 7.2-7.4 ms. So **the more often a
follower is skipped, the higher G2's throughput, and the further behind that
follower falls**. A saturated run never lets it catch up: each round sends
it at most 256 entries, and the leader appends about that many per round.
At the end of the checkpoint's fastest runs one follower was up to 108,000
entries (about 3 s) behind the leader.

**Two operating modes.** Each run ends with both followers caught up, or one
behind (below the leader's applied count when the run ends). At the
checkpoint, Phase 0 ended 10 of 25 runs with a follower behind and Phase 3
ended 1 of 25 (Fisher's exact test, p = 0.005); the runs that end behind are
the fast ones (median 39,624/s against 34,918/s). The A/A run's 9.56% paired
CV, which widened G2's bound from −2% to −5.4%, is this bimodality: which
mode a run lands in is decided by a timing race, run by run.

**Bisection** (`g2-bisect/`; 25 rounds, the four phase-end builds rotated
round by round on a quiet machine; rounds 1-2 were re-run because analysis
scripts overlapped them the first time):

| Build | Runs ending with a follower behind | Median, all runs | Median, runs ending caught up (vs Phase 0, Mann-Whitney p) |
|---|---|---|---|
| Phase 0 | 13 of 25 | 37,289/s | 34,768/s |
| Phase 1 | 10 of 25 | 36,782/s | 35,046/s (+0.8%, p = 0.68) |
| Phase 2 | 3 of 25 (Fisher p = 0.005 vs Phase 0) | 35,200/s | 34,914/s (+0.4%, p = 0.96) |
| Phase 3 | 3 of 25 (p = 0.005) | 34,739/s | 34,406/s (−1.0%, p = 0.46) |

Paired throughput, each build against the one before (median ratio, sign
test): Phase 1 vs 0 −0.5% (p = 1.0); **Phase 2 vs 1 −4.7% (p = 0.043)**;
Phase 3 vs 2 −2.3% (p = 0.69); Phase 3 vs 0 −5.1% (p = 0.015, the
checkpoint's result again).

**The cost of a round, first look.** Traced runs (3 per build, rotated)
give the time of rounds that sent to both followers, the comparison the skip
rate cannot bias. Medians: Phase 0 7,346 µs, Phase 1 7,317, Phase 2 7,266,
Phase 3 7,228. The follower's handler per 256-entry batch moved by +20 to
+65 µs (Phase 0 2,562 µs; Phase 1 2,581; Phase 2 2,629; Phase 3 2,604),
under 1% of a round. Three runs per build cannot resolve 1%; the 10-round
validation in 3.2 does, and finds Phase 3 about 1% slower. Tracing slows
every build alike and shifts the race, so the traced runs skip at their own
rates.

**Placement modulates the race but is not the difference.** Runs whose three
processes ran mostly on one NUMA node rarely ended behind (1 of 11); runs
split across both nodes did more often. Among split runs alone: Phase 0
12 of 21, Phase 1 9 of 22, Phase 2 3 of 18, Phase 3 3 of 20.

**Why Phase 2.** Not pinned. Phase 2's runtime changes are M6 (the leader
reads each entry's cached metadata when it builds a follower's batch, instead
of three casts per entry per follower; a follower pays those casts once per
entry when it appends), M10 (22 fewer kernel calls), and M3's action
executor at the end of each critical section. Any of them moves the
leader's timing relative to the second follower's reply. The traced gap
between the two followers' replies did not change measurably (p50
470-700 µs in every build), so the shift sits in the tail of that race, not
in a mean the traces can resolve.

**Reading.** G2 was meant to measure per-message CPU (plan §6, "default G2").
At this point it measures mostly how often the stop-and-wait race starves a
follower, and Phase 2 starves it less. Measured on runs that end caught up,
no phase is slower; measured on rounds that send to both followers, which
resolves less, Phase 3 is about 1% slower than Phase 0 (3.2).

### 3.2 Decision (user, 2026-10-04)

**The checkpoint passes**, G2 included. From the Phase 6 checkpoint on, G2
gates on the traced time of rounds that sent to both followers (10 rotated
rounds, +2% bound, §6's pass rule); 25 untraced rounds' throughput and the
count of runs ending with a follower behind are reported beside it, not
gated (plan 0.10, Q8). The user first chose to gate on the runs that end
caught up; their spread (CV 3.45% over 110 runs) would have needed about
75-100 rounds to hold −2%, so the traced metric replaced it, also by the
user's choice.

**Validation** (`$RESULTS/p3v/G2`: `gate_point.sh p3v G2` with a second
link to the Phase 0 build as the parent arm, so one run gives an A/A and the
Phase 3 comparison; 10 traced rounds, rotated):

| Comparison | Two-follower round, median ratio | Rounds slower / faster | Sign test p | Verdict |
|---|---|---|---|---|
| Phase 0 vs a second link to it (A/A) | −0.58% | 3 / 7 | 0.344 | pass |
| Phase 3 vs Phase 0 | +1.08% | 8 / 2 | 0.109 | pass |
| Phase 3 vs the second link | +1.53% | 9 / 1 | 0.021 | pass (consistent shift within bound) |

No false alarm on the A/A. Phase 3's rounds that send to both followers are
about 1-1.5% slower than Phase 0's, consistently, inside the +2% bound. The
paired spread is 1.4-3.1%. The larger values come from two A/A-arm runs
that skipped heavily (65-69% of rounds sent to both followers); the median
and the sign test absorb them. The first version of the metric counted every
round. One Phase 3 run fell into a stretch of 1,208 short follow-up rounds,
and its median dropped 21%, so the metric now counts full rounds only (the
run's largest batch, 256 entries). The figures above are full-round figures.
The untraced lines the gate reports beside it were exercised with 2 rounds.

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
