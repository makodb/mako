# Raft Phase 8.1b: TransportProxy on RaftServer

Date: 2026-05-02

## Scope

Phase 8.1b is intentionally small and commit-sized:

1. Add `TransportProxy transport_` to `RaftServer` and expose `transport()` accessor.
2. Initialize `transport_` in `RaftServer::Setup()` once `commo_` is available.
3. Add a focused compile-time test to lock the accessor API shape.

This is under 500 LOC and does not change outbound call sites yet.

## Design Rationale

- `RaftServer` remains wired to `RaftCommo` for now; this phase only plumbs the facade.
- `make_rrr_transport(commo(), site_id_, partition_id_)` is called during `Setup()` because by that point the server identity and communicator are already populated.
- If `commo_` is unexpectedly null, setup logs a warning and preserves old behavior (no outbound transport use yet in this phase).

## User/Developer Notes

- `RaftServer::transport()` is now available for follow-up phases 8.1c+ migration work.
- No election/replication logic has been switched to transport facade in this phase.

## Full-Suite Gate Findings

Running the full lab workload (`./build/deptran_server -f config/raft_lab_test.yml`)
exposed two pre-existing restart/snapshot invariants that had to be fixed to keep
the branch green while landing 8.1b:

1. `RaftTestConfig::Restart()` bypasses `Setup()`, so it now explicitly restores:
   - snapshot manager initialization
   - `current_config_` seeding from static config
   - apply fiber/thread startup (+ enqueue of already committed gap)
2. `RaftServer::OnRequestVote()` had a snapshot-unaware debug invariant
   (`verify(lstoff == lastLogIndex)`) that fails once `snapidx_ > 0`.
   It is replaced with the correct offset invariant:
   `verify(lastLogIndex >= snapidx_)` and `verify(lstoff + snapidx_ == lastLogIndex)`.
