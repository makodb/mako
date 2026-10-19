//! Store creation's decisions (design Decision 10): only a
//! creating launch starts empty; a crash while creating leaves nothing that
//! blocks a creating relaunch; every refusal says why.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use raft_store::fs::Crash;
use raft_store::{open_store, BytesCodec, Identity, MemFs, Opened, StoreFs, WalOptions};

const ID: Identity = Identity { site: 1, partition: 0, fingerprint: 7, format: 1 };

fn store() -> PathBuf {
    PathBuf::from("/d/run/1-0")
}

fn open(fs: &MemFs, create: bool) -> Result<Opened<Vec<u8>>, String> {
    open_store(Arc::new(fs.clone()) as Arc<dyn StoreFs>, &store(), ID, WalOptions::default(), create, &BytesCodec, 0)
}

fn fresh() -> MemFs {
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/d/run"));
    fs
}

#[test]
fn missing_store_needs_the_flag() {
    let fs = fresh();
    let why = open(&fs, false).err().unwrap();
    assert!(why.contains("no store") && why.contains("MAKO_RAFT_CREATE=1"), "{why}");
    let o = open(&fs, true).unwrap();
    assert!(o.created && o.d == 0);
}

#[test]
fn the_flag_refuses_a_store_with_records_and_reopens_an_empty_one() {
    let fs = fresh();
    drop(open(&fs, true).unwrap());
    // Never recorded anything: a creating relaunch takes it (a creation
    // killed after its rename).
    let mut o = open(&fs, true).unwrap();
    assert!(o.created && o.d == 0);
    let mut b = Vec::new();
    let r: raft_store::Record<Vec<u8>> = raft_store::Record { replace_from: Some(1), entries: vec![(1, vec![7])], ..Default::default() };
    raft_store::record::encode(&r, &BytesCodec, &mut b);
    o.wal.append(1, &[b]).unwrap();
    drop(o);
    let why = open(&fs, true).err().unwrap();
    assert!(why.contains("1 record(s) exists"), "{why}");
    assert!(!open(&fs, false).unwrap().created);
}

#[test]
fn crash_at_every_creation_step() {
    let probe = fresh();
    let before = probe.ops();
    drop(open(&probe, true).unwrap());
    let steps = probe.ops() - before;
    for budget in 0..steps {
        for how in [Crash::Kill, Crash::PowerCut] {
            let fs = fresh();
            fs.set_budget(Some(budget));
            assert!(open(&fs, true).is_err(), "budget {budget}: creation should have crashed");
            fs.crash(how);
            let ctx = format!("budget {budget} {how:?}");
            match open(&fs, false) {
                // The rename happened and survived: a finished, empty store.
                Ok(o) => assert_eq!(o.d, 0, "{ctx}"),
                Err(why) => {
                    assert!(why.contains("MAKO_RAFT_CREATE=1"), "{ctx}: {why}");
                    if fs.exists(Path::new("/d/run/1-0.creating")) {
                        assert!(why.contains("creation interrupted"), "{ctx}: {why}");
                    }
                    // A creating relaunch always succeeds and leaves no side directory.
                    let o = open(&fs, true).unwrap_or_else(|e| panic!("{ctx}: {e}"));
                    assert!(o.created, "{ctx}");
                    assert!(!fs.exists(Path::new("/d/run/1-0.creating")), "{ctx}");
                }
            }
        }
    }
}

/// Record 1 of a fresh store, encoded: entry 1 (term 1).
fn record_one() -> Vec<u8> {
    let r: raft_store::Record<Vec<u8>> = raft_store::Record { replace_from: Some(1), entries: vec![(1, vec![7])], ..Default::default() };
    let mut b = Vec::new();
    raft_store::record::encode(&r, &BytesCodec, &mut b);
    b
}

/// The store's directories above `/d`, created and never synced: `/d` does
/// not durably hold `run` (the launch's `create_dir_all`, or a killed one's).
fn unsynced_data_dir() -> MemFs {
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/d"));
    fs.create_dir(Path::new("/d/run")).unwrap();
    fs
}

/// A new data directory is made durable before anything is acknowledged in
/// the store inside it: a power cut after an acknowledged append keeps the
/// store, not just its entry in a directory the cut takes away (B35).
#[test]
fn a_new_data_directory_survives_a_power_cut() {
    let fs = unsynced_data_dir();
    let mut o = open(&fs, true).unwrap();
    o.wal.append(1, &[record_one()]).unwrap();
    drop(o);
    fs.crash(Crash::PowerCut);
    let o = open(&fs, false).unwrap_or_else(|why| panic!("the acknowledged store was lost: {why}"));
    assert_eq!(o.d, 1);
    assert_eq!(o.state.entries, vec![(1, vec![7])]);
}

/// Whatever a killed creation left, a store recovery opens keeps what this
/// launch then acknowledged across a power cut: recovery makes the whole path
/// durable first, a data directory nobody synced included (B31, B35).
#[test]
fn what_recovery_acknowledges_survives_a_power_cut() {
    let probe = unsynced_data_dir();
    let before = probe.ops();
    drop(open(&probe, true).unwrap());
    let steps = probe.ops() - before;
    let mut recovered = 0;
    for budget in 0..=steps {
        let fs = unsynced_data_dir();
        fs.set_budget(Some(budget));
        let _ = open(&fs, true);
        fs.crash(Crash::Kill);
        let Ok(mut o) = open(&fs, false) else { continue };
        recovered += 1;
        assert_eq!(o.d, 0, "budget {budget}");
        o.wal.append(1, &[record_one()]).unwrap();
        drop(o);
        fs.crash(Crash::PowerCut);
        let o = open(&fs, false).unwrap_or_else(|why| panic!("budget {budget}: the acknowledged store was lost: {why}"));
        assert_eq!(o.d, 1, "budget {budget}");
    }
    assert!(recovered > 0, "no budget left a store to recover");
}

/// No writer names a segment 0 (records are numbered from 1), so a store
/// whose only segment is named 0 is refused, not opened with its last
/// durable record wrapped to u64::MAX -- which acknowledged every later
/// batch before its sync (bugs-found B45).
#[test]
fn a_segment_named_zero_is_refused() {
    let fs = fresh();
    drop(open(&fs, true).unwrap());
    let wal = store().join("wal");
    let zero = wal.join(raft_store::segment::name(0));
    fs.rename(&wal.join(raft_store::segment::name(1)), &zero).unwrap();
    fs.poke(&zero, raft_store::segment::header(&ID, 0));
    let why = open(&fs, false).err().unwrap();
    assert!(why.contains("starts at record 0"), "{why}");
}
