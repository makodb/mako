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
| RemoteDB class | ✅ Complete | Full interface with TCP socket RPC |
| RemoteTable class | ✅ Complete | Put/Get/Delete via RPC |
| Transaction handles | ✅ Complete | Encoding/decoding works |
| makoServer binary | ✅ Complete | Standalone server with TCP listener |
| Client mode flag | ✅ Complete | `--client <host> <port>` |
| RPC message types | ✅ Complete | Defined in common.h |
| Actual RPC calls | ✅ Complete | TCP socket communication implemented |
| Server-side handlers | ✅ Complete | Added to ShardReceiver (types 20-25) |
| ClientTcpServer | ✅ Complete | TCP listener for client connections |

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

### Phase 6: Full RPC Integration ✅
- [x] Implement actual RPC communication for Put/Get/Delete via TCP sockets
- [x] Add server-side handlers for client requests (`HandleClientPutRequest`, etc.)
- [x] Add transaction state management on server (`client_transactions_` map)
- [x] Add `ClientTcpServer` for accepting client connections
- [x] Integrated TCP server into `makoServer.cc` via `setup_client_tcp_server()`
- Est: ~500 LOC (actual: ~490 LOC)

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

### Implementation Complete

The full client-server RPC communication is now implemented:

#### Client-Side Components (RemoteDB)

| Method | Implementation |
|--------|----------------|
| `RemoteDB::Connect()` | Establishes TCP socket connection to server |
| `RemoteDB::BeginTransaction()` | Sends RPC to server, receives txn_id |
| `RemoteDB::Commit()` | Sends commit RPC with txn_id |
| `RemoteDB::Rollback()` | Sends rollback RPC with txn_id |
| `RemoteDB::SendPut()` | Sends Put RPC with key/value to server |
| `RemoteDB::SendGet()` | Sends Get RPC, receives value from server |
| `RemoteDB::SendDelete()` | Sends Delete RPC with key |

#### Server-Side Components

| Component | Location | Description |
|-----------|----------|-------------|
| `ClientTcpServer` | `src/mako/lib/client_tcp_server.h` | TCP listener for client connections |
| `HandleClientBeginTxnRequest` | `src/mako/lib/server.cc` | Handler for BeginTxn requests |
| `HandleClientCommitRequest` | `src/mako/lib/server.cc` | Handler for Commit requests |
| `HandleClientRollbackRequest` | `src/mako/lib/server.cc` | Handler for Rollback requests |
| `HandleClientPutRequest` | `src/mako/lib/server.cc` | Handler for Put requests |
| `HandleClientGetRequest` | `src/mako/lib/server.cc` | Handler for Get requests |
| `HandleClientDeleteRequest` | `src/mako/lib/server.cc` | Handler for Delete requests |

#### Transaction State Management

The server maintains a map of client transactions:
```cpp
// In ShardReceiver (server.h)
std::unordered_map<uint64_t, uint64_t> client_transactions_;  // client_txn_id -> server_txn_id
std::mutex client_txn_mutex_;  // Thread-safe access
std::atomic<uint64_t> server_txn_counter_{0};  // Unique server-side txn ID generator
```

#### Architecture Diagram

```
┌─────────────────────┐                 ┌─────────────────────────────────┐
│   Client Machine    │                 │         Mako Server             │
│  ┌─────────────────┐│                 │                                 │
│  │   RemoteDB      ││                 │  ┌─────────────────────────┐   │
│  │  (TCP Socket)   │├──── TCP ───────→│  │    ClientTcpServer      │   │
│  └─────────────────┘│   Port 31000    │  │    (TCP Listener)       │   │
│                     │                 │  └───────────┬─────────────┘   │
└─────────────────────┘                 │              │                 │
                                        │              ▼                 │
                                        │  ┌─────────────────────────┐   │
                                        │  │     ShardReceiver       │   │
                                        │  │   (RPC Handlers)        │   │
                                        │  └───────────┬─────────────┘   │
                                        │              │                 │
                                        │              ▼                 │
                                        │  ┌─────────────────────────┐   │
                                        │  │    Database Engine      │   │
                                        │  │  (Masstree + Paxos/Raft)│   │
                                        │  └─────────────────────────┘   │
                                        └─────────────────────────────────┘
```

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
