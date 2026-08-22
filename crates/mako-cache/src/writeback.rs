//! Transaction-ordered, asynchronous durable write-back.
//!
//! Before entering native commit, a transaction prepares its complete record
//! and claims bounded queue capacity, but it does not yet receive a cache
//! sequence or occupy an ordered slot. After Mako has chosen the transaction's
//! logical timestamp and native validation has succeeded,
//! [`DetachedPermit::bind`] attaches that preallocated record to the ordered
//! queue and records that Mako timestamp. The hook-time operation is
//! allocation-free and never performs backend IO.
//!
//! A bound slot remains Prepared until native commit returns successfully.
//! Publishing then finalizes the checksum outside Silo's lock critical section
//! and flips the slot Ready. An ambiguous post-bind outcome pins the slot, so
//! the background consumer can neither skip it nor mistake it for an abort.

use std::collections::VecDeque;
use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

use mako_local::MakoTimestamp;
use mrx_core::{BlobError, Blobs};

use crate::record::{
    BoundCommitRecord, CommitRecord, CommitSeq, Mutation, PreparedCommitRecord, RecordError,
};

/// Default maximum encoded transaction-record size (8 MiB).
pub const DEFAULT_MAX_RECORD_BYTES: usize = 8 * 1024 * 1024;

/// Controls the bounded commit queue and synchronous flush retries.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WritebackConfig {
    /// Maximum number of detached permits plus prepared or ready slots.
    pub capacity: usize,
    /// Maximum encoded size accepted by [`PreparedCommitRecord::prepare`].
    pub max_record_bytes: usize,
    /// Extra attempts made by [`Writeback::flush`] after a failed backend
    /// write. Zero means that the first failed attempt is returned.
    pub max_flush_retries: usize,
    /// Delay between backend retry attempts.
    pub retry_delay: Duration,
}

impl Default for WritebackConfig {
    fn default() -> Self {
        Self {
            capacity: 1_024,
            max_record_bytes: DEFAULT_MAX_RECORD_BYTES,
            max_flush_retries: 8,
            retry_delay: Duration::from_millis(1),
        }
    }
}

/// Invalid construction parameters for [`Writeback`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConfigError {
    /// A zero-capacity queue can never admit a commit.
    ZeroCapacity,
    /// A zero-byte record budget cannot hold the record envelope.
    ZeroRecordBudget,
    /// A zero retry delay would turn a persistent backend error into a spin
    /// loop.
    ZeroRetryDelay,
    /// The durable seed leaves no sequence number for a future bind.
    SequenceExhausted,
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ZeroCapacity => write!(f, "write-back capacity must be nonzero"),
            Self::ZeroRecordBudget => write!(f, "transaction record budget must be nonzero"),
            Self::ZeroRetryDelay => write!(f, "write-back retry delay must be nonzero"),
            Self::SequenceExhausted => write!(f, "commit sequence space is exhausted"),
        }
    }
}

impl std::error::Error for ConfigError {}

/// Failure to prepare a complete transaction record and claim queue capacity.
#[derive(Debug)]
pub enum ReserveError {
    /// The transaction could not be represented within the configured record
    /// limit.
    Record(RecordError),
    /// No nonzero commit sequence remains.
    SequenceExhausted,
    /// An earlier native commit has an ambiguous outcome. New commits are
    /// rejected because they could never safely pass that slot.
    UnknownOutcome {
        /// The first pinned sequence.
        sequence: CommitSeq,
    },
}

impl fmt::Display for ReserveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Record(error) => write!(f, "cannot prepare transaction record: {error}"),
            Self::SequenceExhausted => write!(f, "commit sequence space is exhausted"),
            Self::UnknownOutcome { sequence } => write!(
                f,
                "commit sequence {} has an unknown native outcome",
                sequence.get()
            ),
        }
    }
}

impl std::error::Error for ReserveError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Record(error) => Some(error),
            Self::SequenceExhausted | Self::UnknownOutcome { .. } => None,
        }
    }
}

impl From<RecordError> for ReserveError {
    fn from(value: RecordError) -> Self {
        Self::Record(value)
    }
}

/// An impossible-in-normal-use reservation transition was rejected.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResolveError {
    /// The slot is no longer present in the queue.
    Missing {
        /// Sequence of the missing slot.
        sequence: CommitSeq,
    },
    /// The slot had already left its prepared state.
    AlreadyResolved {
        /// Sequence of the resolved slot.
        sequence: CommitSeq,
    },
    /// The slot is pinned because the native commit outcome is unknown.
    Pinned {
        /// Sequence of the pinned slot.
        sequence: CommitSeq,
    },
    /// A known-committed transaction cannot be acknowledged because an
    /// earlier ordered slot has an ambiguous native outcome.
    BlockedByPriorUnknown {
        /// Sequence of the known-committed transaction retained and pinned.
        sequence: CommitSeq,
        /// Earlier sequence whose native outcome is ambiguous.
        prior_unknown: CommitSeq,
    },
}

impl fmt::Display for ResolveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Missing { sequence } => {
                write!(f, "reservation {} is no longer queued", sequence.get())
            }
            Self::AlreadyResolved { sequence } => {
                write!(f, "reservation {} is already resolved", sequence.get())
            }
            Self::Pinned { sequence } => write!(
                f,
                "reservation {} has an unknown native outcome",
                sequence.get()
            ),
            Self::BlockedByPriorUnknown {
                sequence,
                prior_unknown,
            } => write!(
                f,
                "reservation {} cannot be acknowledged because prior reservation {} has an unknown native outcome",
                sequence.get(),
                prior_unknown.get()
            ),
        }
    }
}

