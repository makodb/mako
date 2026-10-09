//! Store creation's decisions (2026-10-09 decision; plan P2, P5): only a
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
