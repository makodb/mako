//! Affine native participants owned by the shard service through a saved decision.
use crate::{
    rpc::Network,
    storage::{engine, Protection, Store},
    wire::*,
};
use sto_core::protocol::{
    contracts::ParticipantEngine,
    journal::{Entry, Outcome, Record, Transaction, TransactionId},
    participant::Lane,
};
use sto_core::protocol::verified::two_phase::{self as core, Phase};
use mako_storage_api as sto;
use std::{
    sync::{atomic::Ordering, Arc, Weak},
    time::{Duration, Instant},
};

pub(crate) struct NativeEngine {
    worker: sto::Worker,
    store: Weak<Store>,
    network: Arc<Network>,
    protection: Option<Protection>,
    retained: bool,
    phase: Phase,
    id: Option<TransactionId>,
    lease: Option<u64>,
    term: Option<u32>,
    transaction: Option<Transaction>,
    proposal: Option<u32>,
    prepared_rows: Option<Vec<sto_core::protocol::redo::Row>>,
    receipt: Option<GatewayReceipt>,
    last_progress: Instant,
}
impl NativeEngine {
    pub(crate) fn new(store: &Arc<Store>, network: Arc<Network>) -> Result<Self> {
        Ok(Self {
            worker: store.database.worker().map_err(engine)?,
            store: Arc::downgrade(store),
            network,
            protection: None,
            retained: false,
            phase: Phase::Executing,
            id: None,
            lease: None,
            term: None,
            transaction: None,
            proposal: None,
            prepared_rows: None,
            receipt: None,
            last_progress: Instant::now(),
        })
    }
    fn store(&self) -> Result<Arc<Store>> {
        self.store
            .upgrade()
            .ok_or_else(|| Error::new(ErrorCode::Closed, "shard stopped"))
    }
    fn abort_unprepared(&mut self) -> Result<()> {
        if !core::may_abort_locally(self.phase) {
            return Err(Error::new(
                ErrorCode::Unknown,
                "prepared participant needs a saved decision",
            ));
        }
        self.worker.abort().map_err(engine)?;
        self.retained = false;
        self.phase = Phase::Aborted;
        self.protection.take();
        Ok(())
    }
    fn lock(&mut self, id: TransactionId, lease: u64, term: u32) -> Result<TransactionValue> {
        let store = self.store()?;
        if self.id == Some(id) {
            if let Some(timestamp) = self.proposal {
                return Ok(TransactionValue::Locked {
                    timestamp,
                    durable: store.journal.durable(),
                });
            }
        }
        if !self.retained || !matches!(self.phase, Phase::Executing) {
            return Err(Error::new(
                ErrorCode::Conflict,
                "participant is not executing",
            ));
        }
        if term < store.term.load(Ordering::Acquire) {
            return Err(Error::new(
                ErrorCode::Conflict,
                "application term changed before locking",
            ));
        }
        self.protection.as_ref().expect("executing epoch").check()?;
        let mut participant = self.worker.resume_participant().map_err(engine)?;
        let result = participant
            .lock()
            .and_then(|()| participant.timestamp())
            .map_err(engine);
        participant.retain();
        let timestamp = result?;
        self.id = Some(id);
        self.lease = Some(lease);
        self.term = Some(term);
        self.proposal = Some(timestamp);
        self.phase = Phase::Locked;
        self.last_progress = Instant::now();
        Ok(TransactionValue::Locked {
            timestamp,
            durable: store.journal.durable(),
        })
    }
    fn prepare(
        &mut self,
        transaction: &Transaction,
        receipt: Option<&GatewayReceipt>,
    ) -> Result<TransactionValue> {
        let store = self.store()?;
        if self.id != Some(transaction.id) || !self.retained {
            return Err(Error::new(
                ErrorCode::Conflict,
                "prepare has no locked participant",
            ));
        }
        if self
            .transaction
            .as_ref()
            .is_some_and(|prior| prior != transaction)
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "prepared descriptor changed",
            ));
        }
        if self.transaction.is_some() && self.receipt.as_ref() != receipt {
            return Err(Error::new(
                ErrorCode::Invalid,
                "prepared gateway receipt changed",
            ));
        }
        if matches!(self.phase, Phase::Prepared) {
            return Ok(TransactionValue::Prepared);
        }
        if !matches!(self.phase, Phase::Locked | Phase::Preparing) {
            return Err(Error::new(
                ErrorCode::Conflict,
                "participant cannot prepare",
            ));
        }
        self.protection.as_ref().expect("locked epoch").check()?;
        if self.prepared_rows.is_none() {
            let mut participant = self.worker.resume_participant().map_err(engine)?;
            let result = (|| {
                participant.validate().map_err(engine)?;
                participant
                    .prepared_rows(transaction.time_term)
                    .map_err(engine)
            })();
            // Retain even if append or validation later refuses. Only a saved
            // Abort may release a participant after its prepare might be saved.
            participant.retain();
            self.prepared_rows = Some(result?);
            self.phase = core::begin_prepare(self.phase, true, true);
            self.transaction = Some(transaction.clone());
            self.receipt = receipt.cloned();
        }
        store.journal.apply(&Record::Prepare {
            transaction: transaction.clone(),
            rows: self.prepared_rows.as_ref().expect("frozen redo").clone(),
            receipt: self.receipt.clone(),
        })?;
        self.phase = core::prepared(self.phase, true);
        self.last_progress = Instant::now();
        Ok(TransactionValue::Prepared)
    }
    fn complete(
        &mut self,
        transaction: &Transaction,
        outcome: Outcome,
    ) -> Result<TransactionValue> {
        let store = self.store()?;
        if self
            .transaction
            .as_ref()
            .is_some_and(|prior| prior.id == transaction.id && prior != transaction)
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "completion descriptor changed",
            ));
        }
        // Verify authority rather than treating a delayed client message as a
        // decision. An unreachable authority leaves protection in place.
        if decision(&store, &self.network, transaction, false)? != Some(outcome) {
            return Err(Error::new(
                ErrorCode::Unknown,
                "completion has no matching saved decision",
            ));
        }
        let current = self.id == Some(transaction.id);
        let saved = store.journal.lookup(transaction.id)?;
        if saved
            .as_ref()
            .is_some_and(|entry| entry.transaction != *transaction)
        {
            return Err(Error::new(ErrorCode::Invalid, "transaction ID reused"));
        }
        if let Some(saved) = saved
            .as_ref()
            .and_then(|e| e.participant.as_ref())
            .and_then(|p| p.outcome)
        {
            if saved != outcome {
                return Err(Error::new(
                    ErrorCode::Conflict,
                    "participant outcome changed",
                ));
            }
            if !current || !self.retained {
                return Ok(TransactionValue::Completed(outcome));
            }
        }
        if outcome == Outcome::Commit
            && (!current
                || !self.retained
                || !matches!(self.phase, Phase::Preparing | Phase::Prepared))
        {
            return Err(Error::new(
                ErrorCode::Unknown,
                "prepared intent needs recovery before commit",
            ));
        }
        // Persistent adapters atomically save completion with committed rows.
        // Volatile receipts remain private to this serial worker until install.
        store.journal.apply(&Record::Complete {
            transaction: transaction.clone(),
            outcome,
        })?;
        if current && self.retained {
            let result = store.log.publish(transaction.id, &mut || {
                if outcome == Outcome::Commit {
                    let mut participant = self.worker.resume_participant().map_err(engine)?;
                    match participant
                        .install(transaction.time_term / 10)
                        .map_err(engine)
                    {
                        Ok(()) => participant.finish(true).map(|_| ()).map_err(engine),
                        Err(error) => {
                            participant.retain();
                            Err(error)
                        }
                    }
                } else {
                    self.worker.abort().map_err(engine)
                }
            });
            if let Err(error) = result {
                // A saved completion must never expose a partly installed RAM
                // image to another transaction. Restart rebuilds durable rows.
                store.fence(error.clone());
                return Err(Error::new(ErrorCode::Unknown, error.to_string()));
            }
            self.retained = false;
            // A Preparing state can resolve a saved Prepare whose reply was lost.
            self.phase = core::prepared(self.phase, true);
            self.phase = core::finish(self.phase, outcome.decision(), true, true);
            self.protection.take();
        }
        self.last_progress = Instant::now();
        Ok(TransactionValue::Completed(outcome))
    }
    pub(crate) fn restore(&mut self, entry: Entry) -> Result<()> {
        if !entry.unresolved() {
            return Ok(());
        }
        let store = self.store()?;
        let transaction = entry.transaction;
        store
            .term
            .fetch_max(transaction.time_term % 10, Ordering::AcqRel);
        if self.retained {
            if self.transaction.as_ref() == Some(&transaction) {
                // Reacquire authority for a new term without dropping locks.
                self.protection = Some(store.protect_recovered()?);
                return Ok(());
            }
            self.abort_unprepared()?;
        }
        self.protection = Some(store.protect_recovered()?);
        let prepared = entry.participant.expect("unresolved participant");
        let rows = prepared.rows.expect("unresolved intent");
        let mut participant = self
            .worker
            .begin_participant(transaction.time_term % 10)
            .map_err(engine)?;
        let result = (|| {
            for row in &rows {
                let table = store.table_for_log(row.table)?;
                let mutation = if row.value.is_some() {
                    sto::Mutation::Put
                } else {
                    sto::Mutation::Remove
                };
                if !participant
                    .mutate(
                        &table.tree,
                        row.table,
                        mutation,
                        &row.key,
                        row.value.as_deref().unwrap_or_default(),
                        false,
                    )
                    .map_err(engine)?
                {
                    // An insert followed by delete may have prepared a final
                    // tombstone for an originally absent key. Rebuild a private
                    // tombstone intent so another insertion cannot pass it.
                    if row.value.is_some()
                        || !participant
                            .mutate(
                                &table.tree,
                                row.table,
                                sto::Mutation::Put,
                                &row.key,
                                &[],
                                false,
                            )
                            .map_err(engine)?
                        || !participant
                            .mutate(
                                &table.tree,
                                row.table,
                                sto::Mutation::Remove,
                                &row.key,
                                &[],
                                false,
                            )
                            .map_err(engine)?
                    {
                        return Err(Error::new(
                            ErrorCode::Unknown,
                            "prepared intent cannot be restored",
                        ));
                    }
                }
            }
            participant.lock().map_err(engine)?;
            participant.validate().map_err(engine)?;
            let restored = participant
                .prepared_rows(transaction.time_term)
                .map_err(engine)?;
            if restored != rows {
                return Err(Error::new(
                    ErrorCode::Unknown,
                    "recovered intent differs from saved prepare",
                ));
            }
            Ok(())
        })();
        participant.retain();
        self.retained = true;
        self.phase = Phase::Prepared;
        self.id = Some(transaction.id);
        self.lease = transaction.member(store.owner).map(|member| member.lease);
        self.term = Some(transaction.time_term % 10);
        self.transaction = Some(transaction);
        self.prepared_rows = Some(rows);
        self.receipt = prepared.receipt;
        self.last_progress = Instant::now() - Duration::from_secs(31);
        if let Err(error) = &result {
            store.fence(error.clone());
        }
        result
    }
}
impl ParticipantEngine for NativeEngine {
    fn minimum_term(&self) -> u32 {
        self.store
            .upgrade()
            .map_or(u32::MAX, |store| store.term.load(Ordering::Acquire))
    }
    fn accepts(&self, action: &Action) -> bool {
        match self.phase {
            Phase::Executing | Phase::Committed | Phase::Aborted => true,
            Phase::Locked => *action == Action::Abort,
            Phase::Preparing | Phase::Prepared => false,
        }
    }
    fn release_terminal(&mut self) {
        self.protection.take();
    }
    fn retain_uncertain(&mut self) {}
    fn execute(&mut self, request: &Request, begin: bool) -> Result<Value> {
        let store = self.store()?;
        store.health()?;
        if begin {
            if self.retained {
                self.abort_unprepared()?;
            }
            self.protection.take();
            self.id = None;
            self.lease = None;
            self.term = None;
            self.transaction = None;
            self.proposal = None;
            self.prepared_rows = None;
            self.receipt = None;
            self.phase = Phase::Executing;
            store
                .term
                .fetch_max(request.identity.term, Ordering::AcqRel);
            if request.action != Action::Abort {
                self.protection = Some(store.protect()?);
                self.worker
                    .begin_participant(request.identity.term)
                    .map_err(engine)?
                    .retain();
                self.retained = true;
            }
        }
        self.last_progress = Instant::now();
        if request.action == Action::Abort {
            self.abort_unprepared()?;
            return Ok(Value::Done);
        }
        let mut participant = self.worker.resume_participant().map_err(engine)?;
        let result = execute(
            &store,
            self.protection.as_ref().expect("executing epoch"),
            &mut participant,
            request.identity,
            &request.action,
        );
        participant.retain();
        if result.is_err() {
            self.abort_unprepared()?;
        }
        result
    }
}
impl Drop for NativeEngine {
    fn drop(&mut self) {
        if matches!(self.phase, Phase::Preparing | Phase::Prepared) {
            if let Some(store) = self.store.upgrade() {
                // A worker panic can unwind this owner independently of shard
                // shutdown. Fence before native Drop releases its locks, and
                // keep migration blocked until the shard itself is retired.
                store.fence(Error::new(ErrorCode::Unknown, "prepared worker terminated"));
                if let Some(protection) = self.protection.take() {
                    store.quarantine(protection);
                }
            }
        }
        let _ = self.worker.abort();
        // Durable intent survives and must be restored before serving again.
    }
}

