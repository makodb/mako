# Phase 1.1: Connection State Machine Implementation Plan

## Overview

Implement a thread-safe connection state machine for RPC client connections that tracks the full lifecycle from creation to cleanup.

## Current State

The existing `ClientConnection` class has a simple enum:
```cpp
enum {
    NEW, CONNECTED, CLOSED
} status_;
```

This lacks:
- Intermediate states (CONNECTING, DISCONNECTING)
- Error state (FAILED)
- State transition callbacks
- Thread-safe state access

## Design

### State Enum

```cpp
enum class ConnectionState : int {
    NEW = 0,          // Initial state, not yet connected
    CONNECTING = 1,   // Connection attempt in progress
    CONNECTED = 2,    // Successfully connected
    DISCONNECTING = 3,// Graceful disconnect in progress
    DISCONNECTED = 4, // Cleanly disconnected
    FAILED = 5        // Connection failed (error occurred)
};
```

### State Transition Diagram

```
    +-----------+
    |    NEW    |
    +-----------+
         |
         | connect()
         v
    +-----------+     connection failed     +-----------+
    |CONNECTING |-------------------------->|  FAILED   |
    +-----------+                           +-----------+
         |                                       ^
         | connection success                    |
         v                                       |
    +-----------+     error occurred       +-----+
    | CONNECTED |------------------------->|
    +-----------+                          |
         |                                 |
         | close()                         |
         v                                 |
    +-----------+     error during close   |
    |DISCONNECT-|------------------------->+
    |   ING     |
    +-----------+
         |
         | close complete
         v
    +-----------+
    |DISCONNECT-|
    |    ED     |
    +-----------+
```

### Valid Transitions

| From State     | To State      | Trigger                    |
|----------------|---------------|----------------------------|
| NEW            | CONNECTING    | connect() called           |
| CONNECTING     | CONNECTED     | connection established     |
| CONNECTING     | FAILED        | connection error           |
| CONNECTED      | DISCONNECTING | close() called             |
| CONNECTED      | FAILED        | error during operation     |
| DISCONNECTING  | DISCONNECTED  | close completed            |
| DISCONNECTING  | FAILED        | error during close         |
| FAILED         | CONNECTING    | reconnect attempt (future) |
| DISCONNECTED   | CONNECTING    | reconnect attempt (future) |

### ConnectionStateMachine Class

```cpp
// @safe - Thread-safe connection state management
class ConnectionStateMachine {
private:
    rusty::Cell<ConnectionState> state_;

    // Callbacks for state transitions
    std::function<void(ConnectionState, ConnectionState)> on_state_change_;

public:
    ConnectionStateMachine();

    // @safe - Get current state
    ConnectionState state() const;

    // @safe - Attempt state transition, returns true if successful
    bool transition_to(ConnectionState new_state);

    // @safe - Check if transition is valid
    bool can_transition_to(ConnectionState new_state) const;

    // @safe - Set state change callback
    void set_on_state_change(std::function<void(ConnectionState, ConnectionState)> callback);

    // @safe - State queries
    bool is_connected() const;
    bool is_failed() const;
    bool is_terminal() const;  // DISCONNECTED or FAILED
};
```

## Implementation Details

### Thread Safety

- Use `rusty::Cell<ConnectionState>` for the state field (trivially copyable enum)
- State transitions are atomic via Cell::set()
- Callbacks are invoked after successful transition

### RustyCpp Compliance

All code follows rusty-cpp guidelines:
- Uses `rusty::Cell<T>` for interior mutability
- All functions annotated with `@safe`
- No raw pointers
- No mutable fields (use Cell for interior mutability)

### Integration with ClientConnection

Modify `ClientConnection` to use `ConnectionStateMachine`:
1. Replace `status_` enum with `ConnectionStateMachine state_machine_`
2. Update `connect()` to use state transitions
3. Update `close()` to use state transitions
4. Update `handle_error()` to transition to FAILED

## File Structure

New file: `src/srpc/rpc/connection_state.hpp`

## Estimated LOC

- connection_state.hpp: ~120-150 lines
- Integration changes: ~30-50 lines
- Total: ~150-200 lines

## Testing

Unit tests will be added in Phase 7 (as per TODO.md).
