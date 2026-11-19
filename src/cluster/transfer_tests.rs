//! Concrete byte/version regressions over the existing file-backed engine.
//! Fixture ownership excludes concurrent engine access; it does not simulate or
//! certify a production STO cut. Native callers must still complete that cut.
use super::{capture, capture_frozen, execute_final, precopy_put, RawStore};
use crate::directory::RouteTable;
use crate::durable_engine::DurableEngine;
use crate::durable_kv::{Admission, CommitResult, Namespace};
use crate::durable_recovery_tests::engine::DiskEngine;
use crate::durable_recovery_tests::store::{Directory, Fault};
use crate::participant::Participant;
use crate::storage::Source;
use crate::types::*;

const TABLE: u64 = 31;

fn grant() -> Grant { Grant { owner: 0, ownership_version: 0 } }
fn tables() -> Vec<RouteTable> {
    vec![RouteTable { table: TABLE, boundaries: vec![Boundary { start: vec![], grant: grant() }] }]
}
fn plan(generation: u64) -> MigrationPlan {
    MigrationPlan { generation, nonce: TxnId { client: 9, sequence: generation }, source: 0, destination: 1,
        range: KeyRange { table: TABLE, lo: b"b".to_vec(), hi: Some(b"f".to_vec()) },
        old: vec![Boundary { start: vec![], grant: grant() }] }
}
fn row(coordinate: &[u8], key: &[u8], value: &[u8]) -> Row {
    Row { coordinate: coordinate.to_vec(), key: key.to_vec(), value: value.to_vec() }
}

/// Only adapts physical primitives; it has no metadata oracle or expected-result
/// echo. All bytes, absence and versions come from DiskEngine's real files.
struct Rows {
    engine: DiskEngine,
    admission: Admission,
    directory: Directory,
}
impl Rows {
    fn new(owner: u32) -> Self {
        let directory = Directory::new();
        let mut engine = DiskEngine::open(directory.path(), owner);
        let admission = Admission { namespace: Namespace { owner, coordinator: false }, incarnation: 1 };
        assert_eq!(engine.recover_rows(), Status::Ok);
        assert_eq!(engine.install(admission), Status::Ok);
        Self { engine, admission, directory }
    }
    fn seed(&mut self, coordinate: &[u8], key: &[u8], value: &[u8]) {
        self.engine.seed(TABLE, row(coordinate, key, value));
    }
    fn value(&self, coordinate: &[u8], key: &[u8]) -> Option<&[u8]> {
        self.engine.value(TABLE, coordinate, key)
    }
    fn version(&self, coordinate: &[u8], key: &[u8]) -> u64 {
        self.engine.physical_version(TABLE, coordinate, key)
    }
    fn reopen(&self) -> DiskEngine { DiskEngine::open(self.directory.path(), self.admission.namespace.owner) }
}
fn status(result: CommitResult) -> Status {
    match result { CommitResult::Committed => Status::Ok, CommitResult::NotCommitted => Status::Retry,
        CommitResult::Unknown => Status::Io }
}
impl RawStore for Rows {
    type Context = ();
    fn scan(&self, range: &KeyRange, after: Option<&Row>) -> Result<Option<Row>, Status> {
        self.engine.scan(&self.admission, range, after)
    }
    fn put(&mut self, range: &KeyRange, row: &Row) -> Status {
        status(self.engine.mutate(&self.admission, range, row, false))
    }
    fn delete(&mut self, range: &KeyRange, row: &Row) -> Status {
        status(self.engine.mutate(&self.admission, range, row, true))
    }
}
struct Frozen<'a> { participant: &'a Participant, rows: &'a Rows }
impl Source for Frozen<'_> {
    fn scan(&self, plan: &MigrationPlan, after: Option<&Row>) -> Result<Option<Row>, Status> {
        capture_frozen(self.participant, self.rows, plan, after)
    }
}

