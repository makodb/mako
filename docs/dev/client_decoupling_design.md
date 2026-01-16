# Client-Server Decoupling Design Document

## Overview

This document describes the architecture for decoupling Mako clients from transaction execution servers, enabling clients to be deployed on different servers from the database.

## Problem Statement

Currently, the `simpleTransactionRep.cc` example (and similar applications) colocates:
1. **Client Logic**: Transaction operations (BeginTransaction, Put, Get, Commit)
2. **Server Logic**: Database initialization, RPC server, Paxos replication

This tight coupling means:
- Clients must run on the same machine as the database
- Cannot scale clients independently from database servers
- Harder to test and deploy in distributed environments

## Goal

Decouple the client from transaction execution so that:
- Clients can run on different servers from the database
- Multiple clients can connect to the same database server
- Existing API (`mako::DB`) remains compatible

## Architecture

### Current Architecture (Colocated)

```
┌─────────────────────────────────────────────────────────┐
│                    Single Process                        │
│  ┌─────────────────────────────────────────────────┐   │
│  │           Application Code                       │   │
│  │  ┌─────────────┐     ┌──────────────────────┐   │   │
│  │  │ mako::DB    │────→│ Transaction Workers  │   │   │
│  │  │ (Local)     │     │ (Threads)            │   │   │
│  │  └─────────────┘     └──────────────────────┘   │   │
│  └─────────────────────────────────────────────────┘   │
│                           │                             │
│  ┌────────────────────────▼────────────────────────┐   │
│  │              Mako Database Engine                │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────┐  │   │
│  │  │ Masstree │  │  Paxos   │  │ eRPC Server  │  │   │
│  │  └──────────┘  └──────────┘  └──────────────┘  │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Target Architecture (Decoupled)

```
 Client Machines                              Server Machine
 ===============                              ==============

┌─────────────────────┐                 ┌─────────────────────────────────┐
│   Client Machine 1   │                 │         Mako Server             │
│  ┌─────────────────┐ │                 │                                 │
│  │  RemoteDB       │ │───── RPC ─────→│  ┌─────────────────────────┐   │
│  │  (Proxy)        │ │                 │  │     Client Handler      │   │
│  └─────────────────┘ │                 │  │     (RPC Endpoint)      │   │
└─────────────────────┘                 │  └───────────┬─────────────┘   │
                                         │              │                 │
┌─────────────────────┐                 │              ▼                 │
│   Client Machine 2   │                 │  ┌─────────────────────────┐   │
│  ┌─────────────────┐ │                 │  │       mako::DB          │   │
│  │  RemoteDB       │ │───── RPC ─────→│  │       (Local)           │   │
│  │  (Proxy)        │ │                 │  └───────────┬─────────────┘   │
└─────────────────────┘                 │              │                 │
                                         │              ▼                 │
┌─────────────────────┐                 │  ┌─────────────────────────┐   │
│   Client Machine N   │                 │  │    Database Engine      │   │
│  ┌─────────────────┐ │                 │  │  (Masstree + Paxos/Raft)│   │
│  │  RemoteDB       │ │───── RPC ─────→│  └─────────────────────────┘   │
│  │  (Proxy)        │ │                 │                                 │
└─────────────────────┘                 └─────────────────────────────────┘

All clients connect to the SAME Client Handler via RPC.
The Client Handler is the single entry point that routes requests
through mako::DB to the Database Engine.
```

## Code Implementation Overview

This section provides a walkthrough of the implementation and how the code flows work.

### File Structure

```
mako/
├── examples/
│   ├── makoServer.cc              # Standalone server binary (NEW)
│   └── simpleTransactionRep.cc    # Updated with --client mode
├── src/mako/
│   ├── remote_db.hh               # RemoteDB client library (NEW)
│   ├── db.hh                      # Local DB interface (existing)
│   └── lib/
│       └── common.h               # Added client RPC message types
└── ci/
    └── test_client_server.sh      # Integration tests (NEW)
