# Phase 1.3: Automatic Reconnection Logic Plan

## Overview

Implement automatic reconnection for RPC client connections when the connection is lost.

## Design

### ReconnectManager Class

The `ReconnectManager` coordinates reconnection attempts using the `ReconnectCalculator`:

```cpp
class ReconnectManager {
    ReconnectPolicy policy_;
    ReconnectCalculator calculator_;
    rusty::Cell<bool> reconnecting_{false};

public:
    // Start reconnection process
    void start_reconnect(std::function<int()> connect_fn,
                         std::function<void(bool)> on_complete);

    // Cancel ongoing reconnection
    void cancel();

    // Check if reconnecting
    bool is_reconnecting() const;
};
```

### Integration with ClientConnection

1. Add `ReconnectPolicy` field to `ClientConnection`
2. Add `ReconnectManager` field (optional, created on first reconnection)
3. Modify `handle_error()` to trigger reconnection if policy allows
4. Add `reconnect()` method for manual reconnection
5. Add callbacks for reconnection events

### Connection State Machine Integration

The reconnection uses the state machine:
- On error: CONNECTED -> FAILED
- Start reconnect: FAILED -> CONNECTING
- Success: CONNECTING -> CONNECTED
- Failure after retries: stays in FAILED

### Async Reconnection Flow

```
1. handle_error() called
2. State -> FAILED
3. If policy.auto_reconnect && should_retry():
     a. Calculate delay
     b. Schedule reconnect after delay
     c. State -> CONNECTING
     d. Call connect()
     e. If success: State -> CONNECTED, callback(true)
     f. If fail: Go to step 3
4. Else: callback(false)
```

## Implementation Details

### Thread Safety

- `ReconnectManager` uses `rusty::Cell<bool>` for reconnecting flag
- Reconnection happens on the poll thread (same as connection operations)
- State machine provides thread-safe state tracking

### Timer Integration

Use existing reactor timer mechanism for delays:
- `Reactor::get_reactor()->create_timeout_event(delay_ms, callback)`

### RustyCpp Compliance

- All functions annotated @safe
- Uses rusty::Cell for interior mutability
- No raw pointers

## API Changes

### ClientConnection
```cpp
// New fields
private:
    ReconnectPolicy reconnect_policy_;
    rusty::Option<ReconnectManager> reconnect_manager_;

// New methods
public:
    void set_reconnect_policy(ReconnectPolicy policy);
    void reconnect(std::function<void(bool)> on_complete = nullptr);
    bool is_reconnecting() const;
```

### Client
```cpp
// New methods
public:
    void set_reconnect_policy(ReconnectPolicy policy);
    void reconnect(std::function<void(bool)> on_complete = nullptr);
```

## File Changes

- Modified: `src/srpc/rpc/client.hpp` (~100 LOC)
- Modified: `src/srpc/rpc/client.cpp` (~150 LOC)

## Estimated LOC

~200-300 lines total
