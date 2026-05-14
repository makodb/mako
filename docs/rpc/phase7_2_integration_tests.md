# Phase 7.2 Integration Tests Design

## Overview

This document outlines integration tests for the RPC reliability components implemented in Phase 1, 3.1, and 6.1. Unlike unit tests that test components in isolation, these integration tests verify behavior when components work together with actual RPC client/server.

## Scope

Testing the following implemented components in realistic scenarios:
- Connection State Machine (Phase 1.1)
- Reconnection Policy (Phase 1.2)
- Automatic Reconnection (Phase 1.3)
- Circuit Breaker (Phase 1.4)
- Heartbeat Manager (Phase 3.1)
- Structured Error Types (Phase 6.1)

**Out of scope** (not implemented yet):
- Request Queue/Buffering (Phase 2)
- Idempotency (Phase 2.3)
- Enhanced ClientPool (Phase 5)

## Test Files

### 1. `test/rpc_state_integration_test.cc` (~200 LOC)

Tests connection state machine behavior during actual RPC operations:

```cpp
// Tests:
- StateTransitionsDuringConnect       // NEW -> CONNECTING -> CONNECTED
- StateTransitionsDuringDisconnect    // CONNECTED -> DISCONNECTING -> DISCONNECTED
- StateAfterServerShutdown           // CONNECTED -> FAILED on server close
- StateAfterConnectionError          // CONNECTING -> FAILED on connection refused
- StateQueryDuringOperations         // is_connected(), is_usable() during lifecycle
- StateCallbacksDuringRealConnect    // on_state_change fires during real connect
- CanConnectAfterDisconnected        // can_connect() returns true after disconnect
- CanConnectAfterFailed              // can_connect() returns true after failure
```

### 2. `test/rpc_reconnect_integration_test.cc` (~250 LOC)

Tests reconnection policy and auto-reconnect with real connections:

```cpp
// Tests:
- ReconnectAfterServerRestart        // Client reconnects after server comes back
- ReconnectWithBackoff               // Delay increases between attempts
- ReconnectMaxRetriesHonored         // Stops after max_retries reached
- ReconnectPolicyPresets             // Aggressive vs conservative behavior
- ManualReconnectCall                // Client::reconnect() works
- ReconnectToSameAddress             // Uses stored address for reconnection
- ReconnectResetsOnSuccess           // Calculator resets after successful connect
- ReconnectWithJitter                // Delays are not exactly deterministic
```

### 3. `test/rpc_circuit_breaker_integration_test.cc` (~200 LOC)

Tests circuit breaker behavior with real RPC failures:

```cpp
// Tests:
- CircuitOpensAfterFailures          // Circuit opens after threshold failures
- CircuitBlocksRequests              // Requests fail-fast when open
- CircuitAllowsProbeAfterTimeout     // Half-open state allows single probe
- CircuitClosesOnProbeSuccess        // Returns to closed after success
- CircuitReopensOnProbeFailure       // Goes back to open on probe failure
- CircuitWithConnectionErrors        // Connection errors count as failures
- CircuitDisabledBypassesLogic       // Disabled circuit always allows
- CircuitResetClearsState            // Manual reset returns to closed
```

### 4. `test/rpc_error_integration_test.cc` (~150 LOC)

Tests structured error types in real RPC scenarios:

```cpp
// Tests:
- ConnectionRefusedError             // Returns CONNECTION_REFUSED
- ServerNotFoundError                // Returns SERVER_NOT_FOUND on bad address
- TimeoutError                       // Returns TIMEOUT on long request
- InvalidRpcIdError                  // Returns INVALID_REQUEST
- ErrorCategoriesCorrect             // Categories match error types
- ErrorRetryableCheck                // is_retryable_error() for different errors
```

### 5. `test/rpc_combined_reliability_test.cc` (~200 LOC)

Tests multiple reliability components working together:

```cpp
// Tests:
- StateAndCircuitBreaker             // State machine + circuit breaker interaction
- ReconnectWithCircuitBreaker        // Reconnection respects circuit state
- FailureRecoveryFullCycle           // Complete failure -> recovery cycle
- GracefulDegradation                // System handles partial failures
- RapidFailureRecovery               // Fast recovery under repeated failures
```

## Total Estimated LOC

- rpc_state_integration_test.cc: ~200 LOC
- rpc_reconnect_integration_test.cc: ~250 LOC
- rpc_circuit_breaker_integration_test.cc: ~200 LOC
- rpc_error_integration_test.cc: ~150 LOC
- rpc_combined_reliability_test.cc: ~200 LOC

**Total: ~1000 LOC** (broken into 5 files for manageability)

## Implementation Notes

1. **Dynamic Port Allocation**: Use atomic counter like existing tests to avoid port conflicts
2. **Server Lifecycle**: Tests may need to start/stop servers during test
3. **Timing Dependencies**: Use reasonable timeouts (50-100ms) for test speed
4. **Thread Safety**: Follow existing patterns with rusty::Arc<PollThread>

## Test Infrastructure

Reuse existing infrastructure from `test_rpc.cc`:
- TestService class for RPC handlers
- RPCTest fixture pattern for setup/teardown
- BenchmarkService for RPC definitions

## Build Integration

Add to CMakeLists.txt:
```cmake
# RPC Integration Tests (Phase 7.2)
add_executable(test_rpc_state_integration test/rpc_state_integration_test.cc test/benchmark_service.cc)
# ... etc
```
