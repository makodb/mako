# The Rust cache

The same write-back cache as
[`docs/masstree-rocks-cache.md`](masstree-rocks-cache.md), rebuilt in
Rust. That document is still the one to read for *what the cache does and
why* — the invariants, the four traps, the overload behaviour. This one
covers what is different about the Rust build: how it is put together,
what it costs, and which decisions are load-bearing.

Both implementations exist and both are tested. They are compared
against each other by
[`tests/test_mrx_differential.cc`](../tests/test_mrx_differential.cc).

## Shape

```
                        C or C++ caller
                               │
                     mrxdb_* C ABI  (crates/mrx-ffi)
                               │
                       Db  (crates/mrx)
                    Rust API, RocksDB-shaped
                               │
                    Store  (crates/mrx-core)
      all the cache logic; no FFI, no unsafe, nothing outside std
                        │                  │
              KeyIndex trait          Blobs trait
                        │                  │
          crates/mrx-masstree       crates/mrx-rocks
           mtx_* C ABI (unsafe)      rocksdb/c.h (unsafe)
                        │                  │
                   masstree              RocksDB
```

The split is the point. `mrx-core` holds every decision that could go
wrong — versions, floors, the watermark, eviction eligibility, the
dirty map — and knows nothing about either backend. It is
`#![forbid(unsafe_code)]` and depends on nothing but `std`, so the whole
cache can be tested, mutation-tested, and reasoned about with no C++, no
RocksDB, and no FFI in the way.

`unsafe` exists in exactly two crates, both of which are thin: they
convert slices to pointers, check status codes, and copy results out.

| crate | what it is |
|---|---|
| `mrx-core` | the cache. Traits for the two backends, fakes for both, the mutation harness |
| `mtree-sys` | raw declarations for the `mtx_*` C ABI. No logic |
| `mrx-masstree` | `KeyIndex` over masstree |
| `mrx-rocks` | `Blobs` over RocksDB's C API |
| `mrx` | the assembled cache: `Db`, `WriteBatch`, `Iter` |
| `mrx-ffi` | the `mrxdb_*` C ABI, built as a staticlib |

## Building and testing

```bash
# The C++ has to exist first: mrx-masstree links mtx_* out of libmako.a.
ninja -C build_c22 mako

cd crates && cargo test --workspace     # 116 tests
```

Tests that need a real backend are gated on `cfg(have_mako)` /
`cfg(have_rocksdb)`, set by each crate's build script. **A missing
backend shows up as "0 tests run", never as a green suite that tested
nothing.** Point `MAKO_BUILD_DIR` or `ROCKSDB_LIB_DIR` at them if the
search does not find them.

CMake builds `mrx-ffi` and the differential test:

```bash
ninja -C build_c22 test_mrx_differential && ./build_c22/test_mrx_differential
```

Mutation testing, which is what the `mrx-core` suite is actually worth:

```bash
python3 crates/mrx-core/mutations/run.py     # 6/6
```

## Using it

From Rust:

```rust
let db = mrx::Db::open("/var/lib/thing", mrx::Options::default())?;
db.put(b"key", b"value")?;             // returns BEFORE it is durable
assert_eq!(db.get(b"key")?.as_deref(), Some(&b"value"[..]));
db.flush()?;                            // the real barrier
db.close()?;                            // drains, then stops
```

From C, via [`crates/mrx-ffi/include/mrxdb.h`](../crates/mrx-ffi/include/mrxdb.h):

```c
char *err = NULL;
mrxdb_t *db = mrxdb_open(NULL, "/var/lib/thing", &err);
mrxdb_put(db, "key", 3, "value", 5, &err);
mrxdb_flush(db, &err);
mrxdb_close(db, &err);
```

`mrxdb_rocksdb_compat.h` renames `rocksdb_*` onto these for a caller
already written against `rocksdb/c.h`. Read its header comment first: a
rename cannot change a guarantee, and one guarantee differs.

## What differs from RocksDB

Three things, in the order they will bite:

**A write returns before it is durable.** That is the whole point of the
cache. Code that relied on RocksDB's WAL making a write safe by the time
`put` returned needs an explicit `flush` wherever durability was
previously implicit. This is why the functions are named `mrxdb_*` and
not `rocksdb_*`.

**Memory is bounded by `capacity_bytes`, not a block cache.** Every key
is resident; only values are evicted. A workload with a billion tiny
keys is bounded by the key set and no setting changes that.

**A batch is not atomic.** `mrxdb_write` replays through the ordinary
write path, so a reader can observe a partially applied batch. It
amortises call overhead and fixes ordering within itself. Nothing more.

