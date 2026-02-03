# Phase 7.4: Chaos Engineering Tests

## Overview
Create a chaos testing framework for the RPC reliability layer that can inject various failure modes and verify system recovery.

## Components

### 1. ChaosConfig
Configuration for chaos injection:
- `failure_rate`: Probability of failure per operation (0.0 - 1.0)
- `failure_types`: Which failure types to inject
- `duration_ms`: How long to run chaos
- `verification_timeout_ms`: Time to wait for recovery verification

### 2. FailureType Enum
- `SERVER_KILL`: Kill and optionally restart server
- `LATENCY_INJECTION`: Add artificial delay to operations
- `CONNECTION_RESET`: Force disconnect clients
- `PACKET_LOSS`: Simulate lost messages by timing out requests
- `COMBINED`: Random combination of above

### 3. ChaosController
Thread-safe controller for chaos injection:
- `start()`: Begin chaos injection
- `stop()`: Stop chaos injection
- `inject_failure()`: Manually inject a specific failure
- `get_stats()`: Get injection statistics

### 4. ChaosVerifier
Verifies system state after chaos:
- `verify_connectivity()`: Check clients can reconnect
- `verify_requests_work()`: Check requests complete successfully
- `verify_no_data_corruption()`: Check data integrity
- `get_recovery_time_ms()`: Measure time to recover

### 5. ChaosResult
Results from a chaos test run:
- `failures_injected`: Count by type
- `recovery_time_ms`: Time to recover
- `requests_during_chaos`: Stats during chaos
- `requests_after_recovery`: Stats after recovery
- `passed`: Whether verification passed

## Test Scenarios

1. **RandomServerKills**: Server randomly killed and restarted
2. **LatencySpikes**: Random latency added to requests
3. **ConnectionChurn**: Clients randomly disconnected
4. **CombinedChaos**: Multiple failure types simultaneously
5. **RecoveryVerification**: Verify system recovers correctly

## Implementation Notes

- Uses atomic flags for thread-safe chaos control
- Random number generation with configurable seed for reproducibility
- Integrates with existing stress test infrastructure
- Labeled as "chaos" tests for selective CI execution
- ~250 LOC for framework, ~300 LOC for tests
