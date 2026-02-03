# Transaction Timeout and Shard Failure Handling

## Status: IMPLEMENTED

This document describes the transaction timeout feature that allows transactions to complete with an error state if shards fail or become unreachable, preventing the system from blocking indefinitely.

## Implementation Summary

### Completed Tasks

1. **Task 1: Timeout Configuration** - DONE
   - Added `txn_timeout_us_` to `Config` class (default: 30 seconds)
   - Added `get_txn_timeout()` getter method
   - Added YAML config support via `txn_timeout_ms` field

2. **Task 2: Coordinator Wait Calls** - DONE
   - Modified 4 wait() calls in `CoordinatorClassic` to use timeout
   - Added timeout handling with logging and abort flag

3. **Task 3: Timeout Status** - DONE
   - Added `TXN_TIMEOUT = -30` constant
   - Added `timed_out_` flag to `TxReply`

4. **Task 4: Shard Failure Simulation** - DONE
   - Created `ShardFailureController` class
   - Thread-safe using `std::atomic<bool>`
   - Global controller pointer for test integration

5. **Task 5: Unit Tests** - DONE
   - Created 9 unit tests for ShardFailureController
   - All tests passing

## Design

### 1. Configuration

Added to `src/deptran/config.h`:

```cpp
// Transaction timeout configuration
// Default: 30 seconds (30,000,000 microseconds)
uint64_t txn_timeout_us_ = 30000000;

// Getter
uint64_t get_txn_timeout() const { return txn_timeout_us_; }
```

YAML config support in `LoadModeYML()`:
```yaml
mode:
  txn_timeout_ms: 30000  # 30 seconds (optional)
```

### 2. Coordinator Changes

Added timeout member to `Coordinator` base class:

```cpp
// Transaction timeout in microseconds (from config, default 30 seconds)
uint64_t txn_timeout_{30000000};
```

Modified wait calls in `CoordinatorClassic` (4 locations):

```cpp
// Example: DispatchAsync
sp_int_event->wait(txn_timeout_);
if (sp_int_event->status_.get() == Event::TIMEOUT) {
  Log_warn("Transaction %lu: DispatchAsync timed out after %lu us",
           (unsigned long)cmd_->id_, (unsigned long)txn_timeout_);
  aborted_ = true;
  tx_data().reply_.timed_out_ = true;
  tx_data().reply_.res_ = TXN_TIMEOUT;
}
```

Similar patterns for:
- `Prepare()` - aborts on timeout
- `Commit()` - best-effort (logs warning, continues)
- `Abort()` - best-effort (logs warning, continues)

### 3. Timeout Status

Added to `src/deptran/constants.h`:
```cpp
#define TXN_TIMEOUT (-30)  // Transaction timed out
```

Added to `TxReply` in `src/deptran/procedure.h`:
```cpp
// Timeout flag - set when transaction timed out
bool timed_out_ = false;
```

### 4. Shard Failure Simulation

Created `src/mako/benchmarks/shard_failure_controller.h`:

```cpp
class ShardFailureController {
private:
    std::vector<std::unique_ptr<std::atomic<bool>>> shard_failed_;
    size_t num_shards_;

public:
    explicit ShardFailureController(size_t num_shards);
    void fail_shard(size_t shard_idx);
    void recover_shard(size_t shard_idx);
    bool is_shard_failed(size_t shard_idx) const;
    void fail_all_shards();
    void recover_all_shards();
    size_t failed_shard_count() const;
};

// Global pointer for test integration
inline ShardFailureController* g_shard_failure_controller = nullptr;
inline bool is_shard_failed(size_t shard_idx);
```

## Usage

### Configuration (YAML)

```yaml
mode:
  cc: mako
  ab: paxos
  txn_timeout_ms: 30000  # 30 second timeout (optional)
```

### Shard Failure Simulation (Test Code)

```cpp
#include "mako/benchmarks/shard_failure_controller.h"

// Create controller
janus::ShardFailureController controller(num_shards);
janus::g_shard_failure_controller = &controller;

// Fail a shard
controller.fail_shard(1);

// Check if shard is failed
if (janus::is_shard_failed(1)) {
    // Skip RPC or handle failure
}

// Recover shard
controller.recover_shard(1);

// Clean up
janus::g_shard_failure_controller = nullptr;
```

## Files Changed

| File | Change |
|------|--------|
| `src/deptran/config.h` | Added `txn_timeout_us_`, `get_txn_timeout()` |
| `src/deptran/config.cc` | Added YAML loading for `txn_timeout_ms` |
| `src/deptran/coordinator.h` | Added `txn_timeout_` member |
| `src/deptran/coordinator.cc` | Initialize `txn_timeout_` from config |
| `src/deptran/classic/coordinator.cc` | Added timeout to 4 wait() calls |
| `src/deptran/constants.h` | Added `TXN_TIMEOUT = -30` |
| `src/deptran/procedure.h` | Added `timed_out_` to `TxReply` |
| `src/mako/benchmarks/shard_failure_controller.h` | New file |
| `test/deptran/txn_timeout_test.cc` | New test file (9 tests) |
| `CMakeLists.txt` | Added test_txn_timeout target |

## Future Work

1. **Deep Communicator Integration**: Integrate ShardFailureController with Communicator to automatically skip RPCs to failed shards
2. **Shard Timeout Metrics**: Track per-shard timeout statistics
3. **Signal-based Failure Control**: Add signal handlers (USR1/USR2) for runtime failure injection
4. **Integration Tests**: Add CI test for shard failure scenarios

## Key Implementation Notes

1. **Timeout unit**: `Event::wait()` takes timeout in **microseconds**
   - 30 seconds = `30 * 1000 * 1000` = 30,000,000 microseconds

2. **Checking timeout**: After `wait()` returns:
   ```cpp
   if (event->status_.get() == Event::TIMEOUT) {
     // Handle timeout
   }
   ```

3. **Best-effort semantics**: For timed-out transactions during Commit/Abort:
   - Logs warning but continues
   - Some shards may have committed, others may not
   - Timeout flag is set for informational purposes

4. **Thread-safety**: ShardFailureController uses `std::atomic<bool>` via `std::unique_ptr` per shard