Smaller: deleted keys are never reclaimed from the index (a tombstone is
published, because erasing would break the "index miss means absent"
invariant that makes reads cheap), so `len()` counts them; and there is
no `iter_prev` — direction is fixed at seek time.

## Performance

`masstree_rocks_bench` runs both implementations in one process against
the same keys and values. 16 threads, 100-byte values, 200k keyspace,
unbounded capacity; median of three runs:

| phase | masstree (ceiling) | cache C++ | cache Rust | RocksDB | rust/cpp |
|---|---|---|---|---|---|
| write (ack) | 7.5M/s | 3.8M/s | 2.3M/s | 0.27M/s | **0.60** |
| read (hot set) | 41M/s | 24M/s | 21M/s | 3.0M/s | **0.83** |
| read (uniform) | 18M/s | 13M/s | 11M/s | 2.5M/s | **0.87** |

Single-threaded the two are at parity (0.90–1.08×). Against the baseline
the cache actually exists to beat — plain RocksDB — the Rust version is
13–15× on writes and 5–10× on reads.

### Where the write cost actually goes

An earlier version of this section blamed `Mutex<Arc<Val>>` and the two
per-write allocations. **Both were wrong**, and the ablation below is
what corrected them. It is produced by
[`write_scaling`](../crates/mrx-masstree/examples/write_scaling.rs),
which isolates the cache's own bookkeeping by pairing **real masstree**
with **instant blobs**, so neither the index nor the durable store is
the limit.

16 threads, 200k keyspace, per-thread random keys — matching what
`masstree_rocks_bench` actually does:

| layer added | ops/s | ns/op | cost |
|---|---|---|---|
| masstree `get_or_insert` alone | 11.1M | 90 | — |
| + entry table + publish under the entry lock | 8.9M | 112 | +22 ns |
| + the two allocations | 8.6M | 116 | ~0 (noise) |
| + **announce floor and version draw** | 4.5M | 221 | **+106 ns** |
| + writer batch and ticket log | 4.1M | 244 | +23 ns |
| + flusher running | 3.9M | 256 | +12 ns |

**The announce floor and the version counter cost more than everything
else combined.** `arm` is a `seq_cst` store — a full barrier on x86,
draining the store buffer on every write — and the draw is a `seq_cst`
fetch-add on one globally shared counter.

That cost is **not** Rust's. The C++ implementation does the same thing
by the same mechanism (`w->announce.store(..., seq_cst)` before a
`seq_cst` fetch-add, `masstree_rocks_index.hh`), so both pay it. It is
the price of closing the publish gap, which is the single defect that
escaped both the cache and crash suites entirely — see the
`publish-gap` mutation. A cheaper floor is possible in principle; a
*wrong* floor is the data-loss direction.

### What was tried, and what it bought

One change moved the number. Nine did not, and they are listed because
the next person will otherwise try them again:

| change | result |
|---|---|
| **entry table: `RwLock<Vec<Arc<Entry>>>` → doubling segments** | **writes 0.24 → 0.60, reads 0.28 → 0.83** |
| O(1) minimum for the ticket log | noise |
| unlocked fast path in `steal` | noise |
| lazy seed in `intern` (dropped a wasted alloc per write) | noise |
| cache-line padding on `WriterSlot` | **worse** — reverted |
| flusher scans only registered writer slots | noise |
| writeback borrows keys/values instead of copying (−8192 allocs/cycle) | noise |
| RocksDB: stop `increase_parallelism` / `optimize_level_style_compaction` | noise |
| lazy watermark recompute under backlog (matches the C++) | noise |
| 8× larger ticket log (do producers block on backpressure?) | noise |

Every one after the first is kept anyway — each is defensible on its own
terms, and several protect against pathologies this benchmark does not
reach — but none of them is a throughput fix, and saying otherwise would
be inventing a result.

**After the entry table there is no single remaining dominant cause.**
The gap is the sum of many 10–30 ns differences, nearly all of them
`std::sync::Mutex` where the C++ has a lock-free CAS or a spinlock.

The one structural lever left is dropping the zero-dependency rule:
`arc-swap` for the value slot and a lock-free queue for the ticket log
would remove the entry lock (+22 ns) and the writer batch/log (+23 ns),
which the ablation puts at about 16% of the write path — meaningful, not
transformative, and paid for with `unsafe` in a dependency. That is a
decision to take deliberately, not a tuning knob.

### What was ruled out, each by measurement rather than argument:

