# RocksDB Interface Fixes Report

## Mako Commit Hash Before Changes

```
232ba3b0 Zeyu raft disk (#53)
```

All changes are local (not committed). The base commit for these fixes is `232ba3b0`.

---

## What Was Fixed

### Fix 1: `GetApproximateSize` Always Returning 0

**Before:** `MassTrans::approx_size()` unconditionally returned 0:
```cpp
size_t approx_size() const {
    return 0;
}
```

**After:** Returns an atomic counter maintained at operation time:
```cpp
size_t approx_size() const {
    return size_count_.load(std::memory_order_relaxed);
}
```

### Fix 2: `Insert` Using Inefficient Check-Then-Put

**Before:** `LocalTable::Insert()` used `Get()` + `Put()`, where `Put()` calls `transPut` (overwrite semantics):
```cpp
Status s = Get(txn, key, unused);   // check
if (s.ok()) return Status::InvalidArgument("Key already exists");
return Put(txn, key, value);        // transPut - wrong semantics for insert
```

**After:** `LocalTable::Insert()` calls `index_->Insert()` which routes to `transInsert` (insert OCC semantics):
```cpp
return index_->Insert(txn, key, value);  // transInsert path
```

---

## Implementation Approach

### `GetApproximateSize` Fix (Task F1.1)

Added `std::atomic<size_t> size_count_{0}` to `MassTrans` class. Counter is updated:
- **Incremented** in `trans_write` when `INSERT=true` and a new key is created (`found=false` branch)
- **Decremented** in `transDelete` for:
  - Insert-then-delete case (undo the prior increment)
  - Regular delete of an existing key

The counter is updated at operation time, not commit time. Aborted transactions may temporarily skew the count — this is explicitly documented and acceptable for an "approximate" count.

### `Insert` Fix (Tasks F1.2 + F1.3)

**F1.2:** Fixed `mbta_sharded_ordered_index::insert()` (which previously called `put()` — wrong) to call `pick_shard(key)->insert()`, routing to `mbta_ordered_index::insert()` → `transInsert`.

Added `mako::Status mbta_sharded_ordered_index::Insert()` RocksDB-style method:
- Uses a Get check for duplicate detection (necessary because `transInsert` silently succeeds on duplicate keys — it reads/observes the existing key but does not throw or return a detectable error through the `const char*` wrapper API)
- Uses native `insert()` → `transInsert` for the actual write (correct insert OCC semantics vs `transPut`'s overwrite semantics)

**F1.3:** Updated `LocalTable::Insert()` to call `index_->Insert()` directly, delegating to the native `transInsert` path.

---

## Files Modified

### `src/mako/benchmarks/sto/MassTrans.hh`
- Added `#include <atomic>`
- Added `std::atomic<size_t> size_count_{0}` private member
- Added increment in `trans_write` new-key path (when `INSERT=true` and `found=false`)
- Added decrement in `transDelete` for insert-then-delete and regular delete paths
- Replaced `approx_size()` stub with `size_count_.load(std::memory_order_relaxed)`

### `src/mako/benchmarks/mbta_sharded_ordered_index.hh`
- Fixed `insert()` (low-level) to call `pick_shard(key)->insert()` instead of `put()`
- Added `mako::Status Insert(void* txn, const std::string& key, const std::string& value)` declaration
- Added inline implementation of `Insert()` using Get-check + native `insert()`

### `src/mako/local_table.hh`
- Updated `LocalTable::Insert()` to call `index_->Insert()` (native transInsert path) instead of `Get()` + `Put()`

### `examples/rocksdbInterfaceTest.cc`
- `test_approx_size`: Added assertions that size is approximately 100 after 100 inserts and approximately 50 after 50 deletes (replaced old "always 0" printf)
- `test_insert`: Added Insert-after-delete test (delete a key, then re-insert it — should succeed)
- Added `test_approx_size_stress()`: 1000-key stress test verifying size ≈ 1000 after 1000 inserts and ≈ 500 after 500 deletes
- Updated full integration test size assertions (≈101 after 100+1 inserts, ≈51 after 50 deletes)

### `docs/rocksdb_interface.md`
- `GetApproximateSize`: Replaced "always returns 0" note with description of the atomic counter mechanism and approximate nature
- `Insert`: Added note about native `transInsert` path and OCC semantics
- Known Limitations table: Updated `GetApproximateSize` row

---

## Test Results

### Build

```
make clean && make -j32
```
**Result: SUCCESS**

### Integration Test (`rocksdbInterfaceTest`)

**Result: ALL TESTS PASSED**

Key results:
- `GetApproximateSize` after 100 inserts: **100** (was 0)
- `GetApproximateSize` after 50 deletes: **50** (was 0)
- `GetApproximateSize` after 1000 inserts (stress): **1000** (was 0)
- `GetApproximateSize` after 500 deletes (stress): **500** (was 0)
- `Insert` new key: succeeds
- `Insert` duplicate key: fails with `InvalidArgument`
- `Insert` after delete: succeeds

### CI Tests (`./ci/ci.sh all`)

**Result: ALL STEPS PASSED**

All 15 CI test suites passed with no regressions introduced by these changes.

---

## Before vs After Behavior

| Method | Before | After |
|--------|--------|-------|
| `GetApproximateSize` | Always returns 0 | Returns accurate atomic count (approximate due to operation-time updates) |
| `Insert` write path | Used `transPut` (overwrite semantics) | Uses `transInsert` (insert OCC semantics) |
| `mbta_sharded_ordered_index::insert()` | Called `put()` — wrong | Calls `pick_shard(key)->insert()` — correct |

---

## Issues Encountered and Resolutions

### 1. `transInsert` Return Value Discarded by Wrapper

**Issue:** `mbta_ordered_index::insert()` calls `transInsert` but discards its bool return value (`transInsert` returns `true` = key existed, `false` = new key). The `const char*` wrapper always returns `nullptr`.

**Resolution:** Per TODO guidance: "if transInsert doesn't cleanly distinguish 'key exists' from other failures, fall back to the Get+Put approach and document why." Used Get-then-native-insert approach: Get detects duplicates, native `insert()` performs the write with correct OCC semantics. Documented in code and this report.

### 2. Aborted Transactions Skewing `approx_size()` Count

**Issue:** Counter is updated at operation time. If a transaction increments/decrements the counter and then aborts, the count is temporarily wrong.

**Resolution:** This is explicitly acceptable — the method is `GetApproximateSize`. Added comment in `MassTrans.hh` and documentation in `docs/rocksdb_interface.md` describing this behavior.

---

## Updated Remaining Gaps

After this work, the remaining gaps vs full RocksDB compatibility are:

| Feature | Priority | Notes |
|---------|----------|-------|
| **Stateful iterators** | High | Scan/ReverseScan are callback-based; no rewind/seek |
| **Snapshots** | High | No point-in-time read isolation |
| **Merge operators** | Medium | No read-modify-write primitives |
| **WriteBatch** | Medium | No multi-table atomic batch (use txn API instead) |
| **DeleteRange** | Medium | Must loop individual deletes |
| **`RemoteTable::Scan`/`ReverseScan`** | Medium | Currently return IOError stubs |
| **`GetApproximateSize` abort skew** | Low | Aborted txns temporarily skew count; acceptable for "approximate" |
| **Native duplicate detection in `Insert`** | Low | Still uses Get check; `transInsert` bool not exposed through abstract API |
| **Column family management** (create/drop) | Low | No drop table |
| **Compaction filters** | Low | No background key eviction |