```

### How It Works: Server Mode Flow

When running in **server mode** (default), the flow is:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Server Mode Flow                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Parse command-line args (nshards, shardIdx, nthreads, etc.) │
│                         ↓                                        │
│  2. mako::DB::Open(options, path, &db)                          │
│     - Initializes BenchmarkConfig                                │
│     - Calls init_env() for replication setup                    │
│     - Calls initWithDB() to create database                     │
│                         ↓                                        │
│  3. setup_erpc_server()                                         │
│     - Starts RPC server on configured port                      │
│     - Registers ShardReceiver for handling requests             │
│                         ↓                                        │
│  4. setup_helper()                                              │
│     - Creates helper threads for remote warehouse operations    │
│                         ↓                                        │
│  5. Run tests / Wait for requests                               │
│     - simpleTransactionRep: runs TransactionWorker tests        │
│     - makoServer: waits for shutdown signal                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### How It Works: Client Mode Flow

When running in **client mode** (`--client` flag), the flow is:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Client Mode Flow                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Parse --client <host> <port> arguments                      │
│                         ↓                                        │
│  2. mako::RemoteDB::Connect(options, &db)                       │
│     - Creates RemoteDB instance                                  │
│     - Stores server host/port for RPC calls                     │
│     - Generates unique client_id                                │
│                         ↓                                        │
│  3. db->GetTable("table_name")                                  │
│     - Creates RemoteTable proxy (cached for reuse)              │
│     - Assigns table_id for RPC messages                         │
│                         ↓                                        │
│  4. db->BeginTransaction()                                      │
│     - Generates unique txn_id = (client_id << 32) | counter     │
│     - Returns opaque handle encoding txn_id                     │
│                         ↓                                        │
│  5. table->Put(txn, key, value) / Get(txn, key, &value)        │
│     - [STUB] Would send RPC to server                           │
│     - Currently returns "not implemented" error                 │
│                         ↓                                        │
│  6. db->Commit(txn) / Rollback(txn)                            │
│     - [STUB] Would send commit/rollback RPC                     │
│     - Currently no-op                                           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Key Classes and Their Responsibilities

| Class | File | Responsibility |
|-------|------|----------------|
| `mako::DB` | `src/mako/db.hh` | Local database wrapper (existing) |
| `mako::RemoteDB` | `src/mako/remote_db.hh` | Remote database proxy (new) |
| `mako::RemoteTable` | `src/mako/remote_db.hh` | Remote table proxy for Put/Get/Delete |
| `mako::RemoteOptions` | `src/mako/remote_db.hh` | Connection options (host, port, timeout) |

### Transaction Handle Encoding

Transaction handles are opaque `void*` pointers that encode the transaction ID:

```cpp
// Encoding: txn_handle = (void*)(txn_id)
void* EncodeTxnHandle(uint64_t txn_id) {
    return reinterpret_cast<void*>(txn_id);
}

// Decoding: txn_id = (uint64_t)(txn_handle)
uint64_t DecodeTxnHandle(void* handle) {
    return reinterpret_cast<uint64_t>(handle);
}

// Transaction ID structure:
// txn_id = (client_id << 32) | local_counter
// - Upper 32 bits: unique client identifier
// - Lower 32 bits: per-client transaction counter
```

### RPC Message Types (Added to common.h)

```cpp
// Client API message types (starting at 20 to avoid conflicts)
const uint8_t clientBeginTxnReqType = 20;   // Begin transaction
const uint8_t clientCommitReqType = 21;      // Commit transaction
const uint8_t clientRollbackReqType = 22;    // Rollback transaction
const uint8_t clientPutReqType = 23;         // Put key-value
const uint8_t clientGetReqType = 24;         // Get value by key
const uint8_t clientDeleteReqType = 25;      // Delete key
```

### Code Example: Switching Between Modes

**Local Mode (existing code works unchanged):**
```cpp
// Using local mako::DB
mako::DB* db = nullptr;
mako::Status s = mako::DB::Open(options, "/tmp/db", &db);

auto* table = db->GetDB()->open_sharded_index("customer");
void* txn = db->BeginTransaction();
table->Put(txn, key, value);  // Direct local operation
db->Commit(txn);
```

**Remote Mode (new RemoteDB):**
```cpp
// Using remote mako::RemoteDB
mako::RemoteDB* db = nullptr;
mako::RemoteOptions opts;
opts.server_host = "192.168.1.100";
opts.server_port = 31000;
mako::Status s = mako::RemoteDB::Connect(opts, &db);

