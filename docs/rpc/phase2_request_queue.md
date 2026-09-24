# Phase 2.1: Request Queue with Persistence Option

## Overview

This phase implements an in-memory request queue for storing pending requests during connection failures. The queue provides configurable size limits, overflow strategies, and TTL-based expiration.

## Design Goals

1. **Bounded Memory**: Configurable maximum queue size to prevent unbounded growth
2. **Overflow Strategies**: Multiple strategies for handling full queue
3. **Request Expiration**: TTL-based expiration for stale requests
4. **Thread-Safe**: Safe for concurrent enqueue/dequeue operations
5. **RustyCpp Safe**: Use std::mutex for thread safety, proper annotations

## Data Structures

### QueuedRequest

```cpp
struct QueuedRequest {
    i64 xid;                          // Request transaction ID
    i32 rpc_id;                       // RPC method ID
    std::chrono::steady_clock::time_point timestamp;  // When queued
    uint32_t retry_count;             // Number of retries
    Marshal payload;                  // Serialized request data
    std::function<void(int)> callback; // Completion callback
};
```

### RequestQueueConfig

```cpp
struct RequestQueueConfig {
    size_t max_size = 1000;           // Maximum queue entries
    uint32_t default_ttl_ms = 30000;  // 30 second default TTL
    OverflowStrategy overflow_strategy = OverflowStrategy::DROP_OLDEST;
    bool enabled = true;
};
```

### OverflowStrategy Enum

```cpp
enum class OverflowStrategy {
    DROP_OLDEST,   // Remove oldest request to make room
    DROP_NEWEST,   // Reject new request if queue full
    BLOCK,         // Block until space available (not implemented initially)
    FAIL_FAST      // Immediately fail the request
};
```

## API Design

### RequestQueue Class

```cpp
class RequestQueue {
public:
    RequestQueue(RequestQueueConfig config = {});

    // Enqueue a request (returns false if rejected based on overflow strategy)
    bool enqueue(QueuedRequest request);

    // Dequeue the next request (returns None if empty)
    rusty::Option<QueuedRequest> dequeue();

    // Peek at the next request without removing
    rusty::Option<const QueuedRequest*> peek() const;

    // Remove expired requests, return count removed
    size_t expire_stale();

    // Get current queue size
    size_t size() const;

    // Check if queue is empty
    bool empty() const;

    // Check if queue is full
    bool full() const;

    // Clear all requests, invoke callbacks with error code
    void clear_all(int error_code);

    // Get configuration
    const RequestQueueConfig& config() const;

private:
    RequestQueueConfig config_;
    std::deque<QueuedRequest> queue_;
    mutable std::mutex mutex_;
};
```

## Implementation Steps

1. Create `src/srpc/rpc/request_queue.hpp` with:
   - QueuedRequest struct
   - RequestQueueConfig struct
   - OverflowStrategy enum
   - RequestQueue class

2. Implement queue operations:
   - Thread-safe enqueue with overflow handling
   - Thread-safe dequeue
   - TTL-based expiration

## Thread Safety

- All public methods protected by std::mutex
- Callbacks invoked outside lock to prevent deadlocks
- Copy data before releasing lock when needed

## Test Plan

Unit tests (`test/rpc_request_queue_test.cc`):
1. Basic enqueue/dequeue
2. Queue size limits
3. Overflow strategies (DROP_OLDEST, DROP_NEWEST, FAIL_FAST)
4. TTL expiration
5. Thread-safe concurrent access
6. Clear with callbacks
7. Empty/full state checks

## File Changes

| File | Change |
|------|--------|
| `src/srpc/rpc/request_queue.hpp` | New file: RequestQueue class |
| `test/rpc_request_queue_test.cc` | New test file |
| `CMakeLists.txt` | Add test target |

## Estimated Size

~200-250 LOC for request_queue.hpp
~200-250 LOC for tests
