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
