//! Where does the Rust write path lose throughput at high thread count?
//!
//! `masstree_rocks_bench` compares the whole stack, which means a slow
//! write arm could be masstree, RocksDB, the flusher, or the cache's own
//! bookkeeping. This isolates the last one: **real masstree** (so the
//! index scales the way it does in production) and **instant blobs** (so
//! the durable store is never the limit).
//!
//! Read it as a sweep, not a single number. A per-op cost and a
//! contention cost look identical at one thread count and nothing alike
//! across four, which is the only reason the earlier 0.24x regression
//! was diagnosable without a profiler.
//!
//! ```text
//! cargo run --release --example write_scaling -- [threads] [ops] [mode] [batch] [keyspace] [blob_us]
//!
//!   mode  full       flusher running (the real configuration)
//!         noflusher  no background thread at all; the log just fills
//!
//!   ...and the ablations, each adding one layer, so the cost of a write
//!   can be attributed rather than guessed at. In ladder order:
//!
//!         index      masstree get_or_insert only. The ceiling.
//!         index_ver  + one draw from the shared version counter. The
//!                    first thing in the write path that all sixteen
//!                    threads touch, and not a small addition.
//!         alloc      + entry table + publish under the entry's lock,
//!                    republishing a pre-built record instead of
//!                    allocating one.
//!         alloc_ring as `alloc`, but from a per-thread ring rather
//!                    than one record shared by every thread — the
//!                    other end of the bracket on what allocation
//!                    costs. See `Prebuilt`.
//!         entry      + the two per-write allocations, i.e. the real
//!                    `Record::resident(ver, bytes)`.
//!         floors     + the announce floor. Splits the durability
//!                    bookkeeping into its memory-ordering half and its
//!                    locking half, which is the difference between a
//!                    cost inherent to the design and one that comes
//!                    from choosing a futex Mutex over a spinlock.
//!         noflusher  the real `Store`, with no background thread at
//!                    all; the log just fills.
//!         full       the real `Store` with the flusher running.
//!
//!         atomic     ONE shared fetch_add per op, and NOTHING else.
//!                    Read it as a lower bound and a lesson, not as a
//!                    floor: with no work between two increments a core
//!                    keeps the line and retires a run of them before
//!                    losing it, so it reports a fraction of what the
//!                    same instruction costs once real work sits
//!                    between draws. `index` -> `index_ver` is that
//!                    same instruction measured in place.
//!
//!   What `entry` and `floors` still do NOT mirror, so that the rung
//!   above them is not read as being about only what its name says:
//!
//!     * neither calls `Store::account`, whose `resident_bytes`
//!       fetch_add is a SECOND counter every thread touches. Its cost
//!       lands in the `floors` -> `noflusher` rung, which is therefore
//!       not purely "batch and ticket log".
//!     * both draw the version BEFORE taking the entry lock, where
//!       `Store::write` draws it inside. Same operations, shorter
//!       critical section; it matters only for same-entry collisions,
//!       which at a 200k keyspace and 16 threads are rare.
//!     * `noflusher` sizes the ticket log to hold every ticket
//!       (`threads*ops*2`) where `full` always uses 1<<20, so those two
//!       rows differ in log geometry as well as in the flusher. At
//!       `ops = 32768` the two sizes coincide exactly, which is how to
//!       check whether a gap between them is the flusher or the log.
//!
//!   Read-side ablations, which answer a different question: how much of
//!   a read is the Arc refcount traffic that epoch reclamation would
//!   remove?
//!
//!         rd_index   masstree lookup only
//!         rd_entry   + entry table + `Entry::load` (the Arc clone/drop
//!                    and the slot lock)
//!         rd_full    + copying the value out, i.e. `Store::get`
//! ```
//!
//! # An ablation only measures the real path if it SHARES what the real
//! # path shares
//!
//! Every mode below is a hand-written stand-in for a slice of
//! `Store::write`, and the way a stand-in goes wrong is not usually by
//! computing the wrong thing — it is by putting a per-thread field on a
//! shared cache line, or a shared field on sixteen. Then the mode
//! measures coherence traffic the real store never generates, the layer
//! it was supposed to isolate is charged for it, and the number does not
//! merely have error bars, it points at the wrong code.
//!
//! That happened here. `floors` built ONE `WriterSlot` and shared it
//! across all sixteen threads, where `Store` gives each thread its own
//! slot out of a `Vec<WriterSlot>`. Its seq_cst `announce` store — a
//! thread-private write in production — became a sixteen-way contended
//! line, and the mode reported the announce floor costing +106 ns/op.
//! See `Writers` below for what the real assignment is.
//!
//! The rule this file now follows: for each mode, ask of every piece of
//! state whether `Store` gives it to one thread or to all of them, and
//! match that. Where a mode cannot (see `Prebuilt::Shared`), say so next
//! to the number rather than in a commit message nobody reads.
//!
//! Measures ACK throughput, matching the benchmark's "write (ack)" row:
//! writes return once visible, and making them durable is the flusher's
//! problem.
//!
//! `keyspace` matters more than it looks. With distinct keys every write
//! is an INSERT, and the insert path costs two masstree traversals (a
//! probe that misses, then `get_or_insert`) where an overwrite costs
//! one. `masstree_rocks_bench` writes 3.2M times over 200k keys, so it
//! is overwhelmingly overwrites — measure the insert path and attribute
//! the result to the overwrite path and you have explained the wrong
//! number. Default here is 0, meaning all-distinct; pass the bench's
//! 200000 to match it.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Barrier};
use std::time::Instant;

