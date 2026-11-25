//! Distributed OCC coordinator over service-owned participants.
use crate::{
    rpc::Network,
    storage::{Protection, Store},
    wire::*,
};
use mako_protocol::{
    commit::{self, Effects},
    journal::{self, Outcome, Record},
    remote::Remotes,
};
use mako_storage_api as sto;
use std::{collections::BTreeMap, io::Read, sync::Arc, time::Duration};

pub use crate::wire::Target;

pub struct Client {
    network: Arc<Network>,
    endpoints: Arc<BTreeMap<u32, String>>,
    replicas: BTreeMap<u32, ReplicaSet>,
    stream: Stream,
    incarnation: u64,
    attempt: u64,
    term: u32,
    timeout: Duration,
    fenced: bool,
}
impl Client {
    pub fn new(
        network: Arc<Network>,
        endpoints: BTreeMap<u32, String>,
        stream: Stream,
        term: u32,
        timeout: Duration,
    ) -> Result<Self> {
        if term >= 10 || timeout.is_zero() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid term or RPC deadline",
            ));
        }
        let mut nonce = [0; 8];
        std::fs::File::open("/dev/urandom")
            .and_then(|mut file| file.read_exact(&mut nonce))
            .map_err(|e| Error::new(ErrorCode::Fatal, format!("client incarnation: {e}")))?;
        let incarnation = u64::from_ne_bytes(nonce).max(1);
        Ok(Self {
            network,
            endpoints: Arc::new(endpoints),
            replicas: BTreeMap::new(),
            stream,
            incarnation,
            attempt: 0,
            term,
            timeout,
            fenced: false,
        })
    }
    /// Identity of the last begin attempt, including an uncertain begin failure.
    pub fn last_attempt_id(&self) -> journal::TransactionId {
        journal::TransactionId {
            coordinator: self.stream.node,
            worker: self.stream.worker,
            incarnation: self.incarnation,
            attempt: self.attempt,
        }
    }
    pub fn is_fenced(&self) -> bool {
        self.fenced
    }
    pub fn set_replicas(&mut self, owner: u32, replicas: ReplicaSet) -> Result<()> {
        if replicas.endpoints.is_empty() {
            return Err(Error::new(ErrorCode::Invalid, "empty replica group"));
        }
        self.replicas.insert(owner, replicas);
        Ok(())
    }
    /// Resolve the saved attempt without executing its body again. A missing
    /// registration remains Unknown. An available authority can choose Abort
    /// only while Pending; a saved Commit is completed at every participant.
    pub fn resolve(&mut self, store: Arc<Store>, id: journal::TransactionId) -> Result<Outcome> {
        if id.coordinator != store.owner
            || id.coordinator != self.stream.node
            || id.worker != self.stream.worker
            || id.incarnation != self.incarnation
            || id.attempt != self.attempt
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "transaction does not identify this client's last attempt",
            ));
        }
        let outcome = resolve_transaction(self.network.clone(), store, id, self.timeout)?;
        self.fenced = false;
        Ok(outcome)
    }
    pub fn begin<'a, 'worker>(
        &'a mut self,
        store: Arc<Store>,
        worker: &'worker mut sto::Worker,
    ) -> Result<Transaction<'a, 'worker>> {
        if self.fenced {
            return Err(Error::new(
                ErrorCode::Unknown,
                "client has an unresolved outcome or remote cleanup",
            ));
        }
        if store.owner != self.stream.node {
            return Err(Error::new(
                ErrorCode::Invalid,
                "client stream belongs to another shard",
            ));
        }
        if !worker.belongs_to(&store.database) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "worker belongs to another database",
            ));
        }
        self.attempt = self
            .attempt
            .checked_add(1)
            .ok_or_else(|| Error::new(ErrorCode::Capacity, "transaction attempt exhausted"))?;
        let protection = store.protect()?;
        let service = crate::receiver::ParticipantService::for_store(&store, self.network.clone())?;
        let endpoint = service.endpoint(&store)?;
        let transport = Arc::new(crate::receiver::LocalTransport {
            service,
            network: self.network.clone(),
            endpoint: endpoint.clone(),
            owner: store.owner,
        });
        let mut endpoints = (*self.endpoints).clone();
        endpoints.insert(store.owner, endpoint);
        let mut remotes = Remotes::new(
            transport,
            Arc::new(endpoints),
            self.replicas.clone(),
            self.stream,
            Identity {
                incarnation: self.incarnation,
                attempt: self.attempt,
                sequence: 0,
                term: self.term,
            },
            store.log.clone(),
            self.timeout,
        );
        if let Err(error) = remotes.call(store.owner, Action::Begin) {
            if remotes.abort().is_err() {
                self.fenced = true;
            }
            return Err(error);
        }
        Ok(Transaction {
            worker: std::marker::PhantomData,
            finished: false,
            remotes,
            store,
            protection: Some(protection),
            client_fenced: &mut self.fenced,
            failed: None,
            receipt: None,
            retain_snapshot: false,
        })
    }
    /// A body error runs cleanup before returning. Unknown outcomes and failed
    /// cleanup fence this stream; callers cannot retry it.
    pub fn run<T>(
        &mut self,
        store: Arc<Store>,
        worker: &mut sto::Worker,
        body: impl FnOnce(&mut Transaction<'_, '_>) -> Result<T>,
    ) -> Result<T> {
        let mut transaction = self.begin(store, worker)?;
        match body(&mut transaction) {
            Ok(value) => {
                transaction.commit()?;
                Ok(value)
            }
            Err(error) => {
                transaction.abort()?;
                Err(error)
            }
        }
    }
}

