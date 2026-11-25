//! Committed application rows over the existing native RocksDB adapter.
//! Its atomic batches own both the row versions and applied Raft position.
//! Checkpoint generations, validation, publication and fsync are delegated to
//! sto-rocksdb. Masstree is rebuilt from this committed state after restart.
use crate::{
    redo::Row,
    wire::{self, Error, ErrorCode, Result},
};
use mako_protocol::journal::{self, Entry, Record, TransactionId};
use parking_lot::Mutex;
use sha2::{Digest, Sha256};
use std::{collections::BTreeMap, io::Write, path::Path, sync::Arc};
use sto_rocksdb::{Command, Database, Position, RequestId};

pub struct DurableRows {
    db: Arc<Database>,
    table_name: String,
    gate: Mutex<()>,
    sync_writes: bool,
}
fn storage(error: impl std::fmt::Display) -> Error {
    Error::new(ErrorCode::Fatal, format!("durable rows: {error}"))
}
fn row_key(row: &Row) -> Vec<u8> {
    let mut key = vec![0];
    key.extend_from_slice(&row.table.to_be_bytes());
    key.extend_from_slice(&row.key);
    key
}
fn entry_key(index: u64) -> Vec<u8> {
    let mut key = vec![1];
    key.extend_from_slice(&index.to_be_bytes());
    key
}
fn receipt_key(client: u64, sequence: u64) -> Vec<u8> {
    let mut key = vec![2];
    key.extend_from_slice(&client.to_be_bytes());
    key.extend_from_slice(&sequence.to_be_bytes());
    key
}
fn transaction_key(id: TransactionId) -> Vec<u8> {
    let mut key = vec![3];
    key.extend_from_slice(&id.coordinator.to_be_bytes());
    key.extend_from_slice(&id.worker.to_be_bytes());
    key.extend_from_slice(&id.incarnation.to_be_bytes());
    key.extend_from_slice(&id.attempt.to_be_bytes());
    key
}
fn request(position: Position) -> RequestId {
    RequestId {
        client: 1,
        sequence: position.index,
    }
}
impl DurableRows {
    pub fn open(path: &Path, group: u64, sync_writes: bool, schema: &[u8]) -> Result<Arc<Self>> {
        let digest = Sha256::digest(schema);
        let table_name = format!("mako-native-rows-v1-{:x}", digest);
        let db = Database::open(path, group, sync_writes).map_err(storage)?;
        let tables = db.list_tables().map_err(storage)?;
        if !tables.is_empty()
            && (tables.len() != 1 || db.lookup_table(&table_name).map_err(storage)?.is_none())
        {
            return Err(storage("schema does not match durable store"));
        }
        if tables.is_empty() && db.applied().map_err(storage)?.index != 0 {
            return Err(storage("missing durable row table"));
        }
        Ok(Arc::new(Self {
            db,
            table_name,
            gate: Mutex::new(()),
            sync_writes,
        }))
    }
    pub fn durable(&self) -> bool {
        self.sync_writes
    }
    pub fn position(&self) -> Result<Position> {
        self.db.applied().map_err(storage)
    }
    fn table(&self) -> Result<u64> {
        self.db
            .lookup_table(&self.table_name)
            .map_err(storage)?
            .map(|table| table.id)
            .ok_or_else(|| storage("row store not initialized by first Raft no-op"))
    }
    pub fn advance(&self, position: Position) -> Result<()> {
        let _gate = self.gate.lock();
        if self
            .db
            .lookup_table(&self.table_name)
            .map_err(storage)?
            .is_none()
        {
            if position.index != 1 {
                return Err(storage("first Raft prefix must be a no-op"));
            }
            let bytes =
                Command::create_table(self.db.group(), request(position), self.table_name.clone())
                    .and_then(|command| command.encode())
                    .map_err(storage)?;
            self.db.apply(position, &bytes).map_err(storage)?;
        } else {
            // Replaying the initialization no-op must reproduce its exact
            // internal create-table command, not a different empty entry.
            if position.index == 1 {
                let bytes = Command::create_table(
                    self.db.group(),
                    request(position),
                    self.table_name.clone(),
                )
                .and_then(|command| command.encode())
                .map_err(storage)?;
                self.db.apply(position, &bytes).map_err(storage)?;
            } else {
                self.db.advance_applied(position).map_err(storage)?;
            }
        }
        Ok(())
    }
    /// The source envelope is fingerprinted in the same batch as its rows.
    /// Duplicate positions require the same envelope, even after restart.
    pub fn apply(&self, position: Position, source: &[u8], rows: &[Row]) -> Result<Vec<Row>> {
        self.apply_gateway(position, source, rows, None)
    }
    pub fn apply_gateway(
        &self,
        position: Position,
        source: &[u8],
        rows: &[Row],
        receipt: Option<&wire::GatewayReceipt>,
    ) -> Result<Vec<Row>> {
        let _gate = self.gate.lock();
        if (rows.is_empty() && receipt.is_none()) || source.len() > wire::MAX_BYTES {
            return Err(storage("invalid durable command size"));
        }
        for row in rows {
            row.validate()?;
        }
        if let Some(receipt) = receipt {
            receipt.validate()?;
        }
        let table = self.table()?;
        let mut fingerprint = position.term.to_be_bytes().to_vec();
        fingerprint.extend_from_slice(&Sha256::digest(source));
        // Include decoded rows too: callers cannot pair one envelope with
        // different effects during repeated application.
        fingerprint.extend_from_slice(&Sha256::digest(wire::encode(&(rows, receipt))?));
        let mut worker = self.db.attach_worker().map_err(storage)?;
        let mut tx = worker.begin().map_err(storage)?;
        let entry = entry_key(position.index);
        let previous = self.position()?;
        let duplicate = position.index <= previous.index;
        if duplicate && tx.get(table, &entry).map_err(storage)?.as_deref() != Some(&fingerprint) {
            return Err(storage("conflicting replay position"));
        }
        if let Some(receipt) = receipt {
            let key = receipt_key(receipt.request.client, receipt.request.sequence);
            let encoded = wire::encode(receipt)?;
            let existing = tx.get(table, &key).map_err(storage)?;
            if duplicate {
                if existing.as_deref() != Some(&encoded) {
                    return Err(storage("receipt replay mismatch"));
                }
            } else {
                if existing.is_some() {
                    return Err(storage(
                        "request already published at another Raft position",
                    ));
                }
                tx.put(table, &key, &encoded).map_err(storage)?;
            }
        }
        let mut published = BTreeMap::new();
        for row in rows {
            let key = row_key(row);
            let prior: Option<Row> = tx
                .get(table, &key)
                .map_err(storage)?
                .map(|bytes| wire::decode(&bytes))
                .transpose()?;
            if !duplicate
                && prior
                    .as_ref()
                    .is_none_or(|old| row.supersedes(old.time_term))
            {
                tx.put(table, &key, &wire::encode(row)?).map_err(storage)?;
                published.insert(key, row.clone());
            } else if let Some(prior) = prior {
                published.insert(key, prior);
            } else {
                return Err(storage("duplicate entry is missing its row"));
            }
        }
        if duplicate {
            tx.rollback();
            return Ok(published.into_values().collect());
        }
        tx.put(table, &entry, &fingerprint).map_err(storage)?;
        let mut prepared = tx.prepare(request(position)).map_err(storage)?;
        prepared.certify().map_err(storage)?;
        prepared.mark_submitted().map_err(storage)?;
        prepared.mark_committed().map_err(storage)?;
        self.db
            .apply(position, prepared.bytes().map_err(storage)?)
            .map_err(storage)?;
        prepared.complete_applied().map_err(storage)?;
        Ok(published.into_values().collect())
    }
    pub fn visit(&self, mut visitor: impl FnMut(Row) -> Result<()>) -> Result<()> {
        let _gate = self.gate.lock();
        if self.position()?.index == 0 {
            return Ok(());
        }
        let table = self.table()?;
        let mut worker = self.db.attach_worker().map_err(storage)?;
        let mut tx = worker.begin().map_err(storage)?;
        let mut lower = vec![0];
        loop {
            let rows = tx.scan(table, &lower, Some(&[1]), 256).map_err(storage)?;
            if rows.is_empty() {
                break;
            }
            for (key, value) in rows {
                let row: Row = wire::decode(&value)?;
                row.validate()?;
                if row_key(&row) != key {
                    return Err(storage("durable row identity mismatch"));
                }
                visitor(row)?;
                lower = key;
                lower.push(0); // exclusive lower bound for the complete last key.
            }
        }
        tx.rollback();
        Ok(())
    }

