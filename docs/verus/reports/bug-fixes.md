# Bug-fix phase report (F12-F19)

Plan: [../modification-plan.md](../modification-plan.md) A.2, items F12-F19,
approved by the user on 2026-10-06 ("can you first fix all remaining bugs of
Raft on this branch?"), and the performance checkpoint after them (plan
0.10). Raw results: `$RESULTS/fixes-checkpoint1/`, `$RESULTS/fixes-checkpoint2/`,
`$RESULTS/fixes-fullci/` and `$RESULTS/fixes/` (not in git). The replay
recordings were taken on local tmpfs and are kept compressed in
`$RESULTS/replay/` (§4).

**Scope.** Every open Raft bug in [../bugs-found.md](../bugs-found.md): B1,
the rest of B4, B7, B8, B14, B16, B17, B18 and B19. Not in scope: B6 (Raft
state is memory-only; that is the disk-persistence project,
[../disk-persistence.md](../disk-persistence.md)), and B9, B10 and B13,
which are test tooling and the transpiler, not Raft.

## 1. What changed

Per commit; per line: [../diff-ledger.md](../diff-ledger.md), "Bug fixes after
Phase 8".

| Commit | Item | What |
|---|---|---|
| `a586a7f51` | F12 (B17); B18 | PHASE 3 advances the commit index only while the core leads. The round end's premise is the gate alone, the reply's `is_leader ==> ` the core leads (ghost only). `core/tests/b17_round_end.rs` is no longer ignored. The ghost-only lint is retired; F12-F19 are registered. |
| `49efd82c1` | | Ledger rows for F12. |
| `924a4dfdd` | F13 (B16) | `Start` refuses a command without a value, an envelope holding no command object (`has_value()` is `inner_.is_some()`); an empty payload, such as raft_bench's end markers, is still a value. Lab case 16. |
| `21c287832` | F14 (B4) | The AppendEntries decoder refuses an entry term above the append's own; lab case 12's second probe. |
| `ad7af330e` | F15 (B1) | A campaign is lost once `no > (n - 1) - n/2` (raft-rt's tally and the core's `VoteSet`). |
| `00881d428` | | clippy `--all-targets` tidy in raft-replay's tests. |
| `cd46bbf20` | F16 (B8), F17 (B14), F18 (B7) | The dead term check removed; `heartbeat_interval_us_` an atomic; `get_outstanding_logs` counts the worker's own uncommitted submissions. |
| `74dab7c9a` | F19 (B19) | No thread holds `&mut RaftServerBase`: every entry takes `&RaftServerBase`, and what changes after the server is shared is an atomic or a `ShellCell` under a stated lock or owner. `RaftSpecific` takes `&self` (its C++ virtuals are `const`). |
| `0633e1ffc` | | The records: bugs-found, host contract, ledger, plan, code-structure. |

What behaves differently: a server that no longer leads commits nothing at
its round end (F12); a command without a value is refused (F13); one more kind of
malformed append is refused (F14); a lost campaign ends as soon as it is
lost (F15); the outstanding-logs metric is this worker's own (F18). F16, F17
and F19 change no behaviour.

## 2. Verification

`scripts/verus/verify_core.sh`: **387 verified, 0 errors** after every
commit, the trusted surface still one function (`blocks_for`); the spec is
still v1. The two weaker premises (B18) verified without a proof change: the
handlers' proofs did not use what was dropped once F12 made PHASE 3 check
the role itself.

The ledger lint: 0 unregistered lines (13 core files). Executable core lines
change again in this phase, so its ghost-only mode is retired; each changed
line carries its `[fix, Fn]` tag.

What the certificate covers now that it did not: a round end after a
step-down (B17) or during shutdown (B18), and a reply after shutdown has
begun (B18). What the shell now checks instead of assuming: a proposal has a
value (F13). The host contract lists no gap of the shell's own
([../host-contract.md](../host-contract.md) §6).

## 3. Gates

