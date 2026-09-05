# Phase 5.1: Enhanced ClientPool with Health Awareness

## Status: COMPLETE (2026-01-10)

## Overview

Enhance `ClientPool` to track connection health per pooled client, automatically remove unhealthy connections, and support configurable pool behavior.

## Goals

1. Add configurable pool settings (min/max connections, idle timeout, health check toggle)
2. Track connection health using existing ConnectionMetrics
3. Automatically remove unhealthy connections based on failure rates
4. Rebalance connections across healthy endpoints
5. Support idle connection cleanup

## Design

### PoolConfig Struct

```cpp
struct PoolConfig {
    int min_connections = 1;      // Minimum connections per address
    int max_connections = 4;      // Maximum connections per address
    uint64_t idle_timeout_ms = 300000;  // 5 minutes default
    bool health_check_enabled = true;   // Enable health-based removal
    double unhealthy_threshold = 0.5;   // Remove if success rate < 50%
    int min_requests_for_health = 10;   // Min requests before health check

    // Presets
    static PoolConfig defaults();
    static PoolConfig aggressive();  // More connections, shorter timeout
    static PoolConfig conservative();  // Fewer connections, longer timeout
};
```

### Health Tracking

Use existing `ConnectionMetrics` which already tracks:
- requests_sent, requests_completed, requests_failed
- success_rate_percent()
- reconnect_count

Health determination:
1. If `requests_sent < min_requests_for_health`: client is "unknown health" (keep)
2. If `success_rate_percent() < unhealthy_threshold * 100`: client is "unhealthy" (remove)
3. Otherwise: client is "healthy" (keep)

### ClientPool Changes

New private members:
```cpp
PoolConfig config_;
```

New public methods:
```cpp
// Configuration
void set_pool_config(const PoolConfig& config);
PoolConfig pool_config() const;

// Health management
size_t get_healthy_client_count(const std::string& addr);
size_t remove_unhealthy_clients(const std::string& addr);
size_t close_idle_clients(const std::string& addr, uint64_t current_time_ms);

// Pool-wide operations
size_t remove_all_unhealthy();
size_t close_all_idle(uint64_t current_time_ms);
```

### Modified get_client()

1. Before returning client, check health if enabled
2. If client is unhealthy, skip to next
3. If all clients unhealthy, remove them and recreate
4. Ensure at least min_connections exist

### Implementation Tasks

#### Task 1: Add PoolConfig (~50 LOC)
- Create `PoolConfig` struct in `client.hpp`
- Add default constructor with sensible defaults
- Add static preset methods

#### Task 2: Update ClientPool Members (~20 LOC)
- Add `config_` member
- Update constructor to accept optional config
- Add getter/setter methods

#### Task 3: Add Health Check Methods (~80 LOC)
- Implement `get_healthy_client_count()`
- Implement `remove_unhealthy_clients()`
- Implement helper `is_client_healthy()`

#### Task 4: Add Idle Cleanup Methods (~50 LOC)
- Implement `close_idle_clients()`
- Implement `close_all_idle()`

#### Task 5: Modify get_client() (~50 LOC)
- Add health-based client selection
- Respect min/max connections
- Handle idle timeout

### Total: ~250 LOC

## Files Changed

| File | Changes |
|------|---------|
| `src/srpc/rpc/client.hpp` | Add PoolConfig, update ClientPool |
| `src/srpc/rpc/client.cpp` | Implement new methods |
| `test/rpc_client_pool_test.cc` | Unit tests |
| `CMakeLists.txt` | Add test target |

## Testing Plan

### Unit Tests

1. **PoolConfigDefaults**: Default values are sensible
2. **PoolConfigPresets**: Presets work correctly
3. **GetHealthyClientCount**: Counts healthy clients correctly
4. **RemoveUnhealthyClients**: Removes clients below threshold
5. **CloseIdleClients**: Closes clients exceeding idle timeout
6. **GetClientWithHealthCheck**: Returns healthy clients
7. **MinConnectionsMaintained**: At least min_connections exist
8. **MaxConnectionsRespected**: Never exceeds max_connections
9. **HealthCheckDisabled**: Works without health checking
10. **ThreadSafety**: Concurrent access is safe

## Success Criteria

1. Pool config works with all presets
2. Unhealthy clients are automatically removed
3. Idle clients are closed after timeout
4. min/max connections are respected
5. All tests pass
6. No borrow checker violations