impl std::error::Error for ResolveError {}

/// A flush could not make its acknowledged snapshot durable.
#[derive(Debug, Clone)]
pub enum FlushError {
    /// A commit at or before the target has an ambiguous native outcome.
    UnknownOutcome {
        /// The first pinned sequence.
        sequence: CommitSeq,
    },
    /// Rocks (or another [`Blobs`] implementation) kept rejecting the exact
    /// front transaction after the configured retry budget.
    Backend {
        /// Sequence whose atomic backend batch failed.
        sequence: CommitSeq,
        /// Number of failed attempts made by this flush call.
        attempts: usize,
        /// Last backend error.
        source: BlobError,
    },
}

impl fmt::Display for FlushError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownOutcome { sequence } => write!(
                f,
                "commit sequence {} has an unknown native outcome",
                sequence.get()
            ),
            Self::Backend {
                sequence,
                attempts,
                source,
            } => write!(
                f,
                "backend rejected commit sequence {} after {attempts} attempt(s): {source}",
                sequence.get()
            ),
        }
    }
}

impl std::error::Error for FlushError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::UnknownOutcome { .. } => None,
            Self::Backend { source, .. } => Some(source),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SlotState {
    Prepared { pinned: bool },
    Ready,
}

#[derive(Debug)]
struct Slot {
    sequence: CommitSeq,
    record: Arc<OnceLock<CommitRecord>>,
    state: SlotState,
}

#[derive(Debug)]
struct State {
    queue: VecDeque<Slot>,
    /// Capacity claimed before native commit but not yet attached to `queue`.
    detached: usize,
    last_bound: u64,
    durable: u64,
    highest_acknowledged: u64,
    first_unknown: Option<CommitSeq>,
}

/// A bounded, transaction-ordered bridge from native Silo commits to Rocks.
///
/// `B` is stored directly. Use `Arc<B>` as the type argument when another
/// component also needs an owning backend handle; `mrx_core` implements
/// [`Blobs`] for `Arc<B>`.
pub struct Writeback<B: Blobs> {
    backend: B,
    config: WritebackConfig,
    state: Mutex<State>,
    changed: Condvar,
    capacity_available: Condvar,
    consumer: Mutex<()>,
}

impl<B: Blobs> Writeback<B> {
    /// Create an empty queue whose first reservation is `durable_seed + 1`.
    ///
    /// `durable_seed` is also the initial durable watermark, allowing a caller
    /// that recovered a durable transaction log to continue its sequence.
    pub fn new(
        backend: B,
        durable_seed: u64,
        config: WritebackConfig,
    ) -> Result<Self, ConfigError> {
        if config.capacity == 0 {
            return Err(ConfigError::ZeroCapacity);
        }
        if config.max_record_bytes == 0 {
            return Err(ConfigError::ZeroRecordBudget);
        }
        if config.retry_delay.is_zero() {
            return Err(ConfigError::ZeroRetryDelay);
        }
        if durable_seed == u64::MAX {
            return Err(ConfigError::SequenceExhausted);
        }

        Ok(Self {
            backend,
            config,
            state: Mutex::new(State {
                queue: VecDeque::with_capacity(config.capacity),
                detached: 0,
                last_bound: durable_seed,
                durable: durable_seed,
                highest_acknowledged: durable_seed,
                first_unknown: None,
            }),
            changed: Condvar::new(),
            capacity_available: Condvar::new(),
            consumer: Mutex::new(()),
        })
    }

    /// Access the underlying durable backend.
    pub fn backend(&self) -> &B {
        &self.backend
    }

    /// Prepare a complete transaction record and claim capacity before native
    /// commit, without assigning a sequence or inserting an ordered log slot.
    ///
    /// Capacity counts both detached permits and queued slots. Record encoding,
    /// key construction, and the reference-counted record cell are allocated
    /// after capacity is claimed but before this method returns. Consequently
    /// [`DetachedPermit::bind`] can run inside Silo's post-validation hook
    /// without allocation, waiting for capacity, or touching the backend.
    pub fn reserve(&self, mutations: Vec<Mutation>) -> Result<DetachedPermit<'_, B>, ReserveError> {
        self.claim_detached_capacity()?;