| suspected | test | effect |
|---|---|---|
| the flusher thread | remove it entirely | +14% |
| the ticket-log mutex | batch 8 → 4096, i.e. 512× fewer acquisitions | +21% |
| the dirty map's O(n) scan | slow the durable store 4× to force a backlog | 0% |
| the global version counter alone | one contended `fetch_add`, nothing else | 101M/s — 25× headroom |
| per-write allocations | publish a pre-built record instead | within noise |

So the residual gap against C++ is **not one thing**. It is spread
across a `std::sync::Mutex` where the C++ uses a spinlock, a
mutex-guarded ticket log where the C++ has a lock-free ring, and
`Vec` churn in the writer batch where the C++ has a fixed array — none
of them individually large, and all of them consequences of
`forbid(unsafe_code)` with zero dependencies rather than of oversight.

The profiling did find one real waste: `intern` took its seed value by
argument, so every write allocated an `Arc<Val>` and dropped it unused
on all but the first write to a key. Now a closure. It measured as
noise too, which is the honest report.

### What was already fixed, and how it was found

The first measurement was much worse: 0.24× on writes and 0.28× on
reads at 16 threads, against 0.90–1.08× single-threaded. A ratio that
collapses with thread count *and* affects reads is contention, and the
read path's only shared mutable state was the entry table — then a
`RwLock<Vec<Arc<Entry>>>` that every operation locked and cloned an
`Arc` out of. Three atomic read-modify-writes per operation, two of them
on a single cache line shared by every core.

Replacing it with [`table.rs`](../crates/mrx-core/src/table.rs)'s
doubling segments — allocated once, never moved, so `&Entry` is sound
without `unsafe` because entries are immortal by design — took reads
from 0.28× to 0.83× and writes from 0.24× to 0.60×.

Worth recording that `perf` was unavailable (`perf_event_paranoid`), and
scaling the thread count found it faster than a profiler would have.

Also worth recording: **nine subsequent fixes all measured as noise or
worse** — see the table above. Nine predictions that did not survive
contact with a measurement is the reason this section reports ablations
rather than reasoning, and the reason each of them is described by what
it was *measured* to do rather than by what it should have done.

The most valuable thing the performance work produced was not
performance. Chasing an A/B that hung for ten minutes uncovered a
flusher deadlock — the blocking `append` on log backpressure — that
stops the whole process under sustained overload, which is precisely the
regime the cache exists for. See
`the_flusher_never_blocks_on_the_backpressure_it_relieves`.

## How this is verified

Four layers, each answering something the others cannot.

**Unit and integration tests (116).** The 40 C++ cache tests are ported
one-for-one, so a property that exists there and not here is one the
port dropped. `tests/durability.rs` states the three durability
properties as three named groups. A crash is `Runtime::abort` over a
surviving `MemBlobs` — which models strictly *more* loss than a real
crash, since it discards the entire cache tier atomically.

**Mutation testing (6/6).** A suite that has never been shown to fail
has not been shown to test anything.
[`mutations/run.py`](../crates/mrx-core/mutations/run.py) injects six
durability defects — the publish gap, stale writeback, discharging
before the write lands, evicting above the watermark, exiting without
draining, coalescing to the newest version — and requires each to be
caught. One better than the C++ original's 5/5, because the port makes
the dirty map's oldest-version rule explicit where C++ only reached it
through timing.

`no-shutdown-barrier` survived the first run: nothing asserted the
durable store was complete after a clean exit in a way the background
flusher could not win by luck. The fix was to give `MemBlobs` a
write-latency knob, because **a backlog has to be able to exist for any
durability property to be observable**.

**The differential harness.** Both implementations, one generated
operation stream, required to agree on read results, existence
reporting, scans, and post-reopen state. Watermarks and residency are
deliberately not compared — internal accounting whose exact values are
implementation choices.

It passed on the first run, which meant nothing until a deliberate
divergence was shown to turn it red. It did not: CMake's
`add_custom_command(OUTPUT ...)` had not re-run cargo, so the test
relinked against a stale archive. Fixed with `add_custom_target`, which
always runs. **This is the same class of failure as the C++ mutation
run's stale build directory, and it produces the same outcome: a green
suite that verified nothing.**

**The benchmark**, above, which is the only one of the four that can
tell you the port is correct *and* unusable.

## Status

The port is complete and independently verified. The C++ implementation
is still the one wired into `OrderedIndex`; swapping the consumer over
is a separate change, and keeping both while the differential harness
runs is the point of having built it this way.
