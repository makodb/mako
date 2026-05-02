# Raft Phase 8.5 Breakdown (TestCluster with real RaftServer)

## Why this breakdown exists

The original Phase 8.5 first bullet in `docs/TODO-raft.md` is larger than a
single low-risk commit because it combines:

1. `RaftNode` ownership changes
2. `RaftServer` lifecycle/thread/fiber startup
3. test harness behavior changes

To keep each commit reviewable and debuggable, split 8.5 into smaller leaves.

## Decomposed leaves

### 8.5.a Contract-stabilization leaf (this commit)

- Keep the current `RaftNode` skeleton operational after Phase 8.3 expanded
  `DispatcherFacade` with membership handlers.
- Add `DummyDispatcher::handle_add_server` and
  `DummyDispatcher::handle_remove_server`.
- Add a targeted `raft_test_cluster` case that exercises the two handlers
  through `RaftNode::take_dispatcher()`.

Rationale:
- This is a correctness gate for the skeleton baseline.
- It prevents build breaks while larger 8.5 server-integration work is staged.

### 8.5.b RaftNode server-ownership scaffolding

- Introduce `RaftNode` fields required for real-server ownership
  (`rusty::Box<RaftServer>` and any required storage of runtime hooks),
  but do not start election/heartbeat/apply loops yet.
- Preserve existing cluster tests by keeping a fallback dispatcher path.

Status:
- Added `RaftNode::server_` ownership scaffold as
  `rusty::Option<rusty::Box<RaftServer>>` with read-only accessors
  (`server()`, `has_server()`).
- Kept `DummyDispatcher` as the active dispatcher backend for now.
- Added `RaftTestClusterTest.ServerOwnershipScaffoldStartsEmpty` to lock
  in the baseline (`has_server()==false`) until 8.5.c/8.5.d wire a real server.

### 8.5.c Minimal RaftServer-for-tests bootstrap

- Add a narrowly scoped test bootstrap path for `RaftServer` that can run in
  `TestCluster` without full deptran-server scaffolding.
- Wire transport/storage/snapshot dependencies explicitly.

### 8.5.d Switch RaftNode dispatcher from Dummy to RaftServerDispatcher

- Replace `DummyDispatcher` dispatching path with
  `make_raft_server_dispatcher(server_.get())`.
- Keep cluster construction and worker-thread topology unchanged.

### 8.5.e Behavioral test upgrades

- Add/enable 8.5 goal tests:
  - election convergence
  - agreement/commit-index advance
  - disconnect + catch-up behavior

### 8.5.f Cleanup leaf

- Remove `DummyDispatcher` and stale skeleton-only paths once no references
  remain.
