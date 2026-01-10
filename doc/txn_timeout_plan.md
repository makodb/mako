# Transaction Timeout and Shard Failure Handling Plan

## Overview

This document describes the plan to add transaction-level timeout support so that requests complete with an "error" state when shards fail, allowing the system to continue running without blocking.

## Goals

1. **Non-blocking failure handling**: Transactions should timeout and return error instead of blocking indefinitely
2. **Minimal changes**: Leverage existing `Event::wait(timeout)` infrastructure
3. **Best-effort commit**: Commit changes on reachable shards (no consistency guarantees for failed transactions)
4. **Testability**: Add shard failure simulation to multi-shard single-process framework

## Current Architecture

### Event Timeout Infrastructure (Already Exists)

```cpp
// src/rrr/reactor/event.h
class Event {
  enum EventStatus { INIT = 0, WAIT = 1, READY = 2, DONE = 3, TIMEOUT = 4 };

  // Timeout in microseconds (0 = no timeout)
  virtual void wait(uint64_t timeout=0) final;
};

// src/rrr/reactor/quorum_event.h
class QuorumEvent : public Event {
  bool timeouted_ = false;  // Currently unused
};
```

### Reactor Timeout Handling (Already Exists)

```cpp
// src/rrr/reactor/reactor.cc (lines 246-276)
// - Checks timeout_events_ for expired wakeup_time_
// - Sets status to TIMEOUT if time_now > wakeup_time_
// - Resumes coroutine with TIMEOUT status
```

### Current Coordinator Wait Calls (No Timeout)

```cpp
// src/deptran/classic/coordinator.cc

// Line 309 - Dispatch phase
sp_int_event->wait();  // Blocks indefinitely

// Line 433 - Prepare phase
quorum_event->wait();  // Blocks indefinitely

// Line 544 - Commit phase
quorum_event->wait();  // Blocks indefinitely

// Line 584 - Abort phase
quorum_event->wait();  // Blocks indefinitely
```

## Design

### 1. Configuration

Add to `src/deptran/config.h`:

```cpp
class Config {
  // Transaction timeout in microseconds (default: 30 seconds)
  uint64_t txn_timeout_us_ = 30 * 1000 * 1000;  // 30 seconds

public:
  uint64_t get_txn_timeout() const { return txn_timeout_us_; }
};
```

YAML config support:
```yaml
# In benchmark config file
txn_timeout_ms: 30000  # 30 seconds (will be converted to microseconds)
```

### 2. Coordinator Changes

Add timeout member to base class:

```cpp
// src/deptran/coordinator.h
class Coordinator {
protected:
  uint64_t txn_timeout_;  // Timeout in microseconds

public:
  Coordinator() {
    txn_timeout_ = Config::GetConfig()->get_txn_timeout();
  }
};
```

Modify wait calls in `src/deptran/classic/coordinator.cc`:

```cpp
// Dispatch phase (line 309)
sp_int_event->wait(txn_timeout_);
if (sp_int_event->status_.get() == Event::TIMEOUT) {
  Log_warn("Transaction %lu: Dispatch timeout", txn->id_);
  aborted_ = true;
  txn->commit_.store(false);
  GotoNextPhase();  // Go to abort/cleanup
  return;
}

// Prepare phase (line 433)
quorum_event->wait(txn_timeout_);
if (quorum_event->status_.get() == Event::TIMEOUT || !quorum_event->yes()) {
  Log_warn("Transaction %lu: Prepare timeout, voted_yes=%d/%d",
           cmd_->id_, quorum_event->n_voted_yes_, quorum_event->quorum_);
  quorum_event->timeouted_ = true;
  aborted_ = true;
}

// Commit phase (line 544) - best effort
quorum_event->wait(txn_timeout_);
if (quorum_event->status_.get() == Event::TIMEOUT) {
  Log_warn("Transaction %lu: Commit timeout, committed to %d/%d shards",
           cmd_->id_, quorum_event->n_voted_yes_, quorum_event->n_total_);
  // Continue - some shards may have committed
}

// Abort phase (line 584) - best effort
quorum_event->wait(txn_timeout_);
if (quorum_event->status_.get() == Event::TIMEOUT) {
  Log_warn("Transaction %lu: Abort timeout, aborted on %d/%d shards",
           cmd_->id_, quorum_event->n_voted_yes_, quorum_event->n_total_);
  // Continue - best effort abort
}
```

