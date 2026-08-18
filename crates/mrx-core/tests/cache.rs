//! The C++ cache suite, ported.
//!
//! Each test here corresponds to one in
//! `tests/test_masstree_rocks_cache.cc`, which is mutation-verified 5/5.
//! Keeping the correspondence one-to-one is the point: the C++ version is
//! the oracle, so a property that exists there and not here is a property
//! this port has silently dropped.
//!
//! Three of them differ from their C++ counterpart in *method* rather than
//! in what they assert. The C++ versions sleep and hope the background
//! flusher does something; here the flusher is driven a cycle at a time,
//! which turns a timing-dependent test into a deterministic one.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Barrier};

use mrx_core::fakes::{MemBlobs, MemIndex};
use mrx_core::{Config, KeyIndex, Runtime, Store};

type S = Store<Arc<MemIndex>, Arc<MemBlobs>>;

/// A store with no background threads: the flusher is driven by hand.
struct Fixture {
    store: Arc<S>,
    blobs: Arc<MemBlobs>,
    index: Arc<MemIndex>,
}

impl Fixture {
    fn new() -> Self {
        Self::with(Config::default(), MemBlobs::new())
    }

    fn with_capacity(cap: u64) -> Self {
        let cfg = Config { capacity_bytes: Some(cap), ..Config::default() };
        Self::with(cfg, MemBlobs::new())
    }

    fn with(cfg: Config, blobs: MemBlobs) -> Self {
        let blobs = Arc::new(blobs);
        let index = Arc::new(MemIndex::new());
        let store = Arc::new(
            Store::open(cfg, Arc::clone(&index), Arc::clone(&blobs)).expect("open"),
        );
        Self { store, blobs, index }
    }

    /// Reopen over the same durable store with a fresh index and cache,
    /// as a restart would.
    fn reopen(&self, cfg: Config) -> Arc<S> {
        let index = Arc::new(MemIndex::new());
        Arc::new(Store::open(cfg, index, Arc::clone(&self.blobs)).expect("reopen"))
    }

    fn put(&self, k: &str, v: &str) -> bool {
        self.store.put(k.as_bytes(), v.as_bytes()).wrote
    }

    fn get(&self, k: &str) -> Option<String> {
        self.store
            .get(k.as_bytes())
            .expect("get")
            .map(|b| String::from_utf8(b).expect("utf8"))
    }

    fn flush(&self) -> bool {
        self.store.drain_fully()
    }
}

fn get_of(s: &S, k: &str) -> Option<String> {
    s.get(k.as_bytes())
        .expect("get")
        .map(|b| String::from_utf8(b).expect("utf8"))
}

// ===================================================================
// Basic visibility and durability
// ===================================================================

#[test]
fn write_is_visible_immediately() {
    let f = Fixture::new();
    assert!(f.put("k", "v"));
    assert_eq!(f.get("k").as_deref(), Some("v"));
    // ...and specifically BEFORE it is durable. That is the trade the
    // whole design exists to make.
    assert_eq!(f.blobs.peek(b"k"), None);
}

#[test]
fn flush_makes_writes_durable() {
    let f = Fixture::new();
    for i in 0..64 {
        assert!(f.put(&format!("k{i}"), &format!("v{i}")));
    }
    assert!(f.flush());
    for i in 0..64 {
        assert_eq!(
            f.blobs.peek(format!("k{i}").as_bytes()).as_deref(),
            Some(format!("v{i}").as_bytes()),
            "key {i} acked but never reached the durable store"
        );
    }
}

#[test]
fn flush_on_idle_store_succeeds() {
    let f = Fixture::new();
    assert!(f.flush(), "an empty store is trivially durable");
    assert!(f.flush(), "and stays that way");
}

#[test]
fn overwrite_flushes_latest_value() {
    let f = Fixture::new();
    f.put("k", "first");
    f.put("k", "second");
    f.put("k", "third");
    assert!(f.flush());
    assert_eq!(f.blobs.peek(b"k").as_deref(), Some(&b"third"[..]));
}

// ===================================================================
// Existence reporting
// ===================================================================