/// CPU seconds this process has burned, from /proc/self/stat.
///
/// Used to report how many cores were actually BUSY during the timed
/// region. With N workers the ideal is N; materially below it means the
/// workers were blocked rather than computing, which is a different
/// bottleneck from executing too many instructions.
///
/// Reads utime+stime (fields 14 and 15). The comm field can contain
/// spaces and parentheses, so parsing starts after the LAST ')'.
fn cpu_secs() -> f64 {
    let Ok(stat) = std::fs::read_to_string("/proc/self/stat") else {
        return 0.0;
    };
    let Some(idx) = stat.rfind(')') else { return 0.0 };
    let f: Vec<&str> = stat[idx + 1..].split_whitespace().collect();
    // After ')' the first field is `state`, which is field 3.
    let get = |n: usize| -> f64 {
        f.get(n - 3).and_then(|v| v.parse::<f64>().ok()).unwrap_or(0.0)
    };
    // USER_HZ is 100 on every Linux this runs on; the C++ arm uses
    // getrusage and needs no such assumption, so the two agreeing is the
    // check that this is right.
    (get(14) + get(15)) / 100.0
}

use mrx_core::fakes::MemBlobs;
use mrx_core::{CacheLine, Config, EntryTable, KeyIndex, Record, Runtime, Store};
use mrx_masstree::MasstreeIndex;