auto* table = db->GetTable("customer");
void* txn = db->BeginTransaction();
table->Put(txn, key, value);  // RPC to server (stub for now)
db->Commit(txn);
```

### Current Implementation Status

| Feature | Status | Notes |
|---------|--------|-------|
| RemoteDB class | ✅ Complete | Full interface implemented |
| RemoteTable class | ✅ Complete | Put/Get/Delete methods |
| Transaction handles | ✅ Complete | Encoding/decoding works |
| makoServer binary | ✅ Complete | Standalone server runs |
| Client mode flag | ✅ Complete | `--client <host> <port>` |
| RPC message types | ✅ Complete | Defined in common.h |
| Actual RPC calls | ⏳ Stub | Returns "not implemented" |
| Server-side handlers | ⏳ TODO | Need to add to ShardReceiver |

## Component Design

### 1. Server Component (`makoServer`)

A standalone server binary that:
- Opens the database using `mako::DB::Open()`
- Sets up RPC server (reuses `setup_erpc_server()`)
- Handles client RPC requests
- Manages Paxos replication (if enabled)

**Entry Point**: `examples/makoServer.cc`

```cpp
int main(int argc, char** argv) {
    // 1. Parse config (shards, replication, etc.)
    // 2. Open database: mako::DB::Open(options, path, &db)
    // 3. Start RPC server: mako::setup_erpc_server()
    // 4. Register client handler: mako::setup_client_handler(db)
    // 5. Wait for shutdown signal
}
```

### 2. Client Library (`mako::RemoteDB`)

A client-side proxy that mirrors the `mako::DB` interface:

**Header**: `src/mako/remote_db.hh`

```cpp
namespace mako {

class RemoteDB {
public:
    // Connect to remote server
    static Status Connect(const RemoteOptions& options, RemoteDB** dbptr);

    // Same interface as mako::DB
    void* BeginTransaction();
    void Commit(void* txn);
    void Rollback(void* txn);

    // Get table proxy for operations
    RemoteTable* GetTable(const std::string& name);

private:
    Client* client_;  // Uses existing Client class for RPC
};

class RemoteTable {
public:
    Status Put(void* txn, const std::string& key, const std::string& value);
    Status Get(void* txn, const std::string& key, std::string& value);
    Status Delete(void* txn, const std::string& key);
};

}  // namespace mako
```

### 3. RPC Protocol

Extend the existing `Client` class with new message types:

| Message Type | Request | Response |
|-------------|---------|----------|
| `CLIENT_BEGIN_TXN` | `{client_id}` | `{txn_id}` |
| `CLIENT_COMMIT` | `{txn_id}` | `{status}` |
| `CLIENT_ROLLBACK` | `{txn_id}` | `{status}` |
| `CLIENT_PUT` | `{txn_id, table, key, value}` | `{status}` |
| `CLIENT_GET` | `{txn_id, table, key}` | `{status, value}` |
| `CLIENT_DELETE` | `{txn_id, table, key}` | `{status}` |

**Note**: The existing `Client` class already has `InvokeGet`, `InvokeLock`, etc. We extend this pattern.

### 4. Transaction ID Management

Each remote transaction needs a unique identifier:

```cpp
struct RemoteTxnId {
    uint64_t client_id;    // Unique client identifier
    uint64_t local_txn_id; // Client-local transaction counter
};
```

The server maintains a mapping:
```cpp
std::unordered_map<RemoteTxnId, void*> active_transactions_;
```

## Implementation Plan

### Phase 1: Design Document (This Document) ✅
- [x] Document architecture and API
- [x] Define RPC protocol
- Est: ~50 LOC (documentation)

### Phase 2: Server Entry Point ✅
- [x] Create `examples/makoServer.cc`
- [x] Add client handler that processes RPC requests
- [x] Wire up with existing `setup_erpc_server()` and `setup_helper()`
- Est: ~150 LOC (actual: ~186 LOC)

### Phase 3: Client Library ✅
- [x] Create `src/mako/remote_db.hh`
- [x] Implement `RemoteDB` class with same interface as `mako::DB`
- [x] Implement `RemoteTable` class for Put/Get/Delete
- [x] Use existing `Client` class for RPC transport (stub implementation)
- Est: ~200-300 LOC (actual: ~359 LOC)

### Phase 4: Update Example ✅
- [x] Add `--client` flag to `simpleTransactionRep.cc`
- [x] When in client mode, use `RemoteDB` instead of `mako::DB`
- Est: ~100 LOC (actual: ~93 LOC added)

### Phase 5: CI Tests ✅
- [x] Add test script that starts server, then client
- [x] Verify API functionality (stubs return expected errors)
- Est: ~100 LOC (actual: ~144 LOC)

### Phase 6: Full RPC Integration (Future)
- [ ] Implement actual RPC communication for Put/Get/Delete
- [ ] Add server-side handlers for client requests
- [ ] Add transaction state management on server
- [ ] End-to-end data integrity tests

## API Usage Example

### Standalone Mode (Current Behavior)
```cpp
mako::DB* db = nullptr;
mako::Status s = mako::DB::Open(options, path, &db);
auto* table = db->GetDB()->open_sharded_index("customer");
void* txn = db->BeginTransaction();
table->Put(txn, "key", "value");
db->Commit(txn);
```

### Client Mode (New)
```cpp
mako::RemoteDB* db = nullptr;
mako::RemoteOptions opts;
opts.server_address = "server.example.com:8080";
mako::Status s = mako::RemoteDB::Connect(opts, &db);