#[test]
fn put_reports_newly_inserted() {
    let f = Fixture::new();
    assert!(!f.store.put(b"k", b"a").existed, "first put is an insert");
    assert!(f.store.put(b"k", b"b").existed, "second is an overwrite");
}

#[test]
fn insert_refuses_live_key() {
    let f = Fixture::new();
    assert!(f.store.insert(b"k", b"a").wrote);
    let second = f.store.insert(b"k", b"b");
    assert!(!second.wrote);
    assert!(second.existed);
    assert_eq!(f.get("k").as_deref(), Some("a"), "and left the value alone");
}

#[test]
fn remove_reports_existence() {
    let f = Fixture::new();
    assert!(!f.store.remove(b"missing").wrote);
    f.put("k", "v");
    let r = f.store.remove(b"k");
    assert!(r.wrote && r.existed);
    assert!(!f.store.remove(b"k").wrote, "second remove finds a tombstone");
}

#[test]
fn concurrent_inserts_elect_one_winner() {
    // Exactly one thread must observe "I inserted this". Reporting
    // insertion from two threads is how duplicate-key bugs reach callers
    // that use the return value as a uniqueness check.
    let f = Fixture::new();
    let winners = Arc::new(AtomicU64::new(0));
    let barrier = Arc::new(Barrier::new(16));
    std::thread::scope(|scope| {
        for _ in 0..16 {
            let store = Arc::clone(&f.store);
            let winners = Arc::clone(&winners);
            let barrier = Arc::clone(&barrier);
            scope.spawn(move || {
                barrier.wait();
                if store.insert(b"contested", b"v").wrote {
                    winners.fetch_add(1, Ordering::SeqCst);
                }
            });
        }
    });
    assert_eq!(winners.load(Ordering::SeqCst), 1);
}

// ===================================================================
// Deletes
// ===================================================================

#[test]
fn read_after_delete_is_not_found() {
    let f = Fixture::new();
    f.put("k", "v");
    f.store.remove(b"k");
    assert_eq!(f.get("k"), None);
}

#[test]
fn delete_reaches_the_system_of_record() {
    let f = Fixture::new();
    f.put("k", "v");
    assert!(f.flush());
    assert!(f.blobs.peek(b"k").is_some());
    f.store.remove(b"k");
    assert!(f.flush());
    assert_eq!(f.blobs.peek(b"k"), None, "the tombstone must be written back");
}

#[test]
fn reinsert_after_delete_succeeds() {
    let f = Fixture::new();
    f.put("k", "v1");
    f.store.remove(b"k");
    assert!(f.store.insert(b"k", b"v2").wrote, "a tombstone is absent");
    assert_eq!(f.get("k").as_deref(), Some("v2"));
    assert!(f.flush());
    assert_eq!(f.blobs.peek(b"k").as_deref(), Some(&b"v2"[..]));
}

#[test]
fn deleted_key_survives_reopen_as_absent() {
    // The failure mode this pins: the tombstone is applied in memory,
    // never written back, and the key returns from the dead on restart.
    let f = Fixture::new();
    f.put("k", "v");
    assert!(f.flush());
    f.store.remove(b"k");
    assert!(f.flush());

    let re = f.reopen(Config::default());
    assert_eq!(get_of(&re, "k"), None, "a deleted key came back after reopen");
}

#[test]
fn missing_key_is_absent() {
    let f = Fixture::new();
    assert_eq!(f.get("never-written"), None);
}

// ===================================================================
// Reopen and read-through
// ===================================================================

#[test]
fn reopen_loads_every_key() {
    // The key-resident invariant. If open does not intern every durable
    // key, an index miss stops meaning "absent" and every read has to
    // fall through to the durable store.
    let f = Fixture::new();
    for i in 0..200 {
        f.put(&format!("k{i:04}"), &format!("v{i}"));
    }
    assert!(f.flush());

    let re = f.reopen(Config::default());
    assert_eq!(re.len(), 200);
    for i in 0..200 {
        assert_eq!(
            get_of(&re, &format!("k{i:04}")).as_deref(),
            Some(format!("v{i}").as_str())
        );
    }
}