/// A version counter living where the **C++** ladder's lives.
///
/// The two ladders priced one `seq_cst` fetch_add 58% apart (+57.0 ns in
/// C++, +36.0 in Rust) on top of near-identical ~36 ns baselines. They
/// were not incrementing the same kind of memory. C++ draws from
/// `mrx_store::version_ctr`: `alignas(64)`, near the front of ONE ~24 MB
/// allocation whose tail is the ticket ring. Rust's `index_ver` drew
/// from an `Arc<AtomicU64>` — 24 bytes off the small-object allocator,
/// the word at offset 16, sharing its line with the Arc's own refcounts
/// and with whatever the allocator placed next to it.
///
/// This is the C++ home, rebuilt in Rust, so the difference between the
/// two rungs is the language and not the address. `index_ver` keeps the
/// Arc, `index_ver_store` uses this, and `mrx_write_ablation` gained the
/// mirror-image pair (`index_ver` / `index_ver_bare`) so the comparison
/// is a 2x2 rather than two unmatched numbers.
#[repr(C)]
struct StoreLike {
    /// `mrx_store`'s cold prefix: gen, tree, db, opts, ropts, wopts.
    _cold: [usize; 6],
    version_ctr: CacheLine<AtomicU64>,
    _resident_bytes: CacheLine<AtomicU64>,
    _floor_bytes: CacheLine<AtomicU64>,
    _persisted: CacheLine<AtomicU64>,
    /// Stand-in for `mrx_log::slots[1 << 20]`: 24 MB of tail, INLINE,
    /// because what is being reproduced is a counter inside a large
    /// allocation — a 64-aligned counter in a small one is a different
    /// experiment.
    bulk: [u64; 3 << 20],
}

impl StoreLike {
    /// Heap-allocate and leak, since `Box::new` would build 24 MB on the
    /// stack first. Leaking matches the C++ ablation, which also never
    /// frees its store.
    fn leak() -> &'static StoreLike {
        let layout = std::alloc::Layout::new::<StoreLike>();
        // SAFETY: non-zero layout; every field is valid all-zeroes
        // (integers and atomics). The allocation is leaked, so the
        // 'static borrow is sound.
        unsafe {
            let p = std::alloc::alloc_zeroed(layout) as *mut StoreLike;
            assert!(!p.is_null(), "24 MB alloc failed");
            (*p).version_ctr.0.store(1, Ordering::Relaxed);
            // First-touch every page, as `mrx_store_open`'s slot-init
            // loop does, so the timed run is not measuring page faults
            // in one arm and not the other.
            for i in (0..(3usize << 20)).step_by(512) {
                (*p).bulk[i] = 1;
            }
            &*p
        }
    }
}

/// The real store's writer-slot pool, mirrored.
///
/// `Store` holds a `Vec<WriterSlot>` of 64 and hands each thread one
/// slot — `next_writer.fetch_add() % len`, cached in a `thread_local` so
/// the assignment happens once per thread and every subsequent write
/// goes straight to its own slot. `announce`, `batch_min` and
/// `staged_min` are therefore written by exactly one thread.
///
/// The count is 64 rather than `threads` on purpose: it reproduces the
/// store's actual array, so the used slots are the same contiguous
/// prefix of the same unpadded layout, sharing lines with each other the
/// same way. Sizing the array to the thread count would quietly change
/// the geometry that `WriterSlot`'s "deliberately NOT cache-line padded"
/// note was measured against.
struct Writers {
    slots: Vec<mrx_core::WriterSlot>,
    next: AtomicU64,
}

impl Writers {
    fn new() -> Self {
        Self {
            slots: (0..64).map(|_| mrx_core::WriterSlot::new()).collect(),
            next: AtomicU64::new(0),
        }
    }

    /// This thread's slot, assigned once and remembered — the body of
    /// `Store::writer`.
    fn mine(&self) -> &mrx_core::WriterSlot {
        thread_local! {
            static SLOT: std::cell::Cell<Option<usize>> =
                const { std::cell::Cell::new(None) };
        }
        let n = self.slots.len();
        let i = SLOT.with(|s| match s.get() {
            Some(i) => i,
            None => {
                let i = (self.next.fetch_add(1, Ordering::AcqRel) as usize) % n;
                s.set(Some(i));
                i
            }
        });
        &self.slots[i]
    }
}