pub(crate) fn transaction(
    lane: &mut Lane<NativeEngine>,
    request: TransactionRequest,
) -> TransactionResponse {
    let owner = lane
        .engine()
        .store
        .upgrade()
        .map_or(u32::MAX, |store| store.owner);
    let current = lane.owns_transaction(&request)
        || (lane.engine().id == Some(request.id)
            && lane.engine().lease == Some(request.lease)
            && lane.engine().term == Some(request.term));
    let result = (|| {
        if request.term >= 10 || request.lease == 0 {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid transaction request",
            ));
        }
        match &request.action {
            TransactionAction::Lock => {
                if !current {
                    return Err(Error::new(
                        ErrorCode::Conflict,
                        "lock does not own this attempt",
                    ));
                }
                lane.engine_mut()
                    .lock(request.id, request.lease, request.term)
            }
            TransactionAction::Prepare(transaction)
            | TransactionAction::PrepareGateway { transaction, .. }
            | TransactionAction::Decide { transaction, .. } => {
                transaction.validate()?;
                if transaction.id != request.id
                    || transaction.time_term % 10 != request.term
                    || transaction
                        .member(owner)
                        .is_none_or(|member| member.lease != request.lease)
                {
                    return Err(Error::new(
                        ErrorCode::Invalid,
                        "transaction envelope mismatch",
                    ));
                }
                if matches!(
                    request.action,
                    TransactionAction::Prepare(_) | TransactionAction::PrepareGateway { .. }
                ) {
                    if !current {
                        return Err(Error::new(
                            ErrorCode::Conflict,
                            "prepare does not own this attempt",
                        ));
                    }
                    let receipt = match &request.action {
                        TransactionAction::PrepareGateway { receipt, .. } => Some(receipt),
                        _ => None,
                    };
                    lane.engine_mut().prepare(transaction, receipt)
                } else if let TransactionAction::Decide { outcome, .. } = request.action {
                    lane.engine_mut().complete(transaction, outcome)
                } else {
                    unreachable!()
                }
            }
        }
    })();
    if current {
        match lane.engine().phase {
            Phase::Preparing | Phase::Prepared => lane.seal_external(),
            Phase::Committed => lane.finish_external(Outcome::Commit),
            Phase::Aborted => lane.finish_external(Outcome::Abort),
            _ => (),
        }
    }
    TransactionResponse {
        id: request.id,
        owner,
        result,
    }
}

