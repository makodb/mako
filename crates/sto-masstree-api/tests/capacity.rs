#![cfg(mtree_native_integration)]
use sto_masstree_api::native::*;
#[test]
fn scalar_capacity_aborts_staged_changes_and_preserves_existing_access() {
    for config in [
        TableConfig {
            max_retained_records: 2,
            ..Default::default()
        },
        TableConfig {
            max_retained_key_bytes: 2,
            ..Default::default()
        },
    ] {
        let db = Database::new(DatabaseConfig::default()).unwrap();
        let mut worker = db.worker().unwrap();
        let table = worker.create_table(config).unwrap();
        let mut tx = worker.begin().unwrap();
        tx.put(&table, b"a", b"original").unwrap();
        tx.commit().unwrap();
        let mut tx = worker.begin().unwrap();
        tx.put(&table, b"a", b"rollback").unwrap();
        tx.put(&table, b"b", b"rollback").unwrap();
        assert_eq!(
            tx.put(&table, b"c", b"no room").unwrap_err().kind,
            ErrorKind::Capacity
        );
        assert!(tx.commit().is_err());
        let mut tx = worker.begin().unwrap();
        assert_eq!(tx.get(&table, b"a").unwrap(), Some(b"original".to_vec()));
        assert_eq!(tx.get(&table, b"b").unwrap(), None);
        tx.put(&table, b"a", b"updated").unwrap();
        tx.commit().unwrap();
        let mut tx = worker.begin().unwrap();
        // Missing reads no longer consume row capacity. The write limit above
        // still applies, while absence is certified by the native leaf version.
        assert_eq!(tx.get(&table, b"c").unwrap(), None);
        tx.commit().unwrap();
        let mut tx = worker.begin().unwrap();
        assert_eq!(tx.get(&table, b"a").unwrap(), Some(b"updated".to_vec()));
        tx.commit().unwrap();
        assert_eq!(table.len(), 1);
    }
}
#[test]
fn native_row_budget_is_shared_and_exhaustion_preserves_existing_tables() {
    let maximum = 8192;
    let db = Database::new(DatabaseConfig {
        max_native_row_bytes: maximum,
        ..Default::default()
    })
    .unwrap();
    let mut worker = db.worker().unwrap();
    let first = worker.create_table(TableConfig::default()).unwrap();
    let second = worker.create_table(TableConfig::default()).unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&first, b"saved", b"value").unwrap();
    tx.commit().unwrap();
    let before = db.native_row_bytes();
    let mut exhausted = false;
    for id in 0..1024u64 {
        let mut tx = worker.begin().unwrap();
        match tx.put(&second, &id.to_be_bytes(), b"row") {
            Ok(()) => tx.commit().unwrap(),
            Err(e) => {
                assert_eq!(e.kind, ErrorKind::Capacity);
                exhausted = true;
                break;
            }
        }
    }
    assert!(exhausted);
    assert!(db.native_row_bytes() > before);
    assert!(db.native_row_bytes() <= maximum);
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&first, b"saved").unwrap(), Some(b"value".to_vec()));
    tx.put(&first, b"saved", b"updated").unwrap();
    tx.commit().unwrap();
}
#[test]
fn scan_capacity_failure_cannot_commit_prior_writes() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker
        .create_table(TableConfig {
            max_scan_physical_records: 1,
            ..Default::default()
        })
        .unwrap();
    let mut tx = worker.begin().unwrap();
    for key in [b"a", b"b"] {
        tx.put(&table, key, b"old").unwrap();
    }
    tx.commit().unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&table, b"a", b"new").unwrap();
    assert!(tx
        .scan(
            &table,
            std::ops::Bound::Unbounded,
            std::ops::Bound::Unbounded,
            false,
            5
        )
        .is_err());
    assert!(tx.commit().is_err());
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&table, b"a").unwrap(), Some(b"old".to_vec()));
    tx.commit().unwrap();
}
#[test]
fn commit_lock_capacity_releases_acquired_locks_without_installing() {
    let db = Database::new(DatabaseConfig {
        max_locks_per_txn: 1,
        ..Default::default()
    })
    .unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&table, b"a", b"one").unwrap();
    tx.put(&table, b"b", b"two").unwrap();
    assert_eq!(tx.commit().unwrap_err().kind, ErrorKind::Capacity);
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&table, b"a").unwrap(), None);
    assert_eq!(tx.get(&table, b"b").unwrap(), None);
    tx.commit().unwrap();
    assert_eq!(table.len(), 0);
}
