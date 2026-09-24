# Phase 4.2: Server Restart Detection

## Status: DONE (2026-01-09)

## Overview

This phase implements server restart detection for the srpc/rpc module. When a server restarts, clients can detect this by checking if the server's instance ID has changed.

## Goals

1. Each server gets a unique instance ID on startup
2. Instance ID is included in initial connection response
3. Clients can detect server restarts when the ID changes
4. Callbacks notify application code of server restarts

## Design

### Server Instance ID

Generate a unique ID on server startup using a combination of:
- Current timestamp
- Random component
- Process ID (for uniqueness in multi-process scenarios)

```cpp
// In server.hpp
class Server {
    // Unique instance ID generated on startup
    uint64_t instance_id_;

public:
    // @safe - Returns server instance ID
    uint64_t instance_id() const { return instance_id_; }
};
```

### Client-Side Tracking

Store expected instance ID on client connection:

```cpp
// In client.hpp
class ClientConnection {
    // Server instance ID from last successful connection
    rusty::Cell<uint64_t> server_instance_id_{0};

    // Callback for restart detection
    std::function<void(uint64_t old_id, uint64_t new_id)> on_server_restart_;

public:
    void set_on_server_restart(std::function<void(uint64_t, uint64_t)> callback);
    uint64_t server_instance_id() const;
};
```

### Protocol Addition

Add instance ID to RPC protocol:
1. Server sends instance ID in first response after connection
2. Client stores the ID
3. On reconnection, client compares new ID with stored ID
4. If different, emit `on_server_restart` callback

### Implementation Approach

Since modifying the RPC wire protocol is complex and risky, we'll use a simpler approach:

1. **Server-side**: Generate instance_id_ on Server construction
2. **API exposure**: Provide `Server::instance_id()` getter
3. **Application-level detection**: Applications can periodically query server ID via a custom RPC
4. **Future enhancement**: Wire protocol changes can be added later if needed

This gives us the building blocks without requiring protocol changes.

## Implementation Tasks

### Task 1: Add Instance ID Generation (~30 LOC)
- Add `instance_id_` member to Server class
- Generate unique ID in Server constructor using:
  - `std::chrono::steady_clock::now()` for timestamp
  - `std::random_device` for randomness
  - Mix with XOR for final ID

### Task 2: Add Instance ID to Client (~40 LOC)
- Add `server_instance_id_` to ClientConnection (Cell<uint64_t>)
- Add `set_server_instance_id()` to store ID
- Add `server_instance_id()` getter
- Add `on_server_restart_` callback member
- Add `set_on_server_restart()` setter

### Task 3: Add Restart Detection Helper (~30 LOC)
- Add `check_server_instance(uint64_t new_id)` method
- Compare with stored ID, emit callback if changed
- Update stored ID

### Total: ~100 LOC

## Files Changed

| File | Changes |
|------|---------|
| `src/srpc/rpc/server.hpp` | Add instance_id_, getter |
| `src/srpc/rpc/server.cpp` | Generate ID in constructor |
| `src/srpc/rpc/client.hpp` | Add server_instance_id_, callback |
| `src/srpc/rpc/client.cpp` | Implement restart detection |
| `test/rpc_restart_detection_test.cc` | Unit tests |
| `CMakeLists.txt` | Add test target |

## Testing Plan

### Unit Tests

1. **InstanceIdGenerated**: Server generates non-zero ID
2. **InstanceIdUnique**: Different servers get different IDs
3. **InstanceIdStableAcrossRequests**: Same server keeps same ID
4. **ClientTracksServerId**: Client stores server ID
5. **RestartCallbackCalled**: Callback fires when ID changes
6. **RestartCallbackNotCalledSameId**: No callback for same ID

## Success Criteria

1. Servers generate unique instance IDs
2. Clients can store and compare server IDs
3. Restart detection callback works correctly
4. All tests pass
5. No changes to wire protocol (future enhancement)

## Implementation Summary

### Completed 2026-01-09

#### Server-Side (server.hpp/server.cpp)
- Added `instance_id_` member to Server class (uint64_t)
- Generated unique ID in constructor using:
  - `std::chrono::steady_clock::now()` for timestamp (nanoseconds)
  - `std::random_device` for two 32-bit random values
  - `getpid()` for process ID
  - XOR mixing for final ID
- Added `instance_id()` getter

#### Client-Side (client.hpp)
- Added `server_instance_id_` member to ClientConnection (rusty::Cell<uint64_t>)
- Added `on_server_restart_` callback member (mutable std::function)
- Added `server_instance_id()` getter
- Added `set_on_server_restart()` callback setter (marked @unsafe for std::function ops)
- Added `check_server_instance(uint64_t new_id)` method (marked @unsafe):
  - Compares new ID with stored ID
  - Updates stored ID
  - If old ID was non-zero and differs from new, triggers callback
  - Returns true if restart detected
- Added wrapper methods to Client class delegating to ClientConnection

#### Tests (test/rpc_restart_detection_test.cc)
11 tests covering:
- InstanceIdGenerated
- InstanceIdUnique
- InstanceIdStableAcrossRequests
- InstanceIdUniqueAcrossThreads
- ClientInitialIdIsZero
- ClientTracksServerId
- RestartCallbackCalled
- RestartCallbackNotCalledSameId
- MultipleRestarts
- NoCallbackIfNotSet
- ClientWrapperServerInstanceId

### Design Notes
- Application-level detection approach chosen (no wire protocol changes)
- Applications can query server ID via custom RPC and use check_server_instance()
- Wire protocol changes deferred as future enhancement
- Total: ~100 LOC
