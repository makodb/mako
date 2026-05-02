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

Status (c1):
- Added frame-less `RaftServer(site_id, partition_id, loc_id)` constructor.
- Removed hard `frame_->site_info_` dereferences from election paths that
  block frame-less construction (`RequestVote`, debug log site lookup).
- Added `test_raft_server_test_mode_ctor` to verify identity initialization
  with `frame_ == nullptr`.

Status (c2):
- `RaftNode` now owns a real frame-less `RaftServer` at construction time.
- In-memory storage/snapshot backends are wired into the server via
  non-owning shared_ptr aliases.
- `take_dispatcher()` now returns `RaftServerDispatcher` backed by the node's
  server.
- Timer/fiber startup remains deferred; this leaf intentionally keeps the
  behavior change limited to ownership + dispatcher routing.

Status (c3):
- Added `RaftServer::BootstrapCurrentConfigForTest(...)` so frame-less
  in-process servers can populate `current_config_` without `Setup()` /
  `Config::GetConfig()`.
- `TestCluster` now passes full site-id membership into each `RaftNode`
  constructor, and `RaftNode` seeds the owned server's membership during
  construction.
- Added `RaftNode` accessors (`server_config_size`,
  `server_config_contains`) and a focused test asserting membership is
  bootstrapped on all nodes.

Status (c4):
- Removed obsolete `DummyDispatcher` implementation from `raft_node.hpp`
  (no remaining runtime references after c2/c3).
- Updated harness comments/docs to consistently describe
  RaftServerDispatcher-backed in-process dispatch.
- Added a behavior test (`VoteRejectsDifferentCandidateInSameTerm`) that
  would fail under the old dummy "always grant vote" semantics.

Status (c5):
- `RaftNode` inspection accessors (`is_leader`, `current_term`,
  `commit_index`) now read directly from the owned `RaftServer` instead of
  local shadow fields.
- Test helper mutators (`force_leader`, `set_current_term`,
  `set_commit_index`) now write into owned server state.
- Frame-less `RaftServer` constructor marks `looping_ = true` in test mode
  so `IsLeader()` can reflect `setIsLeader()` without running fibers.

Status (c6):
- Added `RaftServer` replication-peer discovery fallback for frame-less
  mode: when `commo()` is absent, peer lists come from
  `current_config_` + `learners_`.
- Added test bootstrap/readiness helpers:
  `BootstrapReplicationStateForTest()` and
  `ReplicationStateReadyForHeartbeatTickForTest()`.
- `TestCluster::with_in_memory_transport(n)` now pre-initializes each
  node's `next_index_` / `match_index_` maps so servers are ready for the
  first heartbeat tick before timer/fiber bring-up.
- Added focused tests in `raft_server_test_mode_ctor_test` and
  `raft_test_cluster_test` to lock this readiness contract.

Status (c7):
- Added `RaftServer::StartInProcessTestRuntimeForTest()` as an idempotent
  test-harness entrypoint that starts heartbeat/election fibers and apply
  runtime without going through full deptran `Setup()`.
- `TestCluster` now creates one `PollThread` per node and schedules that
  runtime startup via `OneTimeJob`, preserving the poll-thread affinity used
  by production Raft startup paths.
- Added a runtime smoke test (`RuntimeStartupElectsSingleLeader`) to verify
  election convergence once timers/fibers are running in-process.

Status (c8a):
- Added `RaftTestClusterTest.AgreementAdvancesCommitIndexOnAllNodes` as the
  `DoAgreement`-equivalent leaf: submit one leader log entry and wait until
  every node reports `commit_index() >= log_index`.
- `TestCluster` now registers a default no-op learner callback so frame-less
  apply threads can execute committed entries without full scheduler wiring.
- Hardened frame-less test mode by guarding rule-witness `Config::GetConfig()`
  dependencies (`RuleWitnessGC` and original-path placeholder) when global
  `Config::config_s` is absent.
- Fixed a teardown race in leadership-transfer monitor threads:
  `StopLeadershipTransferMonitoring()` now requests stop and joins (with
  self-join guard), and the monitor loop checks the stop flag before locking
  `mtx_` to avoid post-destruction `recursive_mutex` lock failures.
- Gate run for this leaf: `ctest -R '^(test_raft_.*|raft_lab_standalone)$'`
  passes.

Status (c8b):
- Added `RaftTestClusterTest.DisconnectFollowerBlocksCatchupUntilResetFaults`
  to cover the remaining fault-injection behavior leaf:
  disconnect one follower, append entries on the leader, verify majority
  commits while the disconnected follower stays behind, then verify the
  follower catches up only after `reset_faults()`.
- Re-ran the full Raft gate (`ctest -R '^(test_raft_.*|raft_lab_standalone)$'`)
  and confirmed no regressions.

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

## Remaining split for the large 8.5 node-startup bullet

1. All c8 behavioral leaves are complete; remaining work is Phase 8.5 commit
   bookkeeping in `docs/TODO-raft.md`.
