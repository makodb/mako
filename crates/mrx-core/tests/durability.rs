//! The three durability properties, ported from
//! `tests/test_masstree_rocks_crash.cc`.
//!
//! 1. **If the watermark is past a write, that write is durable** —
//!    recoverable after a crash, a reboot, whatever.
//! 2. **If it is not, the write may be lost** — but the system must boot
//!    normally afterwards (it cannot be corrupted), and a write is either
//!    fully there or not there at all. Never partially.
//! 3. **A maintenance exit loses nothing** — the system waits until
//!    everything acked is durable before it exits.
//!
//! The C++ suite gets these by forking a child through `/proc/self/exe`
//! and SIGKILLing it, because the durable store is a real RocksDB on
//! disk. Here the durable store is [`MemBlobs`], which survives its
//! store, so a crash is modelled by [`Runtime::abort`] — stop the threads
//! where they stand, throw away every byte of in-memory state, and reopen
//! over the same durable bytes. That models strictly *more* loss than a
//! real crash: a real one may still have OS page cache in flight, whereas
//! this discards the entire cache tier atomically.
//!
//! The reason this is worth doing at all rather than trusting the C++
//! version: the properties are about *this* implementation's watermark
//! and flusher, and a port can lose any of them while every functional
//! test stays green.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Barrier};
use std::time::Duration;

use mrx_core::fakes::{MemBlobs, MemIndex};
use mrx_core::{Config, Runtime, Store};

type S = Store<Arc<MemIndex>, Arc<MemBlobs>>;

fn open(cfg: Config, blobs: &Arc<MemBlobs>) -> Arc<S> {
    Arc::new(Store::open(cfg, Arc::new(MemIndex::new()), Arc::clone(blobs)).expect("open"))
}

fn slow_config() -> Config {
    // A backlog has to be able to EXIST for any of this to be
    // observable. With an instant durable store the flusher keeps up and
    // every property below passes vacuously.
    Config {
        writeback_chunk: 16,
        ..Config::default()
    }
}

// ===================================================================
// Property 1: past the watermark means durable
// ===================================================================

#[test]
fn writes_below_the_watermark_survive_a_crash() {
    let blobs = Arc::new(MemBlobs::new());
    let expected: BTreeMap<String, String> = (0..500)
        .map(|i| (format!("k{i:04}"), format!("v{i}")))
        .collect();

    {
        let store = open(Config::default(), &blobs);
        let mut rt = Runtime::start(Arc::clone(&store));
        for (k, v) in &expected {
            store.put(k.as_bytes(), v.as_bytes());
        }
        assert!(store.sync(), "the barrier must succeed with healthy IO");
        let w = store.watermark();
        assert!(w > 0, "the watermark never advanced");

        // Every published version is at or below W, so all of it is
        // claimed durable. Crash here.
        for k in expected.keys() {
            assert!(
                store.version_of(k.as_bytes()).expect("known key") <= w,
                "{k} is above the watermark, so this proves nothing"
            );
        }
        rt.abort();
    }

    let re = open(Config::default(), &blobs);
    for (k, v) in &expected {
        let got = re.get(k.as_bytes()).expect("get");
        assert_eq!(
            got.as_deref(),
            Some(v.as_bytes()),
            "{k} was below the watermark and did not survive the crash"
        );
    }
}

#[test]
fn the_watermark_is_a_promise_not_an_estimate() {
    // Stronger than the above, and the one that actually pins the
    // low-water semantics: crash at an ARBITRARY moment under load, then
    // check every key whose version is at or below the surviving
    // watermark. Not one of them may be missing or stale.
    let blobs = Arc::new(MemBlobs::new());
    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    let mut expected: BTreeMap<String, String> = BTreeMap::new();

    // Phase 1, at full speed, then a barrier: these are unambiguously
    // below the watermark.
    for i in 0..500 {
        let (k, v) = (format!("a{i:05}"), format!("v{i}"));
        store.put(k.as_bytes(), v.as_bytes());
        expected.insert(k, v);
    }
    assert!(store.sync());

    // Phase 2, outrunning a 2ms-per-batch durable store, so the flusher
    // is provably behind when the crash lands. Without this the test runs
    // entirely inside one regime or the other and proves nothing.
    blobs.set_write_delay_us(2000);
    for i in 0..5000 {
        let (k, v) = (format!("b{i:05}"), format!("v{i}"));
        store.put(k.as_bytes(), v.as_bytes());
        expected.insert(k, v);
    }

    let w = store.watermark();
    let below: Vec<(String, String)> = expected
        .iter()
        .filter(|(k, _)| store.version_of(k.as_bytes()).is_some_and(|ver| ver <= w))
        .map(|(k, v)| (k.clone(), v.clone()))
        .collect();
    rt.abort();
    drop(store);
    blobs.set_write_delay_us(0);

    assert!(
        below.len() >= 500,
        "the barrier-covered phase was not below the watermark ({} keys)",
        below.len()
    );
    assert!(
        below.len() < expected.len(),
        "everything was already durable, so the crash window was empty \
         and this proves nothing"
    );

    let re = open(Config::default(), &blobs);
    for (k, v) in &below {
        assert_eq!(
            re.get(k.as_bytes()).expect("get").as_deref(),
            Some(v.as_bytes()),
            "{k} was at or below the watermark and did not survive"
        );
    }
}

