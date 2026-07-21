# Correctness Testing Analysis

## Testing Strategy

The correctness test suite uses the **makoCon** Redis-compatible server as the test interface.
This was chosen because:

1. Python `redis` library provides clean, binary-safe client API
2. Multi-connection testing is natural (each `redis.Redis()` is a new TCP connection)
3. MULTI/EXEC/DISCARD maps directly to transaction semantics
4. No C++ compilation needed for test development

## Key Architectural Findings

### Value Encoding Transparency

`mako::Encode()` appends ~20 bytes of metadata to each value, but `mbta_sharded_ordered_index::Get()`
strips this transparently. Redis clients see only the user-provided bytes.

### OCC Read-Your-Own-Writes Limitation

Masstree's OCC (via Sto) defers writes to commit time. Within a single MULTI/EXEC transaction:
- SET creates a pending write in the write buffer
- GET reads from the main Masstree index (not the write buffer)
- At commit, the validator detects a conflict: GET saw "key not found" but the write set adds the key
- Result: OCC abort (EXEC returns `*-1`)

Workaround: Separate writes (MULTI/EXEC) from reads (auto-committed GET).

### Growing-Value Overwrite Bug (Found and Fixed)

**File:** `src/mako/sto/MassTrans.hh`

When a key was overwritten with a larger value crossing an allocation boundary,
the Sto OCC transaction aborted due to a stale TransItem:

1. `handlePutFound()` line 735: observed version of OLD versioned_value location `e`
2. `reallyHandlePutFound()` line 666: marked `e->version() |= invalid_bit`
3. Line 673: created NEW location via `resizeIfNeeded()`
4. Line 697: created NEW TransItem keyed by `new_location`
5. OLD TransItem (keyed by `e`) remained in transaction set with stale read
6. At commit: `check()` → `validityCheck(e)` → `e->version() & invalid_bit` → ABORT

**Fix applied:** Moved `observe()` from before `reallyHandlePutFound()` to after it.
After the call, `item` points to the new location (if resized), so the observation
records the correct version. The old TransItem has no read observation, so
`Transaction.cc:538` (`it->has_read()`) skips it during commit validation.

### Auto-Commit Isolation Model

Each non-MULTI command is auto-committed as its own single-operation transaction.
This provides:
- No dirty reads (each command is instantly committed)
- Immediate visibility (committed = visible)
- Last-writer-wins for concurrent writes
- NO cross-key invariant protection (write skew possible)

### Persistence Model

makoCon uses in-memory Masstree without RocksDB. All data is volatile.
For persistence testing, use `dbtest` with RocksDB configuration.