#[test]
fn reopened_values_start_non_resident() {
    let f = Fixture::new();
    f.put("k", "v");
    assert!(f.flush());
    let re = f.reopen(Config::default());
    assert_eq!(
        re.is_resident(b"k"),
        Some(false),
        "open must intern keys without loading values"
    );
}

#[test]
fn read_through_fill_serves_reopened_value() {
    let f = Fixture::new();
    f.put("k", "v");
    assert!(f.flush());
    let re = f.reopen(Config::default());
    assert_eq!(get_of(&re, "k").as_deref(), Some("v"));
    assert_eq!(re.is_resident(b"k"), Some(true), "and caches it on the way");
}

// ===================================================================
// Scans
// ===================================================================

#[test]
fn scan_returns_ascending_range() {
    let f = Fixture::new();
    for i in 0..10 {
        f.put(&format!("k{i}"), &format!("v{i}"));
    }
    let mut seen = Vec::new();
    f.store
        .scan(b"k3", |k, v| {
            seen.push((
                String::from_utf8(k.to_vec()).unwrap(),
                String::from_utf8(v.to_vec()).unwrap(),
            ));
            true
        })
        .expect("scan");
    assert_eq!(seen.len(), 7);
    assert_eq!(seen[0].0, "k3");
    assert_eq!(seen[6].0, "k9");
    assert!(seen.windows(2).all(|w| w[0].0 < w[1].0), "must be ordered");
}

#[test]
fn scan_omits_deleted_keys() {
    let f = Fixture::new();
    for i in 0..10 {
        f.put(&format!("k{i}"), "v");
    }
    f.store.remove(b"k4");
    f.store.remove(b"k7");
    let mut seen = Vec::new();
    f.store
        .scan(b"", |k, _| {
            seen.push(String::from_utf8(k.to_vec()).unwrap());
            true
        })
        .expect("scan");
    assert_eq!(seen.len(), 8);
    assert!(!seen.contains(&"k4".to_string()));
    assert!(!seen.contains(&"k7".to_string()));
}

#[test]
fn scan_stops_when_callback_returns_false() {
    let f = Fixture::new();
    for i in 0..100 {
        f.put(&format!("k{i:03}"), "v");
    }
    let mut n = 0;
    f.store
        .scan(b"", |_, _| {
            n += 1;
            n < 5
        })
        .expect("scan");
    assert_eq!(n, 5, "the scan must stop, not merely ignore the result");
}

#[test]
fn scan_fills_non_resident_values() {
    let f = Fixture::new();
    for i in 0..20 {
        f.put(&format!("k{i:02}"), &format!("v{i}"));
    }
    assert!(f.flush());
    let re = f.reopen(Config::default());

    let mut seen = BTreeMap::new();
    re.scan(b"", |k, v| {
        seen.insert(k.to_vec(), v.to_vec());
        true
    })
    .expect("scan");
    assert_eq!(seen.len(), 20);
    for i in 0..20 {
        assert_eq!(
            seen.get(format!("k{i:02}").as_bytes()).map(|v| v.as_slice()),
            Some(format!("v{i}").as_bytes()),
            "scan served an evicted value incorrectly"
        );
    }
}

#[test]
fn scan_spans_multiple_chunks() {
    // The seam between index chunks is where an off-by-one either drops a
    // key or yields one twice.
    let cfg = Config { scan_chunk: 7, ..Config::default() };
    let f = Fixture::with(cfg, MemBlobs::new());
    for i in 0..100 {
        f.put(&format!("k{i:03}"), &format!("v{i}"));
    }
    let mut seen = Vec::new();
    f.store
        .scan(b"", |k, _| {
            seen.push(k.to_vec());
            true
        })
        .expect("scan");
    assert_eq!(seen.len(), 100, "a chunk seam dropped or duplicated a key");
    let unique: std::collections::BTreeSet<_> = seen.iter().collect();
    assert_eq!(unique.len(), 100);
}

