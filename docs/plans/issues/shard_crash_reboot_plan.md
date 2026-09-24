# Shard Crash and Reboot Recovery Plan (Simple Mode)

## Status: IMPLEMENTED (Phase 1) - 2026-01-09

## Overview

This document describes the plan to support shard servers crashing and rebooting while the system continues operating. This is a simplified version:

- **No replication**: Single shard per partition
- **No RocksDB recovery**: Shard reboots to empty state
- **Focus**: RPC client reconnection and graceful error handling

## Implementation Summary

### Completed Tasks

1. **Transaction Timeout** (from previous task)
   - Transactions timeout after 30 seconds (configurable via `txn_timeout_ms`)
   - `TXN_TIMEOUT = -30` result code for timed-out transactions
   - Modified 4 coordinator wait() calls to use timeout

2. **Client Reconnection Support**
   - **`Client::connection_state()`**: Returns current connection state (NEW, CONNECTING, CONNECTED, FAILED, etc.)
   - **`Client::try_reconnect_if_needed()`**: Attempts reconnection if in FAILED/DISCONNECTED state
   - **`ClientPool::get_client()` health checking**: Now checks connection health, attempts reconnection, recreates failed connections
   - **`Communicator::EnsureClientConnected()`**: Helper for checking/repairing connections to specific sites

3. **ShardFailureController** (from previous task)
   - Thread-safe failure simulation per shard
   - `fail_shard()`, `recover_shard()`, `is_shard_failed()` methods

### Files Changed

| File | Changes |
|------|---------|
| `src/srpc/rpc/client.hpp` | Added `connection_state()`, `try_reconnect_if_needed()` |
| `src/srpc/rpc/client.cpp` | Modified `ClientPool::get_client()` with health checking |
| `src/deptran/communicator.h` | Added `EnsureClientConnected()` declaration |
| `src/deptran/communicator.cc` | Added `EnsureClientConnected()` implementation |

### How Recovery Works

```
1. Shard Failure
   └─→ Connection fails → handle_error() → state = FAILED
   └─→ Pending RPCs get ENOTCONN error
   └─→ RPC callbacks see error, return early (don't vote)
   └─→ QuorumEvent never reaches quorum
   └─→ Transaction times out after 30 seconds

2. Shard Recovery
   └─→ Next transaction attempt calls ClientPool::get_client()
   └─→ get_client() detects FAILED state
   └─→ Calls try_reconnect_if_needed()
   └─→ If reconnection succeeds, return healthy client
   └─→ If reconnection fails, recreate all connections
   └─→ Transaction proceeds normally
```

## Goals

1. **Graceful failure**: Transactions to crashed shard fail with error (not hang) ✓
2. **Partial availability**: Transactions to healthy shards continue working ✓
3. **Automatic reconnection**: Clients reconnect when shard comes back ✓
4. **Testability**: Use multi-shard single-process framework for testing ✓

## Original Analysis

### What Works

| Component | Status | Details |
|-----------|--------|---------|
| Connection state machine | ✓ Exists | `connection_state.hpp` - transitions to FAILED |
| Reconnection policy | ✓ Exists | `reconnect_policy.hpp` - exponential backoff |
| ShardFailureController | ✓ Implemented | From timeout task - simulates shard failure |

### What Was Missing (Now Fixed)

| Gap | Impact | Priority | Status |
|-----|--------|----------|--------|
| No auto RPC retry | Clients get error immediately, no retry | High | ✓ Fixed via timeout + reconnection |
| No pool recovery | Failed connections stay in pool | High | ✓ Fixed in ClientPool::get_client() |
| No auto reconnect | Clients don't reconnect when server returns | High | ✓ Fixed via try_reconnect_if_needed() |
| No graceful txn failure | Transactions may hang on crashed shard | High | ✓ Fixed via transaction timeout |

## Architecture

### Current Error Flow

```
Client sends RPC → Server crashes → TCP RST
                                 ↓
                    ClientConnection::handle_error()
                                 ↓
                    State → FAILED
                                 ↓
                    invalidate_pending_futures() → ENOTCONN
                                 ↓
                    Coordinator sees error → ??? (may hang or crash)
```

### Proposed Error Flow

```
Client sends RPC → Server crashes → TCP RST
                                 ↓
                    ClientConnection::handle_error()
                                 ↓
                    State → FAILED
                                 ↓
                    [NEW] Schedule background reconnection
                                 ↓
                    Return ENOTCONN to pending futures
                                 ↓
                    [NEW] Coordinator retries with backoff
                                 ↓
                    After max retries → Transaction fails with TIMEOUT error
                                 ↓
                    Other transactions to healthy shards continue
```

### Crash/Reboot Flow (No Replication)