/// Complete a saved coordinator decision without the original Client object.
/// Callers must not resolve an attempt concurrently with its live body/commit.
/// A missing registration remains Unknown and is never treated as an abort.
pub fn resolve_transaction(
    network: Arc<Network>,
    store: Arc<Store>,
    id: journal::TransactionId,
    timeout: Duration,
) -> Result<Outcome> {
    use mako_protocol::contracts::Transport;
    if id.coordinator != store.owner || timeout.is_zero() {
        return Err(Error::new(
            ErrorCode::Invalid,
            "wrong coordinator or deadline",
        ));
    }
    let service = crate::receiver::ParticipantService::for_store(&store, network.clone())?;
    service.recover()?;
    let transaction = store
        .journal
        .lookup(id)?
        .ok_or_else(|| Error::new(ErrorCode::Unknown, "transaction registration unavailable"))?
        .transaction;
    let outcome = crate::prepared::local_decision(&store, &transaction, true)?
        .ok_or_else(|| Error::new(ErrorCode::Unknown, "decision is unavailable"))?;
    let endpoint = service.endpoint(&store)?;
    let transport = crate::receiver::LocalTransport {
        service,
        network: network.clone(),
        endpoint: endpoint.clone(),
        owner: store.owner,
    };
    let mut failure = None;
    for member in &transaction.members {
        let endpoints = if member.owner == store.owner {
            vec![endpoint.clone()]
        } else {
            member.endpoints.clone()
        };
        let mut acknowledged = false;
        let mut last = Error::new(ErrorCode::Unknown, "participant is unavailable");
        for address in endpoints {
            let message = Message::Transaction(TransactionRequest {
                id,
                lease: member.lease,
                term: transaction.time_term % 10,
                action: TransactionAction::Decide {
                    transaction: transaction.clone(),
                    outcome,
                },
            });
            match transport.call(&address, message, timeout) {
                Ok(Reply::Transaction(reply)) if reply.id == id && reply.owner == member.owner => {
                    match reply.result {
                        Ok(TransactionValue::Completed(saved)) if saved == outcome => {
                            acknowledged = true;
                            break;
                        }
                        Err(error) => last = error,
                        _ => last = Error::new(ErrorCode::Unknown, "completion reply mismatch"),
                    }
                }
                Ok(Reply::Error(error)) | Err(error) => last = error,
                _ => last = Error::new(ErrorCode::Unknown, "completion reply identity mismatch"),
            }
        }
        if !acknowledged {
            failure.get_or_insert(last);
        }
    }
    if let Some(error) = failure {
        return Err(Error::new(
            ErrorCode::Unknown,
            format!("saved {outcome:?}; completion pending: {error}"),
        ));
    }
    Ok(outcome)
}

