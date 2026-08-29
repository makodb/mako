//! Transaction-ordered, asynchronous RocksDB write-back.
//!
//! Before entering native commit, STO preflights its canonical write set while
//! Rust claims bounded queue capacity and allocates the exact record buffer,
//! but the transaction does not yet receive a cache sequence or occupy an
//! ordered slot. After the complete write set is locked, a per-database native
//! gate orders Mako timestamp assignment, final validation, and the hook. A
//! validation loser leaves only a harmless timestamp gap. A winner receives
//! the next dense cache sequence; native retires the ordering turn, then STO
//! writes the checksummed record directly into that buffer while retaining its
//! write locks and before installation. The hook-time Rust operation is
//! allocation-free and never performs backend IO.
//!
//! A bound slot remains Prepared until native commit returns successfully and
//! Rust attaches the completed bytes. Publishing flips the slot Ready, but a
//! caller receives success only after Ready spans the dense acknowledgement
//! prefix through that slot. An ambiguous post-bind outcome pins the slot, so
//! the background consumer can neither skip it nor mistake it for an abort.
//! The consumer decodes records off the foreground path and replays a bounded
//! contiguous Ready prefix in one atomic backend batch.

use std::collections::VecDeque;
use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

#[cfg(test)]
use std::cell::RefCell;

use mako_local::MakoTimestamp;
use mrx_core::{BlobError, Blobs};

use crate::record::{
    BoundCommitRecord, CommitSeq, Mutation, NativeCommitRecord, PreparedCommitRecord,
    QueuedCommitRecord, RecordError,
};

#[cfg(test)]
thread_local! {
    /// Deterministic materialization failure injection for sequence-bounded
    /// consumer tests. Thread-local storage prevents parallel tests from
    /// perturbing one another or a background worker.
    static TEST_MATERIALIZATION_FAILURE: RefCell<Option<(CommitSeq, RecordError)>> =
        const { RefCell::new(None) };
}

/// Default maximum encoded transaction-record size (8 MiB).
pub const DEFAULT_MAX_RECORD_BYTES: usize = 8 * 1024 * 1024;

/// In-memory progress of the ordered RocksDB consumer.
///
/// `sequence` is the dense, serialization-safe [`CommitSeq`] prefix and is
/// therefore the field that proves contiguity. `mako_timestamp` identifies the
/// record at exactly that frontier. Mako timestamps remain strictly increasing
/// for production cache records but may contain gaps from validation aborts or
/// unrelated native work, so the timestamp alone is not a dense log position.
/// Neither field claims that RocksDB has synced data to disk.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct AppliedWatermark {
    sequence: u64,
    mako_timestamp: Option<MakoTimestamp>,
}

impl AppliedWatermark {
    /// Reconstruct progress from the backend during cache open.
    pub(crate) const fn recovered(sequence: u64, mako_timestamp: Option<MakoTimestamp>) -> Self {
        Self {
            sequence,
            mako_timestamp,
        }
    }

    /// Highest contiguous cache sequence accepted by the backend.
    pub const fn sequence(self) -> u64 {
        self.sequence
    }

    /// Mako timestamp of the transaction at the applied sequence frontier.
    pub const fn mako_timestamp(self) -> Option<MakoTimestamp> {
        self.mako_timestamp
    }

    fn advance(&mut self, sequence: CommitSeq, mako_timestamp: MakoTimestamp) {
        debug_assert_eq!(
            sequence.get(),
            self.sequence + 1,
            "the applied watermark must advance one cache sequence at a time"
        );
        self.sequence = sequence.get();
        self.mako_timestamp = Some(mako_timestamp);
    }
}

/// Controls the bounded commit queue and synchronous drain retries.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WritebackConfig {
    /// Maximum number of detached permits plus prepared or ready slots.
    pub capacity: usize,
    /// Maximum encoded size accepted by native record preflight.
    pub max_record_bytes: usize,
    /// Maximum number of consecutive Ready transactions submitted in one
    /// atomic backend batch.
    pub max_batch_records: usize,
    /// Approximate encoded-log byte budget for one backend batch. A front
    /// record larger than this limit is submitted alone.
    pub max_batch_bytes: usize,
    /// Extra attempts made by [`Writeback::wait_applied`] after a failed backend
    /// write. Zero means that the first failed attempt is returned.
    pub max_apply_retries: usize,
    /// Delay between backend retry attempts.
    pub retry_delay: Duration,
}

impl Default for WritebackConfig {
    fn default() -> Self {
        Self {
            capacity: 1_024,
            max_record_bytes: DEFAULT_MAX_RECORD_BYTES,
            max_batch_records: 64,
            max_batch_bytes: 1024 * 1024,
            max_apply_retries: 8,
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
    /// A zero-record batch can never make progress.
    ZeroBatchRecords,
    /// A zero-byte batch can never make progress.
    ZeroBatchBytes,
    /// A zero retry delay would turn a persistent backend error into a spin
    /// loop.
    ZeroRetryDelay,
    /// The applied seed leaves no sequence number for a future bind.
    SequenceExhausted,
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ZeroCapacity => write!(f, "write-back capacity must be nonzero"),
            Self::ZeroRecordBudget => write!(f, "transaction record budget must be nonzero"),
            Self::ZeroBatchRecords => write!(f, "write-back batch record limit must be nonzero"),
            Self::ZeroBatchBytes => write!(f, "write-back batch byte limit must be nonzero"),
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
    /// A previously acknowledged native record is permanently malformed.
    /// New work cannot safely pass its ordered slot.
    PermanentRecordFailure {
        /// Sequence of the malformed record.
        sequence: CommitSeq,
        /// Exact structural validation failure retained by the queue.
        source: RecordError,
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
            Self::PermanentRecordFailure { sequence, source } => write!(
                f,
                "commit record {} is permanently unreplayable: {source}",
                sequence.get()
            ),
        }
    }
}

impl std::error::Error for ReserveError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Record(error) | Self::PermanentRecordFailure { source: error, .. } => Some(error),
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
#[derive(Clone, Debug, Eq, PartialEq)]
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
    /// A known-committed transaction cannot be acknowledged because an
    /// earlier record is permanently malformed.
    BlockedByPriorRecordFailure {
        /// Sequence of the known-committed transaction retained in the queue.
        sequence: CommitSeq,
        /// Earlier malformed sequence that prevents replay.
        prior_failure: CommitSeq,
        /// Exact structural validation failure retained by the queue.
        source: RecordError,
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
            Self::BlockedByPriorRecordFailure {
                sequence,
                prior_failure,
                source,
            } => write!(
                f,
                "reservation {} cannot be acknowledged because prior record {} is permanently unreplayable: {source}",
                sequence.get(),
                prior_failure.get()
            ),
        }
    }
}

impl std::error::Error for ResolveError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::BlockedByPriorRecordFailure { source, .. } => Some(source),
            Self::Missing { .. }
            | Self::AlreadyResolved { .. }
            | Self::Pinned { .. }
            | Self::BlockedByPriorUnknown { .. } => None,
        }
    }
}

/// A queue drain could not apply its acknowledged snapshot to the backend.
#[derive(Debug, Clone)]
pub enum ApplyError {
    /// A commit at or before the target has an ambiguous native outcome.
    UnknownOutcome {
        /// The first pinned sequence.
        sequence: CommitSeq,
    },
    /// Rocks (or another [`Blobs`] implementation) kept rejecting the atomic
    /// Ready prefix beginning at the front transaction after the configured
    /// retry budget.
    Backend {
        /// Sequence whose atomic backend batch failed.
        sequence: CommitSeq,
        /// Number of failed attempts made by this application barrier.
        attempts: usize,
        /// Last backend error.
        source: BlobError,
    },
    /// A native-produced record could not be decoded for replay. The ordered
    /// slot remains present and later transactions cannot pass it.
    Record {
        /// Sequence of the malformed record.
        sequence: CommitSeq,
        /// Record validation failure.
        source: RecordError,
    },
}

impl fmt::Display for ApplyError {
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
            Self::Record { sequence, source } => write!(
                f,
                "commit record {} cannot be replayed: {source}",
                sequence.get()
            ),
        }
    }
}

impl std::error::Error for ApplyError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::UnknownOutcome { .. } => None,
            Self::Backend { source, .. } => Some(source),
            Self::Record { source, .. } => Some(source),
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
    record: Arc<OnceLock<QueuedCommitRecord>>,
    state: SlotState,
}

/// Stable identity for one slot in the dense ordered queue.
///
/// Slots are only removed from the front and every bound transaction receives
/// the next dense commit sequence. Consequently subtracting the current front
/// sequence from this token gives the slot's current `VecDeque` offset without
/// scanning unrelated transactions.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct QueueToken(CommitSeq);

impl QueueToken {
    const fn new(sequence: CommitSeq) -> Self {
        Self(sequence)
    }

