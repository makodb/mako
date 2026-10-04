# Phase 6 report

Plan: [../modification-plan.md](../modification-plan.md), Phase 6 ("Separate
core crate in the Verus subset"), the performance checkpoint after it (plan
0.10). Raw results: `$RESULTS/p6/` and `$RESULTS/replay/` (not in git).

No mako-dev merge at the phase start: `origin/mako-dev` (`3e102604d`) was
already merged.

## 1. What changed

| Commit | What |
|---|---|
| `43c57e3ac` | The hybrid and cpp lanes removed (user decision Q9): the worktree is rust-lane only. |
| `896cdfc85` | The core becomes the crate `raft-core` (`src/deptran/raft/core`): M1 moves; M11 (the command is the type parameter `C`, the payload any `InboundBatch`); M7 (the core's 52 log calls push records into `CoreOutput`). |
| `53bb170b4` … `192e15c39` | Every core module verifies under the pinned Verus (M12): panic and overflow freedom, every M10 check proved, `RaftCore`'s invariant kept by every call. M10 explicit wraps (`wrapping_add`, `wrapping_sub`) where release builds always wrapped; M9 `SiteSet` insert/remove by `Vec::insert`/`remove`. |
| `62a237637` | M1: the snapshot threshold and callback tokens leave the core (it never read them). |
| `7aa01a586` | Setup through the core: `set_identity` (M1), `configure` (M1, and F5: the peer table built with the membership), `enter_gates` (F5: `verified_config_ok`'s decision in the core; a pass sets `gated_`). |
| `4962bd6d8` | `step(Event)`: the core's one entry point (M5); the shell calls nothing else. The decode scratch moves into the core. B15 fixed: the round-state resets take `mtx_`. |
| `affc46aa7` | The replay recorder (M0) and the `raft-replay` crate with `core_replay` (A.4 item 4). |
| `9665c8f3e` | F9: `step_checked`. |
| `3d768bdcf`, `0986e4b7f`, `ce967f6f7` | The ledger lint, run with clippy, the correspondence check and Verus by `core_check.sh`. |

Per-item detail: [../diff-ledger.md](../diff-ledger.md), "Phase 6".

## 2. Verification

`scripts/verus/verify_core.sh`: **321 verified, 0 errors**, every module of
the crate inside `verus!`. The trusted surface (`scripts/verus/core_trusted.txt`)
is one function, `blocks_for` (`u64::div_ceil`, which vstd does not specify).
What is proved, for every core call and for `step` and `step_checked`:

- **Panic freedom.** Every index in bounds, every `unwrap` on a `Some`, every
  loop terminating, and every M10 check (`runtime_assert`, the plan's ~29
  `assert!`s) proved to hold.
- **Overflow freedom.** All index and term arithmetic, under the 2^62 index
  ceiling (`raft_index_limit`).
- **`RaftCore`'s invariant** (`core/src/node.rs:110`), kept by every call: the
  log's block layout; the peer table is the configuration without this
  server; the term below the ceiling; `snapidx ≤ commit ≤ last index`, the
  log starting at or below the snapshot boundary's successor; a strictly
  sorted configuration; an opened round admitted exactly the configuration;
  round ids fresh within a leadership epoch; a leader runs no campaign; an
  epoch's term never passes the current term; and the gate's facts once
  `enter_gates` passed (the configuration contains this server, snapidx 0,
  base 1).
- **F9:** a dropped message leaves the core exactly as it was.

### The host contract so far

What the verified calls assume of the shell, stated as preconditions (Phase 8
writes them up as `docs/verus/host-contract.md` with the rest of the plan's Phase 8
list):

1. **The index ceiling.** No index the shell hands the core reaches 2^62:
   a proposal finds room in the log; an inbound batch the decoder accepts
   ends below the ceiling (`InboundBatch::decode_terms`). A log that long would
   hold 4.6e18 entries.
2. **Terms off the wire are below the ceiling**: a RequestVote's, a vote
   reply's, an AppendEntries', an append reply's; and a campaign's next term.
3. **The inbound batch is the RPC's**: `has_cmd` is the flag the batch was
   built from, and no payload decodes to no terms.
4. **The heartbeat round's order**: the tick's `is_leader` is
   `IsLeaderLocked` (it reads the core's role); a reply's slot is one of the
   core's; a round end follows a tick that opened the round.
5. **The configuration contains this server** (the tick and the round end;
   F5's gate checks it, and `gated_` carries it).
6. **Setup's order**: the identity before the membership; the membership once,
   sorted and duplicate-free (the config kernel's contract), before any round.
7. **Serialization (Q1)**: every core call under `mtx_` (B15 fixed the two
   that were not).
8. **The core changes only through `step` and `step_checked`**, except the
   snapshot paths, outside the verified configuration, which mark the
   recorder (§4).

## 3. Tier 1

At `ce967f6f7` (`$RESULTS/p6/tier1/`), rust lane, the lab's tree rebuilt from
that commit first:

| Suite | Result |
|---|---|
| raftLabTest (31 cases: case 15, F9's, is new) | pass, 279 s (with its build) |
| shard1ReplicationRaft | pass, 56 s |
| shard2ReplicationRaft | pass, 68 s |
| shard1ReplicationSimpleRaft | pass, 41 s |
| shard2ReplicationSimpleRaft | pass, 51 s |

No retries in any log. Along the way, `shard1ReplicationSimpleRaft` also
passed at `4962bd6d8` (the `step` conversion) and with the recorder on at
`affc46aa7`. The crate's own tests pass: raft-core's `tests/step_checked.rs`
(F9's reply case and its drops), raft-replay's unit tests (the format, and a
history recorded through `step`, replayed, a tampered reply caught at its
line, a `T` line stopping the replay).

## 4. Equivalence (A.4 item 4)

The recorder and the replay arrive in this phase with `step`, so there are no
parent recordings to replay (Phases 3 and 4 deferred them here: their
reports, §4). What runs instead, at `ce967f6f7`
(`$RESULTS/replay/ce967f6f7/`): each run recorded with
`MAKO_RAFT_REPLAY_DIR`, then `core_replay` feeding every recorded event to a
fresh core and comparing every record's actions, log lines and reply with
the recording, byte for byte.

| Recording | Files (one per server) | Steps matched | Stopped at a `T` line | Mismatched |
|---|---|---|---|---|
| a full lab run (31 cases) | 9 | 31,792 | 9 | 0 |
| G1, 5 s (4 KB, 240/s) | 3 | 41,361 | 0 | 0 |
| G4, 5 s (286 KB, 6 partitions, unthrottled) | 18 | 108,318 | 0 | 0 |

The lab's recordings stop where its snapshot cases (outside the verified
configuration) write the core directly: five servers part-way through the
run, and the four fresh servers the recovery cases build, at their second
line. Everything up to there matches.

Two things this shows beyond determinism. The core is a function of the
recorded events alone: nothing it decides depends on a clock, a random
number or shell state the event does not carry (M4 held). And the shell
changes the core only through `step`: a direct write anywhere else would make
the replayed state, and soon an output, differ from the recording. These
recordings are the parent recordings Phase 8's ghost-instrumented core must
replay byte for byte (A.4 item 4).

## 5. Performance checkpoint (plan 0.10)

`build_rust_base` (`verus-p0`) against `build_rust` (`ce967f6f7`), alternated
round by round on a quiet machine (load 1.7 at the start), rounds and bounds
from [../gate-params.md](../gate-params.md) (`$RESULTS/p6/`). No G7 (after
Phases 3 and 8 only). **Every point passes.**

| Point | What | Median (Phase 6 vs Phase 0) | Bound | Sign test p | Verdict |
|---|---|---|---|---|---|
| G1 | 4 KB at 240/s, latency (10 rounds) | p50 −0.21%, p99 +1.11% | +2%, +5% | 0.754, 0.754 | pass |
| G2 | 4 KB unthrottled, time of rounds that sent to both followers, traced (10) | −0.43% | +2% | 0.754 | pass |
| G3 | 286 KB × 6 partitions at 190/s, latency (25) | p50 +0.81%, p99 +0.50% | +2.4%, +6% | 0.230, 0.690 | pass |
| G4 | 286 KB × 6 partitions unthrottled, throughput (11) | +0.59% | −2% | 1.000 | pass |
| G5 | 1 MiB at 55/s, latency (20) | p50 −0.04%, p99 +1.77% | +2%, +5% | 0.824, 0.115 | pass |
| G6 | 1 MiB unthrottled, throughput (12) | +1.04% | −2% | 0.109 | pass |

**G2's reported lines (not gated, plan 0.10).** Untraced, 25 rounds:
throughput −5.09% (Phase 6 lower in 19 of 25, p = 0.015). The follower-behind
count accounts for it: the base build ended 15 of its 25 runs with a follower
behind, Phase 6 only 3 (Fisher p = 0.001). A run that leaves a follower behind
skips it in most rounds, so its rounds are cheaper and its throughput higher
(Phase 3 report, §3.1); Phase 6 keeps both followers caught up far more often,
which is the better outcome for replication and costs raw throughput at G2's
saturation. Comparing like with like: runs that ended caught up, +1.71% for
Phase 6 (Mann-Whitney p = 0.792); runs that ended behind, 38,016/s against
37,702/s. The gated metric, the time of a round that sent to both followers,
is 0.43% faster.

## 6. Plan deviations

- **`new_gated(cfg) -> Result` is three Setup calls.** The core exists before
  Setup (the worker sets the identity first, and snapshot recovery, outside
  the gate, can restore state before the membership loads), so a constructor
  would reorder Setup. Instead `set_identity`, `configure` and `enter_gates`
  run where the shell's writes ran; `enter_gates` is F5's decision in the
  core, and a pass sets `gated_`, whose facts the invariant carries.
- **`step` returns a `Reply`.** The plan's `step(&mut self, ev, out)` has no
  result; the calls' results (the tick, the append report, the campaign...)
  are `Reply` variants, and `CoreOutput` stays actions and log records.
- **Terms stay `i64`/`u64` as the wire has them.** "Terms as a single u64" is
  not forced by the Verus subset: the mixed casts verify, proved in range
  under the ceiling. Converting would move the negative-term refusals
  (`can_term < 0`, F4's `term < 1`, frozen behaviour) or rewrite their
  comparisons, the class of change that once let a far-stale candidate win a
  vote (the comment in `raft_on_request_vote`). Phase 8 maps terms to `nat`
  under boundary condition 2 (terms on the wire are non-negative).
- **`RaftLog`'s `view(): Seq<Term>` waits for Phase 8.** It is ghost (M12) and
  what Phase 8's state view needs; Phase 6 proves the layout and the index
  arithmetic the view will rest on.
- **`with_core` is not compiler-enforced.** `RaftCore`'s fields stay `pub`:
  the shell reads them for its log lines, mirrors and the lab, and the specs
  name them. Writes are enforced otherwise: every decision is a `step`; the
  only direct writes are the snapshot paths (outside the gate), which mark
  the recorder; and the replay would catch any other (§4).
- **F9's drop is the unavailable answer**, not an RPC error, so no RPC
  signature moves: 0/0/0 for an append (the leader's "no reply"), "no" at
  the candidate's term for a vote request (V3's unmodelled input). A
  payload's defects stay the decoder's refusals, as A.2's closing paragraph
  keeps them; "term 0" is read as written (a negative vote term stays the
  handler's malformed refusal). F9's new behaviour is lab case 15, and its
  reply case raft-core's tests (a reply cannot be injected into a live lab
  round, as for F1).
- **The ledger lint checks Phase 6's changes, not Phase 0's.** Its moved
  corpus is the shell at `43c57e3ac`; Phases 1-4 ledgered their changes per
  function, not with a tag on every line.
- **No parent recordings** (§4).

## 7. Bugs found

B15 (the round state reset without `mtx_`; fixed). B14's summary row, missing
since Phase 4, is added.
