# Client-Server RPC Implementation Plan

## Overview

This document describes the implementation plan for completing the full-fledged client-server RPC communication, replacing the stub implementations with actual working code.

## Background

The current implementation has:
- **Server-side**: `ShardReceiver` handles existing request types (get, lock, validate, etc.)
- **Client-side**: `RemoteDB` has stub implementations that return "not implemented" errors
- **Protocol**: Message types 20-25 defined in `common.h` for client API

## Implementation Strategy

### Design Decision: rrr::Client vs. Custom Socket

After analyzing the codebase, we choose **simple synchronous TCP sockets** for the initial implementation:

**Rationale:**
1. The `rrr::Client` is async-based and requires `PollThread` setup
2. The `FastTransport` requires full configuration with YAML files
3. Simple sockets allow a clean, standalone client library
4. Easy to optimize/replace with more efficient transport later

### Implementation Components

#### 1. Server-Side: Add Handlers to ShardReceiver (~150 LOC)

Location: `src/mako/lib/server.cc`

Add handlers for message types 20-25:
- `HandleClientBeginTxnRequest()` - Create transaction context
- `HandleClientCommitRequest()` - Commit transaction
- `HandleClientRollbackRequest()` - Abort transaction
- `HandleClientPutRequest()` - Put key-value
- `HandleClientGetRequest()` - Get value
- `HandleClientDeleteRequest()` - Delete key

**Note**: These handlers will be called by the existing transport layer when client requests arrive.

#### 2. Client-Side: Implement RPC in RemoteDB (~250 LOC)

Location: `src/mako/remote_db.hh`

Replace stub implementations:
- `Connect()` - Establish TCP connection to server
- `BeginTransaction()` - Send begin txn request, get txn_id
- `Commit()` - Send commit request
- `Rollback()` - Send rollback request
- `SendPut()` - Send put request with key/value
- `SendGet()` - Send get request, receive value
- `SendDelete()` - Send delete request

**Socket Communication Pattern:**
```cpp
// Connect
socket_fd = socket(AF_INET, SOCK_STREAM, 0);
connect(socket_fd, server_addr, ...);

// Send request
write(socket_fd, &request, sizeof(request));

// Receive response
read(socket_fd, &response, sizeof(response));
```

#### 3. Integration: Wire Server Handlers (~50 LOC)

Update `ShardReceiver::ReceiveRequest()` switch statement to dispatch client API message types to new handlers.

### Request/Response Protocol

Using existing structures from `common.h`:

| Operation | Request Struct | Response Struct |
|-----------|---------------|-----------------|
| BeginTxn | `client_begin_txn_request_t` | `client_begin_txn_response_t` |
| Commit | `client_commit_request_t` | `client_commit_response_t` |
| Rollback | `client_commit_request_t` | `client_commit_response_t` |
| Put/Get/Delete | `client_kv_request_t` | `client_kv_response_t` |

### Transaction State Management

On the server, maintain a map of client transaction IDs to local transaction objects:

```cpp
// In ShardReceiver
std::unordered_map<uint64_t, void*> client_transactions_;
std::mutex client_txn_mutex_;
```

## Rusty-Safety Requirements

All new code must follow RustyCpp guidelines:
- Use `rusty::Cell<T>` for interior mutability
- Annotate functions with `@safe` or `@unsafe`
- Avoid raw pointers for ownership
- Mark STL I/O calls as `@unsafe`

## Testing Strategy

1. **Unit test**: Verify server handlers work with mock requests
2. **Integration test**: Start server, connect client, run Put/Get operations
3. **CI regression**: Run all existing CI tests to ensure no breakage

## Estimated LOC

| Component | LOC |
|-----------|-----|
| Server handlers | ~150 |
| Client RPC | ~250 |
| Integration wiring | ~50 |
| Tests | ~50 |
| **Total** | **~500** |

## Dependencies

- Existing structures in `common.h`
- Existing `ShardReceiver` infrastructure
- Standard POSIX sockets

## Risk Mitigation

1. **Socket errors**: Add proper error handling and connection retry
2. **Timeout**: Add configurable timeout for RPC calls
3. **Thread safety**: Use mutex for transaction state map
