# Raft Phase 8.1d: AppendEntries Migration Breakdown

Date: 2026-05-02

## Why This Needs Multiple Leaves

Phase 8.1d touches the hottest replication path (`HeartbeatLoop`) plus
leadership-transfer signaling and legacy communicator wrappers. Doing all of
that in one commit would mix behavior changes, transport semantics changes, and
API deletion in a way that is hard to verify and rollback.

## Leaf Breakdown

1. **Leaf 1 (transport hardening prerequisite)**  
   Ensure the callback bridge used by append fan-out always emits a reply
   object on RPC error (`RaftCommo::SendAppendEntriesCb`), so higher layers can
   treat failures as explicit "no-progress" replies instead of waiting on
   never-filled state.

2. **Leaf 2 (heartbeat migration)**  
   Migrate `RaftServer::HeartbeatLoop()` append fan-out from
   `commo()->SendAppendEntries2(...)` to per-peer
   `transport_->send_append_entries(...)` / `send_empty_append_entries(...)`
   calls with bounded waits and preserve current next/match/spec-ack behavior.

3. **Leaf 3 (leadership-transfer migration)**  
   Replace `commo()->SendAppendEntries(... trigger_election_now=true)` in
   `InitiateLeadershipTransfer()` with the transport facade equivalent.

4. **Leaf 4 (legacy cleanup)**  
   Delete `SendAppendEntries2`, `SendAppendEntriesResults`, and
   `RaftCommo::SendAppendEntries(...)` only after no call sites remain.

## Leaf 2 Sizing Analysis (2026-05-02)

Leaf 2 is still a reasonable single-commit change because it is contained to:

- one fan-out site (`RaftServer::HeartbeatLoop` send path),
- no wire-format/schema changes,
- no commit-index algorithm rewrite (response handling stays the same), and
- one focused test expansion that validates both append transport variants.

No additional nested split is required for Leaf 2 beyond the existing 1/2/3/4
leaf partition above.

## User/Developer Notes

- Leaf 1 is intentionally behavior-preserving for successful RPCs; it only
  hardens error completion semantics.
- Full raft gates (`test_raft_*`, `raft_lab_standalone`, and
  `deptran_server -f config/raft_lab_test.yml`) remain mandatory after each
  leaf.

## Leaf 3 Implementation Note (2026-05-02)

- `RaftServer::InitiateLeadershipTransfer()` no longer calls
  `commo()->SendAppendEntries(...)` for transfer-trigger heartbeats.
- The method now:
  1. Builds `EmptyAppendEntriesReq` objects (`trigger_election_now=true`)
     for each peer while holding `mtx_`.
  2. Releases `mtx_`.
  3. Sends each request via `transport_->send_empty_append_entries(...)`.
- This preserves behavior while removing the last leadership-transfer
  dependency on the legacy append-results wrapper.