#[test]
fn rscan_returns_descending_range() {
    let f = Fixture::new();
    for i in 0..10 {
        f.put(&format!("k{i}"), "v");
    }
    let mut seen = Vec::new();
    f.store
        .rscan(b"k6", |k, _| {
            seen.push(String::from_utf8(k.to_vec()).unwrap());
            true
        })
        .expect("rscan");
    assert_eq!(seen[0], "k6", "the bound is inclusive");
    assert!(seen.windows(2).all(|w| w[0] > w[1]), "must be descending");
    assert_eq!(seen.len(), 7);
}

// ===================================================================
// The flusher
// ===================================================================

#[test]
fn flush_covers_hot_key_under_concurrent_overwrites() {
    // The false-durability bug, verbatim: the drain confirms an
    // obligation from bytes it snapshotted, so a key overwritten faster
    // than drain latency is reported durable having never been written.
    let f = Fixture::new();
    let stop = Arc::new(AtomicBool::new(false));
    let last = Arc::new(AtomicU64::new(0));

    std::thread::scope(|scope| {
        {
            let store = Arc::clone(&f.store);
            let stop = Arc::clone(&stop);
            let last = Arc::clone(&last);
            scope.spawn(move || {
                let mut n = 0u64;
                while !stop.load(Ordering::Acquire) {
                    n += 1;
                    store.put(b"hot", format!("v{n}").as_bytes());
                    last.store(n, Ordering::Release);
                }
            });
        }
        // Let the writer get well ahead, then stop it and flush.
        std::thread::sleep(std::time::Duration::from_millis(150));
        stop.store(true, Ordering::Release);
    });

    assert!(f.flush());
    let n = last.load(Ordering::Acquire);
    assert!(n > 0, "the writer never ran");
    assert_eq!(
        f.blobs.peek(b"hot").as_deref(),
        Some(format!("v{n}").as_bytes()),
        "flush() reported success for a key that never reached the store"
    );
}

#[test]
fn clear_truncates_both_tiers() {
    let f = Fixture::new();
    for i in 0..50 {
        f.put(&format!("k{i:02}"), "v");
    }
    assert!(f.flush());
    assert!(f.store.clear());

    for i in 0..50 {
        assert_eq!(f.get(&format!("k{i:02}")), None, "cache tier not cleared");
        assert_eq!(
            f.blobs.peek(format!("k{i:02}").as_bytes()),
            None,
            "durable tier not cleared"
        );
    }
    let re = f.reopen(Config::default());
    let mut n = 0;
    re.scan(b"", |_, _| {
        n += 1;
        true
    })
    .expect("scan");
    assert_eq!(n, 0, "keys returned after reopen following a clear");
}

#[test]
fn overwrite_storm_ends_consistent() {
    let f = Fixture::new();
    for round in 0..200 {
        for i in 0..20 {
            f.put(&format!("k{i:02}"), &format!("r{round}"));
        }
    }
    assert!(f.flush());
    for i in 0..20 {
        assert_eq!(
            f.blobs.peek(format!("k{i:02}").as_bytes()).as_deref(),
            Some(&b"r199"[..]),
            "the durable store holds a stale value after flush"
        );
    }
}

#[test]
fn concurrent_writers_and_readers_stay_consistent() {
    let f = Fixture::new();
    let mut rt = Runtime::start(Arc::clone(&f.store));
    let barrier = Arc::new(Barrier::new(17));

    std::thread::scope(|scope| {
        for t in 0..16u64 {
            let store = Arc::clone(&f.store);
            let barrier = Arc::clone(&barrier);
            scope.spawn(move || {
                barrier.wait();
                for i in 0..500u64 {
                    let k = format!("t{t}-k{i:04}");
                    store.put(k.as_bytes(), format!("{t}:{i}").as_bytes());
                    // Read back what this thread just wrote: no other
                    // thread touches this key, so a mismatch is a real
                    // violation and not a race.
                    let got = store.get(k.as_bytes()).expect("get");
                    assert_eq!(
                        got.as_deref(),
                        Some(format!("{t}:{i}").as_bytes()),
                        "read-your-own-write violated"
                    );
                }
            });
        }
        barrier.wait();
    });

    assert!(rt.shutdown(), "shutdown must drain everything");
    for t in 0..16u64 {
        for i in 0..500u64 {
            assert_eq!(
                f.blobs.peek(format!("t{t}-k{i:04}").as_bytes()).as_deref(),
                Some(format!("{t}:{i}").as_bytes()),
                "an acked write never became durable"
            );
        }
    }
}

