//! The RocksDB base (plan P7; `--features rocksdb`): batches with RocksDB's
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
        ops_for(&rec, 1, &mut ops);
        let cut: Record<Vec<u8>> = Record { replace_from: Some(4), entries: vec![(4, b"x".to_vec())], ..Default::default() };
        ops_for(&cut, 2, &mut ops);
        b.write(&ops).unwrap();
        b.flush().unwrap();
        assert!(b.get(&entry_key(5)).unwrap().is_none());
    }
    assert!(RocksBase::open(&path, true).is_err(), "a creating open accepted an existing base");
    let b = RocksBase::open(&path, false).unwrap();
    let (c, state) = base::load(&b, &ID, u16::MAX).unwrap();
    assert_eq!((c, state.hard.term, state.last()), (2, 3, 4));
    assert_eq!(state.entries[3], (4, b"x".to_vec()));
    let other = Identity { site: 5, ..ID };
    assert!(base::load(&b, &other, u16::MAX).unwrap_err().contains("another server"));
    drop(b);
    std::fs::remove_dir_all(&dir).unwrap();
}
