# Phase 1.4: Circuit Breaker Pattern Plan

> Current status (2026-08-12): implemented by canonical
> `src/srpc/src/circuit_breaker.rs`; rusty-cpp generates the
> `srpc.circuit_breaker` C++ module. The original header/file sketches below
> are historical design notes. The production state is Cell-backed, Send,
> and deliberately not Sync; callers must serialize mutation.

## Overview

Implement circuit breaker pattern to prevent cascading failures by failing fast when a service is unhealthy.

## Design

### Circuit Breaker States

```
     +--------+
     | CLOSED | <-----------+
     +--------+             |
          |                 |
          | failure_threshold exceeded
          v                 |
     +--------+             |
     |  OPEN  |------------>+ (timeout expires: try one request)
     +--------+             |
          |                 |
          | timeout expires |
          v                 |
   +-----------+            |
   | HALF_OPEN |------------+
   +-----------+   success_threshold reached
          |
          | failure (any)
          v
     +--------+
     |  OPEN  |
     +--------+
```

### CircuitBreakerConfig

```cpp
struct CircuitBreakerConfig {
    uint32_t failure_threshold;     // Failures before opening (default: 5)
    uint32_t success_threshold;     // Successes to close from half-open (default: 3)
    uint32_t timeout_ms;            // Time in OPEN before trying (default: 30000)
    bool enabled;                   // Enable/disable circuit breaker (default: true)
};
```

### CircuitBreaker Class

```cpp
class CircuitBreaker {
private:
    CircuitBreakerConfig config_;
    rusty::Cell<CircuitState> state_;
    rusty::Cell<uint32_t> failure_count_;
    rusty::Cell<uint32_t> success_count_;
    rusty::Cell<uint64_t> last_failure_time_;

public:
    // Check if request should be allowed
    bool allow_request() const;

    // Record success
    void record_success();

    // Record failure
    void record_failure();

    // Get current state
    CircuitState state() const;

    // Reset to closed state
    void reset();
};
```

### Integration with ClientConnection

The circuit breaker is checked before making requests:
1. In `request()`: check `circuit_breaker_.allow_request()`
2. If OPEN: return error immediately (fail-fast)
3. On response: call `record_success()` or `record_failure()`

## Implementation Details

### Mutation model

- Uses `rusty::Cell<T>` for all mutable state
- State transitions are single-threaded; `Cell` is not atomic or Sync
- Time-based checks use the terminal plain-C monotonic-clock kernel

### Timeout Handling

- OPEN state tracks `last_failure_time_`
- After `timeout_ms`, transition to HALF_OPEN
- HALF_OPEN allows one probe request

### RustyCpp Compliance

- All functions annotated @safe
- Uses rusty::Cell for interior mutability
- No raw pointers or mutable fields

## File Structure

Canonical source: `src/srpc/src/circuit_breaker.rs`; generated provider:
`srpc.circuit_breaker`.

## Estimated LOC

~150-200 lines
