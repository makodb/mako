//! File-backed reference engine for tests/examples, not the production TP layer.
//! Row bytes + versions and transaction effects + terminal receipts use atomic DiskKv batches.
//! This fixture assigns each transaction to one owner (client's low bit); it is
//! not a distributed TP implementation. Accepted work needs a durable decision.
//! An absent transaction can instead receive a durable empty-abort fence.
use super::native::durable_engine::DurableEngine;
use super::native::durable_kv::{Admission, CommitResult, DurableKv, WriteOp};
use super::native::routing_codec::{Reader, Writer};
use super::native::types::{KeyRange, Row, Status, TxnId};
use super::store::{DiskKv, Fault};
use std::collections::BTreeMap;
use std::path::Path;

type Cell = (u64, Vec<u8>, Vec<u8>);
type Rows = BTreeMap<Cell, Vec<u8>>;

fn key(kind: u32, cell: &Cell) -> Vec<u8> {
    let mut out = Writer::new();
    out.write_u32(kind);
    out.write_u64(cell.0);
    out.write_bytes(&cell.1);
    out.write_bytes(&cell.2);
    out.into_bytes()
}
fn cell_key(bytes: &[u8]) -> Option<(u32, Cell)> {
    let mut input = Reader::new(bytes);
    let kind = input.read_u32().ok()?;
    if kind != 1 && kind != 2 { return None; }
    let table = input.read_u64().ok()?;
    let coordinate = input.read_bytes().ok()?;
    let raw = input.read_bytes().ok()?;
    if input.finish() != Status::Ok { return None; }
    Some((kind, (table, coordinate, raw)))
}
fn txn_key(id: TxnId) -> Vec<u8> {
    let mut out = Writer::new();
    out.write_u32(3);
    out.write_u64(id.client);
    out.write_u64(id.sequence);
    out.into_bytes()
}
fn selected(range: &KeyRange, cell: &Cell) -> bool {
    cell.0 == range.table && cell.1 >= range.lo
        && range.hi.as_ref().is_none_or(|end| cell.1 < *end)
}

