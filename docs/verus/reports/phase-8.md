# Phase 8 report

Plan: [../modification-plan.md](../modification-plan.md), Phase 8 ("Coupling
and the proof"), and the performance checkpoint after it (plan 0.10). Raw
results: `$RESULTS/p8/` and `$RESULTS/replay/` (not in git).

No mako-dev merge at the phase start: `origin/mako-dev` was already merged
(Phase 6).

## 1. What changed

All of it ghost code (M12) or tooling and docs: no executable line of the
core moved (the ghost-only lint against Phase 6's last commit, `51d100d1f`:
0 executable changes), and no shell line. Per commit:
[../diff-ledger.md](../diff-ledger.md), "Phase 8".

| Commit | What |
|---|---|
| `dfe6ba6d8`, `0feae5ac7` | The proof side imports spec v1 (Verus `--export`/`--import` of the frozen tag); `RaftLog::view()`. |
| `274a72654` | `core_check.sh` runs the ghost-only lint (the plan's diff-2 lint). |
| `5f97ae6fe` | The coupling's foundation (`core/src/coupling.rs`, Verus only): the ghost log on the core, the four ghost-log operations, `state_view`, `ginv`; Setup records LoadConfig. |
| `6b5030ff4` | The stutters; StepAside; ClientRequest. |
| `033078d5b`, `cc73145a0`, `e2f63615e` | The election: an inbound RequestVote (StepDown, GrantVote / RejectVote); a campaign (Timeout); a settlement (ReceiveVoteGranted, BecomeLeader, StepDown, StepAside). |
| `762573fc4` | An inbound AppendEntries: StepDown, RejectAppendEntries, or one FollowerAppendEntries per component (BR2). B16 recorded. |
| `1d8babc9c`, `e60880223`, `37319d389`, `ba5e0ed1b` | The leader: V2; a reply (StepDown, HandleAppendResponse); the commit advance (AdvanceCommitIndex); a tick's sends (SendAppendEntries per component, BR1). B17 recorded. |
| `998b86820` | `step` and `step_checked` keep the coupling; the per-node certificate; the cluster theorem. |
| `426eb7e1d`, `4d8357131`, `a09862d2f` | The ledger lint's ghost scanner fixed (§6). |
| `7220c62b9` | [../host-contract.md](../host-contract.md); the ledger rows; the coupling table's departures. |

## 2. Verification

`scripts/verus/verify_core.sh`: **387 verified, 0 errors**, the trusted
surface still one function (`blocks_for`). The spec is v1 (tag
`raft-spec-v1-export`, `verify_spec.sh` exits 0, §3).

What is proved, for every core call (`core/src/coupling.rs` and the
contracts in `node.rs`, `heartbeat.rs`, `event.rs`):

- **The coupling invariant `ginv`**: the core's ghost log is fully closed and
  a certificate of its history (every closed segment one atomic step of the
  spec, every label bound to its trigger's message, every send routed: the
  group's `log_inv`, `wf`, `routed`), and replaying it gives the core's state
  as the spec sees it: the term; the role (Leader; Candidate while a
  campaign of the current term runs; else Follower); the vote by rank; the
  log, entry by entry (term and the command's value view); the commit
  index; the campaign's tally, the leader's match and next tables (ghost,
  V1-V3). Beside the replay: V2, a leader's match index never above the
  spec's; a leader or candidate voted for itself; a candidate holds its own
  vote; every entry has a value and a term at least 0; no member the
  sentinel site; `snapterm_ == 0`.
- **Every step keeps it**: `step` from a core with `ginv`, under the event's
  premise (`coupled`, the host contract's part), and `step_checked`, which
  supplies F9's admission itself. A fresh core has it.
- **The per-node certificate** (`lemma_node_cert`): the ghost log is the
  group's `node_cert` for the core's n-member cluster and its rank.
- **The cluster theorem** (`theorem_mako_safety`): n such cores, under any
  causal schedule of their segments, satisfy `RaftSafetyInvariant` after
  every step: at most one leader per term, matching logs, committed entries
  never lost or changed (the group's `theorem_compose_safety`,
  instantiated). Liveness is not proved.

Each handler's segments, against the spec's actions:

| Core call | Segments (spec actions) |
|---|---|
| `configure` | LoadConfig |
| `append_local` (Propose) | ClientRequest |
| `start_election` | Timeout (one broadcast RequestVote) |
| `raft_on_request_vote` | StepDown when the request's higher term is taken up, then GrantVote or RejectVote (the answer the reply) |
| `election_settle` | a higher reply term: StepDown; won: ReceiveVoteGranted per granted reply, BecomeLeader, and the rollback's StepAside; lost, timed out, stopped: StepAside |
| `raft_on_append_entries` | StepDown when the term is higher, then RejectAppendEntries (and a candidate's StepAside on a committed-conflict refusal), or one FollowerAppendEntries per component (BR2) |
| `heartbeat_tick` | PHASE 0's AdvanceCommitIndex, then SendAppendEntries per component of each follower's RPC (BR1) |
| `heartbeat_on_reply` | StepDown, or HandleAppendResponse when a success reports past the spec's match, or nothing |
| `heartbeat_round_end` | AdvanceCommitIndex (premise: the server leads, B17) |
| `SetFollower`, `StepDown` events | StepAside from a leader or candidate |
| the rest (timers, the round state, the ledger, the applied mark, setup's identity and gate, the peer table's rebuild) | nothing the spec sees |

Every new lemma, and the deep branches of the handlers (the vote, the
settlement's won and higher-term paths, the append accept, the reply's
HandleAppendResponse, the tick's send), was checked non-vacuous: an
`assert(false)` at its end fails.

**Size.** `coupling.rs` is 2,706 lines (45 lemmas, 66 spec functions);
Phase 8 changed `core/src` by +4,029 / -56 lines, all ghost. The plan's
extrapolation was 1.5-2k proof lines plus a 2-3k-line coupling layer.

## 3. Gates

At `998b86820` (`$RESULTS/p8/`), `build_rust` rebuilt from it first:

| Gate (plan Phase 8) | Result |
|---|---|
| `verify_core.sh` | 387 verified, 0 errors; trusted: `blocks_for` |
| `verify_spec.sh` | 2,129 verified (the recorded count), 0 errors |
| ledger lint, ghost-only mode | 0 executable changes against `51d100d1f` |
| A.4 item 4: the instrumented core replays Phase 6's recordings | byte for byte, the same counts as Phase 6 (table below) |
| Tier 1 | raftLabTest 31/31 (242 s); shard1ReplicationRaft (58 s), shard2ReplicationRaft (68 s), shard1ReplicationSimpleRaft (41 s), shard2ReplicationSimpleRaft (51 s) pass; no retries in any log |

| Recording (`$RESULTS/replay/ce967f6f7/`) | Files | Steps matched | Stopped at a `T` line | Mismatched |
|---|---|---|---|---|
| a full lab run (31 cases) | 9 | 31,792 | 9 | 0 |
| G1, 5 s | 3 | 41,361 | 0 | 0 |
| G4, 5 s | 18 | 108,318 | 0 | 0 |

The `T` lines are the lab's snapshot cases (outside the verified
configuration), as at Phase 6. That the replay matches is expected: Phase 8
changed no executable line, and the ghost code is erased under cargo.

## 4. Performance checkpoint (plan 0.10)

`build_rust_base` (`verus-p0`) against `build_rust` (`998b86820`), alternated
round by round on a quiet machine (load 1.5 at the start), rounds and bounds
from [../gate-params.md](../gate-params.md) (`$RESULTS/p8/`). G1-G6 and, as
after Phase 3, G7. **Every point passes.**

| Point | What | Median (Phase 8 vs Phase 0) | Bound | p | Verdict |
|---|---|---|---|---|---|
| G1 | 4 KB at 240/s, latency (10 rounds) | p50 +0.88%, p99 +0.49% | +2%, +5% | 0.344, 0.754 | pass |
| G2 | 4 KB unthrottled, time of rounds that sent to both followers, traced (10) | +1.64% | +2% | 0.754 | pass |
| G3 | 286 KB x 6 partitions at 190/s, latency (25) | p50 +0.00%, p99 +0.91% | +2.4%, +6% | 1.000, 0.108 | pass |
| G4 | 286 KB x 6 partitions unthrottled, throughput (11) | -2.38% | -2% | 0.549 | pass (past the bound, not significant) |
| G5 | 1 MiB at 55/s, latency (20) | p50 +0.02%, p99 -0.51% | +2%, +5% | 0.824, 0.824 | pass |
| G6 | 1 MiB unthrottled, throughput (12) | -0.40% | -2% | 1.000 | pass |
| G7 | leader kill: new leader / first commit, median and p90 (20 kills each, none failed) | -1.1% / -3.5%; -1.0% / -3.4% | 10% | 0.409 | pass |

p is the sign test's (G1-G6) or Mann-Whitney's (G7).

**G4.** Its median is past the bound but not significant (Phase 8 lower in
7 of 11 rounds, p = 0.549), and it cannot be a code effect: Phase 8 changed
no executable line, so this build runs Phase 6's core, which measured G4 at
+0.59% against the same base. Three points between two runs of the same code
is G4's run-to-run noise at 11 rounds.

**G2's reported lines (not gated, plan 0.10).** Untraced, 25 rounds:
throughput -8.24% (Phase 8 lower in 17 of 25, p = 0.064). As at Phase 6, the
follower-behind count accounts for it: the base build ended 14 of its 25 runs
with a follower behind, Phase 8 only 1 (Fisher p < 0.001), and a run that
leaves a follower behind skips it in most rounds, so its rounds are cheaper.
Like with like: runs that ended caught up, -0.25% (Mann-Whitney p = 0.582);
runs that ended behind, 39,285/s against 39,066/s.

## 5. Plan deviations

- **The host contract is a document plus `coupled(ev)`**, not a separate
  Verus layer: each handler's contract states its premises, `step`'s
  `coupled` collects them per event, and
  [../host-contract.md](../host-contract.md) says who guarantees each.
- **Backoff is a stutter**, not LHandleAppendReject; **HandleAppendResponse
  takes the reported match**, recorded only when it rises past the spec's
  (V2 as an inequality); **a won campaign's votes are recorded at
  settlement**; the details are the coupling table's §5.
- **V2's frame.** A new leader's peer table must start at match 0 against the
  empty spec table: `set_is_leader` rebuilds it only under failover, so the
  settlement's premise includes the gate's failover.
- **The round end's premise** (B17, §7): the certificate holds for runs that
  do not hit the race until it is fixed.
- **Phase 6 lines found unregistered.** The ledger lint's scanner had read
  `raft_on_append_entries`' whole body as ghost since Phase 6 (a bodiless
  trait `spec fn` opened a head that never closed). Fixed in `a09862d2f`;
  checked properly, six Phase 6 lines of that body needed their Phase 6
  tags (comments only).

## 6. Tooling

The ledger lint's ghost scanner was fixed three times as the proofs
exercised it (a struct literal or a block in a clause; a multi-line `let
ghost`; a bodiless trait `spec fn`); each fix was checked by diffing the
scanner's ghost lines before and after over `core/src`, and only the last
removed lines from ghost (198 of `raft_on_append_entries`, see §5). The lint
also reads a Verus-only module as ghost and classifies a removed line by
the base file's own regions.

## 7. Bugs found

- **B16** (liveness, latent): an entry without a value is never replicated
  (the leader reads it as missing and skips every follower behind it). No
  caller proposes one; the proof takes "a proposal has a value" as a
  premise. A fix (`Start` refusing an empty command) is a behaviour change
  for the user.
- **B17** (safety, race): PHASE 3 advances the commit index without checking
  that the server still leads; the shell checks leadership before taking
  `mtx_`. A server that steps down in that window, and takes a newer
  leader's entries over its tail, can commit an index a majority does not
  hold (the five-server scenario in [../bugs-found.md](../bugs-found.md)).
  Reproduced at the core by `core/tests/b17_round_end.rs` (ignored, so the
  suite stays green; it fails with `--ignored`). The fix, advancing only
  while leading, makes it pass (tried in a working copy, not applied); it
  is a behaviour change outside A.2: **for the user to decide**.