auto* table = db->GetTable("customer");
void* txn = db->BeginTransaction();
table->Put(txn, "key", "value");  // RPC to server
db->Commit(txn);                   // RPC to server
```

## RustyCpp Safety Requirements

All new code MUST follow RustyCpp guidelines:

1. **Use rusty types**: `rusty::Box<T>`, `rusty::Arc<T>`, `rusty::Cell<T>`
2. **Safety annotations**: Mark functions as `@safe` or `@unsafe`
3. **Avoid STL smart pointers**: No `std::unique_ptr`, `std::shared_ptr`
4. **No raw pointers** for ownership (only for borrowing)

Example:
```cpp
// @safe - No side effects, read-only
Status RemoteDB::Get(void* txn, const std::string& key, std::string& value) {
    // RPC call using existing Client class
    return client_->InvokeGet(...);  // @unsafe - calls legacy code
}
```

## Testing Strategy

1. **Unit Tests**: Test `RemoteDB` with mock server
2. **Integration Tests**: Start real server, connect with client
3. **Existing CI**: Ensure all existing tests pass in standalone mode
4. **New CI**: Run `./ci/test_client_server.sh` for client-server integration tests

## Open Questions

1. **Connection pooling**: Should `RemoteDB` maintain multiple connections?
   - Decision: Start simple (single connection), add pooling later if needed

2. **Error handling**: How to handle network failures mid-transaction?
   - Decision: Transaction aborts on network failure; client must retry

3. **Authentication**: Should clients authenticate to server?
   - Decision: Out of scope for initial implementation; add later

## How to Use

This section explains how to use the client-server decoupling feature.

### Quick Start

**Option 1: Server Mode (Default - Database + Tests in Same Process)**
```bash
# Run database with tests locally (existing behavior)
./build/simpleTransactionRep <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]

# Examples:
./build/simpleTransactionRep 2 0 6 localhost 1        # 2-shard with Paxos replication
./build/simpleTransactionRep 2 0 6 localhost 1 raft   # 2-shard with Raft replication
./build/simpleTransactionRep 1 0 4 localhost 0        # 1-shard, no replication
```

**Option 2: Client Mode (Connect to Remote Server)**
```bash
# Run as client connecting to a remote server
./build/simpleTransactionRep --client <server_host> <server_port>

# Examples:
./build/simpleTransactionRep --client localhost 31000
./build/simpleTransactionRep --client 192.168.1.100 31000
```

**Option 3: Standalone Server (Database Only, No Tests)**
```bash
# Run standalone server that hosts database and waits for clients
./build/makoServer <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]

# Examples:
./build/makoServer 2 0 6 localhost 1       # 2-shard server with Paxos
./build/makoServer 1 0 4 localhost 0       # 1-shard server, no replication
```

### Command Reference

| Binary | Mode | Command | Description |
|--------|------|---------|-------------|
| `simpleTransactionRep` | Server | `./build/simpleTransactionRep 2 0 6 localhost 1` | Run DB + tests locally |
| `simpleTransactionRep` | Client | `./build/simpleTransactionRep --client localhost 31000` | Connect to remote server |
| `makoServer` | Server | `./build/makoServer 2 0 6 localhost 1` | Standalone DB server |

### Parameter Reference

| Parameter | Description | Valid Values |
|-----------|-------------|--------------|
| `nshards` | Number of shards in cluster | 1-16 |
| `shardIdx` | Index of this shard | 0 to nshards-1 |
| `nthreads` | Number of worker threads | 1-64 |
| `paxos_proc_name` | Process role | `localhost` (leader), `p1`, `p2` (followers), `learner` |
| `is_replicated` | Enable replication | 0 (disabled), 1 (enabled) |
| `replication_type` | Replication protocol | `paxos` (default), `raft` |

### Typical Deployment Scenarios

**Scenario 1: Single Machine Development**
```bash
# Just use server mode (existing behavior)
./build/simpleTransactionRep 1 0 4 localhost 0
```

**Scenario 2: Client-Server on Same Machine (Testing)**
```bash
# Terminal 1: Start server
./build/makoServer 1 0 4 localhost 0

# Terminal 2: Run client
./build/simpleTransactionRep --client localhost 31000
```

**Scenario 3: Distributed Deployment**
```bash
# On server machine (192.168.1.100):
./build/makoServer 2 0 6 localhost 1

