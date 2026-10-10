//! A crash at every filesystem operation of a run (store creation, three
//! segment rotations, mid-header and mid-batch included), under a process
//! kill, a power cut and a torn power cut. The restart must recover a prefix
//! of what was written that holds every batch the WAL acknowledged (plan P2),
//! and a recovery that is itself interrupted must converge when rerun (P5).

use std::path::{Path, PathBuf};
use std::sync::Arc;

use raft_store::fs::Crash;
use raft_store::{open_store, BytesCodec, Hard, Identity, MemFs, Opened, Record, SavedState, StoreFs, WalOptions};

const ID: Identity = Identity { site: 2, partition: 0, fingerprint: 0xabcd, format: 1 };
const NO_VOTE: u16 = u16::MAX;

fn store() -> PathBuf {
    PathBuf::from("/data/run/2-0")
}

/// Record k (1-based): appends entry k, of a size that varies, and every
/// third record also raises the term and moves the commit; every seventh
/// cuts the log back two entries and rewrites them (a conflict).
fn record(k: u64) -> Record<Vec<u8>> {
    let payload = vec![(k % 251) as u8; (k as usize * 37) % 200];
    let mut r = Record::default();
    if k.is_multiple_of(7) && k > 2 {
        r.replace_from = Some(k - 2);
        r.entries = vec![(k / 3 + 1, payload.clone()), (k / 3 + 1, payload.clone()), (k / 3 + 1, payload)];
    } else {
        r.replace_from = Some(k);
        r.entries = vec![(k / 3 + 1, payload)];
    }
    if k.is_multiple_of(3) {
        r.hard = Some(Hard { term: k / 3 + 1, vote: (k % 5) as u16, commit: k / 2 });
    }
    r
}

/// The state after records 1..=n.
fn fold(n: u64) -> SavedState<Vec<u8>> {
    let mut s = SavedState::new(NO_VOTE);
    for k in 1..=n {
        s.apply(record(k)).unwrap();
    }
    s
}

fn encode(k: u64) -> Vec<u8> {
    let mut b = Vec::new();
    raft_store::record::encode(&record(k), &BytesCodec, &mut b);
    b
}

/// Batch sizes cycle 1, 2, 3, 4.
fn batches(total: u64) -> Vec<(u64, u64)> {
    let mut out = Vec::new();
    let mut k = 1;
    let mut size = 1;
    while k <= total {
        let n = size.min(total - k + 1);
        out.push((k, n));
        k += n;
        size = size % 4 + 1;
    }
    out
}

const OPTS: WalOptions = WalOptions { segment_bytes: 700 };
const TOTAL: u64 = 40;

fn open(fs: &MemFs, create: bool) -> Result<Opened<Vec<u8>>, String> {
    open_store(Arc::new(fs.clone()) as Arc<dyn StoreFs>, &store(), ID, OPTS, create, &BytesCodec, NO_VOTE)
}

/// Creates the store and appends TOTAL records; returns how many records
/// the WAL acknowledged (append returned) and how many it was handed.
fn run(fs: &MemFs) -> (u64, u64) {
    let Ok(mut o) = open(fs, true) else { return (0, 0) };
    let (mut acked, mut tried) = (0, 0);
    for (first, n) in batches(TOTAL) {
        let recs: Vec<Vec<u8>> = (first..first + n).map(encode).collect();
        tried = first + n - 1;
        if o.wal.append(first, &recs).is_err() {
            break;
        }
        acked = tried;
    }
    (acked, tried)
}

fn segments(fs: &MemFs) -> usize {
    fs.list(&store().join("wal")).map(|v| v.len()).unwrap_or(0)
}

/// Recovers after a crash and checks the result.
fn check_restart(fs: &MemFs, acked: u64, tried: u64, ctx: &str) -> u64 {
    match open(fs, false) {
        Ok(o) => {
            assert!(o.d >= acked, "{ctx}: recovered through {} but {acked} were acknowledged", o.d);
            assert!(o.d <= tried, "{ctx}: recovered {} of {tried} written", o.d);
            assert_eq!(o.state, fold(o.d), "{ctx}: state is not the fold of 1..={}", o.d);
            o.d
        }
        Err(why) => {
            // Only a crash before the store existed may refuse; a creating
            // launch must then succeed.
            assert_eq!(acked, 0, "{ctx}: refused after acknowledging {acked}: {why}");
            assert!(why.contains("MAKO_RAFT_CREATE=1"), "{ctx}: {why}");
            let o = open(fs, true).unwrap_or_else(|e| panic!("{ctx}: creating launch failed: {e}"));
            assert_eq!(o.d, 0);
            0
        }
    }
}

