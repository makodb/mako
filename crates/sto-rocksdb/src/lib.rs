//! Direct RocksDB datatype for local Rust STO, with decided Raft redo application.
//! Native I/O is deliberately outside the bounded datatype callbacks. Committed
//! values exist only in RocksDB and transaction-local native snapshot leases.
#![deny(unsafe_code)]
mod checkpoint;
mod command;
mod database;
mod directory;
mod layout;
pub use directory::control_path;
pub use layout::META as METADATA_COLUMN_FAMILY;
pub mod distributed;
mod transaction;

pub use checkpoint::Checkpoint;
pub use command::{
    ApplyOutcome, Command, CommandBody, Mutation, Outcome, OutcomeKind, Position, RequestId,
    MAX_COMMAND_BYTES, MAX_KEY_BYTES, MAX_OPERATIONS,
};
#[cfg(feature = "test-hooks")]
pub use database::ApplyFault;
pub use database::{Database, Table};
pub use transaction::{Prepared, Transaction, Worker};

pub type Result<T, E = Error> = std::result::Result<T, E>;
#[derive(Debug)]
pub enum Error {
    Storage(String),
    Corruption(String),
    Core(String),
    Conflict,
    Fenced,
    WrongGroup,
    UnknownTable(u64),
    AlreadyExists,
    TooLarge,
    VersionExhausted,
    InvalidCommand(&'static str),
    RequestReuse,
    AppliedGap { expected: u64, received: u64 },
    InvalidState(&'static str),
}
impl std::fmt::Display for Error {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{self:?}")
    }
}
impl std::error::Error for Error {}
impl From<mako_rocksdb_store::BlobError> for Error {
    fn from(error: mako_rocksdb_store::BlobError) -> Self {
        Self::Storage(error.0)
    }
}
impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::Storage(error.to_string())
    }
}
pub(crate) fn core(error: impl std::fmt::Display) -> Error {
    Error::Core(error.to_string())
}
pub(crate) fn mutex<T>(lock: &std::sync::Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}
