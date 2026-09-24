# Phase 2.4: Request Timeout and Retry Logic

## Overview

This phase adds configurable timeout handling and automatic retry support for RPC requests. It builds on Phase 1.2 (ReconnectPolicy) for backoff calculations and enhances the request mechanism to handle transient failures gracefully.

## Design Goals

1. **Timeout Types**: Distinguish between connect, request, and response timeouts
2. **Automatic Retry**: Support configurable retry with exponential backoff
3. **Idempotency Awareness**: Only retry requests marked as idempotent
4. **Non-Blocking**: Retry logic runs asynchronously via coroutines
5. **RustyCpp Safe**: Use rusty types and proper annotations

## Data Structures

### TimeoutType Enum

```cpp
enum class TimeoutType {
    NONE = 0,           // No timeout occurred
    CONNECT_TIMEOUT,    // Failed to establish connection
    REQUEST_TIMEOUT,    // Request send timed out
    RESPONSE_TIMEOUT,   // Waiting for response timed out
    TOTAL_TIMEOUT       // Overall operation timeout exceeded
};
```

The production ABI uses the default signed 32-bit enum backing.

### RequestOptions Struct

```cpp
struct RequestOptions {
    // Timeout configuration
    uint64_t timeout_ms;                   // Per-attempt timeout
    uint64_t total_timeout_ms;             // Total operation timeout (0 = no limit)

    // Retry configuration
    uint16_t max_retries;                  // Max retry attempts (0 = no retry)
    uint16_t base_delay_ms;                // Base delay for exponential backoff
    uint16_t max_delay_ms;                 // Maximum delay between retries
    float jitter_factor;                   // Jitter factor for backoff

    // Idempotency
    bool idempotent;                       // If true, safe to retry on timeout

    // Presets
    static RequestOptions new_();          // 1s timeout, no retry
    static RequestOptions defaults();      // Same as new_()
    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms);
    static RequestOptions idempotent_retry(uint16_t max_retries);
    static RequestOptions no_timeout();   // Infinite wait
};
```

## Integration Points

### Future Enhancements

Add to the Future class:
- `options_`: Storage for request options
- `timeout_type_`: Type of timeout that occurred
- `retry_count_`: Number of retries attempted

```cpp
class Future {
private:
    // ... existing members ...

    // New members for retry support
    rusty::Cell<RequestOptions> options_;
    rusty::Cell<TimeoutType> timeout_type_{TimeoutType::NONE};
    rusty::Cell<uint16_t> retry_count_{0};

public:
    // @safe - Get timeout type
    TimeoutType timeout_type() const { return timeout_type_.get(); }

    // @safe - Get retry count
    uint16_t retry_count() const { return retry_count_.get(); }

    // @safe - Check if should retry
    bool should_retry() const;

    // @safe - Set options (called during request creation)
    void set_options(const RequestOptions& opts) { options_.set(opts); }
};
```

### Client/ClientConnection Methods

```cpp
// New method for retry-capable requests
template<typename F>
FutureResult request_with_options(
    i32 rpc_id,
    const RequestOptions& options,
    const FutureAttr& attr,
    F&& write_fn) const;

// Helper to wait with automatic retry
// Uses coroutines for non-blocking retry
void wait_with_retry(
    rusty::Arc<Future> future,
    std::function<FutureResult()> retry_fn,
    std::function<void(rusty::Arc<Future>)> on_complete = nullptr) const;
```

## Retry Flow

```
request_with_options() called:
  ├─ Create Future with options
  ├─ Serialize request payload (for potential retry)
  ├─ Send request
  └─ Return Future

wait_with_retry() called:
  ├─ Wait with timeout (options.timeout_ms)
  ├─ If success: return
  ├─ If timeout:
  │    ├─ If NOT idempotent: set error, return
  │    ├─ If retry_count >= max_retries: set error, return
  │    ├─ Calculate backoff delay
  │    ├─ Sleep (via coroutine yield)
  │    ├─ Increment retry_count
  │    ├─ Call retry_fn() to re-send
  │    └─ Loop back to wait
  └─ If total_timeout exceeded: set error, return
```

## Implementation Details

### Backoff Calculation

Reuse ReconnectCalculator from Phase 1.2:

```cpp
double RequestOptions::calculate_delay(uint16_t attempt) const {
    // Exponential backoff: base_delay * 2^attempt
    double delay = base_delay_ms * std::pow(2.0, attempt);

    // Cap at max_delay
    delay = std::min(delay, static_cast<double>(max_delay_ms));

    // Add jitter
    double jitter = delay * jitter_factor * (rand_double() - 0.5);
    return delay + jitter;
}
```

