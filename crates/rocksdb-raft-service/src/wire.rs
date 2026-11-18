use serde::{Deserialize, Serialize};

pub const RPC_ID: i32 = 0x5264_0001;
pub const PROTOCOL_VERSION: u32 = 1;
pub const MAX_WIRE_BYTES: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct RequestId { pub client: u64, pub sequence: u64 }
impl From<RequestId> for sto_rocksdb::RequestId {
    fn from(id: RequestId) -> Self { Self { client: id.client, sequence: id.sequence } }
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct Position { pub index: u64, pub term: u64 }
impl From<raft_host::Position> for Position {
    fn from(p: raft_host::Position) -> Self { Self { index: p.index, term: p.term } }
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, Hash)]
pub struct Handle { pub group: u64, pub lane: usize, pub nonce: u64 }

#[cfg(feature = "test-hooks")]
#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum TestStage { BeforeProposal, AfterAppend, BeforeApply, AfterApply, BeforeReply }

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
pub enum Operation {
    Get { key: Vec<u8> },
    Exists { key: Vec<u8> },
    Put { key: Vec<u8>, value: Vec<u8> },
    Insert { key: Vec<u8>, value: Vec<u8> },
    Delete { key: Vec<u8> },
    Scan { start: Vec<u8>, end: Option<Vec<u8>>, limit: usize },
    ReverseScan { start: Vec<u8>, end: Option<Vec<u8>>, limit: usize },
}
impl Operation {
    pub fn bytes(&self) -> usize {
        match self {
            Self::Get { key } | Self::Exists { key } | Self::Delete { key } => key.len(),
            Self::Put { key, value } | Self::Insert { key, value } => key.len().saturating_add(value.len()),
            Self::Scan { start, end, .. } | Self::ReverseScan { start, end, .. } => start.len().saturating_add(end.as_ref().map_or(0, Vec::len)),
        }
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Request {
    Status { group: u64 },
    Begin { group: u64, request: RequestId },
    Execute { handle: Handle, group: u64, table: String, operation: Operation },
    Commit { handle: Handle },
    Rollback { handle: Handle },
    CreateTable { group: u64, request: RequestId, name: String },
    ListTables { group: u64 },
    LookupTable { group: u64, name: String },
    Count { group: u64, name: String },
    Outcome { group: u64, request: RequestId },
    Checkpoint { group: u64 },
    #[cfg(feature = "test-hooks")]
    InjectApplyFailure { group: u64, after_batch: bool },
    #[cfg(feature = "test-hooks")]
    TestGate { group: u64, stage: TestStage, paused: Option<bool> },
    #[cfg(feature = "test-hooks")]
    TestIsolate { group: u64, isolated: bool },
    #[cfg(feature = "test-hooks")]
    TestLocalScan { group: u64, table: String, start: Vec<u8>, end: Vec<u8>, limit: usize },
}
impl Request {
    pub fn group(&self) -> u64 {
        match self {
            Self::Commit { handle } | Self::Rollback { handle } | Self::Execute { handle, .. } => handle.group,
            Self::Status { group } | Self::Begin { group, .. } | Self::CreateTable { group, .. }
            | Self::ListTables { group } | Self::LookupTable { group, .. } | Self::Count { group, .. }
            | Self::Outcome { group, .. } | Self::Checkpoint { group } => *group,
            #[cfg(feature = "test-hooks")]
            Self::InjectApplyFailure { group, .. } | Self::TestGate { group, .. } | Self::TestIsolate { group, .. }
            | Self::TestLocalScan { group, .. } => *group,
        }
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Envelope { pub version: u32, pub request: Request }
impl Envelope {
    pub fn new(request: Request) -> Self { Self { version: PROTOCOL_VERSION, request } }
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCode {
    Invalid, TooLarge, Busy, UnknownGroup, UnknownTable, InvalidHandle, CrossShard,
    Conflict, NotLeader, Closed, UnknownOutcome, StorageFailure, Transport,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Error { pub code: ErrorCode, pub message: String }
impl Error {
    pub fn new(code: ErrorCode, message: impl Into<String>) -> Self { Self { code, message: message.into() } }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "{:?}: {}", self.code, self.message) }
}
impl std::error::Error for Error {}
pub type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Response {
    Status { group: u64, node: u16, term: u64, leader: Option<u16>, leader_endpoint: Option<String>, is_leader: bool, committed: u64, applied: u64, failure: Option<String>, durability: crate::config::DurabilityMode, power_crash_durable: bool },
    Begun { handle: Handle, term: u64, position: Position },
    Value { value: Option<Vec<u8>> },
    Exists { exists: bool },
    Rows { rows: Vec<(Vec<u8>, Vec<u8>)> },
    Ok,
    Committed { request: RequestId, position: Position, read_only: bool },
    Tables { tables: Vec<(String, u64)> },
    Table { table: Option<u64> },
    Count { count: u64 },
    Outcome { request: RequestId, position: Option<Position> },
    Checkpoint { position: Position },
    Error { error: Error },
    #[cfg(feature = "test-hooks")]
    TestGate { paused: bool, waiting: usize, position: Option<Position> },
}
impl Response {
    pub fn from_result(result: Result<Self>) -> Self { result.unwrap_or_else(|error| Self::Error { error }) }
    pub fn into_result(self) -> Result<Self> { match self { Self::Error { error } => Err(error), response => Ok(response) } }
}
