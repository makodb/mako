//! Immutable transaction identities and the journal shared by 2PC adapters.
//! A journal acknowledgement means the selected durability policy is met.
//! Errors may follow a saved record: resolve by reading or repeating that record.
use crate::protocol::{
    redo::Row,
    wire::{Error, ErrorCode, Result, MAX_BYTES},
};
use bincode::Options;
use crate::protocol::verified::two_phase::{self as core, Decision};
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub const VERSION: u32 = 1;
pub const MAX_VOLATILE_PREPARE_BYTES: usize = 128 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd, Serialize, Deserialize)]
pub struct TransactionId {
    pub coordinator: u32,
    pub worker: u32,
    pub incarnation: u64,
    pub attempt: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Member {
    pub owner: u32,
    /// Original endpoint plus the configured replica endpoints for recovery.
    /// Discovery must still authenticate the owner and current authority.
    pub endpoints: Vec<String>,
    pub lease: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Transaction {
    pub version: u32,
    pub id: TransactionId,
    pub time_term: u32,
    pub durable: bool,
    /// Frozen in ascending owner order before the lock barrier begins.
    pub members: Vec<Member>,
}
impl Transaction {
    pub fn validate(&self) -> Result<()> {
        if self.version != VERSION
            || self.id.incarnation == 0
            || self.id.attempt == 0
            || self.members.is_empty()
            || self.members.len() > 256
            || crate::clock::distributed::DistributedTimeTerm::decode_legacy(self.time_term).is_err()
        {
            return Err(invalid("invalid transaction descriptor"));
        }
        let mut prior = None;
        for member in &self.members {
            if prior.is_some_and(|owner| owner >= member.owner)
                || member.lease == 0
                || member.endpoints.is_empty()
                || member.endpoints.len() > 256
                || member
                    .endpoints
                    .iter()
                    .any(|endpoint| endpoint.is_empty() || endpoint.len() > 1024)
            {
                return Err(invalid("invalid or repeated transaction member"));
            }
            prior = Some(member.owner);
        }
        if self.member(self.id.coordinator).is_none() {
            return Err(invalid("transaction coordinator is not a participant"));
        }
        Ok(())
    }
    pub fn member(&self, owner: u32) -> Option<&Member> {
        self.members.iter().find(|member| member.owner == owner)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Outcome {
    Commit,
    Abort,
}
impl Outcome {
    pub fn decision(self) -> Decision {
        match self {
            Self::Commit => Decision::Commit,
            Self::Abort => Decision::Abort,
        }
    }
}

#[expect(
    clippy::large_enum_variant,
    reason = "Journal commands own their bounded payloads and are passed by reference"
)]
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Record {
    Begin(Transaction),
    Prepare {
        transaction: Transaction,
        rows: Vec<Row>,
        receipt: Option<crate::protocol::wire::GatewayReceipt>,
    },
    Decide {
        transaction: Transaction,
        outcome: Outcome,
    },
    Complete {
        transaction: Transaction,
        outcome: Outcome,
    },
}
impl Record {
    pub fn transaction(&self) -> &Transaction {
        match self {
            Self::Begin(transaction)
            | Self::Prepare { transaction, .. }
            | Self::Decide { transaction, .. }
            | Self::Complete { transaction, .. } => transaction,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Participant {
    /// None is an Abort tombstone created before Prepare arrived.
    pub rows: Option<Vec<Row>>,
    pub outcome: Option<Outcome>,
    pub receipt: Option<crate::protocol::wire::GatewayReceipt>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Entry {
    pub transaction: Transaction,
    pub begun: bool,
    pub decision: Option<Outcome>,
    pub participant: Option<Participant>,
}
impl Entry {
    pub fn unresolved(&self) -> bool {
        self.participant
            .as_ref()
            .is_some_and(|p| p.rows.is_some() && p.outcome.is_none())
    }
}

pub struct Transition {
    pub entry: Entry,
    /// Publish these rows atomically with the changed journal entry. Prepare
    /// always returns an empty list. Duplicate completion never reapplies rows.
    pub rows: Vec<Row>,
    pub changed: bool,
}

/// Check a journal result after a compare-and-set race. A conflicting command
/// may consume a log position while the earlier transaction decision wins.
pub fn acknowledge(record: &Record, entry: &Entry) -> Result<()> {
    if entry.transaction != *record.transaction() {
        return Err(conflict("transaction ID belongs to another descriptor"));
    }
    let matches = match record {
        Record::Begin(_) => entry.begun,
        Record::Prepare { rows, receipt, .. } => entry.participant.as_ref().is_some_and(|p| {
            p.rows.as_ref() == Some(rows)
                && p.receipt == *receipt
                && p.outcome != Some(Outcome::Abort)
        }),
        Record::Decide { outcome, .. } => entry.decision == Some(*outcome),
        Record::Complete { outcome, .. } => {
            entry.participant.as_ref().and_then(|p| p.outcome) == Some(*outcome)
        }
    };
    if matches {
        Ok(())
    } else {
        Err(conflict("journal retained an earlier transaction outcome"))
    }
}

fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Invalid, message)
}
fn conflict(message: &str) -> Error {
    Error::new(ErrorCode::Conflict, message)
}

/// Validate and calculate one atomic journal transition. The caller serializes
/// this read/modify/write and acknowledges only after entry and rows are saved.
/// Decide(Commit) is submitted by the verified coordinator after all Yes votes.
/// Complete is delivered by that authenticated decision authority.
pub fn transition(
    owner: u32,
    durable: bool,
    prior: Option<&Entry>,
    record: &Record,
) -> Result<Transition> {
    let transaction = record.transaction();
    transaction.validate()?;
    if transaction.durable != durable || transaction.member(owner).is_none() {
        return Err(invalid("transaction durability or participant mismatch"));
    }
    let limit = if durable {
        MAX_BYTES
    } else {
        MAX_VOLATILE_PREPARE_BYTES
    };
    bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(limit as u64)
        .serialized_size(&(crate::protocol::wire::VERSION, record))
        .map_err(|error| {
            Error::new(
                ErrorCode::Capacity,
                format!("transaction journal record: {error}"),
            )
        })?;
    if prior.is_some_and(|entry| entry.transaction != *transaction) {
        return Err(conflict("transaction ID reused with different descriptor"));
    }
    let mut entry = prior.cloned().unwrap_or_else(|| Entry {
        transaction: transaction.clone(),
        begun: false,
        decision: None,
        participant: None,
    });
    let mut published = Vec::new();
    match record {
        Record::Begin(_) => {
            if owner != transaction.id.coordinator {
                return Err(invalid("Begin sent to non-coordinator"));
            }
            entry.begun = true;
        }
        Record::Prepare { rows, receipt, .. } => {
            if let Some(receipt) = receipt {
                receipt.validate()?;
                if transaction.members.len() != 1
                    || transaction.id.coordinator != owner
                    || receipt
                        .request
                        .target
                        .as_ref()
                        .is_none_or(|target| target.owner != owner)
                {
                    return Err(invalid(
                        "gateway receipt requires its single owning participant",
                    ));
                }
            }
            let mut keys = BTreeMap::new();
            for row in rows {
                row.validate()?;
                if row.time_term != transaction.time_term
                    || keys.insert((row.table, &row.key), ()).is_some()
                {
                    return Err(invalid(
                        "prepared rows have repeated keys or a different decision timestamp",
                    ));
                }
            }
            if let Some(participant) = &entry.participant {
                if participant.rows.as_ref() != Some(rows) || participant.receipt != *receipt {
                    return Err(conflict(
                        "prepared intent changed or transaction was aborted",
                    ));
                }
                if participant.outcome == Some(Outcome::Abort) {
                    return Err(conflict("transaction was aborted"));
                }
            } else {
                if entry.decision == Some(Outcome::Abort) {
                    return Err(conflict("coordinator has aborted this transaction"));
                }
                entry.participant = Some(Participant {
                    rows: Some(rows.clone()),
                    outcome: None,
                    receipt: receipt.clone(),
                });
            }
        }
        Record::Decide { outcome, .. } => {
            if owner != transaction.id.coordinator || !entry.begun {
                return Err(invalid("decision has no registered coordinator"));
            }
            if entry.decision.is_some_and(|saved| saved != *outcome)
                || entry
                    .participant
                    .as_ref()
                    .and_then(|p| p.outcome)
                    .is_some_and(|saved| saved != *outcome)
            {
                return Err(conflict("transaction decision is immutable"));
            }
            entry.decision = Some(*outcome);
        }
        Record::Complete { outcome, .. } => {
            if owner == transaction.id.coordinator && entry.decision != Some(*outcome) {
                return Err(conflict("local completion lacks its coordinator decision"));
            }
            let participant = entry.participant.get_or_insert(Participant {
                rows: None,
                outcome: None,
                receipt: None,
            });
            if let Some(saved) = participant.outcome {
                if saved != *outcome {
                    return Err(conflict("participant outcome is immutable"));
                }
            } else {
                let phase = if participant.rows.is_some() {
                    core::Phase::Prepared
                } else {
                    core::Phase::Executing
                };
                let finished = core::finish(phase, outcome.decision(), true, true);
                if core::retains_protection(finished) {
                    return Err(conflict("Commit has no prepared intent"));
                }
                if *outcome == Outcome::Commit {
                    published = participant
                        .rows
                        .as_ref()
                        .expect("checked prepared intent")
                        .clone();
                }
                participant.outcome = Some(*outcome);
            }
        }
    }
    let changed = prior != Some(&entry);
    Ok(Transition {
        entry,
        rows: published,
        changed,
    })
}

/// Stable records survive process failure iff durable() is true. Lookup returns
/// the current authoritative prefix, never a stale replica snapshot. apply is
/// atomic with respect to competing decisions, and errors permit prior effects.
pub trait Journal: Send + Sync {
    fn durable(&self) -> bool;
    fn apply(&self, record: &Record) -> Result<Entry>;
    fn lookup(&self, id: TransactionId) -> Result<Option<Entry>>;
    fn entries(&self) -> Result<Vec<Entry>>;
}

/// Testing-only, in-memory transaction log. It obeys the same journal
/// transitions as LocalLog and ReplicatedLog but never advertises durability.
/// Native row effects are supplied by the participant adapter; isolated tests
/// use their own byte-level row oracle. Production configuration must select
/// a persistent backend; native test fixtures explicitly opt into volatile mode.
pub struct MemoryLog {
    owner: u32,
    entries: Mutex<BTreeMap<TransactionId, Entry>>,
}
impl MemoryLog {
    pub fn new(owner: u32) -> Self {
        Self {
            owner,
            entries: Mutex::new(BTreeMap::new()),
        }
    }
}
impl Journal for MemoryLog {
    fn durable(&self) -> bool {
        false
    }
    fn apply(&self, record: &Record) -> Result<Entry> {
        let mut entries = self.entries.lock();
        let id = record.transaction().id;
        let next = transition(self.owner, false, entries.get(&id), record)?;
        entries.insert(id, next.entry.clone());
        Ok(next.entry)
    }
    fn lookup(&self, id: TransactionId) -> Result<Option<Entry>> {
        Ok(self.entries.lock().get(&id).cloned())
    }
    fn entries(&self) -> Result<Vec<Entry>> {
        Ok(self.entries.lock().values().cloned().collect())
    }
}

impl crate::protocol::contracts::CommitLog for MemoryLog {
    fn append(&self, _: &[u8], _: u32) -> Result<()> {
        Ok(())
    }
}