#[test]
fn crash_at_every_operation() {
    // Size the sweep with an uncrashed run.
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/data/run"));
    let (acked, _) = run(&fs);
    assert_eq!(acked, TOTAL);
    assert!(segments(&fs) >= 4, "the run must rotate at least three times ({} segments)", segments(&fs));
    let ops = fs.ops();

    let mut checked = 0;
    for budget in 0..=ops {
        for how in [Crash::Kill, Crash::PowerCut, Crash::Torn(budget * 31 + 7), Crash::Torn(budget * 17 + 3)] {
            let fs = MemFs::new();
            fs.mkdir_p(Path::new("/data/run"));
            fs.set_budget(Some(budget));
            let (acked, tried) = run(&fs);
            fs.crash(how);
            let ctx = format!("budget {budget} {how:?}");
            let d = check_restart(&fs, acked, tried, &ctx);
            // The restarted server keeps going, and a second crash recovers
            // everything it acknowledged.
            let mut o = open(&fs, false).unwrap_or_else(|e| panic!("{ctx}: reopen: {e}"));
            assert_eq!(o.d, d);
            for k in d + 1..=d + 3 {
                o.wal.append(k, &[encode(k)]).unwrap();
            }
            drop(o);
            fs.crash(Crash::PowerCut);
            let o = open(&fs, false).unwrap_or_else(|e| panic!("{ctx}: second restart: {e}"));
            assert_eq!(o.d, d + 3, "{ctx}");
            assert_eq!(o.state, fold(d + 3), "{ctx}");
            checked += 1;
        }
    }
    assert!(checked > 200, "only {checked} crash cases");
}

/// Recovers `crashed()`'s store with a crash after each operation of the
/// recovery (every one, from its measured count, under a kill and a power
/// cut), then recovers cleanly: always `want` records, folded.
fn recovery_crashes_converge(crashed: &dyn Fn() -> MemFs, want: u64) {
    let fs = crashed();
    let before = fs.ops();
    let clean = open(&fs, false).unwrap();
    assert_eq!(clean.d, want);
    let rec_ops = fs.ops() - before;
    drop(clean);
    for budget in 0..=rec_ops {
        for how in [Crash::Kill, Crash::PowerCut] {
            let fs = crashed();
            fs.set_budget(Some(budget));
            let _ = open(&fs, false);
            fs.crash(how);
            let o = open(&fs, false).unwrap_or_else(|e| panic!("budget {budget} {how:?}: {e}"));
            assert_eq!(o.d, want, "budget {budget} {how:?}");
            assert_eq!(o.state, fold(want), "budget {budget} {how:?}");
        }
    }
}

#[test]
fn interrupted_recovery_converges() {
    // A torn crash mid-run, then a recovery crashed at each of its own
    // operations, then a clean recovery: always the same state.
    let full = {
        let probe = MemFs::new();
        probe.mkdir_p(Path::new("/data/run"));
        run(&probe);
        probe.ops()
    };
    let crashed = || {
        let fs = MemFs::new();
        fs.mkdir_p(Path::new("/data/run"));
        fs.set_budget(Some(full * 2 / 3));
        run(&fs);
        fs.crash(Crash::Torn(99));
        fs
    };
    let want = open(&crashed(), false).unwrap().d;
    let (acked, _) = {
        let fs = MemFs::new();
        fs.mkdir_p(Path::new("/data/run"));
        fs.set_budget(Some(full * 2 / 3));
        run(&fs)
    };
    assert!(want >= acked);
    recovery_crashes_converge(&crashed, want);
}

#[test]
fn a_fresh_store_survives_a_crashed_recovery() {
    // A store with no records holds one segment, its header alone. Recovery
    // resumes it: a crash at any point leaves a store that opens (it used to
    // delete and recreate it, and a crash between the two left none).
    let fresh = || {
        let fs = MemFs::new();
        fs.mkdir_p(Path::new("/data/run"));
        drop(open(&fs, true).unwrap());
        fs
    };
    recovery_crashes_converge(&fresh, 0);
}

