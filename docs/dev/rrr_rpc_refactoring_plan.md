# RRR RPC Refactoring Plan for Client-Server Communication

## Overview

This document outlines the plan to refactor the Mako client-server communication from raw TCP sockets to use the existing RRR RPC framework.

## Current Implementation (Raw TCP Sockets)

**Files involved:**
- `src/mako/lib/client_tcp_server.h` - Raw TCP server accepting client connections
- `src/mako/remote_db.hh` - Client using raw TCP sockets
- `src/mako/lib/server.cc` - Handlers for client messages (types 20-25)
- `examples/makoServer.cc` - Starts ClientTcpServer

**Issues:**
1. Custom protocol requires manual message framing
2. Not integrated with existing RRR infrastructure
3. Duplicates socket handling code that RRR already provides
4. CI tests may fail due to timing/connection issues

## Target Implementation (RRR RPC)

### Server Side: MakoClientService

Create a new `rrr::Service` implementation for the client API.

```cpp
// src/mako/client_service.h

class MakoClientService : public rrr::Service {
public:
    // RPC IDs (matching existing message types for compatibility)
    static const i32 BEGIN_TXN = 20;
    static const i32 COMMIT = 21;
    static const i32 ROLLBACK = 22;
    static const i32 PUT = 23;
    static const i32 GET = 24;
    static const i32 DELETE = 25;

    // Constructor takes reference to ShardReceiver for actual operations
    MakoClientService(ShardReceiver* receiver);

    // RRR Service interface
    int __reg_to__(rrr::Server& server, size_t svc_index) override;
    void __dispatch__(i32 rpc_id, rusty::Box<rrr::Request> req,
                      rrr::WeakServerConnection sconn) override;

    // RPC Handlers
    void HandleBeginTxn(rusty::Box<rrr::Request> req,
                        rrr::WeakServerConnection sconn);
    void HandleCommit(rusty::Box<rrr::Request> req,
                      rrr::WeakServerConnection sconn);
    void HandleRollback(rusty::Box<rrr::Request> req,
                        rrr::WeakServerConnection sconn);
    void HandlePut(rusty::Box<rrr::Request> req,
                   rrr::WeakServerConnection sconn);
    void HandleGet(rusty::Box<rrr::Request> req,
                   rrr::WeakServerConnection sconn);
    void HandleDelete(rusty::Box<rrr::Request> req,
                      rrr::WeakServerConnection sconn);

private:
    ShardReceiver* receiver_;  // Handles actual DB operations
};
```

### Client Side: MakoClientProxy

Create a proxy class that wraps `rrr::Client` for client-side RPC calls.

```cpp
// src/mako/client_proxy.h

class MakoClientProxy {
public:
    MakoClientProxy(rusty::Arc<rrr::Client> client);

    // Sync methods (block until response)
    i32 BeginTxn(uint64_t client_txn_id, uint64_t* server_txn_id);
    i32 Commit(uint64_t client_txn_id);
    i32 Rollback(uint64_t client_txn_id);
    i32 Put(uint64_t client_txn_id, int table_id,
            const std::string& key, const std::string& value);
    i32 Get(uint64_t client_txn_id, int table_id,
            const std::string& key, std::string* value);
    i32 Delete(uint64_t client_txn_id, int table_id,
               const std::string& key);

    // Async methods (return Future for non-blocking)
    rrr::FutureResult async_BeginTxn(uint64_t client_txn_id,
                                     const rrr::FutureAttr& attr = {});
    // ... etc

private:
    rusty::Arc<rrr::Client> client_;
};
```

### Integration Changes

**makoServer.cc:**
```cpp
// Replace:
mako::setup_client_tcp_server(...);

// With:
auto poll_thread = rrr::PollThread::create();
auto server = rusty::Arc<rrr::Server>::make(rusty::Some(poll_thread));
auto client_service = rusty::Box<MakoClientService>::make(&receiver);
server->reg_service(std::move(client_service));
server->start(client_listen_addr);
```

**remote_db.hh (RemoteDB class):**
```cpp
// Replace:
bool Connect(const std::string& host, int port);
// Raw socket operations...

// With:
bool Connect(const std::string& addr) {
    poll_thread_ = rrr::PollThread::create();
    client_ = rrr::Client::create(poll_thread_);
    if (client_->connect(addr.c_str()) != 0) {
        return false;
    }
    proxy_ = std::make_unique<MakoClientProxy>(client_);
    return true;
}
```

## Implementation Steps

### Phase 1: Create RPC Service (Server Side)
1. Create `src/mako/client_service.h`
2. Create `src/mako/client_service.cc`
3. Add to CMakeLists.txt
4. Estimated LOC: ~150

### Phase 2: Create RPC Proxy (Client Side)
1. Create `src/mako/client_proxy.h`
2. Create `src/mako/client_proxy.cc`
3. Add to CMakeLists.txt
4. Estimated LOC: ~150

### Phase 3: Integration
1. Update `makoServer.cc` to use rrr::Server
2. Update `remote_db.hh` to use MakoClientProxy
3. Update `simpleTransactionRep.cc` run_client_mode()
4. Estimated LOC: ~100

### Phase 4: Cleanup
1. Remove `client_tcp_server.h` (or keep for reference)
2. Remove raw socket code from `remote_db.hh`
3. Update CI tests if needed
4. Estimated LOC: -200 (removal)

## Message Protocol

Request/Response format using RRR Marshal:

**BeginTxn Request:**
```
<client_txn_id: uint64>
```

**BeginTxn Response:**
```
<server_txn_id: uint64>
<status: i32>
```

**Put Request:**
```
<client_txn_id: uint64>
<table_id: i32>
<key_len: i32><key_data: bytes>
<value_len: i32><value_data: bytes>
```

**Put Response:**
```
<status: i32>
```

**Get Request:**
```
<client_txn_id: uint64>
<table_id: i32>
<key_len: i32><key_data: bytes>
```

**Get Response:**
```
<status: i32>
<value_len: i32><value_data: bytes>  (if status == 0)
```

## Benefits

1. **Reuse existing infrastructure**: RRR handles connection management, buffering, timeouts
2. **Consistent with codebase**: Same pattern as Paxos, transaction services
3. **Built-in features**: Reconnection, keepalive, health checking
4. **Simpler maintenance**: One RPC framework instead of two

## Risks

1. **Learning curve**: Understanding RRR patterns
2. **Migration effort**: ~400 LOC changes
3. **Testing**: Need to verify all operations work correctly

## Total Estimated LOC

- New code: ~400 LOC
- Removed code: ~200 LOC
- Net change: ~200 LOC

Within the 500 LOC limit per task.
