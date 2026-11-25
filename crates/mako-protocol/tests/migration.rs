//! Same production actors and transfer loop, over ordered in-memory cells.
use mako_sharding::{
    directory::RouteTable,
    migration::Coordinator,
    participant::Participant,
    storage::Source,
    transfer::{self, RawStore},
    types::*,
};
use std::{cell::Cell, collections::BTreeMap};

type Key = (u64, Vec<u8>, Vec<u8>);
#[derive(Default)]
struct Rows {
    cells: BTreeMap<Key, Vec<u8>>,
    writes: usize,
    fail: Cell<bool>,
}
impl Rows {
    fn seed(&mut self, coordinate: &[u8], key: &[u8], value: &[u8]) {
        self.cells
            .insert((31, coordinate.to_vec(), key.to_vec()), value.to_vec());
    }
}
impl RawStore for Rows {
    type Context = ();
    fn scan(&self, range: &KeyRange, after: Option<&Row>) -> Result<Option<Row>, Status> {
        Ok(self
            .cells
            .iter()
            .find(|((table, c, k), _)| {
                *table == range.table
                    && c >= &range.lo
                    && range.hi.as_ref().is_none_or(|hi| c < hi)
                    && after.is_none_or(|a| (c, k) > (&a.coordinate, &a.key))
            })
            .map(|((_, c, k), v)| Row {
                coordinate: c.clone(),
                key: k.clone(),
                value: v.clone(),
            }))
    }
    fn put(&mut self, range: &KeyRange, row: &Row) -> Status {
        if self.fail.replace(false) {
            return Status::Retry;
        }
        self.cells.insert(
            (range.table, row.coordinate.clone(), row.key.clone()),
            row.value.clone(),
        );
        self.writes += 1;
        Status::Ok
    }
    fn delete(&mut self, range: &KeyRange, row: &Row) -> Status {
        if self.fail.replace(false) {
            return Status::Retry;
        }
        self.cells
            .remove(&(range.table, row.coordinate.clone(), row.key.clone()));
        self.writes += 1;
        Status::Ok
    }
}
struct Frozen<'a>(&'a Participant, &'a Rows);
impl Source for Frozen<'_> {
    fn scan(&self, plan: &MigrationPlan, after: Option<&Row>) -> Result<Option<Row>, Status> {
        transfer::capture_frozen(self.0, self.1, plan, after)
    }
}
fn grant() -> Grant {
    Grant {
        owner: 0,
        ownership_version: 0,
    }
}
fn tables() -> Vec<RouteTable> {
    vec![RouteTable {
        table: 31,
        boundaries: vec![Boundary {
            start: vec![],
            grant: grant(),
        }],
    }]
}
fn plan() -> MigrationPlan {
    MigrationPlan {
        generation: 1,
        nonce: TxnId {
            client: 9,
            sequence: 1,
        },
        source: 0,
        destination: 1,
        range: KeyRange {
            table: 31,
            lo: b"b".to_vec(),
            hi: Some(b"f".to_vec()),
        },
        old: tables().remove(0).boundaries,
    }
}

