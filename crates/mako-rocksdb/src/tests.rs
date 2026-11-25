use super::*;
use backend::{Backend, Session};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

type Rows = BTreeMap<(u64, Vec<u8>), Vec<u8>>;
#[derive(Default)]
struct StateStore {
    rows: Rows,
    version: u64,
    outcomes: BTreeMap<TransactionId, Outcome>,
    calls: Vec<&'static str>,
}
#[derive(Clone, Default)]
struct Model {
    state: Arc<Mutex<StateStore>>,
    sequence: Arc<AtomicU64>,
    unknown_commit: Arc<AtomicBool>,
    unknown_abort: Arc<AtomicBool>,
    fail_scan: Arc<AtomicBool>,
}
impl Backend for Model {
    fn column_families(&self) -> Vec<(String, u64)> {
        vec![("default".into(), 1), ("other".into(), 2)]
    }
    fn validate_write(&self, _: &WriteOptions) -> Result<()> {
        Ok(())
    }
    fn begin(&self) -> Result<Box<dyn Session>> {
        let mut state = self.state.lock().unwrap();
        state.calls.push("begin");
        Ok(Box::new(Attempt {
            model: self.clone(),
            id: TransactionId {
                coordinator: 0,
                worker: 0,
                incarnation: 1,
                attempt: self.sequence.fetch_add(1, Ordering::Relaxed) + 1,
            },
            rows: state.rows.clone(),
            version: state.version,
            changed: false,
        }))
    }
    fn resolve(&self, id: TransactionId) -> Result<Outcome> {
        self.state
            .lock()
            .unwrap()
            .outcomes
            .get(&id)
            .copied()
            .ok_or_else(|| {
                Error::new(ErrorKind::UnknownCommit, "missing decision").with_transaction(id)
            })
    }
}
struct Attempt {
    model: Model,
    id: TransactionId,
    rows: Rows,
    version: u64,
    changed: bool,
}
impl Session for Attempt {
    fn id(&self) -> TransactionId {
        self.id
    }
    fn get(&mut self, table: u64, key: &[u8]) -> Result<Option<Vec<u8>>> {
        Ok(self.rows.get(&(table, key.to_vec())).cloned())
    }
    fn put(&mut self, table: u64, key: &[u8], value: &[u8]) -> Result<()> {
        self.rows.insert((table, key.to_vec()), value.to_vec());
        self.changed = true;
        Ok(())
    }
    fn delete(&mut self, table: u64, key: &[u8]) -> Result<()> {
        self.rows.remove(&(table, key.to_vec()));
        self.changed = true;
        Ok(())
    }
    fn scan(
        &mut self,
        table: u64,
        lower: Bound,
        upper: Bound,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        if self.model.fail_scan.load(Ordering::Relaxed) {
            return Err(Error::new(ErrorKind::TryAgain, "route changed"));
        }
        let rows: Vec<_> = self
            .rows
            .iter()
            .filter(|((t, k), _)| {
                *t == table
                    && match &lower {
                        Bound::Unbounded => true,
                        Bound::Included(v) => k >= v,
                        Bound::Excluded(v) => k > v,
                    }
                    && match &upper {
                        Bound::Unbounded => true,
                        Bound::Included(v) => k <= v,
                        Bound::Excluded(v) => k < v,
                    }
            })
            .map(|((_, k), v)| Row {
                key: k.clone(),
                value: v.clone(),
                time_term: 0,
            })
            .collect();
        Ok(if reverse {
            rows.into_iter().rev().take(limit).collect()
        } else {
            rows.into_iter().take(limit).collect()
        })
    }
    fn commit(self: Box<Self>) -> Result<()> {
        let mut state = self.model.state.lock().unwrap();
        state.calls.push("commit");
        if state.version != self.version {
            return Err(Error::new(ErrorKind::Busy, "model conflict"));
        }
        if self.changed {
            state.version += 1;
            state.rows = self.rows;
        }
        state.outcomes.insert(self.id, Outcome::Commit);
        if self.model.unknown_commit.swap(false, Ordering::Relaxed) {
            return Err(Error::new(ErrorKind::UnknownCommit, "lost acknowledgement")
                .with_transaction(self.id));
        }
        Ok(())
    }
    fn rollback(self: Box<Self>) -> Result<()> {
        let mut state = self.model.state.lock().unwrap();
        state.calls.push("abort");
        state.outcomes.insert(self.id, Outcome::Abort);
        if self.model.unknown_abort.swap(false, Ordering::Relaxed) {
            return Err(
                Error::new(ErrorKind::UnknownCommit, "abort acknowledgement lost")
                    .with_transaction(self.id),
            );
        }
        Ok(())
    }
}
fn fixture() -> (Model, DB) {
    let model = Model::default();
    let db = DB::from_backend(Arc::new(model.clone())).unwrap();
    (model, db)
}
fn keys(iter: DBIterator<'_>) -> Vec<Vec<u8>> {
    iter.map(|r| r.unwrap().0.into_vec()).collect()
}

