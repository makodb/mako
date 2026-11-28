#![cfg(mtree_native_integration)]
use sto_masstree_api::native::*;
fn finish(mut participant: Participant<'_>) -> ParticipantReport {
    participant.lock().unwrap();
    let base = participant.timestamp().unwrap();
    participant.validate().unwrap();
    participant.install(base).unwrap();
    participant.finish(false).unwrap()
}
#[test]
fn retained_participant_repeated_locking_installs_copied_final_payload_and_metadata() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut p = worker.begin_participant(3).unwrap();
    let mut bytes = b"first".to_vec();
    p.mutate(&table, 201, Mutation::Put, b"row", &bytes, true)
        .unwrap();
    bytes.fill(b'x');
    assert_eq!(
        p.get(&table, b"row").unwrap().value,
        Some(b"first".to_vec())
    );
    p.retain();
    let mut p = worker.resume_participant().unwrap();
    p.mutate(&table, 201, Mutation::Put, b"row", b"final", true)
        .unwrap();
    assert_eq!(
        p.get(&table, b"row").unwrap().value,
        Some(b"final".to_vec())
    );
    p.validate().unwrap();
    let base = p.timestamp().unwrap();
    let redo = p.prepared_rows(base).unwrap();
    assert_eq!(redo.len(), 1);
    assert_eq!(redo[0].value, Some(b"final".to_vec()));
    assert_eq!(redo, p.prepared_rows(base).unwrap());
    assert!(p.prepared_rows(base + 1).is_err());
    p.install(base).unwrap();
    let report = p.finish(false).unwrap();
    assert_eq!(report.disposition, 2);
    assert_eq!(report.time_term, base * 10 + 3);
    let mut p = worker.begin_participant(3).unwrap();
    assert_eq!(
        p.get(&table, b"row").unwrap(),
        VersionedValue {
            value: Some(b"final".to_vec()),
            time_term: base * 10 + 3
        }
    );
    p.finish(false).unwrap();
}
#[test]
fn historical_mutations_publish_tombstones_without_corrupting_physical_cardinality() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    worker.replay(&table, b"row", Some(b"stable"), 100).unwrap();
    worker.replay(&table, b"row", None, 200).unwrap();
    let mut p = worker.begin_participant(1).unwrap();
    let mut stable = [None; 10];
    stable[0] = Some(10);
    p.visibility(stable, None).unwrap();
    assert_eq!(
        p.get(&table, b"row").unwrap().value,
        Some(b"stable".to_vec())
    );
    assert!(p
        .mutate(&table, 201, Mutation::Remove, b"row", b"", false)
        .unwrap());
    assert_eq!(p.get(&table, b"row").unwrap().value, None);
    finish(p);
    assert_eq!(table.len(), 0);
    let other = worker.create_table(TableConfig::default()).unwrap();
    worker.replay(&other, b"row", None, 110).unwrap();
    worker
        .replay(&other, b"row", Some(b"speculative"), 210)
        .unwrap();
    let mut p = worker.begin_participant(1).unwrap();
    stable[0] = Some(11);
    p.visibility(stable, None).unwrap();
    assert!(p
        .mutate(&other, 202, Mutation::Insert, b"row", b"replacement", false)
        .unwrap());
    finish(p);
    assert_eq!(other.len(), 1);
}
#[test]
fn checkpoint_exports_committed_tombstones_behind_retained_prepare_locks() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    worker.replay(&table, b"gone", None, 100).unwrap();
    worker.replay(&table, b"live", Some(b"old"), 110).unwrap();
    let mut p = worker.begin_participant(0).unwrap();
    p.mutate(&table, 1, Mutation::Put, b"live", b"prepared", true)
        .unwrap();
    p.validate().unwrap();
    p.retain();
    let db2 = db.clone();
    let table2 = table.clone();
    std::thread::spawn(move || {
        let mut exporter = db2.worker().unwrap();
        let page = exporter.checkpoint_page(&table2, None, 16).unwrap();
        assert!(page.end);
        assert_eq!(
            page.rows,
            vec![
                CheckpointRow {
                    key: b"gone".to_vec(),
                    value: None,
                    time_term: 100
                },
                CheckpointRow {
                    key: b"live".to_vec(),
                    value: Some(b"old".to_vec()),
                    time_term: 110
                }
            ]
        );
    })
    .join()
    .unwrap();
    worker.resume_participant().unwrap().finish(false).unwrap();
}
#[test]
fn abandoned_installed_participant_fences_worker_and_preserves_publication() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut p = worker.begin_participant(0).unwrap();
    p.mutate(&table, 1, Mutation::Put, b"row", b"installed", false)
        .unwrap();
    p.validate().unwrap();
    let base = p.timestamp().unwrap();
    p.install(base).unwrap();
    drop(p);
    assert!(matches!(
        worker.begin(),
        Err(Error {
            kind: ErrorKind::UnknownOutcome,
            ..
        })
    ));
    assert!(worker.abort().is_err());
    std::thread::spawn(move || {
        let mut w = db.worker().unwrap();
        let mut tx = w.begin().unwrap();
        assert_eq!(tx.get(&table, b"row").unwrap(), Some(b"installed".to_vec()));
        tx.commit().unwrap();
    })
    .join()
    .unwrap();
}