        // Build the returned value first so unwinding or a preparation error
        // releases the detached claim through Drop.
        let mut permit = DetachedPermit {
            owner: self,
            prepared: None,
            record: None,
            owns_capacity: true,
        };
        permit.prepared = Some(PreparedCommitRecord::prepare(
            mutations,
            self.config.max_record_bytes,
        )?);
        permit.record = Some(Arc::new(OnceLock::new()));
        Ok(permit)
    }

    fn claim_detached_capacity(&self) -> Result<(), ReserveError> {
        let mut state = lock_recover(&self.state);
        loop {
            if let Some(sequence) = state.first_unknown {
                return Err(ReserveError::UnknownOutcome { sequence });
            }
            if state.last_bound == u64::MAX {
                return Err(ReserveError::SequenceExhausted);
            }

            let occupied = state
                .queue
                .len()
                .checked_add(state.detached)
                .expect("write-back occupancy cannot overflow");
            let detached =
                u64::try_from(state.detached).map_err(|_| ReserveError::SequenceExhausted)?;
            let sequence_available = state
                .last_bound
                .checked_add(detached)
                .and_then(|tail| tail.checked_add(1))
                .is_some();

            if occupied < self.config.capacity && sequence_available {
                state.detached += 1;
                return Ok(());
            }

            state = wait_recover(&self.capacity_available, state);
        }
    }

    fn release_detached_capacity(&self) {
        let mut state = lock_recover(&self.state);
        state.detached = state
            .detached
            .checked_sub(1)
            .expect("detached permit capacity underflow");
        drop(state);
        self.capacity_available.notify_all();
        self.changed.notify_all();
    }

    /// Highest contiguous bound record applied atomically to the backend.
    pub fn durable_sequence(&self) -> u64 {
        lock_recover(&self.state).durable
    }

    /// Highest sequence acknowledged to a caller as a successful native
    /// commit. This is deliberately distinct from the queue tail.
    pub fn highest_acknowledged(&self) -> u64 {
        lock_recover(&self.state).highest_acknowledged
    }

    /// Number of bound queue slots. Detached permits are deliberately omitted.
    pub fn queue_len(&self) -> usize {
        lock_recover(&self.state).queue.len()
    }

    #[cfg(test)]
    fn detached_len(&self) -> usize {
        lock_recover(&self.state).detached
    }

    /// Snapshot the highest acknowledged sequence and make that snapshot
    /// durable.
    ///
    /// Transactions acknowledged after the snapshot are intentionally not
    /// part of this barrier. That includes a later pinned unknown slot: it does
    /// not invalidate an earlier acknowledged prefix, although clean shutdown
    /// separately rejects any unknown outcome. Each ready record is submitted
    /// to the backend in exactly one atomic `write_batch` call. A failed record
    /// remains unchanged at the front for the next retry.
    pub fn flush(&self) -> Result<u64, FlushError> {
        let target = {
            let state = lock_recover(&self.state);
            let target = state.highest_acknowledged;
            if let Some(sequence) = state.first_unknown {
                if sequence.get() <= target {
                    return Err(FlushError::UnknownOutcome { sequence });
                }
            }
            target
        };

        let mut failed_sequence = None;
        let mut failed_attempts = 0usize;

        loop {
            {
                let state = lock_recover(&self.state);
                if state.durable >= target {
                    return Ok(target);
                }
                if let Some(sequence) = state.first_unknown {
                    if sequence.get() <= target {
                        return Err(FlushError::UnknownOutcome { sequence });
                    }
                }
            }

            match self.process_front() {
                ProcessOutcome::Advanced => {
                    failed_sequence = None;
                    failed_attempts = 0;
                }
                ProcessOutcome::BackendFailed { sequence, error } => {
                    if failed_sequence == Some(sequence) {
                        failed_attempts = failed_attempts.saturating_add(1);
                    } else {
                        failed_sequence = Some(sequence);
                        failed_attempts = 1;
                    }

                    if failed_attempts > self.config.max_flush_retries {
                        // A concurrent consumer may have recovered between our
                        // failed attempt and this decision.
                        let durable = lock_recover(&self.state).durable;
                        match retry_progress(durable, target, sequence) {
                            RetryProgress::TargetDurable => return Ok(target),
                            RetryProgress::FailedSequenceDurable => {
                                failed_sequence = None;
                                failed_attempts = 0;
                                continue;
                            }
                            RetryProgress::NoProgress => {}
                        }
                        return Err(FlushError::Backend {
                            sequence,
                            attempts: failed_attempts,
                            source: error,
                        });
                    }
                    self.wait_for_activity(self.config.retry_delay);
                }
                ProcessOutcome::Blocked | ProcessOutcome::Idle => {
                    self.wait_for_flush_progress(target);
                }
                ProcessOutcome::Pinned(sequence) => {
                    return Err(FlushError::UnknownOutcome { sequence });
                }
            }
        }
    }

    fn resolve(&self, sequence: CommitSeq, resolution: Resolution) -> Result<(), ResolveError> {
        let mut state = lock_recover(&self.state);
        let prior_unknown = state.first_unknown.filter(|unknown| *unknown < sequence);
        let Some(slot) = state
            .queue
            .iter_mut()
            .find(|slot| slot.sequence == sequence)
        else {
            return Err(ResolveError::Missing { sequence });
        };

        match slot.state {
            SlotState::Prepared { pinned: true } => {
                return Err(ResolveError::Pinned { sequence });
            }
            SlotState::Prepared { pinned: false } => {}
            SlotState::Ready => {
                return Err(ResolveError::AlreadyResolved { sequence });
            }
        }

        let result = match resolution {
            Resolution::Publish => {
                if let Some(prior_unknown) = prior_unknown {
                    // The current transaction is known committed, but callers
                    // must not observe an acknowledgement beyond an ambiguous
                    // prefix. Retain it as a pinned Prepared slot.
                    slot.state = SlotState::Prepared { pinned: true };
                    Err(ResolveError::BlockedByPriorUnknown {
                        sequence,
                        prior_unknown,
                    })
                } else {
                    slot.state = SlotState::Ready;
                    state.highest_acknowledged = state.highest_acknowledged.max(sequence.get());
                    Ok(())
                }
            }
            Resolution::PinUnknown => {
                slot.state = SlotState::Prepared { pinned: true };
                state.first_unknown = Some(match state.first_unknown {
                    Some(current) => current.min(sequence),
                    None => sequence,
                });
                Ok(())
            }
        };

        drop(state);
        self.changed.notify_all();
        // A producer blocked on a full queue must wake and observe a newly
        // pinned unknown outcome instead of waiting forever for capacity that
        // can no longer advance.
        self.capacity_available.notify_all();
        result
    }

    pub(crate) fn process_front(&self) -> ProcessOutcome {
        // This guard is separate from the queue state so the single-consumer
        // rule remains explicit if the state lock is narrowed around IO later.
        // Both locks deliberately recover poison: if a backend panics, the
        // untouched Ready record is safe to retry.
        let _consumer = lock_recover(&self.consumer);
        let state = lock_recover(&self.state);

        let Some(front) = state.queue.front() else {
            return ProcessOutcome::Idle;
        };
        let sequence = front.sequence;

        match front.state {
            SlotState::Prepared { pinned: false } => ProcessOutcome::Blocked,
            SlotState::Prepared { pinned: true } => ProcessOutcome::Pinned(sequence),
            SlotState::Ready => {
                // The consumer guard preserves global order while the Arc keeps
                // the exact record alive. Drop the queue-state lock during slow
                // backend IO so later native commits can publish without
                // waiting for Rocks.
                let record = Arc::clone(&front.record);
                drop(state);
                let record = record
                    .get()
                    .expect("a Ready slot must contain its finalized record");
                let operations = record.backend_ops();
                let result = self.backend.write_batch(&operations);

                match result {
                    Ok(()) => {
                        let mut state = lock_recover(&self.state);
                        let current = state
                            .queue
                            .front()
                            .expect("serialized consumer keeps front present");
                        assert_eq!(
                            current.sequence, sequence,
                            "serialized consumer changed the front sequence"
                        );
                        assert_eq!(
                            current.state,
                            SlotState::Ready,
                            "published front changed state during backend IO"
                        );
                        state.queue.pop_front();
                        state.durable = sequence.get();
                        drop(state);
                        self.capacity_available.notify_all();
                        self.changed.notify_all();
                        ProcessOutcome::Advanced
                    }
                    Err(error) => ProcessOutcome::BackendFailed { sequence, error },
                }
            }
        }
    }

    pub(crate) fn wait_for_activity(&self, timeout: Duration) {
        let state = lock_recover(&self.state);
        drop(wait_timeout_recover(&self.changed, state, timeout));
    }

    pub(crate) fn retry_delay(&self) -> Duration {
        self.config.retry_delay
    }

    pub(crate) fn ensure_no_unknown(&self) -> Result<(), FlushError> {
        match lock_recover(&self.state).first_unknown {
            Some(sequence) => Err(FlushError::UnknownOutcome { sequence }),
            None => Ok(()),
        }
    }

    pub(crate) fn wake_waiters(&self) {
        self.changed.notify_all();
        self.capacity_available.notify_all();
    }

    fn wait_for_flush_progress(&self, target: u64) {
        let state = lock_recover(&self.state);
        if state.durable >= target {
            return;
        }
        if state
            .first_unknown
            .is_some_and(|sequence| sequence.get() <= target)
        {
            return;
        }
        if state
            .queue
            .front()
            .is_some_and(|slot| slot.state == SlotState::Ready)
        {
            // Publication may have raced between process_front() observing a
            // Prepared hole and this waiter acquiring the state lock. Do not
            // sleep after the only Ready notification has already fired.
            return;
        }

        // Holding `state` until `wait` atomically releases it closes the usual
        // notification gap between observing a Prepared hole and sleeping.
        drop(wait_recover(&self.changed, state));
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Resolution {
    Publish,
    PinUnknown,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum DropAction {
    PinUnknown,
    Done,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RetryProgress {
    TargetDurable,
    FailedSequenceDurable,
    NoProgress,
}

fn retry_progress(durable: u64, target: u64, failed: CommitSeq) -> RetryProgress {
    if durable >= target {
        RetryProgress::TargetDurable
    } else if durable >= failed.get() {
        RetryProgress::FailedSequenceDurable
    } else {
        RetryProgress::NoProgress
    }
}

/// A fully preallocated record and bounded capacity claim that has not entered
/// the ordered commit log.
///
/// This value is created before native commit. Dropping it means Silo failed
/// before reaching the post-validation hook: capacity is released without
/// assigning a sequence or leaving a cancellation marker.
pub struct DetachedPermit<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    prepared: Option<PreparedCommitRecord>,
    record: Option<Arc<OnceLock<CommitRecord>>>,
    owns_capacity: bool,
}

impl<'a, B: Blobs> DetachedPermit<'a, B> {
    /// Bind this record in Silo's post-validation, pre-install hook.
    ///
    /// This assigns the next cache sequence, embeds Mako's transaction
    /// timestamp, and appends one Prepared slot. The method performs no heap
    /// allocation, capacity wait, backend IO, or record-length work. It can
    /// reject the transaction if an earlier commit became ambiguous after this
    /// permit was detached; the caller must then abort native commit before
    /// installation. A rejection leaves the prepared bytes on this permit so
    /// they can be destroyed after Silo returns and releases its write locks.
    /// Concurrent hooks may briefly contend on the queue-state mutex, whose
    /// protected work is bounded and entirely in memory.
    #[must_use = "a bound native commit must be published or pinned unknown"]
    pub fn bind(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<BoundReservation<'a, B>, ReserveError> {
        let mut state = lock_recover(&self.owner.state);
        if let Some(sequence) = state.first_unknown {
            // Leave the complete prepared record on this permit. The caller
            // is inside Silo's hook and will drop it only after native abort
            // has returned and released the write locks.
            return Err(ReserveError::UnknownOutcome { sequence });
        }
        let raw_sequence = state
            .last_bound
            .checked_add(1)
            .expect("detached capacity guarantees commit sequence space");
        let sequence = CommitSeq::new(raw_sequence)
            .expect("the sequence after a valid durable seed is nonzero");
        let prepared = self
            .prepared
            .take()
            .expect("a detached permit owns one prepared record");
        let record = self
            .record
            .take()
            .expect("a detached permit owns one preallocated record cell");
        let bound = prepared.bind(sequence, mako_timestamp);

        // Queue storage and the record cell were both preallocated before
        // native commit. Since detached claims count against capacity, this
        // push cannot grow the VecDeque.
        assert!(
            state.queue.len() < self.owner.config.capacity,
            "a detached permit must own queue capacity"
        );
        assert!(
            state.queue.len() < state.queue.capacity(),
            "the bounded queue must be preallocated"
        );
        let detached_after_bind = state
            .detached
            .checked_sub(1)
            .expect("binding owns one detached capacity claim");
        let slot = Slot {
            sequence,
            record: Arc::clone(&record),
            state: SlotState::Prepared { pinned: false },
        };

        // Perform every checked operation before logical insertion. From this
        // point through returning the bound handle, only scalar assignments and
        // a no-growth VecDeque push remain.
        state.detached = detached_after_bind;
        state.last_bound = raw_sequence;
        self.owns_capacity = false;
        state.queue.push_back(slot);
        drop(state);
        self.owner.changed.notify_all();
        // Wake sequence-space waiters. Occupancy is unchanged by binding, but
        // a bind at u64::MAX turns a temporary sequence shortage permanent.
        self.owner.capacity_available.notify_all();

        Ok(BoundReservation {
            owner: self.owner,
            sequence,
            bound: Some(bound),
            record,
            on_drop: DropAction::PinUnknown,
        })
    }
}

impl<B: Blobs> Drop for DetachedPermit<'_, B> {
    fn drop(&mut self) {
        if self.owns_capacity {
            self.owner.release_detached_capacity();
            self.owns_capacity = false;
        }
    }
}

/// A Prepared ordered slot created after native validation.
///
/// Dropping this handle pins its slot because Silo may already have installed
/// the transaction. A definite native success must call [`Self::publish`]; an
/// ambiguous return may call [`Self::pin_unknown`] explicitly or simply drop.
pub struct BoundReservation<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    sequence: CommitSeq,
    bound: Option<BoundCommitRecord>,
    record: Arc<OnceLock<CommitRecord>>,
    on_drop: DropAction,
}

