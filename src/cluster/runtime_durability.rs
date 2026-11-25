//! Trusted runtime bridge from the verified durable actors to the application's
//! selected log. Implementations own authority fencing and atomic KV submission;
//! these concrete I/O facts are adapter contracts, not new Verus axioms.
use crate::{
    durable_kv::{CommitResult, DurableKv, ReadResult, WriteOp},
    types::Status,
};
use std::sync::Arc;

/// One exclusively owned actor namespace. Its authority token is captured when
/// opened; another leader/incarnation must never mutate this handle's map.
pub trait MetadataKv: Send {
    fn read(&mut self, key: &[u8]) -> ReadResult;
    fn submit(&mut self, batch: Vec<WriteOp>) -> CommitResult;
}
/// Supplied by the transaction runtime after data-log recovery. Namespace writes
/// use the same log as application rows and are included in its checkpoints.
/// An affine scope which binds the current storage callback thread to a term.
/// It cannot be sent to another thread and its drop restores the previous scope.
pub trait RowAuthority {}
pub trait RecoveryHost: Send + Sync {
    fn rows(&self, authority: u64) -> Result<Box<dyn RowAuthority>, Status>;
    /// Current leader term (or exclusive LocalLog term). This observation must
    /// not block shutdown. Metadata reads/writes and row_barrier separately
    /// confirm quorum authority and fence handles captured in an older term.
    fn authority(&self) -> Result<u64, Status>;
    fn metadata(&self, coordinator: bool, authority: u64) -> Result<Box<dyn MetadataKv>, Status>;
    /// True only after retained transaction protection has been reconstructed.
    fn transactions_restored(&self) -> Result<bool, Status>;
    /// All previously accepted physical row effects have settled before success.
    fn row_barrier(&self, authority: u64) -> Result<(), Status>;
}

pub(crate) struct Kv {
    inner: Box<dyn MetadataKv>,
    uncertain: bool,
}
impl Kv {
    pub fn new(inner: Box<dyn MetadataKv>) -> Self {
        Self {
            inner,
            uncertain: false,
        }
    }
}
impl DurableKv for Kv {
    fn read(&mut self, key: &[u8]) -> ReadResult {
        if self.uncertain {
            ReadResult::Io
        } else {
            self.inner.read(key)
        }
    }
    fn submit(&mut self, batch: Vec<WriteOp>) -> CommitResult {
        if self.uncertain {
            return CommitResult::NotCommitted;
        }
        let result = self.inner.submit(batch);
        if matches!(result, CommitResult::Unknown) {
            self.uncertain = true;
        }
        result
    }
    // A pending namespace requires rebuilding from authoritative log recovery;
    // this live handle cannot upgrade an uncertain submission by guessing.
    fn settle(&mut self) -> bool {
        !self.uncertain
    }
}

pub type Recovery = Arc<dyn RecoveryHost>;

/// No second row image lives here. The host has settled its log before recovery,
/// and each returned successful mutation has crossed that same log's barrier.
/// Ordinary transaction settlement uses the application's epoch/prepare gate;
/// this bridge never invents a successful settlement for an absent lease ID.
pub(crate) struct Engine {
    host: crate::host::Host,
    recovery: Recovery,
    authority: u64,
    owner: u32,
    admission: Option<crate::durable_kv::Admission>,
}
impl Engine {
    pub fn new(host: crate::host::Host, recovery: Recovery, authority: u64, owner: u32) -> Self {
        Self {
            host,
            recovery,
            authority,
            owner,
            admission: None,
        }
    }
}
impl crate::durable_engine::DurableEngine for Engine {
    fn recover_rows(&mut self) -> Status {
        self.admission = None;
        self.recovery
            .row_barrier(self.authority)
            .map_or_else(|e| e, |()| Status::Ok)
    }
    fn install(&mut self, admission: crate::durable_kv::Admission) -> Status {
        if admission.namespace.coordinator || admission.namespace.owner != self.owner {
            return Status::Invalid;
        }
        self.admission = Some(admission);
        Status::Ok
    }
    fn check(&self, admission: &crate::durable_kv::Admission) -> bool {
        self.recovery.authority() == Ok(self.authority)
            && self.admission.as_ref().is_some_and(|current| {
                current.namespace.owner == admission.namespace.owner
                    && current.namespace.coordinator == admission.namespace.coordinator
                    && current.incarnation == admission.incarnation
            })
    }
    fn scan(
        &self,
        admission: &crate::durable_kv::Admission,
        range: &crate::types::KeyRange,
        after: Option<&crate::types::Row>,
    ) -> Result<Option<crate::types::Row>, Status> {
        if !self.check(admission) {
            return Err(Status::Retry);
        }
        self.host.scan(range, after)
    }
    fn mutate(
        &mut self,
        admission: &crate::durable_kv::Admission,
        range: &crate::types::KeyRange,
        row: &crate::types::Row,
        delete: bool,
    ) -> CommitResult {
        if !self.check(admission) {
            return CommitResult::NotCommitted;
        }
        let status = if delete {
            self.host.remove(range.table, row)
        } else {
            self.host.put(range.table, row)
        };
        // The host's storage callback fail-stops on ambiguous physical outcomes.
        if status == Status::Ok {
            #[cfg(feature = "test-hooks")]
            test_pause(if delete {
                "cleanup-row"
            } else {
                "final-copy-row"
            });
            CommitResult::Committed
        } else {
            CommitResult::NotCommitted
        }
    }
    fn commit_rows(
        &mut self,
        admission: &crate::durable_kv::Admission,
        _: &crate::types::KeyRange,
    ) -> CommitResult {
        if !self.check(admission) {
            return CommitResult::NotCommitted;
        }
        match self.recovery.row_barrier(self.authority) {
            Ok(()) => CommitResult::Committed,
            Err(_) => CommitResult::Unknown,
        }
    }
    fn settle_transaction(
        &mut self,
        _: &crate::durable_kv::Admission,
        _: crate::types::TxnId,
    ) -> Status {
        // This runtime uses recovered transaction guards + host epochs. It never
        // writes ordinary lease IDs into the durable migration actor's journal.
        Status::Retry
    }
}

/// Deterministic process-crash boundary, absent from production builds.
#[cfg(feature = "test-hooks")]
pub fn test_pause(point: &str) {
    if std::env::var("MAKO_TEST_MIGRATION_PAUSE").ok().as_deref() != Some(point) {
        return;
    }
    let marker = std::env::var_os("MAKO_TEST_MIGRATION_MARKER").expect("test marker path");
    let mut file = match std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(marker)
    {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => return,
        Err(error) => panic!("publish crash boundary: {error}"),
    };
    std::io::Write::write_all(&mut file, point.as_bytes()).expect("write crash boundary");
    loop {
        std::thread::park();
    }
}