/// Where the `entry` family gets the record it publishes.
enum Prebuilt {
    /// Allocate one per write, as `Store::write` does: an `Arc` and a
    /// `Vec` of bytes, both fresh, both refcount-1 and thread-local
    /// until published.
    Fresh,
    /// Republish clones of ONE record shared by every thread.
    ///
    /// This is `alloc` mode, and it does NOT cleanly isolate allocation.
    /// It removes two thread-local allocations and adds a refcount
    /// increment on a line all sixteen threads increment, plus the
    /// matching decrement when the previous value is dropped. The
    /// substitute is contended where the thing it replaced was not, so
    /// read `entry - alloc` as a LOWER bound on allocation cost and
    /// `entry - alloc_ring` as the upper one.
    Shared(Record),
    /// Republish from a per-thread ring of distinct records.
    ///
    /// Same "no allocation" as `Shared` without the shared refcount:
    /// each thread cycles 64 records that only it increments, so the
    /// refcount traffic keeps the shape the real path has — one
    /// increment by the writer, one decrement by whoever overwrites —
    /// and the ring is small enough to stay resident.
    Ring(Vec<Vec<Record>>),
}

/// Records per thread in [`Prebuilt::Ring`]. 64 x ~150 B is under
/// 10 KiB, so a thread's ring stays in L1 and the reuse does not itself
/// turn into a cache-miss ablation.
const RING: usize = 64;