// ===================================================================
// IO failure
// ===================================================================

#[test]
fn flush_reports_failure_while_io_is_failing() {
    let f = Fixture::new();
    f.blobs.fail_next_writes(usize::MAX);
    f.put("k", "v");
    assert!(!f.flush(), "flush must not claim success while writes fail");
}

#[test]
fn watermark_stays_pinned_while_io_is_failing() {
    let f = Fixture::new();
    f.blobs.fail_next_writes(usize::MAX);
    for i in 0..100 {
        f.put(&format!("k{i}"), "v");
    }
    for _ in 0..20 {
        f.store.flush_cycle();
    }
    assert_eq!(
        f.store.watermark(),
        0,
        "the watermark advanced past writes the store never accepted"
    );
}

#[test]
fn transient_io_failure_self_heals() {
    let f = Fixture::new();
    f.blobs.fail_next_writes(3);
    for i in 0..10 {
        f.put(&format!("k{i}"), &format!("v{i}"));
    }
    // The obligations stay in the dirty map across the failures and the
    // same entries retry, so nothing needs re-writing by the caller.
    assert!(f.flush());
    for i in 0..10 {
        assert_eq!(
            f.blobs.peek(format!("k{i}").as_bytes()).as_deref(),
            Some(format!("v{i}").as_bytes())
        );
    }
}

#[test]
fn non_durable_value_is_never_evicted_under_io_failure() {
    // Trap 4 end to end: hold IO down so values stay uncovered, apply
    // capacity pressure, and assert nothing is lost.
    let f = Fixture::with_capacity(16 * 1024);
    f.blobs.fail_next_writes(usize::MAX);

    let value = "p".repeat(512);
    for i in 0..200 {
        assert!(f.put(&format!("k{i}"), &format!("{value}{i}")));
    }
    // Give the sweeper ample opportunity to misbehave.
    let mut cursor = 0u32;
    for _ in 0..2000 {
        f.store.flush_cycle();
        f.store.sweep_chunk(&mut cursor);
    }

    for i in 0..200 {
        assert_eq!(
            f.get(&format!("k{i}")).as_deref(),
            Some(format!("{value}{i}").as_str()),
            "eviction discarded the only copy of key {i}"
        );
    }
}

#[test]
fn clear_is_refused_while_io_is_failing() {
    let f = Fixture::new();
    f.put("k", "v");
    assert!(f.flush());
    f.blobs.fail_next_writes(usize::MAX);
    f.store.flush_cycle(); // let the store notice
    assert!(!f.store.clear(), "clear must not report success it cannot deliver");
}

// ===================================================================
// The publish gap
// ===================================================================

#[test]
fn announce_floor_pins_watermark_below_in_flight_version() {
    let f = Fixture::new();
    for i in 0..16 {
        f.put(&format!("warm{i}"), "v");
    }
    assert!(f.flush());

    // Occupy exactly the state a writer is in between drawing a version
    // and getting its ticket into a batch: announced, never submitted.
    let announced = f.store.watermark() + 1;
    let hold = f.store.hold_announce(announced);

    for i in 0..64 {
        f.put(&format!("other{i}"), "v");
    }
    for _ in 0..50 {
        f.store.flush_cycle();
    }

    let pinned = f.store.watermark();
    assert!(
        pinned < announced,
        "the watermark ({pinned}) covered a version still in flight \
         ({announced}); the publish gap is open"
    );

    // Release and confirm it WOULD otherwise have advanced -- without
    // this the assertion above could pass merely because the flusher
    // never ran.
    f.store.release_announce_hold(hold);
    assert!(f.flush());
    assert!(
        f.store.watermark() > announced,
        "the watermark never advanced even after the floor was released, \
         so the assertion above proved nothing"
    );
}

