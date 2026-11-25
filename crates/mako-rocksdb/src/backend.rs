//! Adapter contract. The facade tests use an in-memory implementation; production
//! delegates to the existing distributed coordinator, never a local engine txn.
use crate::{Outcome, Result, TransactionId, WriteOptions};
use mako_protocol::wire::{Bound, Row};

pub trait Backend: Send + Sync {
    fn column_families(&self) -> Vec<(String, u64)>;
    fn validate_write(&self, options: &WriteOptions) -> Result<()>;
    /// Either return one exclusively owned attempt or fail without starting work.
    fn begin(&self) -> Result<Box<dyn Session>>;
    /// Resolve the saved decision and finish every participant before success.
    fn resolve(&self, id: TransactionId) -> Result<Outcome>;
}

/// Methods record read/range dependencies and see their own writes. Commit must
/// use global write-lock acquisition followed by read validation and durable 2PC.
/// A failed/uncertain cleanup retains protection and prevents stream reuse.
pub trait Session: Send {
    fn id(&self) -> TransactionId;
    fn get(&mut self, table: u64, key: &[u8]) -> Result<Option<Vec<u8>>>;
    fn put(&mut self, table: u64, key: &[u8], value: &[u8]) -> Result<()>;
    fn delete(&mut self, table: u64, key: &[u8]) -> Result<()>;
    /// Return at most limit rows, in the requested order. The facade asks for
    /// one extra row so truncation becomes an explicit error.
    fn scan(
        &mut self,
        table: u64,
        lower: Bound,
        upper: Bound,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>>;
    fn commit(self: Box<Self>) -> Result<()>;
    fn rollback(self: Box<Self>) -> Result<()>;
}
