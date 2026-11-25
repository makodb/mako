//! A group-local embedding of the existing verified Raft core, durable by default.
//! Explicit async mode is a weak persistence experiment, never a durable quorum.
//! Admission is not commitment. Only `Applied` is a successful write; losing
//! leadership after an append yields `Unknown`, never an invented abort.
mod durable;
mod local;
pub mod offline;
pub use local::{LocalConfig, LocalHost};
mod runtime;
mod snapshot;
mod transport;
mod wire;

use parking_lot::{Condvar, Mutex};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

/// Largest opaque proposal accepted by the host's bounded replication frame.
/// Checkpoints have no total-size cap and use a separate chunked protocol.
pub const MAX_COMMAND_BYTES: usize = 8 << 20;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Serialize, Deserialize)]
pub struct Position {
    pub index: u64,
    pub term: u64,
}

/// Callbacks are ordered on a dedicated application thread, never an SRPC poll
/// thread or a worker retaining STO guards. `apply(None)` persists a no-op's
/// exact applied prefix. An error is fatal to this host until recovery.
///
/// Checkpoint destinations belong to the host. Creation/validation must leave
/// them immutable; publication may copy/link but must not consume them.
/// `publish_checkpoint` MUST be idempotent: recovery repeats it after a crash
/// between durable Raft authority and publication of the application generation.
pub trait StateMachine: Send + Sync + 'static {
    fn applied_position(&self) -> Result<Position, String>;
    fn apply(&self, position: Position, command: Option<&[u8]>) -> Result<(), String>;
    fn create_checkpoint(&self, destination: &Path, position: Position) -> Result<(), String>;
    fn validate_checkpoint(&self, directory: &Path, position: Position) -> Result<(), String>;
    fn publish_checkpoint(&self, directory: &Path, position: Position) -> Result<(), String>;
}

#[derive(Clone, Debug)]
pub struct Peer {
    pub id: u16,
    pub address: String,
}