pub(crate) fn resolve_idle(lane: &mut Lane<NativeEngine>) {
    let engine = lane.engine_mut();
    if engine.last_progress.elapsed() < Duration::from_secs(30) {
        return;
    }
    if engine.retained && matches!(engine.phase, Phase::Executing | Phase::Locked) {
        if engine.abort_unprepared().is_ok() {
            lane.finish_external(Outcome::Abort);
        }
        return;
    }
    if !matches!(engine.phase, Phase::Preparing | Phase::Prepared) {
        return;
    }
    engine.last_progress = Instant::now();
    let Some(transaction) = engine.transaction.clone() else {
        return;
    };
    let Ok(store) = engine.store() else {
        return;
    };
    if let Ok(Some(outcome)) = decision(&store, &engine.network, &transaction, true) {
        if engine.complete(&transaction, outcome).is_ok() {
            lane.finish_external(outcome);
        }
    }
}

/// Only the coordinator's authoritative journal may turn Pending into Abort.
/// Its atomic decision record races safely with the foreground Commit writer.
pub(crate) fn local_decision(
    store: &Store,
    transaction: &Transaction,
    resolve: bool,
) -> Result<Option<Outcome>> {
    transaction.validate()?;
    if transaction.id.coordinator != store.owner {
        return Err(Error::new(
            ErrorCode::Invalid,
            "wrong coordinator authority",
        ));
    }
    let entry = store.journal.lookup(transaction.id)?.ok_or_else(|| {
        Error::new(
            ErrorCode::Unknown,
            "coordinator registration is unavailable",
        )
    })?;
    if entry.transaction != *transaction || !entry.begun {
        return Err(Error::new(
            ErrorCode::Invalid,
            "coordinator descriptor mismatch",
        ));
    }
    if entry.decision.is_some() || !resolve {
        return Ok(entry.decision);
    }
    match store.journal.apply(&Record::Decide {
        transaction: transaction.clone(),
        outcome: Outcome::Abort,
    }) {
        Ok(entry) => Ok(entry.decision),
        Err(error) => store
            .journal
            .lookup(transaction.id)?
            .and_then(|e| e.decision)
            .map(Some)
            .ok_or(error),
    }
}
fn decision(
    store: &Store,
    network: &Network,
    transaction: &Transaction,
    resolve: bool,
) -> Result<Option<Outcome>> {
    if store.owner == transaction.id.coordinator {
        return local_decision(store, transaction, resolve);
    }
    let home = transaction
        .member(transaction.id.coordinator)
        .expect("validated coordinator");
    let mut failure = Error::new(ErrorCode::Unknown, "coordinator is unavailable");
    for endpoint in &home.endpoints {
        match network.call(
            endpoint,
            Message::TransactionOutcome {
                transaction: transaction.clone(),
                resolve,
            },
            Duration::from_secs(2),
        ) {
            Ok(Reply::TransactionOutcome { id, owner, outcome })
                if id == transaction.id && owner == home.owner =>
            {
                match outcome {
                    Ok(outcome) => return Ok(outcome),
                    Err(error) => failure = error,
                }
            }
            Ok(Reply::Error(error)) | Err(error) => failure = error,
            _ => failure = Error::new(ErrorCode::Transport, "decision reply identity mismatch"),
        }
    }
    Err(failure)
}

