use crate::{
    config::ClientConfig,
    rpc::Connection,
    wire::{Error, ErrorCode, Handle, Operation, Position, Request, RequestId, Response, Result},
};
use mako_sharding::routing::Snapshot;
use srpc::reactor::PollThread;
use std::{
    collections::HashMap,
    sync::Arc,
    time::{Duration, Instant},
};

/// A synchronous client belongs to an application thread; SRPC's poll thread is
/// independent. Calls never replay application operations after a lost reply.
pub struct Client {
    config: Arc<ClientConfig>,
    poll: Arc<PollThread>,
    connections: HashMap<String, Connection>,
    snapshot: Arc<Snapshot>,
    leaders: HashMap<u64, String>,
    timeout: Duration,
    owns_poll: bool,
}
#[derive(Clone, Debug)]
pub struct Transaction {
    pub handle: Handle,
    pub request: RequestId,
    pub term: u64,
    pub position: Position,
    endpoint: String,
}
impl Client {
    pub fn new(config: Arc<ClientConfig>) -> Self {
        Self::with_runtime(config, PollThread::create(), true)
    }
    pub fn with_poll(config: Arc<ClientConfig>, poll: Arc<PollThread>) -> Self {
        Self::with_runtime(config, poll, false)
    }
    fn with_runtime(config: Arc<ClientConfig>, poll: Arc<PollThread>, owns_poll: bool) -> Self {
        let timeout = Duration::from_millis(config.limits.request_timeout_ms);
        let snapshot = config.snapshot();
        Self {
            config,
            snapshot,
            poll,
            connections: HashMap::new(),
            leaders: HashMap::new(),
            timeout,
            owns_poll,
        }
    }
    pub fn set_timeout(&mut self, timeout: Duration) {
        self.timeout = timeout;
    }
    pub fn call_endpoint(&mut self, endpoint: &str, request: Request) -> Result<Response> {
        if !self.connections.contains_key(endpoint) {
            let connection = Connection::connect(endpoint, self.poll.clone())?;
            self.connections.insert(endpoint.to_owned(), connection);
        }
        let result =
            self.connections[endpoint].call(request, self.timeout, self.config.limits.rpc_bytes);
        if result
            .as_ref()
            .is_err_and(|e| matches!(e.code, ErrorCode::Transport | ErrorCode::UnknownOutcome))
        {
            self.connections.remove(endpoint);
        }
        result
    }
    pub fn discover(&mut self, group: u64) -> Result<String> {
        let config = self.config.clone();
        let peers = &config
            .routing
            .shards
            .iter()
            .find(|g| u64::from(g.id) == group)
            .ok_or_else(|| Error::new(ErrorCode::UnknownGroup, "group missing from client config"))?
            .peers;
        let cached = self.leaders.get(&group).cloned();
        if let Some(address) = &cached {
            if let Ok(Response::Status {
                is_leader: true,
                failure: None,
                ..
            }) = self.call_endpoint(address, Request::Status { group })
            {
                return Ok(address.clone());
            }
        }
        for peer in peers {
            if cached.as_deref() == Some(peer.client.as_str()) {
                continue;
            }
            if let Ok(Response::Status {
                is_leader: true,
                failure: None,
                ..
            }) = self.call_endpoint(&peer.client, Request::Status { group })
            {
                self.leaders.insert(group, peer.client.clone());
                return Ok(peer.client.clone());
            }
        }
        self.leaders.remove(&group);
        Err(Error::new(
            ErrorCode::NotLeader,
            "no healthy group leader discovered",
        ))
    }
    pub fn wait_for_leader(&mut self, group: u64, timeout: Duration) -> Result<String> {
        let deadline = Instant::now() + timeout;
        loop {
            match self.discover(group) {
                Ok(address) => return Ok(address),
                Err(error) if Instant::now() >= deadline => return Err(error),
                Err(_) => std::thread::sleep(Duration::from_millis(50)),
            }
        }
    }
    pub fn begin(&mut self, group: u64, request: RequestId) -> Result<Transaction> {
        let endpoint = self.discover(group)?;
        match self.call_endpoint(&endpoint, Request::Begin { group, request })? {
            Response::Begun {
                handle,
                term,
                position,
            } => Ok(Transaction {
                handle,
                request,
                term,
                position,
                endpoint,
            }),
            _ => Err(Error::new(ErrorCode::Transport, "invalid Begin response")),
        }
    }
    pub fn begin_key(
        &mut self,
        table: &str,
        key: &[u8],
        request: RequestId,
    ) -> Result<Transaction> {
        let table = self.config.route_id(table)?;
        let route = self
            .snapshot
            .capture(table, key)
            .ok_or_else(|| Error::new(ErrorCode::UnknownTable, "routing table absent"))?;
        self.begin(u64::from(route.grant.owner), request)
    }
    pub fn execute(
        &mut self,
        transaction: &Transaction,
        group: u64,
        table: &str,
        operation: Operation,
    ) -> Result<Response> {
        self.call_endpoint(
            &transaction.endpoint,
            Request::Execute {
                handle: transaction.handle,
                group,
                table: table.into(),
                operation,
            },
        )
    }
    pub fn get(
        &mut self,
        transaction: &Transaction,
        table: &str,
        key: &[u8],
    ) -> Result<Option<Vec<u8>>> {
        match self.execute(
            transaction,
            transaction.handle.group,
            table,
            Operation::Get { key: key.to_vec() },
        )? {
            Response::Value { value } => Ok(value),
            _ => Err(Error::new(ErrorCode::Transport, "invalid Get response")),
        }
    }
    pub fn put(
        &mut self,
        transaction: &Transaction,
        table: &str,
        key: &[u8],
        value: &[u8],
    ) -> Result<()> {
        self.execute(
            transaction,
            transaction.handle.group,
            table,
            Operation::Put {
                key: key.to_vec(),
                value: value.to_vec(),
            },
        )
        .map(|_| ())
    }
    pub fn commit(&mut self, transaction: Transaction) -> Result<Response> {
        self.call_endpoint(
            &transaction.endpoint,
            Request::Commit {
                handle: transaction.handle,
            },
        )
    }
    pub fn rollback(&mut self, transaction: Transaction) -> Result<()> {
        self.call_endpoint(
            &transaction.endpoint,
            Request::Rollback {
                handle: transaction.handle,
            },
        )
        .map(|_| ())
    }
    pub fn group_request(&mut self, request: Request) -> Result<Response> {
        let endpoint = self.discover(request.group())?;
        self.call_endpoint(&endpoint, request)
    }
    pub fn create_table(&mut self, group: u64, request: RequestId, name: &str) -> Result<u64> {
        self.group_request(Request::CreateTable {
            group,
            request,
            name: name.into(),
        })?;
        match self.group_request(Request::LookupTable {
            group,
            name: name.into(),
        })? {
            Response::Table { table: Some(table) } => Ok(table),
            _ => Err(Error::new(
                ErrorCode::UnknownTable,
                "created table is not visible",
            )),
        }
    }
    /// `position: None` is unresolved/not observed, NEVER a proof of abort.
    pub fn outcome(&mut self, group: u64, request: RequestId) -> Result<Response> {
        self.group_request(Request::Outcome { group, request })
    }
}
impl Drop for Client {
    fn drop(&mut self) {
        self.connections.clear();
        if self.owns_poll {
            self.poll.shutdown();
        }
    }
}

