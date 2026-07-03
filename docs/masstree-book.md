# The Masstree Book

A developer guide for Mako's in-memory storage engine, based on the EuroSys'12 paper by Mao, Kohler, and Morris.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Data Structure Design](#2-data-structure-design)
3. [Key Encoding](#3-key-encoding)
4. [Node Layout](#4-node-layout)
5. [Version Numbers and Concurrency](#5-version-numbers-and-concurrency)
6. [Operations](#6-operations)
7. [Memory Management](#7-memory-management)
8. [Mako Integration: Transactions](#8-mako-integration-transactions)
9. [Mako Integration: Multi-Version (MVCC)](#9-mako-integration-multi-version-mvcc)
10. [Performance Characteristics](#10-performance-characteristics)
11. [File Reference](#11-file-reference)
12. [Common Pitfalls](#12-common-pitfalls)

---

## 1. Introduction

**Masstree** is a fast in-memory key-value index designed for multicore machines. It was created at MIT by Yandong Mao, Eddie Kohler, and Robert Morris, and published at EuroSys'12 ("Cache Craftiness for Fast Multicore Key-Value Storage").

On a 16-core machine, Masstree executes over 6 million simple queries per second — more than 30x faster than VoltDB or MongoDB, and comparable to memcached.

### Why Masstree?

The core insight: on modern hardware, **cache behavior dominates performance**. Masstree is designed from the ground up for cache efficiency:

- **Wide B+-tree nodes** (fanout 15) fit useful data into 2-3 cache lines, overlapping DRAM fetches via prefetching.
- **Trie-of-trees structure** handles variable-length keys without deep trees.
- **Lock-free reads** avoid cache-line bouncing between cores.
- **Per-node version counters** (not per-record locks) minimize write contention.

### Masstree in Mako

Mako uses Masstree as its **primary in-memory storage engine**. The integration adds:

- **OCC (Optimistic Concurrency Control)** with transaction read/write sets.
- **MVCC (Multi-Version Concurrency Control)** for snapshot isolation.
- **Watermark-based version reclamation** tied to Mako's speculative 2PC.
- A wrapper layer (`mbta_ordered_index`) exposing a table-oriented API.

### Paper Reference

> Yandong Mao, Eddie Kohler, Robert Morris. "Cache Craftiness for Fast Multicore Key-Value Storage." EuroSys 2012. https://pdos.csail.mit.edu/papers/masstree:eurosys12.pdf

---

## 2. Data Structure Design

### Trie of B+-Trees

A Masstree is a **trie with fanout 2^64**, where each trie node is a B+-tree. The trie structure handles variable-length keys; the B+-tree structure handles sorting and range queries within each trie level.

```
Layer 0: B+-tree indexed by key bytes 0-7
  |
  +-- For keys sharing the same 8-byte prefix:
      |
      Layer 1: B+-tree indexed by key bytes 8-15
        |
        +-- Layer 2: indexed by bytes 16-23
            ...
```

Each **layer** is a complete B+-tree that indexes one 8-byte **slice** of the key. Keys shorter than 8h+8 bytes (where h is the layer) are stored directly in the layer-h tree. Keys longer than 8h+8 bytes that share the same 8h-byte prefix are stored in a deeper layer.

### Key Invariants

1. Keys shorter than 8h+8 bytes are stored at layer h.
2. Any keys stored in the same layer share the same 8h-byte prefix.
3. When two keys share a prefix, they are stored at least as deep as the shared prefix. (If two keys have the same 8h-byte prefix, they are stored at layer >= h.)

### Why This Works

- **Short keys** (most common) stay in layer 0 — a single B+-tree lookup.
- **Long keys with common prefixes** (e.g., URLs sharing a domain) are handled efficiently by deeper layers, without wasting space on repeated prefixes.
- **Each layer is a B+-tree**, so range queries within a layer are efficient.
- The trie depth is bounded by `ceil(key_length / 8)`, not by the number of keys.

### Example

```
t.put("01234567AB")  // Stored in layer 0, key slice "01234567", suffix "AB"
t.put("01234567XY")  // Same 8-byte prefix → creates layer 1
                     // Layer 1 tree stores "AB" and "XY" under slices

t.remove("01234567XY")  // Traverses to layer 1, deletes "XY"
                        // "AB" key remains in layer 1
```

---

## 3. Key Encoding

### Key Slices (ikey)

Keys are split into 8-byte **slices** called `ikey` (integer key). Each slice is stored as a `uint64_t`, **byte-swapped** so that native less-than comparison produces the same result as lexicographic string comparison.

```
Key: "Hello World!"
      |       |    |
      ikey0   ikey1 suffix
      (8 B)   (4 B)

Layer 0 ikey: bytes_to_uint64("Hello Wo")  // byte-swapped
Layer 1 ikey: bytes_to_uint64("rld!\0\0\0\0")
```

This is the single most important optimization in Masstree — it converts string comparison to integer comparison, which is a single CPU instruction.

### Key Length Encoding (keylenx)

Each key slot in a border node has a `keylenx` byte that encodes the key's relationship to layers:

| keylenx value | Meaning |
|---------------|---------|
| 0-7 | Key fits entirely in this layer (length = keylenx) |
| 8-63 | Key has a suffix stored separately (length = keylenx) |
| 64 | Key has a suffix in the key suffix pool |
| >= 128 | **Layer marker** — this slot points to a child B+-tree (next layer) |

### Suffix Storage

Keys longer than 8 bytes have their remaining bytes stored as a **suffix**. Suffixes can be stored:

- **Inline** (`iksuf_`): Small suffixes stored directly in the node.
- **External** (`ksuf_`): Larger suffixes in a separately-allocated block.

The key suffix area is managed to minimize per-node memory, using a simple technique: allocating fixed space for up to 15 suffixes per node, with separate key memory blocks.

### Key Adapter (`masstree_key.hh`)

The `key_type` adapter provides:

```cpp
template <typename T>
class key {
    uint64_t ikey_;       // Current 8-byte slice
    int len_;             // Total key length
    int offset_;          // Current slice offset (layer * 8)
    const char* suffix_;  // Remaining bytes after ikey

    uint64_t ikey() const;     // Current slice as uint64
    int length() const;        // Total length
    Str suffix() const;        // Bytes after current slice
    void shift();              // Advance to next layer (offset += 8)
    void unshift_all();        // Reset to layer 0
};
```

---

## 4. Node Layout

### Border Nodes (Leaves)

Border nodes store keys, values, and metadata. Width = 15 by default (fits in ~4 cache lines).

```
struct border_node {
    uint32_t  version;         // Version counter (see Section 5)
    uint8_t   nremoved;        // Count of removed keys (for GC)
    uint8_t   keylenx[15];     // Key length encoding per slot
    uint64_t  permutation;     // Packed sorted order (see below)
    uint64_t  ikey0[15];       // 8-byte key slices
    link_or_value lv[15];      // Values or layer pointers
    border_node* prev;         // Doubly-linked leaf chain
    border_node* next;
    interior_node* parent;
    key_suffix_t  keysuffixes; // Suffix storage
};
```

**link_or_value** is a tagged union: it stores either:
- A **value pointer** (for leaf entries), or
- A **next_layer pointer** (for keys that extend into a deeper B+-tree layer).

### Interior Nodes

Interior nodes store keys and child pointers. Width = 15 (same as border).

```
struct interior_node {
    uint32_t  version;         // Version counter
    uint8_t   nkeys;           // Number of keys (0-15)
    uint64_t  ikey0[15];       // Key slices
    node*     child[16];       // Child pointers (nkeys + 1)
    interior_node* parent;
};
```

### Permutation Array (`kpermuter.hh`)

The **permutation** field is the key innovation for concurrent inserts. It is a 64-bit packed array encoding:

- **Lowest 4 bits**: `nkeys` (number of live keys, 0-15)
- **Remaining 60 bits**: 15 four-bit indices, representing the sorted order

```
Permutation: [nkeys=3] [keyindex[0]=2] [keyindex[1]=0] [keyindex[2]=1] ...
Means: The 3 live keys are at physical slots 2, 0, 1 (in sorted order)
```

**Why this matters**: To insert a key, the writer:
1. Writes the key and value to an unused physical slot.
2. Constructs a new permutation with the key in sorted position.
3. Writes the new permutation with a single aligned write (atomic on modern CPUs).

Readers see either the old permutation (without the new key) or the new permutation (with it). No intermediate state is visible. **No version increment needed for inserts** — only the permutation write.

---

## 5. Version Numbers and Concurrency

### Version Counter Layout

Each node has a 32-bit version counter with bit fields:

```
Bit:  0     1      2        3       4       5      6      7-12    13-31
    locked splitting inserting deleted isroot isborder unused vinsert vsplit
```

| Field | Meaning |
|-------|---------|
| `locked` | Write lock held (claimed by update or insert) |
| `splitting` | Node split in progress (dirty flag) |
| `inserting` | Insert in progress (dirty flag) |
| `deleted` | Node removed from tree |
| `isroot` | Root of a B+-tree layer |
| `isborder` | Border (leaf) vs interior node |
| `vinsert` | Counter incremented after each insert or split |
| `vsplit` | Counter incremented after each split |

### Read Protocol (Lock-Free)

Readers never acquire locks. Instead:

```
1. v = stableversion(node)      // Spin while dirty bits are set
2. Read node contents            // May see stale data
3. Check: has version changed?   // If v != node.version, retry from 1
```

`stableversion()` spins until the `inserting` and `splitting` bits are clear, then issues an acquire fence:

```cpp
value_type x = v_;
while (x & dirty_mask) {   // dirty_mask = locked | inserting | splitting
    spin();
    x = v_;
}
acquire_fence();
return x;
```

### Write Protocol

Writers acquire the node's lock (CAS on the `locked` bit), make changes, then unlock:

```
unlock(node):
    if node.version.inserting:
        ++node.version.vinsert     // Increment insert counter
    else if node.version.splitting:
        ++node.version.vsplit      // Increment split counter
    node.version.{locked, inserting, splitting} = 0  // Clear dirty bits
```

This is a **single memory write** — atomic on modern architectures.

### Split Protocol

Splits use **hand-over-hand locking** — lock the node, create the new sibling, lock the parent, insert the new child pointer, unlock in reverse order. The `splitting` bit prevents concurrent readers from seeing inconsistent state.

```
split(node n, key k):          // precondition: n is locked
    n' = new border node
    n.version.splitting = 1
    n'.version = n.version     // n' initially locked
    split keys between n and n'
    p = lockedparent(n)        // hand-over-hand locking
    insert n' into p
    unlock(n); unlock(n'); unlock(p)
```

Readers detect splits via the `vsplit` counter: if it changed between their two version checks, they retry from the root.

### Key Safety Properties

- **Readers never block writers** (no shared locks).
- **Writers never block readers** (readers just retry on version mismatch).
- **Locks are held briefly** (only during the actual mutation, not during tree traversal).
- **Deadlock-free**: Locks are acquired bottom-up (child before parent), matching B-link tree conventions.

---

## 6. Operations

### Get (Lock-Free Lookup)

```
get(root, key k):
retry:
    (n, v) = findborder(root, k)  // Traverse to border node
forward:
    if v.deleted: goto retry
    (l, lv) = extract link_or_value for k in n
    if n.version != v ("locked"):
        v = stableversion(n); next = n.next
        while !v.deleted and next != NIL and k >= lowkey(next):
            n = next; v = stableversion(n); next = n.next
        goto forward
    if lv == NOTFOUND: return NOTFOUND
    if lv == VALUE: return lv.value
    if lv == LAYER: root = lv.next_layer; advance k; goto retry
    if lv == UNSTABLE: goto retry  // Key being inserted
```

**Key invariant for correctness**: When a border node splits, its **higher** keys are shifted to the new sibling. The `lowkey(n)` (lowest key) of a node is constant over its lifetime. This means `get` can reliably find the right border node by comparing the current key with the next node's `lowkey`.

### Put (Insert/Update)

```
put(root, key k, value v):
    cursor = find_insert(root, k)  // Lock the border node, find slot
    if key exists:
        update value atomically     // Single aligned write
    else:
        write key and value to unused slot
        write new permutation        // Atomic, makes key visible
    unlock(cursor.node)
```

Updates use **aligned write instructions** — on modern machines, a concurrent `get` will see either the old value or the new value, never a torn write. Therefore updates don't increment the version counter.

### New Layers

When inserting key k1 into a border node that already contains key k2 with the same 8-byte slice:

1. Allocate a new empty border node n'.
2. Insert k2's suffix value into n' under k2's next slice.
3. Replace k2's `link_or_value` in the original node with a `next_layer` pointer to n'.
4. Unlock the original node.
5. Continue inserting k1 into the new layer n'.

The writer marks the key as `UNSTABLE` during this transition. Readers seeing `UNSTABLE` retry.

### Remove

Removes delete the key and mark it as removed. The border node list must be doubly-linked. Removed nodes are not freed immediately — they are marked `deleted` and reclaimed via epoch-based RCU.

Removes can delete entire layer-h trees for h >= 1. The cleanup requires locking both the layer-h tree and the layer-(h-1) border node that points to it.

### Scan (Range Query)

```
scan(root, begin_key, end_key, callback):
    // Stack-based traversal with version validation
    // Handles concurrent splits during iteration
    // Calls callback(key, value) for each match
```

Scans maintain a **stack of traversal state** (`scanstackelt`) to track position across layers. The scan is not atomic with respect to concurrent inserts and updates — `getrange` "is not atomic with respect to inserts and updates" (paper, Section 3).

---

## 7. Memory Management

### Epoch-Based RCU

Masstree uses **epoch-based reclamation** (a form of read-copy-update) for safe memory deallocation:

```
Epoch timeline:
    E1          E2          E3
    |           |           |
    Readers     Readers     No readers
    active      active      from E1
                            → Safe to free E1 nodes
```

- Each thread tracks its current epoch via `threadinfo`.
- When a node is removed, it is placed on a **limbo queue** tagged with the current epoch.
- Nodes are freed only when all threads have advanced past that epoch.
- `deallocate_rcu(ptr, size, tag)` schedules deferred deallocation.

### Thread-Local Memory Pools

Each thread has its own memory pool (`threadinfo`) to avoid contention on the global allocator:

```cpp
class threadinfo {
    // Thread-local pools for node allocation
    void* pool_allocate(size_t size, memtag tag);
    void  deallocate_rcu(void* ptr, size_t size, memtag tag);

    // Epoch management
    void rcu_start();   // Enter read-side critical section
    void rcu_stop();    // Exit read-side critical section
};
```

### Allocator Integration

Masstree is configured to use **jemalloc** (`HAVE_JEMALLOC=1`), with support for:
- **NUMA awareness** (`HAVE_LIBNUMA=1`)
- **Transparent huge pages** (`HAVE_MADV_HUGEPAGE=1`)
- **Superpages** (`HAVE_SUPERPAGE=1`)

---

## 8. Mako Integration: Transactions

Mako wraps Masstree with OCC (Optimistic Concurrency Control) via the **STO** (Software Transactional Objects) framework.

### MassTrans

`MassTrans<V>` (in `src/mako/sto/MassTrans.hh`) wraps a Masstree instance with transaction semantics:

```cpp
template <typename V>
class MassTrans {
    typedef Masstree::basic_table<table_params> table_type;
    table_type table_;

    // Transactional operations
    bool transGet(Str key, V& retval);      // Read with version tracking
    bool transInsert(K key, V value);        // Insert with conflict detection
    bool transPut(K key, V value);           // Insert or update
    bool transUpdate(K key, V value);        // Update only
    bool transDelete(K key);                 // Mark for deletion
    void transQuery(Str begin, Str end, Cb); // Range scan
};
```

MassTrans (and the `abstract_ordered_index` layer above it) also
exposes a **non-transactional API** whose shape matches Masstree's own
operation set — `get / put / insert / remove / scan / rscan` without a
transaction handle, each op per-key atomic on its own. See
[storage-interface.md](storage-interface.md)
for the design and semantic contract.

### How Transactions Work

1. **Read**: `transGet` reads the value and records `(node, version)` in the transaction's read set.
2. **Write**: `transPut` buffers the write in the transaction's write set (not yet applied to Masstree).
3. **Certification**: At commit time, the transaction validates all read set entries — if any version has changed, the transaction aborts.
4. **Install**: If validation passes, buffered writes are applied to Masstree atomically.

### Versioned Values

Masstree stores `versioned_value*` instead of raw values. Each versioned value contains:

```cpp
struct versioned_value_struct<T> {
    TransactionTid::type version;  // Transaction ID that wrote this value
    T value;                       // The actual data
    // + MVCC chain pointer (see Section 9)
};
```

### Wrapper Layer

`mbta_ordered_index` (in `mbta_wrapper.hh`) provides the table-oriented API used by benchmarks and the Mako server:

```cpp
class mbta_ordered_index : public abstract_ordered_index {
    void get(void* txn, Str key, Str& value);
    void put(void* txn, Str key, Str value);
    void insert(void* txn, Str key, Str value);
    void remove(void* txn, Str key);
    void scan(void* txn, Str start, Str end, callback);
};
```

For sharded deployments, `mbta_sharded_ordered_index` wraps multiple Masstree instances.

---

## 9. Mako Integration: Multi-Version (MVCC)

### Version Chains

For MVCC support, each value has a pointer to its previous version:

```
Current Version --> Previous Version --> Older Version --> ...
  (timestamp T3)     (timestamp T2)       (timestamp T1)
```

A reader at timestamp T2 follows the chain to find the version visible at T2.

### Watermark-Based Reclamation

Old versions are reclaimed based on Mako's **global watermark** — the timestamp below which all transactions are durably replicated:

```cpp
void lazyReclaim() {
    // Remove versions older than global watermark
    // These are no longer needed by any active transaction
}
```

This ties Masstree's MVCC garbage collection to Mako's speculative 2PC watermark system.

### Multiversion Value Layout

```cpp
// multiversion.hh
struct MultiVersionValue {
    char* data;                 // Value bytes
    uint32_t timestamp;         // Transaction timestamp
    uint32_t term;              // Replication term
    MultiVersionValue* next;    // Previous version (chain)
};
```

---

## 10. Performance Characteristics

### From the Paper (16 cores, 2012 hardware)

| Benchmark | Throughput |
|-----------|-----------|
| Get (small keys) | 8.03 Mreq/sec |
| Put (small keys) | 5.78 Mreq/sec |
| Mixed get/put | ~6 Mreq/sec |
| vs. memcached | Comparable |
| vs. VoltDB | >30x faster |
| vs. MongoDB | >30x faster |

### Scalability

- Near-linear scaling from 1 to 16 cores for get-dominant workloads.
- Put scaling is slightly sublinear due to DRAM bandwidth contention.
- Single-core Masstree is only 13% slower than a no-concurrency version (low overhead from version counters).

### Cache Behavior

- >30% of Masstree lookup cost is in-computation (not DRAM wait), due to cache-efficient layout.
- Linear search within nodes (not binary search) has better locality on Intel CPUs.
- Prefetching tree nodes during descent overlaps DRAM latency with computation.
- B+-tree fanout of 15 means 2-3 cache lines per node — good prefetch granularity.

### Design Feature Contributions (Figure 8 of paper)

| Feature | Get Improvement | Put Improvement |
|---------|----------------|-----------------|
| +Superpage | 36% | 36% |
| +IntCmp (integer key comparison) | 15-24% | 15-24% |
| +Prefetch | 9-31% | 9-31% |
| +Permuter (permutation array) | 4% | 4% |
| Total (Masstree vs binary tree) | 3.3x | 2.9x |

---

## 11. File Reference

### Core Data Structure (`src/mako/masstree/`)

| File | Purpose |
|------|---------|
| `masstree.hh` | Main B+-tree interface, `basic_table` class |
| `masstree_struct.hh` | Node layouts: `leaf<P>` (border), `internode<P>` |
| `masstree_key.hh` | Key adapter: ikey slices, suffix, layer navigation |
| `kpermuter.hh` | Permutation array (packed 64-bit sorted order) |
| `nodeversion.hh` | Optimistic versioned node locking (sequence locks) |

### Operations

| File | Purpose |
|------|---------|
| `masstree_get.hh` | Lock-free point lookup (`find_unlocked`) |
| `masstree_insert.hh` | Insert with splits (`find_insert`) |
| `masstree_remove.hh` | Delete with layer GC (`gc_layer`) |
| `masstree_scan.hh` | Range scan with stack-based traversal |
| `masstree_split.hh` | Node split logic |
| `masstree_tcursor.hh` | Transaction cursor for traversal with version tracking |

### Concurrency and Memory

| File | Purpose |
|------|---------|
| `kvthread.hh` | Thread-local state, memory pools, epoch-based RCU |
| `masstree_context.h/cc` | Per-instance context for multiple Masstree instances |
| `TRcu.hh` | RCU memory reclamation |
| `compiler.hh` | Compiler fences, prefetch hints, alignment macros |

### Configuration

| File | Purpose |
|------|---------|
| `config.h` | Build-time settings: cache line size, jemalloc, NUMA, hugepages |

### Mako Integration (`src/mako/sto/`)

| File | Purpose |
|------|---------|
| `MassTrans.hh` | Transaction wrapper: `transGet`, `transPut`, `transDelete`, `transQuery` |
| `Transaction.hh/cc` | OCC engine: read/write sets, validation, commit |
| `versioned_value.hh` | Versioned value struct with TID |
| `multiversion.hh` | MVCC chain and watermark-based reclamation |

### Wrapper Layer (`src/mako/benchmarks/`)

| File | Purpose |
|------|---------|
| `mbta_wrapper.hh/cc` | `mbta_ordered_index`: table API over MassTrans |
| `mbta_sharded_ordered_index.hh` | Sharded variant for multi-shard deployments |
| `abstract_ordered_index.h` | Abstract interface for index implementations |

---

## 12. Common Pitfalls

### For Developers Modifying Masstree

**Never hold a node lock during a blocking operation.** Masstree locks are spinlocks. Blocking (I/O, sleep, fiber yield) while holding a lock will stall all other threads trying to access that node.

**Always validate versions after reading.** If you read node contents without checking the version afterward, you may act on stale data that was concurrently modified.

**Respect the permutation array.** Physical slot order != logical key order. Always iterate keys through the permutation, not by physical index.

**Don't modify node contents without the lock.** The lock-free read protocol assumes writes are protected by the node lock. Writing without the lock causes data races.

**Be careful with layer transitions.** When a `link_or_value` slot is `LAYER`, the value is a pointer to a child B+-tree, not a user value. Check `keylenx` before interpreting `lv`.

**RCU is mandatory for memory safety.** Never free a node directly. Always use `deallocate_rcu()` to defer deallocation until no readers hold references.

**Suffix storage can move.** When a node splits, key suffixes may be relocated. Don't cache pointers to suffix data across operations.

### For Developers Using MassTrans

**Always use transactions.** Direct Masstree access bypasses version tracking and can break OCC invariants.

**Handle abort exceptions.** `transGet`/`transPut` may throw `abstract_abort_exception` if a conflict is detected during the operation. Callers must catch and retry.

**Initialize threads.** Each thread must call `InitThread()` before accessing Masstree to set up thread-local state (memory pools, epoch tracking).

**Don't hold transaction state across yields.** In Mako's fiber model, yielding to another fiber while holding transaction state can cause the read set to become stale. Keep transactions short.

---

## 13. RocksDB API Compatibility Analysis

Mako exposes a RocksDB-shaped API — `mako::IDatabase` / `mako::ITable` — as a facade over the transactional layer described in §8 ("Mako Integration: Transactions"). Two backends implement the interface: `mako::DB` (in-process, wraps a `SiloRuntime` directly) and `mako::RemoteDB` (RPC client to a remote Mako server).

For the current method-level mapping (RocksDB C++ API → `IDatabase`/`ITable`), the conceptual analysis of where Silo/Masstree aligns with and diverges from RocksDB (persistence, snapshots, epochs, iteration model, secondary indexes, extension points), the compatibility feasibility matrix, and the roadmap for further extensions, see the standalone reference:

**→ [`docs/rocksdb_interface.md`](rocksdb_interface.md)**

The remainder of this section is a Masstree-side note on the one topic that's easier to see from the storage-engine angle: **how RocksDB's Column Family model maps onto separate Masstree instances**.

### Column Families: RocksDB Model → Masstree Mapping

In RocksDB, a **column family** is a logical namespace within a single DB. Each column family has its own memtable and SST files but shares the WAL with other column families. The key properties are:

- A DB always has a `default` column family.
- You can create/drop column families at runtime.
- Each column family can have different options (compression, compaction style, etc.).
- A `WriteBatch` can atomically write to **multiple column families** in one call.
- `Get`/`Put`/`Delete` take a `ColumnFamilyHandle*` to specify the target.

```cpp
// RocksDB column family usage
ColumnFamilyHandle* cf;
db->CreateColumnFamily(cf_options, "my_cf", &cf);
db->Put(write_options, cf, "key", "value");       // Write to specific CF
db->Get(read_options, cf, "key", &value);          // Read from specific CF

WriteBatch batch;
batch.Put(cf1, "k1", "v1");                        // Cross-CF atomic write
batch.Put(cf2, "k2", "v2");
db->Write(write_options, &batch);
```

**Masstree mapping**: Each column family maps to a **separate Masstree instance** (a separate `MassTrans` / `open_index()` call). This is a natural fit:

```
RocksDB DB with 3 column families:
  "default"  →  Masstree instance 0 (open_index("default"))
  "metadata" →  Masstree instance 1 (open_index("metadata"))
  "logs"     →  Masstree instance 2 (open_index("logs"))
```

**Cross-CF atomicity**: RocksDB's `WriteBatch` can write to multiple CFs atomically. In Masstree, this maps to a **single OCC transaction spanning multiple indexes**. Mako's STO framework supports multi-index transactions — the read/write set can include entries from different Masstree instances, and `commit()` validates and installs all of them atomically.

```cpp
// Masstree equivalent of cross-CF atomic write
auto txn = Sto::start_transaction();
index_default->transPut("k1", "v1");   // CF "default"
index_metadata->transPut("k2", "v2");  // CF "metadata"
Sto::commit();                          // Atomic across both indexes
```

**What doesn't map**: Per-CF options (compression, compaction style) are RocksDB-specific and have no Masstree equivalent. `mako::DB` accepts these options at the interface boundary but ignores them — all Masstree instances use the same in-memory configuration.

---

*This document is based on the Masstree paper (EuroSys'12) and the implementation in `src/mako/masstree/`. For Mako-specific transaction and replication details, see [mako-book.md](mako-book.md). For the full RocksDB API compatibility reference and roadmap, see [rocksdb_interface.md](rocksdb_interface.md).*