#[test]
fn final_reconciles_updates_inserts_deletes_and_extras_without_rewriting_equal_rows() {
    let migration = plan(1);
    let mut source = Participant::new(0, tables()).unwrap();
    let mut destination = Participant::new(1, tables()).unwrap();
    let mut src = Rows::new(0);
    let mut dst = Rows::new(1);
    src.seed(b"b", b"equal", b"same\0\xff");
    src.seed(b"c", b"updated", b"before");
    src.seed(b"d", b"deleted", b"before");
    dst.seed(b"a", b"outside", b"low");
    dst.seed(b"f", b"outside", b"high");
    dst.seed(b"e", b"orphan", b"inserted");
    dst.engine.seed(TABLE + 1, row(b"c", b"other-table", b"untouched"));
    assert_eq!(destination.deliver(&migration, Command::Start).status, Status::Ok);
    let mut after = None;
    while let Some(captured) = src.scan(&migration.range, after.as_ref()).unwrap() {
        assert_eq!(precopy_put(&destination, &mut dst, &migration, &captured), Status::Ok);
        after = Some(captured);
    }
    assert_eq!(dst.engine.mutations, 3);
    let equal_version = dst.version(b"b", b"equal");
    // Source changes after the advisory pass, including a new raw key at the
    // same coordinate as an equal-valued destination-only row.
    src.seed(b"c", b"updated", b"after\0\xfe");
    src.seed(b"e", b"fresh", b"inserted");
    assert_eq!(src.delete(&migration.range, &row(b"d", b"deleted", b"")), Status::Ok);
    assert_eq!(source.deliver(&migration, Command::Freeze).status, Status::Ok);
    assert_eq!(destination.deliver(&migration, Command::Final).status, Status::Ok);
    let result = execute_final(&mut destination, &mut dst, &migration, &Frozen { participant: &source, rows: &src });
    assert_eq!(result.status, Status::Ok);
    assert!(result.certificate == Some(Certificate::Ready));
    assert_eq!(dst.engine.mutations, 7, "only update, insert, and two deletes are needed after precopy");
    assert_eq!(dst.version(b"b", b"equal"), equal_version);
    assert_eq!(dst.value(b"b", b"equal"), Some(&b"same\0\xff"[..]));
    assert_eq!(dst.value(b"c", b"updated"), Some(&b"after\0\xfe"[..]));
    assert_eq!(dst.value(b"d", b"deleted"), None);
    assert_eq!(dst.value(b"e", b"fresh"), Some(&b"inserted"[..]));
    assert_eq!(dst.value(b"e", b"orphan"), None);
    assert_eq!(dst.value(b"a", b"outside"), Some(&b"low"[..]));
    assert_eq!(dst.value(b"f", b"outside"), Some(&b"high"[..]));
    assert_eq!(dst.version(b"a", b"outside"), 1);
    assert_eq!(dst.version(b"f", b"outside"), 1);
    assert_eq!(dst.engine.value(TABLE + 1, b"c", b"other-table"), Some(&b"untouched"[..]));
    assert_eq!(dst.engine.physical_version(TABLE + 1, b"c", b"other-table"), 1);
    let reopened = dst.reopen();
    assert_eq!(reopened.value(TABLE, b"c", b"updated"), Some(&b"after\0\xfe"[..]));
    assert_eq!(reopened.value(TABLE, b"d", b"deleted"), None);
    assert_eq!(reopened.value(TABLE, b"e", b"orphan"), None);
    assert_eq!(reopened.physical_version(TABLE, b"b", b"equal"), equal_version);
}

