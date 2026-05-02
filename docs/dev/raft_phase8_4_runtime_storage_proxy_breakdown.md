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
3. **8.4.c3**: finalize callsite cleanup and factory/wiring consistency across
   recovery/bootstrap/test harness paths.

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