#[derive(Clone, Debug)]
pub struct Config {
    pub group: u64,
    pub node: u16,
    /// Complete membership, including this node. Ids are stable, not ordinals.
    pub peers: Vec<Peer>,
    pub listen: String,
    pub directory: PathBuf,
    /// Explicit bootstrap only; a missing store is otherwise refused.
    pub create: bool,
    /// Sync WAL/metadata before acknowledging (default). False retains writes
    /// but omits foreground persistence barriers: machine/power loss can lose
    /// acknowledged Raft state, so this is NOT a durable quorum.
    /// A store cannot be reopened under a different durability policy.
    pub sync_writes: bool,
    pub election_timeout: Duration,
    pub heartbeat_interval: Duration,
    pub rpc_timeout: Duration,
    pub max_command_bytes: usize,
    pub snapshot_chunk_bytes: usize,
    pub queue_capacity: usize,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            group: 0,
            node: 0,
            peers: Vec::new(),
            listen: "127.0.0.1:0".into(),
            directory: PathBuf::new(),
            create: false,
            sync_writes: true,
            // Peer replies include durable WAL sync, not just network RTT.
            // Keep failure detection above that budget; heartbeats stay frequent.
            election_timeout: Duration::from_secs(3),
            heartbeat_interval: Duration::from_millis(40),
            rpc_timeout: Duration::from_secs(1),
            max_command_bytes: 4 << 20,
            snapshot_chunk_bytes: 1 << 20,
            queue_capacity: 1024,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Status {
    pub term: u64,
    pub leader: Option<u16>,
    pub is_leader: bool,
    pub committed: Position,
    pub applied: Position,
    pub durable_sequence: u64,
    pub failure: Option<String>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum AdmissionError {
    NotLeader {
        term: u64,
        leader: Option<u16>,
    },
    Unavailable(String),
    TooLarge,
    /// The event reached admission but its result was lost. It is not abortable.
    Unknown,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum HostError {
    Unavailable(String),
    NotLeader,
    Timeout,
    Unknown,
    Storage(String),
}
impl std::fmt::Display for HostError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for HostError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ProposalStatus {
    Pending,
    Committed,
    Applied,
    /// May still become committed; query the durable application request id.
    Unknown,
    /// A different-term entry durably replaced this uncommitted position.
    Discarded,
    StorageFailed(String),
}
#[derive(Clone)]
pub struct Proposal {
    pub position: Position,
    pub(crate) completion: Arc<(Mutex<ProposalStatus>, Condvar)>,
    pub(crate) committed: Arc<AtomicBool>,
}
impl Proposal {
    pub fn status(&self) -> ProposalStatus {
        self.completion.0.lock().clone()
    }
    /// Monotone decision evidence, retained even if a later storage failure
    /// replaces the progress status. False means unknown, not rejected.
    pub fn is_committed(&self) -> bool {
        self.committed.load(Ordering::Acquire)
    }
    pub fn wait(&self, timeout: Duration) -> ProposalStatus {
        let deadline = Instant::now() + timeout;
        let mut guard = self.completion.0.lock();
        while matches!(&*guard, ProposalStatus::Pending | ProposalStatus::Committed) {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                break;
            }
            self.completion.1.wait_for(&mut guard, remaining);
        }
        match &*guard {
            ProposalStatus::Pending => ProposalStatus::Unknown,
            s => s.clone(),
        }
    }
    pub(crate) fn set(&self, value: ProposalStatus) {
        if matches!(value, ProposalStatus::Committed | ProposalStatus::Applied) {
            self.committed.store(true, Ordering::Release);
        }
        *self.completion.0.lock() = value;
        self.completion.1.notify_all();
    }
}

// Zero-sized and constant-false in production: the partition hook changes
// only peer traffic, not application, admission, or client-facing threads.
#[derive(Clone, Default)]
pub(crate) struct Isolation {
    #[cfg(feature = "test-hooks")]
    flag: Arc<AtomicBool>,
}
impl Isolation {
    pub(crate) fn active(&self) -> bool {
        #[cfg(feature = "test-hooks")]
        {
            self.flag.load(Ordering::SeqCst)
        }
        #[cfg(not(feature = "test-hooks"))]
        {
            false
        }
    }
}

struct Shared {
    tx: mpsc::SyncSender<runtime::Message>,
    status: Arc<Mutex<Status>>,
    thread: Mutex<Option<std::thread::JoinHandle<()>>>,
    max_command_bytes: usize,
    #[cfg(feature = "test-hooks")]
    isolation: Isolation,
}
impl Drop for Shared {
    fn drop(&mut self) {
        // The group only holds a Weak<Shared>, never an upgraded owner. Its
        // threads must be joined before the final public handle disappears,
        // including test unwind and ordinary implicit shutdown.
        let _ = self.tx.try_send(runtime::Message::Shutdown);
        if let Some(thread) = self.thread.get_mut().take() {
            let _ = thread.join();
        }
    }
}
#[derive(Clone)]
pub struct Host {
    shared: Arc<Shared>,
}
impl Host {
    pub fn start(config: Config, machine: Arc<dyn StateMachine>) -> Result<Self, HostError> {
        runtime::start(config, machine)
    }
    pub fn status(&self) -> Status {
        self.shared.status.lock().clone()
    }
    /// Test-only network partition: reject inbound and outbound Raft traffic
    /// while the host, application and client-facing threads remain alive.
    #[cfg(feature = "test-hooks")]
    pub fn set_isolated(&self, isolated: bool) {
        self.shared.isolation.flag.store(isolated, Ordering::SeqCst);
    }
    /// Waits only for local admission (durable in sync mode), never for quorum
    /// or application. Async acknowledgement has no machine-crash guarantee.
    /// The caller must mark its retained transaction submitted BEFORE this call.
    pub fn propose(&self, expected_term: u64, bytes: Vec<u8>) -> Result<Proposal, AdmissionError> {
        if bytes.len() > self.shared.max_command_bytes {
            return Err(AdmissionError::TooLarge);
        }
        let (tx, rx) = mpsc::sync_channel(1);
        self.shared
            .tx
            .try_send(runtime::Message::Propose {
                term: expected_term,
                command: wire::Command(Some(Arc::from(bytes))),
                reply: tx,
            })
            .map_err(|_| AdmissionError::Unavailable("admission queue unavailable".into()))?;
        rx.recv().unwrap_or(Err(AdmissionError::Unknown))
    }
    /// Confirms a fresh current-term quorum after this invocation, then waits
    /// for the committed prefix to be applied. This is not a cached lease:
    /// callers arriving after a heartbeat launch must await a later generation.
    pub fn read_barrier(
        &self,
        expected_term: u64,
        timeout: Duration,
    ) -> Result<Position, HostError> {
        let started = Instant::now();
        let deadline = started
            .checked_add(timeout)
            .ok_or_else(|| HostError::Unavailable("read deadline overflow".into()))?;
        let (reply, receiver) = mpsc::sync_channel(1);
        self.shared
            .tx
            .try_send(runtime::Message::ReadBarrier {
                term: expected_term,
                deadline,
                reply,
            })
            .map_err(|_| HostError::Unavailable("read queue unavailable".into()))?;
        let position = receiver
            .recv_timeout(timeout.saturating_sub(started.elapsed()))
            .map_err(|error| match error {
                mpsc::RecvTimeoutError::Timeout => HostError::Timeout,
                mpsc::RecvTimeoutError::Disconnected => {
                    HostError::Unavailable("host stopped".into())
                }
            })??;
        let status = self.status();
        if let Some(error) = status.failure {
            return Err(HostError::Storage(error));
        }
        if !status.is_leader || status.term != expected_term {
            return Err(HostError::NotLeader);
        }
        Ok(position)
    }
    pub fn checkpoint(&self) -> Result<Position, HostError> {
        let (tx, rx) = mpsc::sync_channel(1);
        self.shared
            .tx
            .send(runtime::Message::Checkpoint(tx))
            .map_err(|_| HostError::Unavailable("stopped".into()))?;
        rx.recv().map_err(|_| HostError::Unknown)?
    }
    pub fn shutdown(&self) {
        let _ = self.shared.tx.send(runtime::Message::Shutdown);
        if let Some(thread) = self.shared.thread.lock().take() {
            let _ = thread.join();
        }
    }
}