pub struct DiskEngine {
    owner: u32,
    kv: DiskKv,
    working: Rows,
    installed: Option<Admission>,
    recovered: bool,
    pub row_fault: Fault,
    pub barrier_unknown: bool,
    pub fail_recovery: bool,
    pub mutations: usize,
}
impl DiskEngine {
    pub fn open(path: &Path, owner: u32) -> Self {
        let kv = DiskKv::open(path);
        let working = Self::materialize(&kv);
        Self { owner, kv, working, installed: None, recovered: false,
            row_fault: Fault::None, barrier_unknown: false, fail_recovery: false, mutations: 0 }
    }
    fn materialize(kv: &DiskKv) -> Rows {
        kv.all_bytes().iter().filter_map(|(key, value)| {
            let (kind, cell) = cell_key(key)?;
            (kind == 1).then(|| (cell, value.clone()))
        }).collect()
    }
    fn version(&self, cell: &Cell) -> u64 {
        self.kv.bytes(&key(2, cell)).map(|bytes|
            u64::from_le_bytes(bytes.try_into().unwrap())).unwrap_or(0)
    }
    fn row_ops(&self, table: u64, row: &Row, delete: bool) -> Option<Vec<WriteOp>> {
        let cell = (table, row.coordinate.clone(), row.key.clone());
        let version = self.version(&cell).checked_add(1)?;
        Some(vec![
            WriteOp { key: key(1, &cell), value: if delete { None } else { Some(row.value.clone()) } },
            WriteOp { key: key(2, &cell), value: Some(version.to_le_bytes().to_vec()) },
        ])
    }
    pub fn seed(&mut self, table: u64, row: Row) {
        let writes = self.row_ops(table, &row, false).unwrap();
        assert!(matches!(self.kv.submit(writes), CommitResult::Committed));
        self.working = Self::materialize(&self.kv);
    }
    pub fn value(&self, table: u64, coordinate: &[u8], raw: &[u8]) -> Option<&[u8]> {
        self.working.get(&(table, coordinate.to_vec(), raw.to_vec())).map(Vec::as_slice)
    }
    pub fn physical_version(&self, table: u64, coordinate: &[u8], raw: &[u8]) -> u64 {
        self.version(&(table, coordinate.to_vec(), raw.to_vec()))
    }
    /// A durable accepted transaction, with an optional known TP decision.
    /// Versioned effects are fixed here, before the settlement API is called.
    pub fn transaction(&mut self, id: TxnId, commit: Option<bool>, writes: Vec<(u64, Row, bool)>) -> bool {
        if self.owner != (id.client & 1) as u32 || self.kv.bytes(&txn_key(id)).is_some() {
            return false;
        }
        let mut operations = Vec::new();
        for (table, row, delete) in writes {
            operations.extend(self.row_ops(table, &row, delete).unwrap());
        }
        let value = Self::encode_transaction(commit, false, &operations);
        assert!(matches!(self.kv.submit(vec![WriteOp { key: txn_key(id), value: Some(value) }]),
            CommitResult::Committed));
        true
    }
    fn encode_transaction(commit: Option<bool>, settled: bool, operations: &[WriteOp]) -> Vec<u8> {
        let mut out = Writer::new();
        out.write_u32(match commit { None => 0, Some(true) => 1, Some(false) => 2 });
        out.write_u32(u32::from(settled));
        out.write_u64(operations.len() as u64);
        for operation in operations {
            out.write_bytes(&operation.key);
            out.write_u32(u32::from(operation.value.is_some()));
            if let Some(value) = &operation.value { out.write_bytes(value); }
        }
        out.into_bytes()
    }
    fn decode_transaction(bytes: &[u8]) -> (Option<bool>, bool, Vec<WriteOp>) {
        let mut input = Reader::new(bytes);
        let commit = match input.read_u32().ok().unwrap() {
            0 => None, 1 => Some(true), 2 => Some(false), _ => panic!("bad TP decision"),
        };
        let settled = match input.read_u32().ok().unwrap() {
            0 => false, 1 => true, _ => panic!("bad TP receipt"),
        };
        let count = usize::try_from(input.read_u64().ok().unwrap()).unwrap();
        let mut operations = Vec::new();
        for _ in 0..count {
            let key = input.read_bytes().ok().unwrap();
            let value = match input.read_u32().ok().unwrap() {
                0 => None, 1 => Some(input.read_bytes().ok().unwrap()), _ => panic!("bad TP write"),
            };
            operations.push(WriteOp { key, value });
        }
        assert!(input.finish() == Status::Ok);
        (commit, settled, operations)
    }
    pub fn decide(&mut self, id: TxnId, commit: bool) {
        let (old, settled, operations) = Self::decode_transaction(self.kv.bytes(&txn_key(id)).unwrap());
        assert!(old.is_none() || old == Some(commit));
        assert!(!settled);
        let value = Self::encode_transaction(Some(commit), false, &operations);
        assert!(matches!(self.kv.submit(vec![WriteOp { key: txn_key(id), value: Some(value) }]),
            CommitResult::Committed));
    }
}
impl DurableEngine for DiskEngine {
    fn recover_rows(&mut self) -> Status {
        if std::mem::take(&mut self.fail_recovery) { return Status::Io; }
        if !self.kv.settle() { return Status::Io; }
        self.working = Self::materialize(&self.kv);
        self.installed = None;
        self.recovered = true;
        Status::Ok
    }
    fn install(&mut self, admission: Admission) -> Status {
        if !self.recovered || admission.namespace.owner != self.owner || admission.namespace.coordinator {
            return Status::Invalid;
        }
        self.installed = Some(admission);
        Status::Ok
    }
    fn check(&self, admission: &Admission) -> bool {
        self.installed == Some(*admission) && self.kv.settled()
            && admission.namespace.owner == self.owner && !admission.namespace.coordinator
    }
    fn scan(&self, admission: &Admission, range: &KeyRange, after: Option<&Row>) -> Result<Option<Row>, Status> {
        if !self.check(admission) { return Err(Status::Retry); }
        for (cell, value) in &self.working {
            if selected(range, cell) && after.is_none_or(|row|
                (&cell.1, &cell.2) > (&row.coordinate, &row.key)) {
                return Ok(Some(Row { coordinate: cell.1.clone(), key: cell.2.clone(), value: value.clone() }));
            }
        }
        Ok(None)
    }
    fn mutate(&mut self, admission: &Admission, range: &KeyRange, row: &Row, delete: bool) -> CommitResult {
        if !self.check(admission) { return CommitResult::NotCommitted; }
        let cell = (range.table, row.coordinate.clone(), row.key.clone());
        assert!(selected(range, &cell));
        let Some(operations) = self.row_ops(range.table, row, delete) else { return CommitResult::NotCommitted; };
        self.kv.fault = std::mem::take(&mut self.row_fault);
        let result = self.kv.submit(operations);
        if result == CommitResult::Committed {
            if delete { self.working.remove(&cell); }
            else { self.working.insert(cell, row.value.clone()); }
        }
        self.mutations += 1;
        result
    }
    fn commit_rows(&mut self, admission: &Admission, _range: &KeyRange) -> CommitResult {
        if std::mem::take(&mut self.barrier_unknown) { return CommitResult::Unknown; }
        if !self.check(admission) { return CommitResult::NotCommitted; }
        assert!(self.working == Self::materialize(&self.kv));
        CommitResult::Committed
    }
    fn settle_transaction(&mut self, admission: &Admission, id: TxnId) -> Status {
        if !self.check(admission) || self.owner != (id.client & 1) as u32 { return Status::Retry; }
        let Some(bytes) = self.kv.bytes(&txn_key(id)) else {
            // No other fixture owner accepts this identity. Persisting this
            // receipt excludes every subsequent transaction() submission.
            let receipt = Self::encode_transaction(Some(false), true, &[]);
            assert!(matches!(self.kv.submit(vec![
                WriteOp { key: txn_key(id), value: Some(receipt) },
            ]), CommitResult::Committed));
            return Status::Ok;
        };
        let (decision, settled, mut operations) = Self::decode_transaction(bytes);
        if settled { return Status::Ok; }
        let Some(commit) = decision else { return Status::Retry; };
        let receipt = Self::encode_transaction(decision, true, &operations);
        if !commit { operations.clear(); }
        operations.push(WriteOp { key: txn_key(id), value: Some(receipt) });
        assert!(matches!(self.kv.submit(operations), CommitResult::Committed));
        self.working = Self::materialize(&self.kv);
        Status::Ok
    }
}
