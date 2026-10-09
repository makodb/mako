//! The base and the applier (plan P7): with checkpoints deleting segments,
//! with a one-slot offer queue (catch-up reads), and with failures injected
//! into the base and the filesystem, a recovery is always the fold of
//! records 1..=d with d covering every published record, and the WAL stays
//! bounded.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use raft_store::applier::{Applier, ApplierConfig};
use raft_store::base::{Base, MemBase};
use raft_store::flusher::FlusherConfig;
use raft_store::fs::Crash;
use raft_store::{open_store_with_base, BaseFactory, BytesCodec, Durable, DurableState, Flusher, Hard, Identity,
                 MemFs, Opened, Record, RecordQueue, SavedState, StoreFs, WalOptions};

const ID: Identity = Identity { site: 1, partition: 0, fingerprint: 9, format: 1 };

fn store() -> PathBuf {
    PathBuf::from("/d/run/1-0")
}

fn record(k: u64) -> Record<Vec<u8>> {
    let payload = vec![(k % 251) as u8; (k as usize * 37) % 120];
    let mut r = Record::default();
    if k % 7 == 0 && k > 2 {
        r.replace_from = Some(k - 2);
        r.entries = vec![(k / 3 + 1, payload.clone()), (k / 3 + 1, payload.clone()), (k / 3 + 1, payload)];
    } else {
        r.replace_from = Some(k);
        r.entries = vec![(k / 3 + 1, payload)];
    }
    if k % 3 == 0 {
        r.hard = Some(Hard { term: k / 3 + 1, vote: (k % 5) as u16, commit: k / 2 });
    }
    r
}

fn fold(n: u64) -> SavedState<Vec<u8>> {
    let mut s = SavedState::new(u16::MAX);
    for k in 1..=n {
        s.apply(record(k)).unwrap();
    }
    s
}

fn open(fs: &MemFs, base: &MemBase, create: bool) -> Result<Opened<Vec<u8>>, String> {
    let b = base.clone();
    let factory: Box<BaseFactory> = Box::new(move |_p: &Path, _create: bool| Ok(Box::new(b.clone()) as Box<dyn Base>));
    open_store_with_base(Arc::new(fs.clone()) as Arc<dyn StoreFs>, &store(), ID,
                         WalOptions { segment_bytes: 2048 }, create, &BytesCodec, u16::MAX, Some(&*factory))
}

/// Runs the flusher and the applier over `n` records (from `o.d + 1`) and
/// returns the last published record.
fn run(fs: &MemFs, o: Opened<Vec<u8>>, n: u64, cfg: ApplierConfig) -> u64 {
    let start = Durable { seq: o.d, last: o.state.last(), commit: o.state.hard.commit };
    let durable = Arc::new(DurableState::new(start));
    let (applier, tap) = Applier::spawn(o.base.unwrap(), o.c, Arc::new(fs.clone()), store().join("wal"), ID,
                                        durable.clone(), cfg);
    let queue = Arc::new(RecordQueue::new(o.d + 1));
    let flusher = Flusher::spawn(o.wal, queue.clone(), Arc::new(BytesCodec), durable.clone(), start,
                                 FlusherConfig { delay: Duration::ZERO, tap: Some(tap) }, Box::new(|_| {}));
    for k in o.d + 1..=o.d + n {
        queue.push(record(k));
        if k % 50 == 0 {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    queue.close();
    let flusher_ok = std::thread::spawn(move || flusher.join()).join().is_ok();
    let published = durable.seq();
    let _ = applier.join();
    if flusher_ok {
        assert_eq!(published, o.d + n);
    }
    published
}

fn segments(fs: &MemFs) -> usize {
    fs.list(&store().join("wal")).unwrap().len()
}

fn fresh() -> (MemFs, MemBase) {
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/d/run"));
    (fs, MemBase::new())
}

#[test]
fn checkpoints_bound_the_wal_and_recovery_folds() {
    for queue in [64, 1] {
        let (fs, base) = fresh();
        let o = open(&fs, &base, true).unwrap();
        let d = run(&fs, o, 3000, ApplierConfig { checkpoint_bytes: 4096, checkpoint_secs: 1, queue });
        assert_eq!(d, 3000);
        assert!(segments(&fs) <= 6, "queue {queue}: {} segments left", segments(&fs));
        fs.crash(Crash::PowerCut);
        base.crash();
        let o = open(&fs, &base, false).unwrap();
        assert_eq!(o.d, 3000);
        assert!(o.c > 0, "queue {queue}: the base folded nothing");
        assert_eq!(o.state, fold(3000), "queue {queue}");
        // And it carries on from there.
        let d = run(&fs, o, 500, ApplierConfig { checkpoint_bytes: 4096, checkpoint_secs: 1, queue });
        fs.crash(Crash::PowerCut);
        base.crash();
        let o = open(&fs, &base, false).unwrap();
        assert_eq!((o.d, &o.state), (d, &fold(d)));
    }
}

#[test]
fn failures_anywhere_recover_to_a_fold() {
    for seed in 0..24u64 {
        let (fs, base) = fresh();
        let o = open(&fs, &base, true).unwrap();
        // A base failure stops the applier (the WAL keeps everything); a
        // filesystem failure stops the flusher (it panics, as in production).
        if seed % 2 == 0 {
            base.set_budget(Some(seed * 3));
        } else {
            fs.set_budget(Some(fs.ops() + 40 + seed * 37));
        }
        let published = run(&fs, o, 1500, ApplierConfig { checkpoint_bytes: 2048, checkpoint_secs: 1, queue: 4 });
        fs.crash(Crash::PowerCut);
        base.crash();
        let o = open(&fs, &base, false).unwrap_or_else(|e| panic!("seed {seed}: {e}"));
        assert!(o.d >= published, "seed {seed}: recovered {} of {published} published", o.d);
        assert_eq!(o.state, fold(o.d), "seed {seed}");
    }
}