pub struct Transaction<'client, 'worker> {
    worker: std::marker::PhantomData<&'worker mut sto::Worker>,
    finished: bool,
    remotes: Remotes,
    store: Arc<Store>,
    protection: Option<Protection>,
    client_fenced: &'client mut bool,
    // A failed operation cannot be ignored to publish a partial transaction.
    failed: Option<Error>,
    receipt: Option<GatewayReceipt>,
    retain_snapshot: bool,
}
impl Transaction<'_, '_> {
    fn routed_target(
        &self,
        schema: &crate::migration::Schema,
        table: TableKey,
        key: &[u8],
    ) -> Result<Target> {
        let mut target = schema.target(table, key, self.store.owner, false)?;
        let spec = schema.spec(table.table)?;
        if self.store.migration && spec.kind == crate::migration::Kind::Governed {
            let warehouse = table.warehouse.to_be_bytes();
            let coordinate = if spec.warehouse {
                warehouse.as_slice()
            } else {
                key
            };
            let route = self
                .protection
                .as_ref()
                .expect("transaction epoch")
                .captured_route(table.table, coordinate)?;
            target.owner = route.owner;
            target.access.route = Some(route);
        }
        Ok(target)
    }
    /// Point access uses the same captured routing epoch as routed scans.
    pub fn get_routed(
        &mut self,
        schema: &crate::migration::Schema,
        table: TableKey,
        key: &[u8],
    ) -> Result<Option<Vec<u8>>> {
        self.attempt(|this| {
            let target = this.routed_target(schema, table, key)?;
            this.get(&target, key)
        })
    }
    pub fn mutate_routed(
        &mut self,
        schema: &crate::migration::Schema,
        table: TableKey,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
    ) -> Result<()> {
        self.attempt(|this| {
            let target = this.routed_target(schema, table, key)?;
            this.mutate(&target, mutation, key, value)
        })
    }
    /// Scan all captured routing segments in key order. The existing checked
    /// cursor/page core bounds each RPC and verifies order, EOF and snapshot
    /// provenance before rows are delivered. Limit applies across all segments.
    pub fn scan_routed(
        &mut self,
        schema: &crate::migration::Schema,
        table: TableKey,
        lower: Bound,
        upper: Bound,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        self.attempt(|this| {
            if limit == 0 {
                return Ok(Vec::new());
            }
            let (lower, upper) = crate::scan::bounds(lower, upper);
            if upper.as_ref().is_some_and(|upper| lower >= *upper) {
                return Ok(Vec::new());
            }
            let log_id = schema.log_id(table)?;
            let spec = schema.spec(table.table)?;
            let governed = this.store.migration && spec.kind == crate::migration::Kind::Governed;
            let mut rows = Vec::new();
            if governed && !spec.warehouse {
                let segments = this
                    .protection
                    .as_ref()
                    .expect("transaction epoch")
                    .segments(table.table, &lower, upper.as_deref(), reverse)?;
                for segment in segments {
                    let target = Target {
                        owner: segment.route.owner,
                        access: Access {
                            table,
                            route: Some(segment.route),
                        },
                        log_id,
                    };
                    this.scan_pages(
                        &target,
                        segment.lower,
                        segment.upper,
                        reverse,
                        &mut rows,
                        limit,
                    )?;
                    if rows.len() == limit {
                        break;
                    }
                }
            } else {
                let mut target = schema.target(table, &lower, this.store.owner, false)?;
                if governed {
                    let route = this
                        .protection
                        .as_ref()
                        .expect("transaction epoch")
                        .captured_route(table.table, &table.warehouse.to_be_bytes())?;
                    target.owner = route.owner;
                    target.access.route = Some(route);
                }
                this.scan_pages(&target, lower, upper, reverse, &mut rows, limit)?;
            }
            Ok(rows)
        })
    }
    fn scan_pages(
        &mut self,
        target: &Target,
        lower: Vec<u8>,
        upper: Option<Vec<u8>>,
        reverse: bool,
        rows: &mut Vec<Row>,
        limit: usize,
    ) -> Result<()> {
        let identity = self.remotes.remote_identity(target.owner)?;
        let mut driver = crate::scan::Driver::new(target, identity, lower, upper, reverse)?;
        loop {
            let request = driver.request()?;
            let response = self.remotes.call(
                target.owner,
                Action::ScanPage {
                    access: target.access.clone(),
                    request,
                },
            )?;
            let Value::Page { bytes, .. } = response else {
                return Err(Error::new(ErrorCode::Transport, "missing scan page"));
            };
            if driver.consume(&bytes, rows, limit)? {
                return Ok(());
            }
        }
    }

