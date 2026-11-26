//! Engine-neutral storage effects for the sharding actor. The actor owns range
//! policy; its range guard authorizes every mutation before the OCC + 2PC driver.
use crate::{
    migration::Schema,
    storage::{engine, CommitPermit, Store},
    wire::{Error, ErrorCode, Result, TableKey},
};
use mako_protocol::{
    commit::{self, Effects},
    journal::{self, Member, Outcome, Record, Transaction, TransactionId},
};
use mako_storage_api as api;
use std::{
    io::Read,
    ops::Bound,
    sync::{atomic::Ordering, Arc},
};

pub(crate) struct Row {
    pub coordinate: Vec<u8>,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}
pub(crate) struct Worker {
    worker: api::Worker,
    store: Arc<Store>,
    schema: Arc<Schema>,
    incarnation: u64,
    attempt: u64,
}
impl Worker {
    pub fn new(store: Arc<Store>, schema: Arc<Schema>) -> Result<Self> {
        let mut nonce = [0; 8];
        std::fs::File::open("/dev/urandom")
            .and_then(|mut f| f.read_exact(&mut nonce))
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        Ok(Self {
            worker: store.database.worker().map_err(engine)?,
            store,
            schema,
            incarnation: u64::from_ne_bytes(nonce).max(1),
            attempt: 0,
        })
    }
    pub fn scan(
        &mut self,
        catalog: u64,
        lo: &[u8],
        hi: Option<&[u8]>,
        after: Option<(&[u8], &[u8])>,
    ) -> Result<Option<Row>> {
        self.store.health()?;
        crate::migration_ranges::scan(&self.store, catalog, lo, hi)?;
        let permit = self.store.log.admit()?;
        if crate::migration_metadata::row_authority()
            .is_some_and(|term| permit.authority() != Some(term))
        {
            return Err(Error::new(
                ErrorCode::RouteChanged,
                "migration row authority changed",
            ));
        }
        if hi.is_some_and(|hi| hi <= lo) {
            return Err(Error::new(ErrorCode::Invalid, "empty migration range"));
        }
        let warehouse = self.schema.spec(catalog)?.warehouse;
        let (after_coordinate, after_key) = after.unwrap_or_default();
        let raw = if warehouse {
            None
        } else {
            Some(
                shard_host_core::scan_arguments(
                    u32::from(hi.is_some()),
                    u32::from(after.is_some()),
                    lo,
                    hi.unwrap_or_default(),
                    after_coordinate,
                    after_key,
                )
                .map_err(|_| Error::new(ErrorCode::Invalid, "invalid migration scan bounds"))?,
            )
        };
        for (key, table) in self
            .store
            .tables()
            .into_iter()
            .filter(|(key, _)| key.table == catalog)
        {
            let coordinate = key.warehouse.to_be_bytes();
            let (lower, upper) = if warehouse {
                let lower = match shard_host_core::fixed_scan(
                    &coordinate,
                    lo,
                    hi.unwrap_or_default(),
                    hi.is_some(),
                    after_coordinate,
                    after.is_some(),
                ) {
                    shard_host_core::FixedScan::Skip => continue,
                    shard_host_core::FixedScan::Stop => break,
                    shard_host_core::FixedScan::AllKeys => Bound::Unbounded,
                    shard_host_core::FixedScan::AfterKey => Bound::Excluded(after_key),
                };
                (lower, Bound::Unbounded)
            } else {
                match raw.as_ref().expect("raw scan decision") {
                    shard_host_core::ScanStart::Exhausted => return Ok(None),
                    shard_host_core::ScanStart::Rows { lower, upper } => {
                        let lower = match lower {
                            shard_host_core::ScanLower::Included(k) => Bound::Included(*k),
                            shard_host_core::ScanLower::Excluded(k) => Bound::Excluded(*k),
                        };
                        (lower, upper.map_or(Bound::Unbounded, Bound::Excluded))
                    }
                }
            };
            let mut participant = self.worker.begin_participant(0).map_err(engine)?;
            let rows = participant
                .scan(&table.tree, lower, upper, false, 1)
                .map_err(engine)?;
            participant.lock().map_err(engine)?;
            participant.validate().map_err(engine)?;
            participant.finish(true).map_err(engine)?;
            permit.check()?;
            if let Some(row) = rows.rows.into_iter().next() {
                return Ok(Some(Row {
                    coordinate: if warehouse {
                        coordinate.to_vec()
                    } else {
                        row.key.clone()
                    },
                    key: row.key,
                    value: row.value,
                }));
            }
        }
        permit.check()?;
        Ok(None)
    }
    pub fn put(&mut self, table: u64, coordinate: &[u8], key: &[u8], value: &[u8]) -> Result<()> {
        self.write(table, coordinate, key, Some(value))
    }
    pub fn remove(&mut self, table: u64, coordinate: &[u8], key: &[u8]) -> Result<()> {
        self.write(table, coordinate, key, None)
    }
    fn write(
        &mut self,
        table: u64,
        coordinate: &[u8],
        key: &[u8],
        value: Option<&[u8]>,
    ) -> Result<()> {
        self.store.health()?;
        crate::migration_ranges::point(&self.store, table, coordinate)?;
        let warehouse = if self.schema.spec(table)?.warehouse {
            u32::from_be_bytes(
                coordinate
                    .try_into()
                    .map_err(|_| Error::new(ErrorCode::Invalid, "invalid warehouse coordinate"))?,
            )
        } else {
            if coordinate != key {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "raw coordinate differs from key",
                ));
            }
            0
        };
        let table = self.store.table(TableKey { table, warehouse })?;
        let permit = self.store.log.admit()?;
        if crate::migration_metadata::row_authority()
            .is_some_and(|term| permit.authority() != Some(term))
        {
            return Err(Error::new(
                ErrorCode::RouteChanged,
                "migration row authority changed",
            ));
        }
        self.attempt = self.attempt.checked_add(1).ok_or_else(|| {
            Error::new(ErrorCode::Capacity, "migration transaction IDs exhausted")
        })?;
        let term = self.store.term.load(Ordering::Acquire);
        let mut participant = self.worker.begin_participant(term).map_err(engine)?;
        let changed = participant
            .mutate(
                &table.tree,
                table.log_id,
                if value.is_some() {
                    api::Mutation::Put
                } else {
                    api::Mutation::Remove
                },
                key,
                value.unwrap_or_default(),
                false,
            )
            .map_err(engine)?;
        if !changed && value.is_none() {
            participant
                .mutate(
                    &table.tree,
                    table.log_id,
                    api::Mutation::Put,
                    key,
                    &[],
                    false,
                )
                .map_err(engine)?;
            participant
                .mutate(
                    &table.tree,
                    table.log_id,
                    api::Mutation::Remove,
                    key,
                    &[],
                    false,
                )
                .map_err(engine)?;
        }
        commit_participant(
            participant,
            &self.store,
            term,
            TransactionId {
                coordinator: self.store.owner,
                worker: u32::MAX,
                incarnation: self.incarnation,
                attempt: self.attempt,
            },
            &*permit,
        )
    }
}

