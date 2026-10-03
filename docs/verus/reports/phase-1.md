# Phase 1 report

Plan: [../modification-plan.md](../modification-plan.md), Phase 1 ("One
`RaftCore` struct; F1, F3-F5"), reported at stopping point 0.7 point 5. Raw
results: `$RESULTS/p1/` (not in git). Testing between phases follows plan 0.10:
Tier 1 once at the phase end, no performance run (the next one is after
Phase 3).

## 1. What changed

| Commit | What |
|---|---|
| `c5fa64ec5` | M1/M2/M9: `RaftConsensusState` → `RaftCore`, `state_` → `core`; the heartbeat round's state (in-flight slots, authority ledger, round scope) moves into `RaftCore`; the authority sets become a sorted `Vec<u16>` |
| `b75f285e2` | F1: the Rust lane's vote tally counts each voter once per campaign |
| `372b73e6f` | F3: phase 1 re-checks leadership in the locked block that builds each AppendEntries, and sends the commit index read there |
| `a38c5012e` | F4: an AppendEntries entry of term below 1 is refused; lab case 12 |
| `771d238c5` | F5: `verified_config_ok()` at setup; `MAKO_RAFT_VERIFIED_GATES=1` fails closed outside the verified configuration |
| `15160ea4d` | lab case 13 pins today's reply from an unavailable voter (V3); no behaviour change |
| `590370719` | regenerated `docs/migration/raft/cpp-rust-correspondence.md` (the build's freshness check) |

Per-hunk detail: [../diff-ledger.md](../diff-ledger.md), "Phase 1".

## 2. Tier 1

Commit `590370719`, three lanes (`$RESULTS/p1/tier1/`):

| Suite | Result |
|---|---|
| raftLabTest (rust lane, 29 cases) | pass, 284 s |
| shard1ReplicationRaft | pass, 62 s |
| shard2ReplicationRaft | pass, 68 s |
| shard1ReplicationSimpleRaft | pass, 40 s |
| shard2ReplicationSimpleRaft | pass, 51 s |
| raftLabTestHybrid (29 cases) | pass, 277 s |
| raftLabTestCpp (29 cases) | first attempt: **failed to compile** (see below); after the fix: pass, 330 s |

First-attempt failures counted by the suites' retry (`Retrying` lines): 0 in
every log.

The cpp lane transpiles the lab, and lab case 12 bound the same name, `ok`,
twice in one scope (two `let Some((ok, _, _)) = ...`). Rust shadows; the
transpiled C++ redeclares, and clang rejects it ("redefinition of 'ok'"). The
two bindings were renamed (`control_ok`, `probe_ok`) in the F4 commit, which
was re-created; only `src/lab_cases.rs` changed, so only the cpp lab was
re-run.

### 2.1 cpp lane after the fix

29/29 cases, `deptran_server` exited 0, no retries
(`$RESULTS/p1/tier1/run_cpp_retry.txt`).

## 3. Equivalence (plan A.4, as amended by 0.10)

The lab passing is the check until Phase 3: all 29 cases pass on every lane,
including the two new ones, and each case checks that no two replicas commit
different entries at one index.

## 4. Build gates met on the way

- The source gate runs clippy with warnings as errors on the Raft crate. The
  sorted-set insert of the M9 change swaps by hand (both lanes' `Vec` support
  only push, pop and indexing), which clippy's `manual_swap` rejects; it
  carries `#[allow(clippy::manual_swap)]` with that reason.
- `raft_correspondence_check` fails the build when
  `cpp-rust-correspondence.md` is stale; every change to a Raft source's line
  count needs `python3 scripts/gen_correspondence.py`.

## 5. Deviations from the plan

- The plan asks for lab cases for a duplicated and a stale-term vote reply.
  The lab cannot inject a reply into a live campaign, so both are raft-rt
  unit tests on the tally itself (`a_repeated_reply_counts_once`,
  `a_stale_term_reply_is_counted_as_cast`).
- The vote request to a non-ready replica is pinned at the reply (case 13),
  not at the candidate's early "no" decision: in the 5-node lab that decision
  needs all four peers to refuse (bugs-found B1).

## 6. Bugs found

None new in Phase 1's own changes. Found while reviewing them for the next
phases and recorded in [../bugs-found.md](../bugs-found.md): B12 (a declined
heartbeat round still runs phases 1-3, harmlessly). Fates updated: B3 fixed by
F1, B4 (terms 0 and negative) by F4, B5 by F3.
