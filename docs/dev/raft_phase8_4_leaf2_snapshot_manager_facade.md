# Raft Phase 8.4 Leaf 2: SnapshotManager Facade

## Scope selection

Phase 8.4 still includes runtime migration of `RaftServer` storage members to
proxy types. That migration is larger and riskier than a single commit.

This leaf intentionally scopes to the second scaffold bullet only:

1. Add `src/deptran/raft/snapshot_manager_facade.hpp`
2. Mirror every `SnapshotManager` method in a proxy facade
3. Add facade conformance tests

## Why this is commit-sized

- One new header and one focused test target.
- No runtime behavior change in `RaftServer` ownership/wiring.
- Validation is deterministic: facade conformance plus full raft suite.

## Design rationale

- Follow the same proxy scaffold used for `TransportFacade`,
  `DispatcherFacade`, and `LogStorageFacade`.
- Mirror method signatures exactly, including:
  - `unique_ptr<SnapshotWriter>` and `unique_ptr<SnapshotReader>` return paths
  - raw-pointer arguments for `TakeSnapshot` / `LoadLatestSnapshot`
  - const query methods returning `rusty::Option` and vectors
- Keep this as a pure boundary contract so the later runtime migration leaf can
  be mechanical.

## Operator notes

- New focused test target:
  - `test_raft_snapshot_manager_facade`
- Full regression gate used:
  - `ctest --test-dir build --output-on-failure -R '^(test_raft_.*|raft_lab_standalone)$'`