pub(crate) fn commit_participant(
    participant: api::Participant<'_>,
    store: &Arc<Store>,
    term: u32,
    id: TransactionId,
    permit: &dyn CommitPermit,
) -> Result<()> {
    let mut effects = One {
        participant: Some(participant),
        store,
        term,
        id,
        permit,
    };
    match commit::coordinate(&mut effects) {
        Ok(completion) if completion.acknowledged && completion.outcome == Outcome::Commit => {
            Ok(())
        }
        Ok(completion) if completion.acknowledged && completion.outcome == Outcome::Abort => {
            Err(completion
                .failure
                .unwrap_or_else(|| Error::new(ErrorCode::Conflict, "internal transaction aborted")))
        }
        Ok(_)
        | Err(Error {
            code: ErrorCode::Unknown,
            ..
        }) => {
            if let Some(participant) = effects.participant.take() {
                participant.retain();
            }
            let error = Error::new(
                ErrorCode::Unknown,
                "internal storage outcome requires recovery",
            );
            store.fence(error.clone());
            Err(error)
        }
        Err(error) => Err(error),
    }
}

struct One<'a, 'worker> {
    participant: Option<api::Participant<'worker>>,
    store: &'a Arc<Store>,
    term: u32,
    id: TransactionId,
    permit: &'a dyn CommitPermit,
}
impl One<'_, '_> {
    fn apply(&self, record: &Record) -> Result<journal::Entry> {
        match self.permit.authority() {
            Some(term) => self.store.log.apply_at(record, term),
            None => self.store.journal.apply(record),
        }
    }
}
impl Effects for One<'_, '_> {
    fn participants(&self) -> usize {
        1
    }
    fn lock(&mut self, _: usize) -> Result<()> {
        self.permit.check()?;
        self.participant.as_mut().unwrap().lock().map_err(engine)
    }
    fn register(&mut self) -> Result<Transaction> {
        self.permit.check()?;
        let timestamp = self
            .participant
            .as_mut()
            .unwrap()
            .timestamp()
            .map_err(engine)?;
        let endpoint =
            self.store.endpoint.read().clone().ok_or_else(|| {
                Error::new(ErrorCode::Invalid, "migration storage endpoint missing")
            })?;
        let transaction = Transaction {
            version: journal::VERSION,
            id: self.id,
            time_term: timestamp
                .checked_mul(10)
                .and_then(|t| t.checked_add(self.term))
                .ok_or_else(|| {
                    Error::new(ErrorCode::Capacity, "migration logical time overflow")
                })?,
            durable: self.store.journal.durable(),
            members: vec![Member {
                owner: self.store.owner,
                endpoints: vec![endpoint],
                lease: self.id.attempt,
            }],
        };
        self.apply(&Record::Begin(transaction.clone()))?;
        Ok(transaction)
    }
    fn prepare(&mut self, _: usize, transaction: &Transaction) -> Result<()> {
        self.permit.check()?;
        let participant = self.participant.as_mut().unwrap();
        participant.validate().map_err(engine)?;
        let rows = participant
            .prepared_rows(transaction.time_term)
            .map_err(engine)?;
        self.apply(&Record::Prepare {
            transaction: transaction.clone(),
            rows,
            receipt: None,
        })?;
        #[cfg(feature = "test-hooks")]
        if self.id.worker == u32::MAX {
            mako_sharding::runtime_durability::test_pause("row-prepared");
        }
        Ok(())
    }
    fn decide(&mut self, transaction: &Transaction, outcome: Outcome) -> Result<Outcome> {
        self.permit.check()?;
        self.apply(&Record::Decide {
            transaction: transaction.clone(),
            outcome,
        })?
        .decision
        .ok_or_else(|| Error::new(ErrorCode::Unknown, "migration decision not saved"))
    }
    fn complete(&mut self, _: usize, transaction: &Transaction, outcome: Outcome) -> Result<()> {
        self.permit.check()?;
        self.apply(&Record::Complete {
            transaction: transaction.clone(),
            outcome,
        })?;
        self.store.log.publish(transaction.id, &mut || {
            let mut participant = self.participant.take().ok_or_else(|| {
                Error::new(ErrorCode::Invalid, "migration participant already finished")
            })?;
            if outcome == Outcome::Commit {
                participant
                    .install(transaction.time_term / 10)
                    .map_err(engine)?;
            }
            participant
                .finish(outcome == Outcome::Commit)
                .map_err(engine)?;
            Ok(())
        })
    }
    fn abort_unprepared(&mut self, _: usize) -> Result<()> {
        if let Some(participant) = self.participant.take() {
            participant.finish(false).map_err(engine)?;
        }
        Ok(())
    }
}