### 3. Transaction Reply Enhancement

Add timeout status to reply:

```cpp
// In appropriate header (constants.h or similar)
enum TxnResult {
  SUCCESS = 0,
  REJECT = 1,
  WRONG_LEADER = 2,
  TIMEOUT = 3,        // New: transaction timed out
  SHARD_FAILURE = 4   // New: specific shard(s) unreachable
};

// In TxReply or TxData
struct TxReply {
  TxnResult res_;
  std::vector<int> timed_out_shards_;  // Which shards didn't respond
};
```

### 4. Shard Failure Simulation

Create `src/mako/benchmarks/shard_failure_controller.h`:

```cpp
#pragma once

#include <rusty/rusty.hpp>
#include <vector>

namespace mako {

/**
 * Controller for simulating shard failures in tests.
 * Thread-safe using rusty::Cell for interior mutability.
 */
class ShardFailureController {
public:
  static constexpr int MAX_SHARDS = 16;

private:
  // Per-shard failure flag
  rusty::Cell<bool> failed_[MAX_SHARDS];

  // Singleton instance
  static ShardFailureController* instance_;

public:
  ShardFailureController() {
    for (int i = 0; i < MAX_SHARDS; i++) {
      failed_[i].set(false);
    }
  }

  // @safe - Get singleton instance
  static ShardFailureController* get() {
    if (!instance_) {
      instance_ = new ShardFailureController();
    }
    return instance_;
  }

  // @safe - Mark shard as failed
  void fail_shard(int shard_idx) {
    if (shard_idx >= 0 && shard_idx < MAX_SHARDS) {
      failed_[shard_idx].set(true);
      Log_info("ShardFailureController: Shard %d marked as FAILED", shard_idx);
    }
  }

  // @safe - Mark shard as recovered
  void recover_shard(int shard_idx) {
    if (shard_idx >= 0 && shard_idx < MAX_SHARDS) {
      failed_[shard_idx].set(false);
      Log_info("ShardFailureController: Shard %d marked as RECOVERED", shard_idx);
    }
  }

  // @safe - Check if shard is failed
  bool is_shard_failed(int shard_idx) const {
    if (shard_idx >= 0 && shard_idx < MAX_SHARDS) {
      return failed_[shard_idx].get();
    }
    return false;
  }

  // @safe - Reset all shards to healthy
  void reset() {
    for (int i = 0; i < MAX_SHARDS; i++) {
      failed_[i].set(false);
    }
  }
};

}  // namespace mako
```

### 5. Integration with RPC Layer

Modify `src/deptran/communicator.cc` to check failure controller:

```cpp
#include "mako/benchmarks/shard_failure_controller.h"

// In SendXxx methods, before actually sending:
Future* Communicator::SendPrepare(..., int shard_idx) {
  // Check if shard is simulated as failed
  if (mako::ShardFailureController::get()->is_shard_failed(shard_idx)) {
    Log_debug("Skipping RPC to failed shard %d", shard_idx);
    // Don't send - the quorum event will timeout
    return nullptr;  // Or return a dummy future that never completes
  }

  // Normal RPC send
  ...
}
```

Alternative approach - check in scheduler on receiving side:

```cpp
// In scheduler, when receiving request:
void Scheduler::OnPrepare(...) {
  int my_shard = Config::GetConfig()->get_shard_id();
  if (mako::ShardFailureController::get()->is_shard_failed(my_shard)) {
    Log_debug("Shard %d is failed, dropping request", my_shard);
    return;  // Don't process, don't reply
  }
  // Normal processing
  ...
}
```

## Test Plan

### Test 1: Basic Timeout Test

```cpp
// test/deptran/txn_timeout_test.cpp

TEST(TxnTimeout, BasicTimeout) {
  // 1. Start 2-shard single-process mode
  // 2. Run normal transactions - should succeed
  // 3. Fail shard 1
  ShardFailureController::get()->fail_shard(1);

  // 4. Run cross-shard transaction
  auto result = RunCrossShardTxn(shard0, shard1);

  // 5. Verify timeout
  EXPECT_EQ(result.res_, TIMEOUT);
  EXPECT_TRUE(result.timed_out_shards_.contains(1));

  // 6. Verify single-shard txns still work
  auto result2 = RunSingleShardTxn(shard0);
  EXPECT_EQ(result2.res_, SUCCESS);
}
```