    /// Save a 2PC transition and its committed rows in the same RocksDB command.
    /// Prepare saves intent only. Applied-position replay checks the exact input
    /// fingerprint, even when the transaction subsequently reached a later state.
    pub fn apply_journal(
        &self,
        position: Position,
        source: &[u8],
        owner: u32,
        record: &Record,
    ) -> Result<(Vec<Row>, Entry)> {
        let _gate = self.gate.lock();
        if source.len() > wire::MAX_BYTES {
            return Err(storage("journal envelope too large"));
        }
        let table = self.table()?;
        let mut fingerprint = position.term.to_be_bytes().to_vec();
        fingerprint.extend_from_slice(&Sha256::digest(source));
        fingerprint.extend_from_slice(&Sha256::digest(wire::encode(&(owner, record))?));
        let mut worker = self.db.attach_worker().map_err(storage)?;
        let mut tx = worker.begin().map_err(storage)?;
        let entry_key = entry_key(position.index);
        let key = transaction_key(record.transaction().id);
        let prior: Option<Entry> = tx
            .get(table, &key)
            .map_err(storage)?
            .map(|bytes| wire::decode(&bytes))
            .transpose()?;
        if position.index <= self.position()?.index {
            if tx.get(table, &entry_key).map_err(storage)?.as_deref() != Some(&fingerprint) {
                return Err(storage("conflicting journal replay position"));
            }
            let entry = prior.ok_or_else(|| storage("journal replay lost its transaction"))?;
            if entry.transaction != *record.transaction() {
                return Err(storage("journal replay descriptor changed"));
            }
            tx.rollback();
            return Ok((Vec::new(), entry));
        }
        if record.transaction().durable && !self.sync_writes {
            return Err(storage("durable prepare requires synchronous storage"));
        }
        let next = match journal::transition(
            owner,
            record.transaction().durable,
            prior.as_ref(),
            record,
        ) {
            Ok(next) => next,
            // Competing decisions are normal CAS refusals, not corruption of
            // the Raft state machine. Persist the refused command's position
            // and fingerprint while retaining the first transaction record.
            Err(error) if error.code == ErrorCode::Conflict && prior.is_some() => {
                journal::Transition {
                    entry: prior.expect("checked existing transaction"),
                    rows: Vec::new(),
                    changed: false,
                }
            }
            Err(error) => return Err(error),
        };
        let mut published = Vec::new();
        for row in &next.rows {
            let key = row_key(row);
            let old: Option<Row> = tx
                .get(table, &key)
                .map_err(storage)?
                .map(|bytes| wire::decode(&bytes))
                .transpose()?;
            if old.as_ref().is_none_or(|old| row.supersedes(old.time_term)) {
                tx.put(table, &key, &wire::encode(row)?).map_err(storage)?;
                published.push(row.clone());
            } else if old.as_ref() != Some(row) {
                // A prepared transaction owns its write locks. A newer or
                // different committed value is a violated adapter contract,
                // not permission to acknowledge a silently skipped commit.
                return Err(storage("prepared write conflicts with durable row"));
            }
        }
        if let Some(participant) = &next.entry.participant {
            if participant.outcome == Some(journal::Outcome::Commit) {
                if let Some(receipt) = &participant.receipt {
                    let receipt_key = receipt_key(receipt.request.client, receipt.request.sequence);
                    let bytes = wire::encode(receipt)?;
                    if tx
                        .get(table, &receipt_key)
                        .map_err(storage)?
                        .is_some_and(|prior| prior != bytes)
                    {
                        return Err(storage(
                            "gateway receipt changed during transaction completion",
                        ));
                    }
                    tx.put(table, &receipt_key, &bytes).map_err(storage)?;
                }
            }
        }
        tx.put(table, &key, &wire::encode(&next.entry)?)
            .map_err(storage)?;
        tx.put(table, &entry_key, &fingerprint).map_err(storage)?;
        let mut prepared = tx.prepare(request(position)).map_err(storage)?;
        prepared.certify().map_err(storage)?;
        prepared.mark_submitted().map_err(storage)?;
        prepared.mark_committed().map_err(storage)?;
        self.db
            .apply(position, prepared.bytes().map_err(storage)?)
            .map_err(storage)?;
        prepared.complete_applied().map_err(storage)?;
        Ok((published, next.entry))
    }

