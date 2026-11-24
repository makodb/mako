//! Native shard coordinator. STO owns the decision algorithm; these callbacks
//! deliver its phases to retained native SRPC participants.
use crate::{
    rpc::Network,
    storage::{engine, CommitLog, Protection, Store},
    wire::*,
};
use std::{collections::BTreeMap, io::Read, sync::Arc, time::Duration};
use sto_tpcc_ffi::native as sto;

pub use crate::wire::Target;

pub struct Client {
    network: Arc<Network>,
    endpoints: Arc<BTreeMap<u32, String>>,
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
            stream,
            incarnation,
            attempt: 0,
            term,
            timeout,
            fenced: false,
        })
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
        let participant = worker.begin_participant(self.term).map_err(engine)?;
        let remotes = Remotes {
            network: self.network.clone(),
            endpoints: self.endpoints.clone(),
            stream: self.stream,
            identity: Identity {
                incarnation: self.incarnation,
                attempt: self.attempt,
                sequence: 0,
                term: self.term,
            },
            peers: BTreeMap::new(),
            targets: BTreeMap::new(),
            log: store.log.clone(),
            timeout: self.timeout,
        };
        Ok(Transaction {
            participant: Some(participant),
            remotes,
            store,
            protection: Some(protection),
            client_fenced: &mut self.fenced,
            failed: None,
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

struct Peer {
    sequence: u64,
}
struct Remotes {
    network: Arc<Network>,
    endpoints: Arc<BTreeMap<u32, String>>,
    stream: Stream,
    identity: Identity,
    peers: BTreeMap<u32, Peer>,
    targets: BTreeMap<(u32, u16, Vec<u8>), Access>,
    log: Arc<dyn CommitLog>,
    timeout: Duration,
}
impl Remotes {
    fn call(&mut self, owner: u32, action: Action) -> Result<Value> {
        let endpoint = self
            .endpoints
            .get(&owner)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "missing shard endpoint"))?;
        // A lost initial reply may leave a remote session requiring ABORT.
        let peer = self.peers.entry(owner).or_insert(Peer { sequence: 0 });
        peer.sequence = peer.sequence.checked_add(1).ok_or_else(|| {
            Error::new(
                ErrorCode::Capacity,
                "participant request sequence exhausted",
            )
        })?;
        let identity = Identity {
            sequence: peer.sequence,
            ..self.identity
        };
        let reply = self.network.call(
            endpoint,
            Message::Participant(Request {
                stream: self.stream,
                identity,
                action,
            }),
            self.timeout,
        )?;
        match reply {
            Reply::Participant(response) if response.identity == identity => response.result,
            Reply::Error(error) => Err(error),
            _ => Err(Error::new(
                ErrorCode::Transport,
                "reply does not match pending participant identity",
            )),
        }
    }
    fn all(&mut self, action: Action) -> Result<()> {
        let owners: Vec<_> = self.peers.keys().copied().collect();
        let mut failure = None;
        for owner in owners {
            // Send cleanup to every peer even when an earlier ABORT failed.
            if let Err(error) = self.call(owner, action.clone()) {
                failure.get_or_insert(error);
            }
        }
        failure.map_or(Ok(()), Err)
    }
}
fn callback_error(error: Error) -> sto::Error {
    let kind = match error.code {
        ErrorCode::Conflict | ErrorCode::RouteChanged | ErrorCode::Busy => sto::ErrorKind::Conflict,
        ErrorCode::Capacity => sto::ErrorKind::Capacity,
        ErrorCode::Unknown => sto::ErrorKind::UnknownOutcome,
        ErrorCode::CommittedFailure => sto::ErrorKind::CommittedFailure,
        _ => sto::ErrorKind::Fatal,
    };
    sto::Error {
        kind,
        message: error.to_string(),
    }
}
impl sto::ProtocolCallbacks for Remotes {
    fn stage(
        &mut self,
        shard: u32,
        table: u16,
        mutation: sto::Mutation,
        key: &[u8],
        value: &[u8],
    ) -> sto::Result<()> {
        let access = self
            .targets
            .get(&(shard, table, key.to_vec()))
            .cloned()
            .ok_or_else(|| {
                callback_error(Error::new(
                    ErrorCode::Invalid,
                    "remote table was not resolved",
                ))
            })?;
        let mutation = match mutation {
            sto::Mutation::Put => Mutation::Put,
            sto::Mutation::Insert => Mutation::Insert,
            sto::Mutation::Remove => Mutation::Remove,
        };
        self.call(
            shard,
            Action::Stage {
                writes: vec![Write {
                    access,
                    mutation,
                    key: key.to_vec(),
                    value: value.to_vec(),
                }],
            },
        )
        .map(|_| ())
        .map_err(callback_error)
    }
    fn timestamp(&mut self) -> sto::Result<u32> {
        let mut maximum = 0;
        for owner in self.peers.keys().copied().collect::<Vec<_>>() {
            match self
                .call(owner, Action::Timestamp)
                .map_err(callback_error)?
            {
                Value::Timestamp(timestamp) => maximum = maximum.max(timestamp),
                _ => {
                    return Err(callback_error(Error::new(
                        ErrorCode::Transport,
                        "missing participant timestamp",
                    )))
                }
            }
        }
        Ok(maximum)
    }
    fn validate(&mut self) -> sto::Result<()> {
        self.all(Action::Validate).map_err(callback_error)
    }
    fn install(&mut self, timestamp: u32) -> sto::Result<()> {
        self.all(Action::Install { timestamp })
            .map_err(callback_error)
    }
    fn abort(&mut self) -> sto::Result<()> {
        self.all(Action::Abort).map_err(callback_error)
    }
    fn log(&mut self, bytes: &[u8], time_term: u32) -> sto::Result<()> {
        self.log.append(bytes, time_term).map_err(callback_error)
    }
}

