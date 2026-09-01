# Phase 3.1: Heartbeat/Keep-Alive Mechanism Plan

## Overview

Implement a heartbeat mechanism to detect stale connections and trigger reconnection when the remote endpoint becomes unresponsive.

## Design

### HeartbeatConfig

```cpp
struct HeartbeatConfig {
    bool enabled;              // Enable/disable heartbeat (default: true)
    uint32_t interval_ms;      // Ping interval (default: 10000ms = 10s)
    uint32_t timeout_ms;       // Pong timeout (default: 5000ms = 5s)
    uint32_t max_missed;       // Max missed pongs before triggering action (default: 3)
};
```

### HeartbeatManager Class

```cpp
class HeartbeatManager {
    HeartbeatConfig config_;
    rusty::Cell<uint64_t> last_send_time_;
    rusty::Cell<uint64_t> last_recv_time_;
    rusty::Cell<uint32_t> missed_count_;
    rusty::Cell<bool> pending_pong_;
    std::function<void()> on_timeout_;  // Callback for timeout (reconnection trigger)

public:
    // Called to send heartbeat (by timer)
    void send_heartbeat();

    // Called when pong received
    void on_pong_received();

    // Called periodically to check timeout
    bool check_timeout() const;

    // Set timeout callback
    void set_on_timeout(std::function<void()> callback);
};
```

### Heartbeat Protocol

The heartbeat uses a simple ping/pong protocol:
1. Client sends ping periodically (every `interval_ms`)
2. Server responds with pong
3. If no pong received within `timeout_ms`, increment missed count
4. If `missed_count >= max_missed`, trigger timeout callback

Note: For simplicity, we use empty RPC calls as heartbeats rather than a special protocol.
The heartbeat RPC is just a no-op request that exercises the connection.

### Integration

HeartbeatManager is optional and created separately from ClientConnection.
Applications can create and manage heartbeats as needed.

## Implementation Details

### Thread Safety

- Uses `rusty::Cell<T>` for all mutable state
- Timer callback runs on poll thread

### Timer Integration

Uses existing reactor timeout events for periodic heartbeats.

### RustyCpp Compliance

- All functions annotated @safe
- Uses rusty::Cell for interior mutability
- No raw pointers

## File Structure

New file: `src/srpc/rpc/heartbeat.hpp`

## Estimated LOC

~150-200 lines