#[test]
fn equal_rows_do_not_consume_a_physical_fault_or_hide_a_changed_row_failure() {
    let migration = plan(1);
    let mut source = Participant::new(0, tables()).unwrap();
    let mut destination = Participant::new(1, tables()).unwrap();
    let mut src = Rows::new(0);
    let mut dst = Rows::new(1);
    src.seed(b"b", b"equal", b"same");
    dst.seed(b"b", b"equal", b"same");
    src.seed(b"c", b"changed", b"new");
    dst.seed(b"c", b"changed", b"old");
    assert_eq!(source.deliver(&migration, Command::Freeze).status, Status::Ok);
    assert_eq!(destination.deliver(&migration, Command::Start).status, Status::Ok);
    assert_eq!(destination.deliver(&migration, Command::Final).status, Status::Ok);
    dst.engine.row_fault = Fault::NotCommitted;
    let frozen = Frozen { participant: &source, rows: &src };
    let failed = execute_final(&mut destination, &mut dst, &migration, &frozen);
    assert_eq!(failed.status, Status::Retry);
    assert!(failed.certificate.is_none());
    assert_eq!(dst.engine.mutations, 1);
    assert_eq!(dst.version(b"b", b"equal"), 1);
    assert_eq!(dst.version(b"c", b"changed"), 1);
    assert_eq!(dst.value(b"c", b"changed"), Some(&b"old"[..]));
    let completed = execute_final(&mut destination, &mut dst, &migration, &frozen);
    assert_eq!(completed.status, Status::Ok);
    assert!(completed.certificate == Some(Certificate::Ready));
    assert_eq!(dst.engine.mutations, 2, "retry must still skip the equal row");
    assert_eq!(dst.version(b"b", b"equal"), 1);
    assert_eq!(dst.version(b"c", b"changed"), 2);
    assert_eq!(dst.value(b"c", b"changed"), Some(&b"new"[..]));
}

#[test]
fn all_equal_final_completes_without_entering_faulting_physical_mutation() {
    let migration = plan(1);
    let mut source = Participant::new(0, tables()).unwrap();
    let mut destination = Participant::new(1, tables()).unwrap();
    let mut src = Rows::new(0);
    let mut dst = Rows::new(1);
    src.seed(b"b", b"same", b"");
    dst.seed(b"b", b"same", b"");
    assert_eq!(source.deliver(&migration, Command::Freeze).status, Status::Ok);
    assert_eq!(destination.deliver(&migration, Command::Start).status, Status::Ok);
    assert_eq!(destination.deliver(&migration, Command::Final).status, Status::Ok);
    dst.engine.row_fault = Fault::NotCommitted;
    let result = execute_final(&mut destination, &mut dst, &migration, &Frozen { participant: &source, rows: &src });
    assert_eq!(result.status, Status::Ok);
    assert!(result.certificate == Some(Certificate::Ready));
    assert_eq!(dst.engine.mutations, 0);
    assert_eq!(dst.version(b"b", b"same"), 1);
    assert!(matches!(dst.engine.row_fault, Fault::NotCommitted));
}

#[test]
fn precopy_checks_entire_envelope_range_and_row_before_touching_storage() {
    let migration = plan(1);
    let mut destination = Participant::new(1, tables()).unwrap();
    let mut dst = Rows::new(1);
    let captured = row(b"c", b"key", b"captured");
    assert_eq!(precopy_put(&destination, &mut dst, &migration, &captured), Status::Retry);
    assert_eq!(destination.deliver(&migration, Command::Start).status, Status::Ok);
    let before = destination.meta(TABLE, b"c");
    let mut malformed = plan(1);
    malformed.old.clear();
    assert_eq!(precopy_put(&destination, &mut dst, &malformed, &captured), Status::Retry);
    let mut extended = plan(1);
    extended.range.hi = Some(b"g".to_vec());
    assert_eq!(precopy_put(&destination, &mut dst, &extended, &captured), Status::Retry);
    let mut foreign = plan(1);
    foreign.destination = 2;
    assert_eq!(precopy_put(&destination, &mut dst, &foreign, &captured), Status::Retry);
    assert_eq!(precopy_put(&destination, &mut dst, &plan(2), &captured), Status::Retry);
    assert_eq!(precopy_put(&destination, &mut dst, &migration, &row(b"f", b"key", b"outside")), Status::Invalid);
    assert_eq!(dst.engine.mutations, 0);
    assert!(destination.meta(TABLE, b"c") == before);
    assert_eq!(precopy_put(&destination, &mut dst, &migration, &captured), Status::Ok);
    assert_eq!(dst.engine.mutations, 1);
    assert!(destination.meta(TABLE, b"c") == before);
    let metadata = destination.meta(TABLE, b"c").unwrap();
    assert!(metadata.role == Role::Stage && metadata.round == 0 && !metadata.terminal);
}