| Gate | Result |
|---|---|
| clippy `-D warnings`, default and `raft_test` | pass after every commit |
| Workspace tests (core, rt, replay, the shell, `server_is_send`) | pass, both feature sets; `b17_round_end` runs and passes |
| `raft_dsl.sh --check`, `raft_field_census.py`, the correspondence check | pass |
| Tier 1, checkpoint 1 (after `cd46bbf20`) | raftLabTest 32/32 (305 s); shard1ReplicationRaft 58 s, shard2ReplicationRaft 68 s, shard1ReplicationSimpleRaft 41 s, shard2ReplicationSimpleRaft 51 s; no retries |
| Tier 1, checkpoint 2 (F19) | raftLabTest 32/32 (308 s); 58 s, 69 s, 40 s, 50 s; no retries |
| The rest of `ci.sh all` (`74dab7c9a`'s code, `$RESULTS/fixes-fullci/`) | srpcTests (ctest 49/49), simpleTransaction, clientServer, simplePaxos, shardNoReplication, shard1Replication, shard2Replication, shard1ReplicationSimple, shard2ReplicationSimple, rocksdbTests, multiShardSingleProcess, shard2SingleProcess, shard2SingleProcessReplication: all pass, no retries |

With Tier 1, every suite of `ci.sh all` has now passed on `verus-raft`;
before this phase only Tier 1 had run. The Paxos suites matter for F19:
they run PaxosServer against the regenerated `scheduler.h`, whose
`TxLogServer` is unchanged. (`shardFaultTolerance` is disabled in `ci.sh`;
`cpuThrottlingScaling` is not part of `all` and was not run.)

Checkpoint 2 ran on F19's code before its last edits, which changed only
comments and one pointer expression for the same value (`Disconnect` passes
`self.handle()` instead of the same address taken by hand); the lab run of §4
ran on `74dab7c9a` itself.

## 4. Equivalence (plan A.4 item 4)

Two checks.

**Fresh recordings of the fixed build** (`74dab7c9a`). Each run was
recorded with `MAKO_RAFT_REPLAY_DIR` on local tmpfs, then `core_replay` fed
every recorded event to a fresh core and compared every record's actions,
log lines and reply with the recording, byte for byte. The recordings are
kept compressed as `$RESULTS/replay/74dab7c9a.tar.gz` (1.8 MB), the baseline
for later replays.

| Recording | Files (one per server) | Steps matched | Stopped at a `T` line | Mismatched |
|---|---|---|---|---|
| a full lab run (32 cases, all passed) | 9 | 31,902 | 9 | 0 |
| G1, 5 s | 3 | 41,785 | 0 | 0 |
| G4, 5 s | 18 | 106,660 | 0 | 0 |

The `T` lines are the lab's snapshot cases, outside the verified
configuration, as at Phases 6 and 8. This is the check F19 needed: it
changed how every shell function reaches the core (`core()` rather than
the field), and a write that bypassed `step` would make the replayed state,
and soon an output, differ from the recording.

**Phase 6's recordings** (`$RESULTS/replay/ce967f6f7/`), replayed through
the fixed core. G1 matches in full (41,361 steps). The lab (19,345 steps
matched; 7 files stop at a `T` line) and G4 (63,251 matched) differ in 8
files, each at its first `settle` and only there. Every one is a campaign
that waited out its deadline with refusals the new rule counts as a loss:

- the lab's two: five replicas, one grant and three refusals. F15:
  `3 > (5 - 1) - 2`, lost; the old rule: `3 > 5 - 2` is false, so it
  timed out;
- G4's six: three replicas, two refusals at term 1 from peers not yet
  ready. F15: `2 > (3 - 1) - 1`, lost; the old rule: `2 > 3 - 1` is false.

The old core ended such a campaign as timed out. The fixed one ends it as
lost, which also records the role as follower (a `ROLE_SET` action); the
server leads in neither case. A file's replay stops at its first mismatch,
so nothing after it is compared; that is why the lab's and G4's step counts
are lower than Phase 6's (31,792 and 108,318).

## 5. Performance checkpoint (plan 0.10)

`build_rust_base` (`verus-p0`) against `build_rust` (built from
`74dab7c9a`, every fix in), alternated round by round (load 2.8 at the
start), rounds and bounds from [../gate-params.md](../gate-params.md)
(`$RESULTS/fixes/`). G1-G6 and G7, as after Phase 8. **Every point
passes.**

| Point | What | Median (fixes vs Phase 0) | Bound | p | Verdict |
|---|---|---|---|---|---|
| G1 | 4 KB at 240/s, latency (10 rounds) | p50 +0.67%, p99 +0.68% | +2%, +5% | 0.344, 0.754 | pass |
| G2 | 4 KB unthrottled, time of rounds that sent to both followers, traced (10) | +1.52% | +2% | 0.344 | pass |
| G3 | 286 KB x 6 partitions at 190/s, latency (25) | p50 -0.06%, p99 +0.92% | +2.4%, +6% | 1.000, 0.108 | pass |
| G4 | 286 KB x 6 partitions unthrottled, throughput (11) | +1.43% | -2% | 0.549 | pass |
| G5 | 1 MiB at 55/s, latency (20) | p50 -0.85%, p99 -0.50% | +2%, +5% | 0.115, 0.503 | pass |
| G6 | 1 MiB unthrottled, throughput (12) | +0.41% | -2% | 0.754 | pass |
| G7 | leader kill: new leader / first commit, median and p90 (20 kills each, none failed) | -0.9% / -0.8%; -10.6% / -10.5% | 10% | 0.643, 0.632 | pass |

p is the sign test's (G1-G6) or Mann-Whitney's (G7).

**What the fixes add to the hot path.** F19: the core is reached through
an accessor that compiles to the field's address; the atomics it adds on
the replication path are Relaxed loads (plain moves on x86-64) and one
`fetch_add` per batch of committed entries; no lock. F18: one uncontended
mutex acquisition and a deque push per submission in `RaftWorker::Submit`,
which raft_bench goes through (`add_log_to_nc`). F12, F14, F15: one
comparison each. G1-G6 show no cost.

**G7.** F15 cannot shorten a failover in a three-replica cluster: with the
leader dead, a candidate has one live peer, and one refusal does not reach
`(3 - 1) - 1`. The p90 gains are not significant (p = 0.64) and the medians
did not move.

**G2's reported lines (not gated, plan 0.10).** Untraced, 25 rounds:
throughput -8.01% (lower in 17 of 25, p = 0.108). As at Phases 6 and 8 the
follower-behind count accounts for it: the base build ended 13 of its 25
runs with a follower behind, this build 1 (Fisher p < 0.001), and a run
that leaves a follower behind skips it in most rounds, so its rounds are
cheaper. Like with like: runs that ended caught up, -0.12% (Mann-Whitney
p = 0.801).

## 6. Notes and deviations

- **F19 changes an interface.** `RaftSpecific`'s twelve methods take `&self`
  at their declaration in `src/deptran/scheduler.h`, so the C++ virtuals
  and the `RaftServer` shim are `const`. `TxLogServer`, which PaxosServer
  implements too, is unchanged; the Raft server's exports of `set_commo` and
  `reg_learner_action` call `&self` twins instead of the trait methods,
  because the worker calls `reg_learner_action` after it registers the RPC
  service.
- **F19's discipline is stated, not checked.** Each `ShellCell` field names
  the lock or the single owner it is reached under; as with `mtx_` before,
  nothing enforces it. What the fix removes is the aliasing itself: no two
  `&mut RaftServerBase` exist at once, and `ShellCell` is `Sync` only for a
  `Send` payload.
- **Phase 6's recordings no longer replay byte for byte** (§4): F15 changes
  election decisions, as intended. Fresh recordings are the equivalence
  baseline from here on.
- **The ghost-log case study** (`ZhangZihao270/ghost-log-refinement`,
  branch `work/mako-raft-log-refinement`) still carries the core as it was
  before these fixes; it was not updated.

## 7. Bugs found

None new. The fixes closed B1, B4, B7, B8, B14, B16, B17, B18 and B19; the
fates are in [../bugs-found.md](../bugs-found.md).