#[test]
fn crud_is_atomic_and_binary_with_read_your_writes() {
    let (_, db) = fixture();
    let tx = db.transaction();
    tx.put(b"\0\xff", b"\0value").unwrap();
    tx.put(b"", b"").unwrap();
    assert_eq!(tx.get(b"\0\xff").unwrap(), Some(b"\0value".to_vec()));
    assert_eq!(db.get(b"\0\xff").unwrap(), None);
    tx.commit().unwrap();
    assert_eq!(db.get(b"").unwrap(), Some(Vec::new()));
    db.delete(b"missing").unwrap();
    db.delete(b"\0\xff").unwrap();
    assert_eq!(db.get(b"\0\xff").unwrap(), None);
}
#[test]
fn rollback_drop_and_reuse_do_not_publish() {
    let (model, db) = fixture();
    {
        let tx = db.transaction();
        tx.put(b"drop", b"x").unwrap();
    }
    let tx = db.transaction();
    tx.put(b"rollback", b"x").unwrap();
    let first = tx.transaction_id();
    tx.rollback().unwrap();
    tx.put(b"committed", b"yes").unwrap();
    assert_ne!(tx.transaction_id(), first);
    tx.commit().unwrap();
    assert_eq!(model.state.lock().unwrap().rows.len(), 1);
    assert_eq!(
        model.state.lock().unwrap().calls,
        ["begin", "abort", "begin", "abort", "begin", "commit"]
    );
}
#[test]
fn ignored_operation_error_poison_and_rollback_recovery() {
    let (_, db) = fixture();
    let tx = db.transaction();
    tx.put(b"before", b"x").unwrap();
    assert!(tx.put(vec![0; MAX_KEY + 1], b"x").is_err());
    assert!(tx.put(b"after", b"x").is_err());
    assert!(tx.commit().is_err());
    assert_eq!(db.get(b"before").unwrap(), None);
    let tx = db.transaction();
    assert!(tx.put(b"big", vec![0; MAX_VALUE + 1]).is_err());
    tx.rollback().unwrap();
    tx.put(b"valid", b"yes").unwrap();
    tx.commit().unwrap();
}
#[test]
fn column_family_handles_cannot_cross_databases_or_batches() {
    let (model, db) = fixture();
    let (_, foreign) = fixture();
    let cf = db.cf_handle("other").unwrap();
    db.put_cf(cf, b"key", b"other").unwrap();
    assert_eq!(db.get(b"key").unwrap(), None);
    assert_eq!(db.get_cf(cf, b"key").unwrap(), Some(b"other".to_vec()));
    let mut batch = WriteBatch::default();
    batch.put(b"first", b"x");
    batch.put_cf(foreign.cf_handle("other").unwrap(), b"key", b"foreign");
    let before = model.sequence.load(Ordering::Relaxed);
    assert_eq!(
        db.write(batch).unwrap_err().kind(),
        ErrorKind::InvalidArgument
    );
    assert_eq!(model.sequence.load(Ordering::Relaxed), before);
}
#[test]
fn batch_cross_family_order_and_atomic_failure() {
    let (_, db) = fixture();
    let cf = db.cf_handle("other").unwrap();
    let mut batch = WriteBatch::default();
    batch.put(b"key", b"first");
    batch.delete(b"key");
    batch.put(b"key", b"last");
    batch.put_cf(cf, b"key", b"other");
    assert_eq!(batch.len(), 4);
    db.write(batch).unwrap();
    assert_eq!(db.get(b"key").unwrap(), Some(b"last".to_vec()));
    assert_eq!(db.get_cf(cf, b"key").unwrap(), Some(b"other".to_vec()));
    let mut bad = WriteBatch::default();
    bad.put(b"key", b"uncommitted");
    bad.put(b"large", vec![0; MAX_VALUE + 1]);
    assert!(db.write(bad).is_err());
    assert_eq!(db.get(b"key").unwrap(), Some(b"last".to_vec()));
}
#[test]
fn unsupported_options_fail_before_backend_and_survive_rollback() {
    let (model, db) = fixture();
    let mut write = WriteOptions::default();
    write.disable_wal(true);
    assert_eq!(
        db.put_opt(b"k", b"v", &write).unwrap_err().kind(),
        ErrorKind::NotSupported
    );
    let mut options = OptimisticTransactionOptions::default();
    options.set_snapshot(true);
    let tx = db.transaction_opt(&WriteOptions::default(), &options);
    assert_eq!(tx.get(b"k").unwrap_err().kind(), ErrorKind::NotSupported);
    tx.rollback().unwrap();
    assert_eq!(tx.commit().unwrap_err().kind(), ErrorKind::NotSupported);
    assert_eq!(model.sequence.load(Ordering::Relaxed), 0);
}
#[test]
fn iterator_seek_reverse_and_intersected_binary_bounds() {
    let (_, db) = fixture();
    for k in [b"".as_slice(), b"a", b"a\0", b"a\xff", b"b", b"\xff"] {
        db.put(k, k).unwrap();
    }
    assert_eq!(
        keys(db.iterator(IteratorMode::From(b"a\0", Direction::Forward))),
        [
            b"a\0".to_vec(),
            b"a\xff".to_vec(),
            b"b".to_vec(),
            b"\xff".to_vec()
        ]
    );
    let mut read = ReadOptions::default();
    read.set_iterate_lower_bound(b"a".to_vec());
    read.set_iterate_upper_bound(b"b".to_vec());
    assert_eq!(
        keys(db.iterator_opt(IteratorMode::From(b"b", Direction::Reverse), read.clone())),
        [b"a\xff".to_vec(), b"a\0".to_vec(), b"a".to_vec()]
    );
    assert!(keys(db.iterator_opt(IteratorMode::From(b"c", Direction::Forward), read)).is_empty());
    assert_eq!(
        keys(db.iterator(IteratorMode::End)).last(),
        Some(&Vec::new())
    );
}
#[test]
fn iterator_failure_is_an_error_and_poison_not_empty_success() {
    let (model, db) = fixture();
    let tx = db.transaction();
    tx.put(b"k", b"v").unwrap();
    model.fail_scan.store(true, Ordering::Relaxed);
    let mut it = tx.iterator(IteratorMode::Start);
    assert_eq!(it.next().unwrap().unwrap_err().kind(), ErrorKind::TryAgain);
    assert!(it.next().is_none());
    drop(it);
    assert_eq!(tx.commit().unwrap_err().kind(), ErrorKind::TryAgain);
    assert!(model.state.lock().unwrap().rows.is_empty());
}
#[test]
fn iterator_limit_rejects_truncation_and_large_values() {
    let (model, db) = fixture();
    for n in 0..=MAX_ITERATOR_ROWS {
        model
            .state
            .lock()
            .unwrap()
            .rows
            .insert((1, n.to_be_bytes().to_vec()), vec![]);
    }
    assert_eq!(
        db.iterator(IteratorMode::Start)
            .next()
            .unwrap()
            .unwrap_err()
            .kind(),
        ErrorKind::Incomplete
    );
    model.state.lock().unwrap().rows.clear();
    for n in 0..65u32 {
        model
            .state
            .lock()
            .unwrap()
            .rows
            .insert((1, n.to_be_bytes().to_vec()), vec![0; MAX_VALUE]);
    }
    assert_eq!(
        db.iterator(IteratorMode::Start)
            .next()
            .unwrap()
            .unwrap_err()
            .kind(),
        ErrorKind::Incomplete
    );
}
#[test]
fn unknown_commit_resolves_without_reexecution_and_drop_retains_id() {
    let (model, db) = fixture();
    model.unknown_commit.store(true, Ordering::Relaxed);
    let tx = db.transaction();
    tx.put(b"k", b"once").unwrap();
    let id = tx.transaction_id().unwrap();
    let error = tx.commit().unwrap_err();
    assert_eq!(error.transaction_id(), Some(id));
    assert_eq!(db.pending_transactions(), [id]);
    assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
    assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
    assert!(db.pending_transactions().is_empty());
    assert_eq!(model.sequence.load(Ordering::Relaxed), 1);
    model.unknown_abort.store(true, Ordering::Relaxed);
    let tx = db.transaction();
    tx.put(b"drop", b"discard").unwrap();
    let id = tx.transaction_id().unwrap();
    drop(tx);
    assert_eq!(db.pending_transactions(), [id]);
    assert_eq!(db.resolve(id).unwrap(), Outcome::Abort);
    assert_eq!(model.state.lock().unwrap().rows.len(), 1);
}
#[test]
fn conflicts_are_exposed_without_hidden_retry() {
    let (model, db) = fixture();
    let tx = db.transaction();
    assert_eq!(tx.get_for_update(b"absent", true).unwrap(), None);
    db.put(b"absent", b"other").unwrap();
    tx.put(b"must-abort", b"x").unwrap();
    assert_eq!(tx.commit().unwrap_err().kind(), ErrorKind::Busy);
    assert_eq!(model.sequence.load(Ordering::Relaxed), 2);
    assert!(!model
        .state
        .lock()
        .unwrap()
        .rows
        .contains_key(&(1, b"must-abort".to_vec())));
}
#[test]
fn multiget_preserves_order_duplicates_and_missing() {
    let (_, db) = fixture();
    db.put(b"a", b"value").unwrap();
    assert_eq!(
        db.multi_get([b"a", b"b", b"a"]),
        [
            Ok(Some(b"value".to_vec())),
            Ok(None),
            Ok(Some(b"value".to_vec()))
        ]
    );
}
#[test]
fn transaction_and_database_send_contract() {
    fn send<T: Send>() {}
    fn sync<T: Sync>() {}
    send::<DB>();
    sync::<DB>();
    send::<Transaction<'_>>();
}

#[test]
fn uncertain_rollback_cannot_be_cleared_by_repeating_rollback() {
    let (model, db) = fixture();
    let tx = db.transaction();
    tx.put(b"key", b"discard").unwrap();
    let id = tx.transaction_id().unwrap();
    model.unknown_abort.store(true, Ordering::Relaxed);
    assert_eq!(tx.rollback().unwrap_err().kind(), ErrorKind::UnknownCommit);
    assert_eq!(tx.rollback().unwrap_err().kind(), ErrorKind::UnknownCommit);
    assert_eq!(
        tx.put(b"new", b"attempt").unwrap_err().kind(),
        ErrorKind::UnknownCommit
    );
    assert_eq!(db.resolve(id).unwrap(), Outcome::Abort);
    drop(tx);
    assert_eq!(model.sequence.load(Ordering::Relaxed), 1);
}