    const fn sequence(self) -> CommitSeq {
        self.0
    }
}

/// Earliest structural record failure discovered by the ordered consumer.
///
/// Allocation failure is deliberately excluded: it does not prove the native
/// bytes are malformed and can succeed on a later replay attempt.
#[derive(Clone, Debug, Eq, PartialEq)]
struct PermanentRecordFailure {
    sequence: CommitSeq,
    source: RecordError,
}

impl PermanentRecordFailure {
    fn apply_error(&self) -> ApplyError {
        ApplyError::Record {
            sequence: self.sequence,
            source: self.source.clone(),
        }
    }

    fn reserve_error(&self) -> ReserveError {
        ReserveError::PermanentRecordFailure {
            sequence: self.sequence,
            source: self.source.clone(),
        }
    }

    fn resolve_error(&self, sequence: CommitSeq) -> ResolveError {
        ResolveError::BlockedByPriorRecordFailure {
            sequence,
            prior_failure: self.sequence,
            source: self.source.clone(),
        }
    }
}

#[derive(Debug)]
struct State {
    queue: VecDeque<Slot>,
    /// Capacity claimed before native commit but not yet attached to `queue`.
    detached: usize,
    last_bound: u64,
    applied: AppliedWatermark,
    highest_acknowledged: u64,
    first_unknown: Option<CommitSeq>,
    permanent_record_failure: Option<PermanentRecordFailure>,
}

impl State {
    /// Resolve a stable queue token to its current `VecDeque` offset.
    ///
    /// A token before the current front has already been consumed; a token
    /// beyond the back was never inserted (or has otherwise gone missing).
    /// Retain the final equality check as a fail-closed guard around the dense
    /// queue invariant.
    fn queue_offset(&self, token: QueueToken) -> Option<usize> {
        let front = self.queue.front()?.sequence;
        let offset = token
            .sequence()
            .get()
            .checked_sub(front.get())
            .and_then(|offset| usize::try_from(offset).ok())?;
        self.queue
            .get(offset)
            .is_some_and(|slot| slot.sequence == token.sequence())
            .then_some(offset)
    }

    /// Extend the caller-visible acknowledgement frontier across the maximal
    /// contiguous Ready prefix after the current frontier.
    ///
    /// Ready publication itself may happen out of order. Keeping this frontier
    /// dense ensures that returning success for sequence N also proves every
    /// lower bound native commit has a known-successful outcome. Already
    /// applied slots need no special case: the consumer only removes records
    /// after they have crossed this acknowledgement frontier.
    fn advance_acknowledged_prefix(&mut self) {
        while let Some(raw_next) = self.highest_acknowledged.checked_add(1) {
            let Some(next) = CommitSeq::new(raw_next) else {
                break;
            };
            let Some(offset) = self.queue_offset(QueueToken::new(next)) else {
                break;
            };
            if self.queue[offset].state != SlotState::Ready {
                break;
            }
            self.highest_acknowledged = raw_next;
        }
    }

    /// Return the earliest fail-stop condition that rejects new work.
    fn reserve_health_error(&self) -> Option<ReserveError> {
        match (&self.permanent_record_failure, self.first_unknown) {
            (Some(failure), Some(unknown)) if unknown < failure.sequence => {
                Some(ReserveError::UnknownOutcome { sequence: unknown })
            }
            (Some(failure), _) => Some(failure.reserve_error()),
            (None, Some(sequence)) => Some(ReserveError::UnknownOutcome { sequence }),
            (None, None) => None,
        }
    }

    /// Return the earliest fail-stop condition relevant to `target`.
    fn apply_health_error_through(&self, target: u64) -> Option<ApplyError> {
        let record = self
            .permanent_record_failure
            .as_ref()
            .filter(|failure| failure.sequence.get() <= target);
        let unknown = self
            .first_unknown
            .filter(|sequence| sequence.get() <= target);
        match (record, unknown) {
            (Some(failure), Some(sequence)) if sequence < failure.sequence => {
                Some(ApplyError::UnknownOutcome { sequence })
            }
            (Some(failure), _) => Some(failure.apply_error()),
            (None, Some(sequence)) => Some(ApplyError::UnknownOutcome { sequence }),
            (None, None) => None,
        }
    }

    /// Return any fail-stop health condition, including one after an already
    /// applied target. Clean shutdown uses this stronger check.
    fn health_error(&self) -> Option<ApplyError> {
        self.apply_health_error_through(u64::MAX)
    }
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
    acknowledgement_changed: Condvar,
    capacity_available: Condvar,
    consumer: Mutex<()>,
}

impl<B: Blobs> Writeback<B> {
    /// Create an empty queue whose first reservation is `applied_seed + 1`.
    ///
    /// This constructor is used by tests that only need a sequence seed. Cache
    /// recovery uses [`Self::new_with_watermark`] to retain the recovered Mako
    /// timestamp as well.
    #[cfg(test)]
    pub fn new(
        backend: B,
        applied_seed: u64,
        config: WritebackConfig,
    ) -> Result<Self, ConfigError> {
        Self::new_with_watermark(
            backend,
            AppliedWatermark::recovered(applied_seed, None),
            config,
        )
    }

    /// Create a queue from progress reconstructed while opening the backend.
    pub(crate) fn new_with_watermark(
        backend: B,
        applied_seed: AppliedWatermark,
        config: WritebackConfig,
    ) -> Result<Self, ConfigError> {
        if config.capacity == 0 {
            return Err(ConfigError::ZeroCapacity);
        }
        if config.max_record_bytes == 0 {
            return Err(ConfigError::ZeroRecordBudget);
        }
        if config.max_batch_records == 0 {
            return Err(ConfigError::ZeroBatchRecords);
        }
        if config.max_batch_bytes == 0 {
            return Err(ConfigError::ZeroBatchBytes);
        }
        if config.retry_delay.is_zero() {
            return Err(ConfigError::ZeroRetryDelay);
        }
        if applied_seed.sequence == u64::MAX {
            return Err(ConfigError::SequenceExhausted);
        }

        Ok(Self {
            backend,
            config,
            state: Mutex::new(State {
                queue: VecDeque::with_capacity(config.capacity),
                detached: 0,
                last_bound: applied_seed.sequence,
                applied: applied_seed,
                highest_acknowledged: applied_seed.sequence,
                first_unknown: None,
                permanent_record_failure: None,
            }),
            changed: Condvar::new(),
            acknowledgement_changed: Condvar::new(),
            capacity_available: Condvar::new(),
            consumer: Mutex::new(()),
        })
    }

    /// Access the underlying backend.
    pub fn backend(&self) -> &B {
        &self.backend
    }

    /// Maximum native/log record extent accepted by this queue.
    pub(crate) const fn max_record_bytes(&self) -> usize {
        self.config.max_record_bytes
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
            native_record: false,
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

    /// Claim queue capacity and preallocate a publication cell for a record
    /// that the trusted native transaction adapter will serialize directly.
    ///
    /// The exact byte buffer is owned by `mako-local` across the synchronous
    /// native commit call. This permit owns only the bounded queue slot and
    /// therefore adds no duplicate write-set representation.
    pub(crate) fn reserve_native(
        &self,
        record_bytes: usize,
    ) -> Result<DetachedPermit<'_, B>, ReserveError> {
        if record_bytes == 0 {
            return Err(ReserveError::Record(RecordError::Truncated));
        }
        if record_bytes > self.config.max_record_bytes {
            return Err(ReserveError::Record(RecordError::RecordTooLarge {
                size: record_bytes,
                max: self.config.max_record_bytes,
            }));
        }
        self.claim_detached_capacity()?;
        Ok(DetachedPermit {
            owner: self,
            prepared: None,
            native_record: true,
            record: Some(Arc::new(OnceLock::new())),
            owns_capacity: true,
        })
    }