fn execute(
    store: &Store,
    protection: &Protection,
    participant: &mut sto::Participant<'_>,
    identity: Identity,
    action: &Action,
) -> Result<Value> {
    protection.check()?;
    match action {
        Action::ScanPage { access, request } => {
            crate::scan::produce(store, protection, participant, identity, access, request)
        }
        Action::Get { access, key } => {
            protection.point(access, key)?;
            let table = store.table(access.table)?;
            let result = participant.get(&table.tree, key).map_err(engine)?;
            Ok(Value::Point {
                value: result.value,
                time_term: result.time_term,
            })
        }
        Action::Scan {
            access,
            lower,
            upper,
            reverse,
            limit,
        } => {
            protection.range(access, lower, upper)?;
            let table = store.table(access.table)?;
            let result = participant
                .scan(
                    &table.tree,
                    lower.as_ref(),
                    upper.as_ref(),
                    *reverse,
                    *limit,
                )
                .map_err(engine)?;
            let value = Value::Scan {
                rows: result
                    .rows
                    .into_iter()
                    .map(|r| Row {
                        key: r.key,
                        value: r.value,
                        time_term: r.time_term,
                    })
                    .collect(),
                read_dependency: result.read_dependency,
            };
            // Refuse before terminal effects; the complete response adds a small
            // fixed identity/envelope overhead to the encoded value.
            if encode(&value)?.len() > MAX_BYTES - 256 {
                return Err(Error::new(
                    ErrorCode::Capacity,
                    "scan response exceeds RPC limit",
                ));
            }
            Ok(value)
        }
        Action::Stage { writes } => {
            for write in writes {
                protection.point(&write.access, &write.key)?;
                let table = store.table(write.access.table)?;
                let mutation = match write.mutation {
                    Mutation::Put => sto::Mutation::Put,
                    Mutation::Insert => sto::Mutation::Insert,
                    Mutation::Remove => sto::Mutation::Remove,
                };
                if !participant
                    .mutate(
                        &table.tree,
                        table.log_id,
                        mutation,
                        &write.key,
                        &write.value,
                        false,
                    )
                    .map_err(engine)?
                {
                    return Err(Error::new(
                        ErrorCode::Conflict,
                        "remote insert/delete precondition failed",
                    ));
                }
            }
            Ok(Value::Done)
        }
        Action::Begin => Ok(Value::Done),
        Action::Timestamp | Action::Validate | Action::Install { .. } => Err(Error::new(
            ErrorCode::Invalid,
            "use the OCC prepare/decision protocol",
        )),
        Action::Abort => unreachable!("abort handled before borrowing participant"),
    }
}
