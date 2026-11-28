//! Derived protocol state for engines whose rows recover from checkpoint + log.
//! This index stores protocol records and replay identities, never a live user
//! row map. Its position is volatile; after process loss it starts at the last
//! checkpoint actually restored, regardless of any previous process's progress.
use crate::wire::{Error, ErrorCode, GatewayReceipt, Result};
use sto_core::protocol::{
    journal::{self, Entry, Record, TransactionId},
    redo::Row,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
pub(crate) struct Position {
    pub index: u64,
    pub term: u64,
}
impl From<raft_host::Position> for Position {
    fn from(p: raft_host::Position) -> Self {
        Self {
            index: p.index,
            term: p.term,
        }
    }
}
impl From<Position> for raft_host::Position {
    fn from(p: Position) -> Self {
        Self {
            index: p.index,
            term: p.term,
        }
    }
}
#[derive(Clone, Default, Serialize, Deserialize)]
pub(crate) struct Index {
    pub position: Position,
    pub fingerprints: BTreeMap<u64, (u64, [u8; 32])>,
    pub transactions: BTreeMap<TransactionId, Entry>,
    pub receipts: BTreeMap<(u64, u64), GatewayReceipt>,
}
fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Fatal, message)
}
pub(crate) struct Change {
    pub position: Position,
    fingerprint: [u8; 32],
    pub rows: Vec<Row>,
    entry: Option<Entry>,
    receipt: Option<GatewayReceipt>,
    duplicate: bool,
}
impl Change {
    pub fn table_ids(&self) -> Vec<u16> {
        self.rows
            .iter()
            .map(|row| row.table)
            .chain(
                self.entry
                    .iter()
                    .flat_map(|entry| entry.participant.iter())
                    .flat_map(|p| p.rows.iter().flatten())
                    .map(|row| row.table),
            )
            .collect::<std::collections::BTreeSet<_>>()
            .into_iter()
            .collect()
    }
}
impl Index {
    fn next(&self, position: Position, source: &[u8]) -> Result<Change> {
        let fingerprint = Sha256::digest(source).into();
        if position.index == 0 || position.term == 0 {
            return Err(invalid("zero application position"));
        }
        let duplicate = position.index <= self.position.index;
        if duplicate {
            if self.fingerprints.get(&position.index) != Some(&(position.term, fingerprint)) {
                return Err(invalid("changed command at an applied position"));
            }
        } else if self.position.index.checked_add(1) != Some(position.index)
            || position.term < self.position.term
        {
            return Err(invalid("application gap or regressing term"));
        }
        Ok(Change {
            position,
            fingerprint,
            rows: Vec::new(),
            entry: None,
            receipt: None,
            duplicate,
        })
    }
    pub fn no_op(&self, position: Position) -> Result<Change> {
        self.next(position, &[])
    }
    pub fn journal(
        &self,
        position: Position,
        source: &[u8],
        owner: u32,
        durable: bool,
        record: &Record,
    ) -> Result<Change> {
        let mut change = self.next(position, source)?;
        if change.duplicate {
            return Ok(change);
        }
        let prior = self.transactions.get(&record.transaction().id);
        let next = match journal::transition(owner, durable, prior, record) {
            Ok(next) => next,
            // A losing compare-and-set is still an applied log record. Its
            // caller checks acknowledgement against the retained first decision.
            Err(error) if error.code == ErrorCode::Conflict && prior.is_some() => {
                journal::Transition {
                    entry: prior.unwrap().clone(),
                    rows: Vec::new(),
                    changed: false,
                }
            }
            Err(error) => return Err(error),
        };
        if let Some(participant) = &next.entry.participant {
            if participant.outcome == Some(journal::Outcome::Commit) {
                if let Some(receipt) = &participant.receipt {
                    self.check_receipt(receipt)?;
                    change.receipt = Some(receipt.clone());
                }
            }
        }
        change.rows = next.rows;
        change.entry = Some(next.entry);
        Ok(change)
    }

