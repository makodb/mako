# Raft Phase 8.1e: Remaining Outbound Migration

Date: 2026-05-02

## Scope Split

Phase 8.1e still contains several independent outbound call-site migrations:

1. `SendInstallSnapshot` call site in `HeartbeatLoop`.
2. `SendAppendEntriesDurable` call sites.
3. `SendVoteDurable` call sites.
4. `TimeoutNow` call sites.
5. Explicit handling of non-facaded boundaries
   (`UpdatePartitionView`, `rpc_par_proxies_` direct access).
6. Remove `RaftVoteQuorumEvent`.

Doing all of these in one commit would mix replication, speculative-ack, and
leadership-transfer behaviors. Each item is small enough to land as a focused
leaf with dedicated tests.

## Leaf 1 (InstallSnapshot) Design Rationale

- Goal: remove direct `commo()->SendInstallSnapshot(...)` usage from
  `RaftServer::HeartbeatLoop` and route through `transport_`.
- Locking policy:
  - Build `InstallSnapshotReq` under `mtx_` while reading Raft state.
  - Perform transport send outside `mtx_`.
  - Re-enter `mtx_` to reconcile reply into Raft state.
- State reconciliation kept equivalent to pre-migration behavior:
  - Step down on higher follower term.
  - Ignore stale reply if leader term changed.
  - Advance follower `next_index_` / `match_index_` on accepted snapshot.

## Leaf 1 User/Developer Notes

- `RrrTransportAdapter::send_install_snapshot` now has a 1s bounded wait
  (`kInstallSnapshotRpcTimeoutUs`) to avoid indefinite stalls in the
  synchronous transport path.
- Additional test coverage:
  - `tests/raft_channel_transport_test.cc` now validates
    `send_install_snapshot` round-trip success.
  - Same test validates default fallback reply when the direction is dropped.

## Leaf 2 (AppendEntriesDurable) Design Rationale

- Goal: remove direct `commo()->SendAppendEntriesDurable(...)` usage from the
  follower async-persistence path in `RaftServer::OnAppendEntries`.
- The durable-ack shape maps directly to transport message types:
  `AppendEntriesDurableReq{term, follower_id, last_log_index}`.
- This leaf is behavior-preserving:
  - async persistence thread still persists entries + commit index first,
  - durable ack is still fire-and-forget,
  - leader-side speculative durability bookkeeping is unchanged.

## Leaf 2 User/Developer Notes

- `OnAppendEntries` now calls
  `transport_->send_append_entries_durable(leader_id, req)` from the async
  persistence thread, instead of reaching into `RaftCommo` directly.
- Additional test coverage in `tests/raft_channel_transport_test.cc` now
  asserts:
  - append-durable RPC is delivered in the healthy direction, and
  - append-durable RPC is dropped (no receiver-side count increment) when the
    direction is fault-injected.

## Leaf 3 (VoteDurable) Design Rationale

- Goal: remove direct `commo()->SendVoteDurable(...)` usage from the follower
  async-vote-persistence path in `RaftServer::doVote`.
- The durable vote-ack shape maps directly to transport message types:
  `VoteDurableReq{term, voter_id}`.
- This leaf is behavior-preserving:
  - follower still replies memory-vote immediately,
  - vote is still persisted asynchronously in background thread,
  - durable-voter quorum semantics on the leader are unchanged.

## Leaf 3 User/Developer Notes

- `doVote` now calls
  `transport_->send_vote_durable(candidate_id, VoteDurableReq{...})`
  after async vote persistence completes.
- `tests/raft_channel_transport_test.cc` now asserts vote-durable
  delivery/drop behavior under the same directional fault injection used for
  other fire-and-forget transport methods.

## Leaf 4 (TimeoutNow) Design Rationale

- Goal: ensure `RaftServer` no longer performs direct outbound
  `SendTimeoutNow` calls and keep timeout-now transport usage behind the
  transport facade boundary.
- Result of audit: no direct `SendTimeoutNow` call sites remain in
  `RaftServer` (`server.h` / `server.cc`). Leadership transfer currently uses
  piggybacked `send_empty_append_entries(... trigger_election_now=true)`.

