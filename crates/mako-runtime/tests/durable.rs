#![cfg(feature = "durable-storage")]
use mako_runtime::{durable::DurableRows, redo::Row};
use sto_rocksdb::Position;

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
