# Phase 3.2: Connection Health Metrics

## Status: COMPLETED (2026-01-09)

## Overview

Implement connection health metrics tracking to provide visibility into connection behavior, performance, and reliability. This enables monitoring, debugging, and adaptive behavior based on connection health.

## Goals

1. Track request metrics (sent, completed, failed counts)
2. Track data transfer metrics (bytes sent/received)
3. Track connection lifecycle metrics (reconnect count, uptime)
4. Track latency metrics (average, min, max RTT)
5. Provide thread-safe accessors for all metrics

## Design

### ConnectionMetrics Class

```cpp
struct ConnectionMetrics {
    // Request counters (atomic for thread-safety)
    rusty::Cell<uint64_t> requests_sent{0};
    rusty::Cell<uint64_t> requests_completed{0};
    rusty::Cell<uint64_t> requests_failed{0};
    rusty::Cell<uint64_t> requests_timed_out{0};

    // Data transfer counters
    rusty::Cell<uint64_t> bytes_sent{0};
    rusty::Cell<uint64_t> bytes_received{0};

    // Connection lifecycle
    rusty::Cell<uint64_t> reconnect_count{0};
    rusty::Cell<uint64_t> connect_time_ms{0};  // When connected

    // Latency tracking (in microseconds)
    rusty::Cell<uint64_t> total_latency_us{0};  // Sum for averaging
    rusty::Cell<uint64_t> min_latency_us{UINT64_MAX};
    rusty::Cell<uint64_t> max_latency_us{0};

    // Accessors
    uint64_t success_rate_percent() const;
    uint64_t avg_latency_us() const;
    uint64_t uptime_ms() const;

    // Mutators
    void record_request_sent();
    void record_request_completed(uint64_t latency_us);
    void record_request_failed();
    void record_request_timeout();
    void record_bytes_sent(uint64_t bytes);
    void record_bytes_received(uint64_t bytes);
    void record_reconnect();
    void record_connect();

    // Reset
    void reset();
};
```

### Integration Points

1. **ClientConnection::request()**: Increment requests_sent
2. **ClientConnection::handle_read()**: Increment bytes_received, requests_completed
3. **ClientConnection::handle_write()**: Increment bytes_sent
4. **Future timeout/error**: Increment requests_failed or requests_timed_out
5. **ClientConnection::connect()**: Record connect_time_ms
6. **ClientConnection::reconnect()**: Increment reconnect_count

### Implementation Tasks

#### Task 1: Create ConnectionMetrics Class (~80 LOC)
- Create `src/srpc/rpc/connection_metrics.hpp`
- Add all metric fields using rusty::Cell for thread-safety
- Implement accessor methods
- Implement mutator methods
- Add reset() method

#### Task 2: Integrate with ClientConnection (~40 LOC)
- Add `ConnectionMetrics metrics_` member to ClientConnection
- Add `metrics()` accessor returning const reference
- Update request() to call record_request_sent()
- Update handle_read() to call record_bytes_received()
- Update handle_write() to call record_bytes_sent()
- Update connect() to call record_connect()
- Update reconnect() to call record_reconnect()

#### Task 3: Add Client Wrapper (~20 LOC)
- Add `metrics()` method to Client class
- Delegate to ClientConnection::metrics()

### Total: ~140 LOC

## Files Changed

| File | Changes |
|------|---------|
| `src/srpc/rpc/connection_metrics.hpp` | New file - ConnectionMetrics class |
| `src/srpc/rpc/client.hpp` | Add metrics_ member, accessor |
| `src/srpc/rpc/client.cpp` | Update methods to record metrics |
| `test/rpc_metrics_test.cc` | Unit tests |
| `CMakeLists.txt` | Add test target |

## Testing Plan

### Unit Tests

1. **InitialValuesZero**: All counters start at zero
2. **RequestSentIncrement**: record_request_sent increments counter
3. **RequestCompletedWithLatency**: Tracks latency correctly
4. **RequestFailedIncrement**: record_request_failed increments counter
5. **ByteCountersAccumulate**: bytes_sent/received accumulate
6. **ReconnectCountIncrement**: record_reconnect increments counter
7. **SuccessRateCalculation**: success_rate_percent() computes correctly
8. **AverageLatencyCalculation**: avg_latency_us() computes correctly
9. **MinMaxLatencyTracking**: Min/max latency updated correctly
10. **Reset**: reset() clears all counters
11. **ThreadSafety**: Concurrent updates are safe

### Integration Tests

1. **MetricsWithRealConnection**: Verify metrics update during actual RPC
2. **MetricsAfterReconnect**: Verify reconnect_count increments

## Success Criteria

1. All metrics track correctly in unit tests
2. Thread-safe concurrent access
3. Integration with existing connection code
4. No performance regression
5. All tests pass
