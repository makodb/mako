#![cfg(mtree_native_integration)]

// This executable uses the public Rust API and its default native runtime.
// Legacy FFI unit fixtures deliberately initialize a smaller process-wide
// runtime, so they run in their own test executable.
use std::{ops::Bound, sync::Arc};
use sto_masstree_api::native::*;

#[test]
fn prepared_rows_freezes_payload_without_installing_or_releasing_the_participant() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    worker.replay(&table, b"key", Some(b"before"), 100).unwrap();
    let mut participant = worker.begin_participant(0).unwrap();
    assert_eq!(
        participant.get(&table, b"key").unwrap().value.as_deref(),
        Some(b"before".as_slice())
    );
    participant
        .mutate(&table, 1, Mutation::Put, b"key", b"prepared", false)
        .unwrap();
    participant.lock().unwrap();
    let timestamp = participant.timestamp().unwrap();
    participant.validate().unwrap();
    let redo = participant.prepared_rows(timestamp).unwrap();
    assert_eq!(redo, participant.prepared_rows(timestamp).unwrap());
    assert_eq!(redo[0].value.as_deref(), Some(b"prepared".as_slice()));
    assert_eq!(participant.report().unwrap().phase, 2);
    participant.finish(false).unwrap();
    let mut read = worker.begin_participant(0).unwrap();
    assert_eq!(
        read.get(&table, b"key").unwrap().value.as_deref(),
        Some(b"before".as_slice())
    );
    read.finish(false).unwrap();
}

#[test]
fn owned_replay_preserves_source_metadata_tombstones_and_clock() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    worker.replay(&table, b"row", Some(b"latest"), 900).unwrap();
    worker.replay(&table, b"row", Some(b"old"), 800).unwrap();
    worker
        .replay(&table, b"row", Some(b"duplicate"), 900)
        .unwrap();
    let mut participant = worker.begin_participant(0).unwrap();
    let read = participant.get(&table, b"row").unwrap();
    assert_eq!(read.value.as_deref(), Some(b"latest".as_slice()));
    assert_eq!(read.time_term, 900);
    assert!(participant.timestamp().unwrap() > 90);
    drop(participant);
    worker.replay(&table, b"row", None, 1000).unwrap();
    worker
        .replay(&table, b"row", Some(b"stale resurrect"), 950)
        .unwrap();
    let mut participant = worker.begin_participant(0).unwrap();
    let read = participant.get(&table, b"row").unwrap();
    assert_eq!(read.value, None);
    assert_eq!(read.time_term, 1000);
    drop(participant);
    assert_eq!(table.len(), 0);
    drop(worker);
    let other = Database::new(DatabaseConfig::default()).unwrap();
    let mut other_worker = other.worker().unwrap();
    assert_eq!(
        other_worker
            .replay(&table, b"row", Some(b"foreign"), 1100)
            .unwrap_err()
            .kind,
        ErrorKind::InvalidUse
    );
}

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
