use sto_rocksdb::{
    distributed::{Prefix, Row},
    Database, Position, RequestId,
};
fn row(value: Option<&[u8]>, time: u32) -> Row {
    Row {
        table: 1,
        key: b"k".to_vec(),
        value: value.map(<[u8]>::to_vec),
        time_term: time,
    }
}
#[test]
fn external_batch_recovery_checkpoint_and_profile() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("db");
    let db = Database::open_distributed(&root, 7, true).unwrap();
    db.distributed_table(1).unwrap();
    db.bind_distributed_identity(b"schema").unwrap();
    let position = Position { index: 1, term: 1 };
    let prefix = Prefix {
        position,
        fingerprint: [3; 32],
    };
    let mut worker = db.attach_worker().unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(1, b"k", b"v").unwrap();
    let mut prepared = tx
        .prepare(RequestId {
            client: 1,
            sequence: 1,
        })
        .unwrap();
    prepared.certify().unwrap();
    assert!(db.distributed_page(1, None, 8).unwrap().0.is_empty());
    db.apply_distributed(
        Some(&prefix),
        &[row(Some(b"v"), 100)],
        &vec![(b"decision".to_vec(), b"commit".to_vec())],
    )
    .unwrap();
    prepared.complete_distributed(100).unwrap();
    drop(prepared);
    drop(worker);
    db.apply_distributed(Some(&prefix), &[row(Some(b"v"), 100)], &Vec::new())
        .unwrap();
    let checkpoint = dir.path().join("checkpoint");
    db.create_checkpoint(&checkpoint, position).unwrap();
    drop(db);
    assert!(Database::open(&root, 7, true).is_err());
    let db = Database::open_distributed(&root, 7, true).unwrap();
    assert_eq!(
        db.distributed_metadata().unwrap(),
        vec![(b"decision".to_vec(), b"commit".to_vec())]
    );
    assert_eq!(
        db.distributed_page(1, None, 8).unwrap().0,
        vec![row(Some(b"v"), 100)]
    );
    let target = Database::open_distributed(dir.path().join("target"), 7, true).unwrap();
    target.validate_checkpoint(&checkpoint, position).unwrap();
    target.publish_checkpoint(&checkpoint, position).unwrap();
    assert_eq!(
        target.distributed_identity().unwrap().as_deref(),
        Some(b"schema".as_slice())
    );
    assert_eq!(
        target.distributed_metadata().unwrap(),
        db.distributed_metadata().unwrap()
    );
    let delete = Prefix {
        position: Position { index: 2, term: 1 },
        fingerprint: [4; 32],
    };
    target
        .apply_distributed(Some(&delete), &[row(None, 110)], &Vec::new())
        .unwrap();
    target
        .apply_distributed(None, &[row(Some(b"v"), 100)], &Vec::new())
        .unwrap();
    assert_eq!(target.count(1).unwrap(), 0);
    assert_eq!(
        target.distributed_page(1, None, 8).unwrap().0,
        vec![row(None, 110)]
    );
}
#[cfg(feature = "test-hooks")]
#[test]
fn ambiguous_external_batch_keeps_rows_and_metadata_together() {
    for (fault, applied) in [
        (sto_rocksdb::ApplyFault::BeforeBatch, false),
        (sto_rocksdb::ApplyFault::AfterBatch, true),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let db = Database::open_distributed(dir.path(), 3, true).unwrap();
        db.distributed_table(1).unwrap();
        let prefix = Prefix {
            position: Position { index: 1, term: 1 },
            fingerprint: [5; 32],
        };
        db.inject_apply_fault(fault);
        assert!(db
            .apply_distributed(
                Some(&prefix),
                &[row(Some(b"v"), 100)],
                &vec![(b"prepared".to_vec(), b"yes".to_vec())]
            )
            .is_err());
        assert!(db.is_fenced());
        drop(db);
        let db = Database::open_distributed(dir.path(), 3, true).unwrap();
        assert_eq!(db.applied().unwrap().index, u64::from(applied));
        assert_eq!(
            !db.distributed_page(1, None, 8).unwrap().0.is_empty(),
            applied
        );
        assert_eq!(!db.distributed_metadata().unwrap().is_empty(), applied);
        db.apply_distributed(
            Some(&prefix),
            &[row(Some(b"v"), 100)],
            &vec![(b"prepared".to_vec(), b"yes".to_vec())],
        )
        .unwrap();
    }
}

#[test]
fn private_import_publishes_exact_prefix_and_refuses_reuse_or_a_gap() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("good");
    let history = vec![
        Prefix {
            position: Position { index: 1, term: 2 },
            fingerprint: [1; 32],
        },
        Prefix {
            position: Position { index: 2, term: 5 },
            fingerprint: [9; 32],
        },
    ];
    let db = Database::open_distributed(&root, 7, true).unwrap();
    db.bind_distributed_identity(b"schema").unwrap();
    db.distributed_table(1).unwrap();
    db.apply_distributed(None, &[row(None, 50)], &Vec::new())
        .unwrap();
    let metadata = vec![(b"pending".to_vec(), b"prepare".to_vec())];
    db.finish_distributed_import(&history, &metadata).unwrap();
    drop(db);
    let db = Database::open_distributed(&root, 7, true).unwrap();
    assert_eq!(db.applied().unwrap(), history[1].position);
    assert_eq!(
        db.distributed_page(1, None, 8).unwrap().0,
        vec![row(None, 50)]
    );
    assert_eq!(db.distributed_metadata().unwrap(), metadata);
    let saved = db.entry_history().unwrap();
    for (a, b) in saved.iter().zip(&history) {
        assert_eq!(a.position, b.position);
        assert_eq!(a.fingerprint, b.fingerprint);
    }
    assert!(db.finish_distributed_import(&history, &metadata).is_err());
    assert!(db.distributed_metadata().is_err(), "failed import fences");
    let bad = Database::open_distributed(dir.path().join("gap"), 7, true).unwrap();
    assert!(bad
        .finish_distributed_import(&history[1..], &metadata)
        .is_err());
    assert_eq!(
        bad.applied().unwrap(),
        Position::default(),
        "invalid prefix was never published"
    );
}
