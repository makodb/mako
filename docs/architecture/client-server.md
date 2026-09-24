# Client-Server Architecture

This document describes Mako's client-server architecture that enables decoupled deployment where clients run on different machines from database servers.

## Overview

Mako supports three operating modes in `simpleTransactionRep`:

| Mode | Command | Description |
|------|---------|-------------|
| Default | `./simpleTransactionRep <args>` | Server + transaction tests |
| Server-only | `./simpleTransactionRep --server <args>` | Standalone server waiting for clients |
| Client | `./simpleTransactionRep --client <host> <port>` | Remote client connecting to server |

## Quick Start

### Server Mode (Standalone)
```bash
# Start server on shard 0 with 6 threads, Paxos replication
./build/simpleTransactionRep --server 2 0 6 localhost 1

# Or without replication
./build/simpleTransactionRep --server 1 0 4 localhost 0
```

### Client Mode
```bash
# Connect to server at localhost:31000
./build/simpleTransactionRep --client localhost 31000
```

## Client API

The remote client uses `mako::RemoteDB` which mirrors the local `mako::DB` interface:

```cpp
#include "remote_db.hh"

// Connect to remote server
mako::RemoteOptions opts;
opts.server_host = "192.168.1.100";
opts.server_port = 31000;

mako::RemoteDB* db = nullptr;
mako::Status s = mako::RemoteDB::Connect(opts, &db);
if (!s.ok()) { /* handle error */ }

// Get table proxy
RemoteTable* table = db->GetTable("customer_0");

// Transaction operations (same as local DB)
void* txn = db->BeginTransaction();
table->Put(txn, "key", value);
table->Get(txn, "key", retrieved_value);
db->Commit(txn);

delete db;
```

## Architecture

```
┌─────────────────────┐                  ┌─────────────────────┐
│   Client Process    │                  │   Server Process    │
│                     │                  │                     │
│  ┌───────────────┐  │      TCP/RPC     │  ┌───────────────┐  │
│  │  RemoteDB     │──┼──────────────────┼──│ ClientHandler │  │
│  └───────────────┘  │                  │  └───────────────┘  │
│         │           │                  │         │           │
│  ┌───────────────┐  │                  │  ┌───────────────┐  │
│  │ RemoteTable   │  │                  │  │  abstract_db  │  │
│  └───────────────┘  │                  │  └───────────────┘  │
└─────────────────────┘                  └─────────────────────┘
```

### Key Components

| Component | File | Description |
|-----------|------|-------------|
| RemoteDB | `src/mako/remote_db.hh` | Client-side database proxy |
| RemoteTable | `src/mako/remote_db.hh` | Client-side table proxy |
| MakoClientProxy | `src/mako/client_proxy.h` | SRPC RPC client wrapper |
| ClientTcpServer | `src/mako/lib/client_tcp_server.h` | Server-side TCP listener |

### RPC Protocol

The client-server communication uses 6 message types defined in `common.h`:

| Type ID | Operation | Request Fields | Response Fields |
|---------|-----------|----------------|-----------------|
| 20 | BeginTxn | client_id | txn_id, status |
| 21 | Commit | txn_id | status |
| 22 | Rollback | txn_id | status |
| 23 | Put | txn_id, table_id, key, value | status |
| 24 | Get | txn_id, table_id, key | value, status |
| 25 | Delete | txn_id, table_id, key | status |

### Transaction Handle Encoding

Transaction IDs are encoded as 64-bit integers:
- Upper 32 bits: client_id (unique per client connection)
- Lower 32 bits: per-service atomic counter (ensures uniqueness across all BeginTxn calls)

Implementation in `MakoClientService::HandleBeginTxn`:
```cpp
uint32_t counter = next_txn_counter_.fetch_add(1, std::memory_order_relaxed);
uint64_t txn_id = (static_cast<uint64_t>(client_id) << 32) | counter;
```

The client passes opaque `void*` handles that encode this txn_id.

## Transaction Semantics

### Auto-Commit Model

**Mako uses auto-commit semantics for the client API.** Each Put/Get/Delete operation is immediately committed when executed - operations are NOT buffered for a later Commit call.

| API Call | What It Does |
|----------|--------------|
| BeginTransaction | Creates a transaction tracking entry, generates txn_id |
| Put/Get/Delete | Immediately commits the operation (auto-commit) |
| Commit | Removes transaction from tracking (operations already committed) |
| Rollback | Removes transaction from tracking (cannot undo already-committed ops) |

This design ensures:
- **Durability**: No buffered data is lost on crash
- **Simplicity**: Each operation is atomic and immediately visible

Implications:
- **No multi-operation atomicity**: Cannot commit or rollback multiple operations as a unit
- **Rollback is a no-op**: Since operations auto-commit, Rollback cannot undo them

### Example: Auto-Commit Behavior

```cpp
void* txn = db->BeginTransaction();
table->Put(txn, "key1", "value1");  // Immediately committed
table->Put(txn, "key2", "value2");  // Immediately committed
db->Rollback(txn);  // Does NOT undo the puts - they're already committed!
// key1 and key2 now exist in the database
```

## Known Limitations

### Transaction Isolation

**The current implementation does NOT guarantee transaction isolation for concurrent clients.**

Safe use cases:
- Single client performing transactions
- Read-only workloads (multiple readers, no writers)
- Testing and development

Unsafe use cases:
- Multiple clients writing concurrently (lost updates possible)
- Production workloads requiring ACID guarantees

See [Client-Server Roadmap](client-server-roadmap.md) for planned isolation improvements.

### Single Shard Only

The client TCP server currently only works in multi-shard configurations where helper servers exist. For single-shard deployments, use local `mako::DB` instead.

## Configuration

### Server Configuration

Server listens on port `31000 + shardIdx`:
- Shard 0: port 31000
- Shard 1: port 31001
- etc.

### Client Configuration

```cpp
struct RemoteOptions {
    std::string server_host = "localhost";
    int server_port = 31000;
    int shard_index = 0;
    int num_shards = 1;
    uint32_t timeout_ms = 5000;  // RPC timeout
};
```

## History

The client-server architecture was implemented in January 2026:
- Phase 1-5: Design, server, client library, examples, CI tests
- Phase 6-7: SRPC RPC integration, code consolidation
- `makoServer.cc` was merged into `simpleTransactionRep.cc` with `--server` flag