    fn claim_detached_capacity(&self) -> Result<(), ReserveError> {
        let mut state = lock_recover(&self.state);
        loop {
            if let Some(error) = state.reserve_health_error() {
                return Err(error);
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
        // Exactly one bounded-capacity claim became available. A detached
        // cancellation neither creates consumer work nor changes an applied
        // target, so the consumer/activity condition needs no notification.
        self.capacity_available.notify_one();
    }

    /// Current in-memory applied watermark.
    pub fn applied_watermark(&self) -> AppliedWatermark {
        lock_recover(&self.state).applied
    }

    /// Highest contiguous bound record accepted atomically by the backend.
    pub fn applied_sequence(&self) -> u64 {
        self.applied_watermark().sequence()
    }

    /// Highest dense sequence prefix acknowledged to callers as successful
    /// native commits. This is deliberately distinct from both the queue tail
    /// and out-of-order Ready publication.
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

    /// Snapshot the highest acknowledged sequence and apply that snapshot to
    /// the backend.
    ///
    /// Transactions acknowledged after the snapshot are intentionally not
    /// part of this barrier. That includes a later pinned unknown slot: it does
    /// not invalidate an earlier acknowledged prefix, although clean shutdown
    /// separately rejects any unknown outcome. Consecutive ready records are
    /// submitted in bounded atomic `write_batch` calls. A failed prefix remains
    /// unchanged at the front for the next retry.
    pub fn wait_applied(&self) -> Result<u64, ApplyError> {
        let target = {
            let state = lock_recover(&self.state);
            state.highest_acknowledged
        };
        self.wait_applied_through(target)
    }

    /// Apply exactly one caller-visible acknowledgement snapshot.
    ///
    /// The background consumer may batch every currently Ready slot, but a
    /// synchronous barrier must not inspect or materialize a suffix published
    /// after it captured `target`. Otherwise corruption or transient memory
    /// pressure in later work can incorrectly fail an earlier barrier.
    fn wait_applied_through(&self, target: u64) -> Result<u64, ApplyError> {
        if let Some(error) = lock_recover(&self.state).apply_health_error_through(target) {
            return Err(error);
        }

        let mut failed_sequence = None;
        let mut failed_attempts = 0usize;

        loop {
            {
                let state = lock_recover(&self.state);
                if let Some(error) = state.apply_health_error_through(target) {
                    return Err(error);
                }
                if state.applied.sequence >= target {
                    return Ok(target);
                }
            }

            match self.process_front_through(Some(target)) {
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

                    if failed_attempts > self.config.max_apply_retries {
                        // A concurrent consumer may have recovered between our
                        // failed attempt and this decision.
                        let applied = lock_recover(&self.state).applied.sequence;
                        match retry_progress(applied, target, sequence) {
                            RetryProgress::TargetApplied => return Ok(target),
                            RetryProgress::FailedSequenceApplied => {
                                failed_sequence = None;
                                failed_attempts = 0;
                                continue;
                            }
                            RetryProgress::NoProgress => {}
                        }
                        return Err(ApplyError::Backend {
                            sequence,
                            attempts: failed_attempts,
                            source: error,
                        });
                    }
                    self.wait_for_activity(self.config.retry_delay);
                }
                ProcessOutcome::RecordFailed { sequence, error } => {
                    if sequence.get() <= target {
                        return Err(ApplyError::Record {
                            sequence,
                            source: error,
                        });
                    }
                    // The sequence cap makes this unreachable for a healthy
                    // dense queue. Retain the defensive branch for a cached
                    // concurrent failure, then re-check applied progress.
                }
                ProcessOutcome::Blocked | ProcessOutcome::Idle => {
                    self.wait_for_apply_progress(target);
                }
                ProcessOutcome::Pinned(sequence) => {
                    return Err(ApplyError::UnknownOutcome { sequence });
                }
            }
        }
    }

    fn resolve(&self, token: QueueToken, resolution: Resolution) -> Result<(), ResolveError> {
        let mut state = lock_recover(&self.state);
        let sequence = token.sequence();
        let prior_unknown = state.first_unknown.filter(|unknown| *unknown < sequence);
        let prior_record_failure = state
            .permanent_record_failure
            .as_ref()
            .filter(|failure| failure.sequence < sequence)
            .cloned();
        let Some(offset) = state.queue_offset(token) else {
            return Err(ResolveError::Missing { sequence });
        };
        match state.queue[offset].state {
            SlotState::Prepared { pinned: true } => {
                return Err(ResolveError::Pinned { sequence });
            }
            SlotState::Prepared { pinned: false } => {}
            SlotState::Ready => {
                return Err(ResolveError::AlreadyResolved { sequence });
            }
        }

        let mut wake_ready_front = false;
        let mut wake_acknowledgement = false;
        let mut wake_fail_stop = false;
        let result = match resolution {
            Resolution::Publish => {
                if let Some(prior_unknown) = prior_unknown.filter(|prior_unknown| {
                    prior_record_failure
                        .as_ref()
                        .is_none_or(|failure| *prior_unknown < failure.sequence)
                }) {
                    // The current transaction is known committed, but callers
                    // must not observe an acknowledgement beyond an ambiguous
                    // prefix. Retain it as a pinned Prepared slot.
                    state.queue[offset].state = SlotState::Prepared { pinned: true };
                    Err(ResolveError::BlockedByPriorUnknown {
                        sequence,
                        prior_unknown,
                    })
                } else if let Some(failure) = prior_record_failure {
                    // This transaction is known committed and retains its
                    // complete record, but no caller acknowledgement may cross
                    // a permanently malformed earlier record.
                    state.queue[offset].state = SlotState::Ready;
                    Err(failure.resolve_error(sequence))
                } else {
                    state.queue[offset].state = SlotState::Ready;
                    state.advance_acknowledged_prefix();
                    // If this publication closed a hole and advanced across a
                    // later Ready suffix, those publishers are parked on the
                    // acknowledgement condition. The ordinary in-order fast
                    // path does not broadcast when no later sequence became
                    // newly acknowledged.
                    wake_acknowledgement = state.highest_acknowledged > sequence.get();
                    // A later Ready slot is still blocked by the current front.
                    // Only publishing the front can create work for a sleeping
                    // consumer or synchronous application waiter.
                    wake_ready_front = offset == 0;

                    // Ready publication and caller acknowledgement are distinct
                    // transitions. A later native commit may finish first and
                    // expose its immutable record to the ordered consumer, but
                    // it cannot return success across a Prepared hole.
                    #[cfg(test)]
                    self.acknowledgement_changed.notify_all();
                    loop {
                        if state.highest_acknowledged >= sequence.get() {
                            break Ok(());
                        }
                        let prior_unknown =
                            state.first_unknown.filter(|unknown| *unknown < sequence);
                        let prior_failure = state
                            .permanent_record_failure
                            .as_ref()
                            .filter(|failure| failure.sequence < sequence)
                            .cloned();
                        if let Some(prior_unknown) = prior_unknown.filter(|prior_unknown| {
                            prior_failure
                                .as_ref()
                                .is_none_or(|failure| *prior_unknown < failure.sequence)
                        }) {
                            let current_offset = state.queue_offset(token).expect(
                                "an unacknowledged Ready slot cannot be consumed from the queue",
                            );
                            let current = &mut state.queue[current_offset];
                            assert_eq!(
                                current.state,
                                SlotState::Ready,
                                "a waiting publication remains Ready until its prefix resolves"
                            );
                            // This native transaction is known committed. Keep
                            // its complete record, but pin it behind the earlier
                            // ambiguity so neither replay nor caller-visible
                            // acknowledgement can cross the unknown outcome.
                            current.state = SlotState::Prepared { pinned: true };
                            break Err(ResolveError::BlockedByPriorUnknown {
                                sequence,
                                prior_unknown,
                            });
                        }
                        if let Some(failure) = prior_failure {
                            break Err(failure.resolve_error(sequence));
                        }
                        state = wait_recover(&self.acknowledgement_changed, state);
                    }
                }
            }
            Resolution::PinUnknown => {
                state.queue[offset].state = SlotState::Prepared { pinned: true };
                state.first_unknown = Some(match state.first_unknown {
                    Some(current) => current.min(sequence),
                    None => sequence,
                });
                wake_fail_stop = true;
                Ok(())
            }
        };

        drop(state);
        if wake_fail_stop {
            // Every application/capacity waiter must observe the permanent
            // fail-stop state rather than remain parked behind a queue that can
            // no longer advance.
            self.changed.notify_all();
            self.acknowledgement_changed.notify_all();
            self.capacity_available.notify_all();
        } else {
            if wake_acknowledgement {
                self.acknowledgement_changed.notify_all();
            }
            if wake_ready_front {
                // Any activity waiter can drive the single serialized consumer;
                // waking one avoids a notify-all herd on the normal commit path.
                self.changed.notify_one();
            }
        }
        result
    }

    pub(crate) fn process_front(&self) -> ProcessOutcome {
        self.process_front_through(None)
    }

    /// Process a bounded Ready prefix, optionally capped at an inclusive
    /// sequence. `None` is the background worker's ordinary unbounded mode;
    /// synchronous barriers pass their immutable acknowledgement snapshot.
    fn process_front_through(&self, max_sequence: Option<u64>) -> ProcessOutcome {
        // This guard is separate from the queue state so the single-consumer
        // rule remains explicit if the state lock is narrowed around IO later.
        // Both locks deliberately recover poison: if a backend panics, the
        // untouched Ready prefix is safe to retry.
        let _consumer = lock_recover(&self.consumer);
        let state = lock_recover(&self.state);

        let Some(front) = state.queue.front() else {
            return ProcessOutcome::Idle;
        };
        let first_sequence = front.sequence;
        if max_sequence.is_some_and(|maximum| first_sequence.get() > maximum) {
            return ProcessOutcome::Idle;
        }
        let permanent_record_failure = state.permanent_record_failure.clone();
        if let Some(failure) = permanent_record_failure
            .as_ref()
            .filter(|failure| failure.sequence <= first_sequence)
        {
            // The first discovery left this exact slot in place. Never decode
            // it again: structural invalidity cannot be repaired by retrying.
            return ProcessOutcome::RecordFailed {
                sequence: failure.sequence,
                error: failure.source.clone(),
            };
        }
        match front.state {
            SlotState::Prepared { pinned: false } => return ProcessOutcome::Blocked,
            SlotState::Prepared { pinned: true } => return ProcessOutcome::Pinned(first_sequence),
            SlotState::Ready => {}
        }

        // Capture a bounded contiguous Ready prefix. Prepared and pinned slots
        // are ordering barriers; a later Ready transaction must never pass
        // either one. Arc ownership keeps every exact record alive after the
        // queue lock is dropped for decoding and backend IO.
        let mut cells = Vec::new();
        let mut encoded_bytes = 0usize;
        for slot in &state.queue {
            if slot.state != SlotState::Ready || cells.len() == self.config.max_batch_records {
                break;
            }
            if max_sequence.is_some_and(|maximum| slot.sequence.get() > maximum) {
                break;
            }
            if permanent_record_failure
                .as_ref()
                .is_some_and(|failure| slot.sequence >= failure.sequence)
            {
                // A prior application barrier may still drain a safe Ready
                // prefix before the later permanent failure.
                break;
            }
            let record = slot
                .record
                .get()
                .expect("a Ready slot must contain its finalized record");
            let next_bytes = encoded_bytes.saturating_add(record.encoded_len());
            if !cells.is_empty() && next_bytes > self.config.max_batch_bytes {
                break;
            }
            encoded_bytes = next_bytes;
            cells.push(Arc::clone(&slot.record));
        }
        assert!(
            !cells.is_empty(),
            "a Ready front must form a nonempty batch"
        );
        drop(state);

        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::ReadyBeforeBackend);

        // Native records are decoded and materialized only on this background
        // path. Keeping the owned decoded records together lets one flat
        // BlobOp vector borrow from all of them for the synchronous batch call.
        let mut records = Vec::with_capacity(cells.len());
        for cell in &cells {
            let queued = cell
                .get()
                .expect("a captured Ready slot retains its record");
            match self.materialize_queued_record(queued) {
                Ok(record) => records.push(record),
                Err(error) => return self.record_failure_outcome(queued.sequence(), error),
            }
        }

        let operation_count = records.iter().fold(0usize, |total, record| {
            total.saturating_add(record.backend_op_count())
        });
        let mut operations = Vec::with_capacity(operation_count);
        for record in &records {
            record.append_backend_ops(&mut operations);
        }
        let result = self.backend.write_batch(&operations);

        match result {
            Ok(()) => {
                #[cfg(test)]
                crate::failpoint::hit(crate::failpoint::Point::BackendWrittenBeforeApplied);
                let mut state = lock_recover(&self.state);
                for record in &records {
                    let current = state
                        .queue
                        .front()
                        .expect("serialized consumer keeps the captured prefix present");
                    assert_eq!(
                        current.sequence,
                        record.sequence(),
                        "serialized consumer changed the captured prefix"
                    );
                    assert_eq!(
                        current.state,
                        SlotState::Ready,
                        "captured Ready slot changed state during backend IO"
                    );
                    state.queue.pop_front();
                    state
                        .applied
                        .advance(record.sequence(), record.mako_timestamp());
                }
                #[cfg(test)]
                crate::failpoint::hit(crate::failpoint::Point::AppliedAdvanced);
                drop(state);
                // A whole prefix became free. Wake all bounded-capacity
                // producers once, and every barrier/consumer observing the
                // new applied frontier once.
                self.capacity_available.notify_all();
                self.changed.notify_all();
                ProcessOutcome::Advanced
            }
            Err(error) => ProcessOutcome::BackendFailed {
                sequence: first_sequence,
                error,
            },
        }
    }

