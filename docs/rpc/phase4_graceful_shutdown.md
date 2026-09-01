# Phase 4.1: Graceful Server Shutdown

## Status: IN PROGRESS

## Overview

This phase implements graceful server shutdown for the srpc/rpc module, allowing servers to:
1. Stop accepting new connections
2. Complete in-flight requests
3. Execute cleanup hooks
4. Close connections cleanly

## Current State Analysis

### Existing Infrastructure

The `Server` class already has:
- `do_shutdown()`: Sets shutdown flag and notifies waiting threads
- `wait_for_shutdown()`: Blocks until shutdown is signaled
- `~Server()`: Requests close for listener and all connections

### What's Missing

1. **Phased shutdown**: Current shutdown is all-or-nothing
2. **In-flight request tracking**: No way to wait for pending requests
3. **Shutdown hooks**: No callback mechanism for cleanup
4. **Drain operation**: No method to wait for all requests to complete

## Design

### Shutdown Phases

```
1. STOP_ACCEPTING
   - Stop listener from accepting new connections
   - Existing connections remain active

2. DRAINING
   - Allow existing requests to complete
   - Reject new requests on existing connections
   - Timeout after configurable duration

3. CLOSING
   - Close all connections
   - Execute shutdown hooks
   - Release resources

4. STOPPED
   - Server fully stopped
```

### New API

```cpp
// Server class additions

// Shutdown state
enum class ShutdownPhase {
    RUNNING,        // Normal operation
    STOP_ACCEPTING, // Not accepting new connections
    DRAINING,       // Waiting for in-flight requests
    CLOSING,        // Closing connections
    STOPPED         // Fully stopped
};

// Shutdown hooks
using ShutdownHook = std::function<void()>;

class Server {
    // ... existing members ...

    // Shutdown state
    rusty::Cell<ShutdownPhase> shutdown_phase_{ShutdownPhase::RUNNING};

    // Shutdown hooks
    std::vector<ShutdownHook> shutdown_hooks_;
    SpinMutex<int> pending_requests_{0};  // Track in-flight requests

    // New methods
    void add_shutdown_hook(ShutdownHook hook);
    void stop_accepting();     // Phase 1: Stop accepting
    bool drain(uint64_t timeout_ms = 30000);  // Phase 2: Wait for requests
    void shutdown();           // Full graceful shutdown
    ShutdownPhase phase() const;
    int pending_request_count() const;
};
```

### Request Tracking

Track in-flight requests using atomic counter:

```cpp
// In ServerConnection::handle_read()
void handle_read() {
    if (server_->phase() >= ShutdownPhase::DRAINING) {
        // Reject new requests during drain
        return;
    }

    // Increment pending count
    server_->increment_pending();

    // ... process request ...

    // Decrement in completion callback
}
```

### Shutdown Flow

```cpp
void Server::shutdown() {
    // Phase 1: Stop accepting new connections
    stop_accepting();

    // Phase 2: Drain existing requests with timeout
    drain(30000);  // 30 second timeout

    // Phase 3: Execute shutdown hooks
    for (auto& hook : shutdown_hooks_) {
        hook();
    }

    // Phase 4: Close all connections
    // ... existing destructor logic ...

    shutdown_phase_.set(ShutdownPhase::STOPPED);
}
```

## Implementation Tasks

### Task 1: Add Shutdown Phase Tracking (~30 LOC)
- Add `ShutdownPhase` enum
- Add `shutdown_phase_` member with Cell for interior mutability
- Add `phase()` getter

### Task 2: Add Shutdown Hooks (~40 LOC)
- Add `shutdown_hooks_` vector
- Add `add_shutdown_hook()` method
- Execute hooks during shutdown

### Task 3: Add Request Tracking (~50 LOC)
- Add `pending_requests_` counter
- Add `increment_pending()` / `decrement_pending()` methods
- Add `pending_request_count()` getter

### Task 4: Implement stop_accepting() (~30 LOC)
- Stop ServerListener from accepting
- Transition to STOP_ACCEPTING phase

### Task 5: Implement drain() (~50 LOC)
- Transition to DRAINING phase
- Wait for pending requests with timeout
- Return true if drained, false if timeout

### Task 6: Implement shutdown() (~30 LOC)
- Orchestrate full graceful shutdown sequence
- Call hooks, drain, close

### Total: ~230 LOC

## Testing Plan

### Unit Tests (test/rpc_graceful_shutdown_test.cc)

1. **ShutdownPhaseTransitions**: Verify state transitions
2. **ShutdownHooks**: Hooks called during shutdown
3. **PendingRequestTracking**: Counter increments/decrements
4. **StopAcceptingNewConnections**: Listener stops after stop_accepting()
5. **DrainCompletesWhenEmpty**: Drain returns immediately if no requests
6. **DrainTimesOut**: Drain respects timeout
7. **GracefulShutdownSequence**: Full shutdown flow

## Files Changed

| File | Changes |
|------|---------|
| `src/srpc/rpc/server.hpp` | Add ShutdownPhase, hooks, request tracking |
| `src/srpc/rpc/server.cpp` | Implement new methods |
| `test/rpc_graceful_shutdown_test.cc` | New test file |
| `CMakeLists.txt` | Add test target |

## RustyCpp Compliance

- Use `rusty::Cell<ShutdownPhase>` for interior mutability
- Use `SpinMutex<int>` for thread-safe request counter
- Add @safe/@unsafe annotations to all new methods
- All new code must pass borrow checker

## Success Criteria

1. Server can be stopped gracefully without losing in-flight requests
2. Shutdown hooks are called in order during shutdown
3. Drain respects timeout and returns appropriate status
4. All new code passes borrow checking
5. All tests pass
