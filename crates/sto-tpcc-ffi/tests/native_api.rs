#![cfg(mtree_native_integration)]

// This executable uses the public Rust API and its default native runtime.
// Legacy FFI unit fixtures deliberately initialize a smaller process-wide
// runtime, so they run in their own test executable.
use std::{ops::Bound, sync::Arc};
use sto_tpcc_ffi::native::*;

#[test]
fn owned_api_preserves_crud_scan_abort_and_table_lifetime() {
    fn shared<T: Send + Sync>() {}
    shared::<Database>();
    shared::<Table>();
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut tx = worker.begin().unwrap();
    assert!(tx.insert(&table, b"a", b"one").unwrap());
    assert!(!tx.insert(&table, b"a", b"duplicate").unwrap());
    tx.put(&table, b"b", &vec![42; 1024]).unwrap();
    assert_eq!(tx.get(&table, b"b").unwrap(), Some(vec![42; 1024]));
    tx.commit().unwrap();
    assert_eq!(table.len(), 2);

    let mut tx = worker.begin().unwrap();
    assert!(tx.remove(&table, b"a").unwrap());
    drop(tx);
    assert_eq!(table.len(), 2);
    let mut tx = worker.begin().unwrap();
    let rows = tx
        .scan(
            &table,
            Bound::Included(b"a"),
            Bound::Excluded(b"c"),
            false,
            10,
        )
        .unwrap();
    assert_eq!(
        rows.iter().map(|r| r.0.as_slice()).collect::<Vec<_>>(),
        vec![b"a", b"b"]
    );
    let reverse = tx
        .scan(&table, Bound::Unbounded, Bound::Unbounded, true, 1)
        .unwrap();
    assert_eq!(reverse[0].0, b"b");
    drop(table);
    drop(db);
    tx.commit().unwrap();
}

#[test]
fn forgotten_guard_keeps_tables_alive_until_explicit_cleanup() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&table, b"key", b"aborted").unwrap();
    std::mem::forget(tx);
    assert!(worker.begin().is_err());
    assert!(worker.create_table(TableConfig::default()).is_err());
    drop(table);
    worker.abort().unwrap();
    worker.begin().unwrap().commit().unwrap();
}

#[test]
fn shared_tables_preserve_conflicts_between_affine_workers() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let table = {
        let mut loader = db.worker().unwrap();
        let table = loader.create_table(TableConfig::default()).unwrap();
        let mut tx = loader.begin().unwrap();
        tx.put(&table, b"counter", b"0").unwrap();
        tx.commit().unwrap();
        table
    };
    let both_read = Arc::new(std::sync::Barrier::new(2));
    let threads: Vec<_> = (0..2)
        .map(|_| {
            let db = db.clone();
            let table = table.clone();
            let both_read = both_read.clone();
            std::thread::spawn(move || {
                let mut worker = db.worker().unwrap();
                let mut tx = worker.begin().unwrap();
                assert_eq!(
                    tx.get(&table, b"counter").unwrap().as_deref(),
                    Some(b"0".as_slice())
                );
                both_read.wait();
                tx.put(&table, b"counter", b"1").and_then(|_| tx.commit())
            })
        })
        .collect();
    let outcomes: Vec<_> = threads.into_iter().map(|t| t.join().unwrap()).collect();
    assert_eq!(outcomes.iter().filter(|r| r.is_ok()).count(), 1);
    assert_eq!(
        outcomes
            .iter()
            .filter(|r| matches!(r, Err(e) if e.kind == ErrorKind::Conflict))
            .count(),
        1
    );
}

#[test]
fn table_from_another_database_is_rejected_before_access() {
    let table = std::thread::spawn(|| {
        let db = Database::new(DatabaseConfig::default()).unwrap();
        db.worker()
            .unwrap()
            .create_table(TableConfig::default())
            .unwrap()
    })
    .join()
    .unwrap();
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let mut tx = worker.begin().unwrap();
    assert_eq!(
        tx.get(&table, b"key").unwrap_err().kind,
        ErrorKind::InvalidUse
    );
    tx.abort().unwrap();
}