### Retry Lambda Creation

```cpp
template<typename F>
FutureResult ClientConnection::request_with_options(
    i32 rpc_id, const RequestOptions& options,
    const FutureAttr& attr, F&& write_fn) const {

    // Create serialized payload for potential retry
    auto payload = std::make_shared<Marshal>();
    i64 xid = xid_counter_.next();
    *payload << v64(xid);
    *payload << rpc_id;
    write_fn(*payload);

    // Create future with options
    auto fu = Future::create(xid, attr);
    fu->set_options(options);

    // Send initial request
    auto result = send_serialized_request(xid, payload, fu);
    if (result.is_err()) {
        return result;
    }

    // Store payload for retry (if idempotent)
    if (options.idempotent && options.max_retries > 0) {
        pending_fu_.set(xid, fu);  // Store for callback
        retry_payloads_.set(xid, payload);  // Store for retry
    }

    return FutureResult::Ok(fu);
}
```

### Wait With Retry Implementation

```cpp
void ClientConnection::wait_with_retry(
    rusty::Arc<Future> future,
    std::function<FutureResult()> retry_fn,
    std::function<void(rusty::Arc<Future>)> on_complete) const {

    auto options = future->options();
    auto start_time = current_time_ms();

    while (true) {
        // Wait with per-attempt timeout
        future->timed_wait(options.timeout_ms / 1000.0);

        if (future->ready() && !future->timed_out()) {
            // Success
            if (on_complete) on_complete(future);
            return;
        }

        // Check if we should retry
        if (!future->should_retry()) {
            future->set_timeout_type(TimeoutType::RESPONSE_TIMEOUT);
            if (on_complete) on_complete(future);
            return;
        }

        // Check total timeout
        if (options.total_timeout_ms > 0) {
            auto elapsed = current_time_ms() - start_time;
            if (elapsed >= options.total_timeout_ms) {
                future->set_timeout_type(TimeoutType::TOTAL_TIMEOUT);
                if (on_complete) on_complete(future);
                return;
            }
        }

        // Calculate and apply backoff
        auto retry_count = future->increment_retry_count();
        auto delay = options.calculate_delay(retry_count);

        // Yield to coroutine scheduler for delay
        Coroutine::sleep(delay / 1000.0);

        // Re-send request
        auto result = retry_fn();
        if (result.is_err()) {
            future->set_error_code(result.err());
            if (on_complete) on_complete(future);
            return;
        }
    }
}
```

## Thread Safety

- `options_`, `timeout_type_`, `retry_count_` use rusty::Cell (atomic for Copy types)
- Retry payloads stored in thread-safe map
- Backoff delay uses Coroutine::sleep for cooperative yielding

## Test Plan

Unit tests (`test/rpc_timeout_retry_test.cc`):
1. Default RequestOptions values
2. RequestOptions presets (with_retry, idempotent_retry, no_timeout)
3. TimeoutType correctly set on timeout
4. No retry when idempotent=false
5. No retry when max_retries=0
6. Retry up to max_retries on timeout
7. Backoff delay increases exponentially
8. Total timeout stops retries
9. Successful retry recovers from transient failure
10. Retry count tracking
11. Thread-safe concurrent retries

Integration tests:
1. Server restart during request → retry succeeds
2. Network timeout → retry with backoff
3. Multiple concurrent retryable requests

## File Changes

| File | Change |
|------|--------|
| `src/srpc/src/request_options.rs` | Canonical Rust source for TimeoutType and RequestOptions; rusty-cpp generates the production C++ module |
| `src/srpc/rpc/client.hpp` | Add Future members, request_with_options (~50 LOC) |
| `src/srpc/rpc/client.cpp` | Implement retry logic (~50 LOC) |
| `test/rpc_timeout_retry_test.cc` | New test file (~200 LOC) |

## Estimated Size

~150-200 LOC total (header + implementation)
~200 LOC for tests

## Dependencies

- Phase 1.2: ReconnectPolicy (for backoff calculation patterns)
- Phase 2.3: Idempotency Support (optional - idempotency key not required, just the flag)

## Notes

- The idempotency key (from Phase 2.3) is optional. This phase uses a simple boolean flag.
- Server-side idempotency caching is not implemented in this phase.
- Retry only on timeout; other errors (connection refused, etc.) use circuit breaker.