/// A finished store with records 1..=TOTAL.
fn full_store() -> MemFs {
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/data/run"));
    run(&fs);
    fs
}

fn wal_files(fs: &MemFs) -> Vec<PathBuf> {
    let dir = store().join("wal");
    fs.list(&dir).unwrap().into_iter().map(|n| dir.join(n)).collect()
}

fn refuses(fs: &MemFs, needle: &str) {
    match open(fs, false) {
        Ok(o) => panic!("opened a damaged store (d = {})", o.d),
        Err(why) => assert!(why.contains(needle), "expected {needle:?} in {why:?}"),
    }
}

#[test]
fn damaged_stores_fail_closed() {
    // A bad header on a segment that is not the last.
    let fs = full_store();
    let segs = wal_files(&fs);
    let mut b = fs.read(&segs[1]).unwrap();
    b[3] ^= 0xff;
    fs.poke(&segs[1], b);
    refuses(&fs, "not the last segment");

    // A corrupt batch with batches after it, in a segment that is not the
    // last: corruption, not a torn tail.
    let fs = full_store();
    let segs = wal_files(&fs);
    let (k, mut b) = segs[..segs.len() - 1].iter().enumerate()
        .map(|(k, p)| (k, fs.read(p).unwrap()))
        .find(|(_, b)| raft_store::segment::scan(&b[raft_store::segment::HEADER_LEN..]).unwrap().batches.len() >= 2)
        .expect("a closed segment with two batches");
    b[raft_store::segment::HEADER_LEN + 12] ^= 0xff;
    fs.poke(&segs[k], b);
    refuses(&fs, "with data after it");

    // A missing middle segment: a gap.
    let fs = full_store();
    let segs = wal_files(&fs);
    fs.remove_file(&segs[1]).unwrap();
    refuses(&fs, "expected");

    // Another server's store.
    let fs = full_store();
    let other = Identity { site: 3, ..ID };
    match open_store::<Vec<u8>>(Arc::new(fs.clone()), &store(), other, OPTS, false, &BytesCodec, NO_VOTE) {
        Ok(_) => panic!("opened another server's store"),
        Err(why) => assert!(why.contains("belongs to"), "{why}"),
    }

    // A stray file in the WAL directory.
    let fs = full_store();
    drop(fs.create_new(&store().join("wal").join("junk")).unwrap());
    refuses(&fs, "unexpected file");

    // No segment at all.
    let fs = full_store();
    for p in wal_files(&fs) {
        fs.remove_file(&p).unwrap();
    }
    refuses(&fs, "no WAL segment");

    // A record that cannot follow the state before it.
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/data/run"));
    let mut o = open(&fs, true).unwrap();
    let mut b = Vec::new();
    let bad: Record<Vec<u8>> = Record { replace_from: Some(5), entries: vec![(1, vec![])], ..Default::default() };
    raft_store::record::encode(&bad, &BytesCodec, &mut b);
    o.wal.append(1, &[b]).unwrap();
    drop(o);
    refuses(&fs, "replace from 5");
}

#[test]
fn a_bad_last_header_with_data_after_it_fails_closed() {
    // A crash leaves a bad header only on an empty segment (the header is
    // synced before any batch): one with data after it is damage, or a
    // newer format, and its records must not be thrown away.
    let fs = full_store();
    let segs = wal_files(&fs);
    let last = segs.last().unwrap();
    let mut b = fs.read(last).unwrap();
    assert!(b.len() > raft_store::segment::HEADER_LEN, "the last segment holds records");
    b[8] = 2; // the version
    fs.poke(last, b);
    refuses(&fs, "with data after the header");
}

#[test]
fn a_last_segment_without_a_header_is_deleted() {
    let fs = full_store();
    let segs = wal_files(&fs);
    let last = segs.last().unwrap();
    let b = fs.read(last).unwrap();
    fs.poke(last, b[..20].to_vec());
    // The records that segment held are gone with it, and recovery says so.
    let o = open(&fs, false).unwrap();
    assert!(o.repairs.iter().any(|r| r.contains("deleted")), "{:?}", o.repairs);
    assert_eq!(o.state, fold(o.d));
}

