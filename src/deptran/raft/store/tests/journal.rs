use raft_store::{
    base::{Base, MemBase},
    journal::{decode_command, encode_command, Journal},
    Hard, Identity, Record,
};
const ID: Identity = Identity {
    site: 1,
    partition: 7,
    fingerprint: 77,
    format: 0x524c_0001,
};
fn record(index: u64, commit: u64, bytes: &[u8]) -> Record<Vec<u8>> {
    Record {
        hard: Some(Hard {
            term: 1,
            vote: u16::MAX,
            commit,
        }),
        replace_from: Some(index),
        entries: vec![(1, encode_command(Some(bytes)))],
        ..Record::default()
    }
}
#[test]
fn same_log_bytes_preserve_local_and_replicated_committed_prefixes() {
    let local = MemBase::new();
    let raft = MemBase::new();
    let mut a = Journal::create(Box::new(local.clone()), &ID, b"local").unwrap();
    let mut b = Journal::create(Box::new(raft.clone()), &ID, b"raft").unwrap();
    a.persist(record(1, 1, b"prepare")).unwrap();
    b.persist(record(1, 0, b"prepare")).unwrap();
    assert_eq!(local.scan(b"e").unwrap(), raft.scan(b"e").unwrap());
    assert_eq!(b.state.hard.commit, 0, "append is not commit");
    b.persist(Record {
        hard: Some(Hard {
            term: 1,
            vote: 3,
            commit: 1,
        }),
        ..Record::default()
    })
    .unwrap();
    local.crash();
    raft.crash();
    let a = Journal::open(Box::new(local.clone()), &ID, b"local").unwrap();
    let b = Journal::open(Box::new(raft.clone()), &ID, b"raft").unwrap();
    assert_eq!(a.state.entries, b.state.entries);
    assert_eq!(b.state.hard.commit, 1);
    assert!(Journal::open(Box::new(raft), &ID, b"local").is_err());
}
#[test]
fn uncertain_batch_fences_until_recovery_and_never_acknowledges_unsynced_data() {
    for budget in 0..=2 {
        let base = MemBase::new();
        let mut log = Journal::create(Box::new(base.clone()), &ID, b"local").unwrap();
        base.set_budget(Some(budget));
        let saved = log.persist(record(1, 1, b"decide")).is_ok();
        assert_eq!(saved, budget == 2);
        if !saved {
            assert!(log.persist(record(1, 1, b"changed")).is_err());
        }
        base.crash();
        let reopened = Journal::open(Box::new(base), &ID, b"local").unwrap();
        assert_eq!(reopened.state.hard.commit, u64::from(saved));
    }
}
#[test]
fn malformed_records_cannot_change_authority_or_committed_history() {
    let base = MemBase::new();
    let mut log = Journal::create(Box::new(base), &ID, b"raft").unwrap();
    log.persist(record(1, 1, b"commit")).unwrap();
    assert!(log.persist(record(1, 1, b"abort")).is_err());
    assert!(log.persist(record(3, 3, b"gap")).is_err());
    assert!(log.persist(record(2, 3, b"future commit")).is_err());
    assert_eq!(log.sequence, 1);
    log.persist(record(2, 2, b"next")).unwrap();
    assert_eq!(
        decode_command(&log.state.entries[0].1).unwrap(),
        Some(b"commit".as_slice())
    );
}
#[cfg(feature = "rocksdb")]
#[test]
fn both_drivers_use_rocks_wal_without_destructor_flush() {
    use raft_store::journal::RocksLog;
    const CHILD: &str = "MAKO_SHARED_LOG_CHILD";
    if let Some(path) = std::env::var_os(CHILD) {
        let mut log =
            RocksLog::open(std::path::Path::new(&path), ID, b"local", true, true).unwrap();
        log.journal
            .persist(record(1, 1, b"decision survives"))
            .unwrap();
        std::process::exit(0);
    }
    let path = std::env::temp_dir().join(format!("shared-rocks-log-{}", std::process::id()));
    assert!(!path.exists());
    assert!(std::process::Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "both_drivers_use_rocks_wal_without_destructor_flush"
        ])
        .env(CHILD, &path)
        .status()
        .unwrap()
        .success());
    let log = RocksLog::open(&path, ID, b"local", false, true).unwrap();
    assert_eq!(log.journal.state.hard.commit, 1);
    assert_eq!(
        decode_command(&log.journal.state.entries[0].1).unwrap(),
        Some(b"decision survives".as_slice())
    );
    assert!(!path.join("wal").exists());
    drop(log);
    assert!(RocksLog::open(&path, ID, b"raft", false, true).is_err());
    assert!(RocksLog::open(&path, ID, b"local", false, false).is_err());
    std::fs::remove_dir_all(path).unwrap();
}

#[test]
fn corrupt_committed_prefix_and_snapshot_bounds_fail_closed() {
    use raft_store::base::{Op, KEY_HARD, KEY_SNAP};
    for snapshot in [false, true] {
        let base = MemBase::new();
        drop(Journal::create(Box::new(base.clone()), &ID, b"local").unwrap());
        let mut raw = base.clone();
        let op = if snapshot {
            let mut bytes = u64::MAX.to_le_bytes().to_vec();
            bytes.extend_from_slice(&0u64.to_le_bytes());
            bytes.extend_from_slice(&[0, 1, 0, b'x']);
            Op::Put(KEY_SNAP.to_vec(), bytes)
        } else {
            let mut bytes = 1u64.to_le_bytes().to_vec();
            bytes.extend_from_slice(&u16::MAX.to_le_bytes());
            bytes.extend_from_slice(&2u64.to_le_bytes());
            Op::Put(KEY_HARD.to_vec(), bytes)
        };
        raw.persist(&[op]).unwrap();
        base.crash();
        assert!(Journal::open(Box::new(base), &ID, b"local").is_err());
    }
}

#[cfg(feature = "rocksdb")]
#[test]
fn format_cutover_preserves_existing_data_and_refuses_concurrent_owners() {
    use raft_store::journal::RocksLog;
    let root = std::env::temp_dir().join(format!("shared-log-refusals-{}", std::process::id()));
    assert!(!root.exists());
    let legacy = root.join("legacy");
    std::fs::create_dir_all(legacy.join("wal")).unwrap();
    let sentinel = legacy.join("wal/retained-segment");
    std::fs::write(&sentinel, b"old log bytes").unwrap();
    for create in [false, true] {
        assert!(RocksLog::open(&legacy, ID, b"raft", create, true).is_err());
        assert_eq!(std::fs::read(&sentinel).unwrap(), b"old log bytes");
        assert!(!legacy.join("base").exists());
    }
    let current = root.join("current");
    assert!(RocksLog::open(&current, ID, b"local", false, true).is_err());
    let mut log = RocksLog::open(&current, ID, b"local", true, true).unwrap();
    assert!(RocksLog::open(&current, ID, b"local", false, true).is_err());
    log.journal
        .persist(record(1, 1, b"retained commit"))
        .unwrap();
    drop(log);
    assert!(RocksLog::open(&current, ID, b"local", true, true).is_err());
    let log = RocksLog::open(&current, ID, b"local", false, true).unwrap();
    assert_eq!(log.journal.state.hard.commit, 1);
    assert_eq!(
        decode_command(&log.journal.state.entries[0].1).unwrap(),
        Some(b"retained commit".as_slice())
    );
    drop(log);
    std::fs::remove_dir_all(root).unwrap();
}