    fn attempt<T>(&mut self, operation: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        if let Some(error) = &self.failed {
            return Err(error.clone());
        }
        let result = self
            .protection
            .as_ref()
            .expect("transaction epoch")
            .check()
            .and_then(|()| operation(self));
        if let Err(error) = &result {
            self.failed = Some(error.clone());
        }
        result
    }
    pub fn id(&self) -> journal::TransactionId {
        self.remotes.transaction_id()
    }
    pub(crate) fn gateway_receipt(&mut self, receipt: GatewayReceipt) {
        self.receipt = Some(receipt);
    }
    pub fn get(&mut self, target: &Target, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.attempt(|this| {
            if key.len() > MAX_KEY {
                return Err(Error::new(
                    ErrorCode::Capacity,
                    "key exceeds protocol limit",
                ));
            }
            match this.remotes.call(
                target.owner,
                Action::Get {
                    access: target.access.clone(),
                    key: key.to_vec(),
                },
            )? {
                Value::Point { value, .. } => Ok(value),
                _ => Err(Error::new(
                    ErrorCode::Transport,
                    "missing participant read result",
                )),
            }
        })
    }
    pub fn mutate(
        &mut self,
        target: &Target,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
    ) -> Result<()> {
        self.attempt(|this| {
            if key.len() > MAX_KEY || value.len() > MAX_VALUE {
                return Err(Error::new(
                    ErrorCode::Capacity,
                    "mutation exceeds protocol limits",
                ));
            }
            this.remotes.bind_target(target, key)?;
            this.remotes
                .stage(target.owner, target.log_id, mutation, key, value)
        })
    }
    pub fn scan(
        &mut self,
        target: &Target,
        lower: Bound,
        upper: Bound,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        self.attempt(|this| {
            let action = Action::Scan {
                access: target.access.clone(),
                lower,
                upper,
                reverse,
                limit,
            };
            if !action.valid() {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "invalid scan bounds or row limit",
                ));
            }
            match this.remotes.call(target.owner, action)? {
                Value::Scan { rows, .. } => Ok(rows),
                _ => Err(Error::new(
                    ErrorCode::Transport,
                    "missing participant scan result",
                )),
            }
        })
    }
    pub fn commit(mut self) -> Result<()> {
        if let Err(error) = self.protection.as_ref().expect("transaction epoch").check() {
            self.failed.get_or_insert(error);
        }
        if let Some(error) = self.failed.take() {
            self.abort_inner()?;
            return Err(error);
        }
        self.finished = true;
        let owners = self.remotes.owners();
        let mut effects = NativeEffects {
            remotes: &mut self.remotes,
            store: &self.store,
            owners,
            proposal: 0,
            durable: None,
            receipt: self.receipt.take(),
            registered: false,
        };
        let result = match commit::coordinate(&mut effects) {
            Ok(completion) if completion.outcome == Outcome::Commit && completion.acknowledged => {
                Ok(())
            }
            Ok(completion) if completion.outcome == Outcome::Abort && completion.acknowledged => {
                Err(completion
                    .failure
                    .filter(|error| {
                        matches!(
                            error.code,
                            ErrorCode::Conflict
                                | ErrorCode::RouteChanged
                                | ErrorCode::Busy
                                | ErrorCode::Capacity
                        )
                    })
                    .unwrap_or_else(|| {
                        Error::new(ErrorCode::Conflict, "transaction aborted by coordinator")
                    }))
            }
            Ok(completion) => Err(Error::new(
                ErrorCode::Unknown,
                format!(
                    "transaction {:?} has saved {:?}; completion is pending: {:?}",
                    completion.transaction.id, completion.outcome, completion.delivery_failure
                ),
            )),
            Err(error) => Err(error),
        };
        if matches!(&result, Err(error) if !matches!(error.code, ErrorCode::Conflict | ErrorCode::RouteChanged | ErrorCode::Busy | ErrorCode::Capacity | ErrorCode::Invalid))
        {
            *self.client_fenced = true;
            self.retain_snapshot = !effects.registered;
        }
        result
    }
    pub fn abort(mut self) -> Result<()> {
        self.abort_inner()
    }
    fn abort_inner(&mut self) -> Result<()> {
        if self.finished {
            return Ok(());
        }
        self.finished = true;
        self.remotes.abort().map_err(|error| {
            *self.client_fenced = true;
            self.retain_snapshot = true;
            Error::new(
                ErrorCode::Unknown,
                format!("transaction cleanup was not confirmed: {error}"),
            )
        })
    }
}
impl Drop for Transaction<'_, '_> {
    fn drop(&mut self) {
        let _ = self.abort_inner();
        if self.retain_snapshot {
            if let Some(protection) = self.protection.take() {
                self.store.quarantine(protection);
            }
        }
    }
}

