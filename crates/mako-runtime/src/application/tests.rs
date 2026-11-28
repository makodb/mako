//! Recovery regressions over the current protocol index and native RocksDB
//! application contract. No historical row encoding participates in these tests.
use super::{Change, Index};
use crate::{redo::Row, storage::engine, wire::Result};
use mako_storage_api::{LogPosition, PersistentApplication};
use std::{path::Path, sync::Arc};
use sto_core::protocol::journal::{
    self, Entry, Member, Outcome, Record, Transaction, TransactionId,
};
use sto_rocksdb::Position;

struct Fixture {
    application: Arc<dyn PersistentApplication>,
    identity: Vec<u8>,
}
fn external(p: Position) -> LogPosition {
    LogPosition {
        index: p.index,
        term: p.term,
    }
}
fn internal(p: Position) -> super::Position {
    super::Position {
        index: p.index,
        term: p.term,
    }
}
impl Fixture {
    fn open(path: &Path, group: u64, durable: bool, identity: &[u8]) -> Result<Self> {
        let application = mako_storage_rocksdb::backing(path, group, durable).map_err(engine)?;
        application.bind_identity(identity).map_err(engine)?;
        Ok(Self {
            application,
            identity: identity.to_vec(),
        })
    }
    fn index(&self) -> Result<Index> {
        Index::from_metadata(
            self.application.metadata().map_err(engine)?,
            self.application.position().map_err(engine)?,
            0,
            true,
        )
    }
    fn save(&self, change: Change) -> Result<Vec<Row>> {
        if change.duplicate {
            return Ok(Vec::new());
        }
        self.application
            .ensure_tables(&change.table_ids())
            .map_err(engine)?;
        self.application
            .apply(
                change.position.into(),
                change.fingerprint(),
                &change.rows,
                &change.metadata()?,
            )
            .map_err(engine)?;
        Ok(change.rows)
    }
    fn advance(&self, p: Position) -> Result<()> {
        self.save(self.index()?.no_op(internal(p))?).map(|_| ())
    }
    fn apply(&self, p: Position, bytes: &[u8], rows: &[Row]) -> Result<Vec<Row>> {
        // Production command bytes include the row effects. Encode the fixture
        // payload too, so a changed effect cannot reuse the original identity.
        let source = crate::wire::encode(&(bytes, rows))?;
        let mut change = self.index()?.next(internal(p), &source)?;
        if !change.duplicate {
            change.rows = rows.to_vec();
        }
        self.save(change)
    }
    fn apply_journal(
        &self,
        p: Position,
        bytes: &[u8],
        owner: u32,
        record: &Record,
    ) -> Result<(Vec<Row>, Entry)> {
        let source = crate::wire::encode(&(bytes, record))?;
        let rows =
            self.save(
                self.index()?
                    .journal(internal(p), &source, owner, true, record)?,
            )?;
        Ok((rows, self.transaction(record.transaction().id)?.unwrap()))
    }
    fn transaction(&self, id: TransactionId) -> Result<Option<Entry>> {
        Ok(self.index()?.transactions.remove(&id))
    }
    fn transactions(&self) -> Result<Vec<Entry>> {
        Ok(self.index()?.transactions.into_values().collect())
    }
    fn position(&self) -> Result<Position> {
        let p = self.application.position().map_err(engine)?;
        Ok(Position {
            index: p.index,
            term: p.term,
        })
    }
    fn visit(&self, mut visitor: impl FnMut(Row) -> Result<()>) -> Result<()> {
        for id in self.application.table_ids().map_err(engine)? {
            let table = u16::try_from(id).unwrap();
            let mut after = None;
            loop {
                let page = self
                    .application
                    .page(table, after.as_deref(), 256)
                    .map_err(engine)?;
                assert!(page.end || !page.rows.is_empty());
                for r in page.rows {
                    after = Some(r.key.clone());
                    visitor(Row {
                        table,
                        key: r.key,
                        value: r.value,
                        time_term: r.time_term,
                    })?;
                }
                if page.end {
                    break;
                }
            }
        }
        Ok(())
    }
    fn create_checkpoint(&self, path: &Path, p: Position) -> Result<()> {
        self.application
            .create_checkpoint(path, external(p))
            .map_err(engine)
    }
    fn validate_checkpoint(&self, path: &Path, p: Position) -> Result<()> {
        let (p, metadata) = self
            .application
            .checkpoint_metadata(path, external(p), &self.identity)
            .map_err(engine)?;
        Index::from_metadata(metadata, p, 0, true).map(|_| ())
    }
    fn publish_checkpoint(&self, path: &Path, p: Position) -> Result<()> {
        self.validate_checkpoint(path, p)?;
        self.application
            .publish_checkpoint(path, external(p))
            .map_err(engine)
    }
}