impl<B: Blobs> BoundReservation<'_, B> {
    /// Return the cache sequence assigned at the native serialization hook.
    #[cfg(test)]
    pub const fn sequence(&self) -> CommitSeq {
        self.sequence
    }

    /// Publish a successfully committed native transaction.
    ///
    /// Checksum finalization is allocation-free but linear in record length, so
    /// it intentionally runs here after Silo has left its lock critical
    /// section. The finalized record is installed in its preallocated cell
    /// before the slot becomes Ready.
    pub fn publish(mut self) -> Result<CommitSeq, ResolveError> {
        self.finalize_once();
        self.owner.resolve(self.sequence, Resolution::Publish)?;
        self.on_drop = DropAction::Done;
        Ok(self.sequence)
    }

    /// Permanently pin the slot because the native commit outcome is unknown.
    ///
    /// The write-back layer cannot safely skip such a slot. Flushes fail and
    /// new reservations are rejected until the database is reopened and
    /// recovered by a higher-level protocol.
    pub fn pin_unknown(mut self) -> Result<CommitSeq, ResolveError> {
        self.on_drop = DropAction::PinUnknown;
        self.finalize_once();
        self.owner.resolve(self.sequence, Resolution::PinUnknown)?;
        self.on_drop = DropAction::Done;
        Ok(self.sequence)
    }

    /// Finalize and retain the exact write set at most once.
    ///
    /// All storage is preallocated. This performs only the deferred checksum
    /// scan and a `OnceLock` store, so neither publication nor fail-stop
    /// retention can fail because of allocation.
    fn finalize_once(&mut self) {
        let Some(bound) = self.bound.take() else {
            return;
        };
        let record = bound.finalize();
        // Safe consuming transitions provide exactly one BoundCommitRecord.
        // If an internal misuse somehow finalized the cell first, retaining
        // that existing complete record is safer than panicking here.
        let _ = self.record.set(record);
    }
}

