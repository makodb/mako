# Phase 3.3: Proactive Connection Validation

## Status: COMPLETED (2026-01-09)

## Overview

Implement proactive connection validation to detect and handle stale or half-open connections before they cause RPC failures. This includes TCP keepalive configuration and idle connection timeout management.

## Goals

1. Configure TCP keepalive to detect dead connections at the OS level
2. Implement idle timeout to close unused connections
3. Add `validate_connection()` method to actively check connection health
4. Integrate with HeartbeatManager from Phase 3.1

## Design

### TCP Keepalive Configuration

Configure OS-level TCP keepalive to detect dead connections:

```cpp
struct KeepaliveConfig {
    bool enabled = true;
    int idle_sec = 60;      // TCP_KEEPIDLE: seconds before first probe
    int interval_sec = 10;  // TCP_KEEPINTVL: seconds between probes
    int count = 5;          // TCP_KEEPCNT: max probes before dropping

    static KeepaliveConfig aggressive() {
        return {true, 10, 2, 3};  // Fast detection: 10s idle, 2s interval, 3 probes = 16s total
    }

    static KeepaliveConfig relaxed() {
        return {true, 60, 10, 5};  // Standard: 60s idle, 10s interval, 5 probes = 110s total
    }

    static KeepaliveConfig disabled() {
        return {false, 0, 0, 0};
    }
};
```

### Idle Timeout

Track last activity time and close connections that have been idle too long:

```cpp
// In ClientConnection
rusty::Cell<uint64_t> last_activity_time_{0};  // Timestamp of last send/receive

void update_last_activity();  // Called on send/receive
bool is_idle(uint64_t idle_timeout_ms) const;  // Check if connection is idle
```

### Connection Validation Method

Add method to actively validate connection health:

```cpp
// In ClientConnection
/**
 * Validate the connection is still alive.
 * Returns true if connection is healthy, false if it needs reconnection.
 *
 * Validation checks:
 * 1. Connection state is CONNECTED
 * 2. Socket is still valid (getsockopt check)
 * 3. Not idle beyond threshold (if configured)
 */
bool validate_connection() const;
```

### Implementation Tasks

#### Task 1: Add Keepalive Configuration (~40 LOC)
- Add `KeepaliveConfig` struct with presets
- Add `set_keepalive()` method to ClientConnection
- Apply keepalive options in `connect()` after socket creation

#### Task 2: Add Idle Tracking (~30 LOC)
- Add `last_activity_time_` member to ClientConnection
- Update timestamp in `send()` and `receive()` paths
- Add `is_idle()` method

#### Task 3: Add Connection Validation (~30 LOC)
- Add `validate_connection()` method
- Check connection state, socket validity, and idle status
- Add error code validation via getsockopt(SO_ERROR)

#### Task 4: Integration with Client (~20 LOC)
- Add wrapper methods to Client class
- Add default keepalive configuration

### Total: ~120 LOC

## Files Changed

| File | Changes |
|------|---------|
| `src/srpc/rpc/client.hpp` | Add KeepaliveConfig, idle tracking, validate_connection() |
| `src/srpc/rpc/client.cpp` | Implement keepalive setup, validation logic |
| `test/rpc_validation_test.cc` | Unit tests |
| `CMakeLists.txt` | Add test target |

## Testing Plan

### Unit Tests

1. **KeepaliveConfigPresets**: Test aggressive/relaxed/disabled presets
2. **SetKeepaliveOptions**: Verify setsockopt is called correctly
3. **IdleTimeTracking**: Verify last_activity_time updates
4. **IsIdleCheck**: Test idle detection logic
5. **ValidateConnectedState**: Test validation in CONNECTED state
6. **ValidateDisconnectedState**: Test validation in other states
7. **ValidateSocketError**: Test validation when socket has error

## Success Criteria

1. TCP keepalive can be configured per connection
2. Idle connections are detected correctly
3. validate_connection() accurately reports connection health
4. All tests pass
5. No regression in existing functionality
