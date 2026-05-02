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