    fn materialize_queued_record(
        &self,
        queued: &QueuedCommitRecord,
    ) -> Result<crate::record::CommitRecord, RecordError> {
        #[cfg(test)]
        if let Some(error) = TEST_MATERIALIZATION_FAILURE.with(|failure| {
            failure
                .borrow()
                .as_ref()
                .filter(|(sequence, _)| *sequence == queued.sequence())
                .map(|(_, error)| error.clone())
        }) {
            return Err(error);
        }

        queued.materialize(self.config.max_record_bytes)
    }

    /// Classify a replay-materialization failure and retain structural errors
    /// as a permanent queue-health condition.
    ///
    /// Allocation failure is transient: the Ready slot remains untouched and
    /// a future background or synchronous consumer may retry materialization.
    fn record_failure_outcome(&self, sequence: CommitSeq, error: RecordError) -> ProcessOutcome {
        if error == RecordError::AllocationFailed {
            return ProcessOutcome::RecordFailed { sequence, error };
        }

        let failure = {
            let mut state = lock_recover(&self.state);
            let replace = state
                .permanent_record_failure
                .as_ref()
                .is_none_or(|current| sequence < current.sequence);
            if replace {
                state.permanent_record_failure = Some(PermanentRecordFailure {
                    sequence,
                    source: error,
                });
            }
            state
                .permanent_record_failure
                .clone()
                .expect("a structural record failure was just latched")
        };

        // Every kind of waiter must re-check queue health. In particular, a
        // capacity waiter must not remain parked behind the malformed Ready
        // slot and an out-of-order publisher must not wait forever for an
        // acknowledgement frontier that can no longer be safely consumed.
        self.changed.notify_all();
        self.acknowledgement_changed.notify_all();
        self.capacity_available.notify_all();

        ProcessOutcome::RecordFailed {
            sequence: failure.sequence,
            error: failure.source,
        }
    }

    pub(crate) fn wait_for_activity(&self, timeout: Duration) {
        let state = lock_recover(&self.state);
        drop(wait_timeout_recover(&self.changed, state, timeout));
    }

    pub(crate) fn retry_delay(&self) -> Duration {
        if lock_recover(&self.state).permanent_record_failure.is_some() {
            // Runtime stop still interrupts this through `wake_waiters`. A
            // permanent structural failure cannot improve through retrying,
            // so keep the worker asleep instead of re-decoding the same bytes
            // at the ordinary transient-error cadence.
            Duration::MAX
        } else {
            self.config.retry_delay
        }
    }

