# Unify Client Mode Test Path Plan

## Issue
ISSUE-33b02756-2

## Problem
`examples/simpleTransactionRep.cc:894-969` routes --client mode through a dedicated
`run_client_mode` path instead of using the unified IDatabase interface with the same
test flow as local mode.

## Analysis

### Current Architecture

**Local Mode (run_tests)**:
- Uses multi-threaded workers (`run_worker_tests`)
- Depends on BenchmarkConfig singleton (nthreads, shardIdx)
- Uses abstract_db specific methods:
  - `open_sharded_index()` - not in IDatabase
  - `scan_tables()` - not in IDatabase
- Tests include:
  - test_basic_transactions (simple Put/Get)
  - test_single_key_contention (multi-thread contention)
  - test_overlapping_keys (shared key ranges)
  - test_cross_shard_contention (multi-shard)
- Data verification using abstract_db methods

**Client Mode (run_client_mode)**:
- Single-threaded
- Uses RemoteDB (implements IDatabase)
- Only tests basic Put/Get/Commit
- No data verification (can't access abstract_db on server)

### Challenges for Unification

1. **API Mismatch**: abstract_db has methods not in IDatabase:
   - `open_sharded_index()` - returns raw table pointer
   - `scan_tables()` - full table scan capability
   - Worker ID management

2. **Threading Model**:
   - Local: Multi-threaded with barriers
   - Remote: Single connection, could add parallelism but adds complexity

3. **Data Verification**:
   - Local: Can scan tables directly
   - Remote: Would need new RPC for verification

### Proposed Solution

Break into smaller tasks:

**Task 1: Create unified simple test function** (~100 LOC)
- Extract basic Put/Get/Commit test into `run_simple_test(IDatabase*)`
- Works for both local and remote
- Replace run_client_mode's inline test code

**Task 2: Add multi-key test to unified interface** (~150 LOC)
- Test multiple keys with single thread
- Works for both local and remote

**Task 3 (Optional): Add remote scan capability** (~200 LOC)
- Add RPC for table scan to RemoteDB
- Enables data verification in client mode

For now, Task 1 is sufficient to address the issue - it uses IDatabase interface
and avoids code duplication.

## Implementation Plan (Task 1)

1. Create `run_simple_test(IDatabase* db)` function:
   - Basic Put/Get/Commit test
   - 5 key-value pairs with unique keys
   - Verify retrieved values match

2. Modify `run_client_mode()`:
   - Call `run_simple_test(remote_db)` instead of inline test code

3. Optionally call `run_simple_test(mako_db)` in local mode too

### Estimated LOC: ~100 lines changed

## Verification
- Run ./ci/ci.sh clientServer
- Run ./ci/ci.sh simpleTransaction
- Run ./ci/ci.sh all