/// What one op does, per mode.
enum Work {
    Store(Arc<Store<MasstreeIndex, MemBlobs>>),
    /// The same store with **RocksDB** underneath instead of
    /// [`MemBlobs`], i.e. what `mrx_write_ablation`'s `full` rung has
    /// always been. Until this existed the two ladders' top rungs were
    /// not the same experiment: C++ paid a durable store and Rust did
    /// not, so `full - floors` charged C++ for RocksDB and Rust for
    /// nothing, and the difference was read as a language gap.
    Db(Arc<mrx::Db>),
    Index(Arc<MasstreeIndex>),
    /// `index`, plus one draw from the shared version counter.
    IndexVer(Arc<MasstreeIndex>, Arc<AtomicU64>),
    /// `index_ver`, with the counter moved into [`StoreLike`] — the
    /// C++ counter's home. The pair isolates the memory from the
    /// instruction.
    IndexVerStore(Arc<MasstreeIndex>, &'static StoreLike),
    Entry(Arc<MasstreeIndex>, Arc<EntryTable>, Arc<AtomicU64>, Prebuilt),
    Atomic(Arc<AtomicU64>),
    /// (index, table, how far to go: 0 = lookup, 1 = +load, 2 = +copy)
    Read(Arc<MasstreeIndex>, Arc<EntryTable>, u8),
    Floors(Arc<MasstreeIndex>, Arc<EntryTable>, Arc<AtomicU64>, Arc<Writers>),
}

impl Work {
    /// `tid` and `seq` are the calling thread's index and its op
    /// number, both computed by the caller with no atomics — so a mode
    /// that needs a unique value per op does not draw one from a shared
    /// counter and charge the contention to whatever it was measuring.
    fn run(&self, tid: usize, seq: u64, key: &[u8], value: &[u8]) {
        match self {
            Work::Store(s) => {
                s.put(key, value);
            }
            Work::Db(db) => {
                db.put(key, value).expect("put");
            }
            Work::Atomic(c) => {
                c.fetch_add(1, Ordering::SeqCst);
            }
            Work::Index(idx) => {
                // The word comes from `seq`, not from a shared counter.
                // `Store` bumps a shared atomic for it (the entry
                // table's `next`) only when a key is ABSENT — about 6%
                // of ops at the bench's 200k keyspace — so charging
                // every op for one put a contended `lock xadd` into the
                // ablation's BASELINE, where it was then subtracted back
                // out of every layer above. `index_ver` is that same
                // instruction measured as its own rung, which is where
                // it belongs.
                idx.get_or_insert(key, seq);
            }
            Work::IndexVer(idx, ver) => {
                // The rung `index` used to hide. Until this was split
                // out, the baseline the whole ladder was measured
                // against included a contended counter, so every layer
                // above had that cost silently subtracted back out of
                // it: the ladder charged masstree 90 ns and the version
                // draw nothing, where masstree is 25 ns and the draw is
                // most of the rest.
                idx.get_or_insert(key, seq);
                ver.fetch_add(1, Ordering::SeqCst);
            }
            Work::IndexVerStore(idx, s) => {
                // Byte-for-byte the rung above, except for WHERE the
                // counter lives. See `StoreLike`.
                idx.get_or_insert(key, seq);
                s.version_ctr.0.fetch_add(1, Ordering::SeqCst);
            }
            Work::Read(idx, table, depth) => {
                let Some(word) = idx.get(key) else { return };
                if *depth == 0 {
                    return;
                }
                let e = table.get((word - 1) as u32);
                // `load` is the Arc clone + drop and the slot lock: the
                // exact pair that epoch reclamation would replace with a
                // plain atomic load.
                if *depth == 3 {
                    // The refcount-free shape: hold the slot, copy out.
                    e.with_value(|v| {
                        if let Some(b) = v.bytes() {
                            std::hint::black_box(b.to_vec());
                        }
                    });
                    return;
                }
                let cur = e.load_touched();
                if *depth == 1 {
                    return;
                }
                // And the copy every caller pays regardless of mechanism.
                if let Some(b) = cur.bytes() {
                    std::hint::black_box(b.to_vec());
                }
            }
            Work::Floors(idx, table, ver, writers) => {
                // One slot per thread, exactly as `Store::writer` hands
                // them out. Sharing one slot here measured 16-way false
                // sharing on `announce` and called it the cost of the
                // announce floor.
                let slot = writers.mine();
                let word = match idx.get(key) {
                    Some(w) => w,
                    None => {
                        let e = mrx_core::Entry::new(key, Record::tombstone(0));
                        let i = table.push(e);
                        idx.get_or_insert(key, u64::from(i) + 1)
                    }
                };
                let e = table.get((word - 1) as u32);
                // The real write path's ordering, minus the ticket: arm
                // the announce floor (a seq_cst STORE, i.e. a full fence
                // on x86), draw a version, publish, release the floor.
                slot.arm(ver.load(Ordering::SeqCst));
                let v = ver.fetch_add(1, Ordering::SeqCst);
                e.with_slot(|_| (Some(Record::resident(v, value)), ()));
                slot.disarm();
            }
            Work::Entry(idx, table, ver, prebuilt) => {
                // The cache's write path with the announce floor and the
                // ticket log removed: find or create the entry, draw a
                // version, publish under the entry's lock.
                let word = match idx.get(key) {
                    Some(w) => w,
                    None => {
                        let e = mrx_core::Entry::new(key, Record::tombstone(0));
                        let idx_new = table.push(e);
                        idx.get_or_insert(key, u64::from(idx_new) + 1)
                    }
                };
                let e = table.get((word - 1) as u32);
                let v = ver.fetch_add(1, Ordering::SeqCst);
                // Every variant publishes under the SAME single
                // acquisition of the entry lock. An earlier version read
                // the current value first, which took the lock twice and
                // charged the difference to allocation.
                let nv = match prebuilt {
                    Prebuilt::Fresh => Record::resident(v, value),
                    Prebuilt::Shared(one) => one.clone(),
                    Prebuilt::Ring(rings) => {
                        rings[tid][(seq as usize) % RING].clone()
                    }
                };
                e.with_slot(|_| (Some(nv), ()));
            }
        }
    }
}

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let threads: usize = a.first().map_or(16, |s| s.parse().unwrap());
    let ops: usize = a.get(1).map_or(100_000, |s| s.parse().unwrap());
    let mode = a.get(2).map_or("full", |s| s.as_str());
    let batch: usize = a.get(3).map_or(64, |s| s.parse().unwrap());
    let keyspace: usize = a.get(4).map_or(0, |s| s.parse().unwrap());
    // Microseconds per writeback batch. Non-zero makes the durable store
    // slow enough that a backlog builds, which is the only state in which
    // the flusher's per-cycle work over the dirty map is visible at all.
    let blob_us: u64 = a.get(5).map_or(0, |s| s.parse().unwrap());
    // Untimed passes before the timed one; see the spawn below. Same
    // env name the C++ ablation reads, so a sweep cannot warm one
    // ladder and not the other.
    let warmup: usize = std::env::var("MRX_ABL_WARMUP")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    assert!(
        warmup == 0 || mode != "noflusher",
        "`noflusher` has no drain: a warm-up pass fills the log the timed \
         pass then blocks on"
    );