#[test]
fn old_precopy_is_fenced_after_final_abort_and_a_new_generation() {
    for command in [Command::Final, Command::Abort] {
        let migration = plan(1);
        let mut destination = Participant::new(1, tables()).unwrap();
        let mut dst = Rows::new(1);
        assert_eq!(destination.deliver(&migration, Command::Start).status, Status::Ok);
        assert_eq!(precopy_put(&destination, &mut dst, &migration, &row(b"c", b"key", b"first")), Status::Ok);
        assert_eq!(destination.deliver(&migration, command).status, Status::Ok);
        let terminal_or_final = destination.meta(TABLE, b"c");
        assert_eq!(precopy_put(&destination, &mut dst, &migration, &row(b"c", b"key", b"late")), Status::Retry);
        assert_eq!(dst.engine.mutations, 1);
        assert_eq!(dst.value(b"c", b"key"), Some(&b"first"[..]));
        assert!(destination.meta(TABLE, b"c") == terminal_or_final);
        if command == Command::Abort {
            let next = plan(2);
            assert_eq!(destination.deliver(&next, Command::Start).status, Status::Ok);
            assert_eq!(precopy_put(&destination, &mut dst, &next, &row(b"c", b"key", b"next")), Status::Ok);
            assert_eq!(precopy_put(&destination, &mut dst, &migration, &row(b"c", b"key", b"obsolete")), Status::Retry);
            assert_eq!(dst.engine.mutations, 2);
            assert_eq!(dst.value(b"c", b"key"), Some(&b"next"[..]));
        }
    }
}

#[test]
fn native_frozen_capture_checks_metadata_without_claiming_a_lease_drain() {
    let migration = plan(1);
    let mut source = Participant::new(0, tables()).unwrap();
    let mut rows = Rows::new(0);
    rows.seed(b"c", b"key", b"frozen");
    // Even an empty conditional lease book is not authority to scan Serving.
    assert!(matches!(capture_frozen(&source, &rows, &migration, None), Err(Status::Retry)));
    let transaction = TxnId { client: 3, sequence: 1 };
    assert_eq!(source.begin(transaction), Status::Ok);
    assert_eq!(source.acquire(transaction, TABLE, b"c", grant()), Status::Ok);
    assert_eq!(source.deliver(&migration, Command::Freeze).status, Status::Ok);
    // This fixture is exclusively owned and has no concurrent engine work. Its
    // deliberately retained *conditional* lease still blocks the durable API.
    assert!(matches!(capture(&source, &rows, &migration, None), Err(Status::Retry)));
    let captured = capture_frozen(&source, &rows, &migration, None).unwrap().unwrap();
    assert_eq!(captured.coordinate, b"c");
    assert_eq!(captured.key, b"key");
    assert_eq!(captured.value, b"frozen");
    assert!(capture_frozen(&source, &rows, &migration, Some(&captured)).unwrap().is_none());
    let mut extended = plan(1);
    extended.range.hi = Some(b"g".to_vec());
    assert!(matches!(capture_frozen(&source, &rows, &extended, None), Err(Status::Retry)));
    assert!(matches!(capture_frozen(&source, &rows, &plan(2), None), Err(Status::Retry)));
    assert_eq!(source.deliver(&migration, Command::Abort).status, Status::Ok);
    assert!(matches!(capture_frozen(&source, &rows, &migration, None), Err(Status::Retry)));
}