// ===================================================================
// Property 2: above the watermark may be lost, never corrupt
// ===================================================================

#[test]
fn a_crash_above_the_watermark_loses_writes_but_never_corrupts() {
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(200);

    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));
    let mut written: BTreeMap<String, String> = BTreeMap::new();
    for i in 0..3000 {
        let (k, v) = (format!("k{i:05}"), format!("v{i}"));
        store.put(k.as_bytes(), v.as_bytes());
        written.insert(k, v);
    }
    rt.abort();
    drop(store);

    // The store must BOOT.
    let re = open(Config::default(), &blobs);

    // And every key present must be *exactly* one of the values written
    // for it — never a prefix, never a splice of two. This is the
    // "not partially there" half of the property.
    let mut survived = 0;
    let mut n = 0;
    re.scan(b"", |k, v| {
        n += 1;
        let key = String::from_utf8(k.to_vec()).expect("utf8");
        let want = written
            .get(&key)
            .unwrap_or_else(|| panic!("reopen produced a key that was never written: {key}"));
        assert_eq!(
            v,
            want.as_bytes(),
            "{key} came back partially written or spliced"
        );
        survived += 1;
        true
    })
    .expect("scan");

    assert!(survived > 0, "everything was lost, which is not the claim");
    assert!(
        survived <= written.len(),
        "reopen produced more keys than were ever written"
    );

    // Losing the tail is allowed. Silently keeping a *stale* value for a
    // key whose newer version is also durable would not be, and the scan
    // above is what rules it out.
    assert_eq!(n, survived);
}

#[test]
fn a_crash_never_resurrects_a_durably_deleted_key() {
    let blobs = Arc::new(MemBlobs::new());
    let store = open(Config::default(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    for i in 0..200 {
        store.put(format!("k{i:03}").as_bytes(), b"v");
    }
    assert!(store.sync());
    for i in (0..200).step_by(2) {
        store.remove(format!("k{i:03}").as_bytes());
    }
    assert!(store.sync(), "the deletes must be durable before the crash");
    rt.abort();
    drop(store);

    let re = open(Config::default(), &blobs);
    for i in 0..200 {
        let k = format!("k{i:03}");
        let got = re.get(k.as_bytes()).expect("get");
        if i % 2 == 0 {
            assert_eq!(got, None, "{k} was durably deleted and came back");
        } else {
            assert_eq!(got.as_deref(), Some(&b"v"[..]), "{k} was lost");
        }
    }
}

#[test]
fn reopening_a_crashed_store_is_itself_crash_safe() {
    // Two crashes in a row. The second reopen must still boot and must
    // not compound the first one's loss.
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(100);

    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));
    for i in 0..1000 {
        store.put(format!("a{i:04}").as_bytes(), b"first");
    }
    rt.abort();
    drop(store);

    let after_first = blobs.snapshot();

    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));
    for i in 0..1000 {
        store.put(format!("b{i:04}").as_bytes(), b"second");
    }
    rt.abort();
    drop(store);

    let re = open(Config::default(), &blobs);
    for (k, v) in &after_first {
        assert_eq!(
            re.get(k).expect("get").as_deref(),
            Some(v.as_slice()),
            "the second crash lost a key the first one had made durable"
        );
    }
}

// ===================================================================
// Property 3: a maintenance exit loses nothing
// ===================================================================