#[test]
fn announce_floor_holds_across_many_flusher_cycles() {
    let f = Fixture::new();
    f.put("k", "v");
    assert!(f.flush());
    let announced = f.store.watermark() + 1;
    let hold = f.store.hold_announce(announced);

    for round in 0..200 {
        f.put(&format!("k{round}"), "v");
        f.store.flush_cycle();
        assert!(
            f.store.watermark() < announced,
            "the floor leaked on cycle {round}"
        );
    }
    f.store.release_announce_hold(hold);
    assert!(f.flush());
}

// ===================================================================
// Eviction
// ===================================================================

#[test]
fn no_capacity_means_no_eviction() {
    let f = Fixture::new(); // capacity_bytes: None
    for i in 0..500 {
        f.put(&format!("k{i}"), &"x".repeat(1024));
    }
    assert!(f.flush());
    assert!(!f.store.over_capacity());
    let mut cursor = 0u32;
    assert_eq!(
        f.store.sweep_chunk(&mut cursor),
        0,
        "nothing may be evicted when no capacity is configured"
    );
    for i in 0..500 {
        assert_eq!(f.store.is_resident(format!("k{i}").as_bytes()), Some(true));
    }
}

#[test]
fn eviction_brings_value_bytes_under_capacity() {
    let cap = 16 * 1024u64;
    let f = Fixture::with_capacity(cap);
    for i in 0..200 {
        f.put(&format!("k{i}"), &"x".repeat(512));
    }
    assert!(f.flush());
    assert!(f.store.over_capacity(), "the test needs real pressure");

    let mut cursor = 0u32;
    for _ in 0..10_000 {
        if !f.store.over_capacity() {
            break;
        }
        f.store.sweep_chunk(&mut cursor);
    }
    assert!(
        !f.store.over_capacity(),
        "the sweeper never brought the value tier under {cap} bytes \
         (evictable: {})",
        f.store.evictable_bytes()
    );
}

#[test]
fn evicted_values_read_back_unchanged() {
    let f = Fixture::with_capacity(4 * 1024);
    let mut expected = BTreeMap::new();
    for i in 0..100 {
        let v = format!("{}{i}", "v".repeat(200));
        f.put(&format!("k{i:03}"), &v);
        expected.insert(format!("k{i:03}"), v);
    }
    assert!(f.flush());
    let mut cursor = 0u32;
    for _ in 0..10_000 {
        if !f.store.over_capacity() {
            break;
        }
        f.store.sweep_chunk(&mut cursor);
    }
    let evicted = (0..100)
        .filter(|i| f.store.is_resident(format!("k{i:03}").as_bytes()) == Some(false))
        .count();
    assert!(evicted > 0, "nothing was evicted, so this proves nothing");

    for (k, v) in &expected {
        assert_eq!(f.get(k).as_deref(), Some(v.as_str()), "{k} came back wrong");
    }
}

#[test]
fn pressure_never_loses_an_acknowledged_write() {
    let cap = 16 * 1024u64;
    let f = Fixture::with_capacity(cap);
    let mut rt = Runtime::start(Arc::clone(&f.store));

    let value = "p".repeat(256);
    for i in 0..500 {
        assert!(f.put(&format!("key{i}"), &format!("{value}{i}")));
    }
    assert!(rt.shutdown());

    for i in 0..500 {
        assert_eq!(
            f.get(&format!("key{i}")).as_deref(),
            Some(format!("{value}{i}").as_str()),
            "eviction discarded key {i}"
        );
    }
}

#[test]
fn evict_value_refuses_ineligible_values() {
    let f = Fixture::with_capacity(1024);

    // Not durable yet: the cache holds the only copy.
    f.put("fresh", "v");
    assert!(
        !f.store.evict_key(b"fresh"),
        "evicting a value above the watermark discards the only copy"
    );

    // Durable: eligible.
    assert!(f.flush());
    assert!(f.store.evict_key(b"fresh"));
    assert_eq!(f.store.is_resident(b"fresh"), Some(false));

    // Already evicted: nothing to do.
    assert!(!f.store.evict_key(b"fresh"));

    // A tombstone must never become "evicted", which would republish the
    // key as live-but-absent and let the next read resurrect it.
    f.put("gone", "v");
    f.store.remove(b"gone");
    assert!(f.flush());
    assert!(!f.store.evict_key(b"gone"));
    assert_eq!(f.get("gone"), None, "a deleted key was resurrected");

    // Unknown keys.
    assert!(!f.store.evict_key(b"never-existed"));
}

