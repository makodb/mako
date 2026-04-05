# Snapshot Design for Mako RocksDB Interface

## Feasibility Analysis

Mako uses OCC (Optimistic Concurrency Control) via Sto/Masstree. Key constraints:

1. **One transaction per thread**: `TThread::txn` is a thread-local pointer. Only one transaction
   may be active on a thread at any time. Starting a second transaction while one is active is
   undefined behaviour.

2. **No multi-version storage**: Masstree is a single-version, in-place-update index. When a key
   is overwritten, the old value is physically destroyed. There is no LSN, no MVCC chain, and no
   way to read a historical version after newer writes have committed.

3. **OCC read-set validation**: Reads are recorded in a thread-local read-set. At commit time the
   system validates that nothing in the read-set was modified since it was read. This provides
   *intra-transaction* snapshot semantics but not *cross-transaction* snapshots.

### Options considered

| Option | Description | Verdict |
|--------|-------------|---------|
| **A – Eager buffer** | Scan a table into memory inside a short read transaction; serve all snapshot reads from the buffer. | ✅ **Chosen** |
| **B – Lazy / txn-held** | Keep a read transaction open for the lifetime of the snapshot. | ❌ Ruled out: the one-txn-per-thread constraint means the holding thread cannot do *anything* else for the snapshot's lifetime. |
| **C – Copy-on-read** | Begin+commit a micro-transaction per key the first time it is accessed. Cache the result. | ❌ Rejected: different keys are captured at different wall-clock times → NOT a true snapshot. Misleads callers. |

## Chosen Approach: Option A with Per-Table Lazy Buffering

### Description

`EagerSnapshot` defers scanning to first access per table:

1. **`GetSnapshot()`**: Allocate an `EagerSnapshot`, record a monotonically increasing snapshot
   sequence number, and store a back-pointer to the `mako::DB` and its table map. Cost: O(1).

2. **First access to a table** (via `Get`, `Exists`, or `NewIterator`): Begin a read-only
   transaction, `Scan` the entire table, buffer all `(key, value)` pairs into a
   `std::map<std::string, std::string>`, and commit. Subsequent accesses to the same table serve
   directly from the buffer without touching Masstree.

3. **`ReleaseSnapshot()`**: Delete the `EagerSnapshot`; all buffers are freed.

### Correctness guarantee

For each table, all keys reflect the state of that table at the moment the table was **first
accessed through this snapshot** — not at `GetSnapshot()` time. If two tables are accessed at
different moments, concurrent writes between those moments can cause the snapshot to see an
inconsistent cross-table state.

This is weaker than RocksDB's global sequence-number snapshot (which is consistent across all
column families at the *same* LSN). It is documented clearly as a **per-table point-in-time**
snapshot.

### Trade-offs

| Property | RocksDB Snapshot | Mako EagerSnapshot |
|----------|------------------|--------------------|
| Creation cost | O(1), atomic | O(1) creation, O(N) first access per table |
| Memory cost | O(0) (immutable SST files stay alive) | O(N) per buffered table |
| Cross-table consistency | Yes (global LSN) | No (per-table, lazy) |
| Reads after creation | Always return captured values | Returns values captured at first table access |
| Writer interference | None (immutable files) | None (buffer is immutable after scan) |
| Thread affinity | Any thread | Any thread (scan uses caller's thread context) |

### Limitations vs RocksDB

1. **Not globally consistent**: Cross-table reads may see slightly different points in time.
2. **O(N) memory**: All values for a table are held in RAM for the snapshot's lifetime.
3. **O(N) first access latency**: The first read to a given table triggers a full table scan.
4. **No sequence-number ordering**: Snapshots have a monotonic counter (not tied to any global
   transaction order) for identification purposes only.

## What Would Be Needed for True MVCC Snapshots

True MVCC in Mako requires changes at the Masstree/Sto level:

1. **Masstree node versioning**: Each node must store a list of `(version, value)` pairs rather
   than a single current value. Version could be a global transaction timestamp or LSN.

2. **Global timestamp oracle**: A monotonically increasing counter that increments on every
   committed write, analogous to RocksDB's sequence number.

3. **Garbage collection**: Old versions must be GC'd when no live snapshot references them.
   Similar to MVCC in PostgreSQL or RocksDB's LSM compaction.

4. **Transaction integration**: `TThread::txn` must carry a "read timestamp" and the Masstree
   lookup path must filter out versions newer than that timestamp.

None of these are small changes. The EagerSnapshot approach is the pragmatic correct solution
for now.

## Implementation Details

### OCC Abort Handling

`commit_txn` (in `mbta_wrapper.hh`) throws `abstract_db::abstract_abort_exception` if:
- `!Sto::in_progress()` — no active transaction (should not happen in practice)
- `!Sto::try_commit()` — OCC validation failed (e.g., epoch advancer races with read set)

`EagerSnapshot::ensure_buffered` catches these exceptions and retries up to 20 times. In
practice, a pure read-only scan transaction committing on a single thread should succeed on
the first or second attempt.

### Key Invariant: Buffer Isolation

Once a table's buffer is populated, it is never modified. All subsequent reads for that
table are served purely from `std::map<std::string, std::string>`. Concurrent writers cannot
affect an already-populated buffer.

### Value Encoding

The Masstree scan callback strips `mako::EXTRA_BITS_FOR_VALUE` bytes from each value before
passing it to the callback. Thus values in the snapshot buffer are stripped values (same as
what `LocalTable::Get` returns).

### Test Results

All 11 snapshot tests pass (SN1.5 Tests 1–5, SN1.6 Tests 6–8, SN1.7 Tests 9–11).
All existing CI tests pass with no regression.

## Implementation Files

- `src/mako/snapshot.hh` — `SnapshotIterator` + `EagerSnapshot` implementation
- `src/mako/idb.hh` — `ISnapshot` interface + `GetSnapshot`/`ReleaseSnapshot` on `IDatabase`
- `src/mako/db.hh` — `DB::GetSnapshot()` / `DB::ReleaseSnapshot()` implementation
- `src/mako/remote_db.hh` — inherits default `GetSnapshot()` returning `nullptr`
- `examples/snapshotTest.cc` — 11 comprehensive tests (SN1.5, SN1.6, SN1.7)
- `docs/rocksdb_interface.md` — API reference, usage examples, limitations table
- `CMakeLists.txt` — `add_apps(snapshotTest examples/snapshotTest.cc)`