#[test]
fn a_clean_exit_makes_every_acked_write_durable() {
    // The shutdown barrier. Deliberately run against a SLOW durable
    // store: with an instant one the flusher keeps up by luck and this
    // passes even with the barrier deleted.
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(200);

    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    let mut expected: BTreeMap<String, String> = BTreeMap::new();
    for i in 0..2000 {
        let (k, v) = (format!("k{i:05}"), format!("v{i}"));
        store.put(k.as_bytes(), v.as_bytes());
        expected.insert(k, v);
    }
    // There must be a real backlog at this point, or the barrier has
    // nothing to wait for and the test proves nothing.
    assert!(
        store.watermark() < 2000,
        "the flusher already caught up; this test has no window"
    );

    assert!(rt.shutdown(), "a clean exit must succeed with healthy IO");
    drop(store);

    let re = open(Config::default(), &blobs);
    for (k, v) in &expected {
        assert_eq!(
            re.get(k.as_bytes()).expect("get").as_deref(),
            Some(v.as_bytes()),
            "{k} was acked before a clean exit and was lost anyway"
        );
    }
}

#[test]
fn a_clean_exit_under_high_concurrency_recovers_everything() {
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(100);

    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));
    let barrier = Arc::new(Barrier::new(8));

    std::thread::scope(|scope| {
        for t in 0..8u64 {
            let store = Arc::clone(&store);
            let barrier = Arc::clone(&barrier);
            scope.spawn(move || {
                barrier.wait();
                // DISTINCT keys, written exactly once. Overwrite-based
                // traffic masks a lost write, because a later write to
                // the same key covers for it — which is precisely why
                // the C++ suite's overwrite tests missed this.
                for i in 0..400u64 {
                    store.put(
                        format!("t{t}-{i:04}").as_bytes(),
                        format!("{t}:{i}").as_bytes(),
                    );
                }
            });
        }
    });

    assert!(rt.shutdown());
    drop(store);

    let re = open(Config::default(), &blobs);
    for t in 0..8u64 {
        for i in 0..400u64 {
            assert_eq!(
                re.get(format!("t{t}-{i:04}").as_bytes())
                    .expect("get")
                    .as_deref(),
                Some(format!("{t}:{i}").as_bytes()),
                "t{t}-{i:04} was lost across a clean exit"
            );
        }
    }
}

#[test]
fn a_clean_exit_with_removes_recovers_everything() {
    // Tombstones must survive a clean exit rather than the key coming
    // back on reopen. Every fifth op removes a key written two
    // iterations earlier, so most keys are written exactly once and a
    // lost write cannot be masked.
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(100);

    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    let mut live: BTreeMap<String, String> = BTreeMap::new();
    let mut dead: Vec<String> = Vec::new();
    for i in 0..2000u64 {
        let (k, v) = (format!("k{i:05}"), format!("v{i}"));
        store.put(k.as_bytes(), v.as_bytes());
        live.insert(k, v);
        if i % 5 == 4 && i >= 2 {
            let victim = format!("k{:05}", i - 2);
            store.remove(victim.as_bytes());
            live.remove(&victim);
            dead.push(victim);
        }
    }
    assert!(rt.shutdown());
    drop(store);

    let re = open(Config::default(), &blobs);
    for (k, v) in &live {
        assert_eq!(
            re.get(k.as_bytes()).expect("get").as_deref(),
            Some(v.as_bytes()),
            "{k} was lost across a clean exit"
        );
    }
    for k in &dead {
        assert_eq!(
            re.get(k.as_bytes()).expect("get"),
            None,
            "{k} was durably deleted and came back after a clean exit"
        );
    }
}

