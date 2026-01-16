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

---

## Implementation Notes (Post-Implementation Update)

### What Was Implemented

The implementation provides a basic client-server RPC infrastructure:

1. **Server-Side** (`src/mako/lib/server.cc`):
   - 6 handlers for message types 20-25 (BeginTxn, Commit, Rollback, Put, Get, Delete)
   - Transaction ID tracking in `client_transactions_` map with mutex protection
   - `ClientTcpServer` class in `lib/client_tcp_server.h` for accepting TCP connections

2. **Client-Side** (`src/mako/remote_db.hh`):
   - TCP socket connection to server
   - Synchronous request-response RPC calls
   - `RemoteDB` and `RemoteTable` classes mirroring local `DB` interface

3. **Integration** (`examples/makoServer.cc`):
   - Server starts `ClientTcpServer` on port 31000+shardIdx
   - Cleanup on shutdown

### Critical Limitation: Transaction Isolation

**Problem Statement:**

In the original colocated model:
- Client and server share the same process/thread
- Operations are synchronous - one transaction completes before the next starts
- `scoped_db_thread_ctx` ensures proper transaction boundaries
- Mako's OCC/2PL provides isolation automatically

In the current decoupled model:
- Multiple TCP clients can connect simultaneously
- Each client sends independent requests
- **The current implementation does NOT provide true transaction isolation**

**Current Behavior:**

```
Client A: BeginTxn → Put(k1, v1) → Put(k2, v2) → Commit
Client B: BeginTxn → Put(k1, v3) →            → Commit

Interleaved execution may result in:
  Put(k1, v1) [A]
  Put(k1, v3) [B]  ← Overwrites A's write!
  Put(k2, v2) [A]
  Commit [B]
  Commit [A]
```

**Root Cause:**

The handlers call `shard_put()` / `shard_get()` directly without:
1. Establishing a proper transaction context per client
2. Acquiring locks or versioning for OCC
3. Integrating with Mako's existing transaction coordinator

**What's Missing for True Transaction Support:**

```cpp
// Current (No Isolation):
void HandleClientPutRequest(...) {
    // Just verifies txn_id exists in map
    it->second->shard_put(key, value);  // Direct call, no isolation
}

// Required (With Isolation):
void HandleClientPutRequest(...) {
    // 1. Get transaction context for this client
    auto* txn_ctx = GetOrCreateTxnContext(txn_id);

    // 2. Execute within transaction boundary
    scoped_db_thread_ctx ctx(db, txn_ctx);  // Bind to transaction

    // 3. OCC: Record read/write set
    txn_ctx->AddToWriteSet(table_id, key, value);

    // 4. Actually perform the write (may be buffered until commit)
    it->second->shard_put(key, value);
}

void HandleClientCommitRequest(...) {
    auto* txn_ctx = GetTxnContext(txn_id);

    // OCC validation: Check for conflicts with concurrent transactions
    if (!txn_ctx->ValidateReadSet()) {
        txn_ctx->Abort();
        return ABORT;
    }

    // Atomically apply write set
    txn_ctx->ApplyWriteSet();
    txn_ctx->Commit();
}
```

### Recommended Next Steps

To achieve true transaction isolation in the decoupled model:

| Priority | Task | Complexity |
|----------|------|------------|
| **High** | Per-client transaction context management | Medium (~200 LOC) |
| **High** | Integrate with existing `scoped_db_thread_ctx` | Medium (~150 LOC) |
| **Medium** | Read/write set tracking for OCC validation | High (~300 LOC) |
| **Medium** | Proper commit protocol with conflict detection | High (~250 LOC) |
| **Low** | Multi-shard transaction coordination (2PC) | Very High (~500+ LOC) |

### Current Suitable Use Cases

The current implementation is suitable for:
- ✅ Single-client scenarios (one client at a time)
- ✅ Read-only workloads (no write conflicts)
- ✅ Demonstration and testing of RPC infrastructure
- ✅ Building blocks for future full transaction support

**NOT suitable for:**
- ❌ Multi-client concurrent writes
- ❌ Production workloads requiring ACID guarantees
- ❌ Distributed transactions across shards