#[test]
fn prepared_intent_survives_restart_without_becoming_a_committed_row() {
    let directory = tempfile::tempdir().unwrap();
    let transaction = Transaction {
        version: journal::VERSION,
        id: TransactionId {
            coordinator: 0,
            worker: 0,
            incarnation: 19,
            attempt: 1,
        },
        time_term: 400,
        durable: true,
        members: vec![Member {
            owner: 0,
            endpoints: vec!["shard-0".into()],
            lease: 20,
        }],
    };
    let write = row(1, b"key", Some(b"value"), 400);
    let begin = Record::Begin(transaction.clone());
    let prepare = Record::Prepare {
        receipt: None,
        transaction: transaction.clone(),
        rows: vec![write.clone()],
    };
    let decide = Record::Decide {
        transaction: transaction.clone(),
        outcome: Outcome::Commit,
    };
    let complete = Record::Complete {
        transaction: transaction.clone(),
        outcome: Outcome::Commit,
    };
    {
        let db = Fixture::open(directory.path(), 9, true, b"journal-schema").unwrap();
        db.advance(position(1)).unwrap();
        db.apply_journal(position(2), b"begin", 0, &begin).unwrap();
        let (rows, entry) = db
            .apply_journal(position(3), b"prepare", 0, &prepare)
            .unwrap();
        assert!(rows.is_empty() && all(&db).is_empty() && entry.unresolved());
        assert_eq!(entry.participant.unwrap().rows, Some(vec![write.clone()]));
    }
    {
        let db = Fixture::open(directory.path(), 9, true, b"journal-schema").unwrap();
        assert!(all(&db).is_empty());
        let entries = db.transactions().unwrap();
        assert_eq!(entries.len(), 1);
        assert!(entries[0].unresolved());
        assert_eq!(entries[0].decision, None);
        db.apply_journal(position(4), b"decision", 0, &decide)
            .unwrap();
        assert!(
            all(&db).is_empty(),
            "decision alone must not complete a participant"
        );
    }
    {
        let db = Fixture::open(directory.path(), 9, true, b"journal-schema").unwrap();
        assert_eq!(
            db.transaction(transaction.id).unwrap().unwrap().decision,
            Some(Outcome::Commit)
        );
        let (rows, entry) = db
            .apply_journal(position(5), b"complete", 0, &complete)
            .unwrap();
        assert_eq!(rows, vec![write.clone()]);
        assert_eq!(all(&db), vec![write.clone()]);
        assert!(!entry.unresolved());
        assert_eq!(entry.participant.unwrap().outcome, Some(Outcome::Commit));
        let abort = Record::Decide {
            transaction: transaction.clone(),
            outcome: Outcome::Abort,
        };
        let (rows, retained) = db
            .apply_journal(position(6), b"late abort", 0, &abort)
            .unwrap();
        assert!(rows.is_empty());
        assert_eq!(retained.decision, Some(Outcome::Commit));
        assert!(journal::acknowledge(&abort, &retained).is_err());
        assert_eq!(db.position().unwrap(), position(6));
        let mut malformed = transaction.clone();
        malformed.version += 1;
        assert_eq!(
            db.apply_journal(position(7), b"malformed", 0, &Record::Begin(malformed))
                .unwrap_err()
                .code,
            crate::wire::ErrorCode::Invalid
        );
        assert_eq!(
            db.position().unwrap(),
            position(6),
            "malformed input must not consume a position"
        );
        let mut reused = transaction.clone();
        reused.members[0].lease += 1;
        let reused = Record::Begin(reused);
        let (_, retained) = db
            .apply_journal(position(7), b"reused identity", 0, &reused)
            .unwrap();
        assert_eq!(retained.transaction, transaction);
        assert!(journal::acknowledge(&reused, &retained).is_err());
    }
    let db = Fixture::open(directory.path(), 9, true, b"journal-schema").unwrap();
    assert_eq!(all(&db), vec![write]);
    let (rows, entry) = db
        .apply_journal(position(5), b"complete", 0, &complete)
        .unwrap();
    assert!(rows.is_empty());
    assert_eq!(entry.decision, Some(Outcome::Commit));
    assert!(db
        .apply_journal(position(5), b"changed command", 0, &complete)
        .is_err());
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
fn all(db: &Fixture) -> Vec<Row> {
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
    let db = Fixture::open(directory.path(), 9, true, b"schema-a").unwrap();
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
    let follower = Fixture::open(replica.path(), 9, true, b"schema-a").unwrap();
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
    assert!(Fixture::open(directory.path(), 9, true, b"different schema").is_err());
    let recovered = Fixture::open(directory.path(), 9, true, b"schema-a").unwrap();
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