#[test]
fn a_clean_exit_reports_failure_rather_than_losing_writes_silently() {
    // The honest-failure case: if the durable store will not accept
    // writes, shutdown must say so. Exiting with a cheerful `true` while
    // dropping acked writes is the one outcome that is not allowed.
    let blobs = Arc::new(MemBlobs::new());
    let store = open(Config::default(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    blobs.fail_next_writes(usize::MAX);
    for i in 0..100 {
        store.put(format!("k{i}").as_bytes(), b"v");
    }
    assert!(
        !rt.shutdown(),
        "shutdown claimed success while the durable store refused writes"
    );
}

#[test]
fn the_barrier_does_not_return_early_under_sustained_writes() {
    // sync() must cover everything acked BEFORE the call, and must not be
    // fooled by traffic that arrives during it.
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(50);
    let store = open(slow_config(), &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    for i in 0..500 {
        store.put(format!("pre{i:04}").as_bytes(), b"v");
    }
    let stop = Arc::new(AtomicU64::new(0));
    std::thread::scope(|scope| {
        {
            let store = Arc::clone(&store);
            let stop = Arc::clone(&stop);
            scope.spawn(move || {
                let mut n = 0u64;
                while stop.load(Ordering::Acquire) == 0 {
                    store.put(format!("post{n}").as_bytes(), b"v");
                    n += 1;
                    // Throttled deliberately. An unthrottled writer
                    // outruns a 50us-per-batch durable store, which is
                    // the sustained-overload regime where the watermark
                    // correctly stops advancing and sync() correctly
                    // never returns — see the test below. That is a
                    // different property, and mixing the two here just
                    // hangs.
                    std::thread::sleep(std::time::Duration::from_micros(200));
                }
            });
        }
        assert!(store.sync(), "barrier failed with healthy IO");
        for i in 0..500 {
            assert!(
                blobs.peek(format!("pre{i:04}").as_bytes()).is_some(),
                "sync() returned before pre{i:04} was durable"
            );
        }
        stop.store(1, Ordering::Release);
    });
    rt.abort();
}

#[test]
fn the_flusher_never_blocks_on_the_backpressure_it_relieves() {
    // A DEADLOCK REGRESSION, and the one gap every other test in this
    // file shared: none of them ever FILLED the ticket log.
    //
    // The flusher steals partial batches from idle writers. When the log
    // has no room for a stolen batch, the flusher must hold it and retry
    // — never wait for room, because it is the only thing that makes
    // room. Waiting deadlocks it against itself and every producer then
    // sleeps forever behind log backpressure.
    //
    // A tiny log plus a durable store slow enough to keep it full is
    // what reproduces it. Before the fix this hung with all threads in
    // state S; the harness bounds it so a regression fails instead.
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(500);
    let cfg = Config {
        log_slots: 64,      // fills almost immediately
        batch: 8,           // so partial batches are common
        writeback_chunk: 4, // and writeback cannot keep up
        ..Config::default()
    };
    let store = open(cfg, &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    let done = Arc::new(AtomicU64::new(0));

    // DETACHED threads, not `std::thread::scope`.
    //
    // Scoped threads are joined on the way out of the closure — including
    // while unwinding from a panic — so a watchdog inside a scope can
    // detect the deadlock and then block forever trying to join the very
    // threads that are stuck. The first version of this test did exactly
    // that: it noticed the hang and hung anyway, which is indistinguishable
    // from having no watchdog at all.
    let mut handles = Vec::new();
    for t in 0..4u64 {
        let store = Arc::clone(&store);
        let done = Arc::clone(&done);
        handles.push(std::thread::spawn(move || {
            for i in 0..500u64 {
                store.put(format!("t{t}-{i:04}").as_bytes(), b"v");
                done.fetch_add(1, Ordering::Release);
            }
        }));
    }

    // A deadlock here is a HANG, and a hung test the runner eventually
    // kills reports as an infrastructure problem rather than as this bug.
    // Failing loudly is worth the plumbing.
    let mut last = 0u64;
    let mut stalled_since = std::time::Instant::now();
    loop {
        let n = done.load(Ordering::Acquire);
        if n >= 2000 {
            break;
        }
        if n != last {
            last = n;
            stalled_since = std::time::Instant::now();
        } else if stalled_since.elapsed() > Duration::from_secs(20) {
            panic!(
                "no progress for 20s at {n}/2000 writes: the flusher is \
                 blocked on log backpressure it is itself responsible for \
                 relieving (stash={}, dirty={})",
                store.stash_len(),
                store.dirty_len()
            );
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    for h in handles {
        h.join().expect("writer thread panicked");
    }

    // And nothing was lost on the way through the stash.
    assert!(rt.shutdown(), "clean shutdown after backpressure");
    drop(store);
    let re = open(Config::default(), &blobs);
    for t in 0..4u64 {
        for i in 0..500u64 {
            assert_eq!(
                re.get(format!("t{t}-{i:04}").as_bytes())
                    .expect("get")
                    .as_deref(),
                Some(&b"v"[..]),
                "t{t}-{i:04} was lost while the log was full"
            );
        }
    }
}

#[test]
fn sustained_overload_pins_the_watermark_rather_than_lying() {
    // The documented consequence of W being a LOW-water mark: when
    // writes arrive faster than the durable store can absorb them, the
    // oldest undischarged obligation never retires and W stops advancing
    // entirely. Writes stay acked and readable; nothing becomes durable.
    //
    // This is correct, and it is worth a test precisely because it looks
    // like a bug. Any future "fix" that lets W advance under backlog is a
    // data-loss bug wearing a performance hat, and it will fail here.
    let blobs = Arc::new(MemBlobs::new());
    blobs.set_write_delay_us(2000);
    let cfg = Config {
        writeback_chunk: 4,
        ..Config::default()
    };
    let store = open(cfg, &blobs);
    let mut rt = Runtime::start(Arc::clone(&store));

    for i in 0..20_000u64 {
        store.put(format!("k{i:06}").as_bytes(), b"v");
    }
    let w = store.watermark();
    assert!(
        w < 20_000,
        "the watermark ({w}) covered writes the durable store cannot \
         possibly have absorbed yet"
    );
    // ...and the data is all still readable, which is the other half of
    // the trade.
    for i in (0..20_000u64).step_by(997) {
        assert_eq!(
            store
                .get(format!("k{i:06}").as_bytes())
                .expect("get")
                .as_deref(),
            Some(&b"v"[..]),
            "an acked write became unreadable under backlog"
        );
    }
    rt.abort();
}

// ===================================================================
// Property 4: the guarantees hold for a thread that touches more than
// one store, and for a drain that runs beside the flusher
// ===================================================================
//
// These were live bugs. They are grouped because they share a
// shape: an invariant the C++ implementation gets structurally, which
// the port lost while reorganising the code around it.

#[test]
fn a_thread_writing_to_two_stores_gets_a_sound_watermark_on_both() {
    // mako opens one index per table, so a worker thread writes to many
    // stores. The writer-slot cache used to be a single unkeyed
    // `Option<usize>`: the FIRST store a thread wrote to captured it, and
    // every other store then handed back that index without incrementing
    // its own `next_writer`. `live_writers()` stayed empty, so
    // `Floors::min_over` saw no floors at all, `recompute_watermark` fell
    // back to `m = counter`, and W jumped over the thread's announce
    // floor and its never-stolen partial batch.
    //
    // The symptom is the worst one this system has: `sync()` returning
    // TRUE over writes the durable store never received. Store A -- the
    // thread's first -- was always clean, which is what made it easy to
    // miss.
    let blobs_a = Arc::new(MemBlobs::new());
    let blobs_b = Arc::new(MemBlobs::new());
    let a = open(Config::default(), &blobs_a);
    let b = open(Config::default(), &blobs_b);
    let mut rt_a = Runtime::start(Arc::clone(&a));
    let mut rt_b = Runtime::start(Arc::clone(&b));

    // Interleaved, as a transaction touching two tables does.
    for k in 0..2000u32 {
        a.put(format!("a{k:05}").as_bytes(), b"value-a");
        b.put(format!("b{k:05}").as_bytes(), b"value-b");
    }

    assert!(a.sync(), "A sync");
    assert!(b.sync(), "B sync");
    let (da, db) = (blobs_a.snapshot().len(), blobs_b.snapshot().len());
    rt_a.shutdown();
    rt_b.shutdown();

    assert_eq!(da, 2000, "store A lost acked writes across sync()");
    assert_eq!(db, 2000, "store B lost acked writes across sync()");
}

#[test]
fn rotating_across_more_than_eight_stores_reuses_writer_slots() {
    // The eight-way array in `writer_index` is a hot cache, not the
    // registry. If eviction forgets the authoritative mapping, a cyclic
    // nine-store workload misses on every access and permanently burns a
    // slot in each store until the process aborts. TPC-C touches more than
    // eight tables, so this is a production-shaped working set.
    let stores: Vec<(Arc<S>, Arc<MemBlobs>)> = (0..9)
        .map(|_| {
            let blobs = Arc::new(MemBlobs::new());
            (open(Config::tiny(), &blobs), blobs)
        })
        .collect();

    for round in 0..600u32 {
        for (store, _) in &stores {
            store.put(format!("k{round:04}").as_bytes(), b"value");
            // Keep each tiny log moving without adding nine background
            // threads; the registry behavior is entirely on this writer.
            store.flush_cycle();
        }
    }

    for (store, blobs) in &stores {
        assert_eq!(
            store.registered_writer_slots(),
            1,
            "one thread must own exactly one slot in each store"
        );
        assert!(store.drain_fully(), "drain");
        assert_eq!(blobs.snapshot().len(), 600, "lost durable rows");
    }
}

#[test]
fn writer_registry_covers_production_masstree_thread_limit() {
    // Production Masstree provides 512 process-wide, non-recycled thread
    // attachments. Exercise the same number of distinct thread lifetimes
    // here rather than merely 512 simultaneous calls.
    const MASSTREE_MAX_THREADS: usize = 512;
    let blobs = Arc::new(MemBlobs::new());
    let store = open(
        Config {
            log_slots: 512,
            writeback_chunk: 512,
            ..Config::default()
        },
        &blobs,
    );

    for i in 0..MASSTREE_MAX_THREADS {
        let s = Arc::clone(&store);
        std::thread::spawn(move || {
            s.put(format!("k{i:04}").as_bytes(), b"value");
        })
        .join()
        .expect("writer");
    }

    assert_eq!(store.registered_writer_slots(), MASSTREE_MAX_THREADS);
    assert!(store.drain_fully(), "drain");
    assert_eq!(blobs.snapshot().len(), MASSTREE_MAX_THREADS);
}

#[test]
fn draining_beside_the_flusher_does_not_write_back_stale_bytes() {
    // `drain_fully` runs `flush_cycle` on the CALLER's thread, and both
    // `clear()` and `Runtime::shutdown()` call it while the flusher
    // thread is still live -- shutdown deliberately drains BEFORE it
    // stops. So two cycles ran at once.
    //
    // Two cycles can select the same entry, read different published
    // versions of it, and have the OLDER batch land last: the durable
    // store then holds stale bytes while W says the newer version is
    // durable. Both then erase the entry unconditionally, so nothing is
    // left owing it and the flusher never revisits it.
    //
    // C++ gets this for free -- `mrx_flusher_cycle` is only ever reached
    // from `mrx_flusher_loop`, and its barrier waits on a condvar rather
    // than running a cycle. The port made `flush_cycle` public.
    for round in 0..40 {
        let blobs = Arc::new(MemBlobs::new());
        blobs.set_write_delay_us(300);
        let store = open(
            Config {
                writeback_chunk: 8,
                ..Config::default()
            },
            &blobs,
        );

        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let mut flushers = Vec::new();
        for _ in 0..2 {
            let s = Arc::clone(&store);
            let stop = Arc::clone(&stop);
            flushers.push(std::thread::spawn(move || {
                while !stop.load(Ordering::Acquire) {
                    s.flush_cycle();
                }
            }));
        }
        {
            let s = Arc::clone(&store);
            std::thread::spawn(move || {
                for gen in 0..60u32 {
                    for k in 0..8u32 {
                        s.put(
                            format!("k{k}").as_bytes(),
                            format!("v{k}-{gen:04}").as_bytes(),
                        );
                    }
                    std::thread::sleep(Duration::from_micros(50));
                }
            })
            .join()
            .expect("writer");
        }
        assert!(store.drain_fully(), "drain");
        stop.store(true, Ordering::Release);
        for f in flushers {
            f.join().expect("flusher");
        }

        for k in 0..8u32 {
            let key = format!("k{k}");
            let cached = store.get(key.as_bytes()).expect("get").expect("present");
            assert_eq!(
                blobs.peek(key.as_bytes()).as_deref(),
                Some(&cached[..]),
                "round {round}: {key} is durable at bytes that are not the \
                 last acked write"
            );
        }
    }
}

#[test]
fn clear_leaves_nothing_in_the_durable_store() {
    // `clear()` is the end-to-end version of both bugs above: it deletes
    // every key through the ordinary write path and then drains on its
    // own thread while the flusher runs, so it races itself. It reported
    // success while rows survived -- and since the cache refills from the
    // durable store, those rows come back on reopen.
    for round in 0..40 {
        let blobs = Arc::new(MemBlobs::new());
        let store = open(
            Config {
                writeback_chunk: 8,
                ..Config::default()
            },
            &blobs,
        );
        let mut rt = Runtime::start(Arc::clone(&store));

        for k in 0..400u32 {
            store.put(format!("k{k:04}").as_bytes(), b"payload-payload-payload");
        }
        blobs.set_write_delay_us(200);
        assert!(store.clear(), "round {round}: clear reported not-durable");
        blobs.set_write_delay_us(0);
        rt.shutdown();

        let left = blobs.snapshot();
        assert!(
            left.is_empty(),
            "round {round}: clear() succeeded but {} rows are still durable, \
             e.g. {:?}",
            left.len(),
            left.keys().next()
        );
    }
}
