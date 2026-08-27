# Phase 6.2: Error Callbacks and Hooks

## Overview

This phase adds a callback system for connection events, allowing users to register handlers for connection lifecycle events like connection establishment, disconnection, errors, and reconnection.

## Design Goals

1. **Event-Driven**: Notify users of connection state changes asynchronously
2. **Multiple Callbacks**: Support multiple callbacks per event type
3. **Thread-Safe**: Safe to register/invoke callbacks from any thread
4. **Non-Blocking**: Callbacks should be invoked without blocking the caller
5. **RustyCpp Safe**: Use `rusty::Cell`/`rusty::RefCell` for interior mutability

## API Design

### ConnectionCallbacks Struct

```cpp
// Callback function types
using ConnectionCallback = std::function<void()>;
using ErrorCallback = std::function<void(RpcError, const std::string&)>;
using ReconnectCallback = std::function<void(bool success)>;

struct ConnectionCallbacks {
    // Connection lifecycle
    std::vector<ConnectionCallback> on_connected;
    std::vector<ConnectionCallback> on_disconnected;

    // Error handling
    std::vector<ErrorCallback> on_error;

    // Reconnection events
    std::vector<ConnectionCallback> on_reconnecting;
    std::vector<ReconnectCallback> on_reconnected;
};
```

### CallbackManager Class

```cpp
class CallbackManager {
public:
    // Registration methods
    void add_on_connected(ConnectionCallback cb);
    void add_on_disconnected(ConnectionCallback cb);
    void add_on_error(ErrorCallback cb);
    void add_on_reconnecting(ConnectionCallback cb);
    void add_on_reconnected(ReconnectCallback cb);

    // Invocation methods (called by ClientConnection)
    void invoke_on_connected();
    void invoke_on_disconnected();
    void invoke_on_error(RpcError error, const std::string& message);
    void invoke_on_reconnecting();
    void invoke_on_reconnected(bool success);

    // Utility
    void clear_all();
    size_t callback_count() const;

private:
    rusty::RefCell<ConnectionCallbacks> callbacks_;
};
```

## Integration Points

### ClientConnection Integration

Update `ClientConnection` to invoke callbacks at appropriate state transitions:

1. **on_connected**: After successful `connect()` (state → CONNECTED)
2. **on_disconnected**: After `close()` or connection lost (state → DISCONNECTED)
3. **on_error**: When errors occur during operations
4. **on_reconnecting**: At start of `reconnect()` (state → CONNECTING)
5. **on_reconnected**: After `reconnect()` completes (success or failure)

### Client Integration

Add callback registration methods to `Client` class:
- `void add_connection_callback(...)` - delegates to ClientConnection's CallbackManager

## Implementation Steps

1. Create `src/srpc/rpc/callbacks.hpp` with:
   - Callback type definitions
   - ConnectionCallbacks struct
   - CallbackManager class (~100-150 LOC)

2. Integrate with `ClientConnection`:
   - Add CallbackManager member
   - Invoke callbacks at state transitions
   - Expose registration methods

3. Update `Client` class to expose callback registration

## Thread Safety

- `rusty::RefCell<ConnectionCallbacks>` for interior mutability
- Callbacks are copied before invocation to avoid holding lock during execution
- Callback invocation catches exceptions to prevent propagation

## Test Plan

Unit tests (`test/rpc_callbacks_test.cc`):
1. Single callback registration and invocation
2. Multiple callbacks per event type
3. Callback receives correct parameters (error code, message)
4. Clear all callbacks
5. Thread-safe registration during invocation
6. Exception in callback doesn't affect others
7. Integration with connection state changes

## File Changes

| File | Change |
|------|--------|
| `src/srpc/rpc/callbacks.hpp` | New file: CallbackManager class |
| `src/srpc/rpc/client.hpp` | Add callback registration methods |
| `src/srpc/rpc/client.cpp` | Invoke callbacks, add CallbackManager |
| `test/rpc_callbacks_test.cc` | New test file |
| `CMakeLists.txt` | Add test target |

## Estimated Size

~100-150 LOC for callbacks.hpp
~50 LOC for integration changes
~200 LOC for tests
