# Phase 8.7: TestCluster-backed `raft_lab_standalone`

## Scope

Phase 8.7 replaces the old 4-case standalone smoke runner with a
`RaftLabTest` driver on top of `TestCluster` + `RaftTestConfig(TestCluster&)`.

## Design decisions

1. `RaftTestConfig(TestCluster&)` now treats public test-control server
   identifiers as **0-based test indexes** (`0..N-1`) and resolves them to
   internal TestCluster site IDs (`1..N`) at call boundaries.
2. `TestCluster` now exposes `node_or_null(siteid_t)` so cluster backend code
   can safely probe for missing nodes without throwing `std::out_of_range`.
3. Cluster backend disconnect/reconnect paths resolve server ID exactly once
   (fixed a double-resolution bug that disconnected the wrong node).
4. `DoAgreement()` cluster backend now tracks leader in index-space, so retry
   checks (`GetServer(ldr)`) inspect the correct node.
5. `RaftLabTest::Run()` stops after Test 64 when running on the TestCluster
   backend. Tests 65+ require full process restart + persistence semantics that
   `TestCluster::restart()` does not yet emulate.

## User-facing behavior

- `./build/raft_lab_standalone` now runs the decoupled lab flow through:
  - Basic Raft tests (1-11)
  - Snapshot/install-snapshot tests (50-60)
  - Rollback tests (63-64)
- For TestCluster backend runs, the runner logs an explicit notice that
  restart/persistence-heavy tests (65+) are skipped.
- The process exits non-zero on any executed test failure.

## Verification

- Standalone run log:
  - `logs/20260502-192608-d0570e7f4-raft_lab_standalone.log`
- Full raft suite log (includes `raft_lab_standalone`):
  - `logs/20260502-193020-d0570e7f4-ctest-raft-suite.log`

