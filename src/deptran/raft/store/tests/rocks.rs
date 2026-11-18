//! The RocksDB base (`--features rocksdb`): batches with RocksDB's
//! WAL off, a waiting flush, reopening, range deletes; a missing base is
//! refused, never created on an ordinary open.
#![cfg(feature = "rocksdb")]

use raft_store::base::{self, entry_key, ops_for, Base, Op};
use raft_store::rocks::RocksBase;
use raft_store::{Hard, Identity, Record};

const ID: Identity = Identity { site: 4, partition: 2, fingerprint: 77, format: 1 };

#[test]
fn rocks_roundtrip() {
    let dir = std::env::temp_dir().join(format!("raft-rocks-test-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("base");
    assert!(RocksBase::open(&path, false).is_err(), "an ordinary open created a base");
    {
        let mut b = RocksBase::open(&path, true).unwrap();
        b.write(&[Op::Put(base::KEY_ID.to_vec(), base::identity_bytes(&ID)),
                  Op::Put(base::KEY_C.to_vec(), 0u64.to_le_bytes().to_vec())]).unwrap();
        let mut ops = Vec::new();
        let rec: Record<Vec<u8>> = Record {
            hard: Some(Hard { term: 3, vote: 1, commit: 2 }),
            replace_from: Some(1),
            entries: (1..=5).map(|i| (3, vec![i as u8; 10])).collect(),
            ..Default::default()
        };
        let mut top = u64::MAX;
        ops_for(&rec, 1, &mut top, &mut ops);
        let cut: Record<Vec<u8>> = Record { replace_from: Some(4), entries: vec![(4, b"x".to_vec())], ..Default::default() };
        ops_for(&cut, 2, &mut top, &mut ops);
        assert_eq!(top, 4);
        let ranges = |ops: &[Op]| ops.iter().filter(|o| matches!(o, Op::DeleteRange(..))).count();
        assert_eq!(ranges(&ops), 2, "the first record (top unknown) and the cut delete");
        // An append past the top deletes nothing; a cut at or below it does.
        let mut more = Vec::new();
        let app: Record<Vec<u8>> = Record { replace_from: Some(5), entries: vec![(4, b"y".to_vec())], ..Default::default() };
        ops_for(&app, 3, &mut top, &mut more);
        assert_eq!((ranges(&more), top), (0, 5));
        let cut2: Record<Vec<u8>> = Record { replace_from: Some(5), entries: vec![], ..Default::default() };
        ops_for(&cut2, 4, &mut top, &mut more);
        assert_eq!((ranges(&more), top), (1, 4));
        ops.extend(more);
        b.write(&ops).unwrap();
        b.flush().unwrap();
        assert!(b.get(&entry_key(5)).unwrap().is_none());
    }
    assert!(RocksBase::open(&path, true).is_err(), "a creating open accepted an existing base");
    let b = RocksBase::open(&path, false).unwrap();
    let (c, state) = base::load(&b, &ID, u16::MAX).unwrap();
    assert_eq!((c, state.hard.term, state.last()), (4, 3, 4));
    assert_eq!(state.entries[3], (4, b"x".to_vec()));
    let other = Identity { site: 5, ..ID };
    assert!(base::load(&b, &other, u16::MAX).unwrap_err().contains("another server"));
    drop(b);
    std::fs::remove_dir_all(&dir).unwrap();
}

/// Exit the writer without running RocksBase::drop or requesting a flush.
/// Recovery must come from real WAL writes, not a destructor's memtable flush.
#[test]
fn async_base_wal_survives_process_exit() {
    const CHILD_PATH: &str = "RAFT_ASYNC_BASE_WAL_TEST_PATH";
    if let Some(path) = std::env::var_os(CHILD_PATH) {
        let mut base = RocksBase::open_with_sync_writes(std::path::Path::new(&path), true, false).unwrap();
        base.write(&[
            Op::Put(base::KEY_ID.to_vec(), base::identity_bytes(&ID)),
            Op::Put(base::KEY_C.to_vec(), 19u64.to_le_bytes().to_vec()),
            Op::Put(b"payload".to_vec(), b"written without a flush".to_vec()),
        ]).unwrap();
        std::process::exit(0);
    }
    let directory = std::env::temp_dir().join(format!("raft-async-rocks-test-{}-{}",
        std::process::id(), std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
    std::fs::create_dir(&directory).unwrap();
    let path = directory.join("base");
    let status = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "async_base_wal_survives_process_exit", "--nocapture"])
        .env(CHILD_PATH, &path).status().unwrap();
    assert!(status.success());
    let base = RocksBase::open_with_sync_writes(&path, false, false).unwrap();
    assert_eq!(base.get(base::KEY_C).unwrap().unwrap(), 19u64.to_le_bytes());
    assert_eq!(base.get(b"payload").unwrap().unwrap(), b"written without a flush");
    drop(base);
    std::fs::remove_dir_all(directory).unwrap();
}