    // Keys are built up front. Building them inside the timed loop costs
    // about as much as the write itself and silently halves every number
    // — the mistake that made three earlier readings of this cache wrong.
    let keys: Vec<Vec<u8>> = if keyspace == 0 {
        (0..threads)
            .flat_map(|t| (0..ops).map(move |i| format!("t{t:02}-k{i:07}").into_bytes()))
            .collect()
    } else {
        // A shared keyspace, so almost every write is an overwrite.
        //
        // PER-THREAD RANDOM, matching masstree_rocks_bench's `rng(t+1)`.
        // Walking `i % keyspace` instead puts all 16 threads on the same
        // key at the same instant — maximum per-entry contention, which
        // measures a workload nobody runs and made the entry lock look
        // twice as expensive as it is.
        (0..threads)
            .flat_map(|t| {
                let mut x = (t as u64) + 1;
                (0..ops).map(move |_| {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    format!("k{:07}", (x as usize) % keyspace).into_bytes()
                })
            })
            .collect()
    };
    let value = vec![b'v'; 100];

    let cfg = Config {
        batch,
        // With no flusher nothing ever drains, so the log has to hold
        // every ticket or `append` blocks forever waiting for room that
        // is never coming.
        log_slots: if mode == "noflusher" {
            (threads * ops * 2).next_power_of_two()
        } else {
            1 << 20
        },
        ..Config::default()
    };

