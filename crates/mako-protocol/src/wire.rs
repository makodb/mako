use bincode::Options;
use serde::{Deserialize, Serialize};

pub const VERSION: u32 = 2;
pub const RPC_ID: i32 = 0x4d4b5201;
pub const MAX_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_KEY: usize = 1024;
pub const MAX_VALUE: usize = u16::MAX as usize;
pub const MAX_ROWS: usize = 4096;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum ErrorCode {
    Invalid,
    Conflict,
    Busy,
    RouteChanged,
    Capacity,
    Closed,
    Transport,
    Unknown,
    CommittedFailure,
    Fatal,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Error {
    pub code: ErrorCode,
    pub message: String,
}
impl Error {
    pub fn new(code: ErrorCode, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}: {}", self.code, self.message)
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct Identity {
    pub incarnation: u64,
    pub attempt: u64,
    pub sequence: u64,
    pub term: u32,
}
impl Identity {
    pub fn valid(self) -> bool {
        self.incarnation != 0 && self.attempt != 0 && self.sequence != 0 && self.term < 10
    }
    pub fn same_attempt(self, other: Self) -> bool {
        self.incarnation == other.incarnation
            && self.attempt == other.attempt
            && self.term == other.term
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct Stream {
    pub node: u32,
    pub worker: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash, Ord, PartialOrd, Serialize, Deserialize)]
pub struct TableKey {
    pub table: u64,
    pub warehouse: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Route {
    pub owner: u32,
    pub ownership_version: u64,
    pub snapshot_version: u64,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Access {
    pub table: TableKey,
    pub route: Option<Route>,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Target {
    pub owner: u32,
    pub access: Access,
    pub log_id: u16,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct GatewayRequest {
    pub client: u64,
    pub sequence: u64,
    pub session: u64,
    pub kind: u32,
    pub target: Option<Target>,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
    pub forwarded: bool,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct GatewayResponse {
    pub client: u64,
    pub sequence: u64,
    pub snapshot_version: u64,
    pub status: i32,
    pub outcome: u32,
    pub op_result: bool,
    pub value: Vec<u8>,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct GatewayReceipt {
    pub request: GatewayRequest,
    pub response: GatewayResponse,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ReplicaSet {
    pub group: u64,
    pub endpoints: Vec<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ReplicaStatus {
    pub owner: u32,
    pub group: Option<u64>,
    pub leader: bool,
    pub term: u64,
    pub healthy: bool,
    pub durable_receipts: bool,
}
impl GatewayReceipt {
    pub fn validate(&self) -> Result<()> {
        let request = &self.request;
        let response = &self.response;
        if request.client < 1 << 63
            || request.sequence == 0
            || !matches!(request.kind, 23 | 24 | 25 | 28)
            || request.target.is_none()
            || request.key.len() > 64
            || request.value.len() > 8000
            || response.client != request.client
            || response.sequence != request.sequence
            || response.outcome != 1
            || response.value.len() > 8000
            || !(response.status == 0
                || (request.kind == 24 && response.status == 3 && !response.op_result))
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid durable gateway receipt",
            ));
        }
        Ok(())
    }
    pub fn matches(&self, request: &GatewayRequest) -> bool {
        let mut normalized = request.clone();
        normalized.forwarded = false;
        let mut saved = self.request.clone();
        saved.forwarded = false;
        normalized == saved
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RouteRequest {
    pub table: Option<u64>,
    pub name: Option<String>,
    pub warehouse: u32,
    pub key: Vec<u8>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Mutation {
    Put,
    Insert,
    Remove,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Write {
    pub access: Access,
    pub mutation: Mutation,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Bound {
    Unbounded,
    Included(Vec<u8>),
    Excluded(Vec<u8>),
}
impl Bound {
    pub fn as_ref(&self) -> std::ops::Bound<&[u8]> {
        match self {
            Self::Unbounded => std::ops::Bound::Unbounded,
            Self::Included(v) => std::ops::Bound::Included(v),
            Self::Excluded(v) => std::ops::Bound::Excluded(v),
        }
    }
    fn valid(&self) -> bool {
        match self {
            Self::Unbounded => true,
            Self::Included(v) | Self::Excluded(v) => v.len() <= MAX_KEY,
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Action {
    ScanPage {
        access: Access,
        request: Vec<u8>,
    },
    Get {
        access: Access,
        key: Vec<u8>,
    },
    Scan {
        access: Access,
        lower: Bound,
        upper: Bound,
        reverse: bool,
        limit: usize,
    },
    Stage {
        writes: Vec<Write>,
    },
    Timestamp,
    Validate,
    Install {
        timestamp: u32,
    },
    Abort,
    /// Start an empty participant, including the coordinator's local shard.
    Begin,
}
impl Action {
    pub fn valid(&self) -> bool {
        match self {
            Self::ScanPage { request, .. } => {
                request.len() <= mako_sharding::full_scan_core::REQUEST_CAPACITY
            }
            Self::Get { key, .. } => key.len() <= MAX_KEY,
            Self::Scan {
                lower,
                upper,
                limit,
                ..
            } => lower.valid() && upper.valid() && *limit > 0 && *limit <= MAX_ROWS,
            Self::Stage { writes } => {
                !writes.is_empty()
                    && writes.len() <= MAX_ROWS
                    && writes
                        .iter()
                        .all(|w| w.key.len() <= MAX_KEY && w.value.len() <= MAX_VALUE)
            }
            Self::Install { timestamp } => *timestamp > 0,
            _ => true,
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Request {
    pub stream: Stream,
    pub identity: Identity,
    pub action: Action,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum TransactionAction {
    Lock,
    Prepare(crate::journal::Transaction),
    Decide {
        transaction: crate::journal::Transaction,
        outcome: crate::journal::Outcome,
    },
    PrepareGateway {
        transaction: crate::journal::Transaction,
        receipt: GatewayReceipt,
    },
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct TransactionRequest {
    pub id: crate::journal::TransactionId,
    pub lease: u64,
    pub term: u32,
    pub action: TransactionAction,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum TransactionValue {
    Locked { timestamp: u32, durable: bool },
    Prepared,
    Completed(crate::journal::Outcome),
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct TransactionResponse {
    pub id: crate::journal::TransactionId,
    pub owner: u32,
    pub result: Result<TransactionValue>,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Row {
    pub key: Vec<u8>,
    pub value: Vec<u8>,
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum Value {
    Page {
        bytes: Vec<u8>,
        read_dependency: u32,
    },
    Done,
    Point {
        value: Option<Vec<u8>>,
        time_term: u32,
    },
    Scan {
        rows: Vec<Row>,
        read_dependency: u32,
    },
    Timestamp(u32),
}
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Response {
    pub identity: Identity,
    pub result: Result<Value>,
}
impl Response {
    pub fn error(identity: Identity, code: ErrorCode, message: impl Into<String>) -> Self {
        Self {
            identity,
            result: Err(Error::new(code, message)),
        }
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum Message {
    OpenStream {
        stream: Stream,
        nonce: u64,
    },
    ReplicaStatus,
    Gateway(GatewayRequest),
    Route(RouteRequest),
    Participant(Request),
    Migration {
        owner: u32,
        operation: u32,
        payload: Vec<u8>,
    },
    Transaction(TransactionRequest),
    /// Query the coordinator authority. When resolve is true, that authority
    /// may atomically choose Abort if no final decision has been recorded.
    TransactionOutcome {
        transaction: crate::journal::Transaction,
        resolve: bool,
    },
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum Reply {
    OpenStream(Result<u64>),
    ReplicaStatus(ReplicaStatus),
    Gateway(GatewayResponse),
    Route(Result<Target>),
    Participant(Response),
    Migration {
        status: u32,
        payload: Vec<u8>,
    },
    Error(Error),
    Transaction(TransactionResponse),
    TransactionOutcome {
        id: crate::journal::TransactionId,
        owner: u32,
        outcome: Result<Option<crate::journal::Outcome>>,
    },
}

fn codec() -> impl Options {
    bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(MAX_BYTES as u64)
        .reject_trailing_bytes()
}
pub fn encode<T: Serialize>(value: &T) -> Result<Vec<u8>> {
    codec()
        .serialize(&(VERSION, value))
        .map_err(|e| Error::new(ErrorCode::Capacity, e.to_string()))
}
pub fn decode<'a, T: Deserialize<'a>>(bytes: &'a [u8]) -> Result<T> {
    if bytes.len() > MAX_BYTES {
        return Err(Error::new(ErrorCode::Capacity, "RPC byte limit exceeded"));
    }
    let (version, value): (u32, T) = codec()
        .deserialize(bytes)
        .map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?;
    if version != VERSION {
        return Err(Error::new(
            ErrorCode::Invalid,
            "unsupported native application protocol",
        ));
    }
    Ok(value)
}
