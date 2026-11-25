#![cfg(feature = "durable-storage")]
use mako_runtime::{durable::DurableRows, redo::Row};
use sto_rocksdb::Position;
use mako_protocol::journal::{self, Member, Outcome, Record, Transaction, TransactionId};

#[test]
fn prepared_intent_survives_restart_without_becoming_a_committed_row() {
    let directory = tempfile::tempdir().unwrap();
    let transaction = Transaction {
        version: journal::VERSION,
        id: TransactionId { coordinator: 0, worker: 0, incarnation: 19, attempt: 1 },
        time_term: 400, durable: true,
        members: vec![Member { owner: 0, endpoints: vec!["shard-0".into()], lease: 20 }],
    };
    let write = row(1, b"key", Some(b"value"), 400);
    let begin = Record::Begin(transaction.clone());
    let prepare = Record::Prepare { transaction: transaction.clone(), rows: vec![write.clone()] };
    let decide = Record::Decide { transaction: transaction.clone(), outcome: Outcome::Commit };
    let complete = Record::Complete { transaction: transaction.clone(), outcome: Outcome::Commit };
    {
        let db = DurableRows::open(directory.path(), 9, true, b"journal-schema").unwrap();
        db.advance(position(1)).unwrap();
        db.apply_journal(position(2), b"begin", 0, &begin).unwrap();
        let (rows, entry) = db.apply_journal(position(3), b"prepare", 0, &prepare).unwrap();
        assert!(rows.is_empty() && all(&db).is_empty() && entry.unresolved());
        assert_eq!(entry.participant.unwrap().rows, Some(vec![write.clone()]));
    }
    {
        let db = DurableRows::open(directory.path(), 9, true, b"journal-schema").unwrap();
        assert!(all(&db).is_empty());
        let entries = db.transactions().unwrap();
        assert_eq!(entries.len(), 1);
        assert!(entries[0].unresolved());
        assert_eq!(entries[0].decision, None);
        db.apply_journal(position(4), b"decision", 0, &decide).unwrap();
        assert!(all(&db).is_empty(), "decision alone must not complete a participant");
    }
    {
        let db = DurableRows::open(directory.path(), 9, true, b"journal-schema").unwrap();
        assert_eq!(db.transaction(transaction.id).unwrap().unwrap().decision, Some(Outcome::Commit));
        let (rows, entry) = db.apply_journal(position(5), b"complete", 0, &complete).unwrap();
        assert_eq!(rows, vec![write.clone()]);
        assert_eq!(all(&db), vec![write.clone()]);
        assert!(!entry.unresolved());
        assert_eq!(entry.participant.unwrap().outcome, Some(Outcome::Commit));
        let abort = Record::Decide { transaction: transaction.clone(), outcome: Outcome::Abort };
        let (rows, retained) = db.apply_journal(position(6), b"late abort", 0, &abort).unwrap();
        assert!(rows.is_empty());
        assert_eq!(retained.decision, Some(Outcome::Commit));
        assert!(journal::acknowledge(&abort, &retained).is_err());
        assert_eq!(db.position().unwrap(), position(6));
    }
    let db = DurableRows::open(directory.path(), 9, true, b"journal-schema").unwrap();
    assert_eq!(all(&db), vec![write]);
    let (rows, entry) = db.apply_journal(position(5), b"complete", 0, &complete).unwrap();
    assert!(rows.is_empty());
    assert_eq!(entry.decision, Some(Outcome::Commit));
    assert!(db.apply_journal(position(5), b"changed command", 0, &complete).is_err());
}

fn position(index: u64) -> Position {
    Position { index, term: 1 }
}
fn row(table: u16, key: &[u8], value: Option<&[u8]>, time_term: u32) -> Row {
    Row {
        table,
        key: key.to_vec(),
        value: value.map(<[u8]>::to_vec),
        time_term,
    }
}
fn all(db: &DurableRows) -> Vec<Row> {
    let mut rows = Vec::new();
    db.visit(|row| {
        rows.push(row);
        Ok(())
    })
    .unwrap();
    rows
}
#[test]
fn committed_rows_reopen_with_tombstones_and_checkpoint_generations() {
    let directory = tempfile::tempdir().unwrap();
    let replica = tempfile::tempdir().unwrap();
    let snapshots = tempfile::tempdir().unwrap();
    let db = DurableRows::open(directory.path(), 9, true, b"schema-a").unwrap();
    db.advance(position(1)).unwrap();
    db.advance(position(1)).unwrap();
    let initial = vec![
        row(1, b"a", Some(b"value"), 200),
        row(2, b"b", Some(b"other"), 200),
    ];
    assert_eq!(
        db.apply(position(2), b"transaction one", &initial).unwrap(),
        initial
    );
    db.apply(position(3), b"transaction two", &[row(1, b"a", None, 300)])
        .unwrap();
    db.apply(
        position(4),
        b"old source",
        &[row(1, b"a", Some(b"stale"), 250)],
    )
    .unwrap();
    let expected = vec![row(1, b"a", None, 300), initial[1].clone()];
    assert_eq!(all(&db), expected);
    let checkpoint = snapshots.path().join("snapshot");
    db.create_checkpoint(&checkpoint, position(4)).unwrap();
    let follower = DurableRows::open(replica.path(), 9, true, b"schema-a").unwrap();
    follower
        .validate_checkpoint(&checkpoint, position(4))
        .unwrap();
    assert_eq!(follower.position().unwrap(), Position::default());
    follower
        .publish_checkpoint(&checkpoint, position(4))
        .unwrap();
    follower
        .publish_checkpoint(&checkpoint, position(4))
        .unwrap();
    assert_eq!(all(&follower), expected);
    drop(db);
    assert!(DurableRows::open(directory.path(), 9, true, b"different schema").is_err());
    let recovered = DurableRows::open(directory.path(), 9, true, b"schema-a").unwrap();
    assert_eq!(recovered.position().unwrap(), position(4));
    assert_eq!(all(&recovered), expected);
    recovered
        .apply(position(3), b"transaction two", &[row(1, b"a", None, 300)])
        .unwrap();
    assert!(recovered
        .apply(position(3), b"changed identity", &[row(1, b"a", None, 300)])
        .is_err());
    assert!(recovered
        .apply(
            position(3),
            b"transaction two",
            &[row(1, b"a", Some(b"changed effect"), 300)]
        )
        .is_err());
}