impl<B: Blobs> Drop for BoundReservation<'_, B> {
    fn drop(&mut self) {
        let resolution = match self.on_drop {
            DropAction::PinUnknown => Resolution::PinUnknown,
            DropAction::Done => return,
        };
        // Preserve the ambiguous write set before pinning. Drop remains
        // best-effort and panic-free even if a future finalizer invariant
        // regresses; pinning is still attempted after a contained panic.
        let _ = catch_unwind(AssertUnwindSafe(|| self.finalize_once()));
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let _ = self.owner.resolve(self.sequence, resolution);
        }));
        self.on_drop = DropAction::Done;
    }
}

#[derive(Debug)]
pub(crate) enum ProcessOutcome {
    Idle,
    Blocked,
    Pinned(CommitSeq),
    Advanced,
    BackendFailed {
        sequence: CommitSeq,
        error: BlobError,
    },
}

fn lock_recover<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn wait_recover<'a, T>(condvar: &Condvar, guard: MutexGuard<'a, T>) -> MutexGuard<'a, T> {
    condvar
        .wait(guard)
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn wait_timeout_recover<'a, T>(
    condvar: &Condvar,
    guard: MutexGuard<'a, T>,
    timeout: Duration,
) -> MutexGuard<'a, T> {
    match condvar.wait_timeout(guard, timeout) {
        Ok((guard, _)) => guard,
        Err(poisoned) => poisoned.into_inner().0,
    }
}

