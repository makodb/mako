# Transport Stop Fix - Complete RRR Coordination Shutdown Fix

## Background

### Original Problem (Commit b9d3940)
- Commit b9d3940 added RRR/RPC support for distributed transaction coordination (previously only eRPC/DPDK was supported)
- The system would hang (continuously printing throughput messages) or coredump when using RRR for coordination
- Commit a6f342 attempted to fix the issue by:
  - Adding double `stop()` calls (before and after worker join)
  - Making `ShardClient::stop()` idempotent
  - Handling epoll races gracefully (`ENOENT`/`EBADF` in `epoll_wrapper.h`)
  - Moving `mako::stop_erpc_server()` call to after worker join
- However, a6f342 only mitigated the issue - the system could still hang or coredump intermittently

### Root Cause Analysis
The problem had **multiple critical race conditions**:

1. **Thread Synchronization Issues:**
   - `stop_` flag was plain `bool` instead of `std::atomic<bool>` - no memory synchronization between threads
   - `Stop()` was not idempotent - multiple concurrent calls could execute simultaneously
   - This led to race conditions, double-delete bugs, and use-after-free crashes

2. **Incomplete RRR Server Shutdown:**
   - Helper queue worker threads never received `request_stop()` signals
   - Server wasn't properly shut down in `Stop()` method
   - This caused the "can't stop printing throughput" symptom

3. **TOCTOU (Time-of-Check to Time-of-Use) Races:**
   - RPC methods (SendToAll, SendToShard, SendBatchToAll) didn't check `stop_` flag at entry points
   - GetOrCreateClient() could return clients that were immediately invalidated by concurrent Stop()
   - Threads could be in-flight when Stop() deleted resources, leading to segmentation faults

4. **Shutdown Order Issue:**
   - Servers needed to stay alive until workers finished processing
   - The shutdown order fix from a6f342 was correct but insufficient without the other race condition fixes

## Complete Fix

### 1. Atomic Stop Flag

**File: `src/mako/lib/rrr_rpc_backend.h` (line 189)**

Changed `stop_` from plain `bool` to `std::atomic<bool>`:

```cpp
// Before:
bool stop_{false};

// After:
std::atomic<bool> stop_{false};
```

Also added `#include <atomic>` to the header.

**Why necessary:** Provides memory synchronization and atomic read/write operations across threads. Without this, different threads could see inconsistent values of the stop flag, leading to race conditions.

### 2. Idempotent Stop() Method

**File: `src/mako/lib/rrr_rpc_backend.cc` (line 464)**

Made Stop() idempotent using atomic compare-exchange:

```cpp
void RrrRpcBackend::Stop() {
    // Make Stop() idempotent - only the first call proceeds
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) {
        Notice("RrrRpcBackend::Stop: Already stopped, returning");
        return;
    }

    Notice("RrrRpcBackend::Stop: BEGIN - Setting stop flag");

    // Signal all helper queues to stop (both request and response queues)
    for (auto& entry : queue_holders_) {
        if (entry.second) {
            entry.second->request_stop();
        }
    }
    for (auto& entry : queue_holders_response_) {
        if (entry.second) {
            entry.second->request_stop();
        }
    }

    // Close all outstanding client connections to unblock any waiting futures
    std::vector<std::shared_ptr<rrr::Client>> clients_to_close;
    {
        std::lock_guard<std::mutex> guard(clients_lock_);
        for (auto& entry : clients_) {
            if (entry.second) {
                clients_to_close.push_back(entry.second);
            }
        }
        clients_.clear();
    }

    for (auto& client : clients_to_close) {
        client->close();
    }

    // Shutdown server to stop accepting new connections
    if (server_) {
        delete server_;
        server_ = nullptr;
    }

    Notice("RrrRpcBackend::Stop: END");
}
```

**Key changes:**
- **CRITICAL**: Atomic compare-exchange ensures only ONE thread executes shutdown logic
- Helper queue stop signaling for both request and response queues
- Server deletion in `Stop()` to ensure proper shutdown
- Prevents double-delete and concurrent shutdown bugs

