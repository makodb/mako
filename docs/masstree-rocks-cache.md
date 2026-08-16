# Masstree as a write-back cache over RocksDB

`masstree_rocks_index` is an `OrderedIndex` (the trait in
`src/mako/storage/abstract_ordered_index.h`) whose system of record is
RocksDB and whose accelerator is an in-memory Masstree. It is
deliberately **isolated**: no Sto, no transaction runtime, no
replication, no sharding. Point ops, ranges, and a flush barrier —
nothing else.

The defining property: **a write acks before it is durable.** `put`,
`insert`, and `remove` apply to Masstree and return; a background
flusher moves the change into RocksDB afterwards. Crash inside that
window loses the un-flushed tail. That is the accepted trade, not a
bug — callers that need durability call `flush()`.

Substrate: `masstree_ordered_index.hh` is the pattern this file
follows — C++ `@unsafe` kernels for btree/RCU surgery, the Rust DSL for
the class shape and logic. It is *not* composed, because a cache entry
needs in-place mutable metadata (CLOCK bit, durability flag) that the
plain index's `std::string`-valued API can only express as a full tree
rewrite.

## Entry

One RCU allocation per entry; value bytes inline.

```
struct mrx_entry {
  uint64_t version;                 // immutable after publish
  uint32_t value_len;               // immutable
  uint8_t  tombstone;               // immutable
  std::atomic<uint8_t> durable;     // 0 = dirty, 1 = in RocksDB
  std::atomic<uint8_t> referenced;  // CLOCK second-chance bit
  // value bytes follow
};
```

**Value and version are immutable for the life of an entry.** An
overwrite does not mutate in place — it allocates a *new* entry,
`insert`s it (displacing the old, which is RCU-deferred), and enqueues
a fresh dirty item. Only `durable` and `referenced` are ever mutated,
and both are atomic. This immutability is what makes the flusher race
tractable; see trap 2.

`version` comes from a single monotonic counter over the whole index.

## Operations

| Op | Behavior |
|---|---|
| `put` / `insert` | new entry (`durable=0`), tree insert, enqueue `(key, version)`, **return** |
| `remove` | new entry with `tombstone=1`, same path — a tombstone is a write, not an erase |
| `get` | tree hit → `referenced=1`; tombstone ⇒ not-found; miss → RocksDB → read-through fill |
| `scan`/`rscan` | chunked two-way merge, Masstree wins on key collision, tombstones suppress |
| `flush()` | barrier: block until every dirty entry enqueued before the call is durable. Returns **false** if it gave up because a RocksDB write failed or the store is shutting down — i.e. some acked writes are not durable |
| flusher | pops dirty items, batches into a RocksDB `WriteBatch`, marks entries durable |
| sweeper | CLOCK eviction once resident bytes exceed capacity |

`flush()` is an inherent method, not a trait method — `OrderedIndex` is
shared with backends that have no durability tier.

## Why there are per-key locks

`OrderedIndex` does not have a "just write it" surface: `put` returns
*newly inserted*, `insert` is put-if-absent, and `remove` returns
*existed*. Every one of those is a read-modify-write, and in a two-tier
store the read half cannot be answered from Masstree alone — a cache
miss has to ask RocksDB. Check and publish therefore have to be atomic
per key, or two concurrent `insert`s both see "absent" and both win.

So writes take a striped `rusty::Mutex` (1024 stripes, FNV-1a over the
key). The read-through fill takes it too, which is what closes the
last window in trap 3: without it, a fill that read RocksDB *before* a
concurrent write could install its stale value *after* that write was
flushed and evicted. These block rather than spin because the critical
section can contain a RocksDB read.

The fill also installs a **negative** entry (a clean tombstone) when
RocksDB has no row, so "absent" becomes a cached fact and a later
`insert` on the same key does not re-read disk to learn it.

Lock order is stripe → queue, and nothing takes them in the other
order. The flusher takes only the queue lock, and marks entries durable
outside it, relying on the version compare plus RCU: if a publish
displaced the entry mid-flush, the version no longer matches, and the
memory it is reading is still alive because the free was RCU-deferred.

## The traps

These are the four places this design can be silently wrong. Each
gets a dedicated test.

**1. Delete must leave a tombstone.** Erasing the key from Masstree
instead of writing a tombstone means the next `get` misses the cache,
falls through to RocksDB, and returns the value the caller just
deleted. A tombstone may only be erased from memory *after* it is
durable — at which point RocksDB no longer has the key either, so the
fall-through correctly returns not-found.

**2. The flusher must not clear dirtiness it did not earn.** Flusher
reads key K at version 7 and writes it to RocksDB. Meanwhile a writer
publishes version 8. If the flusher then marks "K is clean", version 8
is eviction-eligible while RocksDB still holds version 7 — silent data
loss. Guard: the flusher marks `durable` on *the entry instance it
read*, and only if that instance is still the published one. Version 8
is a different instance with its own queue item, so it stays dirty
until its own flush.

**3. Read-through fill must lose to a concurrent write.** A `get` miss
reads RocksDB, and before it can install the result a writer publishes
a newer value. Installing the RocksDB value with `insert` would clobber
the newer write. The fill uses `insert_if_absent` and drops its buffer
on failure, so a write always beats a fill.

**4. Eviction may only touch durable entries.** A dirty entry evicted
before its flush is gone. The sweeper skips `durable == 0`
unconditionally, which means a saturated cache full of dirty entries
cannot evict — it must push back on writers (or block on the flusher)
rather than silently drop.

## Merged scan

Masstree's `search_range` is push-based (functor); RocksDB's iterator
is pull-based. They cannot be merged directly. The scan pulls the
Masstree side in **bounded chunks** (the paced-scan pattern already used
elsewhere in this tree), advances the RocksDB iterator to the chunk's
upper bound, merges the two sorted runs, emits, and repeats from the
last key. Chunking also keeps a long scan from pinning one RCU region
for its entire duration.

Merge rule per key: Masstree entry wins outright — if it is a
tombstone, the key is suppressed and the RocksDB row is skipped;
otherwise the cached value is emitted and the RocksDB row skipped.
Keys only in RocksDB are emitted as-is. `rscan` is the mirror, using
`rsearch_range` and a reverse iterator.

Scans deliberately do **not** populate the cache; a large range would
otherwise evict the working set.

## Stages

| Stage | Content |
|---|---|
| S1 | entry + kernels + DSL struct; point ops, dirty queue, flusher, `flush()`, read-through. Traps 1–3. |
| S2 | merged `scan`/`rscan`, both directions, tombstone-aware. |
| S3 | byte accounting + CLOCK eviction, durable-only. Trap 4. |
| S4 | CMake target, gtest wiring, concurrency/stress test. |
