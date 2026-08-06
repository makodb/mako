# Raft Persistence Tests

> **Note**: The files referenced below (`raft_persistence.h`, `raft_persistence.cc`, `test_raft_persistence.cc`) were removed from the codebase. This document is kept as a design reference for the persistence layer architecture and test approach.

## Overview

This document describes the RocksDB-based persistence layer for Raft and its test suite.

## Files (historical)

- **`raft_persistence.h`** - RaftPersistence class interface
- **`raft_persistence.cc`** - Implementation with RocksDB integration
- **`test_raft_persistence.cc`** - Unit test suite (10 tests)

## How to Run Tests

### Prerequisites

1. **Enable MAKO_USE_RAFT flag:**
   ```bash
   cmake . -DMAKO_USE_RAFT=ON
   ```

2. **Build the test executable:**
   ```bash
   make test_raft_persistence -j8
   ```

3. **Run the tests:**
   ```bash
   ./test_raft_persistence
   ```

### Expected Output

```
================================
Raft Persistence Unit Tests
================================
Test 1: Basic Initialization... PASSED
Test 2: Persist and Load Term... PASSED
Test 3: Persist and Load VotedFor... PASSED
Test 4: Load Defaults (Empty Database)... PASSED
Test 5: Persist and Load Single Log Entry... PASSED
Test 6: Persist and Load Multiple Log Entries... PASSED
Test 7: Load Log Range... PASSED
Test 8: Update Term Multiple Times... PASSED
Test 9: Persist Commit Index... PASSED
Test 10: Multiple Site IDs (Isolation)... PASSED
================================
Results: 10 passed, 0 failed
================================
```

## Test Descriptions

### Test 1: Basic Initialization
**Purpose:** Verify RaftPersistence can initialize and create database directories.

**What it tests:**
- `RaftPersistence::Init()` returns true
- Database directory is created at `/tmp/test_raft_persistence_1/raft_0_partition_0/`
- RocksDB opens successfully with column families

**Why it matters:** Ensures the persistence layer can bootstrap from scratch.

---

### Test 2: Persist and Load Term
**Purpose:** Verify currentTerm persists across database close/reopen.

**What it tests:**
- Write term=42 with `PersistTerm(42)`
- Close database (destroy RaftPersistence object)
- Reopen database with new RaftPersistence instance
- Read term with `LoadTerm()` returns 42

**Why it matters:** Term is critical for Raft safety - must survive crashes. This is the "voted in term X" guarantee.

---

### Test 3: Persist and Load VotedFor
**Purpose:** Verify votedFor persists across database close/reopen.

**What it tests:**
- Write votedFor=5 with `PersistVotedFor(5)`
- Close and reopen database
- Read votedFor with `LoadVotedFor()` returns 5

**Why it matters:** Prevents voting twice in the same term (Raft safety requirement). Must survive crashes.

---

### Test 4: Load Defaults (Empty Database)
**Purpose:** Verify correct default values when no state has been persisted.

**What it tests:**
- Open fresh database (no previous writes)
- `LoadTerm()` returns 0 (default initial term)
- `LoadVotedFor()` returns INVALID_SITEID (uint32_t(-1))

**Why it matters:** First startup should have sensible defaults that don't violate Raft invariants.

---

### Test 5: Persist and Load Single Log Entry
**Purpose:** Verify a single RaftData log entry persists correctly.

**What it tests:**
- Create RaftData entry with term=10, slot_id=1
- Persist with `PersistLogEntry(1, entry)`
- Close and reopen database
- Load all logs with `LoadAllLogs()`
- Verify 1 entry loaded with correct term and slot_id

**Why it matters:** Log persistence is core to Raft - entries must survive crashes to maintain state machine consistency.

---

### Test 6: Persist and Load Multiple Log Entries
**Purpose:** Verify multiple log entries persist and are recovered in order.

**What it tests:**
- Create 10 log entries (slot_id 1-10, term = i*10)
- Persist all entries
- Close and reopen database
- Load all logs
- Verify all 10 entries present with correct terms and slot_ids

**Why it matters:** Real Raft servers have many log entries. Tests bulk persistence and recovery.

---

### Test 7: Load Log Range
**Purpose:** Verify selective log loading by slot ID range.

**What it tests:**
- Persist 10 log entries (slot_id 1-10)
- Load range [3, 7] with `LoadLogRange(3, 7, logs)`
- Verify exactly 5 entries returned (slots 3, 4, 5, 6, 7)
- Verify correct slot_ids for each entry

**Why it matters:** Efficient log transfer during catch-up - don't need to load entire log, just the range needed.

---

### Test 8: Update Term Multiple Times
**Purpose:** Verify term updates overwrite previous values correctly.

**What it tests:**
- Update term 5 times (term = 1, 2, 3, 4, 5)
- After each update, read term back
- Verify each read returns the most recent value

**Why it matters:** Term increases monotonically in Raft. Must correctly overwrite old values, not accumulate.

---

### Test 9: Persist Commit Index
**Purpose:** Verify metadata persistence (commit index).

**What it tests:**
- Persist commit_index=42 with `PersistCommitIndex(42)`
- Verify method returns true (no errors)