struct NativeEffects<'a> {
    remotes: &'a mut Remotes,
    store: &'a Store,
    owners: Vec<u32>,
    proposal: u32,
    durable: Option<bool>,
    receipt: Option<GatewayReceipt>,
    registered: bool,
}
impl Effects for NativeEffects<'_> {
    fn participants(&self) -> usize {
        self.owners.len()
    }
    fn lock(&mut self, member: usize) -> Result<()> {
        match self
            .remotes
            .transaction_call(self.owners[member], TransactionAction::Lock)?
        {
            TransactionValue::Locked { timestamp, durable } => {
                if self.durable.is_some_and(|expected| expected != durable) {
                    return Err(Error::new(
                        ErrorCode::Invalid,
                        "transaction mixes durable and volatile participants",
                    ));
                }
                self.durable = Some(durable);
                self.proposal = self.proposal.max(timestamp);
                Ok(())
            }
            _ => Err(Error::new(
                ErrorCode::Transport,
                "missing participant lock acknowledgement",
            )),
        }
    }
    fn register(&mut self) -> Result<journal::Transaction> {
        use mako_clock::distributed::{DistributedTerm, DistributedTimeTerm, DistributedTimestamp};
        let timestamp = DistributedTimestamp::new(self.proposal)
            .map_err(|error| Error::new(ErrorCode::Invalid, error.to_string()))?;
        let term = DistributedTerm::new(self.remotes.identity().term)
            .map_err(|error| Error::new(ErrorCode::Invalid, error.to_string()))?;
        let transaction = journal::Transaction {
            version: journal::VERSION,
            id: self.remotes.transaction_id(),
            time_term: DistributedTimeTerm::new(timestamp, term).encode_legacy(),
            durable: self.durable.expect("locked membership"),
            members: self.remotes.members(),
        };
        transaction.validate()?;
        self.store
            .journal
            .apply(&Record::Begin(transaction.clone()))?;
        self.registered = true;
        Ok(transaction)
    }
    fn prepare(&mut self, member: usize, transaction: &journal::Transaction) -> Result<()> {
        let action = if let Some(receipt) = &self.receipt {
            TransactionAction::PrepareGateway {
                transaction: transaction.clone(),
                receipt: receipt.clone(),
            }
        } else {
            TransactionAction::Prepare(transaction.clone())
        };
        match self.remotes.transaction_call(self.owners[member], action)? {
            TransactionValue::Prepared => Ok(()),
            _ => Err(Error::new(
                ErrorCode::Transport,
                "missing participant Yes vote",
            )),
        }
    }
    fn decide(&mut self, transaction: &journal::Transaction, proposed: Outcome) -> Result<Outcome> {
        match self.store.journal.apply(&Record::Decide {
            transaction: transaction.clone(),
            outcome: proposed,
        }) {
            Ok(entry) => entry
                .decision
                .ok_or_else(|| Error::new(ErrorCode::Unknown, "decision was not saved")),
            Err(error) => self
                .store
                .journal
                .lookup(transaction.id)?
                .and_then(|entry| entry.decision)
                .ok_or(error),
        }
    }
    fn complete(
        &mut self,
        member: usize,
        transaction: &journal::Transaction,
        outcome: Outcome,
    ) -> Result<()> {
        match self.remotes.transaction_call(
            self.owners[member],
            TransactionAction::Decide {
                transaction: transaction.clone(),
                outcome,
            },
        )? {
            TransactionValue::Completed(saved) if saved == outcome => Ok(()),
            _ => Err(Error::new(
                ErrorCode::Unknown,
                "missing participant completion acknowledgement",
            )),
        }
    }
    fn abort_unprepared(&mut self, member: usize) -> Result<()> {
        self.remotes
            .call(self.owners[member], Action::Abort)
            .map(|_| ())
    }
}
