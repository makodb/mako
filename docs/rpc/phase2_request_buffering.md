# Phase 2.2: Request Buffering During Disconnection

## Overview

This phase extends ClientConnection to buffer RPC requests during temporary disconnections, enabling transparent failover without losing in-flight requests.

## Design Goals

1. **Transparent Buffering**: Queue requests when connection is unavailable, replay on reconnect
2. **Configurable Behavior**: Support QUEUE, FAIL_FAST, and BLOCK modes
3. **Memory Bounded**: Use RequestQueue from Phase 2.1 with size limits
4. **Thread-Safe**: Safe concurrent access to pending queue
5. **RustyCpp Safe**: Use rusty types and proper annotations

## Data Structures

### DisconnectBehavior Enum

```cpp
enum class DisconnectBehavior {
    QUEUE,      // Queue requests for later replay (default)
    FAIL_FAST,  // Immediately fail with ENOTCONN
    BLOCK       // Block until reconnected (future, not implemented)
};
```

### BufferingConfig

```cpp
struct BufferingConfig {
    DisconnectBehavior behavior = DisconnectBehavior::QUEUE;
    size_t max_pending = 1000;    // Max queued requests
    uint32_t default_ttl_ms = 30000;  // 30 second TTL
    OverflowStrategy overflow = OverflowStrategy::DROP_OLDEST;
    bool enabled = true;
};
```

## Integration Points

### ClientConnection Changes

1. Add `pending_queue_` using RequestQueue from Phase 2.1
2. Add `buffering_config_` for behavior configuration
3. Modify `request()` to queue when disconnected (if enabled)
4. Add `replay_pending_requests()` method
5. Modify `reconnect()` to call replay after success

### Request Flow

```
request() called:
  ├─ If CONNECTED: send immediately (current behavior)
  └─ If DISCONNECTED:
       ├─ If behavior == FAIL_FAST: return Err(ENOTCONN)
       ├─ If behavior == QUEUE: queue request, return Ok(Future)
       └─ If behavior == BLOCK: block until reconnected (future)

reconnect() success:
  └─ Call replay_pending_requests() to send queued requests
```

## API Design

### New Methods on ClientConnection

```cpp
// Set buffering configuration
void set_buffering_config(const BufferingConfig& config);

// Get current buffering config
const BufferingConfig& buffering_config() const;

// Get number of pending (queued) requests
size_t pending_request_count() const;

// Clear all pending requests (calls callbacks with error)
void clear_pending_requests(int error_code = ECONNABORTED);

// Internal: replay queued requests after reconnection
private:
size_t replay_pending_requests();
```

### Modified Methods

```cpp
// request() - Now handles buffering when disconnected
template<typename F>
FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const;

// reconnect() - Now replays pending requests on success
int reconnect(std::function<void(bool)> on_complete = nullptr);
```

## Implementation Details

### QueuedRequest Marshaling

Since `request()` receives a write function, we need to capture the serialized data:

```cpp
// In request() when buffering:
QueuedRequest queued;
queued.xid = xid_counter_.next();
queued.rpc_id = rpc_id;
queued.payload = std::make_shared<Marshal>();

// Serialize to the payload
*queued.payload << v64(queued.xid);
*queued.payload << rpc_id;
write_fn(*queued.payload);  // User writes arguments

// Create future and store in pending_fu_
auto fu = Future::create(queued.xid, attr);
queued.callback = [this, fu](int err) {
    if (err < 0) {
        fu->error_code_.set(err);
        fu->notify_ready(fu);
    }
};

pending_queue_.enqueue(std::move(queued));
return FutureResult::Ok(fu);
```

### Replay Logic

```cpp
size_t ClientConnection::replay_pending_requests() {
    size_t replayed = 0;
    while (true) {
        auto req_opt = pending_queue_.dequeue();
        if (req_opt.is_none()) break;

        auto req = req_opt.unwrap();

        // Check if expired
        if (req.is_expired()) {
            if (req.callback) req.callback(-2);  // Expired
            continue;
        }

        // Re-send the pre-serialized payload
        auto guard = out_.lock().unwrap();

        // Write size header
        i32 size = static_cast<i32>(req.payload->content_size());
        Marshal::bookmark bmark = guard->set_bookmark(sizeof(i32));

        // Copy payload to output
        guard->read_from_marshal(*req.payload, req.payload->content_size());
        guard->write_bookmark(bmark, size);

        replayed++;
    }

    // Trigger write
    poll_thread_worker_->update_mode(*this, PollMode::READ | PollMode::WRITE);

    return replayed;
}
```

## Thread Safety

- `pending_queue_` is thread-safe (uses internal std::mutex)
- Access to `buffering_config_` uses rusty::Cell for Copy types
- Callback invocation happens outside locks

## Test Plan

Unit tests (`test/rpc_request_buffering_test.cc`):
1. Request when connected sends immediately
2. Request when disconnected queues (QUEUE mode)
3. Request when disconnected fails (FAIL_FAST mode)
4. Reconnect replays queued requests
5. Expired requests not replayed
6. Queue overflow respects strategy
7. Pending request count tracking
8. Clear pending calls callbacks
9. Thread-safe concurrent buffering

## File Changes

| File | Change |
|------|--------|
| `src/srpc/rpc/client.hpp` | Add buffering members and methods (~50 LOC) |
| `src/srpc/rpc/client.cpp` | Implement buffering logic (~100 LOC) |
| `test/rpc_request_buffering_test.cc` | New test file (~250 LOC) |

## Estimated Size

~150-200 LOC total (header + implementation)
~250 LOC for tests

## Dependencies

- Phase 2.1: RequestQueue (provides queue implementation)
- Phase 1.3: Automatic Reconnection (provides reconnect mechanism)