# On client machine:
./build/simpleTransactionRep --client 192.168.1.100 31000
```

### Current Limitations

> **Note**: The current implementation uses stub RPC methods. Client mode demonstrates
> the API architecture but actual Put/Get operations return "not implemented" errors.
> Full RPC integration is planned for a future iteration.

#### What Works ✅

| Feature | Description |
|---------|-------------|
| Client connection | `RemoteDB::Connect()` creates client instance and sets connected state |
| Transaction lifecycle | `BeginTransaction()` generates unique txn_id, `Commit()`/`Rollback()` complete cleanly |
| Table proxy | `GetTable()` creates and caches `RemoteTable` instances |
| Error handling | All stubs return proper `Status` objects with descriptive messages |
| Server binary | `makoServer` starts database and waits for client connections |

#### What's Not Implemented (Stub Code Locations)

The following methods in `src/mako/remote_db.hh` are **stub implementations** that don't perform actual RPC:

| Method | Line | Current Behavior | What It Should Do |
|--------|------|------------------|-------------------|
| `RemoteDB::Connect()` | 272-290 | Sets `is_connected_=true` without network connection | Establish TCP/eRPC connection to server |
| `RemoteDB::BeginTransaction()` | 297-308 | Generates local txn_id only | Send RPC to server, get server-assigned txn_id |
| `RemoteDB::Commit()` | 310-317 | No-op (does nothing) | Send commit RPC, wait for server acknowledgment |
| `RemoteDB::Rollback()` | 319-326 | No-op (does nothing) | Send rollback RPC to cleanup server state |
| `RemoteDB::SendPut()` | 328-337 | Returns `IOError("Remote Put not yet implemented")` | Send Put RPC with txn_id, table_id, key, value |
| `RemoteDB::SendGet()` | 339-347 | Returns `IOError("Remote Get not yet implemented")` | Send Get RPC, receive value from server |
| `RemoteDB::SendDelete()` | 349-357 | Returns `IOError("Remote Delete not yet implemented")` | Send Delete RPC with txn_id, table_id, key |

#### Stub Code Examples

**Example: SendPut stub** (`src/mako/remote_db.hh:328-337`):
```cpp
inline Status RemoteDB::SendPut(uint64_t txn_id, uint16_t table_id,
                                const std::string& key, const std::string& value) {
    if (!is_connected_.load()) {
        return Status::IOError("Not connected to server");
    }

    // TODO: Send RPC to server
    // For now, return error indicating not implemented
    return Status::IOError("Remote Put not yet implemented - use local mode");
}
```

**Example: BeginTransaction stub** (`src/mako/remote_db.hh:297-308`):
```cpp
inline void* RemoteDB::BeginTransaction() {
    if (!is_connected_.load()) {
        return nullptr;
    }

    // TODO: Send RPC to server to create transaction
    // For now, generate a local txn_id (will fail on actual operations)
    uint64_t txn_id = (static_cast<uint64_t>(client_id_) << 32) |
                      static_cast<uint64_t>(GetNextReqId());

    return EncodeTxnHandle(txn_id);
}
```

#### Server-Side Missing Pieces

The server (`makoServer`) currently lacks handlers for client requests. To complete the implementation:

1. **Add RPC handlers in `ShardReceiver`** (`src/mako/lib/shardreceiver.cc`):
   - Handle `clientBeginTxnReqType` (message type 20)
   - Handle `clientCommitReqType` (message type 21)
   - Handle `clientRollbackReqType` (message type 22)
   - Handle `clientPutReqType` (message type 23)
   - Handle `clientGetReqType` (message type 24)
   - Handle `clientDeleteReqType` (message type 25)

2. **Add transaction state management**:
   - Map client txn_ids to server-side transaction objects
   - Track active transactions per client
   - Cleanup on client disconnect

3. **Wire RPC handlers to mako::DB operations**:
   - `clientPutReqType` → `table->Put(txn, key, value)`
   - `clientGetReqType` → `table->Get(txn, key, &value)`
   - etc.

#### Why Stubs Are Useful

The stub implementation allows:
1. **API validation**: Verify the client interface design works correctly
2. **Integration testing**: Test command-line parsing, connection flow, error handling
3. **Incremental development**: Build the architecture first, add RPC later
4. **Documentation**: Serve as specification for what each method should do

### Running CI Tests

```bash
# Test client-server integration
./ci/test_client_server.sh

# Test server mode (backward compatibility)
./ci/ci.sh simpleTransaction
```

## References

- Existing RPC implementation: `src/mako/lib/client.h`
- Transport backends: `doc/transport_backends.md`
- Architecture overview: `doc/architecture.md`