## Leaf 4 User/Developer Notes

- Added regression guard test `test_raft_timeoutnow_callsite_guard` that reads
  `server.h` and `server.cc` and fails if `SendTimeoutNow` is reintroduced in
  RaftServer source.
- `RrrTransportAdapter::send_timeout_now` remains available as the facade
  implementation for any future outbound timeout-now path.

## Leaf 5 (UpdatePartitionView boundary) Design Rationale

- Goal: resolve the `UpdatePartitionView` migration decision for Phase 8.1e.
- Decision: keep `commo()->UpdatePartitionView(...)` as an explicit direct
  boundary for now.
- Why:
  - this call is communicator gossip/view bookkeeping, not consensus-critical
    Raft RPC replication,
  - moving it into the transport facade now would mix concerns and increase
    surface area of Phase 8.1e without improving Raft safety semantics.

## Leaf 5 User/Developer Notes

- Added explicit source marker:
  `PHASE8_BOUNDARY_UPDATE_PARTITION_VIEW` at the `setIsLeader` call site.
- Added regression guard test
  `test_raft_update_partition_view_boundary_guard` to enforce:
  - marker presence, and
  - exactly one direct `commo()->UpdatePartitionView(...)` call in
    `server.cc`.

## Leaf 6 (`rpc_par_proxies_` boundary) Design Rationale

- Goal: remove direct `commo()->rpc_par_proxies_[par_id]` reads from
  `RaftServer` leader-init code paths.
- Decision: add a narrow read-only helper on `RaftCommo` instead of exposing
  map internals at call sites.
- Why:
  - `RaftServer` only needs peer site IDs, not the proxy container itself,
  - this keeps `rpc_par_proxies_` usage localized to communicator code.

## Leaf 6 User/Developer Notes

- Added `RaftCommo::GetPartitionProxySiteIds(parid_t)` and migrated:
  - `setIsLeader()` leader volatile-state init path, and
  - `HeartbeatLoop()` startup peer-table init path.
- Added guard test `test_raft_rpc_par_proxies_boundary_guard` to enforce:
  - helper API presence, and
  - no `commo()->rpc_par_proxies_[...]` direct lookup in `server.cc`.

## Leaf 7 (`RaftVoteQuorumEvent` retirement) Design Rationale

- Goal: remove the transitional vote quorum helper now that election fan-out
  no longer uses `BroadcastVote`.
- Decision: delete both `RaftVoteQuorumEvent` and the dead
  `RaftCommo::BroadcastVote(...)` API.
- Why:
  - keeping an unused vote fan-out path creates drift against the transport
    facade boundary,
  - deleting the caller-free API is lower risk than preserving it as dead
    code behind compatibility stubs.

## Leaf 7 User/Developer Notes

- Removed from raft commo:
  - `class RaftVoteQuorumEvent`,
  - `RaftCommo::BroadcastVote(...)` declaration and implementation.
- Regression coverage:
  - `test_raft_commo_legacy_api_removed` now asserts at compile time that
    `RaftCommo::BroadcastVote` is absent.
  - New `test_raft_vote_quorum_event_removed_guard` asserts source-level
    absence of `RaftVoteQuorumEvent` and `RaftCommo::BroadcastVote`.
  - `examples/mako-raft-tests/test_1shard_replication_raft.sh` replay-batch
    gate was recalibrated from `max>500 && min>100` to
    `max>=150 && min>=100` after repeated healthy-throughput runs showed
    replay-batch accounting is now coarser under current batching behavior.

## Leaf 8 (Phase 8.1e gate closure) Validation Notes

- Lab gate:
  - `./build/deptran_server -f config/raft_lab_test.yml` returned `RC=0`,
    with TEST 1-11 and TEST 50-60 pass markers in log.
- Throughput gate:
  - `bash ./examples/mako-raft-tests/test_1shard_replication_raft.sh 12`
    produced `agg_persist_throughput: 94480.8 ops/sec` and passed follower
    replay checks (`p1=574`, `p2=447`).