    let counter = Arc::new(AtomicU64::new(1));
    let (work, mut rt) = match mode {
        "atomic" => (Work::Atomic(Arc::clone(&counter)), None),
        "index" => (
            Work::Index(Arc::new(MasstreeIndex::new().expect("masstree"))),
            None,
        ),
        "index_ver" => (
            Work::IndexVer(
                Arc::new(MasstreeIndex::new().expect("masstree")),
                Arc::clone(&counter),
            ),
            None,
        ),
        "index_ver_store" => (
            Work::IndexVerStore(
                Arc::new(MasstreeIndex::new().expect("masstree")),
                StoreLike::leak(),
            ),
            None,
        ),
        m if m.starts_with("rd_") => {
            // Populate first, then measure reads over the same key set.
            let idx = Arc::new(MasstreeIndex::new().expect("masstree"));
            let table = Arc::new(EntryTable::new());
            let seen = std::collections::HashSet::<&[u8]>::from_iter(
                keys.iter().map(|k| k.as_slice()),
            );
            for k in seen {
                let i = table.push(mrx_core::Entry::new(
                    k,
                    Record::resident(1, &[b'v'; 100]),
                ));
                idx.get_or_insert(k, u64::from(i) + 1);
            }
            let depth = match m {
                "rd_index" => 0,
                "rd_entry" => 1,
                "rd_value" => 3,
                _ => 2,
            };
            (Work::Read(idx, table, depth), None)
        }
        "floors" => (
            Work::Floors(
                Arc::new(MasstreeIndex::new().expect("masstree")),
                Arc::new(EntryTable::new()),
                Arc::clone(&counter),
                Arc::new(Writers::new()),
            ),
            None,
        ),
        "entry" | "alloc" | "alloc_ring" => (
            Work::Entry(
                Arc::new(MasstreeIndex::new().expect("masstree")),
                Arc::new(EntryTable::new()),
                Arc::clone(&counter),
                match mode {
                    "entry" => Prebuilt::Fresh,
                    "alloc" => {
                        Prebuilt::Shared(Record::resident(0, &[b'v'; 100]))
                    }
                    _ => Prebuilt::Ring(
                        (0..threads)
                            .map(|t| {
                                (0..RING)
                                    .map(|r| {
                                        Record::resident(
                                            (t * RING + r) as u64,
                                            &[b'v'; 100],
                                        )
                                    })
                                    .collect()
                            })
                            .collect(),
                    ),
                },
            ),
            None,
        ),
        "full_rocks" => {
            // A fresh directory per run: reopening a populated one costs
            // an O(keyspace) scan at open, which is not what is being
            // timed.
            let dir = std::env::temp_dir().join(format!(
                "mrx_ws_rocks_{}_{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ));
            let db = mrx::Db::open(&dir, mrx::Options::default()).expect("open");
            (Work::Db(Arc::new(db)), None)
        }
        _ => {
            let blobs = MemBlobs::new();
            blobs.set_write_delay_us(blob_us);
            let store = Arc::new(
                Store::open(cfg, MasstreeIndex::new().expect("masstree"), blobs)
                    .expect("open"),
            );
            let rt = (mode == "full").then(|| Runtime::start(Arc::clone(&store)));
            (Work::Store(store), rt)
        }
    };
    let work = Arc::new(work);

    let barrier = Arc::new(Barrier::new(threads + 1));
    let elapsed = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..threads)
            .map(|t| {
                let work = Arc::clone(&work);
                let barrier = Arc::clone(&barrier);
                let keys = &keys;
                let value = &value;
                scope.spawn(move || {
                    // WARM-UP, untimed, before the barrier. At the
                    // ladder's usual 50k ops/thread a 16-thread run
                    // lasts ~0.03 s, and in 0.03 s the transients ARE
                    // the measurement: the masstree starts empty so
                    // most ops take the insert path and split nodes,
                    // every page is a first-touch fault, and the boost
                    // clock has not finished ramping. The same rung
                    // measured 38.7 ns/op at 50k ops and 22.1 ns/op at
                    // 500k — a 1.8x error, larger than most of the
                    // rungs the ladder is trying to resolve. Env
                    // MRX_ABL_WARMUP, so both harnesses take it the
                    // same way.
                    //
                    // NOT free for `full`/`full_rocks`: a warm-up pass
                    // doubles the writes the durable path sees, and the
                    // C++ arm's cost is a step function of exactly that
                    // (its ticket ring is 1<<20 and its flusher falls a
                    // full ring behind past ~2.5 laps). Quote a `full`
                    // number WITH its write count.
                    for _ in 0..warmup {
                        for i in 0..ops {
                            let n = t * ops + i;
                            work.run(t, n as u64 + 1, &keys[n], value);
                        }
                    }
                    barrier.wait();
                    for i in 0..ops {
                        let n = t * ops + i;
                        work.run(t, n as u64 + 1, &keys[n], value);
                    }
                })
            })
            .collect();
        barrier.wait();
        let c0 = cpu_secs();
        let t0 = Instant::now();
        for h in handles {
            h.join().expect("writer thread panicked");
        }
        (t0.elapsed(), cpu_secs() - c0)
    });
    let (elapsed, cpu) = elapsed;

    if let Some(rt) = rt.as_mut() {
        rt.abort();
    }

    let total = (threads * ops) as f64;
    println!(
        "threads={threads:3} mode={mode:9} keyspace={:8} blob_us={blob_us:5} \
         {:9.0} ops/s  cpu={:5.2} ({:.3}s)",
        if keyspace == 0 { threads * ops } else { keyspace },
        total / elapsed.as_secs_f64(),
        cpu / elapsed.as_secs_f64(),
        elapsed.as_secs_f64()
    );
}
