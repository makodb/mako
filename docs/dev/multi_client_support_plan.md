# Multiple Clients Support Implementation Plan

## Overview

This document describes the implementation plan for supporting multiple concurrent clients
in the decoupled client-server mode, with proper transaction isolation.

## Problem Statement

The current `ClientTcpServer` implementation:
1. Spawns unlimited handler threads (one per client)
2. No transaction context binding per client
3. Operations from different clients can interleave without isolation

## Solution Design

### Key Insight: Worker Slots with Transaction Context

Each Mako server has `nthreads` worker threads. Each worker can handle one client at a time
with proper transaction isolation by binding the worker to a transaction context.

**Capacity per shard**: `nthreads` concurrent clients
**Total capacity**: `nthreads × nshards` (across all shards)

### Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                      ClientTcpServer                                 │
│                                                                      │
│   ┌───────────────────────────────────────────────────────────────┐ │
│   │                    Worker Pool                                 │ │
│   │                                                                │ │
│   │   Slot 0 ──► [Thread Context 0] ──► Client A (txn bound)      │ │
│   │   Slot 1 ──► [Thread Context 1] ──► Client B (txn bound)      │ │
│   │   Slot 2 ──► [Thread Context 2] ──► (free)                     │ │
│   │   ...                                                          │ │
│   │   Slot N-1 ──► [Thread Context N-1] ──► (free)                 │ │
│   │                                                                │ │
│   └───────────────────────────────────────────────────────────────┘ │
│                                                                      │
│   When slot full:                                                    │
│   New Client ──► Reject("servers occupied, try later")              │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation Components

#### 1. Worker Slot Structure (~30 LOC)

```cpp
// Worker slot for managing client-to-worker binding
struct WorkerSlot {
    std::atomic<bool> in_use{false};      // Is slot occupied?
    int client_fd{-1};                     // Client socket
    std::thread worker_thread;             // Worker thread
    int worker_id{0};                      // Worker ID for TThread

    // Atomically try to acquire this slot
    bool TryAcquire() {
        bool expected = false;
        return in_use.compare_exchange_strong(expected, true);
    }

    void Release() {
        in_use.store(false);
        client_fd = -1;
    }
};
```

#### 2. Worker Pool in ClientTcpServer (~50 LOC)

Add to `ClientTcpServer`:
- `std::vector<WorkerSlot> worker_slots_` - Pool of worker slots
- `size_t max_clients_` - Maximum concurrent clients (= nthreads)
- `SetMaxClients(size_t n)` - Configure capacity
- `TryAcquireSlot()` - Find and acquire a free slot
- `ReleaseSlot(int slot_id)` - Release a slot

#### 3. Connection Handling (~80 LOC)

Update `ListenerLoop()`:
```cpp
void ListenerLoop() {
    while (!stop_requested_) {
        int client_fd = accept(...);

        // Try to acquire a worker slot
        int slot_id = TryAcquireSlot();
        if (slot_id < 0) {
            // No slots available - send rejection message
            SendRejectionResponse(client_fd, "All servers occupied, try later");
            close(client_fd);
            continue;
        }

        // Spawn worker thread with proper context binding
        SpawnWorkerThread(slot_id, client_fd);
    }
}
```

#### 4. Worker Thread with Transaction Context (~100 LOC)

```cpp
void WorkerThread(int slot_id, int client_fd) {
    // Set up thread context for isolation
    scoped_db_thread_ctx ctx(db_, true, 1);
    TThread::set_id(base_thread_id_ + slot_id);
    TThread::set_mode(1);
    TThread::set_shard_index(shard_index_);
    TThread::set_pid(slot_id);
    TThread::set_nshards(nshards_);

    // Process client requests (existing HandleClient logic)
    HandleClientRequests(client_fd);

    // Release slot when done
    ReleaseSlot(slot_id);
}
```

#### 5. Rejection Response Message (~20 LOC)

Add new message type to `common.h`:
```cpp
const uint8_t clientServerBusyType = 26;

struct client_server_busy_response_t {
    uint8_t status;
    char message[64];  // "All servers occupied, try later"
};
```

### Transaction Isolation Guarantee

With this design:
1. Each client is bound to exactly one worker slot
2. Each worker slot has its own `TThread` context
3. All operations from a client go through that context
4. `scoped_db_thread_ctx` ensures proper transaction boundaries
5. OCC/2PL mechanisms provide isolation between workers

### Integration Points

1. **BenchmarkConfig**: Get `nthreads` for worker pool size
2. **rpc_setup.cc**: Pass db pointer and config to ClientTcpServer
3. **makoServer.cc**: Configure max clients based on nthreads

## Estimated LOC

| Component | LOC |
|-----------|-----|
| WorkerSlot structure | ~30 |
| Worker pool management | ~50 |
| Connection handling updates | ~80 |
| Worker thread with context | ~100 |
| Rejection response | ~20 |
| Integration | ~30 |
| **Total** | **~310** |

Within 500 LOC limit.

## Testing Strategy

1. **Single client**: Verify existing functionality still works
2. **Max clients**: Connect exactly `nthreads` clients, verify all work
3. **Overflow**: Connect `nthreads + 1` clients, verify rejection message
4. **Concurrent operations**: Run multiple clients doing Put/Get, verify isolation

## RustyCpp Safety Notes

- Use `std::atomic<bool>` for slot state (thread-safe)
- Mark socket operations as `@unsafe`
- Mark `TThread::set_*` calls as `@unsafe` (legacy code)