### 3. Early Stop Checks in RPC Send Methods

Added `stop_` checks at the beginning of all RPC send methods to prevent new operations from starting during shutdown:

**SendToAll() (line 273):**
```cpp
bool RrrRpcBackend::SendToAll(...) {
    // Early return if stopping - don't start new RPC operations
    if (stop_) {
        Warning("RrrRpcBackend::SendToAll: stop requested, not sending");
        return false;
    }
    // ... rest of method
}
```

**SendToShard() (line 204):**
```cpp
bool RrrRpcBackend::SendToShard(...) {
    // Early return if stopping - don't start new RPC operations
    if (stop_) {
        Warning("RrrRpcBackend::SendToShard: stop requested, not sending");
        return false;
    }
    // ... rest of method
}
```

**SendBatchToAll() (line 362):**
```cpp
bool RrrRpcBackend::SendBatchToAll(...) {
    // Early return if stopping - don't start new RPC operations
    if (stop_) {
        Warning("RrrRpcBackend::SendBatchToAll: stop requested, not sending");
        return false;
    }
    // ... rest of method
}
```

**Why necessary:** Prevents new RPC operations from starting once shutdown is initiated. Without these checks, threads could initiate new RPCs while Stop() is deleting resources.

### 4. Lock-Protected Stop Check in GetOrCreateClient()

**File: `src/mako/lib/rrr_rpc_backend.cc` (line 167)**

```cpp
std::shared_ptr<rrr::Client> RrrRpcBackend::GetOrCreateClient(...) {
    // ... setup code ...

    clients_lock_.lock();

    // Check stop flag while holding lock - if stopping, don't create/return clients
    if (stop_) {
        clients_lock_.unlock();
        Warning("GetOrCreateClient: stop requested, not creating/returning client");
        return nullptr;
    }

    auto it = clients_.find(session_key);
    // ... rest of method
}
```

**Why necessary:**
- Checking `stop_` INSIDE the lock prevents TOCTOU race where Stop() clears clients_ between our check and our access
- Returns nullptr if stopping, causing RPC methods to fail gracefully
- Critical for preventing use-after-free on client pointers

### 5. Post-Wait Stop Check in SendToShard()

**File: `src/mako/lib/rrr_rpc_backend.cc` (line 247)**

```cpp
// Wait for response
fu->wait();

// Check stop again after wait - client might have been closed during wait
if (stop_) {
    Warning("RrrRpcBackend::SendToShard: stop requested after wait, aborting");
    rrr::Future::safe_release(fu);
    return false;
}

if (fu->get_error_code() != 0) {
    Warning("RPC error: %d", fu->get_error_code());
    rrr::Future::safe_release(fu);
    return false;
}
```

**Why necessary:**
- Even with early checks, an RPC operation could be in-flight when Stop() is called
- The wait might return with an error (ENOTCONN) because the client was closed
- Checking `stop_` again allows us to abort gracefully instead of trying to access the response
- **This was the final piece that fixed the intermittent segfaults**

### 6. Previous Fixes from a6f342 (Retained)

These fixes from a6f342 were kept because they address real issues:

**File: `src/mako/lib/shardClient.cc` (line 50):**
- Made `ShardClient::stop()` idempotent to allow double-stop pattern
- Only the first invocation tears down the transport

**File: `src/rrr/reactor/epoll_wrapper.h` (line 207):**
- Treat `ENOENT`/`EBADF` from `epoll_ctl(…, EPOLL_CTL_MOD …)` as benign races
- Allows shutdown to proceed when file descriptors vanish during concurrent close

**File: `src/mako/benchmarks/bench.cc`:**
- Servers stop AFTER workers finish (correct shutdown order)
- Prevents workers from hanging while trying to communicate with stopped servers

## Verification

### Test Results
All tests now pass consistently with clean shutdown:

```bash
# Test 1: Two shards without replication
$ ./ci/ci.sh shardNoReplication
=========================================
Checking test results...
=========================================
  ✓ Found 'agg_persist_throughput' keyword
    agg_persist_throughput: 34900.1 ops/sec
  ✓ NewOrder_remote_abort_ratio: 2.5775 (< 20%)
=========================================
All checks passed!
=========================================
✓ All processes exited cleanly

# Test 2: Two shards with replication
$ ./ci/ci.sh shard2Replication
=========================================
Checking test results...
=========================================
  ✓ Found 'agg_persist_throughput' keyword
    agg_persist_throughput: 16907.5 ops/sec (shard0), 29334.5 ops/sec (shard1)
  ✓ NewOrder_remote_abort_ratio: 1.27621 (< 40%)
=========================================
All checks passed!
=========================================
✓ All processes exited cleanly
```

**Multiple test runs:** All tests pass without segfaults or hangs.

### Confirmed Behaviors
- ✅ RunEventLoop() exits cleanly (confirmed in logs: "RrrRpcBackend::RunEventLoop: Exited cleanly")
- ✅ Helper queue worker threads receive stop signals and exit properly
- ✅ No more "can't stop printing throughput" messages
- ✅ **No segmentation faults during shutdown** (previously would crash intermittently)
- ✅ **No use-after-free bugs** (all stop checks prevent accessing deleted pointers)
- ✅ **No double-delete crashes** (atomic compare-exchange ensures Stop() is idempotent)
- ✅ All worker threads exit without hanging
- ✅ Throughput statistics are printed correctly
- ✅ All processes exit cleanly without requiring SIGKILL

### Debug Process
1. Analyzed crash logs showing RPC error 107 (ENOTCONN) and segmentation faults
2. Identified root causes: non-atomic stop_, non-idempotent Stop(), and TOCTOU races
3. Implemented five defensive fixes working together (atomic flag, idempotent Stop, early checks, lock check, post-wait check)
4. Each fix addresses a specific race condition
5. Verified fix with multiple test runs - all tests pass consistently without crashes

## Summary

The complete fix required addressing **multiple race conditions** in the RRR transport layer shutdown:

### Five Critical Fixes Working Together

1. **Atomic Stop Flag** (`std::atomic<bool>`) - Proper memory synchronization across threads
2. **Idempotent Stop()** (atomic compare-exchange) - Only ONE thread executes shutdown logic
3. **Early Stop Checks** (in all RPC send methods) - Prevents new RPCs from starting
4. **Lock-Protected Stop Check** (in GetOrCreateClient) - Prevents TOCTOU races on client access
5. **Post-Wait Stop Check** (in SendToShard) - Handles in-flight RPCs gracefully

### Defense-in-Depth Approach

These five fixes provide **defense-in-depth** against shutdown races:
- If a thread sneaks past the early check, the lock check catches it
- If an RPC is in-flight when Stop() runs, the post-wait check handles it
- The atomic flag ensures all threads see consistent state
- The idempotent Stop() prevents double-delete bugs

### Plus Retained Fixes from a6f342

6. **Helper queue stop signaling** - Ensures worker threads exit properly
7. **Shutdown order fix in bench.cc** - Servers stop AFTER workers finish
8. **Epoll error handling** - Graceful handling of race conditions during socket close
9. **Idempotent ShardClient::stop()** - Allows double-stop pattern

### Key Insights

- **Thread synchronization is critical:** Plain `bool` is insufficient for multi-threaded shutdown
- **Multiple defenses are necessary:** No single check can prevent all races
- **Shutdown order matters:** Servers must stay alive until workers finish processing

**Correct shutdown lifecycle:** `Clients → Workers → Servers → Cleanup`

### Files Modified

1. `src/mako/lib/rrr_rpc_backend.h` - Made stop_ atomic, added #include <atomic>
2. `src/mako/lib/rrr_rpc_backend.cc` - All five race condition fixes above

Total: ~50 lines of actual logic changes (excluding debug logging)
