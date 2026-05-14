# Phase 7.1: Unit Tests Plan

## Overview

Unit tests for all implemented RPC reliability components to ensure correctness and thread safety.

## Test Files

### 7.1.1 Connection State Machine Tests (`test/rpc_connection_state_test.cc`)

Tests for `ConnectionStateMachine` class:
- Valid state transitions (NEW→CONNECTING→CONNECTED→DISCONNECTED)
- Invalid state transitions (CONNECTED→NEW should fail)
- State query methods (is_connected, is_terminal, can_connect)
- State change callbacks
- Thread-safe state access with rusty::Cell

### 7.1.2 Reconnection Policy Tests (`test/rpc_reconnect_policy_test.cc`)

Tests for `ReconnectPolicy` and `ReconnectCalculator`:
- Default policy values
- Preset policies (aggressive, conservative, no_retry)
- Exponential backoff calculation
- Max delay enforcement
- Max retries behavior
- Jitter behavior (randomized delays)
- Reset after success

### 7.1.3 Circuit Breaker Tests (`test/rpc_circuit_breaker_test.cc`)

Tests for `CircuitBreaker` class:
- Initial state (CLOSED)
- Transition to OPEN after threshold failures
- HALF_OPEN state after recovery timeout
- Reset to CLOSED on success in HALF_OPEN
- Return to OPEN on failure in HALF_OPEN
- Preset configurations (aggressive, conservative, disabled)

### 7.1.6 Heartbeat Tests (`test/rpc_heartbeat_test.cc`)

Tests for `HeartbeatManager` class:
- Configuration presets (aggressive, relaxed, disabled)
- Heartbeat interval timing
- Pending pong tracking
- Timeout detection after missed pongs
- Timeout callback invocation
- Reset after reconnection

### 7.1.7 Error Handling Tests (`test/rpc_errors_test.cc`)

Tests for RPC error types:
- Error category classification
- Error code to string conversion
- Error category to string conversion
- is_retryable_error() classification
- is_connection_error() and is_timeout_error() helpers

## RustyCpp Compliance

All test code will be annotated with @safe/@unsafe as appropriate:
- Test functions themselves can be @unsafe (they're tests)
- But they verify the @safe behavior of the tested components

## Estimated LOC

- Connection State Tests: ~150 LOC
- Reconnection Policy Tests: ~150 LOC
- Circuit Breaker Tests: ~150 LOC
- Heartbeat Tests: ~150 LOC
- Error Handling Tests: ~100 LOC

Total: ~700 LOC
