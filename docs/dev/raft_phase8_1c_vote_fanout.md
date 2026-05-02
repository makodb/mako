# Raft Phase 8.1c (Leaf 1): BroadcastVote -> transport send_vote fan-out

Date: 2026-05-02

## Scope

This leaf migrates the election vote fan-out in `RaftServer::RequestVote()` from
`RaftCommo::BroadcastVote()` / `RaftVoteQuorumEvent` to:

1. Per-peer `transport_->send_vote(peer, VoteReq{...})` calls in sub-fibers.
2. `RaftQuorum<VoteReply>` for quorum wait + reply collection.
3. Local yes/no/highest-term aggregation from collected `VoteReply` values.
4. `specVoters_` population from `vote_granted == true` replies.

The cleanup leaf (deleting legacy helper branches and event wrappers) remains
for later 8.1c/8.1e tasks.

## Design Rationale

- Keep election path semantics intact while removing direct dependence on
  `RaftVoteQuorumEvent` in `RequestVote()`.
- Preserve old branch structure:
  - win (majority yes),
  - explicit rejection (legacy no-threshold rule),
  - timeout.
- Preserve speculative-leader initialization logic (`specVoters_`,
  `durableVoters_`, `securedLeader_`, index resets).

## Stability Fixes Required by Full-Lab Gate

During full `deptran_server -f config/raft_lab_test.yml` verification,
`TEST 63` exposed that per-peer vote fibers could outlive useful election work
when a peer RPC errored and no reply signal was produced.

Fixes applied:

1. `RrrTransportAdapter::send_vote()` now waits with a bounded 1s timeout.
2. `RaftCommo::BroadcastVoteCb()` now invokes callback with a default
   `VoteReply{}` on RPC error.

Together these guarantee vote sub-fibers terminate under restart/partition
conditions instead of hanging indefinitely.

## User/Developer Notes

- `RequestVote()` now uses `TransportProxy` for outbound vote RPCs.
- Legacy `RaftVoteQuorumEvent` still exists and will be deleted in a later leaf.
- New/updated tests:
  - `tests/raft_quorum_test.cc` now includes vote-reply aggregation cases
    matching election yes/no/highest-term behavior.