#[cfg(test)]
mod tests {
    use std::panic::{catch_unwind, AssertUnwindSafe};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{mpsc, Arc};

    use mrx_core::fakes::MemBlobs;
    use mrx_core::BlobOp;

    use super::*;

    const TABLE: u64 = 1;

    fn put(key: &[u8], value: &[u8]) -> Mutation {
        Mutation::Put {
            table_id: TABLE,
            key: key.to_vec(),
            value: value.to_vec(),
        }
    }

    fn delete(key: &[u8]) -> Mutation {
        Mutation::Delete {
            table_id: TABLE,
            key: key.to_vec(),
        }
    }

    fn config(capacity: usize, max_flush_retries: usize) -> WritebackConfig {
        WritebackConfig {
            capacity,
            max_flush_retries,
            retry_delay: Duration::from_millis(1),
            ..WritebackConfig::default()
        }
    }

    fn bind<'a, B: Blobs>(
        mut permit: DetachedPermit<'a, B>,
        mako_timestamp: u32,
    ) -> BoundReservation<'a, B> {
        permit.bind(mako_timestamp_of(mako_timestamp)).unwrap()
    }

    fn mako_timestamp_of(raw: u32) -> MakoTimestamp {
        MakoTimestamp::new(raw).expect("test Mako timestamps are nonzero")
    }

    #[test]
    fn one_transaction_is_exactly_one_backend_batch() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();

        writeback
            .reserve(vec![put(b"a", b"one"), delete(b"b")])
            .unwrap()
            .bind(mako_timestamp_of(11))
            .unwrap()
            .publish()
            .unwrap();

        assert_eq!(writeback.flush().unwrap(), 1);
        assert_eq!(backend.batch_count(), 1);
        // One durable commit-log entry plus the transaction's two data ops.
        assert_eq!(backend.op_count(), 3);
        assert_eq!(writeback.durable_sequence(), 1);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn failed_batch_is_retained_whole_and_retried() {
        let backend = Arc::new(MemBlobs::new());
        backend.fail_next_writes(1);
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        writeback
            .reserve(vec![put(b"a", b"one"), put(b"b", b"two")])
            .unwrap()
            .bind(mako_timestamp_of(12))
            .unwrap()
            .publish()
            .unwrap();

        assert!(matches!(
            writeback.flush(),
            Err(FlushError::Backend {
                sequence,
                attempts: 1,
                ..
            }) if sequence.get() == 1
        ));
        assert!(
            backend.snapshot().is_empty(),
            "failed batch was all-or-none"
        );
        assert_eq!(writeback.queue_len(), 1, "exact record remains queued");

        assert_eq!(writeback.flush().unwrap(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 3);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn exhausted_retry_continues_when_concurrent_consumer_completed_failed_sequence() {
        let failed = CommitSeq::new(7).unwrap();

        assert_eq!(
            retry_progress(7, 8, failed),
            RetryProgress::FailedSequenceDurable,
            "the exact failed sequence is no longer a live backend failure"
        );
        assert_eq!(retry_progress(8, 8, failed), RetryProgress::TargetDurable);
        assert_eq!(retry_progress(6, 8, failed), RetryProgress::NoProgress);
    }

    #[test]
    fn detached_abort_uses_capacity_but_never_assigns_a_sequence_or_slot() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 9, config(2, 0)).unwrap();

        let aborted = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        assert_eq!(writeback.detached_len(), 1);
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(writeback.highest_acknowledged(), 9);
        drop(aborted);

        let committed = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 100);
        assert_eq!(committed.sequence().get(), 10, "abort left no sequence gap");
        committed.publish().unwrap();
        assert_eq!(writeback.flush().unwrap(), 10);
    }

    #[test]
    fn record_preparation_failure_releases_detached_capacity() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(1, 0)).unwrap();

        assert!(matches!(
            writeback.reserve(vec![put(b"same", b"one"), put(b"same", b"two")]),
            Err(ReserveError::Record(_))
        ));
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 0);

        let permit = writeback.reserve(vec![put(b"valid", b"value")]).unwrap();
        assert_eq!(writeback.detached_len(), 1);
        drop(permit);
    }

    #[test]
    fn cache_sequence_is_assigned_by_bind_order_not_reserve_order() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let reserved_first = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        let reserved_second = writeback.reserve(vec![put(b"b", b"two")]).unwrap();

        let bound_first = bind(reserved_second, 202);
        let bound_second = bind(reserved_first, 101);
        assert_eq!(bound_first.sequence().get(), 1);
        assert_eq!(bound_second.sequence().get(), 2);

        bound_second.publish().unwrap();
        assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));
        bound_first.publish().unwrap();
        assert_eq!(writeback.flush().unwrap(), 2);
    }

    #[test]
    fn prepared_hole_blocks_later_ready_transaction() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 21);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 22);
        second.publish().unwrap();

        assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));
        assert_eq!(backend.batch_count(), 0);
        assert_eq!(writeback.highest_acknowledged(), 2);

        first.publish().unwrap();
        assert_eq!(writeback.flush().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
        assert_eq!(writeback.durable_sequence(), 2);
    }

    #[test]
    fn out_of_order_publication_still_flushes_in_sequence_order() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 31);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 32);
        second.publish().unwrap();
        assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));

        first.publish().unwrap();
        assert_eq!(writeback.flush().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
        assert_eq!(writeback.durable_sequence(), 2);
    }

    #[test]
    fn unknown_outcome_pins_queue_and_rejects_new_reservations() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(4, 0)).unwrap();
        let unknown = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 41);
        let later = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 42);
        unknown.pin_unknown().unwrap();
        assert!(matches!(
            later.publish(),
            Err(ResolveError::BlockedByPriorUnknown {
                sequence,
                prior_unknown,
            }) if sequence.get() == 2 && prior_unknown.get() == 1
        ));

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 1
        ));
        // No sequence beyond the ambiguity was acknowledged, so the empty
        // acknowledged prefix is a valid flush target. Clean close still calls
        // ensure_no_unknown and rejects the pinned queue.
        assert_eq!(writeback.flush().unwrap(), 0);
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(FlushError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert!(matches!(
            writeback.reserve(vec![put(b"b", b"two")]),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert_eq!(writeback.durable_sequence(), 0);
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(writeback.queue_len(), 2);
        let state = lock_recover(&writeback.state);
        for slot in &state.queue {
            assert!(
                matches!(slot.state, SlotState::Prepared { pinned: true }),
                "ambiguous prefix and its known-committed suffix stay pinned"
            );
            assert!(
                slot.record.get().is_some(),
                "every pinned slot retains its finalized write set"
            );
        }
    }

    #[test]
    fn dropping_bound_unknown_finalizes_and_retains_its_write_set() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let unknown = bind(
            writeback
                .reserve(vec![put(b"retained", b"ambiguous")])
                .unwrap(),
            43,
        );
        drop(unknown);

        let state = lock_recover(&writeback.state);
        let slot = state.queue.front().expect("unknown slot remains queued");
        assert!(matches!(slot.state, SlotState::Prepared { pinned: true }));
        let record = slot
            .record
            .get()
            .expect("Drop finalized and retained the ambiguous write set");
        assert_eq!(record.mako_timestamp(), mako_timestamp_of(43));
    }

    #[test]
    fn bind_rejects_unknown_health_that_arose_after_detached_reserve() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(3, 0)).unwrap();
        let first = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        let mut detached_before_unknown = writeback.reserve(vec![put(b"b", b"two")]).unwrap();
        bind(first, 45).pin_unknown().unwrap();

        assert!(matches!(
            detached_before_unknown.bind(mako_timestamp_of(46)),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert!(
            detached_before_unknown.prepared.is_some(),
            "hook-time rejection retains the fully encoded record"
        );
        assert!(
            detached_before_unknown.record.is_some(),
            "hook-time rejection retains the preallocated publication cell"
        );
        assert!(detached_before_unknown.owns_capacity);
        assert_eq!(
            writeback.detached_len(),
            1,
            "rejected bind remains detached until native abort returns"
        );
        assert_eq!(writeback.queue_len(), 1, "rejected bind left no slot");

        drop(detached_before_unknown);
        assert_eq!(
            writeback.detached_len(),
            0,
            "prepared storage is released outside the hook"
        );
    }

    #[test]
    fn flush_covers_safe_acknowledged_prefix_before_later_unknown() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let acknowledged = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 51);
        let later_unknown = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 52);
        later_unknown.pin_unknown().unwrap();
        // A later ambiguity must not prevent acknowledgement and durability of
        // an earlier safe prefix whose native outcome is known.
        acknowledged.publish().unwrap();

        assert_eq!(writeback.highest_acknowledged(), 1);
        assert_eq!(writeback.flush().unwrap(), 1);
        assert_eq!(writeback.durable_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(writeback.queue_len(), 1, "later pinned slot remains queued");
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 2
        ));
    }

    #[test]
    fn unknown_outcome_wakes_a_backpressured_reserver() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 61);

        let (result_tx, result_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let rejected = matches!(
                producer_writeback.reserve(vec![put(b"b", b"two")]),
                Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
            );
            result_tx.send(rejected).unwrap();
        });

        assert!(result_rx.recv_timeout(Duration::from_millis(30)).is_err());
        first.pin_unknown().unwrap();
        assert!(result_rx.recv_timeout(Duration::from_secs(1)).unwrap());
        producer.join().unwrap();
    }

    #[test]
    fn flush_targets_acknowledged_not_reserved_tail() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 40, config(4, 0)).unwrap();
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(mako_timestamp_of(71))
            .unwrap()
            .publish()
            .unwrap();
        let still_prepared = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 72);

        assert_eq!(writeback.highest_acknowledged(), 41);
        assert_eq!(writeback.flush().unwrap(), 41);
        assert_eq!(writeback.durable_sequence(), 41);
        assert_eq!(writeback.queue_len(), 1);

        still_prepared.publish().unwrap();
        assert_eq!(writeback.flush().unwrap(), 42);
        assert_eq!(writeback.durable_sequence(), 42);
    }

    #[test]
    fn flush_progress_wait_does_not_sleep_past_ready_publication() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());
        bind(
            writeback.reserve(vec![put(b"ready", b"value")]).unwrap(),
            73,
        )
        .publish()
        .unwrap();

        let (returned_tx, returned_rx) = mpsc::channel();
        let waiter = Arc::clone(&writeback);
        let thread = std::thread::spawn(move || {
            waiter.wait_for_flush_progress(1);
            returned_tx.send(()).unwrap();
        });

        returned_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("a Ready front must not wait for a second notification");
        thread.join().unwrap();
    }

    #[test]
    fn full_queue_backpressures_until_front_advances() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(mako_timestamp_of(81))
            .unwrap()
            .publish()
            .unwrap();

        let (reserved_tx, reserved_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let reservation = producer_writeback.reserve(vec![put(b"b", b"two")]).unwrap();
            reserved_tx.send(()).unwrap();
            drop(reservation);
        });

        assert!(reserved_rx.recv_timeout(Duration::from_millis(30)).is_err());
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        reserved_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        producer.join().unwrap();
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(writeback.detached_len(), 0);
    }

    #[test]
    fn detached_permit_backpressures_another_reserver_until_drop() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let first = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        assert_eq!(writeback.detached_len(), 1);
        assert_eq!(writeback.queue_len(), 0);

        let (reserved_tx, reserved_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let second = producer_writeback.reserve(vec![put(b"b", b"two")]).unwrap();
            reserved_tx.send(()).unwrap();
            drop(second);
        });

        assert!(reserved_rx.recv_timeout(Duration::from_millis(30)).is_err());
        drop(first);
        reserved_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        producer.join().unwrap();
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[derive(Debug, Default)]
    struct BlockingBlobs {
        inner: MemBlobs,
        gate: Mutex<(bool, bool)>,
        changed: Condvar,
    }

    impl BlockingBlobs {
        fn wait_until_entered(&self) {
            let mut gate = self.gate.lock().unwrap();
            while !gate.0 {
                gate = self.changed.wait(gate).unwrap();
            }
        }

        fn release(&self) {
            let mut gate = self.gate.lock().unwrap();
            gate.1 = true;
            self.changed.notify_all();
        }
    }

    impl Blobs for BlockingBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            let mut gate = self.gate.lock().unwrap();
            gate.0 = true;
            self.changed.notify_all();
            while !gate.1 {
                gate = self.changed.wait(gate).unwrap();
            }
            drop(gate);
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn later_bind_does_not_wait_for_front_backend_io() {
        let backend = Arc::new(BlockingBlobs::default());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 91);
        let second = writeback.reserve(vec![put(b"b", b"two")]).unwrap();
        first.publish().unwrap();

        std::thread::scope(|scope| {
            let consumer = scope.spawn(|| writeback.process_front());
            backend.wait_until_entered();

            let (bound_tx, bound_rx) = mpsc::channel();
            let binder = scope.spawn(move || {
                let second = bind(second, 92);
                bound_tx.send(second.sequence()).unwrap();
                second.publish().unwrap();
            });
            let bound = bound_rx.recv_timeout(Duration::from_secs(1));
            // Always release the consumer before asserting, so a regression
            // reports a failure instead of hanging the scoped-thread join.
            backend.release();

            assert!(
                bound.is_ok(),
                "post-validation bind was serialized behind backend IO"
            );
            assert_eq!(bound.unwrap().get(), 2);
            binder.join().unwrap();
            assert!(matches!(consumer.join().unwrap(), ProcessOutcome::Advanced));
        });

        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.flush().unwrap(), 2);
        assert_eq!(backend.inner.batch_count(), 2);
    }

    #[derive(Debug, Default)]
    struct PanicOnceBlobs {
        inner: MemBlobs,
        panic_next: AtomicBool,
    }

    impl PanicOnceBlobs {
        fn new() -> Self {
            Self {
                inner: MemBlobs::new(),
                panic_next: AtomicBool::new(true),
            }
        }
    }

    impl Blobs for PanicOnceBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            if self.panic_next.swap(false, Ordering::SeqCst) {
                panic!("injected backend panic");
            }
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn poisoned_consumer_retries_the_untouched_ready_record() {
        let writeback = Writeback::new(PanicOnceBlobs::new(), 0, config(2, 0)).unwrap();
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(mako_timestamp_of(101))
            .unwrap()
            .publish()
            .unwrap();

        assert!(catch_unwind(AssertUnwindSafe(|| writeback.process_front())).is_err());
        assert_eq!(writeback.queue_len(), 1);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.backend().inner.batch_count(), 1);
        assert_eq!(writeback.durable_sequence(), 1);
    }
}
