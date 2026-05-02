# Raft Phase 8.6 Breakdown (Port RaftTestConfig to TestCluster)

## Why this breakdown exists

Phase 8.6 spans multiple concerns in one TODO bullet:

1. API expansion (`RaftTestConfig(TestCluster&)`)
2. behavioral routing of many test-control operations
3. compatibility with existing frame-based lab flow
4. gating via RaftLab subset execution

This is too large for one safe commit, so split into smaller leaves.

## Decomposed leaves

### 8.6.a Dual-backend RaftTestConfig scaffold + core cluster ops (completed 26:05:02, 15:37)

- Add `RaftTestConfig(TestCluster&)` alongside existing frame-based constructor.
- Keep existing frame-based constructor and behavior intact.
- Add backend routing for core methods used by early lab scenarios:
  - `SetLearnerAction`, `OneLeader`, `NoLeader`, `TermMovedOn`, `OneTerm`
  - `Start`, `Wait`, `DoAgreement`, `NCommitted`
  - `Disconnect`, `Reconnect`, `NDisconnected`, `Shutdown`
  - `GetServer`, ID mapping helpers.
- For cluster mode `SetUnreliable`/RPC counters are bookkeeping-only (no random
  fault-injection control loop yet).
- Add focused tests validating constructor + core behavior on `TestCluster`.

### 8.6.b Kill/Restart parity and reconnect semantics hardening (completed 26:05:02, 15:43)

- Ensure cluster backend `Kill`/`Restart` semantics match frame backend
  expectations in relevant lab tests.
- Remove any cluster-mode reconnect corner cases that require full-fault reset
  as a blunt instrument.
- Implemented by re-applying disconnect-intended faults after reset-based
  `Reconnect`/`Restart` paths, and by guarding cluster `Kill`/`Restart` state
  transitions with `disconnect_mtx_`.
- Covered by:
  `ReconnectPreservesOtherDisconnectsInClusterBackend` and
  `KillRestartPreservesOtherDisconnectsInClusterBackend`.

### 8.6.c ChannelSwitchboard undrop primitive (completed 26:05:02, 15:59)

- Add `undrop_direction(from, to)` (or equivalent) to avoid reset-all behavior
  when reconnecting one server while preserving unrelated injected faults.
- Wire cluster backend `Reconnect` to targeted undrop calls.
- Implemented by adding `ChannelSwitchboard::undrop_direction` and wiring
  `TestCluster::reconnect/restart` and cluster-mode `RaftTestConfig::Reconnect`
  through targeted undrop calls.
- Covered by:
  `UndropDirectionRestoresOneDirectionOnly` and
  `ReconnectPreservesPartitionFaultsInClusterBackend`.

### 8.6.d Lab subset gate with TestCluster-backed RaftTestConfig

- Add/enable a subset runner against the new constructor:
  `testInitialElection`, `testReElection`, `testBasicAgree`, `testFailAgree`.
- Keep existing frame-based path operational.

## Notes

- Phase 8.6.a intentionally avoids jumping straight to full `RaftLabTest::Run()`.
  Full-driver work belongs to Phase 8.7.