    pub(crate) fn ensure_no_unknown(&self) -> Result<(), ApplyError> {
        match lock_recover(&self.state).health_error() {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    pub(crate) fn wake_waiters(&self) {
        self.changed.notify_all();
        self.acknowledgement_changed.notify_all();
        self.capacity_available.notify_all();
    }

    fn wait_for_apply_progress(&self, target: u64) {
        let state = lock_recover(&self.state);
        if state.applied.sequence >= target {
            return;
        }
        if state.apply_health_error_through(target).is_some() {
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
    TargetApplied,
    FailedSequenceApplied,
    NoProgress,
}

fn retry_progress(applied: u64, target: u64, failed: CommitSeq) -> RetryProgress {
    if applied >= target {
        RetryProgress::TargetApplied
    } else if applied >= failed.get() {
        RetryProgress::FailedSequenceApplied
    } else {
        RetryProgress::NoProgress
    }
}

/// A bounded capacity claim that has not entered the ordered commit log.
///
/// This value is created before native commit. Dropping it means Silo failed
/// before reaching the post-validation hook: capacity is released without
/// assigning a sequence or leaving a cancellation marker.
pub struct DetachedPermit<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    prepared: Option<PreparedCommitRecord>,
    native_record: bool,
    record: Option<Arc<OnceLock<QueuedCommitRecord>>>,
    owns_capacity: bool,
}

impl<'a, B: Blobs> DetachedPermit<'a, B> {
    /// Bind this record in Silo's ordered post-validation, pre-install hook.
    ///
    /// Production callers enter this hook under native's per-database
    /// validation gate, so successful binds follow a legal Silo serialization
    /// order even though failed validations consumed no slot. This assigns the
    /// next cache sequence, embeds Mako's transaction timestamp, and appends
    /// one Prepared slot. The method performs no heap
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
        assert!(
            !self.native_record,
            "native record permits must use bind_native"
        );
        self.bind_inner(mako_timestamp)
    }

    /// Assign a dense sequence and insert an empty Prepared slot for a record
    /// being filled directly by native code. The returned reservation must be
    /// given the completed native bytes before it is published or pinned.
    pub(crate) fn bind_native(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<BoundReservation<'a, B>, ReserveError> {
        assert!(
            self.native_record,
            "materialized record permits must use bind"
        );
        self.bind_inner(mako_timestamp)
    }

    fn bind_inner(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<BoundReservation<'a, B>, ReserveError> {
        let mut state = lock_recover(&self.owner.state);
        if let Some(error) = state.reserve_health_error() {
            // Leave the complete prepared record on this permit. The caller
            // is inside Silo's hook and will drop it only after native abort
            // has returned and released the write locks.
            return Err(error);
        }
        let raw_sequence = state
            .last_bound
            .checked_add(1)
            .expect("detached capacity guarantees commit sequence space");
        let sequence = CommitSeq::new(raw_sequence)
            .expect("the sequence after a valid applied seed is nonzero");
        let record = self
            .record
            .take()
            .expect("a detached permit owns one preallocated record cell");
        let bound = self
            .prepared
            .take()
            .map(|prepared| prepared.bind(sequence, mako_timestamp));
        assert_eq!(
            bound.is_none(),
            self.native_record,
            "detached permit record kind changed before binding"
        );

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
        // Binding only replaces one detached capacity claim with one Prepared
        // queue slot; it creates no consumer work and frees no capacity. The
        // sole exceptional transition is assigning the final sequence, which
        // wakes every sequence-space waiter so each can fail closed instead of
        // remaining parked forever.
        if raw_sequence == u64::MAX {
            self.owner.capacity_available.notify_all();
        }

        Ok(BoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            bound,
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
    token: QueueToken,
    mako_timestamp: MakoTimestamp,
    bound: Option<BoundCommitRecord>,
    record: Arc<OnceLock<QueuedCommitRecord>>,
    on_drop: DropAction,
}

impl<B: Blobs> BoundReservation<'_, B> {
    /// Return the cache sequence assigned at the native serialization hook.
    pub const fn sequence(&self) -> CommitSeq {
        self.token.sequence()
    }

    /// Attach the exact bytes initialized by the trusted native serializer.
    /// This is constant-time and allocation-free; decoding is deferred to the
    /// background replay path.
    pub(crate) fn attach_native_record(&mut self, encoded: Vec<u8>) {
        assert!(
            self.bound.is_none(),
            "a materialized reservation cannot accept native bytes"
        );
        let record =
            NativeCommitRecord::from_native(self.token.sequence(), self.mako_timestamp, encoded);
        assert!(
            self.record.set(QueuedCommitRecord::Native(record)).is_ok(),
            "a native record may be attached exactly once"
        );
    }

    /// Publish a successfully committed transaction.
    ///
    /// A native reservation already has its complete checksummed bytes attached.
    /// The legacy materialized test path finalizes its preallocated record here;
    /// either way the complete record is installed before the slot becomes
    /// Ready. If an earlier bound transaction has not published yet, this call
    /// waits without holding the queue mutex until that dense acknowledgement
    /// prefix catches up. An earlier unknown outcome instead retains this
    /// known-committed record as pinned and returns
    /// [`ResolveError::BlockedByPriorUnknown`].
    pub fn publish(mut self) -> Result<CommitSeq, ResolveError> {
        self.finalize_once();
        assert!(
            self.record.get().is_some(),
            "a bound slot must own a complete record before publication"
        );
        self.owner.resolve(self.token, Resolution::Publish)?;
        self.on_drop = DropAction::Done;
        Ok(self.token.sequence())
    }

    /// Permanently pin the slot because the native commit outcome is unknown.
    ///
    /// The write-back layer cannot safely skip such a slot. Flushes fail and
    /// new reservations are rejected until the database is reopened and
    /// recovered by a higher-level protocol.
    pub fn pin_unknown(mut self) -> Result<CommitSeq, ResolveError> {
        self.on_drop = DropAction::PinUnknown;
        self.finalize_once();
        assert!(
            self.record.get().is_some(),
            "a bound slot must retain its complete record before pinning"
        );
        self.owner.resolve(self.token, Resolution::PinUnknown)?;
        self.on_drop = DropAction::Done;
        Ok(self.token.sequence())
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
        let _ = self.record.set(QueuedCommitRecord::Materialized(record));
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
            let _ = self.owner.resolve(self.token, resolution);
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
    RecordFailed {
        sequence: CommitSeq,
        error: RecordError,
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
    use std::sync::{mpsc, Arc, Barrier};
    use std::time::Instant;

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

    fn config(capacity: usize, max_apply_retries: usize) -> WritebackConfig {
        WritebackConfig {
            capacity,
            // Legacy tests exercise one-at-a-time application semantics. New
            // prefix tests opt into larger batches explicitly below.
            max_batch_records: 1,
            max_apply_retries,
            retry_delay: Duration::from_millis(1),
            ..WritebackConfig::default()
        }
    }

    fn batch_config(
        capacity: usize,
        max_batch_records: usize,
        max_batch_bytes: usize,
    ) -> WritebackConfig {
        WritebackConfig {
            max_batch_records,
            max_batch_bytes,
            ..config(capacity, 0)
        }
    }

    fn record_encoded_len(mutations: Vec<Mutation>) -> usize {
        PreparedCommitRecord::prepare(mutations, DEFAULT_MAX_RECORD_BYTES)
            .unwrap()
            .bind(CommitSeq::new(1).unwrap(), mako_timestamp_of(1))
            .finalize()
            .encoded()
            .len()
    }

    fn encoded_native_record(
        raw_sequence: u64,
        raw_mako_timestamp: u32,
        mutations: Vec<Mutation>,
    ) -> Vec<u8> {
        PreparedCommitRecord::prepare(mutations, DEFAULT_MAX_RECORD_BYTES)
            .unwrap()
            .bind(
                CommitSeq::new(raw_sequence).expect("test sequence is nonzero"),
                mako_timestamp_of(raw_mako_timestamp),
            )
            .finalize()
            .encoded()
            .to_vec()
    }

    fn publish_native_bytes<B: Blobs>(
        writeback: &Writeback<B>,
        raw_mako_timestamp: u32,
        encoded: Vec<u8>,
    ) -> CommitSeq {
        let mut permit = writeback.reserve_native(encoded.len()).unwrap();
        let mut reservation = permit
            .bind_native(mako_timestamp_of(raw_mako_timestamp))
            .unwrap();
        reservation.attach_native_record(encoded);
        reservation.publish().unwrap()
    }

    struct MaterializationFailureGuard;

    impl Drop for MaterializationFailureGuard {
        fn drop(&mut self) {
            TEST_MATERIALIZATION_FAILURE.with(|failure| {
                *failure.borrow_mut() = None;
            });
        }
    }

    fn force_materialization_failure(
        sequence: CommitSeq,
        error: RecordError,
    ) -> MaterializationFailureGuard {
        TEST_MATERIALIZATION_FAILURE.with(|failure| {
            let previous = failure.borrow_mut().replace((sequence, error));
            assert!(previous.is_none(), "test failure injection already armed");
        });
        MaterializationFailureGuard
    }

    fn bind<'a, B: Blobs>(
        mut permit: DetachedPermit<'a, B>,
        mako_timestamp: u32,
    ) -> BoundReservation<'a, B> {
        permit.bind(mako_timestamp_of(mako_timestamp)).unwrap()
    }

    fn publish<B: Blobs>(
        writeback: &Writeback<B>,
        mutations: Vec<Mutation>,
        mako_timestamp: u32,
    ) -> CommitSeq {
        writeback
            .reserve(mutations)
            .unwrap()
            .bind(mako_timestamp_of(mako_timestamp))
            .unwrap()
            .publish()
            .unwrap()
    }

    fn mako_timestamp_of(raw: u32) -> MakoTimestamp {
        MakoTimestamp::new(raw).expect("test Mako timestamps are nonzero")
    }

    fn wait_until_ready<B: Blobs>(writeback: &Writeback<B>, raw_sequence: u64) {
        let sequence = CommitSeq::new(raw_sequence).expect("test sequence is nonzero");
        let token = QueueToken::new(sequence);
        let deadline = Instant::now() + Duration::from_secs(1);
        let mut state = lock_recover(&writeback.state);
        loop {
            let offset = state
                .queue_offset(token)
                .expect("publication slot remains queued while acknowledgement waits");
            if state.queue[offset].state == SlotState::Ready {
                return;
            }

            let remaining = deadline
                .checked_duration_since(Instant::now())
                .expect("publication did not make its slot Ready within one second");
            let (next_state, timeout) = writeback
                .acknowledgement_changed
                .wait_timeout(state, remaining)
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            state = next_state;
            if timeout.timed_out() {
                let offset = state
                    .queue_offset(token)
                    .expect("timed-out publication slot remains queued");
                assert_eq!(
                    state.queue[offset].state,
                    SlotState::Ready,
                    "publication did not make its slot Ready within one second"
                );
                return;
            }
        }
    }

    #[derive(Clone, Debug, Eq, PartialEq)]
    enum OwnedBlobOp {
        Put { key: Vec<u8>, value: Vec<u8> },
        Delete { key: Vec<u8> },
    }

    impl OwnedBlobOp {
        fn capture(operation: &BlobOp<'_>) -> Self {
            match operation {
                BlobOp::Put { key, val } => Self::Put {
                    key: key.to_vec(),
                    value: val.to_vec(),
                },
                BlobOp::Delete { key } => Self::Delete { key: key.to_vec() },
            }
        }
    }

    #[derive(Debug, Default)]
    struct RecordingBlobs {
        inner: MemBlobs,
        attempts: Mutex<Vec<Vec<OwnedBlobOp>>>,
    }

    impl RecordingBlobs {
        fn fail_next_writes(&self, count: usize) {
            self.inner.fail_next_writes(count);
        }

        fn attempts(&self) -> Vec<Vec<OwnedBlobOp>> {
            lock_recover(&self.attempts).clone()
        }
    }

    impl Blobs for RecordingBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            lock_recover(&self.attempts)
                .push(operations.iter().map(OwnedBlobOp::capture).collect());
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
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

        assert_eq!(writeback.wait_applied().unwrap(), 1);
        assert_eq!(backend.batch_count(), 1);
        // One backend commit-record entry plus the transaction's two data ops.
        assert_eq!(backend.op_count(), 3);
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(1, Some(mako_timestamp_of(11)))
        );
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn contiguous_ready_prefix_uses_one_backend_call() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(4, 4, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 111);
        publish(&writeback, vec![put(b"b", b"two")], 112);
        publish(&writeback, vec![put(b"c", b"three")], 113);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 6, "each record contributes log + data");
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn ready_prefix_respects_record_count_cap() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(5, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 121);
        publish(&writeback, vec![put(b"b", b"two")], 122);
        publish(&writeback, vec![put(b"c", b"three")], 123);
        publish(&writeback, vec![put(b"d", b"four")], 124);
        publish(&writeback, vec![put(b"e", b"five")], 125);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(writeback.queue_len(), 3);
        assert_eq!(backend.batch_count(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 4);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 2);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 5);
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(backend.batch_count(), 3);
    }

    #[test]
    fn ready_prefix_respects_encoded_byte_cap() {
        let record_bytes = record_encoded_len(vec![put(b"a", b"one")]);
        let two_record_cap = record_bytes.checked_mul(2).unwrap();
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(3, 3, two_record_cap)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 131);
        publish(&writeback, vec![put(b"b", b"two")], 132);
        publish(&writeback, vec![put(b"c", b"six")], 133);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 4);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn oversized_ready_front_is_submitted_alone() {
        let large_value = vec![b'x'; 128];
        let oversized_mutation = put(b"large", &large_value);
        let oversized_bytes = record_encoded_len(vec![oversized_mutation.clone()]);
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(
            Arc::clone(&backend),
            0,
            batch_config(2, 2, oversized_bytes - 1),
        )
        .unwrap();
        publish(&writeback, vec![oversized_mutation], 141);
        publish(&writeback, vec![put(b"small", b"value")], 142);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn prepared_and_pinned_slots_cut_the_ready_prefix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(3, 3, usize::MAX)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 151);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 152);
        let third = bind(writeback.reserve(vec![put(b"c", b"three")]).unwrap(), 153);
        first.publish().unwrap();
        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(third.publish()).unwrap());
            wait_until_ready(&writeback, 3);
            assert_eq!(writeback.highest_acknowledged(), 1);

            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
            assert_eq!(writeback.applied_sequence(), 1);
            assert_eq!(writeback.queue_len(), 2);
            assert_eq!(backend.batch_count(), 1);
            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));

            second.pin_unknown().unwrap();
            assert!(matches!(
                published_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
                Err(ResolveError::BlockedByPriorUnknown {
                    sequence,
                    prior_unknown,
                }) if sequence.get() == 3 && prior_unknown.get() == 2
            ));
            publisher.join().unwrap();
        });
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 2
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 2);
        assert_eq!(backend.batch_count(), 1);
        let state = lock_recover(&writeback.state);
        assert_eq!(state.queue[0].state, SlotState::Prepared { pinned: true });
        assert_eq!(state.queue[1].state, SlotState::Prepared { pinned: true });
    }

    #[test]
    fn failed_ready_prefix_retires_none_and_retries_the_same_operations() {
        let backend = Arc::new(RecordingBlobs::default());
        backend.fail_next_writes(1);
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(4, 3, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 161);
        publish(&writeback, vec![put(b"b", b"two")], 162);
        publish(&writeback, vec![put(b"c", b"three")], 163);
        publish(&writeback, vec![put(b"d", b"four")], 164);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence.get() == 1
        ));
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.queue_len(), 4);
        assert!(backend.inner.snapshot().is_empty());
        assert_eq!(backend.attempts().len(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        let attempts = backend.attempts();
        assert_eq!(attempts.len(), 2);
        assert_eq!(
            attempts[0], attempts[1],
            "retry changed the captured prefix"
        );
        assert_eq!(attempts[0].len(), 6, "three log + data record pairs");
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.inner.batch_count(), 1);
    }

    #[test]
    fn publication_during_backend_io_is_not_retired_with_captured_prefix() {
        let backend = Arc::new(BlockingBlobs::default());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 171);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 172);
        first.publish().unwrap();

        std::thread::scope(|scope| {
            let consumer = scope.spawn(|| writeback.process_front());
            backend.wait_until_entered();
            second.publish().unwrap();
            backend.release();
            assert!(matches!(consumer.join().unwrap(), ProcessOutcome::Advanced));
        });

        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.inner.batch_count(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.inner.batch_count(), 2);
    }

    #[test]
    fn same_key_updates_preserve_transaction_order_inside_one_batch() {
        let backend = Arc::new(RecordingBlobs::default());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"same", b"first")], 181);
        publish(&writeback, vec![put(b"same", b"second")], 182);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.inner.batch_count(), 1);

        let attempts = backend.attempts();
        assert_eq!(attempts.len(), 1);
        assert_eq!(attempts[0].len(), 4);
        match (&attempts[0][1], &attempts[0][3]) {
            (
                OwnedBlobOp::Put {
                    key: first_key,
                    value: first_value,
                },
                OwnedBlobOp::Put {
                    key: second_key,
                    value: second_value,
                },
            ) => {
                assert_eq!(first_key, second_key);
                assert_eq!(first_value, b"first");
                assert_eq!(second_value, b"second");
            }
            operations => panic!("unexpected data operation order: {operations:?}"),
        }

        let snapshot = backend.inner.snapshot();
        assert_eq!(snapshot.len(), 3, "two logs and one materialized data key");
        assert!(!snapshot.values().any(|value| value.as_slice() == b"first"));
        assert_eq!(
            snapshot
                .values()
                .filter(|value| value.as_slice() == b"second")
                .count(),
            1
        );
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
        writeback
            .reserve(vec![put(b"c", b"three")])
            .unwrap()
            .bind(mako_timestamp_of(13))
            .unwrap()
            .publish()
            .unwrap();

        assert!(matches!(
            writeback.wait_applied(),
            Err(ApplyError::Backend {
                sequence,
                attempts: 1,
                ..
            }) if sequence.get() == 1
        ));
        assert!(
            backend.snapshot().is_empty(),
            "failed batch was all-or-none"
        );
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::default(),
            "a rejected backend call cannot advance progress"
        );
        assert_eq!(
            writeback.queue_len(),
            2,
            "failed front and its ready suffix remain queued"
        );

        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
        assert_eq!(backend.op_count(), 5);
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(2, Some(mako_timestamp_of(13)))
        );
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn exhausted_retry_continues_when_concurrent_consumer_completed_failed_sequence() {
        let failed = CommitSeq::new(7).unwrap();

        assert_eq!(
            retry_progress(7, 8, failed),
            RetryProgress::FailedSequenceApplied,
            "the exact failed sequence is no longer a live backend failure"
        );
        assert_eq!(retry_progress(8, 8, failed), RetryProgress::TargetApplied);
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
        assert_eq!(writeback.wait_applied().unwrap(), 10);
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
    fn permanent_native_record_failure_latches_exact_health_error() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(3, 0)).unwrap();
        let mut detached_before_failure = writeback
            .reserve(vec![put(b"already-detached", b"value")])
            .unwrap();

        let mut malformed = encoded_native_record(1, 191, vec![put(b"bad", b"crc")]);
        *malformed.last_mut().unwrap() ^= 1;
        assert_eq!(publish_native_bytes(&writeback, 191, malformed).get(), 1);

        let latched_source = match writeback.process_front() {
            ProcessOutcome::RecordFailed { sequence, error } => {
                assert_eq!(sequence.get(), 1);
                assert!(matches!(error, RecordError::BadChecksum { .. }));
                error
            }
            outcome => panic!("malformed native record was not rejected: {outcome:?}"),
        };
        assert_eq!(writeback.queue_len(), 1, "malformed slot stays queued");
        assert_eq!(
            backend.batch_count(),
            0,
            "malformed bytes never reach Rocks"
        );

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed { sequence, error }
                if sequence.get() == 1 && error == latched_source
        ));
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::Record { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(matches!(
            writeback.wait_applied(),
            Err(ApplyError::Record { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(matches!(
            writeback.reserve(vec![put(b"new", b"work")]),
            Err(ReserveError::PermanentRecordFailure { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(matches!(
            detached_before_failure.bind(mako_timestamp_of(192)),
            Err(ReserveError::PermanentRecordFailure { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(detached_before_failure.owns_capacity);
    }

    #[test]
    fn barrier_snapshot_ignores_a_later_permanent_record_failure() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"safe", b"prefix")], 197);
        let mut malformed = encoded_native_record(2, 198, vec![put(b"bad", b"suffix")]);
        *malformed.last_mut().unwrap() ^= 1;
        publish_native_bytes(&writeback, 198, malformed);
        assert_eq!(writeback.highest_acknowledged(), 2);

        // Model a barrier that captured sequence 1 immediately before the
        // second publication. It must neither decode nor apply sequence 2.
        assert_eq!(writeback.wait_applied_through(1).unwrap(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert!(writeback.ensure_no_unknown().is_ok());

        // The ordinary background mode remains unbounded and discovers the
        // malformed suffix on its next attempt.
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::BadChecksum { .. },
            } if sequence.get() == 2
        ));
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::Record {
                sequence,
                source: RecordError::BadChecksum { .. },
            }) if sequence.get() == 2
        ));
    }

    #[test]
    fn barrier_snapshot_ignores_a_later_retryable_allocation_failure() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"safe", b"prefix")], 199);
        publish(&writeback, vec![put(b"retry", b"suffix")], 200);
        let later = CommitSeq::new(2).unwrap();
        let failure = force_materialization_failure(later, RecordError::AllocationFailed);

        assert_eq!(writeback.wait_applied_through(1).unwrap(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);

        // Background processing is still unbounded, observes the transient
        // failure, and leaves queue health retryable.
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::AllocationFailed,
            } if sequence == later
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert!(writeback.ensure_no_unknown().is_ok());

        drop(failure);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn permanent_record_failure_wakes_capacity_and_acknowledgement_waiters() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());
        let first = bind(writeback.reserve(vec![put(b"front", b"one")]).unwrap(), 193);
        let second = bind(writeback.reserve(vec![put(b"later", b"two")]).unwrap(), 194);

        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(second.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            assert!(published_rx
                .recv_timeout(Duration::from_millis(30))
                .is_err());

            assert!(matches!(
                writeback.record_failure_outcome(
                    CommitSeq::new(1).unwrap(),
                    RecordError::BadMagic,
                ),
                ProcessOutcome::RecordFailed { sequence, error: RecordError::BadMagic }
                    if sequence.get() == 1
            ));
            assert!(matches!(
                published_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
                Err(ResolveError::BlockedByPriorRecordFailure {
                    sequence,
                    prior_failure,
                    source: RecordError::BadMagic,
                }) if sequence.get() == 2 && prior_failure.get() == 1
            ));
            publisher.join().unwrap();
        });
        drop(first);

        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let mut malformed = encoded_native_record(1, 195, vec![put(b"bad", b"crc")]);
        *malformed.last_mut().unwrap() ^= 1;
        publish_native_bytes(&writeback, 195, malformed);

        let (result_tx, result_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let result = match producer_writeback.reserve(vec![put(b"blocked", b"work")]) {
                Err(ReserveError::PermanentRecordFailure { sequence, source }) => {
                    Ok((sequence, source))
                }
                Err(error) => Err(format!("unexpected reservation error: {error}")),
                Ok(permit) => {
                    drop(permit);
                    Err("reservation unexpectedly succeeded".to_owned())
                }
            };
            result_tx.send(result).unwrap();
        });
        assert!(result_rx.recv_timeout(Duration::from_millis(30)).is_err());
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed { sequence, error: RecordError::BadChecksum { .. } }
                if sequence.get() == 1
        ));
        let (sequence, source) = result_rx
            .recv_timeout(Duration::from_secs(1))
            .unwrap()
            .expect("backpressured reservation observed the permanent failure");
        assert_eq!(sequence.get(), 1);
        assert!(matches!(source, RecordError::BadChecksum { .. }));
        producer.join().unwrap();
    }

    #[test]
    fn allocation_failure_remains_retryable_and_does_not_poison_health() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        publish(&writeback, vec![put(b"retry", b"value")], 196);
        let sequence = CommitSeq::new(1).unwrap();

        assert!(matches!(
            writeback.record_failure_outcome(sequence, RecordError::AllocationFailed),
            ProcessOutcome::RecordFailed {
                sequence: failed,
                error: RecordError::AllocationFailed,
            } if failed == sequence
        ));
        assert!(writeback.ensure_no_unknown().is_ok());
        let detached = writeback.reserve(vec![put(b"still", b"healthy")]).unwrap();
        drop(detached);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn permanent_record_latch_retains_the_earliest_sequence() {
        let writeback = Writeback::new(MemBlobs::new(), 0, config(1, 0)).unwrap();
        let seventh = CommitSeq::new(7).unwrap();
        let third = CommitSeq::new(3).unwrap();

        let _ = writeback.record_failure_outcome(seventh, RecordError::BadMagic);
        let _ = writeback.record_failure_outcome(third, RecordError::Truncated);
        let _ = writeback.record_failure_outcome(seventh, RecordError::InvalidSequence);

        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::Record {
                sequence,
                source: RecordError::Truncated,
            }) if sequence == third
        ));
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

        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(bound_second.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));
            assert_eq!(writeback.applied_watermark(), AppliedWatermark::default());
            assert_eq!(writeback.highest_acknowledged(), 0);

            bound_first.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            publisher.join().unwrap();
        });
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(1, Some(mako_timestamp_of(202)))
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(2, Some(mako_timestamp_of(101))),
            "the timestamp names the frontier record; it is not a numeric maximum"
        );
    }

    #[test]
    fn queue_tokens_track_current_vecdeque_offsets_after_front_retirement() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(4, 0)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 203);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 204);
        let third = bind(writeback.reserve(vec![put(b"c", b"three")]).unwrap(), 205);
        let first_token = first.token;
        let second_token = second.token;
        let third_token = third.token;

        {
            let state = lock_recover(&writeback.state);
            assert_eq!(state.queue_offset(first_token), Some(0));
            assert_eq!(state.queue_offset(second_token), Some(1));
            assert_eq!(state.queue_offset(third_token), Some(2));
        }

        first.publish().unwrap();
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        {
            let state = lock_recover(&writeback.state);
            assert_eq!(state.queue_offset(first_token), None);
            assert_eq!(state.queue_offset(second_token), Some(0));
            assert_eq!(state.queue_offset(third_token), Some(1));
        }
        assert!(matches!(
            writeback.resolve(first_token, Resolution::PinUnknown),
            Err(ResolveError::Missing { sequence }) if sequence.get() == 1
        ));

        // Both handles were minted before the front moved. Resolving them
        // afterward must use their new offsets rather than a stale bind-time
        // index or a scan from the current front.
        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(third.publish()).unwrap());
            wait_until_ready(&writeback, 3);
            assert_eq!(writeback.highest_acknowledged(), 1);

            second.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                3
            );
            publisher.join().unwrap();
        });
        assert_eq!(writeback.wait_applied().unwrap(), 3);
    }

    #[test]
    fn prepared_hole_blocks_later_ready_transaction_and_acknowledgement() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 21);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 22);
        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(second.publish()).unwrap());
            wait_until_ready(&writeback, 2);

            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));
            assert_eq!(backend.batch_count(), 0);
            assert_eq!(writeback.highest_acknowledged(), 0);
            assert!(matches!(
                published_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));

            first.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            publisher.join().unwrap();
        });
        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
        assert_eq!(writeback.applied_sequence(), 2);
    }

    #[test]
    fn bind_and_publish_make_the_prepared_to_ready_transition_explicit() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(1, 0)).unwrap();
        let bound = bind(
            writeback
                .reserve(vec![put(b"ready-transition", b"value")])
                .unwrap(),
            23,
        );

        {
            let state = lock_recover(&writeback.state);
            let slot = state.queue.front().expect("bind creates one slot");
            assert_eq!(slot.sequence.get(), 1);
            assert_eq!(slot.state, SlotState::Prepared { pinned: false });
            assert!(
                slot.record.get().is_none(),
                "bind must not expose an unfinalized record as Ready"
            );
            assert_eq!(state.highest_acknowledged, 0);
        }

        assert_eq!(bound.publish().unwrap().get(), 1);
        {
            let state = lock_recover(&writeback.state);
            let slot = state.queue.front().expect("published slot remains queued");
            assert_eq!(slot.state, SlotState::Ready);
            assert!(
                slot.record.get().is_some(),
                "publication must finalize the record before Ready"
            );
            assert_eq!(state.highest_acknowledged, 1);
        }

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn out_of_order_publications_wait_for_one_dense_acknowledgement_prefix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 31);
        let second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 32);
        let third = bind(writeback.reserve(vec![put(b"c", b"three")]).unwrap(), 33);

        std::thread::scope(|scope| {
            let (second_tx, second_rx) = mpsc::channel();
            let (third_tx, third_rx) = mpsc::channel();
            let second_publisher = scope.spawn(move || second_tx.send(second.publish()).unwrap());
            let third_publisher = scope.spawn(move || third_tx.send(third.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            wait_until_ready(&writeback, 3);

            assert_eq!(writeback.highest_acknowledged(), 0);
            assert!(matches!(
                second_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));
            assert!(matches!(
                third_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));
            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));

            first.publish().unwrap();
            assert_eq!(writeback.highest_acknowledged(), 3);
            assert_eq!(
                second_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            assert_eq!(
                third_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                3
            );
            second_publisher.join().unwrap();
            third_publisher.join().unwrap();
        });

        assert_eq!(writeback.wait_applied().unwrap(), 3);
        assert_eq!(backend.batch_count(), 3);
        assert_eq!(writeback.applied_sequence(), 3);
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
        // acknowledged prefix is a valid application target. Clean close still
        // calls ensure_no_unknown and rejects the pinned queue.
        assert_eq!(writeback.wait_applied().unwrap(), 0);
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert!(matches!(
            writeback.reserve(vec![put(b"b", b"two")]),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert_eq!(writeback.applied_sequence(), 0);
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
    fn wait_applied_covers_safe_acknowledged_prefix_before_later_unknown() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let acknowledged = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 51);
        let later_unknown = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 52);
        later_unknown.pin_unknown().unwrap();
        // A later ambiguity must not prevent acknowledgement and application of
        // an earlier safe prefix whose native outcome is known.
        acknowledged.publish().unwrap();

        assert_eq!(writeback.highest_acknowledged(), 1);
        assert_eq!(writeback.wait_applied().unwrap(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
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
    fn wait_applied_targets_acknowledged_not_reserved_tail() {
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
        assert_eq!(writeback.wait_applied().unwrap(), 41);
        assert_eq!(writeback.applied_sequence(), 41);
        assert_eq!(writeback.queue_len(), 1);

        still_prepared.publish().unwrap();
        assert_eq!(writeback.wait_applied().unwrap(), 42);
        assert_eq!(writeback.applied_sequence(), 42);
    }

    #[test]
    fn apply_progress_wait_does_not_sleep_past_ready_publication() {
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
            waiter.wait_for_apply_progress(1);
            returned_tx.send(()).unwrap();
        });

        returned_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("a Ready front must not wait for a second notification");
        thread.join().unwrap();
    }

    #[test]
    fn publishing_a_later_slot_does_not_wake_a_waiter_blocked_on_the_front() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());
        let first = bind(writeback.reserve(vec![put(b"front", b"one")]).unwrap(), 74);
        let second = bind(writeback.reserve(vec![put(b"later", b"two")]).unwrap(), 75);

        std::thread::scope(|scope| {
            let (started_tx, started_rx) = mpsc::channel();
            let (returned_tx, returned_rx) = mpsc::channel();
            let waiter_writeback = Arc::clone(&writeback);
            let waiter = scope.spawn(move || {
                started_tx.send(()).unwrap();
                waiter_writeback.wait_for_apply_progress(2);
                returned_tx.send(()).unwrap();
            });
            started_rx.recv_timeout(Duration::from_secs(1)).unwrap();

            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(second.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            assert!(matches!(
                published_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));
            assert!(
                matches!(returned_rx.try_recv(), Err(mpsc::TryRecvError::Empty)),
                "a Ready suffix cannot unblock a Prepared front"
            );

            first.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            returned_rx.recv_timeout(Duration::from_secs(1)).unwrap();
            publisher.join().unwrap();
            waiter.join().unwrap();
        });
        assert_eq!(writeback.wait_applied().unwrap(), 2);
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

    #[test]
    fn binding_the_final_sequence_wakes_every_sequence_space_waiter() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(
            Writeback::new(backend, u64::MAX - 1, config(3, 0))
                .expect("the final sequence remains available"),
        );
        let final_permit = writeback.reserve(vec![put(b"final", b"value")]).unwrap();
        let start = Arc::new(Barrier::new(3));
        let (result_tx, result_rx) = mpsc::channel();
        let mut waiters = Vec::new();

        for key in [b"blocked-a".as_slice(), b"blocked-b".as_slice()] {
            let waiter_writeback = Arc::clone(&writeback);
            let waiter_start = Arc::clone(&start);
            let waiter_result = result_tx.clone();
            let key = key.to_vec();
            waiters.push(std::thread::spawn(move || {
                waiter_start.wait();
                let exhausted = matches!(
                    waiter_writeback.reserve(vec![put(&key, b"never-admitted")]),
                    Err(ReserveError::SequenceExhausted)
                );
                waiter_result.send(exhausted).unwrap();
            }));
        }
        drop(result_tx);
        start.wait();
        assert!(result_rx.recv_timeout(Duration::from_millis(30)).is_err());

        let final_reservation = bind(final_permit, 82);
        assert_eq!(final_reservation.sequence().get(), u64::MAX);
        for _ in 0..2 {
            assert!(
                result_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
                "a sequence-space waiter did not observe terminal exhaustion"
            );
        }
        for waiter in waiters {
            waiter.join().unwrap();
        }

        final_reservation.publish().unwrap();
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), u64::MAX);
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

    #[derive(Debug, Default)]
    struct ApplyThenBlockBlobs {
        inner: MemBlobs,
        gate: Mutex<(bool, bool)>,
        changed: Condvar,
    }

    impl ApplyThenBlockBlobs {
        fn wait_until_applied(&self) -> bool {
            let mut gate = self.gate.lock().unwrap();
            let deadline = std::time::Instant::now() + Duration::from_secs(1);
            while !gate.0 {
                let now = std::time::Instant::now();
                if now >= deadline {
                    return false;
                }
                let (next, timeout) = self.changed.wait_timeout(gate, deadline - now).unwrap();
                gate = next;
                if timeout.timed_out() && !gate.0 {
                    return false;
                }
            }
            true
        }

        fn release(&self) {
            let mut gate = self.gate.lock().unwrap();
            gate.1 = true;
            self.changed.notify_all();
        }
    }

    impl Blobs for ApplyThenBlockBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            self.inner.write_batch(operations)?;
            let mut gate = self.gate.lock().unwrap();
            gate.0 = true;
            self.changed.notify_all();
            while !gate.1 {
                gate = self.changed.wait(gate).unwrap();
            }
            Ok(())
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn watermark_advances_only_after_a_successful_backend_call_returns() {
        let backend = Arc::new(ApplyThenBlockBlobs::default());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        bind(
            writeback.reserve(vec![put(b"applied", b"value")]).unwrap(),
            90,
        )
        .publish()
        .unwrap();

        std::thread::scope(|scope| {
            let consumer_writeback = &writeback;
            let consumer = scope.spawn(move || consumer_writeback.process_front());
            let reached_backend = backend.wait_until_applied();
            let batch_count = reached_backend.then(|| backend.inner.batch_count());
            let (snapshot_tx, snapshot_rx) = mpsc::channel();
            let observer = if reached_backend {
                let observer_writeback = &writeback;
                Some(scope.spawn(move || {
                    snapshot_tx
                        .send((
                            observer_writeback.applied_watermark(),
                            observer_writeback.queue_len(),
                        ))
                        .unwrap();
                }))
            } else {
                None
            };
            let snapshot_while_blocked =
                reached_backend.then(|| snapshot_rx.recv_timeout(Duration::from_secs(1)));

            // Release before asserting or joining. If a regression held the
            // queue-state mutex across backend IO, the observer above times
            // out but can finish after this release instead of hanging.
            backend.release();
            let outcome = consumer.join().unwrap();
            if let Some(observer) = observer {
                observer.join().unwrap();
            }

            assert!(reached_backend, "backend did not reach its return gate");
            assert_eq!(batch_count, Some(1));
            let (watermark_while_blocked, queue_len_while_blocked) = snapshot_while_blocked
                .expect("backend was reached")
                .expect("watermark read blocked behind backend IO");
            assert_eq!(
                watermark_while_blocked,
                AppliedWatermark::default(),
                "backend side effects alone do not advance the watermark"
            );
            assert_eq!(queue_len_while_blocked, 1);
            assert!(matches!(outcome, ProcessOutcome::Advanced));
        });

        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(1, Some(mako_timestamp_of(90)))
        );
        assert_eq!(writeback.queue_len(), 0);
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
        assert_eq!(writeback.wait_applied().unwrap(), 2);
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
        assert_eq!(writeback.applied_watermark(), AppliedWatermark::default());
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.backend().inner.batch_count(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
    }
}