pub struct Transaction<'client, 'worker> {
    participant: Option<sto::Participant<'worker>>,
    remotes: Remotes,
    store: Arc<Store>,
    protection: Option<Protection>,
    client_fenced: &'client mut bool,
    // A failed operation cannot be ignored to publish a partial transaction.
    failed: Option<Error>,
}
impl Transaction<'_, '_> {
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
        let identity = self.remotes.identity;
        let mut driver = crate::scan::Driver::new(target, identity, lower, upper, reverse)?;
        loop {
            let request = driver.request()?;
            let participant = self.participant.as_mut().expect("live transaction");
            let response = if target.owner == self.store.owner {
                crate::scan::produce(
                    &self.store,
                    self.protection.as_ref().expect("transaction epoch"),
                    participant,
                    identity,
                    &target.access,
                    &request,
                )?
            } else {
                participant
                    .flush_remote(&mut self.remotes)
                    .map_err(engine)?;
                self.remotes.call(
                    target.owner,
                    Action::ScanPage {
                        access: target.access.clone(),
                        request,
                    },
                )?
            };
            let Value::Page {
                bytes,
                read_dependency,
            } = response
            else {
                return Err(Error::new(ErrorCode::Transport, "missing scan page"));
            };
            if target.owner != self.store.owner {
                participant
                    .observe_remote_scan(target.owner, read_dependency)
                    .map_err(engine)?;
            }
            if driver.consume(&bytes, rows, limit)? {
                return Ok(());
            }
        }
    }

    fn attempt<T>(&mut self, operation: impl FnOnce(&mut Self) -> Result<T>) -> Result<T> {
        if let Some(error) = &self.failed {
            return Err(error.clone());
        }
        let result = operation(self);
        if let Err(error) = &result {
            self.failed = Some(error.clone());
        }
        result
    }
    pub fn get(&mut self, target: &Target, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.attempt(|this| {
            if key.len() > MAX_KEY {
                return Err(Error::new(
                    ErrorCode::Capacity,
                    "key exceeds protocol limit",
                ));
            }
            let participant = this.participant.as_mut().expect("live transaction");
            if target.owner == this.store.owner {
                this.protection
                    .as_ref()
                    .expect("transaction epoch")
                    .point(&target.access, key)?;
                let table = this.store.table(target.access.table)?;
                return participant
                    .get(&table.tree, key)
                    .map(|v| v.value)
                    .map_err(engine);
            }
            participant
                .flush_remote(&mut this.remotes)
                .map_err(engine)?;
            let result = this.remotes.call(
                target.owner,
                Action::Get {
                    access: target.access.clone(),
                    key: key.to_vec(),
                },
            )?;
            match result {
                Value::Point { value, time_term } => {
                    participant
                        .observe_remote_read(target.owner, time_term)
                        .map_err(engine)?;
                    Ok(value)
                }
                _ => Err(Error::new(
                    ErrorCode::Transport,
                    "missing remote read result",
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
            let participant = this.participant.as_mut().expect("live transaction");
            let mutation = match mutation {
                Mutation::Put => sto::Mutation::Put,
                Mutation::Insert => sto::Mutation::Insert,
                Mutation::Remove => sto::Mutation::Remove,
            };
            if target.owner == this.store.owner {
                this.protection
                    .as_ref()
                    .expect("transaction epoch")
                    .point(&target.access, key)?;
                let table = this.store.table(target.access.table)?;
                if !participant
                    .mutate(&table.tree, table.log_id, mutation, key, value, false)
                    .map_err(engine)?
                {
                    return Err(Error::new(
                        ErrorCode::Conflict,
                        "insert/delete precondition failed",
                    ));
                }
            } else {
                if this.remotes.targets.iter().any(|((owner, id, _), access)| {
                    *owner == target.owner
                        && *id == target.log_id
                        && access.table != target.access.table
                }) {
                    return Err(Error::new(
                        ErrorCode::Invalid,
                        "remote log ID identifies two physical tables",
                    ));
                }
                let slot = this
                    .remotes
                    .targets
                    .entry((target.owner, target.log_id, key.to_vec()))
                    .or_insert_with(|| target.access.clone());
                if slot != &target.access {
                    return Err(Error::new(
                        ErrorCode::RouteChanged,
                        "remote table route changed during transaction",
                    ));
                }
                participant
                    .stage_remote(target.owner, target.log_id, mutation, key, value)
                    .map_err(engine)?;
            }
            Ok(())
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
            if !(Action::Scan {
                access: target.access.clone(),
                lower: lower.clone(),
                upper: upper.clone(),
                reverse,
                limit,
            })
            .valid()
            {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "invalid scan bounds or row limit",
                ));
            }
            let participant = this.participant.as_mut().expect("live transaction");
            if target.owner == this.store.owner {
                this.protection.as_ref().expect("transaction epoch").range(
                    &target.access,
                    &lower,
                    &upper,
                )?;
                let table = this.store.table(target.access.table)?;
                let result = participant
                    .scan(&table.tree, lower.as_ref(), upper.as_ref(), reverse, limit)
                    .map_err(engine)?;
                Ok(result
                    .rows
                    .into_iter()
                    .map(|r| Row {
                        key: r.key,
                        value: r.value,
                        time_term: r.time_term,
                    })
                    .collect())
            } else {
                participant
                    .flush_remote(&mut this.remotes)
                    .map_err(engine)?;
                match this.remotes.call(
                    target.owner,
                    Action::Scan {
                        access: target.access.clone(),
                        lower,
                        upper,
                        reverse,
                        limit,
                    },
                )? {
                    Value::Scan {
                        rows,
                        read_dependency,
                    } => {
                        participant
                            .observe_remote_scan(target.owner, read_dependency)
                            .map_err(engine)?;
                        Ok(rows)
                    }
                    _ => Err(Error::new(
                        ErrorCode::Transport,
                        "missing remote scan result",
                    )),
                }
            }
        })
    }
    pub fn commit(mut self) -> Result<()> {
        if let Some(error) = self.failed.take() {
            self.abort_inner()?;
            return Err(error);
        }
        let result = self
            .participant
            .take()
            .expect("live transaction")
            .commit(0, &mut self.remotes)
            .map(|_| ())
            .map_err(engine);
        if matches!(&result, Err(e) if !matches!(e.code, ErrorCode::Conflict | ErrorCode::RouteChanged | ErrorCode::Busy | ErrorCode::Capacity))
        {
            *self.client_fenced = true;
        }
        result
    }
    pub fn abort(mut self) -> Result<()> {
        self.abort_inner()
    }
    fn abort_inner(&mut self) -> Result<()> {
        let result = self.participant.take().map_or(Ok(()), |participant| {
            participant.abort(&mut self.remotes).map_err(engine)
        });
        if result.is_err() {
            *self.client_fenced = true;
        }
        result
    }
}
impl Drop for Transaction<'_, '_> {
    fn drop(&mut self) {
        let _ = self.abort_inner();
        // An uncertain local coordinator must keep migration frozen too.
        if *self.client_fenced {
            if let Some(pin) = self.protection.take() {
                self.store.quarantine(pin);
            }
        }
    }
}