```
┌─────────────────────────────────────────────────────────────────┐
│                    NORMAL OPERATION                              │
│  Client → Coordinator → RPC → Shard (no replication)            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ CRASH (simulated via ShardFailureController)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    SHARD CRASH                                   │
│  1. Shard marked as failed (ShardFailureController)             │
│  2. RPCs to shard don't get responses (or connection fails)     │
│  3. Client sees ENOTCONN or timeout                              │
│  4. [NEW] Coordinator retries with exponential backoff          │
│  5. After max retries, transaction fails with TIMEOUT           │
│  6. Transactions to other shards continue normally              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ REBOOT (recover shard)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    SHARD REBOOT (EMPTY STATE)                    │
│  1. ShardFailureController::recover_shard() called              │
│  2. Shard starts responding to RPCs again                        │
│  3. [NEW] Next RPC attempt succeeds                              │
│  4. [NEW] Connection pool creates fresh connection              │
│  5. Transactions resume (shard has empty/fresh state)           │
└─────────────────────────────────────────────────────────────────┘
```

## Implementation

### Task 1: Investigate Current Behavior

Before implementing, test what currently happens:

```cpp
// In test code using multi-shard single-process mode
#include "mako/benchmarks/shard_failure_controller.h"

// Start 2-shard system
// ...

// Run some transactions
auto result1 = RunCrossShardTxn(shard0, shard1);
EXPECT_EQ(result1.status, SUCCESS);

// Simulate shard 1 crash
ShardFailureController::get()->fail_shard(1);

// Try cross-shard transaction - what happens?
auto result2 = RunCrossShardTxn(shard0, shard1);
// Does it hang? Return error? Crash?

// Try single-shard transaction to shard 0
auto result3 = RunSingleShardTxn(shard0);
// Does this still work?
```

Document findings before implementing fixes.

### Task 2: Enable Client Reconnection

#### 2.1 Add Auto-Reconnect to Client

```cpp
// src/srpc/rpc/client.hpp

class Client {
  bool auto_reconnect_ = true;
  ReconnectPolicy reconnect_policy_ = ReconnectPolicy::conservative();
  std::string last_addr_;  // Remember address for reconnection

public:
  void set_auto_reconnect(bool enable) { auto_reconnect_ = enable; }
};

// src/srpc/rpc/client.cpp

void ClientConnection::handle_error(int events) {
  // Existing: transition to FAILED, invalidate futures
  state_machine_.transition_to(ConnectionState::FAILED);
  invalidate_pending_futures();

  // NEW: Schedule reconnection if enabled
  if (auto_reconnect_ && !last_addr_.empty()) {
    Log_info("Connection to %s failed, will reconnect on next request",
             last_addr_.c_str());
    // Don't reconnect immediately - let next request trigger it
    // This avoids blocking and thundering herd
  }
}
```

#### 2.2 Connection Pool Recovery

```cpp
// src/srpc/rpc/client.hpp

class ClientPool {
public:
  Arc<Client> get_client(const std::string& addr) {
    std::lock_guard<SpinLock> guard(lock_);
    auto& clients = pool_[addr];

    // Remove failed connections from pool
    auto it = std::remove_if(clients.begin(), clients.end(),
      [](const Arc<Client>& c) {
        auto state = c->get_state();
        return state == ConnectionState::FAILED ||
               state == ConnectionState::DISCONNECTED;
      });
    clients.erase(it, clients.end());

    // Return existing healthy connection if available
    for (auto& client : clients) {
      if (client->get_state() == ConnectionState::CONNECTED) {
        return client;
      }
    }

    // Create new connection
    auto client = Arc<Client>::make_new();
    int ret = client->connect(addr.c_str());
    if (ret == 0) {
      clients.push_back(client);
      return client;
    }

    // Connection failed - return nullptr or throw
    Log_warn("Failed to connect to %s", addr.c_str());
    return nullptr;
  }
};
```

### Task 3: Coordinator-Level Retry

```cpp
// src/deptran/communicator.cc

// Add retry wrapper for RPC calls
template<typename Func>
auto RetryRpc(Func&& rpc_func, int max_retries = 3) {
  int retry = 0;
  while (retry < max_retries) {
    auto future = rpc_func();
    if (future == nullptr) {
      // Connection failed
      retry++;
      Log_info("RPC failed (no connection), retry %d/%d", retry, max_retries);
      usleep(100000 * (1 << retry));  // 100ms, 200ms, 400ms
      continue;
    }

    // Wait for response with timeout
    future->timed_wait(Config::GetConfig()->get_txn_timeout());

    int err = future->get_error_code();
    if (err == 0) {
      return future;  // Success
    }

    if (err == ENOTCONN || err == ETIMEDOUT) {
      retry++;
      Log_info("RPC failed (err=%d), retry %d/%d", err, retry, max_retries);
      usleep(100000 * (1 << retry));
      continue;
    }

    // Other error - don't retry
    return future;
  }

  Log_warn("RPC failed after %d retries", max_retries);
  return decltype(rpc_func())();  // Return null/empty future
}

// Use in SendPrepare, SendCommit, etc.
shared_ptr<QuorumEvent> Communicator::SendPrepare(...) {
  return RetryRpc([&]() {
    return DoSendPrepare(...);
  });
}
```

### Task 4: Tests

#### 4.1 Unit Test