#[test]
fn hot_key_survives_reclamation() {
    let cap = 32 * 1024u64;
    let f = Fixture::with_capacity(cap);
    let value = "h".repeat(512);
    assert!(f.put("hot", &value));
    for i in 0..400 {
        assert!(f.put(&format!("cold{i}"), &value));
    }
    assert!(f.flush());

    // Keep touching the hot key while the sweeper runs, so its reference
    // bit is set every time the clock hand passes.
    let stop = Arc::new(AtomicBool::new(false));
    let hot_resident = std::thread::scope(|scope| {
        let store = Arc::clone(&f.store);
        let s = Arc::clone(&stop);
        scope.spawn(move || {
            while !s.load(Ordering::Acquire) {
                let _ = store.get(b"hot");
            }
        });
        let mut cursor = 0u32;
        for _ in 0..50_000 {
            if !f.store.over_capacity() {
                break;
            }
            f.store.sweep_chunk(&mut cursor);
        }
        // One last read so the assertion is not racing a sweep that just
        // cleared the bit.
        let got = f.get("hot");
        let resident = f.store.is_resident(b"hot");
        stop.store(true, Ordering::Release);
        (got, resident)
    });

    assert_eq!(hot_resident.0.as_deref(), Some(value.as_str()));
    assert_eq!(
        hot_resident.1,
        Some(true),
        "a continuously read value should survive the clock hand"
    );
}

#[test]
fn writes_and_sweeper_concurrently() {
    let f = Fixture::with_capacity(8 * 1024);
    let mut rt = Runtime::start(Arc::clone(&f.store));
    let barrier = Arc::new(Barrier::new(8));

    std::thread::scope(|scope| {
        for t in 0..8u64 {
            let store = Arc::clone(&f.store);
            let barrier = Arc::clone(&barrier);
            scope.spawn(move || {
                barrier.wait();
                for i in 0..300u64 {
                    let k = format!("t{t}-{i:04}");
                    store.put(k.as_bytes(), format!("{t}:{i}").as_bytes());
                    assert_eq!(
                        store.get(k.as_bytes()).expect("get").as_deref(),
                        Some(format!("{t}:{i}").as_bytes()),
                        "the sweeper raced a write and lost a value"
                    );
                }
            });
        }
    });

    assert!(rt.shutdown());
    for t in 0..8u64 {
        for i in 0..300u64 {
            assert_eq!(
                f.get(&format!("t{t}-{i:04}")).as_deref(),
                Some(format!("{t}:{i}").as_str())
            );
        }
    }
    // The index must still hold every key: eviction takes values, never
    // keys.
    assert_eq!(f.index.len(), 8 * 300);
}

// ===================================================================
// Beyond the C++ suite
// ===================================================================
//
// The C++ oracle does not have this one. It covers a line the Rust port
// makes explicit — the dirty map keeping the OLDEST version per entry —
// which the C++ mutation set only reached indirectly, through timing.

#[test]
fn watermark_never_passes_an_undischarged_overwrite() {
    // Two tickets for ONE key must coalesce to the OLDER version. Keeping
    // the newer one discharges the older obligation early: W advances
    // past a version whose bytes never reached the store.
    //
    // Only visible when writeback is deferred past the drain, so IO is
    // held down to separate the two halves of the cycle.
    let f = Fixture::new();
    f.put("k", "v1");
    f.store.flush_cycle(); // steal the partial batch into the log
    f.put("k", "v2");
    f.blobs.fail_next_writes(usize::MAX);

    for _ in 0..10 {
        f.store.flush_cycle();
    }
    assert!(f.store.dirty_len() > 0, "the obligation must still be owed");
    assert_eq!(
        f.store.watermark(),
        0,
        "the watermark discharged an overwritten key's older version"
    );
    assert_eq!(f.blobs.peek(b"k"), None, "and nothing was written");
}
