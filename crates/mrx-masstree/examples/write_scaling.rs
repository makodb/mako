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
//!   ...and four ablations, each removing one layer, so the cost of a
//!   write can be attributed rather than guessed at:
//!
//!         atomic     ONE shared fetch_add per op. The floor any design
//!                    with a global version counter has to live above.
//!         index      masstree get_or_insert only. The ceiling.
//!         entry      index + entry table + publish under the entry's
//!                    lock. Adds everything except versioning and the
//!                    ticket log.
//!         alloc      `entry` without the per-write allocations, to
//!                    separate allocator cost from lock cost.
//!
//!   Read-side ablations, which answer a different question: how much of
//!   a read is the Arc refcount traffic that epoch reclamation would
//!   remove?
//!
//!         rd_index   masstree lookup only
//!         rd_entry   + entry table + `Entry::load` (the Arc clone/drop
//!                    and the slot lock)
//!         rd_full    + copying the value out, i.e. `Store::get`
//!         floors     `entry` plus the announce floor and the version
//!                    draw, but no ticket. Splits the durability
//!                    bookkeeping into its memory-ordering half and its
//!                    locking half, which is the difference between a
//!                    cost inherent to the design and one that comes
//!                    from choosing a futex Mutex over a spinlock.
//! ```
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

use mrx_core::fakes::MemBlobs;
use mrx_core::{Config, EntryTable, KeyIndex, Runtime, Store, Val};
use mrx_masstree::MasstreeIndex;

/// What one op does, per mode.
enum Work {
    Store(Arc<Store<MasstreeIndex, MemBlobs>>),
    Index(Arc<MasstreeIndex>, Arc<AtomicU64>),
    Entry(
        Arc<MasstreeIndex>,
        Arc<EntryTable>,
        Arc<AtomicU64>,
        bool,
        Arc<mrx_core::Val>,
    ),
    Atomic(Arc<AtomicU64>),
    /// (index, table, how far to go: 0 = lookup, 1 = +load, 2 = +copy)
    Read(Arc<MasstreeIndex>, Arc<EntryTable>, u8),
    Floors(
        Arc<MasstreeIndex>,
        Arc<EntryTable>,
        Arc<AtomicU64>,
        Arc<mrx_core::WriterSlot>,
    ),
}

impl Work {
    fn run(&self, key: &[u8], value: &[u8]) {
        match self {
            Work::Store(s) => {
                s.put(key, value);
            }
            Work::Atomic(c) => {
                c.fetch_add(1, Ordering::SeqCst);
            }
            Work::Index(idx, next) => {
                let w = next.fetch_add(1, Ordering::Relaxed) + 1;
                idx.get_or_insert(key, w);
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
            Work::Floors(idx, table, ver, slot) => {
                let word = match idx.get(key) {
                    Some(w) => w,
                    None => {
                        let e = mrx_core::Entry::new(key, Val::tombstone(0));
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
                e.with_slot(|_| (Some(Val::resident(v, value.to_vec())), ()));
                slot.disarm();
            }
            Work::Entry(idx, table, ver, allocate, shared) => {
                // The cache's write path with versioning and the ticket
                // log removed: find or create the entry, then publish a
                // new value under its lock.
                let word = match idx.get(key) {
                    Some(w) => w,
                    None => {
                        let e = mrx_core::Entry::new(key, Val::tombstone(0));
                        let idx_new = table.push(e);
                        idx.get_or_insert(key, u64::from(idx_new) + 1)
                    }
                };
                let e = table.get((word - 1) as u32);
                let v = ver.fetch_add(1, Ordering::SeqCst);
                // `allocate = false` publishes a clone of one pre-built
                // record: one atomic increment instead of two
                // allocations, and — this part matters — the SAME single
                // acquisition of the entry lock. An earlier version read
                // the current value first, which took the lock twice and
                // charged the difference to allocation.
                let nv = if *allocate {
                    Val::resident(v, value.to_vec())
                } else {
                    Arc::clone(shared)
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
            Work::Index(
                Arc::new(MasstreeIndex::new().expect("masstree")),
                Arc::clone(&counter),
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
                    Val::resident(1, vec![b'v'; 100]),
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
                Arc::new(mrx_core::WriterSlot::new()),
            ),
            None,
        ),
        "entry" | "alloc" => (
            Work::Entry(
                Arc::new(MasstreeIndex::new().expect("masstree")),
                Arc::new(EntryTable::new()),
                Arc::clone(&counter),
                mode == "entry",
                Val::resident(0, vec![b'v'; 100]),
            ),
            None,
        ),
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
                    barrier.wait();
                    for i in 0..ops {
                        work.run(&keys[t * ops + i], value);
                    }
                })
            })
            .collect();
        barrier.wait();
        let t0 = Instant::now();
        for h in handles {
            h.join().expect("writer thread panicked");
        }
        t0.elapsed()
    });

    if let Some(rt) = rt.as_mut() {
        rt.abort();
    }

    let total = (threads * ops) as f64;
    println!(
        "threads={threads:3} mode={mode:9} keyspace={:8} blob_us={blob_us:5} \
         {:9.0} ops/s  ({:.3}s)",
        if keyspace == 0 { threads * ops } else { keyspace },
        total / elapsed.as_secs_f64(),
        elapsed.as_secs_f64()
    );
}