```cpp
// test/deptran/shard_crash_test.cpp

#include "mako/benchmarks/shard_failure_controller.h"

class ShardCrashTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Start 2-shard single-process mode
    StartMultiShardSystem(2);
  }

  void TearDown() override {
    ShardFailureController::get()->reset();
    StopMultiShardSystem();
  }
};

TEST_F(ShardCrashTest, TransactionToHealthyShardContinues) {
  // Fail shard 1
  ShardFailureController::get()->fail_shard(1);

  // Transaction to shard 0 should still work
  auto result = RunSingleShardTxn(0);
  EXPECT_EQ(result.status, SUCCESS);
}

TEST_F(ShardCrashTest, TransactionToCrashedShardFailsGracefully) {
  // Fail shard 1
  ShardFailureController::get()->fail_shard(1);

  // Transaction to shard 1 should fail with timeout, not hang
  auto start = std::chrono::steady_clock::now();
  auto result = RunSingleShardTxn(1);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(result.status, TIMEOUT);
  EXPECT_LT(elapsed, std::chrono::seconds(60));  // Should timeout, not hang forever
}

TEST_F(ShardCrashTest, CrossShardTxnFailsWhenOneShardDown) {
  // Fail shard 1
  ShardFailureController::get()->fail_shard(1);

  // Cross-shard transaction should fail
  auto result = RunCrossShardTxn(0, 1);
  EXPECT_EQ(result.status, TIMEOUT);
}

TEST_F(ShardCrashTest, ReconnectAfterRecovery) {
  // Fail shard 1
  ShardFailureController::get()->fail_shard(1);

  // Transaction fails
  auto result1 = RunSingleShardTxn(1);
  EXPECT_EQ(result1.status, TIMEOUT);

  // Recover shard 1
  ShardFailureController::get()->recover_shard(1);

  // Transaction should work now
  auto result2 = RunSingleShardTxn(1);
  EXPECT_EQ(result2.status, SUCCESS);
}
```

#### 4.2 Integration Test Script

```bash
#!/bin/bash
# examples/test_shard_crash_recovery.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"

echo "========================================="
echo "Shard Crash Recovery Test (Simple Mode)"
echo "========================================="

# Start 2-shard single-process mode
CMD="./${BUILD_DIR:-build}/dbtest \
    --num-threads 4 \
    --shard-config src/mako/config/local-shards2-warehouses4.yml \
    -P localhost \
    -L 0,1 \
    --txn-timeout-ms 5000 \
    --fail-shard-after 15:1 \
    --recover-shard-after 25:1"

nohup $CMD > test_crash_recovery.log 2>&1 &
PID=$!

echo "Started test (PID: $PID)"
echo "- Shard 1 will fail at t=15s"
echo "- Shard 1 will recover at t=25s"

# Wait for test to complete
sleep 40

kill $PID 2>/dev/null

echo ""
echo "Checking results..."

# Check for expected behavior
failed=0

if grep -q "Shard 1 marked as FAILED" test_crash_recovery.log; then
    echo "  ✓ Shard 1 failure detected"
else
    echo "  ✗ Shard 1 failure not detected"
    failed=1
fi

if grep -q "Shard 1 marked as RECOVERED" test_crash_recovery.log; then
    echo "  ✓ Shard 1 recovery detected"
else
    echo "  ✗ Shard 1 recovery not detected"
    failed=1
fi

# Check that throughput resumed after recovery
if grep -q "agg_persist_throughput" test_crash_recovery.log; then
    echo "  ✓ Throughput reported (system running)"
else
    echo "  ✗ No throughput reported"
    failed=1
fi

if [ $failed -eq 0 ]; then
    echo ""
    echo "SUCCESS: Shard crash recovery working"
    exit 0
else
    echo ""
    echo "FAILURE: Check test_crash_recovery.log"
    exit 1
fi
```

## Estimated LOC

| Task | LOC |
|------|-----|
| Task 1: Investigation | 0 (research) |
| Task 2: Client reconnection | ~100 |
| Task 3: Coordinator retry | ~100 |
| Task 4: Tests | ~200 |
| **Total** | **~400** |

## Success Criteria

1. **Graceful failure**: Transactions to crashed shard return TIMEOUT error (not hang)
2. **Partial availability**: Transactions to healthy shards continue during failure
3. **Automatic reconnection**: Clients reconnect when shard comes back
4. **System resilience**: Full throughput resumes after shard recovery
5. **Tests pass**: All tests in multi-shard single-process mode pass

## Relationship to Other Tasks

This task builds on:
- **Transaction Timeout Task**: Uses `ShardFailureController` from that task
- **RPC Reliability Task**: Uses connection state machine and reconnection policy

This task is simpler than full replication recovery:
- No Raft/Paxos election handling
- No RocksDB log replay
- No state recovery - shard starts fresh

## References

- Connection state machine: `src/srpc/rpc/connection_state.hpp`
- Reconnection policy: `src/srpc/rpc/reconnect_policy.hpp`
- Multi-shard test: `examples/test_multi_shard_single_process.sh`
- ShardFailureController: `src/mako/benchmarks/shard_failure_controller.h` (to be created)
