# Silo/Masstree API Unification Plan

## Goal

Give Silo's L3 layer (`abstract_ordered_index`) a **non-transactional API** whose *shape* matches Masstree's `mbtree<P>` API. Callers who want per-key-atomic access without OCC bookkeeping should be able to write code against L3 that reads almost identically to code against Masstree.

The existing transactional L3 API (`get(void* txn, k, v)` etc.) stays exactly as it is. This plan only *adds*.

## What "matching shape" means concretely

By the end of this work, every non-txn method on `abstract_ordered_index` will meet these three constraints:

1. **No transaction parameter.** Just the key and (for writes) the value.
2. **`bool` return.** No `const char*` returned-pointer affordance; no `void`.
3. **Same operation set as Masstree.** `get / put / insert / remove / scan / rscan` — a superset of what MassTrans exposes today.

Naming keeps L3's existing verbs (`get`, `put`, `insert`, `remove`) rather than Masstree's (`search`, `insert`, `insert_if_absent`, `remove`), for two reasons: (a) L3's transactional API already uses these verbs so callers don't context-switch between txn'd and non-txn'd calls on the same class, and (b) Masstree's `insert`-that-overwrites is less common than L3's `put`-overwrites + `insert`-if-absent convention. Verb-alignment with Masstree is a separate cosmetic decision, out of scope here.

## Current state, one more time