#[test]
fn handoff_waits_for_drain_and_exact_copy_then_publishes_and_cleans_source() {
    let p = plan();
    let mut c = Coordinator::new(vec![0, 1], tables()).unwrap();
    assert_eq!(c.begin(p.nonce, 0, 1, plan().range).unwrap(), 1);
    let mut source = Participant::new(0, tables()).unwrap();
    let mut destination = Participant::new(1, tables()).unwrap();
    let txn = TxnId {
        client: 20,
        sequence: 1,
    };
    assert_eq!(source.begin(txn), Status::Ok);
    assert_eq!(source.acquire(txn, 31, b"c", grant()), Status::Ok);
    let mut src = Rows::default();
    let mut dst = Rows::default();
    src.seed(b"b", b"equal", b"same\0\xff");
    src.seed(b"c", b"updated", b"before");
    src.seed(b"d", b"deleted", b"before");
    src.seed(b"a", b"outside", b"keep");
    dst.seed(b"f", b"outside", b"keep");
    assert_eq!(destination.deliver(&p, Command::Start).status, Status::Ok);
    let mut after = None;
    while let Some(row) = src.scan(&p.range, after.as_ref()).unwrap() {
        assert_eq!(
            transfer::precopy_put(&destination, &mut dst, &p, &row),
            Status::Ok
        );
        after = Some(row);
    }
    src.seed(b"c", b"updated", b"after\0\xfe");
    src.seed(b"e", b"new", b"inserted");
    src.cells.remove(&(31, b"d".to_vec(), b"deleted".to_vec()));
    assert_eq!(c.request_freeze(), Status::Ok);
    assert_eq!(source.deliver(&p, Command::Freeze).status, Status::Ok);
    assert!(source.drain(&p).certificate.is_none());
    assert_eq!(c.request_final(), Status::Retry);
    assert_eq!(source.resolve(txn), Status::Ok);
    assert_eq!(source.finish(txn), Status::Ok);
    let drained = source.drain(&p);
    assert!(drained.certificate == Some(Certificate::Drained));
    assert_eq!(c.receive(1, 1, Certificate::Drained), Status::Invalid);
    assert_eq!(c.receive(1, 0, drained.certificate.unwrap()), Status::Ok);
    assert_eq!(c.request_final(), Status::Ok);
    assert_eq!(destination.deliver(&p, Command::Final).status, Status::Ok);
    dst.fail.set(true);
    let failed = transfer::execute_final(&mut destination, &mut dst, &p, &Frozen(&source, &src));
    assert_eq!(failed.status, Status::Retry);
    assert!(failed.certificate.is_none());
    assert_eq!(c.request_retire(), Status::Retry);
    assert_eq!(c.published_version(), 0);
    let ready = transfer::execute_final(&mut destination, &mut dst, &p, &Frozen(&source, &src));
    assert!(ready.certificate == Some(Certificate::Ready));
    assert_eq!(
        dst.writes, 6,
        "three advisory writes, update, insertion and deletion"
    );
    assert_eq!(
        dst.cells[&(31, b"c".to_vec(), b"updated".to_vec())],
        b"after\0\xfe"
    );
    assert!(!dst
        .cells
        .contains_key(&(31, b"d".to_vec(), b"deleted".to_vec())));
    assert_eq!(
        dst.cells[&(31, b"f".to_vec(), b"outside".to_vec())],
        b"keep"
    );
    assert_eq!(c.receive(1, 1, ready.certificate.unwrap()), Status::Ok);
    assert_eq!(c.request_retire(), Status::Ok);
    let retired = source.deliver(&p, Command::Retire);
    assert!(retired.certificate == Some(Certificate::Retired));
    assert_eq!(c.receive(1, 0, retired.certificate.unwrap()), Status::Ok);
    assert_eq!(c.commit(), Status::Ok);
    assert_eq!(c.lookup(31, b"c").unwrap().owner, 1);
    assert_eq!(c.lookup(31, b"a").unwrap().owner, 0);
    assert_eq!(c.published_version(), 1);
    assert_eq!(source.deliver(&p, Command::Commit).status, Status::Ok);
    let done = transfer::execute_cleanup(&mut source, &mut src, &p);
    assert!(done.certificate == Some(Certificate::SourceDone));
    let received = destination.deliver(&p, Command::Commit);
    assert!(received.certificate == Some(Certificate::DestinationDone));
    assert_eq!(c.receive(1, 0, done.certificate.unwrap()), Status::Ok);
    assert_eq!(c.receive(1, 1, received.certificate.unwrap()), Status::Ok);
    assert_eq!(c.finish(), Status::Ok);
    assert_eq!(src.cells.len(), 1);
    assert_eq!(
        src.cells[&(31, b"a".to_vec(), b"outside".to_vec())],
        b"keep"
    );
    assert!(c.reply(p.nonce).unwrap().committed);
    assert_eq!(c.begin(p.nonce, 0, 1, plan().range), Err(Status::Invalid));
    assert!(
        transfer::precopy_put(
            &destination,
            &mut dst,
            &p,
            &Row {
                coordinate: b"c".to_vec(),
                key: b"late".to_vec(),
                value: b"bad".to_vec()
            }
        ) != Status::Ok
    );
}

#[test]
fn aborted_copy_cleans_destination_and_cannot_resurrect_on_delayed_start() {
    let p = plan();
    let mut dst = Rows::default();
    let mut destination = Participant::new(1, tables()).unwrap();
    dst.seed(b"a", b"outside", b"keep");
    assert_eq!(destination.deliver(&p, Command::Start).status, Status::Ok);
    let row = Row {
        coordinate: b"c".to_vec(),
        key: b"key".to_vec(),
        value: b"tentative".to_vec(),
    };
    assert_eq!(
        transfer::precopy_put(&destination, &mut dst, &p, &row),
        Status::Ok
    );
    assert_eq!(destination.deliver(&p, Command::Abort).status, Status::Ok);
    let completed = transfer::execute_cleanup(&mut destination, &mut dst, &p);
    assert!(completed.certificate == Some(Certificate::DestinationDone));
    destination.deliver(&p, Command::Start);
    assert!(transfer::precopy_put(&destination, &mut dst, &p, &row) != Status::Ok);
    assert_eq!(dst.cells.len(), 1);
    assert_eq!(
        dst.cells[&(31, b"a".to_vec(), b"outside".to_vec())],
        b"keep"
    );
}
