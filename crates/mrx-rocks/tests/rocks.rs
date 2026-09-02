//! The RocksDB-backed [`Blobs`], against a real database.
//!
//! Skipped when librocksdb is not present — see `build.rs`. The gate is
//! `cfg(have_rocksdb)` so a missing library shows up as "0 tests run"
//! rather than a green suite that tested nothing.

#![cfg(have_rocksdb)]

use std::collections::BTreeSet;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

use mrx_core::{BlobOp, Blobs};
#[cfg(feature = "test-hooks")]
use mrx_rocks::WriteBatchHookPoint;
use mrx_rocks::{Durability, RocksBlobs};

/// A scratch database that removes itself.
struct Scratch(PathBuf);

impl Scratch {
    fn new(tag: &str) -> Self {
        static N: AtomicU64 = AtomicU64::new(0);
        let mut p = std::env::temp_dir();
        p.push(format!(
            "mrx-rocks-{tag}-{}-{}",
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

fn open(s: &Scratch) -> RocksBlobs {
    RocksBlobs::open(&s.0, Durability::Wal).expect("open")
}

#[test]
fn values_round_trip() {
    let s = Scratch::new("round-trip");
    let db = open(&s);
    db.write_batch(&[BlobOp::Put {
        key: b"k",
        val: b"v",
    }])
    .expect("write");
    assert_eq!(db.get(b"k").expect("get").as_deref(), Some(&b"v"[..]));
    assert_eq!(db.get(b"absent").expect("get"), None);
}

#[test]
fn binary_and_empty_values_survive() {
    // The cache stores opaque bytes. A value that happens to contain NUL,
    // or that is empty, must come back byte-identical — an empty value is
    // NOT the same as an absent key, and conflating them silently deletes
    // data.
    let s = Scratch::new("binary");
    let db = open(&s);
    let cases: [(&[u8], &[u8]); 4] = [
        (b"empty", b""),
        (b"nul", &[0u8, 1, 0, 2]),
        (b"high", &[0xFFu8; 64]),
        (&[0u8, 0xFF], b"binary key"),
    ];
    let ops: Vec<BlobOp<'_>> = cases
        .iter()
        .map(|(k, v)| BlobOp::Put { key: k, val: v })
        .collect();
    db.write_batch(&ops).expect("write");
    for (k, v) in cases {
        assert_eq!(db.get(k).expect("get").as_deref(), Some(v), "{k:?} altered");
    }
    assert!(
        db.get(b"empty").expect("get").is_some(),
        "an empty value must read back as present, not absent"
    );
}

#[test]
fn a_batch_is_all_or_nothing() {
    let s = Scratch::new("atomic");
    let db = open(&s);
    db.write_batch(&[
        BlobOp::Put {
            key: b"a",
            val: b"1",
        },
        BlobOp::Put {
            key: b"b",
            val: b"2",
        },
        BlobOp::Put {
            key: b"c",
            val: b"3",
        },
    ])
    .expect("write");
    for (k, v) in [(&b"a"[..], &b"1"[..]), (b"b", b"2"), (b"c", b"3")] {
        assert_eq!(db.get(k).expect("get").as_deref(), Some(v));
    }
}

#[test]
fn deletes_apply_and_a_batch_may_mix_them() {
    let s = Scratch::new("delete");
    let db = open(&s);
    db.write_batch(&[
        BlobOp::Put {
            key: b"keep",
            val: b"1",
        },
        BlobOp::Put {
            key: b"drop",
            val: b"2",
        },
    ])
    .expect("write");
    db.write_batch(&[
        BlobOp::Delete { key: b"drop" },
        BlobOp::Put {
            key: b"new",
            val: b"3",
        },
    ])
    .expect("write");
    assert_eq!(db.get(b"drop").expect("get"), None);
    assert_eq!(db.get(b"keep").expect("get").as_deref(), Some(&b"1"[..]));
    assert_eq!(db.get(b"new").expect("get").as_deref(), Some(&b"3"[..]));
}

#[test]
fn duplicate_keys_apply_in_slice_order() {
    let s = Scratch::new("duplicate-key-order");
    let db = open(&s);

    db.write_batch(&[
        BlobOp::Put {
            key: b"put-wins",
            val: b"old",
        },
        BlobOp::Delete { key: b"put-wins" },
        BlobOp::Put {
            key: b"put-wins",
            val: b"new",
        },
        BlobOp::Put {
            key: b"delete-wins",
            val: b"present",
        },
        BlobOp::Delete {
            key: b"delete-wins",
        },
    ])
    .expect("ordered duplicate-key batch");

    assert_eq!(
        db.get(b"put-wins").expect("get").as_deref(),
        Some(&b"new"[..])
    );
    assert_eq!(db.get(b"delete-wins").expect("get"), None);
}

#[test]
fn empty_batches_are_a_no_op() {
    let s = Scratch::new("empty-batch");
    let db = open(&s);
    db.write_batch(&[]).expect("an empty batch must succeed");
}

#[cfg(feature = "test-hooks")]
#[test]
fn batch_observer_is_per_database_synchronous_and_ordered() {
    use std::sync::{mpsc, Arc, Mutex};
    use std::time::Duration;

    let observed_scratch = Scratch::new("observed-hooks");
    let unobserved_scratch = Scratch::new("unobserved-hooks");
    let mut observed = open(&observed_scratch);
    let unobserved = open(&unobserved_scratch);
    let events = Arc::new(Mutex::new(Vec::new()));
    let observed_events = Arc::clone(&events);
    let (reached_tx, reached_rx) = mpsc::sync_channel(1);
    let (resume_tx, resume_rx) = mpsc::sync_channel(1);
    let resume_rx = Mutex::new(resume_rx);
    observed.set_write_batch_observer(move |point| {
        observed_events.lock().expect("event lock").push(point);
        if point == WriteBatchHookPoint::BeforeWrite {
            reached_tx.send(()).expect("announce BeforeWrite");
            resume_rx
                .lock()
                .expect("resume lock")
                .recv()
                .expect("resume BeforeWrite");
        }
    });

    unobserved
        .write_batch(&[BlobOp::Put {
            key: b"outside",
            val: b"ignored",
        }])
        .expect("unobserved write");
    assert!(events.lock().expect("event lock").is_empty());

    let before_write = std::thread::scope(|scope| {
        let writer = scope.spawn(|| {
            observed.write_batch(&[BlobOp::Put {
                key: b"inside",
                val: b"seen",
            }])
        });
        reached_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("writer must rendezvous at BeforeWrite");
        let before_write = observed.get(b"inside");
        resume_tx.send(()).expect("resume writer");
        writer
            .join()
            .expect("writer thread")
            .expect("observed write");
        before_write
    });
    assert_eq!(
        before_write.expect("get before write"),
        None,
        "BeforeWrite must run before RocksDB can expose the batch"
    );
    assert_eq!(
        *events.lock().expect("event lock"),
        [
            WriteBatchHookPoint::BatchConstructed,
            WriteBatchHookPoint::BeforeWrite,
            WriteBatchHookPoint::AfterWrite,
        ]
    );
    assert_eq!(
        observed.get(b"inside").expect("get").as_deref(),
        Some(&b"seen"[..]),
        "AfterWrite must run before a successful write returns"
    );
}

#[cfg(feature = "test-hooks")]
#[test]
fn empty_batches_and_cleared_observers_emit_nothing() {
    use std::sync::{Arc, Mutex};

    let scratch = Scratch::new("hook-boundaries");
    let mut db = open(&scratch);
    let events = Arc::new(Mutex::new(Vec::new()));
    let observed_events = Arc::clone(&events);
    db.set_write_batch_observer(move |point| {
        observed_events.lock().expect("event lock").push(point);
    });

    db.write_batch(&[]).expect("empty batch");
    assert!(events.lock().expect("event lock").is_empty());

    db.clear_write_batch_observer();
    db.write_batch(&[BlobOp::Put {
        key: b"after-clear",
        val: b"value",
    }])
    .expect("write after clear");
    assert!(events.lock().expect("event lock").is_empty());
}

#[test]
fn for_each_key_visits_everything_in_order() {
    // This is what establishes the key-resident invariant at open. A key
    // it misses is a key the cache will report as absent forever.
    let s = Scratch::new("iterate");
    let db = open(&s);
    let keys: Vec<String> = (0..500).map(|i| format!("k{i:04}")).collect();
    let ops: Vec<BlobOp<'_>> = keys
        .iter()
        .map(|k| BlobOp::Put {
            key: k.as_bytes(),
            val: b"v",
        })
        .collect();
    db.write_batch(&ops).expect("write");

    let mut seen = Vec::new();
    db.for_each_key(&mut |k| seen.push(k.to_vec()))
        .expect("iterate");
    assert_eq!(seen.len(), 500);
    assert!(seen.windows(2).all(|w| w[0] < w[1]), "must be ascending");
    let unique: BTreeSet<_> = seen.iter().collect();
    assert_eq!(unique.len(), 500);
}

#[test]
fn data_survives_reopen() {
    let s = Scratch::new("reopen");
    {
        let db = open(&s);
        db.write_batch(&[BlobOp::Put {
            key: b"k",
            val: b"v",
        }])
        .expect("write");
        db.flush().expect("flush");
    }
    let db = open(&s);
    assert_eq!(db.get(b"k").expect("get").as_deref(), Some(&b"v"[..]));
}

#[test]
fn concurrent_batches_all_land() {
    let s = Scratch::new("concurrent");
    let db = open(&s);
    std::thread::scope(|scope| {
        for t in 0..8u64 {
            let db = &db;
            scope.spawn(move || {
                for i in 0..200u64 {
                    let k = format!("t{t}-{i:04}");
                    let v = format!("{t}:{i}");
                    db.write_batch(&[BlobOp::Put {
                        key: k.as_bytes(),
                        val: v.as_bytes(),
                    }])
                    .expect("write");
                }
            });
        }
    });
    for t in 0..8u64 {
        for i in 0..200u64 {
            assert_eq!(
                db.get(format!("t{t}-{i:04}").as_bytes())
                    .expect("get")
                    .as_deref(),
                Some(format!("{t}:{i}").as_bytes())
            );
        }
    }
}

#[test]
fn opening_a_bad_path_reports_an_error_rather_than_panicking() {
    // The error string RocksDB hands back is owned by the caller. This
    // path is where forgetting to free it would leak, so it is worth
    // taking deliberately.
    let err = RocksBlobs::open(
        std::path::Path::new("/proc/definitely/not/writable/mrx"),
        Durability::Wal,
    );
    assert!(err.is_err(), "opening an unwritable path must fail cleanly");
}