    fn check_receipt(&self, receipt: &GatewayReceipt) -> Result<()> {
        if self
            .receipts
            .get(&(receipt.request.client, receipt.request.sequence))
            .is_some_and(|prior| prior != receipt)
        {
            return Err(invalid("gateway receipt identity changed"));
        }
        Ok(())
    }
    /// Caller first publishes the complete engine effect or retains its exact
    /// committed overlay behind a publication barrier. Failure must fence; it
    /// must never discard a committed effect and advance this index alone.
    pub fn publish(&mut self, change: Change) {
        if change.duplicate {
            return;
        }
        if let Some(entry) = change.entry {
            self.transactions.insert(entry.transaction.id, entry);
        }
        if let Some(receipt) = change.receipt {
            self.receipts
                .insert((receipt.request.client, receipt.request.sequence), receipt);
        }
        self.fingerprints.insert(
            change.position.index,
            (change.position.term, change.fingerprint),
        );
        self.position = change.position;
    }
    pub fn validate(&self, owner: u32, durable: bool) -> Result<()> {
        if (self.position.index == 0) != (self.position.term == 0) {
            return Err(invalid("invalid checkpoint position"));
        }
        if self.fingerprints.len() as u64 != self.position.index {
            return Err(invalid("incomplete checkpoint replay identities"));
        }
        for (offset, (index, (term, _))) in self.fingerprints.iter().enumerate() {
            if *index != offset as u64 + 1 || *term == 0 || *term > self.position.term {
                return Err(invalid("invalid checkpoint replay prefix"));
            }
        }
        if self.position.index != 0
            && self.fingerprints.get(&self.position.index).map(|v| v.0) != Some(self.position.term)
        {
            return Err(invalid("checkpoint head term mismatch"));
        }
        for (id, entry) in &self.transactions {
            entry.transaction.validate()?;
            if id != &entry.transaction.id
                || entry.transaction.durable != durable
                || entry.transaction.member(owner).is_none()
            {
                return Err(invalid("checkpoint transaction identity mismatch"));
            }
            if let Some(participant) = &entry.participant {
                if participant.outcome != Some(journal::Outcome::Abort)
                    && participant.rows.is_none()
                {
                    return Err(invalid("checkpoint forgot prepared intent"));
                }
                if let Some(rows) = &participant.rows {
                    for row in rows {
                        row.validate()?;
                        if row.time_term != entry.transaction.time_term {
                            return Err(invalid("checkpoint intent timestamp mismatch"));
                        }
                    }
                }
            }
        }
        for (key, receipt) in &self.receipts {
            receipt.validate()?;
            if *key != (receipt.request.client, receipt.request.sequence) {
                return Err(invalid("checkpoint receipt key mismatch"));
            }
        }
        Ok(())
    }
}

impl From<Position> for mako_storage_api::LogPosition {
    fn from(p: Position) -> Self {
        Self {
            index: p.index,
            term: p.term,
        }
    }
}
impl From<mako_storage_api::LogPosition> for Position {
    fn from(p: mako_storage_api::LogPosition) -> Self {
        Self {
            index: p.index,
            term: p.term,
        }
    }
}
fn metadata_key<T: Serialize>(kind: u8, key: &T) -> Result<Vec<u8>> {
    let mut result = vec![kind];
    result.extend(crate::wire::encode(key)?);
    Ok(result)
}
impl Change {
    pub fn fingerprint(&self) -> [u8; 32] {
        self.fingerprint
    }
    pub fn metadata(&self) -> Result<mako_storage_api::Metadata> {
        if self.duplicate {
            return Ok(Vec::new());
        }
        let mut metadata = vec![
            (vec![0], crate::wire::encode(&self.position)?),
            (
                metadata_key(1, &self.position.index)?,
                crate::wire::encode(&(self.position.term, self.fingerprint))?,
            ),
        ];
        if let Some(entry) = &self.entry {
            metadata.push((
                metadata_key(2, &entry.transaction.id)?,
                crate::wire::encode(entry)?,
            ));
        }
        if let Some(receipt) = &self.receipt {
            metadata.push((
                metadata_key(3, &(receipt.request.client, receipt.request.sequence))?,
                crate::wire::encode(receipt)?,
            ));
        }
        Ok(metadata)
    }
}
impl Index {
    pub fn from_metadata(
        metadata: mako_storage_api::Metadata,
        position: mako_storage_api::LogPosition,
        owner: u32,
        durable: bool,
    ) -> Result<Self> {
        let mut result = Self::default();
        for (key, value) in metadata {
            match key.first() {
                Some(0) if key.len() == 1 => result.position = crate::wire::decode(&value)?,
                Some(1) => {
                    let id: u64 = crate::wire::decode(&key[1..])?;
                    if key != metadata_key(1, &id)?
                        || result
                            .fingerprints
                            .insert(id, crate::wire::decode(&value)?)
                            .is_some()
                    {
                        return Err(invalid("invalid persistent prefix key"));
                    }
                }
                Some(2) => {
                    let entry: Entry = crate::wire::decode(&value)?;
                    if key != metadata_key(2, &entry.transaction.id)?
                        || result
                            .transactions
                            .insert(entry.transaction.id, entry)
                            .is_some()
                    {
                        return Err(invalid("invalid persistent transaction key"));
                    }
                }
                Some(3) => {
                    let receipt: GatewayReceipt = crate::wire::decode(&value)?;
                    let id = (receipt.request.client, receipt.request.sequence);
                    if key != metadata_key(3, &id)? || result.receipts.insert(id, receipt).is_some()
                    {
                        return Err(invalid("invalid persistent receipt key"));
                    }
                }
                _ => return Err(invalid("unknown protocol metadata key")),
            }
        }
        if result.position != position.into() {
            return Err(invalid(
                "rows and protocol metadata have different applied positions",
            ));
        }
        result.validate(owner, durable)?;
        Ok(result)
    }
}

#[cfg(test)]
mod tests;