    pub fn transaction(&self, id: TransactionId) -> Result<Option<Entry>> {
        let _gate = self.gate.lock();
        if self.position()?.index == 0 {
            return Ok(None);
        }
        let table = self.table()?;
        let mut worker = self.db.attach_worker().map_err(storage)?;
        let mut tx = worker.begin().map_err(storage)?;
        let bytes = tx.get(table, &transaction_key(id)).map_err(storage)?;
        tx.rollback();
        bytes.map(|bytes| wire::decode(&bytes)).transpose()
    }

    pub fn transactions(&self) -> Result<Vec<Entry>> {
        let _gate = self.gate.lock();
        if self.position()?.index == 0 {
            return Ok(Vec::new());
        }
        let table = self.table()?;
        let mut worker = self.db.attach_worker().map_err(storage)?;
        let mut tx = worker.begin().map_err(storage)?;
        let mut lower = vec![3];
        let mut entries = Vec::new();
        loop {
            let rows = tx.scan(table, &lower, Some(&[4]), 256).map_err(storage)?;
            if rows.is_empty() {
                break;
            }
            for (key, bytes) in rows {
                let entry: Entry = wire::decode(&bytes)?;
                entry.transaction.validate()?;
                if transaction_key(entry.transaction.id) != key {
                    return Err(storage("durable transaction identity mismatch"));
                }
                entries.push(entry);
                lower = key;
                lower.push(0);
            }
        }
        tx.rollback();
        Ok(entries)
    }
    pub fn gateway_receipt(
        &self,
        request: &wire::GatewayRequest,
    ) -> Result<Option<wire::GatewayResponse>> {
        let _gate = self.gate.lock();
        let table = self.table()?;
        let mut worker = self.db.attach_worker().map_err(storage)?;
        let mut tx = worker.begin().map_err(storage)?;
        let bytes = tx
            .get(table, &receipt_key(request.client, request.sequence))
            .map_err(storage)?;
        tx.rollback();
        let Some(bytes) = bytes else {
            return Ok(None);
        };
        let receipt: wire::GatewayReceipt = wire::decode(&bytes)?;
        receipt.validate()?;
        if !receipt.matches(request) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "gateway identity reused with different intent",
            ));
        }
        Ok(Some(receipt.response))
    }
    pub fn create_checkpoint(&self, path: &Path, position: Position) -> Result<()> {
        let _gate = self.gate.lock();
        self.db.create_checkpoint(path, position).map_err(storage)?;
        let mut schema = std::fs::File::create(path.join("MAKO_SCHEMA")).map_err(storage)?;
        schema
            .write_all(self.table_name.as_bytes())
            .map_err(storage)?;
        schema.sync_all().map_err(storage)?;
        std::fs::File::open(path)
            .and_then(|directory| directory.sync_all())
            .map_err(storage)
    }
    fn check_schema(&self, path: &Path) -> Result<()> {
        if std::fs::read(path.join("MAKO_SCHEMA")).map_err(storage)? != self.table_name.as_bytes() {
            return Err(storage("checkpoint schema mismatch"));
        }
        Ok(())
    }
    pub fn validate_checkpoint(&self, path: &Path, position: Position) -> Result<()> {
        let _gate = self.gate.lock();
        self.check_schema(path)?;
        self.db.validate_checkpoint(path, position).map_err(storage)
    }
    pub fn publish_checkpoint(&self, path: &Path, position: Position) -> Result<()> {
        let _gate = self.gate.lock();
        self.check_schema(path)?;
        self.db
            .publish_checkpoint(path, position)
            .map_err(storage)?;
        self.table()?;
        Ok(())
    }
}