| | Masstree (L1) | STO MassTrans | L3 `abstract_ordered_index` |
|---|---|---|---|
| `get`  / `search` | `bool search(k, v)` — no txn | `bool get(Str k, v)` — implicit-txn wrap | `bool get(void* txn, k, &v)` (txn'd), `bool shard_get(k, &v)` (misnamed — actually implicit-txn via `STD_OP`+`transGet`) |
| `put`  (overwrite) | `bool insert(k, v)` — no txn, per-key atomic | `bool put(Str k, v)` — implicit-txn wrap | `const char* put(void* txn, k, v)` (txn'd) — no non-txn variant |
| `insert` (if-absent) | `bool insert_if_absent(k, v)` | none | `const char* insert(void* txn, k, v)` (txn'd) — no non-txn variant |
| `remove` | `bool remove(k, *old)` — no txn | `bool remove(Str k)` — direct raw write via `cursor_type::find_locked` | `void remove(void* txn, k)` (txn'd) — no non-txn variant |
| `scan`  | `void search_range_call(lo, hi, cb&)` — no txn, callback | none non-txn (only `transQuery`) | `void scan(void* txn, lo, hi, cb&)` (txn'd), `bool shard_scan(lo, hi, cb&)` (misnamed — actually implicit-txn via `STD_OP`+`transQuery`) |
| `rscan` | `void rsearch_range_call(...)` | none non-txn (only `transRQuery`) | `void rscan(void* txn, ...)` (txn'd) — no non-txn variant |

**Findings**:
- Only three non-txn methods exist at MassTrans today: `get`, `put`, `remove`.
- `get` and `put` are implicit-txn wraps (safe under concurrent OCC).
- `remove` is a direct raw write via masstree cursor.
- `shard_get` / `shard_scan` at L3 are **not actually non-txn** despite the comment claiming so — they wrap `transGet` / `transQuery` inside a `STD_OP` macro that starts and commits a transaction.
- No `insert` (put-if-absent), `scan`, or `rscan` non-txn variants exist at any layer.

## Design decision: consistency of non-txn write semantics

The user has explicitly confirmed it is fine to keep the current mix:
- **`get`, `put`, `insert`, `scan`, `rscan`** → implicit-txn wrap (Option B). Safe under concurrent OCC readers/writers because they go through STO's read/write-set machinery. Cost: one full txn per op.
- **`remove`** → direct raw write (Option A). Bypasses OCC r/w-set. Faster but has subtle semantics under concurrency: STO transactions with the key in their read-set may still commit successfully if the version counter STO tracks isn't bumped by the raw path.

**We accept this asymmetry as-is.** The reason for the asymmetry is historical (MassTrans's existing implementations), and reshaping it into a uniform Option A or Option C is out of scope for this task. If a future workload needs the semantics to be uniform, that's a separate follow-up.

The trade-off is documented so future readers understand: `remove` outside a transaction is fast but has subtle interaction with concurrent OCC transactions; the other non-txn ops are slower but safe.

## Plan

### Phase 1 — Extend MassTrans's non-txn API

Add three new methods to `src/mako/benchmarks/sto/MassTrans.hh` that follow the same pattern as the existing `put` / `get`:

```cpp
// New — put-if-absent (implicit-txn wrap around transInsert)
bool insert(Str key, const value_type& value,
            threadinfo_type& ti = mythreadinfo) {
    Sto::start_transaction();
    auto ret = transInsert(key, value, ti);
    Sto::commit();
    return ret;
}

// New — forward range scan (implicit-txn wrap around transQuery)
template <typename Callback>
void scan(Str begin, Str end, Callback callback,
          ValAllocator* va = nullptr,
          threadinfo_type& ti = mythreadinfo) {
    Sto::start_transaction();
    transQuery(begin, end, callback, va, ti);
    Sto::commit();
}

// New — reverse range scan (implicit-txn wrap around transRQuery)
template <typename Callback>
void rscan(Str begin, Str end, Callback callback,
           ValAllocator* va = nullptr,
           threadinfo_type& ti = mythreadinfo) {
    Sto::start_transaction();
    transRQuery(begin, end, callback, va, ti);
    Sto::commit();
}
```

Existing `get`, `put`, `remove` need no change.

**Cost**: ~1 day. Mechanical wrapper code; the hard work is in the underlying `transInsert` / `transQuery` / `transRQuery` methods which already exist and are used by the txn'd path.

### Phase 2 — Expose the non-txn API at L3

Add six virtual methods to `src/mako/benchmarks/abstract_ordered_index.h`, each with signatures matching Masstree's shape:

```cpp
class abstract_ordered_index {
public:
    // ... existing transactional API stays exactly as it is ...

    // Non-transactional per-key-atomic ops (Masstree-shape).
    // These do NOT participate in a caller's transaction. Each op is
    // internally wrapped in its own one-op OCC transaction (safe under
    // concurrent OCC readers/writers), except `remove` which does a
    // direct raw write for perf. See docs/silo-masstree-api-unification.md
    // for the semantic contract.
    virtual bool get   (lcdf::Str key, std::string& value,
                        size_t max_bytes_read = std::string::npos) = 0;
    virtual bool put   (lcdf::Str key, const std::string& value) = 0;
    virtual bool insert(lcdf::Str key, const std::string& value) = 0;
    virtual bool remove(lcdf::Str key) = 0;
    virtual void scan  (const std::string& start_key,
                        const std::string* end_key,
                        scan_callback& callback,
                        str_arena* arena = nullptr) = 0;
    virtual void rscan (const std::string& start_key,
                        const std::string* end_key,
                        scan_callback& callback,
                        str_arena* arena = nullptr) = 0;
};
```

Implement them in the two backends that matter for the compat facade path:

**`src/mako/benchmarks/mbta_wrapper.hh`** (single-shard `abstract_ordered_index` impl over MassTrans):
- Delegate directly to MassTrans's non-txn methods (`mbta.get(...)`, `mbta.put(...)`, etc.).
- `scan` / `rscan` wrap the Callback conversion (L3's `scan_callback` → MassTrans's lambda).

**`src/mako/benchmarks/mbta_sharded_ordered_index.hh`** (Mako's sharded `abstract_ordered_index` impl):
- Delegate to `pick_shard(key)->get(...)` etc. — same pattern already used by the txn'd methods.

`RemoteTable` in `src/mako/remote_db.hh` does not need to implement these (the remote client can't do non-txn ops against a remote shard — it always goes through RPC which is inherently boundary-crossing).

**Cost**: ~1 day. Wrapping and delegating; new methods on the two backends.

### Phase 3 — Clarify the role of `shard_get` / `shard_scan` / `shard_put`

**REVISED during implementation.** The original plan proposed aliasing
`shard_get`/`shard_scan` to the new non-txn methods, on the belief that
they were "implicit-txn wraps." Implementation-time inspection showed
that is wrong — and the aliasing option would have broken cross-shard
2PC:

- `STD_OP` does **not** start or commit a transaction; it only
  translates `Transaction::Abort` into `abstract_abort_exception`.
- `shard_get` calls `transGet` against the RPC-handler thread's
  **ambient** Sto transaction and adds the key to its read-set. No
  commit — the read is validated later during 2PC.
- `shard_put` calls `transPut` **and** `Sto::shard_try_lock_last_writeset()`
  — staging a write and locking its write-set entry for the 2PC
  prepare phase. The commit happens later when the coordinator drives
  it.
- `shard_scan` similarly stages into the ambient transaction.

These are the **remote-shard participants of Mako's distributed
transaction protocol**, not standalone ops. They must not be aliased
to (or reimplemented on top of) the self-contained non-txn API.

**What Phase 3 actually did** (original "Option 3"): kept
`shard_get`/`shard_scan`/`shard_put` exactly as they are, replaced the
misleading "non-transaction control" comment with accurate
cross-shard-2PC documentation, and cross-referenced the new non-txn
methods for callers who want self-contained semantics.

**Cost**: ~half day (comment corrections + this plan revision).

### Phase 4 — Tests

New unit tests for the six non-txn L3 methods:

- Round-trip: put then get returns the value.
- Insert semantics: succeeds on new key, fails on existing key.
- Overwrite semantics: put succeeds over existing.
- Remove: succeeds if present, returns false if absent.
- Scan / rscan: return keys in expected order over a bounded range.
- Interleaving safety: concurrent txn'd Put and non-txn Get see consistent snapshots (validates the implicit-txn wrap).

Test location (as implemented): `tests/test_silo_nontxn_api.cc`, following the established gtest + CTest pattern of `tests/test_silo_*.cc` rather than the standalone-binary style of `benchmarks/ut/`.

**Cost**: ~1 day.

### Phase 5 — Documentation

Two documentation updates:

1. **This plan** (`docs/silo-masstree-api-unification.md`) — landed as part of Phase 0, before code changes.
2. **`docs/rocksdb_interface.md` Conceptual Model section §3 (Transaction semantics)** — add a paragraph explaining that Silo now has a non-transactional API alongside the transactional one, mirroring Masstree's per-key shape.
3. **`docs/masstree-book.md` §8 (Mako Integration: Transactions)** — cross-link to this doc so readers exploring the masstree/silo boundary can find the non-txn path.

**Cost**: ~half day.

## Effort estimate

| Phase | Work | Days |
|---|---|---|
| 1 | Extend MassTrans (add `insert` / `scan` / `rscan` non-txn) | ~1 |
| 2 | Expose 6 virtual methods at L3, implement in `mbta_wrapper` + `mbta_sharded_ordered_index` | ~1 |
| 3a | Alias `shard_get` / `shard_scan` to non-txn `get` / `scan` | ~0.5 |
| 3b | Migrate ~10 call sites to new names, delete aliases (optional follow-up) | ~0.5 |
| 4 | Tests for new non-txn API | ~1 |
| 5 | Docs updates | ~0.5 |
| **Total** | | **~4–5 days** |

## Explicit non-goals

- **No change to `abstract_ordered_index`'s transactional API.** All existing `get(txn, ...)` / `put(txn, ...)` / etc. signatures stay exactly as they are.
- **No change to `abstract_db`'s transaction lifecycle** (`new_txn`, `commit_txn`, `abort_txn`, `open_index`).
- **No change to MassTrans's `trans*` methods.**
- **No convergence of value or key types across layers.** Masstree keeps `value_type = uint8_t*` (caller-owned raw pointer); L3 keeps `std::string` (copied on write, copied on read). The unification is at the *method-level shape*, not at the internal type system.
- **No verb-renaming pass** (e.g., Masstree `search` → `get`). That's a separate cosmetic decision, addressed independently if wanted.
- **No unification of `remove` and `put`/`get` semantics.** The accepted asymmetry (raw remove, implicit-txn-wrapped put/get) is retained as-is.
- **No non-txn API on `RemoteTable`.** Remote-shard non-txn access is inherently cross-boundary and would require its own RPC protocol; out of scope.
- **No performance guarantees.** Non-txn `get`/`put`/`insert`/`scan`/`rscan` pay full one-op txn overhead. If callers want raw non-txn perf, they should reach for the MassTrans direct API or Masstree's `mbtree` methods.

## Consumer impact

None of the current L3 consumers need code changes:

- **`server.cc`, `client_service.cc`, `simpleTransaction.cc`** — call `shard_get` / `shard_scan` which remain valid as aliases in Phase 3a. Optional migration in Phase 3b changes ~10 call sites.
- **TPC-C, YCSB benchmarks** — use L5 typed_txn_btree, not L3 directly. Unaffected.
- **`dbtest`** — uses L6 `txn_proto2_impl`. Unaffected.
- **`mako::DB` / `LocalTable` / `IDatabase`** — the RocksDB compat facade uses L3's *transactional* API, not the non-txn one. Unaffected.
- **`makoCon`** — Redis-compat server. Currently uses L3 transactionally. Could adopt non-txn methods for simple SET/GET/DEL that don't need OCC semantics. Optional post-hoc migration.

## Testing checklist

- [ ] MassTrans-level: new `insert` returns true on new key, false on existing.
- [ ] MassTrans-level: new `scan` iterates `[begin, end)` in order.
- [ ] MassTrans-level: new `rscan` iterates in reverse.
- [ ] L3-level: `mbta_wrapper` non-txn `get` reads back what `put` wrote.
- [ ] L3-level: `mbta_wrapper` non-txn `insert` fails on duplicate.
- [ ] L3-level: `mbta_wrapper` non-txn `remove` returns false on missing key.
- [ ] L3-level: `mbta_sharded_ordered_index` non-txn ops route through `pick_shard` correctly.
- [ ] Interleaving: concurrent txn'd Put and non-txn Get see consistent snapshots.
- [ ] Interleaving: non-txn remove correctly interacts with concurrent txn'd reader (documented Option A semantics).
- [ ] Backwards compat: `shard_get` and `shard_scan` still work after aliasing.
- [ ] Docker CI green after Phase 1–4.

## Migration notes for reviewers

- The non-txn methods are *additions* to a virtual interface. Any concrete backend that doesn't override them will fail to compile until it provides implementations. The two backends that need updating (`mbta_wrapper`, `mbta_sharded_ordered_index`) are updated as part of Phase 2. `RemoteTable` gets stubs that return `Status::NotSupported`-equivalent (documented as intentional).
- The `insert` semantics are put-if-absent; callers coming from RocksDB (where `db->Put` overwrites and there's no built-in insert-if-absent) should be steered to `put` unless they specifically need the exclusive-write behavior.
- Non-txn `scan` / `rscan` require the caller to provide a `str_arena` if the underlying tree needs one (matching the existing txn'd `scan` contract). The arena parameter is optional but recommended for perf.

## Related documents

- [`rocksdb_interface.md`](rocksdb_interface.md) — RocksDB-compat facade; the transactional L3 API sits under it.
- [`masstree-book.md`](masstree-book.md) — Masstree's own API and internals.
- `src/mako/benchmarks/abstract_ordered_index.h` — the interface this plan extends.
- `src/mako/benchmarks/sto/MassTrans.hh` — the runtime that Phase 1 extends.
- `src/mako/benchmarks/mbta_wrapper.hh` and `mbta_sharded_ordered_index.hh` — the two backends Phase 2 updates.
