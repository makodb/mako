# Masstree as a write-back cache over RocksDB

`masstree_rocks_index` is an `OrderedIndex` (the trait in
`src/mako/storage/abstract_ordered_index.h`) whose system of record is
RocksDB and whose accelerator is an in-memory Masstree. It is
deliberately **isolated**: no Sto, no transaction runtime, no
replication, no sharding.

Two properties define it.

**A write acks before it is durable.** `put`, `insert`, and `remove`
apply to Masstree and return; a background flusher moves the change
into RocksDB afterwards. Crash inside that window loses the un-flushed
tail. That is the accepted trade — callers that need durability call
`flush()`.

**Masstree holds every key; only values are evictable.** This is the
load-bearing invariant. The tree is a complete index of the keyspace,
so a tree miss is *authoritative absence*. Values come and go under
memory pressure; keys do not.

Substrate: `masstree_ordered_index.hh` is the pattern this file
follows — C++ `@unsafe` kernels for btree/RCU surgery, the Rust DSL for
the class shape and logic.

## What the key-resident invariant buys

Every simplification here follows from it:

- **Existence is answerable in memory.** `OrderedIndex` has no
  fire-and-forget write: `put` returns *newly inserted*, `insert` is
  put-if-absent, `remove` returns *existed*. Each is a
  read-modify-write, and consulting RocksDB for the read half is what
  previously forced per-key locks. Now they are lock-free CAS loops on
  one entry.
- **Eviction never mutates the tree.** It swaps a pointer inside a
  stable entry instead of removing a key, so the eviction path avoids
  masstree's hardest concurrent operation entirely.
- **Range scans do not merge two tiers.** The key set is wholly in
  Masstree, so a scan iterates Masstree and fills values for the
  evicted ones. No two-way merge, no cross-tier dedup, no tombstone
  suppression against a RocksDB iterator.

## What it costs

- **`open()` on a non-empty RocksDB is O(keyspace).** The invariant
  must hold before the first operation, so open scans the whole
  database and installs every key with its value evicted. An empty
  database costs nothing. There is no lazy variant that does not
  reintroduce the two-tier existence check.
- **The memory floor is the key set.** "Bounded cache" means bounded
  *values*; keys plus entry and masstree node overhead are resident
  regardless.
- **Deleted keys keep a tombstone entry.** See below.

## Layout

Two allocations, with different lifetimes.

```
struct mrx_val {                 // IMMUTABLE once published; RCU-freed
  uint64_t version;
  uint32_t len;
  uint8_t  tombstone;            // key is deleted
  uint8_t  resident;             // 0 = value lives only in RocksDB
  // value bytes follow when resident && !tombstone
};

struct mrx_entry {               // STABLE for the key's lifetime
  std::atomic<mrx_val*> val;     // never null
  std::atomic<uint8_t> referenced;   // CLOCK second-chance bit
};
```

The entry is allocated once per key and never moves, so `val` is a
stable CAS target. Every state change — write, delete, evict, fill —
is a single compare-and-swap of that one pointer, and the displaced
`mrx_val` is RCU-freed.

`mrx_val` is immutable after publication *except* for a `durable` flag
(the only atomic in it), because durability is a property of a specific
version. Value and version never change under a reader.

An **evicted** value is not a null pointer; it is a real `mrx_val` with
`resident = 0` that still **carries the version**. That is what makes
the fill race safe — see trap 3.

## Operations

| Op | Behavior |
|---|---|
| `get` | tree miss ⇒ absent, authoritatively. Hit ⇒ load `val`: tombstone ⇒ absent; resident ⇒ copy bytes; evicted ⇒ fill from RocksDB |
| `put` | get-or-create entry, then CAS `val` to a new live version. Returns "newly inserted" = previous state was absent-or-tombstone |
| `insert` | same, but the CAS is abandoned if the loaded state is live ⇒ returns false |
| `remove` | CAS `val` to a tombstone version; returns whether it was live beforehand |
| `scan`/`rscan` | iterate Masstree; emit resident values, fill evicted ones, skip tombstones |
| `flush()` | barrier: block until everything enqueued before the call is durable. **False** ⇒ a RocksDB write failed or the store is stopping, so some acked writes are not durable |
| flusher | drains the dirty queue into RocksDB `WriteBatch`es, marks versions durable |
| sweeper | CLOCK eviction: swaps durable resident values for evicted markers |

`flush()` is an inherent method, not a trait method — `OrderedIndex` is
shared with backends that have no durability tier.

## The traps

**1. Delete must leave a tombstone, and the tombstone must stay.**
Removing the key from the tree would break the invariant: a later tree
miss would report absence while RocksDB still held the row, and worse,
absence would become unprovable in memory. So a delete publishes a
tombstone version.

Reclaiming a tombstone *after* it is durable is the one operation that
would need a lock — a concurrent `insert` on the same key can revive it
between the durability check and the tree removal, and masstree has no
conditional remove. Tombstones are therefore retained. They double as
the negative cache. Consequence: `size()` counts tombstones, so it
overcounts live keys, and reclamation is a later compaction concern.

**2. The flusher must not clear durability it did not earn.** Flusher
reads key K at version 7 and writes it to RocksDB. Meanwhile a writer
publishes version 8. Marking "K is durable" would make version 8
evictable while RocksDB still holds version 7 — silent loss. Guard: the
flusher marks the `mrx_val` *it read*, and only if that exact pointer
is still published. Version 8 is a different allocation with its own
queue item.

**3. A fill must lose to any newer write — including one that has
already been evicted again.** The fill reads the evicted marker, reads
RocksDB, and installs. In between, a writer can publish a new value,
the flusher can persist it, and the sweeper can evict it — returning
the slot to "evicted" and making a naive compare-against-null succeed
with a stale value. This is ABA, and it is why the evicted marker is a
versioned allocation rather than a null: the fill's CAS names the exact
marker pointer it read, so any intervening write fails it.

**4. Eviction may only touch durable values.** Evicting a non-durable
value discards the only copy. The sweeper skips anything whose `val` is
not marked durable, which means a cache saturated with dirty values
cannot evict and must push back on writers or wait on the flusher
rather than silently drop.

## Flusher

One thread. FIFO order over the dirty queue is what makes
`flushed_upto` an exact watermark for the `flush()` barrier; partitioned
flushers (as `rocksdb_persistence.cc` uses) would each need their own.

Queue items carry `(key, version, qseq)`. `qseq` is assigned under the
queue lock so FIFO order and sequence order agree — publication order
alone does not, since two threads can publish and enqueue in opposite
orders.

## Scans

Iterate Masstree over the range. Resident values are emitted directly;
tombstones are skipped; evicted values are fetched from RocksDB.

A cold range therefore costs N point lookups where a merged iterator
would have cost one range read. That is a deliberate trade of speed for
the deletion of an entire class of correctness bug. Batching runs of
adjacent evicted keys into one RocksDB range read is a pure
optimization, available later, and needs no change to the semantics.

`clear()` is a **truncate of both tiers**, not a cache drop — dropping
only the tree would break the invariant while RocksDB still held rows.

## Stages

| Stage | Content |
|---|---|
| S1 | entry/val layout, CAS write path, flusher, `flush()`, read-fill, open-time key load. Traps 1–3 |
| S2 | `scan`/`rscan` over the tree with value fill |
| S3 | byte accounting + CLOCK eviction, durable-only. Trap 4 |
| S4 | CMake target, gtest wiring, concurrency/stress test |