**Why it matters:** Commit index tracking for optimization. Uses async writes (non-critical for safety).

---

### Test 10: Multiple Site IDs (Isolation)
**Purpose:** Verify different Raft servers have isolated databases.

**What it tests:**
- Create persistence for site_id=0, persist term=100
- Create persistence for site_id=1, persist term=200
- Reopen site_id=0, verify term=100 (not affected by site_id=1)
- Reopen site_id=1, verify term=200 (not affected by site_id=0)

**Why it matters:** Multi-server deployments (multiple partitions, shards) must not interfere with each other's state.

---

## Database Structure

### Directory Layout
```
/tmp/raft_{site_id}_partition_{partition_id}/
├── CURRENT
├── MANIFEST-000001
├── 000003.sst (state column family)
├── 000004.sst (logs column family)
└── 000005.sst (meta column family)
```

### Column Families

**1. "state" Column Family:**
- Key: `"term"` → Value: uint64_t currentTerm
- Key: `"voted_for"` → Value: uint32_t votedFor
- **Write Options:** Synchronous (`sync=true`) for durability

**2. "logs" Column Family:**
- Key: `"log:0000000000000001"` → Value: Serialized RaftData for slot 1
- Key: `"log:0000000000000002"` → Value: Serialized RaftData for slot 2
- ...
- **Write Options:** Synchronous (`sync=true`) for durability

**3. "meta" Column Family:**
- Key: `"commit_index"` → Value: uint64_t commitIndex
- **Write Options:** Asynchronous (`sync=false`) for performance

### Serialization Format (RaftData)

Current implementation serializes primitive fields only:
```
| max_ballot_seen_ (8 bytes) |
| max_ballot_accepted_ (8 bytes) |
| term (8 bytes) |
| prevTerm (8 bytes) |
| slot_id (8 bytes) |
| ballot (8 bytes) |
```

**Note:** Marshallable pointers (`accepted_cmd_`, `committed_cmd_`, `log_`) are not yet serialized. This is sufficient for testing primitive Raft state.

## Persistence Guarantees

### What Survives Crashes
✅ **currentTerm** - Prevents term regression
✅ **votedFor** - Prevents double voting
✅ **Log entries** - Maintains state machine consistency
✅ **Commit index** - (eventually, async)

### Raft Safety Properties Preserved
1. **Election Safety:** votedFor prevents voting twice in same term
2. **Leader Append-Only:** Log entries persist, can't be lost
3. **Log Matching:** Persisted logs maintain term/index invariants
4. **State Machine Safety:** Committed entries survive crashes

### Write Durability
- **Synchronous writes** (`sync=true`): Flushed to disk before returning
  - Used for: term, votedFor, log entries
  - Guarantees: Survives power loss, kernel crashes

- **Asynchronous writes** (`sync=false`): Buffered in memory/page cache
  - Used for: commit_index (optimization)
  - Guarantees: Survives process crashes, but not power loss

## Cleaning Up Test Data

Tests automatically clean up after themselves. Manual cleanup if needed:

```bash
# Remove all test databases
rm -rf /tmp/test_raft_persistence_*

# Remove production databases (CAUTION: loses all state!)
rm -rf /tmp/raft_*
```

## Integration with RaftServer

The standalone `RaftPersistence` test helper is not the production integration
boundary. `RaftServer` uses its `LogStorage` and snapshot-manager interfaces
for log/state recovery and persistence, while term/vote and commit ordering are
maintained by the server orchestration paths. Do not add a second persistence
owner without first reconciling those interfaces.

Remaining work is coverage-oriented: add tests that exercise crash/restart
recovery through the production `RaftServer` and `LogStorage` stack, including
term/vote restoration, committed-log replay, and snapshot boundary cases.



## Performance Considerations

### Current Implementation (Simple)
- Direct RocksDB puts for each operation
- Synchronous writes for safety
- No batching

### Future Optimizations
1. **Batch writes:** Group multiple log entries into single RocksDB WriteBatch
2. **WAL tuning:** Adjust RocksDB write-ahead log settings
3. **Compaction:** Tune LSM-tree compaction for Raft access patterns
4. **Snapshots:** Add snapshot support for log compaction

## Troubleshooting

### Test fails with "No such file or directory"
**Cause:** Parent directory not created
**Fix:** Init() method creates directory with `mkdir -p`, check permissions

### Test fails with RocksDB errors
**Cause:** RocksDB library not linked
**Fix:** Ensure `target_link_libraries(deptran_server rocksdb)` in CMakeLists.txt

### Segmentation fault
**Cause:** Usually null pointer in RaftData deserialization
**Fix:** Check that RaftData fields are initialized before use

### Wrong values loaded
**Cause:** Serialization/deserialization mismatch
**Fix:** Verify field order matches in SerializeRaftData/DeserializeRaftData

## References

- **Raft Paper:** "In Search of an Understandable Consensus Algorithm" (Diego Ongaro, John Ousterhout)
- **Raft Persistence Requirements:** Section 5.2 - "If a server crashes, it must restore persistent state"
- **RocksDB Documentation:** https://github.com/facebook/rocksdb/wiki
