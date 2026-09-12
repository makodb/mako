//! The whole thing: `mrx-core`'s cache over real masstree and real
//! RocksDB.
//!
//! Everything up to here was tested against fakes, which is what makes
//! the logic tests fast and deterministic — and also what makes them
//! unable to catch a wrong assumption about a real backend. This file is
//! where the assumptions get checked: that masstree really does treat the
//! stored word as opaque, that a RocksDB WriteBatch really is atomic
//! enough, that `for_each_key` really returns every key at open.
//!
//! Skipped when either backend is missing, rather than silently passing.

#![cfg(all(have_mako, have_rocksdb))]

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::Barrier;
use std::sync::atomic::{AtomicU64, Ordering};

use mrx::{Db, Options, WriteBatch};

struct Scratch(PathBuf);

impl Scratch {
    fn new(tag: &str) -> Self {
        static N: AtomicU64 = AtomicU64::new(0);
        let mut p = std::env::temp_dir();
        p.push(format!(
            "mrx-e2e-{tag}-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::remove_dir_all(&p);
        Self(p)
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn open(s: &Scratch) -> Db {
    Db::open(&s.0, Options::default()).expect("open")
}

#[test]
fn round_trip_through_both_real_backends() {
    let s = Scratch::new("round-trip");
    let db = open(&s);
    db.put(b"k", b"v").expect("put");
    assert_eq!(db.get(b"k").expect("get").as_deref(), Some(&b"v"[..]));
    assert_eq!(db.get(b"absent").expect("get"), None);
    db.close().expect("close");
}

#[test]
fn a_write_acks_before_it_is_durable() {
    // The whole point of the cache, stated as a test against the real
    // RocksDB rather than a fake: the value is readable immediately and
    // the durable store does not have it yet.
    let s = Scratch::new("ack-first");
    let db = open(&s);
    db.put(b"k", b"v").expect("put");
    assert_eq!(db.get(b"k").expect("get").as_deref(), Some(&b"v"[..]));
    let before = db.watermark();
    db.flush().expect("flush");
    assert!(
        db.watermark() > before,
        "the barrier did not advance the watermark"
    );
    db.close().expect("close");
}

#[test]
fn everything_survives_a_clean_close() {
    let s = Scratch::new("clean-close");
    let expected: BTreeMap<String, String> = (0..2000)
        .map(|i| (format!("k{i:05}"), format!("v{i}")))
        .collect();
    {
        let db = open(&s);
        for (k, v) in &expected {
            db.put(k.as_bytes(), v.as_bytes()).expect("put");
        }
        db.close().expect("close must drain");
    }
    let db = open(&s);
    for (k, v) in &expected {
        assert_eq!(
            db.get(k.as_bytes()).expect("get").as_deref(),
            Some(v.as_bytes()),
            "{k} was lost across a clean close"
        );
    }
    db.close().expect("close");
}

#[test]
fn reopen_sees_every_key_without_loading_values() {
    // The key-resident invariant against a real RocksDB iterator. If
    // `for_each_key` misses a key, that key reads as absent forever.
    let s = Scratch::new("reopen");
    {
        let db = open(&s);
        for i in 0..1000 {
            db.put(format!("k{i:04}").as_bytes(), b"v").expect("put");
        }
        db.close().expect("close");
    }
    let db = open(&s);
    assert_eq!(db.len(), 1000, "open did not intern every durable key");
    assert_eq!(
        db.store().is_resident(b"k0500"),
        Some(false),
        "open must intern keys WITHOUT loading their values"
    );
    assert_eq!(db.get(b"k0500").expect("get").as_deref(), Some(&b"v"[..]));
    assert_eq!(
        db.store().is_resident(b"k0500"),
        Some(true),
        "and the read must cache it"
    );
    db.close().expect("close");
}

#[test]
fn deletes_reach_rocksdb_and_stay_deleted() {
    let s = Scratch::new("delete");
    {
        let db = open(&s);
        for i in 0..200 {
            db.put(format!("k{i:03}").as_bytes(), b"v").expect("put");
        }
        db.flush().expect("flush");
        for i in (0..200).step_by(2) {
            assert!(db.delete(format!("k{i:03}").as_bytes()).expect("delete"));
        }
        db.close().expect("close");
    }
    let db = open(&s);
    for i in 0..200 {
        let k = format!("k{i:03}");
        let got = db.get(k.as_bytes()).expect("get");
        if i % 2 == 0 {
            assert_eq!(got, None, "{k} was deleted and came back");
        } else {
            assert_eq!(got.as_deref(), Some(&b"v"[..]), "{k} was lost");
        }
    }
    db.close().expect("close");
}

#[test]
fn iteration_covers_the_whole_keyspace() {
    let s = Scratch::new("iterate");
    let db = open(&s);
    for i in 0..1000 {
        db.put(format!("k{i:04}").as_bytes(), format!("v{i}").as_bytes())
            .expect("put");
    }
    let seen: Vec<(Vec<u8>, Vec<u8>)> = db.iter(b"").collect::<Result<_, _>>().expect("iterate");
    assert_eq!(seen.len(), 1000);
    assert!(seen.windows(2).all(|w| w[0].0 < w[1].0), "must be ordered");
    assert_eq!(seen[0].1, b"v0".to_vec());
    db.close().expect("close");
}

#[test]
fn iteration_skips_deleted_keys_without_ending_early() {
    // An all-tombstone chunk must not look like end-of-range. With a
    // small scan chunk and a contiguous run of deletes, an iterator that
    // stops on an empty chunk loses everything after the run.
    let s = Scratch::new("iterate-deleted");
    let mut opts = Options::default();
    opts.cache.scan_chunk = 8;
    let db = Db::open(&s.0, opts).expect("open");
    for i in 0..200 {
        db.put(format!("k{i:03}").as_bytes(), b"v").expect("put");
    }
    // Delete a run longer than one chunk.
    for i in 40..80 {
        db.delete(format!("k{i:03}").as_bytes()).expect("delete");
    }
    let seen: Vec<Vec<u8>> = db.iter(b"").map(|r| r.expect("iterate").0).collect();
    assert_eq!(seen.len(), 160, "an all-deleted chunk ended the walk early");
    assert!(!seen.iter().any(|k| k.starts_with(b"k04")));
    assert!(seen.iter().any(|k| k.starts_with(b"k08")));
    db.close().expect("close");
}

#[test]
fn reverse_iteration_descends() {
    let s = Scratch::new("iterate-rev");
    let db = open(&s);
    for i in 0..100 {
        db.put(format!("k{i:03}").as_bytes(), b"v").expect("put");
    }
    let seen: Vec<Vec<u8>> = db
        .iter_rev(b"k050")
        .map(|r| r.expect("iterate").0)
        .collect();
    assert_eq!(seen[0], b"k050".to_vec(), "the bound is inclusive");
    assert!(seen.windows(2).all(|w| w[0] > w[1]), "must be descending");
    assert_eq!(seen.len(), 51);
    db.close().expect("close");
}

#[test]
fn eviction_keeps_values_correct_against_real_rocksdb() {
    // Capacity pressure with a real durable store: values get evicted and
    // must read back byte-identical from RocksDB.
    let s = Scratch::new("evict");
    let opts = Options {
        capacity_bytes: Some(64 * 1024),
        ..Options::default()
    };
    let db = Db::open(&s.0, opts).expect("open");

    let mut expected = BTreeMap::new();
    for i in 0..2000 {
        let (k, v) = (format!("k{i:05}"), format!("{}{i}", "v".repeat(256)));
        db.put(k.as_bytes(), v.as_bytes()).expect("put");
        expected.insert(k, v);
    }
    db.flush().expect("flush");

    // POLL, do not sleep. Eviction is asynchronous, so a fixed sleep is a
    // race: it passed locally and failed once on a loaded machine, which
    // is the worst possible way for a test to behave. Waiting for the
    // condition makes the slow case slow instead of red.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
    let evicted = loop {
        let n = expected
            .keys()
            .filter(|k| db.store().is_resident(k.as_bytes()) == Some(false))
            .count();
        if n > 0 {
            break n;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "the sweeper evicted nothing in 30s under {} bytes of pressure",
            db.store().evictable_bytes()
        );
        std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(evicted > 0);

    for (k, v) in &expected {
        assert_eq!(
            db.get(k.as_bytes()).expect("get").as_deref(),
            Some(v.as_bytes()),
            "{k} came back wrong after eviction"
        );
    }
    db.close().expect("close");
}

#[test]
fn concurrent_writers_against_real_backends() {
    let s = Scratch::new("concurrent");
    let db = open(&s);
    let barrier = Barrier::new(8);
    std::thread::scope(|scope| {
        for t in 0..8u64 {
            let db = &db;
            let barrier = &barrier;
            scope.spawn(move || {
                barrier.wait();
                for i in 0..500u64 {
                    let k = format!("t{t}-{i:04}");
                    let v = format!("{t}:{i}");
                    db.put(k.as_bytes(), v.as_bytes()).expect("put");
                    assert_eq!(
                        db.get(k.as_bytes()).expect("get").as_deref(),
                        Some(v.as_bytes()),
                        "read-your-own-write violated"
                    );
                }
            });
        }
    });
    db.close().expect("close");

    let db = open(&s);
    for t in 0..8u64 {
        for i in 0..500u64 {
            assert_eq!(
                db.get(format!("t{t}-{i:04}").as_bytes())
                    .expect("get")
                    .as_deref(),
                Some(format!("{t}:{i}").as_bytes()),
                "t{t}-{i:04} was lost"
            );
        }
    }
    db.close().expect("close");
}

#[test]
fn batches_apply_in_order() {
    let s = Scratch::new("batch");
    let db = open(&s);
    let mut b = WriteBatch::new();
    b.put(b"a", b"1")
        .put(b"b", b"2")
        .delete(b"a")
        .put(b"c", b"3");
    assert_eq!(b.len(), 4);
    db.write(b).expect("write");
    assert_eq!(db.get(b"a").expect("get"), None, "later delete must win");
    assert_eq!(db.get(b"b").expect("get").as_deref(), Some(&b"2"[..]));
    assert_eq!(db.get(b"c").expect("get").as_deref(), Some(&b"3"[..]));
    db.close().expect("close");
}

#[test]
fn insert_is_put_if_absent() {
    let s = Scratch::new("insert");
    let db = open(&s);
    assert!(db.insert(b"k", b"first"));
    assert!(!db.insert(b"k", b"second"));
    assert_eq!(db.get(b"k").expect("get").as_deref(), Some(&b"first"[..]));
    db.close().expect("close");
}

#[test]
fn clear_empties_both_tiers_durably() {
    let s = Scratch::new("clear");
    {
        let db = open(&s);
        for i in 0..500 {
            db.put(format!("k{i:04}").as_bytes(), b"v").expect("put");
        }
        db.flush().expect("flush");
        db.clear().expect("clear");
        assert_eq!(db.get(b"k0001").expect("get"), None);
        db.close().expect("close");
    }
    let db = open(&s);
    let n = db.iter(b"").count();
    assert_eq!(n, 0, "keys returned after reopening a cleared database");
    db.close().expect("close");
}

#[test]
fn binary_keys_and_values_survive_the_whole_stack() {
    // Every layer here copies bytes across a boundary — masstree's arena,
    // the FFI, RocksDB's slices. A NUL or a high byte anywhere is where
    // a length-vs-terminator mistake would show.
    let s = Scratch::new("binary");
    let cases: [(&[u8], &[u8]); 5] = [
        (b"", b"empty key"),
        (&[0u8, 1, 2], b"nul key"),
        (b"nul value", &[0u8, 0, 0]),
        (&[0xFFu8; 300], &[0xFEu8; 300]),
        (b"empty value", b""),
    ];
    {
        let db = open(&s);
        for (k, v) in cases {
            db.put(k, v).expect("put");
        }
        db.close().expect("close");
    }
    let db = open(&s);
    for (k, v) in cases {
        assert_eq!(
            db.get(k).expect("get").as_deref(),
            Some(v),
            "{k:?} did not survive"
        );
    }
    db.close().expect("close");
}
