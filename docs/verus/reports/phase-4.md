# Phase 4 report

Plan: [../modification-plan.md](../modification-plan.md), Phase 4 ("Enforced
serialization and mirrors"), reported at stopping point 0.7 point 5. Raw
results: `$RESULTS/p4/` (not in git). Testing per plan 0.10: Tier 1 at the
phase end (run twice, as Phase 4's gate asks, counting first-attempt
failures), a TSan lab build if the toolchain allows, no performance run.

No mako-dev merge at the phase start: `origin/mako-dev` had no commits since
the last merge (`3e102604d`).

## 1. What changed

| Commit | What |
|---|---|
| `c62fc2519` | F8: `IsLeader`, `GetLeaderHint` and `CommitIndex` read atomic mirrors the shell publishes under `mtx_` at the end of every critical section that runs a core decision (and after its own writes in snapshot recovery and install); the term is published with them. Fixes B2. |
| `366b25935` | M3: the applied index's "never backward" decision is `RaftCore::on_applied`. M1: the preferred leader is a shell atomic written without `mtx_`. |
| `22a54332c` | F5: under `MAKO_RAFT_VERIFIED_GATES=1`, `CompactLog` and `MaybeCreateSnapshot` do nothing. |

Per-item detail: [../diff-ledger.md](../diff-ledger.md), "Phase 4".

## 2. Tier 1, twice

Three lanes, all suites, run back to back (`$RESULTS/p4/tier1/`,
`$RESULTS/p4/run2/tier1/`):

| Suite | Run 1 | Run 2 |
|---|---|---|
| raftLabTest (rust lane, 30 cases) | pass, 240 s | pass, 237 s |
| shard1ReplicationRaft | pass, 58 s | pass, 57 s |
| shard2ReplicationRaft | pass, 68 s | pass, 68 s |
| shard1ReplicationSimpleRaft | pass, 41 s | pass, 41 s |
| shard2ReplicationSimpleRaft | pass, 51 s | pass, 51 s |
| raftLabTestHybrid (30 cases) | pass, 243 s | pass, 236 s |
| raftLabTestCpp (30 cases) | pass, 348 s (including its build) | pass, 235 s |

No retries in any log (the plan counts first-attempt failures because a
threading bug can hide behind `ci.sh`'s retry). The lab binaries of run 1
were rebuilt from the Phase 4 tip before they ran.

## 3. TSan lab build

The toolchain allows it, for the C++ half only. Clang 22 ships the TSan
runtime and the repo has a `MAKO_TSAN=1` switch (libc malloc,
`-fsanitize=thread`). Stable rustc 1.95 cannot instrument Rust (that needs
nightly's `-Zsanitizer=thread` and a rebuilt std), so the rust lane's core
would be invisible to TSan. The build is the **cpp lane's** lab, whose Raft
core is the transpiled C++ of the same crate
(`build_tsan_raftlab_cpp`, `$RESULTS/p4/tsan/`; 4 min 54 s, 614 compile
lines with `-fsanitize=thread`).

Result: **all 30 cases passed in 208 s**; exit status 66 (TSan's "reports
were made"); **3 reports, one cause**: `heartbeat_interval_us_`, written by
`SetHeartbeatInterval` from lab case 67 while the heartbeat loop (two
servers) and the election timer (one) read it. That race predates this work
and is test-only. Production writes the field only before the loops start.
It is recorded as B14, with a proposed fix that waits for the user. Nothing
in Phase 4's mirrors, the notice queue (F6) or the core calls was reported.

## 4. Equivalence

A.4 item 4 asks for a byte-identical replay of the parent's recordings
through the child's core. There are no recordings yet: the recorder was
deferred from Phase 3 (its report, §4) because some core mutations still
bypass recorded calls, and the single `step()` entry point it records at
arrives with Phase 6's crate. The lab passing on all three lanes is the
check, as it was for Phase 3.

## 5. Plan deviations

- **`with_core` is not compiler-enforced.** The plan wants `with_core` to be
  the only accessor, with the compiler finding every direct field access.
  That needs `RaftCore` in a module of its own. The shell would import a
  wrapper that imports the core, and a C++20 module graph may not be cyclic
  (the cpp and hybrid lanes transpile this crate into modules). Phase 6
  makes that split when the core becomes a crate.
  `scripts/verus/core_access_census.py` is the work list until then: it
  lists every shell function that names a core field: 188 accesses in 40
  functions at this phase's tip, 108 of them in the four snapshot functions
  (`OnInstallSnapshotLocked`, `InitializeSnapshotManagerLocked`,
  `InstallSnapshotReplyAcceptedLocked`, `CreateSnapshotLocked`), which the
  verified configuration never reaches.
- **The replay check waits for Phase 6** (section 4).

## 6. Bugs found

B14 (the heartbeat interval, a plain field written while the loops read it;
lab-only; found by the TSan build; fix proposed, not landed). B2 (the
unlocked `CommitIndex` read) is fixed by F8.