### Test 2: Partial Commit Test

```cpp
TEST(TxnTimeout, PartialCommit) {
  // 1. Start cross-shard transaction
  // 2. Let dispatch complete on both shards
  // 3. Fail shard 1 before prepare
  ShardFailureController::get()->fail_shard(1);

  // 4. Transaction should timeout at prepare
  // 5. Shard 0 should be in consistent state (not hung)
}
```

### Test 3: Recovery Test

```cpp
TEST(TxnTimeout, Recovery) {
  // 1. Fail shard 1
  ShardFailureController::get()->fail_shard(1);

  // 2. Run transactions - should timeout
  auto result1 = RunCrossShardTxn();
  EXPECT_EQ(result1.res_, TIMEOUT);

  // 3. Recover shard 1
  ShardFailureController::get()->recover_shard(1);

  // 4. Transactions should work again
  auto result2 = RunCrossShardTxn();
  EXPECT_EQ(result2.res_, SUCCESS);
}
```

### Test Script

Create `examples/test_shard_failure_timeout.sh`:

```bash
#!/bin/bash

# Test shard failure timeout handling

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"

echo "Testing shard failure timeout..."

# Start 2-shard single-process mode with short timeout
CMD="./${BUILD_DIR:-build}/dbtest \
    --num-threads 4 \
    --shard-config src/mako/config/local-shards2-warehouses4.yml \
    -P localhost \
    -L 0,1 \
    --txn-timeout-ms 5000 \
    --fail-shard-after 10:1"  # Fail shard 1 after 10 seconds

nohup $CMD > test_shard_failure.log 2>&1 &
PID=$!

# Wait for failure to occur and transactions to timeout
sleep 20

kill $PID 2>/dev/null

# Check results
if grep -q "Shard 1 marked as FAILED" test_shard_failure.log && \
   grep -q "timeout" test_shard_failure.log; then
    echo "SUCCESS: Shard failure and timeout detected"
    exit 0
else
    echo "FAILURE: Expected timeout behavior not found"
    tail -30 test_shard_failure.log
    exit 1
fi
```

## Implementation Order

1. **Task 1** (Config): Add `txn_timeout_us_` to config (~50 LOC)
2. **Task 2** (Coordinator): Add timeout to wait() calls (~100 LOC)
3. **Task 3** (Reply): Add TIMEOUT status to reply (~50 LOC)
4. **Task 4** (Simulation): Create ShardFailureController (~150 LOC)
5. **Task 5** (Tests): Write tests using multi-shard framework (~300 LOC)

**Total estimated**: ~650 LOC

## Key Implementation Notes

1. **Timeout unit**: `Event::wait()` takes timeout in **microseconds**
   - 30 seconds = `30 * 1000 * 1000` = 30,000,000 microseconds

2. **Checking timeout**: After `wait()` returns:
   ```cpp
   if (event->status_.get() == Event::TIMEOUT) {
     // Handle timeout
   }
   ```

3. **QuorumEvent timeout**: Also check `!quorum_event->yes()` to see if quorum was reached

4. **Best-effort semantics**: For timed-out transactions:
   - No consistency guarantees
   - Some shards may have committed, others may not
   - This is acceptable per requirements

5. **Shard failure simulation**: Works by either:
   - Not sending RPCs to "failed" shards (sender-side)
   - Dropping requests at "failed" shards (receiver-side)
   - Either way, the QuorumEvent won't get enough votes and will timeout

## References

- Event system: `src/rrr/reactor/event.h`, `src/rrr/reactor/event.cc`
- Reactor timeout handling: `src/rrr/reactor/reactor.cc` (lines 246-276)
- Coordinator: `src/deptran/classic/coordinator.cc`
- QuorumEvent: `src/rrr/reactor/quorum_event.h`
- Multi-shard test: `examples/test_multi_shard_single_process.sh`
