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
  uint32_t key_len;              // key bytes inline after the struct
};
```

There is deliberately **no per-value durable flag**: durability is the
single comparison `version <= W` against the store-wide watermark (see
Flusher below). The key lives inline in the entry — paid once per key —
so dirty tickets are 16-byte PODs.

The entry is allocated once per key and never moves, so `val` is a
stable CAS target. Every state change — write, delete, evict, fill —
is a single compare-and-swap of that one pointer, and the displaced
`mrx_val` is RCU-freed.

`mrx_val` is fully immutable after publication. Value and version
never change under a reader.

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
| `flush()` | barrier: block until every write acked before the call is durable (W ≥ version counter at entry). **False** ⇒ a RocksDB write is failing or the store is stopping. "Durable" = sync=0 RocksDB: survives process crash, not power loss |
| flusher | drains tickets into the dirty map at memory speed, writes back each dirty entry's current bytes, advances W |
| sweeper | CLOCK eviction: swaps covered (version ≤ W) resident values for evicted markers, when a capacity is configured |

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

**2. The watermark must not cover obligations it did not discharge.**
An entry stays in the flusher's dirty map — pinning W below its oldest
undischarged version — until a write of its *current* bytes actually
succeeds. Writing current bytes (not a stale snapshot) is what makes
discharging a superseded obligation honest: RocksDB ends up with bytes
at least as new as everything the map entry covered. The first
implementation skipped superseded tickets instead, and a hot key could
stay un-persisted forever while `flush()` reported success — found by
adversarial review, pinned by the
`FlushCoversHotKeyUnderConcurrentOverwrites` regression test.

**3. A fill must lose to any newer write — including one that has
already been evicted again.** The fill reads the evicted marker, reads
RocksDB, and installs. In between, a writer can publish a new value,
the flusher can persist it, and the sweeper can evict it — returning
the slot to "evicted" and making a naive compare-against-null succeed
with a stale value. This is ABA, and it is why the evicted marker is a
versioned allocation rather than a null: the fill's CAS names the exact
marker pointer it read, so any intervening write fails it.

**4. Eviction may only touch covered values.** Evicting a value above
the watermark discards the only copy. The sweeper skips anything with
`version > W`, which means a cache saturated with un-persisted values
cannot evict and must wait on the flusher rather than silently drop.
Capacity is compared against *evictable payload* (`resident_bytes`
minus the un-evictable floor of entries + value headers): comparing
against the total would make any capacity below the floor permanently
"over" and degrade the sweeper into perpetual churn.

## Flusher: log → dirty map → writeback → watermark

The write path is lock-free: CAS-publish, then drop a 16-byte
`{entry*, version}` ticket into a per-thread batch (tiny per-writer
spinlock, uncontended); full batches append to a single MPSC ring with
one `fetch_add` per batch. The flusher (one thread, polling ~100µs):

1. **Drain**: fold tickets into the *dirty map* (`entry → oldest
   undischarged version`) at memory speed, recycling ring slots
   immediately — producers essentially never see backpressure, and
   ring drain does not depend on RocksDB health.
2. **Writeback**: write each dirty entry's **currently published**
   bytes into one `WriteBatch`; erase from the map only on success. A
   failed write keeps the obligations, pins W, and retries — transient
   IO failure self-heals with no data loss.
3. **Watermark**: `W = min(version counter, per-writer
   announce/batch/staged floors, stash, undrained tickets, dirty-map
   minimums) − 1`. The per-writer `announce` (set before the version
   draw, cleared after the ticket is batched) closes the publish gap.
   Recomputation is lazy under a large dirty map; a `flush()` waiter
   forces it every cycle. Laziness lags W, never unsounds it.

The dirty map is simultaneously the **coalescer** (a hot key occupies
one map slot no matter how many tickets, written once per pass) and the
**honesty mechanism** (an undischarged entry pins W). Partial writer
batches are *stolen* by the flusher each cycle, so an idle thread
cannot pin W; all flusher-side ring appends are non-blocking (failed
appends park in a watermark-covered stash), so the flusher cannot
deadlock on the backpressure it relieves.

Measured (16 threads, 8M writes, 200K keys, 128B values): write ack
1.84M/s (7.0x raw RocksDB), end-to-end durable 4.91x, reads 9.9x hot /
6.1x uniform.

## Under sustained overload the watermark stops advancing

W is a **low-water mark** over undischarged obligations, so it can only
move when writeback drains a prefix of the dirty map. If writers
outrun RocksDB ingest indefinitely — as a flat-out distinct-key
workload does — the dirty map grows, the oldest undischarged version
stays put, and **W stops moving entirely**.

Consequences, all correct-by-design but worth knowing before relying on
them:

- writes keep acking and stay readable; nothing is lost
- but nothing becomes **durable**, so `flush()` blocks for as long as
  the overload lasts
- and since eviction requires `version <= W`, the value tier cannot
  reclaim either — a capacity-bound store under write overload holds
  everything resident

This is the backpressure boundary of the design: the cache absorbs
bursts, not permanent excess (see the throughput note above — durable
throughput is bounded by ingest × coalescing, and coalescing only helps
when keys repeat). A workload with any think-time lets W advance
normally, which is why the crash tests pace their writer: with the
producer flat out, *nothing is ever covered* and the covered ⇒ durable
property has nothing to test.

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

## Eviction

The store takes a byte `capacity` for the value tier. **Zero, the
default, disables eviction entirely** — no sweeper thread is even
started — so a caller that does not opt in behaves exactly as it did
before eviction existed.

A sweeper thread runs a CLOCK hand over the keyspace in 256-key chunks.
A value whose reference bit is set survives the pass and loses the bit;
anything else that is durable is evicted, meaning its record is swapped
for a versioned evicted marker. The cursor is a **key**, not an
iterator, so it stays valid across chunks while the tree mutates, and
it wraps to the start when it runs off the end.

The sweeper wakes on pressure — writes and read-through fills both grow
the resident tier, and both nudge it. A pass that reclaims nothing does
**not** re-sweep: the only thing that can make a value evictable is the
flusher marking it durable, so the sweeper waits for the flusher's
progress epoch to advance. That is what keeps a cache saturated with
non-durable values from spinning.

`capacity` is enforced **approximately**, in two distinct ways.

`resident_bytes` is maintained with relaxed atomics across concurrent
swaps, so it is an accounting estimate; making it exact would mean
serializing publishes.

More importantly, `resident_bytes` counts **entry records as well as
value bytes**, and entries are never reclaimed. So the counter has an
un-evictable floor of roughly `key_count × (sizeof(mrx_entry) +
sizeof(mrx_val))`. A capacity below that floor can never be satisfied:
the sweeper will evict every value, find nothing further to reclaim,
and then park waiting on flusher progress rather than spinning. That is
correct behavior, but it means capacity should be set well above the
keyspace floor to mean anything.

Shutdown stops the sweeper **before** the flusher, since the sweeper
waits on flusher progress and would otherwise block on an epoch that
can never advance.

## Stages

| Stage | Content |
|---|---|
| S1 | entry/val layout, CAS write path, flusher, `flush()`, read-fill, open-time key load. Traps 1–3 |
| S2 | `scan`/`rscan` over the tree with value fill |
| S3 | byte accounting + CLOCK eviction, durable-only. Trap 4 |
| S4 | CMake target, gtest wiring, concurrency/stress test |

S1–S4 are built. See `openspec/specs/masstree-rocks-cache/spec.md` for
the behavior contract and `tests/test_masstree_rocks_cache.cc` for the
coverage.