/// JSON/C consumers retain opaque transaction handles here, not STO frames.
/// A handle never migrates to a newly discovered leader.
pub struct Session {
    pub client: Client,
    transactions: HashMap<Handle, Transaction>,
    limit: usize,
}
impl Session {
    pub fn new(config: Arc<ClientConfig>) -> Self {
        let limit = config.context_capacity;
        Self {
            client: Client::new(config),
            transactions: HashMap::new(),
            limit,
        }
    }
    pub fn request(&mut self, request: Request) -> Result<Response> {
        match request {
            Request::Begin { group, request } => {
                if self.transactions.len() >= self.limit {
                    return Err(Error::new(
                        ErrorCode::Busy,
                        "client context capacity exhausted",
                    ));
                }
                let transaction = self.client.begin(group, request)?;
                let response = Response::Begun {
                    handle: transaction.handle,
                    term: transaction.term,
                    position: transaction.position,
                };
                self.transactions.insert(transaction.handle, transaction);
                Ok(response)
            }
            Request::Execute {
                handle,
                group,
                table,
                operation,
            } => {
                let transaction = self.transactions.get(&handle).ok_or_else(|| {
                    Error::new(ErrorCode::InvalidHandle, "client does not own handle")
                })?;
                self.client.execute(transaction, group, &table, operation)
            }
            Request::Commit { handle } => {
                let transaction = self.transactions.remove(&handle).ok_or_else(|| {
                    Error::new(ErrorCode::InvalidHandle, "client does not own handle")
                })?;
                self.client.commit(transaction)
            }
            Request::Rollback { handle } => {
                let transaction = self.transactions.remove(&handle).ok_or_else(|| {
                    Error::new(ErrorCode::InvalidHandle, "client does not own handle")
                })?;
                self.client.rollback(transaction).map(|_| Response::Ok)
            }
            request => self.client.group_request(request),
        }
    }
}
