# Raft Phase 8.4 Runtime Storage Proxy Migration Breakdown

## Why the original leaf was too large

The original TODO item combined two member-type migrations
(`log_storage_` and `snapshot_manager_`), compatibility concerns for existing
`Set*/Get*` APIs, and callsite wiring updates in one step. Doing all of that in
one commit would make regression debugging difficult because failures could come
from either storage boundary.

## Decomposition

1. **8.4.c1 (this commit)**: migrate `RaftServer::log_storage_` to
   `LogStorageProxy`, but keep `SetLogStorage/GetLogStorage` API stable by
   storing a shared ownership handle (`log_storage_owner_`) and wrapping it into
   a proxy.
2. **8.4.c2 (this commit)**: apply the same pattern to `snapshot_manager_`
   with `SnapshotManagerProxy` and a compatibility owner handle.
3. **8.4.c3 (this commit)**: finalize callsite cleanup and factory/wiring
   consistency across recovery/bootstrap/test harness paths.

## Design rationale for 8.4.c1

- Keep runtime semantics unchanged: code still sees optional storage, and
  existing `shared_ptr`-based setup/retrieval paths still work.
- Enforce proxy boundary internally in `RaftServer` now, so later migration
  leaves are mechanical.
- Add focused test coverage for:
  - compatibility of `SetLogStorage/GetLogStorage`
  - successful recovery path through proxy-backed `log_storage_`.

## Design rationale for 8.4.c2

- Keep snapshot wiring semantics unchanged while enforcing the proxy boundary
  internally:
  - `SetSnapshotManager(shared_ptr<SnapshotManager>)` still accepts all
    existing callsites.
  - `GetSnapshotManager()` still returns the original shared pointer.
- Use a forwarding `SnapshotManagerProxyAdapter` so all server-side snapshot
  operations (`HasSnapshot`, `CreateSnapshot`, install-snapshot paths) call
  through `SnapshotManagerProxy`.
- Add focused test coverage for:
  - compatibility and lifetime behavior of `Set/GetSnapshotManager`
  - `HasSnapshot()` correctness with proxy-backed manager state.

## Design rationale for 8.4.c3

- Remove adapter duplication from `RaftServer` internals:
  - extracted shared proxy wiring logic into `storage_proxy_wiring.hpp`.
  - both storage boundaries now use the same factory helpers:
    - `make_log_storage_proxy`
    - `make_snapshot_manager_proxy`
- Normalize wiring callsites:
  - `RaftNode` raw pointer aliases now use shared helper functions
    (`make_non_owning_log_storage`, `make_non_owning_snapshot_manager`)
    instead of reimplementing alias deleters inline.
  - `InitializeSnapshotManager()` metadata load path now reads through
    `snapshot_manager_` proxy after `SetSnapshotManager(...)`, so bootstrap
    path uses the same boundary as runtime snapshot operations.
  - recovery path in `Setup()` keeps using `SetLogStorage(...)`; the setter is
    now the single conversion point for proxy wiring.

## Validation

- Focused tests:
  - `test_raft_storage_proxy_wiring`
  - `test_raft_server_log_storage_proxy`
  - `test_raft_server_snapshot_manager_proxy`
- Full raft gate:
  - `ctest --test-dir build --output-on-failure -R '^(test_raft_.*|raft_lab_standalone)$'`
