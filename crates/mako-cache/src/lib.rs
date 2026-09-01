//! Transactional Silo/MassTrans write-back cache over RocksDB.
//!
//! Native C++ Silo is the authoritative live state while this process runs.
//! Before commit, STO sizes and seals its canonical final write set while Rust
//! claims bounded queue capacity and checks out one output buffer from a
//! queue-sized small-record arena (with a recycled oversized fallback). After
//! Silo locks the complete write set, then native orders Mako timestamp
//! assignment, final validation, and dense record binding. Concurrent caches
//! use one packed process word; single-producer caches retain their exclusive
//! Rust sequence allocator. A failed validation may leave a timestamp gap but
//! never a cache-log slot. Native writes the complete record directly into the
//! claimed buffer while retaining all write locks, and only
//! then installs the writes. Native success publishes the already-built record
//! as Ready in the volatile queue. The trusted concurrent one-Put terminal can
//! return once its own record is Ready; explicit barriers and the background
//! writer still wait for a dense prefix, then replay a bounded contiguous
//! prefix in one atomic RocksDB `WriteBatch`.
//!
//! [`Cache::wait_applied`] and [`Cache::close`] drain acknowledged work into
//! RocksDB, but neither operation adds a separate WAL flush or disk sync. The
//! default batch mode uses `sync=false`; the applied watermark is process-local
//! progress, not a durability promise.
//! This first slice exposes one logical application table. Queue occupancy and
//! individual record size are bounded; transactions may contain many keys.
//!
//! ```no_run
//! # fn main() -> Result<(), mako_cache::Error> {
//! let db = mako_cache::Db::open("/tmp/mako-cache", mako_cache::Options::default())?;
//! let mut tx = db.transaction()?;
//! tx.put(b"alice", b"10")?;
//! tx.put(b"bob", b"20")?;
//! tx.commit()?; // visible and queued, not necessarily in Rocks yet
//! db.wait_applied()?;
//! db.close()?;
//! # Ok(())
//! # }
//! ```

#![deny(unsafe_code)]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]

use std::cell::Cell;
use std::collections::BTreeMap;
use std::fmt;
use std::marker::PhantomData;
use std::num::{NonZeroU32, NonZeroU64};
use std::path::Path;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};

use mako_local::{CommitDisposition, LocalDb};
use mrx_core::{BlobError, Blobs};
use mrx_rocks::RocksBlobs;

#[cfg(test)]
mod failpoint;
// Reviewed native-record ownership seam. Keep unsafe code denied everywhere
// else in the crate so future fast-path work cannot silently broaden it.
#[allow(unsafe_code)]
mod record;
mod runtime;
/// Deterministic cache mutation controls used only by validation binaries.
#[cfg(feature = "test-support")]
#[doc(hidden)]
pub mod test_support;
// Reviewed fixed-address queue/arena implementation.
#[allow(unsafe_code)]
mod writeback;

#[cfg(all(test, have_mako, have_rocksdb, target_family = "unix"))]
mod crash_tests;

#[cfg(all(test, have_mako))]
mod application_history_tests;

#[cfg(all(test, have_mako))]
mod milestone1_acceptance_tests;

pub use mako_local::{CommitRecordChecksum as RecordChecksum, Error as LocalError, MakoTimestamp};
pub use mrx_rocks::Durability;
pub use record::{CommitSeq, RecordError};
pub use writeback::{
    AppliedWatermark, ApplyError, ConfigError as WritebackConfigError, ReserveError, ResolveError,
    WritebackConfig,
};

use record::{BackendKey, CommitRecord, DEFAULT_TABLE_ID, Mutation, classify_backend_key};
use runtime::{Runtime, RuntimeError};
use writeback::Writeback;

const DEFAULT_TABLE_NAME: &[u8] = b"mako-cache/default";

const COMMIT_FENCE_SPINS: usize = 64;
static NEXT_COMMIT_FENCE_THREAD_SLOT: AtomicUsize = AtomicUsize::new(0);

thread_local! {
    /// Process-lifetime slot paired with native's process-lifetime worker.
    ///
    /// A slot number need not equal STO's worker ID. Every thread which gets
    /// here already attached to STO, so no more than `MAX_WORKERS` distinct
    /// threads can allocate one. Neither allocator recycles a departed OS
    /// thread's slot.
    static COMMIT_FENCE_THREAD_SLOT: Cell<usize> = const { Cell::new(usize::MAX) };
}

#[repr(align(64))]
struct CommitWriterSlot {
    /// Owner-written generations: even is idle and the following odd value is
    /// one active outcome. A reader waits for one exact observed odd value, so
    /// a later writer generation cannot extend that wait indefinitely.
    generation: AtomicU64,
}

impl CommitWriterSlot {
    const fn new() -> Self {
        Self {
            generation: AtomicU64::new(0),
        }
    }

    #[inline(always)]
    fn begin(&self) -> u64 {
        let idle = self.generation.load(Ordering::Relaxed);
        assert_eq!(
            idle & 1,
            0,
            "one native worker cannot overlap two cache commit outcomes"
        );
        let active = idle.wrapping_add(1);
        self.generation.store(active, Ordering::Release);
        active
    }

    #[inline(always)]
    fn drain_observed(&self, observed: u64, spins: &mut usize) {
        if observed & 1 == 0 {
            return;
        }
        // Equality can alias a later active generation only after 2^64 owner
        // transitions. No finite read-only scan can overlap that many native
        // outcomes from one process-lifetime worker.
        while self.generation.load(Ordering::Acquire) == observed {
            commit_fence_backoff(spins);
        }
    }
}

const _: () = assert!(std::mem::size_of::<CommitWriterSlot>() == 64);

/// An asymmetric outcome fence for common writers and rare read-only commits.
///
/// Each attached writer modifies only its own cache line before entering the
/// native packed cache-order protocol. A read-only commit marks a cut in that
/// atomic modification order, then guarantees that every writer before it has
/// drained. A scan may also conservatively wait for one observed post-cut
/// generation. Later writers may proceed because STO orders them after the
/// read-only transaction or rejects read-only validation on a conflicting
/// lock, version, or predicate.
struct CommitFence {
    read_only_serial: Mutex<()>,
    writers: Box<[CommitWriterSlot]>,
}

impl CommitFence {
    fn new() -> Self {
        let writers = (0..mako_local::MAX_WORKERS)
            .map(|_| CommitWriterSlot::new())
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Self {
            read_only_serial: Mutex::new(()),
            writers,
        }
    }

    #[inline(always)]
    fn current_thread_slot() -> usize {
        COMMIT_FENCE_THREAD_SLOT.with(|thread_slot| {
            let current = thread_slot.get();
            if current != usize::MAX {
                return current;
            }
            let allocated = NEXT_COMMIT_FENCE_THREAD_SLOT.fetch_add(1, Ordering::Relaxed);
            assert!(
                allocated < mako_local::MAX_WORKERS,
                "a cache writer reached the outcome fence without a native worker slot"
            );
            thread_slot.set(allocated);
            allocated
        })
    }

    #[inline(always)]
    fn enter_writer(&self) -> CommitWriterGuard<'_> {
        self.enter_writer_slot(Self::current_thread_slot())
    }

    #[inline(always)]
    fn enter_writer_slot(&self, slot: usize) -> CommitWriterGuard<'_> {
        let writer = self
            .writers
            .get(slot)
            .expect("commit-fence writer slot is in range");
        let active_generation = writer.begin();
        CommitWriterGuard {
            writer,
            active_generation,
            slot,
        }
    }

    fn close_for_read_only(
        &self,
        order_validation_prefix: impl FnOnce(),
    ) -> CommitReadOnlyGuard<'_> {
        let serial = self
            .read_only_serial
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        // A writer's odd-generation Release publication precedes its packed
        // assignment or general-lock Release RMW. The callback's Acquire RMW
        // reads that modification or a later member of its release sequence.
        // Each following Acquire load must see a prefix writer's active
        // generation, or its later Release clear after the outcome is
        // represented in write-back.
        //
        // A writer can publish its generation before this cut but enter packed
        // order after it. Native acquires that writer's complete write set
        // first, but cannot install until after ordering and validation.
        // STO validation therefore either rejects this read-only transaction
        // on a conflicting lock/predicate, or admits the reader-before-writer
        // serialization. Missing that post-cut generation is safe.
        order_validation_prefix();

        let mut spins = 0;
        for writer in &self.writers {
            let observed = writer.generation.load(Ordering::Acquire);
            writer.drain_observed(observed, &mut spins);
        }
        CommitReadOnlyGuard { _serial: serial }
    }
}

#[inline(always)]
fn commit_fence_backoff(spins: &mut usize) {
    if *spins < COMMIT_FENCE_SPINS {
        *spins += 1;
        std::hint::spin_loop();
    } else {
        std::thread::yield_now();
    }
}

struct CommitWriterGuard<'a> {
    writer: &'a CommitWriterSlot,
    active_generation: u64,
    slot: usize,
}

impl CommitWriterGuard<'_> {
    #[inline(always)]
    fn slot(&self) -> usize {
        self.slot
    }
}

impl Drop for CommitWriterGuard<'_> {
    #[inline(always)]
    fn drop(&mut self) {
        // The closer's Acquire scan carries all native outcome and
        // queue-resolution effects which precede this Release clear.
        debug_assert_eq!(
            self.writer.generation.load(Ordering::Relaxed),
            self.active_generation
        );
        self.writer
            .generation
            .store(self.active_generation.wrapping_add(1), Ordering::Release);
    }
}

struct CommitReadOnlyGuard<'a> {
    _serial: MutexGuard<'a, ()>,
}

/// Foreground transaction concurrency accepted by one cache instance.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ForegroundMode {
    /// Transactions may begin and commit concurrently on many worker threads.
    #[default]
    Concurrent,
    /// Transactions use one explicit, thread-affine foreground lease.
    ///
    /// The lease makes foreground calls sequential without a per-transaction
    /// ownership check. This lets the queue use its single-producer protocol;
    /// [`Cache::transaction`] is deliberately disabled and callers must use
    /// [`Cache::single_producer`] instead.
    SingleProducer,
}

/// Cache-specific behavior independent of the concrete backend.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CacheOptions {
    /// Bounded transaction-log and retry settings.
    pub writeback: WritebackConfig,
    /// Foreground transaction concurrency profile.
    pub foreground_mode: ForegroundMode,
    /// Integrity mode for newly produced cache-log records.
    ///
    /// [`RecordChecksum::Crc32c`] is the corruption-detecting default. The
    /// explicitly selected [`RecordChecksum::None`] format is self-describing
    /// and remains replay-compatible, but deliberately trades payload
    /// corruption detection for foreground commit latency.
    pub record_checksum: RecordChecksum,
    /// Logical CPU on which to run the cache's write-back consumer.
    ///
    /// `None` inherits the process affinity. A selected CPU pins only the
    /// `mako-writeback` thread; RocksDB's own internal threads are outside this
    /// setting. Explicit affinity is currently supported on Linux, where an
    /// invalid or disallowed CPU makes cache startup fail.
    pub writeback_cpu: Option<usize>,
    /// Reject startup unless the native engine advertises conventional
    /// read-your-writes behavior.
    ///
    /// Point and transactional-scan read-your-writes are part of the default
    /// cache profile. Set this to `false` only when deliberately opening
    /// against a legacy native build.
    pub require_read_my_writes: bool,
    /// Isolation profile required from the native transaction engine.
    ///
    /// The production default is committed-transaction strict
    /// serializability. Select [`Isolation::Opaque`] only when applications
    /// also require aborted and in-flight transactions to observe a
    /// consistent snapshot; startup then rejects a non-opaque native build.
    pub isolation: Isolation,
}

/// Native transaction isolation required by the cache deployment.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Isolation {
    /// Committed transactions are strictly serializable.
    #[default]
    StrictSerializable,
    /// Every transaction, including aborted and in-flight work, observes a
    /// consistent snapshot.
    Opaque,
}

impl Default for CacheOptions {
    fn default() -> Self {
        Self {
            writeback: WritebackConfig::default(),
            foreground_mode: ForegroundMode::Concurrent,
            record_checksum: RecordChecksum::Crc32c,
            writeback_cpu: None,
            require_read_my_writes: true,
            isolation: Isolation::StrictSerializable,
        }
    }
}

/// Options for the concrete RocksDB-backed [`Db`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Options {
    /// RocksDB write mode used for each atomic batch.
    ///
    /// The default is [`Durability::Wal`], which keeps the WAL enabled with
    /// per-write synchronization disabled. The cache never separately flushes
    /// or syncs RocksDB.
    pub durability: Durability,
    /// Cache and transaction-log settings.
    pub cache: CacheOptions,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            durability: Durability::Wal,
            cache: CacheOptions::default(),
        }
    }
}

/// Anything that can go wrong opening or using the transaction cache.
#[derive(Debug)]
pub enum Error {
    /// The native Silo/MassTrans boundary failed.
    Native(LocalError),
    /// The backend failed outside the asynchronous application path.
    Backend(BlobError),
    /// A backend transaction record was malformed or too large.
    Record(RecordError),
    /// Write-back construction options were invalid.
    WritebackConfig(WritebackConfigError),
    /// A complete pre-commit reservation could not be made.
    Reserve(ReserveError),
    /// A bound write-back reservation could not be resolved safely.
    ///
    /// In particular, a transaction that is known committed is retained and
    /// left unacknowledged when an earlier queue slot has an unknown outcome.
    Resolve(ResolveError),
    /// An application barrier could not cover its acknowledged snapshot.
    Apply(ApplyError),
    /// Starting the background writer failed.
    RuntimeStart(std::io::Error),
    /// Stopping or draining the background writer failed.
    Runtime(RuntimeError),
    /// The runtime-handle mutex was poisoned.
    RuntimeLockPoisoned,
    /// This cache requires transactions to use its explicit single-producer
    /// lease instead of the concurrent foreground entry point.
    SingleProducerHandleRequired,
    /// A single-producer lease was requested for a concurrent cache.
    SingleProducerModeDisabled,
    /// Another live single-producer lease already owns the foreground path.
    SingleProducerAlreadyClaimed,
    /// Rust could not reserve owned transaction or recovery storage.
    AllocationFailed,
    /// The selected native feature profile is unavailable.
    MissingReadMyWrites,
    /// The deployment requires opacity but the native engine was built
    /// without it.
    MissingOpacity,
    /// The RocksDB contains a key outside this cache's tagged format.
    ForeignBackendKey,
    /// A backend record or data key names a table unsupported by this slice.
    UnsupportedTable(u64),
    /// A backend log record and its materialized RocksDB data disagree.
    BackendStateMismatch,
    /// Recovery replay produced a result impossible for the retained history.
    RecoveryDiverged,
    /// Native installation succeeded, but terminal handle cleanup failed.
    ///
    /// The transaction is visible and its record has already been published.
    CommittedButCleanupFailed {
        /// Cache commit sequence that remains covered by write-back.
        sequence: CommitSeq,
        /// Native cleanup failure.
        source: LocalError,
    },
    /// Native definitely aborted after consuming a dense cache sequence, but
    /// rejected before it could produce a replayable record.
    ///
    /// Visibility is not ambiguous, so callers may retry the logical
    /// transaction. The consumed ordering slot is nevertheless permanently
    /// pinned and this cache instance cannot admit later work.
    BoundRecordUnwritten {
        /// Pinned cache commit sequence.
        sequence: CommitSeq,
        /// Definite pre-install native abort reason.
        abort: LocalError,
        /// A separate terminal-handle cleanup failure, if one occurred.
        cleanup: Option<LocalError>,
    },
    /// Native commit returned an outcome that cannot safely be called abort.
    ///
    /// The corresponding queue slot is permanently pinned; later commits and
    /// application barriers fail rather than risk skipping a possibly visible
    /// transaction.
    UnknownCommitOutcome {
        /// Pinned cache commit sequence.
        sequence: CommitSeq,
        /// Native commit failure whose visibility is ambiguous.
        source: LocalError,
        /// A separate terminal-handle cleanup failure, if one occurred.
        cleanup: Option<LocalError>,
    },
    /// A definitely aborted transaction also failed terminal handle cleanup.
    AbortCleanupFailed {
        /// Definite native abort reason, normally an OCC conflict.
        abort: LocalError,
        /// Cleanup failure.
        cleanup: LocalError,
    },
}

impl Error {
    /// Whether this error is a normal optimistic-concurrency conflict.
    pub fn is_conflict(&self) -> bool {
        matches!(self, Self::Native(LocalError::Conflict))
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Native(error) => write!(f, "{error}"),
            Self::Backend(error) => write!(f, "{error}"),
            Self::Record(error) => write!(f, "{error}"),
            Self::WritebackConfig(error) => write!(f, "{error}"),
            Self::Reserve(error) => write!(f, "{error}"),
            Self::Resolve(error) => write!(f, "{error}"),
            Self::Apply(error) => write!(f, "{error}"),
            Self::RuntimeStart(error) => write!(f, "cannot start write-back worker: {error}"),
            Self::Runtime(error) => write!(f, "{error}"),
            Self::RuntimeLockPoisoned => write!(f, "the cache runtime lock is poisoned"),
            Self::SingleProducerHandleRequired => write!(
                f,
                "this cache requires transactions through its single-producer lease"
            ),
            Self::SingleProducerModeDisabled => {
                write!(
                    f,
                    "this cache was opened for concurrent foreground transactions"
                )
            }
            Self::SingleProducerAlreadyClaimed => {
                write!(f, "this cache already has a live single-producer lease")
            }
            Self::AllocationFailed => write!(f, "could not allocate owned cache state"),
            Self::MissingReadMyWrites => {
                write!(
                    f,
                    "the native engine does not provide required read-your-writes"
                )
            }
            Self::MissingOpacity => write!(
                f,
                "the cache requires opacity, but the native engine provides only strict serializability"
            ),
            Self::ForeignBackendKey => write!(
                f,
                "RocksDB contains a key outside the mako-cache tagged format"
            ),
            Self::UnsupportedTable(table) => {
                write!(f, "backend state names unsupported table {table}")
            }
            Self::BackendStateMismatch => {
                write!(f, "backend transaction log and materialized data disagree")
            }
            Self::RecoveryDiverged => write!(f, "native replay diverged from backend history"),
            Self::CommittedButCleanupFailed { sequence, source } => write!(
                f,
                "transaction {} committed and was queued, but native cleanup failed: {source}",
                sequence.get()
            ),
            Self::BoundRecordUnwritten {
                sequence,
                abort,
                cleanup,
            } => {
                write!(
                    f,
                    "transaction {} definitely aborted after binding, but its unwritten ordering slot pins write-back: {abort}",
                    sequence.get()
                )?;
                if let Some(cleanup) = cleanup {
                    write!(f, "; cleanup also failed: {cleanup}")?;
                }
                Ok(())
            }
            Self::UnknownCommitOutcome {
                sequence,
                source,
                cleanup,
            } => {
                write!(
                    f,
                    "transaction {} has unknown visibility and pins write-back: {source}",
                    sequence.get()
                )?;
                if let Some(cleanup) = cleanup {
                    write!(f, "; cleanup also failed: {cleanup}")?;
                }
                Ok(())
            }
            Self::AbortCleanupFailed { abort, cleanup } => write!(
                f,
                "transaction definitely aborted ({abort}), but cleanup failed: {cleanup}"
            ),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Native(error) => Some(error),
            Self::Backend(error) => Some(error),
            Self::Record(error) => Some(error),
            Self::WritebackConfig(error) => Some(error),
            Self::Reserve(error) => Some(error),
            Self::Resolve(error) => Some(error),
            Self::Apply(error) => Some(error),
            Self::RuntimeStart(error) => Some(error),
            Self::Runtime(error) => Some(error),
            Self::CommittedButCleanupFailed { source, .. } => Some(source),
            Self::BoundRecordUnwritten { abort, .. } => Some(abort),
            Self::UnknownCommitOutcome { source, .. } => Some(source),
            Self::AbortCleanupFailed { cleanup, .. } => Some(cleanup),
            Self::RuntimeLockPoisoned
            | Self::SingleProducerHandleRequired
            | Self::SingleProducerModeDisabled
            | Self::SingleProducerAlreadyClaimed
            | Self::AllocationFailed
            | Self::MissingReadMyWrites
            | Self::MissingOpacity
            | Self::ForeignBackendKey
            | Self::UnsupportedTable(_)
            | Self::BackendStateMismatch
            | Self::RecoveryDiverged => None,
        }
    }
}

impl From<LocalError> for Error {
    fn from(value: LocalError) -> Self {
        Self::Native(value)
    }
}

impl From<BlobError> for Error {
    fn from(value: BlobError) -> Self {
        Self::Backend(value)
    }
}

impl From<RecordError> for Error {
    fn from(value: RecordError) -> Self {
        Self::Record(value)
    }
}

impl From<WritebackConfigError> for Error {
    fn from(value: WritebackConfigError) -> Self {
        Self::WritebackConfig(value)
    }
}

impl From<ReserveError> for Error {
    fn from(value: ReserveError) -> Self {
        Self::Reserve(value)
    }
}

impl From<ResolveError> for Error {
    fn from(value: ResolveError) -> Self {
        Self::Resolve(value)
    }
}

impl From<ApplyError> for Error {
    fn from(value: ApplyError) -> Self {
        Self::Apply(value)
    }
}

impl From<RuntimeError> for Error {
    fn from(value: RuntimeError) -> Self {
        Self::Runtime(value)
    }
}

/// Generic transaction cache over any atomic [`Blobs`] backend.
///
/// Production uses [`Db`], while tests use `Cache<Arc<MemBlobs>>` to inject
/// failures and inspect backend state.
///
/// This local milestone supports one recovered cache namespace per
/// process. Native tables and the Mako timestamp authority are process-wide;
/// independently reopening another pre-existing backend after transactions
/// have begun cannot retroactively establish one timestamp history. A future
/// multi-cache supervisor must preflight every backend before admitting work.
pub struct Cache<B: Blobs + 'static> {
    local: LocalDb,
    writeback: Arc<Writeback<B>>,
    runtime: Mutex<Option<Runtime<B>>>,
    record_checksum: RecordChecksum,
    foreground_mode: ForegroundMode,
    /// Lease acquisition is a cold, once-per-owner operation. The lease
    /// itself is thread-affine, so ordinary transactions pay no owner atomic.
    single_producer_claimed: AtomicBool,
    // Common writers publish only to their thread-private fence cache line.
    // Read-only/no-op commits close admission and drain those lines so they
    // cannot pass a writer whose post-install outcome is still unresolved.
    commit_fence: CommitFence,
}

/// Production cache using RocksDB as its asynchronous backend.
pub type Db = Cache<RocksBlobs>;

impl Cache<RocksBlobs> {
    /// Open or create a RocksDB-backed transaction cache.
    pub fn open<P: AsRef<Path>>(path: P, options: Options) -> Result<Self, Error> {
        let backend = RocksBlobs::open(path.as_ref(), options.durability)?;
        Self::from_backend(backend, options.cache)
    }
}

impl<B: Blobs + 'static> Cache<B> {
    /// Open over an already constructed atomic backend.
    ///
    /// Recovery and validation complete before the background writer starts
    /// or this cache becomes accessible to callers. The Phase 1 process model
    /// permits only one pre-existing cache namespace; see [`Cache`].
    #[allow(unsafe_code)]
    pub fn from_backend(backend: B, options: CacheOptions) -> Result<Self, Error> {
        let features = mako_local::features()?;
        if options.require_read_my_writes
            && (!features.read_my_writes() || !features.scan_read_my_writes())
        {
            return Err(Error::MissingReadMyWrites);
        }
        if options.isolation == Isolation::Opaque && !features.opacity() {
            return Err(Error::MissingOpacity);
        }

        let local = LocalDb::open()?;
        // Claim before recovery mutates process-local tables or exposes work.
        // Native enforces the documented one-cache-namespace process model.
        let cache_order_mode = match options.foreground_mode {
            ForegroundMode::Concurrent => mako_local::CacheOrderMode::Concurrent,
            ForegroundMode::SingleProducer => mako_local::CacheOrderMode::SingleProducer,
        };
        // SAFETY: construction exclusively owns the fresh facade and fixes
        // foreground mode for the complete Cache lifetime.
        unsafe { local.claim_cache_order_namespace(cache_order_mode)? };
        let applied_seed = recover(&local, &backend, options.writeback.max_record_bytes)?;
        // SAFETY: construction still exclusively owns the claimed LocalDb;
        // no foreground handle or ordering terminal exists until return.
        unsafe { local.reseed_cache_order_namespace(applied_seed.sequence())? };
        local.bind_trusted_table(DEFAULT_TABLE_NAME, DEFAULT_TABLE_ID)?;
        let writeback = Arc::new(Writeback::new_with_watermark_mode(
            backend,
            applied_seed,
            options.writeback,
            options.foreground_mode == ForegroundMode::SingleProducer,
        )?);
        let runtime = Runtime::start_on_cpu(Arc::clone(&writeback), options.writeback_cpu)
            .map_err(Error::RuntimeStart)?;

        Ok(Self {
            local,
            writeback,
            runtime: Mutex::new(Some(runtime)),
            record_checksum: options.record_checksum,
            foreground_mode: options.foreground_mode,
            single_producer_claimed: AtomicBool::new(false),
            commit_fence: CommitFence::new(),
        })
    }

    /// Claim the explicit foreground lease for a single-producer cache.
    ///
    /// At most one lease exists at a time. The returned handle is neither
    /// [`Send`] nor [`Sync`], and transactions borrow it, so safe Rust cannot
    /// overlap foreground commits or move the producer to another thread. A
    /// lease may be dropped and reacquired after all of its transactions end.
    pub fn single_producer(&self) -> Result<SingleProducer<'_, B>, Error> {
        if self.foreground_mode != ForegroundMode::SingleProducer {
            return Err(Error::SingleProducerModeDisabled);
        }
        self.single_producer_claimed
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .map_err(|_| Error::SingleProducerAlreadyClaimed)?;
        Ok(SingleProducer {
            cache: self,
            state: self.writeback.single_producer_state(),
            _thread_affine: PhantomData,
        })
    }

    /// Begin one optimistic transaction on the current OS thread.
    ///
    /// Transactions are structurally neither `Send` nor `Sync`; they must be
    /// completed on the worker that began them and should not cross `.await`.
    /// A cache opened with [`ForegroundMode::SingleProducer`] must instead use
    /// [`SingleProducer::transaction`].
    pub fn transaction(&self) -> Result<Transaction<'_, B>, Error> {
        if self.foreground_mode == ForegroundMode::SingleProducer {
            return Err(Error::SingleProducerHandleRequired);
        }
        self.transaction_with(TransactionForeground::Concurrent)
    }

    fn transaction_with<'cache>(
        &'cache self,
        foreground: TransactionForeground<'cache>,
    ) -> Result<Transaction<'cache, B>, Error> {
        // This is an early fail-fast check. Commit rechecks under its outcome
        // fence, which closes the race with an in-flight ambiguous writer.
        self.writeback.ensure_no_unknown()?;
        let (table, native) = self.local.trusted_bound_transaction()?;
        // The cache and table borrows establish the invariants required by
        // mako-local's build-private trusted Put path. Public cache semantics
        // are unchanged: reads/scans and conditional mutations still use the
        // checked ABI, and commit retains separate visibility/cleanup results.
        Ok(Transaction {
            cache: self,
            table,
            native: Some(native),
            foreground,
        })
    }

    /// Read one key through a read-only transaction.
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, Error> {
        let mut transaction = self.transaction()?;
        let value = transaction.get(key)?;
        transaction.commit()?;
        Ok(value)
    }

    /// Upsert one key in its own transaction.
    pub fn put(&self, key: &[u8], value: &[u8]) -> Result<(), Error> {
        let mut transaction = self.transaction()?;
        transaction.put(key, value)?;
        transaction.commit()
    }

    /// Insert one key if absent, returning whether it was inserted.
    pub fn insert(&self, key: &[u8], value: &[u8]) -> Result<bool, Error> {
        let mut transaction = self.transaction()?;
        let inserted = transaction.insert(key, value)?;
        transaction.commit()?;
        Ok(inserted)
    }

    /// Delete one key, returning whether a live value existed.
    pub fn delete(&self, key: &[u8]) -> Result<bool, Error> {
        let mut transaction = self.transaction()?;
        let existed = transaction.remove(key)?;
        transaction.commit()?;
        Ok(existed)
    }

    /// Wait until every transaction acknowledged before this call has been
    /// applied to RocksDB.
    ///
    /// This drains only the in-memory queue. It does not flush or sync
    /// RocksDB; the selected [`Durability`] controls the ordinary batch-write
    /// options.
    pub fn wait_applied(&self) -> Result<u64, Error> {
        Ok(self.writeback.wait_applied()?)
    }

    /// Compatibility spelling for [`Self::wait_applied`].
    ///
    /// Despite the name, this adds no RocksDB flush or sync beyond the
    /// configured ordinary batch writes.
    pub fn flush(&self) -> Result<u64, Error> {
        self.wait_applied()
    }

    /// Current in-memory progress of the ordered RocksDB consumer.
    pub fn applied_watermark(&self) -> AppliedWatermark {
        self.writeback.applied_watermark()
    }

    /// Highest contiguous cache sequence applied to RocksDB.
    pub fn applied_sequence(&self) -> u64 {
        self.writeback.applied_sequence()
    }

    /// Highest cache-sequence acknowledgement high-water mark.
    ///
    /// This need not be a dense prefix: the trusted concurrent one-Put path
    /// may acknowledge a Ready suffix before an earlier producer completes.
    /// [`Self::wait_applied`] still waits for the complete prefix through this
    /// sequence, or reports an earlier asynchronous fail-stop condition.
    pub fn highest_acknowledged_sequence(&self) -> u64 {
        self.writeback.highest_caller_acknowledged()
    }

    /// Number of bound prepared or ready queue slots.
    pub fn queued_transactions(&self) -> usize {
        self.writeback.queue_len()
    }

    /// Access the backend for read-only diagnostics and tests.
    ///
    /// The cache exclusively owns its tagged keyspace. Calling a mutating
    /// [`Blobs`] method through this reference while the cache is live can
    /// invalidate transaction-log and materialized-state invariants.
    pub fn backend(&self) -> &B {
        self.writeback.backend()
    }

    /// Cleanly drain the acknowledged queue and stop the background writer.
    pub fn close(self) -> Result<u64, Error> {
        self.shutdown()
    }

    /// Stop without draining, modelling loss of the volatile queue on crash.
    ///
    /// This is intended for crash tests and controlled process teardown. Any
    /// sequence above the applied watermark may be lost.
    #[doc(hidden)]
    pub fn abort_without_flush(self) -> Result<(), Error> {
        self.writeback
            .reclaim_packed_occupancy_credits_for_shutdown();
        let mut runtime = self
            .runtime
            .lock()
            .map_err(|_| Error::RuntimeLockPoisoned)?;
        if let Some(mut runtime) = runtime.take() {
            runtime.abort()?;
        }
        Ok(())
    }

    fn shutdown(&self) -> Result<u64, Error> {
        self.writeback
            .reclaim_packed_occupancy_credits_for_shutdown();
        let mut runtime = self
            .runtime
            .lock()
            .map_err(|_| Error::RuntimeLockPoisoned)?;
        match runtime.take() {
            Some(mut runtime) => Ok(runtime.shutdown()?),
            None => Ok(self.writeback.applied_sequence()),
        }
    }
}

/// Exclusive foreground capability for a
/// [`ForegroundMode::SingleProducer`] cache.
///
/// The handle is deliberately neither [`Send`] nor [`Sync`]. Its transaction
/// method also requires a mutable borrow, so safe Rust can have neither a
/// second producer thread nor two simultaneously live detached reservations:
///
/// ```compile_fail
/// # fn require_send<T: Send>() {}
/// require_send::<mako_cache::SingleProducer<
///     'static,
///     std::sync::Arc<mrx_core::fakes::MemBlobs>,
/// >>();
/// ```
///
/// ```compile_fail
/// # fn require_sync<T: Sync>() {}
/// require_sync::<mako_cache::SingleProducer<
///     'static,
///     std::sync::Arc<mrx_core::fakes::MemBlobs>,
/// >>();
/// ```
pub struct SingleProducer<'cache, B: Blobs + 'static> {
    cache: &'cache Cache<B>,
    state: writeback::SingleProducerState,
    _thread_affine: PhantomData<Rc<()>>,
}

impl<'cache, B: Blobs + 'static> SingleProducer<'cache, B> {
    /// Begin one optimistic transaction on this foreground producer.
    ///
    /// The mutable borrow keeps the queue's pre-validation capacity promise
    /// unique until the transaction aborts or binds its exact dense sequence.
    pub fn transaction(&mut self) -> Result<Transaction<'_, B>, Error> {
        // The unique foreground lease prevents another terminal from passing
        // us. Commit performs the authoritative health check immediately
        // before native validation; repeating it here would add an Acquire to
        // every one-operation transaction without closing any additional
        // race. Concurrent transactions retain their early fail-fast check in
        // `Cache::transaction_with`.
        let (table, native) = self.cache.local.trusted_bound_transaction()?;
        Ok(Transaction {
            cache: self.cache,
            table,
            native: Some(native),
            foreground: TransactionForeground::SingleProducer(&self.state),
        })
    }

    /// Read one key through a read-only transaction.
    pub fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>, Error> {
        let mut transaction = self.transaction()?;
        let value = transaction.get(key)?;
        transaction.commit()?;
        Ok(value)
    }

    /// Upsert one key in its own transaction.
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<(), Error> {
        let mut transaction = self.transaction()?;
        transaction.put(key, value)?;
        transaction.commit()
    }

    /// Insert one key if absent, returning whether it was inserted.
    pub fn insert(&mut self, key: &[u8], value: &[u8]) -> Result<bool, Error> {
        let mut transaction = self.transaction()?;
        let inserted = transaction.insert(key, value)?;
        transaction.commit()?;
        Ok(inserted)
    }

    /// Delete one key, returning whether a live value existed.
    pub fn delete(&mut self, key: &[u8]) -> Result<bool, Error> {
        let mut transaction = self.transaction()?;
        let existed = transaction.remove(key)?;
        transaction.commit()?;
        Ok(existed)
    }

    /// Wait until every transaction acknowledged before this call has been
    /// applied to the asynchronous backend.
    pub fn wait_applied(&self) -> Result<u64, Error> {
        self.cache.wait_applied()
    }

    /// Current in-memory applied watermark.
    pub fn applied_watermark(&self) -> AppliedWatermark {
        self.cache.applied_watermark()
    }

    /// Highest dense cache sequence acknowledged to foreground callers.
    pub fn highest_acknowledged_sequence(&self) -> u64 {
        self.cache.highest_acknowledged_sequence()
    }
}

impl<B: Blobs + 'static> Drop for SingleProducer<'_, B> {
    fn drop(&mut self) {
        self.cache
            .single_producer_claimed
            .store(false, Ordering::Release);
    }
}

impl<B: Blobs + 'static> Drop for Cache<B> {
    fn drop(&mut self) {
        if let Err(error) = self.shutdown() {
            eprintln!(
                "mako-cache: acknowledged transactions were not applied at shutdown: {error}"
            );
        }
    }
}

#[derive(Clone, Copy)]
enum TransactionForeground<'cache> {
    Concurrent,
    SingleProducer(&'cache writeback::SingleProducerState),
}

impl<'cache> TransactionForeground<'cache> {
    const fn is_concurrent(self) -> bool {
        matches!(self, Self::Concurrent)
    }

    const fn single_producer(self) -> Option<&'cache writeback::SingleProducerState> {
        match self {
            Self::Concurrent => None,
            Self::SingleProducer(state) => Some(state),
        }
    }
}

/// One single-table cache transaction backed by native Silo.
///
/// Keep every field borrowed or trivially droppable. The exact fused success
/// path forgets this facade after native consumes its only owned resource. A
/// future RAII field must be released or transferred before that fast return.
pub struct Transaction<'db, B: Blobs + 'static> {
    cache: &'db Cache<B>,
    table: mako_local::Table<'db>,
    native: Option<mako_local::Transaction<'db>>,
    foreground: TransactionForeground<'db>,
}

/// A transactional range iterator over the cache's default table.
///
/// Forward and reverse scans both use the logical binary-key range `[lower,
/// upper)`. Items own their key and value bytes. The iterator exclusively
/// borrows its transaction until it is consumed or dropped.
pub struct Scan<'txn, 'db> {
    inner: mako_local::Scan<'txn, 'db>,
}

impl Iterator for Scan<'_, '_> {
    type Item = Result<(Vec<u8>, Vec<u8>), Error>;

    fn next(&mut self) -> Option<Self::Item> {
        self.inner
            .next()
            .map(|result| result.map_err(Error::Native))
    }
}

impl<'db, B: Blobs + 'static> Transaction<'db, B> {
    fn native_mut(&mut self) -> &mut mako_local::Transaction<'db> {
        self.native
            .as_mut()
            .expect("cache transaction already consumed")
    }

    /// Read a key, distinguishing missing from present-empty.
    pub fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>, Error> {
        let table = self.table;
        Ok(self.native_mut().get(&table, key)?)
    }

    /// Scan `[lower, upper)` in ascending binary-key order.
    ///
    /// `None` means no upper bound. Earlier writes staged by this transaction
    /// are reflected in the results.
    pub fn scan<'txn>(
        &'txn mut self,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>, Error> {
        let table = self.table;
        Ok(Scan {
            inner: self.native_mut().scan(&table, lower, upper)?,
        })
    }

    /// Scan `[lower, upper)` in descending binary-key order.
    ///
    /// The lower endpoint remains inclusive and the upper endpoint exclusive.
    pub fn rscan<'txn>(
        &'txn mut self,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>, Error> {
        let table = self.table;
        Ok(Scan {
            inner: self.native_mut().rscan(&table, lower, upper)?,
        })
    }

    /// Upsert a key, returning whether it was newly created.
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<bool, Error> {
        let table = self.table;
        Ok(self.native_mut().put(&table, key, value)?)
    }

    /// Insert a key only when absent, returning whether it was staged.
    pub fn insert(&mut self, key: &[u8], value: &[u8]) -> Result<bool, Error> {
        let table = self.table;
        Ok(self.native_mut().insert(&table, key, value)?)
    }

    /// Remove a key, returning whether a live value existed.
    pub fn remove(&mut self, key: &[u8]) -> Result<bool, Error> {
        let table = self.table;
        Ok(self.native_mut().remove(&table, key)?)
    }

    /// Validate/install in Silo, then publish the preallocated write-back
    /// record before returning success.
    #[allow(unsafe_code)]
    #[inline(always)]
    pub fn commit(mut self) -> Result<(), Error> {
        let foreground = self.foreground;
        if self.cache.record_checksum == RecordChecksum::None {
            if let Some(producer) = foreground.single_producer() {
                #[cfg(test)]
                crate::failpoint::hit(crate::failpoint::Point::BeforeDetachedPreparation);

                let mut cold_attempt = std::mem::MaybeUninit::uninit();
                #[cfg(test)]
                let cache = self.cache;
                // SAFETY: this transaction borrows the unique thread-affine
                // producer for its whole lifetime. The producer control and
                // both local atomic words remain stable with the borrowed
                // cache through this synchronous attempt. On true, native has
                // consumed the raw handle and this branch immediately forgets
                // the outer facade with its stale native ownership state. On
                // false, the Rust wrapper initializes the cold lifecycle value
                // before it is read below; native only initializes control
                // scratch for consumed cold codes.
                let published = unsafe {
                    self.native
                        .as_mut()
                        .unwrap_unchecked()
                        .try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
                            producer.fused_holder_control(),
                            producer.next_sequence_ptr(),
                            producer.capacity_limit_ptr(),
                            &mut cold_attempt,
                        )
                };
                if published {
                    // Native destroyed the only owned resource. Every
                    // remaining field is borrowed or scalar. Forget first so
                    // even a test notifier panic cannot drop the stale native
                    // facade. A future owned field must be released or
                    // transferred before this point.
                    std::mem::forget(self);
                    #[cfg(test)]
                    cache.writeback.notify_fused_holder_published_for_test();
                    return Ok(());
                }
                // SAFETY: guaranteed by the false fast-terminal result.
                let attempt = unsafe { cold_attempt.assume_init() };
                return self.finish_single_producer_fused_cold(producer, attempt);
            }
        }
        self.commit_general()
    }

    /// Fused monomorphic terminal for the latency-sensitive one-Put SPSC ACK.
    ///
    /// The ordinary success path crosses native once and returns only one
    /// scalar lifecycle code. Candidate selection, the capacity check, holder
    /// commit, post-commit health ordering, and ACK publication all remain in
    /// that call. Every fallible or anomalous case is routed to the existing
    /// cold Rust protocol.
    #[allow(unsafe_code)]
    #[cold]
    #[inline(never)]
    fn finish_single_producer_fused_cold(
        mut self,
        producer: &writeback::SingleProducerState,
        mut attempt: mako_local::TrustedFusedOnePutHolderAttempt,
    ) -> Result<(), Error> {
        loop {
            match attempt {
                mako_local::TrustedFusedOnePutHolderAttempt::Published => std::process::abort(),
                mako_local::TrustedFusedOnePutHolderAttempt::UntouchedGeneral => {
                    return self.commit_general();
                }
                mako_local::TrustedFusedOnePutHolderAttempt::UntouchedSlow {
                    exact_record_bytes,
                } => {
                    match self
                        .cache
                        .writeback
                        .reserve_native_holder_single_slow(producer, exact_record_bytes)
                    {
                        Ok(sequence) => {
                            debug_assert_eq!(sequence, producer.retained_sequence());
                            // The cold reservation changes no shared ownership;
                            // it refreshes cached applied capacity and the same
                            // still-active transaction retries the fused gate.
                            let mut cold_attempt = std::mem::MaybeUninit::uninit();
                            #[cfg(test)]
                            let cache = self.cache;
                            // SAFETY: the first fused attempt established this
                            // method's unique-producer and stable-pointer
                            // invariants. The successful slow reservation
                            // changes no ownership and permits this one retry.
                            let published = unsafe {
                                self.native
                                    .as_mut()
                                    .unwrap_unchecked()
                                    .try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
                                        producer.fused_holder_control(),
                                        producer.next_sequence_ptr(),
                                        producer.capacity_limit_ptr(),
                                        &mut cold_attempt,
                                    )
                            };
                            if published {
                                std::mem::forget(self);
                                #[cfg(test)]
                                cache.writeback.notify_fused_holder_published_for_test();
                                return Ok(());
                            }
                            // SAFETY: guaranteed by the false retry result.
                            attempt = unsafe { cold_attempt.assume_init() };
                            continue;
                        }
                        Err(error) => {
                            // Native explicitly left this handle active.
                            let native = unsafe { self.native.take().unwrap_unchecked() };
                            return Err(abort_after_precommit_failure(
                                native,
                                Error::Reserve(error),
                            ));
                        }
                    }
                }
                mako_local::TrustedFusedOnePutHolderAttempt::CommittedUnpublished {
                    timestamp,
                    exact_record_bytes,
                } => {
                    // Native consumed the handle, and the cold decoder advanced
                    // the producer-local cursor. A concurrent fail-stop latch
                    // prevented ACK publication.
                    finish_committed_native_holder_cold(
                        &self.cache.writeback,
                        producer.accepted_sequence(),
                        timestamp,
                        exact_record_bytes,
                    )?;
                    return Ok(());
                }
                mako_local::TrustedFusedOnePutHolderAttempt::ConsumedOutcome {
                    outcome,
                    exact_record_bytes,
                } => {
                    // The cold decoder synchronized the producer-local cursor
                    // to the pre-acceptance ACK. The established resolver can
                    // now accept or pin only when the timestamp proves that
                    // native installed the write.
                    finish_holder_outcome_cold(
                        &self.cache.writeback,
                        producer,
                        producer.retained_sequence(),
                        exact_record_bytes,
                        outcome,
                    )?;
                    return Ok(());
                }
                mako_local::TrustedFusedOnePutHolderAttempt::UntouchedMalformed
                | mako_local::TrustedFusedOnePutHolderAttempt::ConsumedMalformed => {
                    // Ownership is known, but same-build lifecycle metadata is
                    // not. Continuing could either reuse an accepted holder or
                    // touch a consumed facade; terminate fail-closed.
                    std::process::abort();
                }
            }
        }
    }

    #[inline(never)]
    #[allow(unsafe_code)]
    fn commit_general(&mut self) -> Result<(), Error> {
        let foreground = self.foreground;
        let mut native = self
            .native
            .take()
            .expect("cache transaction already consumed");
        // The common unchecked one-Put case carries its exact canonical v4
        // extent out of the trusted Put itself. Native independently rederives
        // that shape at the consuming terminal, so we can avoid a second ABI
        // call and plan-sealing pass without trusting Rust for write coverage.
        // Every other transaction retains the general canonical preflight.
        let max_record_bytes = self.cache.writeback.max_record_bytes();
        let unchecked_one_put = (self.cache.record_checksum == RecordChecksum::None)
            .then(|| native.unchecked_one_put_record_candidate())
            .flatten()
            .filter(|candidate| candidate.exact_record_bytes() <= max_record_bytes);
        let preflight = match unchecked_one_put {
            Some(preflight) => preflight,
            None => native.commit_record_preflight_with_checksum(
                max_record_bytes,
                self.cache.record_checksum,
            )?,
        };

        if preflight.is_empty() {
            if foreground.is_concurrent() {
                // Mark one packed cache-order cut, then drain every
                // writer ordered before it. A later writer may proceed: STO
                // either orders it after this read-only transaction or makes
                // the read-set/predicate validation reject this commit.
                let _fence = self.cache.commit_fence.close_for_read_only(|| {
                    self.cache.local.order_record_validation_prefix();
                });
                if let Err(error) = self.cache.writeback.ensure_no_unknown() {
                    return Err(abort_after_precommit_failure(native, Error::Apply(error)));
                }
                return finish_read_only(native);
            }
            // The mutable thread-affine producer lease excludes every writer
            // and read-only commit, so no outcome fence is needed.
            if let Err(error) = self.cache.writeback.ensure_no_unknown() {
                return Err(abort_after_precommit_failure(native, Error::Apply(error)));
            }
            return finish_read_only(native);
        }

        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::BeforeDetachedPreparation);

        // The trusted one-Put terminal is the only native path whose exact
        // small-record shape and bind exclusion are both known here. The
        // concurrent profile uses native's validation gate; the explicit
        // producer lease supplies stronger whole-call exclusion. Oversized
        // and general transactions retain the defensive binder below.
        if unchecked_one_put.is_some() {
            if let Some(producer) = foreground.single_producer() {
                let exact_record_bytes = NonZeroU32::new(preflight.exact_record_bytes() as u32)
                    .expect("a trusted one-Put record has a nonzero u32 extent");
                let reserved = self
                    .cache
                    .writeback
                    .reserve_native_holder_single_slow(producer, exact_record_bytes);
                match reserved {
                    Ok(sequence) => {
                        return finish_trusted_one_put_holder_single(
                            self.cache,
                            native,
                            exact_record_bytes,
                            producer,
                            sequence,
                        );
                    }
                    Err(error) => {
                        return Err(abort_after_precommit_failure(native, Error::Reserve(error)));
                    }
                }
            }
            let (reserved, concurrent_worker_slot) = match foreground.single_producer() {
                Some(producer) => (
                    self.cache
                        .writeback
                        .reserve_native_arena_fast_single(producer, preflight.exact_record_bytes()),
                    None,
                ),
                None => {
                    let worker_slot = CommitFence::current_thread_slot();
                    (
                        self.cache.writeback.reserve_native_arena_fast_packed(
                            preflight.exact_record_bytes(),
                            worker_slot,
                        ),
                        Some(worker_slot),
                    )
                }
            };
            match reserved {
                Ok(Some(permit)) => {
                    return match foreground {
                        TransactionForeground::Concurrent => {
                            finish_trusted_one_put_concurrent(
                                self.cache,
                                native,
                                preflight,
                                permit,
                                concurrent_worker_slot
                                    .expect("a concurrent arena reserve retains its worker slot"),
                            )
                        }
                        TransactionForeground::SingleProducer(_) => {
                            finish_trusted_one_put_arena_single(
                                self.cache, native, preflight, permit,
                            )
                        }
                    };
                }
                Ok(None) => {}
                Err(error) => {
                    return Err(abort_after_precommit_failure(native, Error::Reserve(error)));
                }
            }
        }

        // Capacity waits and buffer checkout/growth complete before native Silo
        // takes write locks. Common records use a queue-capacity fixed arena;
        // oversized buffers are recycled after background application.
        let reserved = match foreground.single_producer() {
            Some(producer) => self
                .cache
                .writeback
                .reserve_native_single(producer, preflight.exact_record_bytes()),
            None => self
                .cache
                .writeback
                .reserve_native(preflight.exact_record_bytes()),
        };
        let mut permit = match reserved {
            Ok(permit) => permit,
            Err(error) => {
                return Err(abort_after_precommit_failure(native, Error::Reserve(error)));
            }
        };
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::DetachedPrepared);

        // The concurrent profile excludes read-only acknowledgement while a
        // writer's native outcome is unresolved. Each concurrent worker owns
        // a separate cache-line slot; the unique mutable producer lease needs
        // no outcome fence.
        let _fence = foreground
            .is_concurrent()
            .then(|| self.cache.commit_fence.enter_writer());
        if let Err(error) = self.cache.writeback.ensure_no_unknown() {
            return Err(abort_after_precommit_failure(native, Error::Apply(error)));
        }

        let mut bound = None;
        let mut bind_error = None;
        let record_report = match foreground {
            TransactionForeground::Concurrent => {
                let (next_bound, unhealthy) = self.cache.writeback.native_ordering_words();
                // SAFETY: native is the sole concurrent dense allocator. This
                // callback adopts exactly the assigned generation into the
                // uniquely claimed arena/owned buffer and retains it through
                // the synchronous terminal.
                let acquire_target = |timestamp, native_preflight, ordered_sequence| {
                    debug_assert_eq!(native_preflight, preflight);
                    let mut reservation = unsafe {
                        permit.bind_native_externally_ordered(timestamp, ordered_sequence)
                    };
                    // SAFETY: `bound` retains the stable exact target until
                    // native has completed serialization or returned failure.
                    let target = unsafe { reservation.native_record_target() };
                    bound = Some(reservation);
                    #[cfg(test)]
                    crate::failpoint::hit(crate::failpoint::Point::PreinstallBound);
                    Some(target)
                };
                let outcome = if unchecked_one_put.is_some() {
                    // SAFETY: native rederives the current restricted one-Put
                    // candidate and assigns its packed pair only after final
                    // validation. `next_bound` is retained only by the ABI;
                    // exact-turn publication makes the assigned generation
                    // discoverable without another shared tail update.
                    unsafe {
                        native
                            .commit_trusted_native_ordered_unchecked_one_put_record_target(
                                preflight,
                                next_bound,
                                unhealthy,
                                acquire_target,
                            )
                    }
                } else {
                    // SAFETY: the preflight is current and nonempty. Native
                    // owns the packed general bit through timestamp allocation,
                    // final validation, and dense assignment.
                    unsafe {
                        native.commit_trusted_native_ordered_record_target(
                            unhealthy,
                            acquire_target,
                        )
                    }
                };
                if !outcome.order_witness_valid() {
                    std::process::abort();
                }
                // A native exception after dense assignment but before the
                // callback cannot make that order cancelable. Adopt it here so
                // the ordinary unwritten path pins the exact hole fail-closed.
                if bound.is_none() {
                    if let Some((timestamp, sequence)) = outcome.accepted_order() {
                        bound = Some(unsafe {
                            permit.bind_native_externally_ordered(timestamp, sequence)
                        });
                    }
                }
                outcome.into_report()
            }
            TransactionForeground::SingleProducer(_) => {
                // The exclusive producer retains its existing private dense
                // cursor. No concurrent packed allocator can run in this cache
                // mode, and recovery reseeds the packed namespace on reopen.
                let acquire_target = |timestamp, native_preflight| {
                    debug_assert_eq!(native_preflight, preflight);
                    match permit.bind_native(timestamp) {
                        Ok(mut reservation) => {
                            // SAFETY: this reservation remains alive in
                            // `bound` through the synchronous terminal.
                            let target = unsafe { reservation.native_record_target() };
                            bound = Some(reservation);
                            #[cfg(test)]
                            crate::failpoint::hit(crate::failpoint::Point::PreinstallBound);
                            Some(target)
                        }
                        Err(error) => {
                            bind_error = Some(error);
                            None
                        }
                    }
                };
                if unchecked_one_put.is_some() {
                    unsafe {
                        native.commit_report_with_unchecked_one_put_record_target(
                            preflight,
                            acquire_target,
                        )
                    }
                } else {
                    unsafe { native.commit_report_with_record_target(acquire_target) }
                }
            }
        };
        enforce_record_completion_contract(&record_report);
        let report = record_report.commit;
        assert_eq!(
            record_report.record_bound,
            bound.is_some(),
            "native and queue record-binding outcomes diverged"
        );
        #[cfg(test)]
        crate::failpoint::observe_post_native_commit();
        #[cfg(test)]
        if bound.is_some() {
            crate::failpoint::hit(crate::failpoint::Point::NativeCommittedBeforeReady);
        }

        if let Some(reservation) = bound.as_mut() {
            if !record_report.record_written {
                // A sequence was assigned but native did not prove complete
                // initialization. Dropping the reservation pins the ordered
                // slot; treating this as an abort could let later data pass a
                // possibly visible transaction with no replayable record.
                return Err(finish_unwritten_bound(reservation, record_report));
            }
            // SAFETY: completion-contract enforcement above and the explicit
            // record_written branch prove native initialized the exact target.
            unsafe { reservation.attach_written_native_record() };
            return match report.disposition {
                CommitDisposition::Committed => {
                    let sequence = reservation.publish()?;
                    match report.cleanup {
                        Ok(()) => Ok(()),
                        Err(source) => Err(Error::CommittedButCleanupFailed { sequence, source }),
                    }
                }
                // Once the hook returned true, phase 3 may have started. Even
                // a nominal CONFLICT is no longer safe to cancel or skip.
                CommitDisposition::Aborted(source) | CommitDisposition::Unknown(source) => {
                    let sequence = reservation.pin_unknown()?;
                    Err(Error::UnknownCommitOutcome {
                        sequence,
                        source,
                        cleanup: report.cleanup.err(),
                    })
                }
            };
        }

        if let Some(error) = bind_error {
            // The hook returned false, so native installation never began.
            debug_assert!(matches!(
                report.disposition,
                CommitDisposition::Aborted(LocalError::CommitHookRejected)
            ));
            return match report.cleanup {
                Ok(()) => Err(Error::Reserve(error)),
                Err(cleanup) => Err(Error::AbortCleanupFailed {
                    abort: LocalError::CommitHookRejected,
                    cleanup,
                }),
            };
        }

        // A nonempty native preflight implies a write set. If no bind callback
        // ran, Silo never entered installation, so every failure is a definite
        // visibility abort even when the generic wrapper labels cleanup as
        // Unknown. A committed result without a record is an uncovered-write
        // contract violation.
        match report.disposition {
            CommitDisposition::Committed => {
                panic!("native write commit succeeded without invoking its pre-install hook")
            }
            CommitDisposition::Aborted(abort) | CommitDisposition::Unknown(abort) => {
                match report.cleanup {
                    Ok(()) => Err(Error::Native(abort)),
                    Err(cleanup) => Err(Error::AbortCleanupFailed { abort, cleanup }),
                }
            }
        }
    }

    /// Explicitly abort without creating a write-back obligation.
    pub fn abort(mut self) -> Result<(), Error> {
        self.native
            .take()
            .expect("cache transaction already consumed")
            .abort()?;
        Ok(())
    }
}

/// Complete the cache-private concurrent one-Put terminal.
///
/// Native's packed process word pairs the Mako timestamp with the dense queue
/// sequence. Native acquires the exact publication generation only after it
/// assigns that pair, then transfers the staged value allocation into a holder
/// that remains owned through backend retirement. The byte arena remains the
/// compatibility path for an unrepresentable holder tag or crash failpoint.
#[allow(unsafe_code)]
fn finish_trusted_one_put_concurrent<'cache, 'db, B: Blobs + 'static>(
    cache: &'cache Cache<B>,
    native: mako_local::Transaction<'db>,
    preflight: mako_local::CommitRecordPreflight,
    mut permit: writeback::NativeArenaPermit<'cache, B>,
    worker_slot: usize,
) -> Result<(), Error> {
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::DetachedPrepared);

    let fence = cache.commit_fence.enter_writer_slot(worker_slot);
    if let Err(error) = cache.writeback.ensure_no_unknown() {
        return Err(abort_after_precommit_failure(native, Error::Apply(error)));
    }

    let mut bound = None;
    // The crash matrix retains its exact post-bind/pre-serialization seam by
    // selecting the legacy callback terminal only in the helper process which
    // armed that point. Oversized 32-bit records retain the direct byte arena;
    // ordinary records use the callback-free zero-copy holder terminal.
    #[cfg(test)]
    let force_callback = crate::failpoint::is_armed(crate::failpoint::Point::PreinstallBound);
    #[cfg(not(test))]
    let force_callback = false;
    let use_holder_terminal = !force_callback
        && writeback::native_holder_record_supported(preflight.exact_record_bytes());

    let outcome = if use_holder_terminal {
        // SAFETY: the candidate is current, the permit owns one concurrent
        // occupancy claim, and the control borrows this queue's stable exact
        // publication/holder layout for the synchronous native terminal.
        let control = unsafe { cache.writeback.native_ordered_holder_control() };
        unsafe {
            native.commit_trusted_native_ordered_unchecked_one_put_holder(
                preflight,
                &control,
            )
        }
    } else if !force_callback {
        // SAFETY: this is the same direct packed terminal and exact generation
        // ownership, with bytes stored in the queue arena instead of a holder.
        let control = unsafe { cache.writeback.native_ordered_arena_control() };
        unsafe {
            native.commit_trusted_native_ordered_unchecked_one_put_arena(
                preflight,
                &control,
            )
        }
    } else {
        let (next_bound, unhealthy) = cache.writeback.native_ordering_words();
        // SAFETY: this test-only callback runs synchronously after the native
        // packed CAS assigned its timestamp/sequence pair. Adopting the
        // exact FREE generation before returning its target retains the prior
        // crash seam and the same backend-retirement ownership.
        let acquire_target = |timestamp, native_preflight, ordered_sequence| {
            debug_assert_eq!(native_preflight, preflight);
            let mut reservation =
                unsafe { permit.bind_externally_ordered(timestamp, ordered_sequence) };
            // SAFETY: `reservation` moves into `bound` before native can write
            // and remains alive through this synchronous terminal.
            let target = unsafe { reservation.native_record_target() };
            bound = Some(reservation);
            #[cfg(test)]
            crate::failpoint::hit(crate::failpoint::Point::PreinstallBound);
            Some(target)
        };
        // SAFETY: the crash helper uses the established callback contract and
        // every concurrent terminal shares the same packed order namespace.
        unsafe {
            native.commit_trusted_native_ordered_unchecked_one_put_record_target(
                preflight,
                next_bound,
                unhealthy,
                acquire_target,
            )
        }
    };

    // A partial scalar witness means native may have advanced packed order but
    // did not provide enough information to construct the matching record.
    // Do not unwind and release the detached occupancy claim across that hole.
    if !outcome.order_witness_valid() {
        std::process::abort();
    }

    // An exception or malformed post-order callback can return after native
    // assigned the order but before Rust acquired the cell. Adopt it now so
    // the ordinary unwritten-record path pins the exact dense obligation.
    if bound.is_none() {
        if let Some((timestamp, sequence)) = outcome.accepted_order() {
            bound = Some(if !force_callback {
                // SAFETY: a nonzero order returned by the direct terminal also
                // proves native Release-published this exact BOUND generation.
                unsafe { permit.adopt_externally_bound(timestamp, sequence) }
            } else {
                // SAFETY: a callback-terminal exception can expose its order
                // before the Rust binder ran; acquire that FREE generation now.
                unsafe { permit.bind_externally_ordered(timestamp, sequence) }
            });
        }
    }

    if outcome.is_committed() {
        let reservation = bound
            .as_mut()
            .expect("trusted native success must retain its bound arena target");
        #[cfg(test)]
        crate::failpoint::observe_post_native_commit();
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::NativeCommittedBeforeReady);
        // SAFETY: `is_committed` proves the exact sealed-holder witness,
        // definite visibility, and successful cleanup. Publish BOUND -> READY.
        if use_holder_terminal {
            unsafe {
                reservation
                    .publish_holder_completed_concurrent_nonblocking(fence.slot())?
            };
        } else {
            unsafe { reservation.publish_completed_concurrent_nonblocking(fence.slot())? };
        }
        return Ok(());
    }

    // Anomalous/unsuccessful returns retain the complete established fail-stop
    // protocol. Only this cold branch pays for decoding the public report.
    let record_report = outcome.into_report();
    enforce_record_completion_contract(&record_report);
    let report = record_report.commit;
    assert_eq!(
        record_report.record_bound,
        bound.is_some(),
        "native and queue record-binding outcomes diverged"
    );
    #[cfg(test)]
    crate::failpoint::observe_post_native_commit();
    #[cfg(test)]
    if bound.is_some() {
        crate::failpoint::hit(crate::failpoint::Point::NativeCommittedBeforeReady);
    }

    if let Some(reservation) = bound.as_mut() {
        if !record_report.record_written {
            return Err(finish_unwritten_native_arena(reservation, record_report));
        }
        return match report.disposition {
            CommitDisposition::Committed => {
                // SAFETY: report validation plus `record_written` proves the
                // direct holder or callback arena target is complete.
                let sequence = if use_holder_terminal {
                    unsafe {
                        reservation
                            .publish_holder_completed_concurrent_nonblocking(fence.slot())?
                    }
                } else {
                    unsafe { reservation.publish_completed()? }
                };
                match report.cleanup {
                    Ok(()) => Ok(()),
                    Err(source) => Err(Error::CommittedButCleanupFailed { sequence, source }),
                }
            }
            CommitDisposition::Aborted(source) | CommitDisposition::Unknown(source) => {
                // SAFETY: the completion witness makes the holder/bytes
                // replayable, but uncertain visibility permanently pins its
                // sequence.
                let sequence = if use_holder_terminal {
                    unsafe { reservation.pin_holder_completed_unknown()? }
                } else {
                    unsafe { reservation.pin_completed_unknown()? }
                };
                Err(Error::UnknownCommitOutcome {
                    sequence,
                    source,
                    cleanup: report.cleanup.err(),
                })
            }
        };
    }

    match report.disposition {
        CommitDisposition::Committed => {
            panic!("native write commit succeeded without invoking its pre-install hook")
        }
        CommitDisposition::Aborted(abort) | CommitDisposition::Unknown(abort) => {
            match report.cleanup {
                Ok(()) => Err(Error::Native(abort)),
                Err(cleanup) => Err(Error::AbortCleanupFailed { abort, cleanup }),
            }
        }
    }
}

/// Complete the callback-free, preselected one-Put SPSC terminal.
///
/// The thread-affine mutable producer lease excludes every other foreground
/// terminal for this LocalDb. That lets Rust select the exact dense sequence
/// and queue-owned native holder before entering native code. The future
/// generation stays invisible until native returns a nonzero accepted Mako
/// timestamp; an OCC loser therefore consumes no log sequence. Native moves
/// STO's staged value allocation into the holder after install, and Rust
/// Release-publishes only the scalar dense tail.
#[allow(unsafe_code)]
#[inline(always)]
fn finish_trusted_one_put_holder_single<'cache, 'db, B: Blobs + 'static>(
    cache: &'cache Cache<B>,
    native: mako_local::Transaction<'db>,
    exact_record_bytes: NonZeroU32,
    producer: &'cache writeback::SingleProducerState,
    sequence: NonZeroU64,
) -> Result<(), Error> {
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::DetachedPrepared);

    // SAFETY: this terminal is reachable only for a queue and lease created in
    // single-producer mode; the cache outlives the synchronous native call.
    let pool = unsafe { cache.writeback.native_holder_pool_single_unchecked() };

    // SAFETY: the thread-affine producer lease owns this future dense
    // generation and the corresponding masked holder through the synchronous
    // consuming terminal. No consumer can view it before Rust's later Release.
    let outcome = unsafe {
        native.commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
            exact_record_bytes,
            pool,
            sequence,
        )
    };

    if let Some(timestamp) = outcome.committed_timestamp() {
        // SAFETY: the fused compact predicate proves definite visibility,
        // successful cleanup, and a sealed exact-generation holder. Ordinary
        // publication consists only of the producer cursor and ACK stores.
        if unsafe {
            cache
                .writeback
                .try_publish_native_holder_single(producer, sequence)
        } {
            return Ok(());
        }
        return finish_committed_native_holder_cold(
            &cache.writeback,
            sequence,
            timestamp,
            exact_record_bytes,
        );
    }

    finish_holder_outcome_cold(
        &cache.writeback,
        producer,
        sequence,
        exact_record_bytes,
        outcome,
    )
}

/// Attach a definitely committed holder behind a concurrently latched
/// fail-stop barrier. The producer cursor was already advanced by the compact
/// publication attempt, so this path must not make the generation reusable.
#[cold]
#[inline(never)]
fn finish_committed_native_holder_cold<B: Blobs + 'static>(
    writeback: &Writeback<B>,
    sequence: NonZeroU64,
    timestamp: MakoTimestamp,
    exact_record_bytes: NonZeroU32,
) -> Result<(), Error> {
    writeback.publish_native_holder_single_cold(sequence, timestamp, exact_record_bytes)?;
    Ok(())
}

/// Decode a non-success holder outcome entirely off the ordinary ACK path.
#[cold]
#[inline(never)]
fn finish_holder_outcome_cold<B: Blobs + 'static>(
    writeback: &Writeback<B>,
    producer: &writeback::SingleProducerState,
    sequence: NonZeroU64,
    exact_record_bytes: NonZeroU32,
    outcome: mako_local::TrustedPreselectedUncheckedOnePutHolderOutcome,
) -> Result<(), Error> {
    let Some(timestamp) = outcome.accepted_timestamp() else {
        let report = outcome.into_report();
        enforce_unaccepted_holder_completion_contract(&report);
        return match report.commit.disposition {
            CommitDisposition::Committed => unreachable!(
                "a valid unaccepted holder outcome cannot report a committed transaction"
            ),
            CommitDisposition::Aborted(abort) | CommitDisposition::Unknown(abort) => {
                match report.commit.cleanup {
                    Ok(()) => Err(Error::Native(abort)),
                    Err(cleanup) => Err(Error::AbortCleanupFailed { abort, cleanup }),
                }
            }
        };
    };

    finish_anomalous_accepted_holder(
        writeback,
        producer,
        sequence,
        exact_record_bytes,
        timestamp,
        outcome.holder_sealed(),
        || outcome.into_report(),
    )
}

/// Pin every accepted holder result outside the exact ordinary-success word
/// before decoding it into richer status types.
///
/// The pin deliberately precedes `decode`. Besides keeping malformed ABI words
/// fail-closed, this ordering means a panic in a future cold decoder cannot
/// unwind before the retained sequence is made visible and silently make an
/// already accepted sequence reusable.
#[cold]
#[inline(never)]
#[allow(unsafe_code)]
fn finish_anomalous_accepted_holder<B, F>(
    writeback: &Writeback<B>,
    producer: &writeback::SingleProducerState,
    sequence: NonZeroU64,
    exact_record_bytes: NonZeroU32,
    timestamp: MakoTimestamp,
    sealed: bool,
    decode: F,
) -> Result<(), Error>
where
    B: Blobs + 'static,
    F: FnOnce() -> mako_local::CommitHolderReport,
{
    let sequence = if sealed {
        // SAFETY: `sealed` is the raw native completion witness paired with the
        // accepted timestamp. Even when the remaining terminal word is corrupt,
        // these two scalars cover the exact preselected holder generation.
        unsafe {
            writeback.pin_native_holder_sealed_unknown_single(
                producer,
                sequence,
                timestamp,
                exact_record_bytes,
            )?
        }
    } else {
        writeback.pin_native_holder_unsealed_unknown_single(producer, sequence)?
    };

    let report = decode();
    let cleanup = report.commit.cleanup.err();
    let source = match report.commit.disposition {
        // Exact OK/OK + accepted + sealed returned through the fast branch
        // above. Any other word decoded as Committed is an ABI contradiction;
        // the sequence is already pinned, so surface a typed unknown instead
        // of risking later reuse.
        CommitDisposition::Committed => LocalError::Internal,
        CommitDisposition::Aborted(source) | CommitDisposition::Unknown(source) => source,
    };
    Err(Error::UnknownCommitOutcome {
        sequence,
        source,
        cleanup,
    })
}

/// Complete the older direct-arena one-Put SPSC terminal used when native
/// holder deferral is unavailable.
#[allow(unsafe_code)]
fn finish_trusted_one_put_arena_single<'cache, 'db, B: Blobs + 'static>(
    cache: &'cache Cache<B>,
    native: mako_local::Transaction<'db>,
    preflight: mako_local::CommitRecordPreflight,
    mut permit: writeback::NativeArenaPermit<'cache, B>,
) -> Result<(), Error> {
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::DetachedPrepared);

    if let Err(error) = cache.writeback.ensure_no_unknown() {
        return Err(abort_after_precommit_failure(native, Error::Apply(error)));
    }

    // SAFETY: the mutable, thread-affine lease which selected this code path
    // remains borrowed through the consuming terminal and queue resolution.
    // It protects this exact FREE turn and arena extent from every other
    // foreground transaction.
    let preselected = unsafe { permit.preselected_record_target() };
    #[cfg(test)]
    // The old callback path reached this rendezvous after publishing BOUND but
    // still before install. Callback-free SPSC deliberately keeps BOUND
    // invisible until native acceptance; selecting the irrevocable target is
    // its corresponding pre-native crash boundary.
    crate::failpoint::hit(crate::failpoint::Point::PreinstallBound);
    // SAFETY: `preselected` is the exact target advertised by `preflight`; the
    // unique producer lease provides the whole-call exclusion required by the
    // callback-free native ABI.
    let outcome = unsafe {
        native.commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
            preflight,
            preselected.native(),
        )
    };

    let Some(timestamp) = outcome.accepted_timestamp() else {
        // No accepted timestamp means native never crossed the point after
        // which this exact invisible turn must become part of the dense log.
        // Decode before dropping the permit so malformed same-build ABI state
        // triggers the process-level contract guard rather than reusing it.
        let record_report = outcome.into_report();
        enforce_record_completion_contract(&record_report);
        assert!(!record_report.record_bound);
        let report = record_report.commit;
        return match report.disposition {
            CommitDisposition::Committed => {
                panic!("native write commit succeeded without accepting its preselected record")
            }
            CommitDisposition::Aborted(abort) | CommitDisposition::Unknown(abort) => {
                match report.cleanup {
                    Ok(()) => Err(Error::Native(abort)),
                    Err(cleanup) => Err(Error::AbortCleanupFailed { abort, cleanup }),
                }
            }
        };
    };

    if outcome.is_committed() {
        #[cfg(not(test))]
        {
            // SAFETY: the compact success witness proves complete target
            // initialization, definite native visibility, successful cleanup,
            // and the same accepted timestamp. The production SPSC path can
            // publish FREE directly to READY without materializing a bound
            // reservation or an intermediate turn.
            unsafe { permit.publish_preselected_committed_after_native(timestamp, preselected)? };
            return Ok(());
        }
        #[cfg(test)]
        {
            // Keep deterministic crash/response observers between BOUND and
            // READY in test builds. Production contains neither observer and
            // uses the direct publication above.
            // SAFETY: `timestamp` is native's acceptance witness for this
            // exact target and the unique lease remains live.
            let mut reservation =
                unsafe { permit.bind_preselected_after_native(timestamp, preselected) };
            crate::failpoint::observe_post_native_commit();
            crate::failpoint::hit(crate::failpoint::Point::NativeCommittedBeforeReady);
            // SAFETY: established by the compact committed witness.
            unsafe { reservation.publish_completed()? };
            return Ok(());
        }
    }

    // Only anomalous/unsuccessful accepted outcomes pay for a materialized
    // bound reservation and the full decode. Every path below either publishes
    // a known commit or permanently pins the exact dense sequence.
    // SAFETY: every accepted timestamp must bind its exact preselected turn
    // before decoding or executing any other fallible operation.
    let mut reservation = unsafe { permit.bind_preselected_after_native(timestamp, preselected) };
    #[cfg(test)]
    crate::failpoint::observe_post_native_commit();
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::NativeCommittedBeforeReady);
    let record_report = outcome.into_report();
    enforce_record_completion_contract(&record_report);
    assert!(record_report.record_bound);
    if !record_report.record_written {
        return Err(finish_unwritten_native_arena(
            &mut reservation,
            record_report,
        ));
    }

    let report = record_report.commit;
    match report.disposition {
        CommitDisposition::Committed => {
            // SAFETY: the validated completion witness covers the full exact
            // target even though terminal cleanup returned an error.
            let sequence = unsafe { reservation.publish_completed()? };
            match report.cleanup {
                Ok(()) => Ok(()),
                Err(source) => Err(Error::CommittedButCleanupFailed { sequence, source }),
            }
        }
        CommitDisposition::Aborted(source) | CommitDisposition::Unknown(source) => {
            // SAFETY: bytes are complete but visibility is not definitely
            // committed, so retaining them behind a permanent pin is sound.
            let sequence = unsafe { reservation.pin_completed_unknown()? };
            Err(Error::UnknownCommitOutcome {
                sequence,
                source,
                cleanup: report.cleanup.err(),
            })
        }
    }
}

fn finish_read_only(native: mako_local::Transaction<'_>) -> Result<(), Error> {
    let report = native.commit_report();
    match report.disposition {
        CommitDisposition::Committed => report.cleanup.map_err(Error::Native),
        CommitDisposition::Aborted(abort) | CommitDisposition::Unknown(abort) => {
            match report.cleanup {
                Ok(()) => Err(Error::Native(abort)),
                Err(cleanup) => Err(Error::AbortCleanupFailed { abort, cleanup }),
            }
        }
    }
}

/// Abort an active native transaction after Rust-side preparation failed.
///
/// Returning the preparation error is correct only when native cleanup is
/// known complete. A cleanup failure quarantines the worker and is the more
/// severe result, matching mako-local's own fail-closed preflight behavior.
fn abort_after_precommit_failure(native: mako_local::Transaction<'_>, preparation: Error) -> Error {
    match native.abort() {
        Ok(()) => preparation,
        Err(cleanup) => Error::Native(cleanup),
    }
}

#[cold]
fn enforce_record_completion_contract(report: &mako_local::CommitRecordReport) {
    if !report.completion_contract_valid && !report.record_bound {
        // Native consumed the transaction handle but returned a malformed
        // terminal result without invoking the only callback that could
        // establish serialization order and a durability slot. There is no
        // sound sequence to pin and no safe way to admit later work. This is
        // ABI corruption, not a recoverable commit error; terminate even in
        // panic-unwind builds.
        std::process::abort();
    }
}

#[cold]
fn enforce_unaccepted_holder_completion_contract(report: &mako_local::CommitHolderReport) {
    if !report.completion_contract_valid
        || report.holder_bound
        || report.holder_sealed
        || matches!(report.commit.disposition, CommitDisposition::Committed)
    {
        // A valid zero-timestamp result is a definite pre-acceptance abort and
        // leaves the retained holder generation FREE. Anything else could hide
        // a visible write for which Rust has no trusted serialization timestamp.
        // Continuing would let Drop reuse the same dense sequence and holder,
        // so terminate instead of turning ABI corruption into a durability gap.
        std::process::abort();
    }
}

fn finish_unwritten_bound<B: Blobs>(
    reservation: &mut writeback::BoundReservation<'_, B>,
    record_report: mako_local::CommitRecordReport,
) -> Error {
    assert!(record_report.record_bound);
    assert!(!record_report.record_written);
    let sequence = reservation.sequence();
    // Resolve in place so the normal callback-owned Option never needs to move
    // the reservation merely to perform its fail-closed transition. Drop is a
    // defense-in-depth retry if this internal transition unexpectedly fails.
    let _ = reservation.pin_unknown();
    let report = record_report.commit;
    match report.disposition {
        CommitDisposition::Aborted(abort)
            if record_report.completion_contract_valid
                && abort == LocalError::CommitHookRejected =>
        {
            Error::BoundRecordUnwritten {
                sequence,
                abort,
                cleanup: report.cleanup.err(),
            }
        }
        CommitDisposition::Committed => Error::UnknownCommitOutcome {
            sequence,
            source: LocalError::Internal,
            cleanup: report.cleanup.err(),
        },
        CommitDisposition::Aborted(source) | CommitDisposition::Unknown(source) => {
            Error::UnknownCommitOutcome {
                sequence,
                source,
                cleanup: report.cleanup.err(),
            }
        }
    }
}

fn finish_unwritten_native_arena<B: Blobs>(
    reservation: &mut writeback::NativeArenaBoundReservation<'_, B>,
    record_report: mako_local::CommitRecordReport,
) -> Error {
    assert!(record_report.record_bound);
    assert!(!record_report.record_written);
    let sequence = reservation.sequence();
    let _ = reservation.pin_unwritten_unknown();
    let report = record_report.commit;
    match report.disposition {
        CommitDisposition::Aborted(abort)
            if record_report.completion_contract_valid
                && abort == LocalError::CommitHookRejected =>
        {
            Error::BoundRecordUnwritten {
                sequence,
                abort,
                cleanup: report.cleanup.err(),
            }
        }
        CommitDisposition::Committed => Error::UnknownCommitOutcome {
            sequence,
            source: LocalError::Internal,
            cleanup: report.cleanup.err(),
        },
        CommitDisposition::Aborted(source) | CommitDisposition::Unknown(source) => {
            Error::UnknownCommitOutcome {
                sequence,
                source,
                cleanup: report.cleanup.err(),
            }
        }
    }
}

fn recover<B: Blobs>(
    local: &LocalDb,
    backend: &B,
    max_bytes: usize,
) -> Result<AppliedWatermark, Error> {
    let mut keys = Vec::<Vec<u8>>::new();
    backend.for_each_key(&mut |key| keys.push(key.to_vec()))?;
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::RecoveryKeysEnumerated);

    let mut log_keys = Vec::<(CommitSeq, Vec<u8>)>::new();
    let mut data_keys = Vec::<(Vec<u8>, u64, Vec<u8>)>::new();
    for key in keys {
        match classify_backend_key(&key) {
            BackendKey::Log(sequence) => log_keys.push((sequence, key)),
            BackendKey::Data { table_id, key: raw } => {
                if table_id != DEFAULT_TABLE_ID {
                    return Err(Error::UnsupportedTable(table_id));
                }
                data_keys.push((key.clone(), table_id, raw.to_vec()));
            }
            BackendKey::Foreign => return Err(Error::ForeignBackendKey),
        }
    }
    log_keys.sort_unstable_by_key(|(sequence, _)| *sequence);
    #[cfg(test)]
    let record_count = log_keys.len();

    let mut records = Vec::new();
    records
        .try_reserve_exact(log_keys.len())
        .map_err(|_| Error::AllocationFailed)?;
    let mut previous = 0u64;
    let mut previous_mako_timestamp = None;
    for (sequence, key) in log_keys {
        let expected = previous.checked_add(1).ok_or(Error::BackendStateMismatch)?;
        if sequence.get() != expected {
            return Err(Error::BackendStateMismatch);
        }
        let value = backend.get(&key)?.ok_or(Error::BackendStateMismatch)?;
        let record = CommitRecord::decode(&key, &value, max_bytes)?;
        if previous_mako_timestamp.is_some_and(|timestamp| record.mako_timestamp() <= timestamp) {
            return Err(Error::BackendStateMismatch);
        }
        previous_mako_timestamp = Some(record.mako_timestamp());
        for mutation in record.mutations() {
            let table_id = match mutation {
                Mutation::Put { table_id, .. } | Mutation::Delete { table_id, .. } => *table_id,
            };
            if table_id != DEFAULT_TABLE_ID {
                return Err(Error::UnsupportedTable(table_id));
            }
        }
        previous = record.sequence().get();
        records.push(record);
        #[cfg(test)]
        {
            if records.len() == 1 {
                crate::failpoint::hit(crate::failpoint::Point::RecoveryFirstRecordValidated);
            }
            if records.len() == record_count {
                crate::failpoint::hit(crate::failpoint::Point::RecoveryLastRecordValidated);
            }
        }
    }

    validate_materialized_data(backend, &records, data_keys)?;
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::RecoveryMaterializedValidated);

    // Mako's logical counter is process-local. Raise it above every recovered
    // timestamp before replay completes and the recovered cache is exposed,
    // otherwise a process restart could reuse transaction timestamps.
    let applied_mako_timestamp = records.last().map(CommitRecord::mako_timestamp);
    if let Some(timestamp) = applied_mako_timestamp {
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryBeforeClockFloor);
        mako_local::advance_mako_timestamp_past(timestamp)?;
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::RecoveryAfterClockFloor);
    }
    replay_records(local, &records)?;
    #[cfg(test)]
    crate::failpoint::hit(crate::failpoint::Point::RecoveryReplayComplete);
    Ok(AppliedWatermark::recovered(
        previous,
        applied_mako_timestamp,
    ))
}

fn validate_materialized_data<B: Blobs>(
    backend: &B,
    records: &[CommitRecord],
    data_keys: Vec<(Vec<u8>, u64, Vec<u8>)>,
) -> Result<(), Error> {
    let mut final_state = BTreeMap::<(u64, Vec<u8>), Option<Vec<u8>>>::new();
    for record in records {
        for mutation in record.mutations() {
            match mutation {
                Mutation::Put {
                    table_id,
                    key,
                    value,
                } => {
                    final_state.insert((*table_id, key.clone()), Some(value.clone()));
                }
                Mutation::Delete { table_id, key } => {
                    final_state.insert((*table_id, key.clone()), None);
                }
            }
        }
    }

    for (backend_key, table_id, raw_key) in data_keys {
        let expected = final_state
            .remove(&(table_id, raw_key))
            .flatten()
            .ok_or(Error::BackendStateMismatch)?;
        let actual = backend
            .get(&backend_key)?
            .ok_or(Error::BackendStateMismatch)?;
        if actual != expected {
            return Err(Error::BackendStateMismatch);
        }
    }
    if final_state.values().any(Option::is_some) {
        return Err(Error::BackendStateMismatch);
    }
    Ok(())
}

fn replay_records(local: &LocalDb, records: &[CommitRecord]) -> Result<(), Error> {
    let table = local.open_table(DEFAULT_TABLE_NAME, DEFAULT_TABLE_ID)?;
    #[cfg(test)]
    let replay_midpoint = records.len() / 2;
    #[cfg(test)]
    let mut records_replayed = 0usize;
    for record in records {
        let mut transaction = local.transaction()?;
        for mutation in record.mutations() {
            match mutation {
                Mutation::Put { key, value, .. } => {
                    transaction.put(&table, key, value)?;
                }
                Mutation::Delete { key, .. } => {
                    if !transaction.remove(&table, key)? {
                        return Err(Error::RecoveryDiverged);
                    }
                }
            }
        }
        let report = transaction.commit_report();
        match report.disposition {
            CommitDisposition::Committed if report.cleanup.is_ok() => {}
            CommitDisposition::Committed => {
                return Err(Error::Native(
                    report.cleanup.expect_err("cleanup checked as failed"),
                ));
            }
            CommitDisposition::Aborted(error) | CommitDisposition::Unknown(error) => {
                return Err(Error::Native(error));
            }
        }
        #[cfg(test)]
        record_replayed_sequence(record.sequence());
        #[cfg(test)]
        {
            records_replayed += 1;
            if records_replayed == replay_midpoint {
                crate::failpoint::hit(crate::failpoint::Point::RecoveryMidReplay);
            }
        }
    }
    Ok(())
}

#[cfg(test)]
thread_local! {
    static RECOVERY_REPLAY_AUDIT: std::cell::RefCell<Option<Vec<u64>>> = const {
        std::cell::RefCell::new(None)
    };
}

#[cfg(test)]
fn begin_replay_audit() {
    RECOVERY_REPLAY_AUDIT.with(|audit| {
        let previous = audit.replace(Some(Vec::new()));
        assert!(previous.is_none(), "nested recovery replay audit");
    });
}

#[cfg(test)]
fn record_replayed_sequence(sequence: CommitSeq) {
    RECOVERY_REPLAY_AUDIT.with(|audit| {
        if let Some(sequences) = audit.borrow_mut().as_mut() {
            sequences.push(sequence.get());
        }
    });
}

#[cfg(test)]
fn finish_replay_audit() -> Vec<u64> {
    RECOVERY_REPLAY_AUDIT.with(|audit| {
        audit
            .replace(None)
            .expect("recovery replay audit was not active")
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn commit_fence_read_only_closer_drains_an_enrolled_writer() {
        use std::sync::mpsc::{channel, TryRecvError};
        use std::time::Duration;

        let fence = Arc::new(CommitFence::new());
        let payload = Arc::new(AtomicUsize::new(0));
        let validation_next = Arc::new(AtomicUsize::new(0));
        let writer = fence.enter_writer_slot(0);
        validation_next.fetch_add(1, Ordering::Release);
        payload.store(17, Ordering::Relaxed);

        let reader_fence = Arc::clone(&fence);
        let reader_payload = Arc::clone(&payload);
        let reader_validation_next = Arc::clone(&validation_next);
        let (cut_tx, cut_rx) = channel();
        let (entered_tx, entered_rx) = channel();
        let reader = std::thread::spawn(move || {
            let _reader = reader_fence.close_for_read_only(|| {
                reader_validation_next.fetch_add(0, Ordering::Acquire);
                cut_tx.send(()).unwrap();
            });
            entered_tx
                .send(reader_payload.load(Ordering::Relaxed))
                .unwrap();
        });

        cut_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("read-only validation cut did not run");
        assert_eq!(entered_rx.try_recv(), Err(TryRecvError::Empty));

        drop(writer);
        assert_eq!(entered_rx.recv_timeout(Duration::from_secs(5)), Ok(17));
        reader.join().unwrap();
    }

    #[test]
    fn commit_fence_writer_after_validation_cut_is_not_blocked() {
        let fence = CommitFence::new();
        let validation_next = AtomicUsize::new(0);
        let reader = fence.close_for_read_only(|| {
            validation_next.fetch_add(0, Ordering::Acquire);
        });

        let writer = fence.enter_writer_slot(1);
        validation_next.fetch_add(1, Ordering::Release);
        assert!(
            fence.writers[1].generation.load(Ordering::Acquire) & 1 != 0,
            "a post-cut writer should enter without waiting for the reader"
        );
        drop(writer);
        drop(reader);
    }

    #[test]
    fn commit_fence_exact_generation_does_not_bridge_worker_reuse() {
        use std::sync::mpsc::{channel, sync_channel};
        use std::time::Duration;

        let fence = Arc::new(CommitFence::new());
        let validation_next = Arc::new(AtomicUsize::new(0));
        let first = fence.enter_writer_slot(2);
        validation_next.fetch_add(1, Ordering::Release);

        let reader_fence = Arc::clone(&fence);
        let reader_validation_next = Arc::clone(&validation_next);
        let (observed_tx, observed_rx) = channel();
        let (reused_tx, reused_rx) = sync_channel(0);
        let (drained_tx, drained_rx) = channel();
        let reader = std::thread::spawn(move || {
            reader_validation_next.fetch_add(0, Ordering::Acquire);
            let observed = reader_fence.writers[2].generation.load(Ordering::Acquire);
            assert_eq!(observed & 1, 1);
            observed_tx.send(observed).unwrap();
            reused_rx.recv().unwrap();
            let mut spins = 0;
            reader_fence.writers[2].drain_observed(observed, &mut spins);
            drained_tx.send(()).unwrap();
        });

        let first_generation = observed_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("reader did not observe the first active generation");
        drop(first);
        let second = fence.enter_writer_slot(2);
        validation_next.fetch_add(1, Ordering::Release);
        assert_ne!(
            fence.writers[2].generation.load(Ordering::Acquire),
            first_generation
        );
        reused_tx.send(()).unwrap();
        drained_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("the first generation wait bridged into its successor");

        // The reader returned while the deliberately post-cut successor still
        // owned the same worker slot.
        assert_eq!(fence.writers[2].generation.load(Ordering::Acquire) & 1, 1);
        drop(second);
        reader.join().unwrap();
    }

    #[test]
    fn unbound_native_record_contract_violation_aborts_process() {
        const ROLE: &str = "MAKO_CACHE_UNBOUND_RECORD_CONTRACT_VIOLATION_ROLE";
        if std::env::var_os(ROLE).is_some() {
            enforce_record_completion_contract(&mako_local::CommitRecordReport {
                commit: mako_local::CommitReport {
                    disposition: CommitDisposition::Unknown(LocalError::Internal),
                    cleanup: Err(LocalError::Internal),
                },
                completion_contract_valid: false,
                record_bound: false,
                record_written: false,
            });
            unreachable!("the uncovered native completion must fail-stop");
        }

        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .arg("--exact")
            .arg("tests::unbound_native_record_contract_violation_aborts_process")
            .env(ROLE, "1")
            .status()
            .expect("run fail-stop subprocess");
        assert!(!status.success(), "contract corruption process survived");
        #[cfg(unix)]
        {
            use std::os::unix::process::ExitStatusExt;
            assert_eq!(status.signal(), Some(6), "expected SIGABRT");
        }
    }

    #[test]
    fn unaccepted_native_holder_contract_violation_aborts_process() {
        const ROLE: &str = "MAKO_CACHE_UNACCEPTED_HOLDER_CONTRACT_VIOLATION_ROLE";
        if std::env::var_os(ROLE).is_some() {
            enforce_unaccepted_holder_completion_contract(&mako_local::CommitHolderReport {
                commit: mako_local::CommitReport {
                    disposition: CommitDisposition::Unknown(LocalError::Internal),
                    cleanup: Err(LocalError::Internal),
                },
                completion_contract_valid: false,
                holder_bound: false,
                holder_sealed: false,
            });
            unreachable!("the uncovered native holder completion must fail-stop");
        }

        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .arg("--exact")
            .arg("tests::unaccepted_native_holder_contract_violation_aborts_process")
            .env(ROLE, "1")
            .status()
            .expect("run holder fail-stop subprocess");
        assert!(!status.success(), "holder contract corruption survived");
        #[cfg(unix)]
        {
            use std::os::unix::process::ExitStatusExt;
            assert_eq!(status.signal(), Some(6), "expected SIGABRT");
        }
    }

    #[cfg(have_mako)]
    #[test]
    fn anomalous_accepted_holder_pins_before_cold_decode() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let mut config = WritebackConfig::default();
        config.capacity = 1;
        let writeback = Writeback::new_with_watermark_mode(
            Arc::new(MemBlobs::new()),
            AppliedWatermark::default(),
            config,
            true,
        )
        .unwrap();
        let producer = writeback.single_producer_state();
        let sequence = writeback
            .reserve_native_holder_single(&producer, NonZeroU32::new(64).unwrap())
            .unwrap();
        let timestamp = MakoTimestamp::new(17).unwrap();

        let error = finish_anomalous_accepted_holder(
            &writeback,
            &producer,
            sequence,
            NonZeroU32::new(64).unwrap(),
            timestamp,
            false,
            || {
                assert!(matches!(
                    writeback.ensure_no_unknown(),
                    Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
                ));
                mako_local::CommitHolderReport {
                    commit: mako_local::CommitReport {
                        disposition: CommitDisposition::Unknown(LocalError::Internal),
                        cleanup: Err(LocalError::Internal),
                    },
                    completion_contract_valid: false,
                    holder_bound: true,
                    holder_sealed: false,
                }
            },
        )
        .unwrap_err();
        assert!(matches!(
            error,
            Error::UnknownCommitOutcome {
                sequence,
                source: LocalError::Internal,
                cleanup: Some(LocalError::Internal),
            } if sequence.get() == 1
        ));
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn anomalous_accepted_holder_stays_pinned_if_decoder_panics() {
        use std::panic::{AssertUnwindSafe, catch_unwind};
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let writeback = Writeback::new_with_watermark_mode(
            Arc::new(MemBlobs::new()),
            AppliedWatermark::default(),
            WritebackConfig::default(),
            true,
        )
        .unwrap();
        let producer = writeback.single_producer_state();
        let sequence = writeback
            .reserve_native_holder_single(&producer, NonZeroU32::new(64).unwrap())
            .unwrap();
        let timestamp = MakoTimestamp::new(18).unwrap();

        let panic = catch_unwind(AssertUnwindSafe(|| {
            let _ = finish_anomalous_accepted_holder(
                &writeback,
                &producer,
                sequence,
                NonZeroU32::new(64).unwrap(),
                timestamp,
                false,
                || {
                    assert!(matches!(
                        writeback.ensure_no_unknown(),
                        Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
                    ));
                    panic!("synthetic cold decoder failure");
                },
            );
        }));
        assert!(panic.is_err());
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
    }

    #[test]
    fn bound_unwritten_completion_is_typed_and_pins_its_dense_slot() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        fn reservation<'a>(
            writeback: &'a Writeback<Arc<MemBlobs>>,
        ) -> writeback::BoundReservation<'a, Arc<MemBlobs>> {
            writeback
                .reserve(vec![Mutation::Put {
                    table_id: DEFAULT_TABLE_ID,
                    key: b"unwritten".to_vec(),
                    value: b"never-installed".to_vec(),
                }])
                .unwrap()
                .bind(MakoTimestamp::new(1).unwrap())
                .unwrap()
        }

        let writeback =
            Writeback::new(Arc::new(MemBlobs::new()), 0, WritebackConfig::default()).unwrap();
        let error = finish_unwritten_bound(
            &mut reservation(&writeback),
            mako_local::CommitRecordReport {
                commit: mako_local::CommitReport {
                    disposition: CommitDisposition::Aborted(LocalError::CommitHookRejected),
                    cleanup: Ok(()),
                },
                completion_contract_valid: true,
                record_bound: true,
                record_written: false,
            },
        );
        assert!(matches!(
            error,
            Error::BoundRecordUnwritten {
                sequence,
                abort: LocalError::CommitHookRejected,
                cleanup: None,
            } if sequence.get() == 1
        ));
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));

        let malformed =
            Writeback::new(Arc::new(MemBlobs::new()), 0, WritebackConfig::default()).unwrap();
        let error = finish_unwritten_bound(
            &mut reservation(&malformed),
            mako_local::CommitRecordReport {
                commit: mako_local::CommitReport {
                    disposition: CommitDisposition::Unknown(LocalError::Internal),
                    cleanup: Err(LocalError::Internal),
                },
                completion_contract_valid: false,
                record_bound: true,
                record_written: false,
            },
        );
        assert!(matches!(
            error,
            Error::UnknownCommitOutcome {
                sequence,
                source: LocalError::Internal,
                cleanup: Some(LocalError::Internal),
            } if sequence.get() == 1
        ));
        assert!(matches!(
            malformed.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));

        // Defense in depth: even if a future wrapper accidentally marks this
        // impossible post-bind conflict shape as contract-valid, the cache
        // must not tell callers it was a retryable definite abort.
        let impossible_conflict =
            Writeback::new(Arc::new(MemBlobs::new()), 0, WritebackConfig::default()).unwrap();
        let error = finish_unwritten_bound(
            &mut reservation(&impossible_conflict),
            mako_local::CommitRecordReport {
                commit: mako_local::CommitReport {
                    disposition: CommitDisposition::Aborted(LocalError::Conflict),
                    cleanup: Ok(()),
                },
                completion_contract_valid: true,
                record_bound: true,
                record_written: false,
            },
        );
        assert!(matches!(
            error,
            Error::UnknownCommitOutcome {
                sequence,
                source: LocalError::Conflict,
                cleanup: None,
            } if sequence.get() == 1
        ));
        assert!(matches!(
            impossible_conflict.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
    }

    #[cfg(have_mako)]
    fn test_record(sequence: u64, timestamp: u32, key: &[u8]) -> CommitRecord {
        test_record_with_mutations(
            sequence,
            timestamp,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: key.to_vec(),
                value: b"value".to_vec(),
            }],
        )
    }

    #[cfg(have_mako)]
    fn test_record_with_mutations(
        sequence: u64,
        timestamp: u32,
        mutations: Vec<Mutation>,
    ) -> CommitRecord {
        crate::record::PreparedCommitRecord::prepare(
            mutations,
            CacheOptions::default().writeback.max_record_bytes,
        )
        .unwrap()
        .bind(
            CommitSeq::new(sequence).expect("test sequence is nonzero"),
            MakoTimestamp::new(timestamp).expect("test timestamp is nonzero"),
        )
        .finalize()
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_replays_each_cache_sequence_exactly_once_in_order() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let put = |key: &[u8], value: &[u8]| Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: key.to_vec(),
            value: value.to_vec(),
        };
        let delete = |key: &[u8]| Mutation::Delete {
            table_id: DEFAULT_TABLE_ID,
            key: key.to_vec(),
        };
        let backend = Arc::new(MemBlobs::new());
        let records = [
            test_record_with_mutations(
                1,
                301,
                vec![
                    put(b"phase1f/replay/chain", b"v1"),
                    put(b"phase1f/replay/a", b"one"),
                ],
            ),
            test_record_with_mutations(
                2,
                302,
                vec![
                    put(b"phase1f/replay/chain", b"v2"),
                    delete(b"phase1f/replay/a"),
                    put(b"phase1f/replay/b", b"two"),
                ],
            ),
            test_record_with_mutations(
                3,
                303,
                vec![
                    put(b"phase1f/replay/chain", b"v3"),
                    delete(b"phase1f/replay/b"),
                    put(b"phase1f/replay/c", b"three"),
                ],
            ),
        ];
        for record in &records {
            backend.write_batch(&record.backend_ops()).unwrap();
        }

        begin_replay_audit();
        let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
            .expect("recover audited history");
        assert_eq!(
            finish_replay_audit(),
            vec![1, 2, 3],
            "recovery must replay each dense CacheSeq exactly once and in order"
        );
        assert_eq!(cache.applied_sequence(), 3);
        assert_eq!(
            cache.get(b"phase1f/replay/chain").unwrap().as_deref(),
            Some(&b"v3"[..])
        );
        assert_eq!(cache.get(b"phase1f/replay/a").unwrap(), None);
        assert_eq!(cache.get(b"phase1f/replay/b").unwrap(), None);
        assert_eq!(
            cache.get(b"phase1f/replay/c").unwrap().as_deref(),
            Some(&b"three"[..])
        );
        cache.close().expect("close audited recovery cache");
    }

    #[test]
    fn default_profile_requires_read_your_writes() {
        assert!(CacheOptions::default().require_read_my_writes);
        assert_eq!(
            CacheOptions::default().foreground_mode,
            ForegroundMode::Concurrent
        );
        assert_eq!(
            Options::default().durability,
            Durability::Wal,
            "the production cache must not request a per-write disk sync"
        );
    }

    #[cfg(have_mako)]
    #[test]
    fn single_producer_mode_requires_and_exclusively_reuses_its_lease() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let mut options = CacheOptions::default();
        options.foreground_mode = ForegroundMode::SingleProducer;
        options.record_checksum = RecordChecksum::None;
        options.writeback.capacity = 1;
        let cache = Cache::from_backend(Arc::clone(&backend), options).unwrap();

        assert!(matches!(
            cache.transaction(),
            Err(Error::SingleProducerHandleRequired)
        ));
        assert!(matches!(
            cache.put(b"bare", b"rejected"),
            Err(Error::SingleProducerHandleRequired)
        ));

        let mut producer = cache.single_producer().unwrap();
        assert!(matches!(
            cache.single_producer(),
            Err(Error::SingleProducerAlreadyClaimed)
        ));
        producer.put(b"leased", b"value").unwrap();
        assert_eq!(
            producer.get(b"leased").unwrap().as_deref(),
            Some(&b"value"[..])
        );
        assert_eq!(producer.highest_acknowledged_sequence(), 1);
        producer.wait_applied().unwrap();
        drop(producer);

        let mut reacquired = cache.single_producer().unwrap();
        assert!(reacquired.delete(b"leased").unwrap());
        drop(reacquired);
        assert_eq!(cache.wait_applied().unwrap(), 2);
        cache.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn capacity_one_native_holder_survives_retry_and_wraps_three_generations() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let mut options = CacheOptions::default();
        options.foreground_mode = ForegroundMode::SingleProducer;
        options.record_checksum = RecordChecksum::None;
        options.writeback.capacity = 1;
        let max_record_bytes = options.writeback.max_record_bytes;
        let cache = Cache::from_backend(Arc::clone(&backend), options).unwrap();
        let mut producer = cache.single_producer().unwrap();

        backend.fail_next_writes(1);
        assert!(backend.is_failing());
        let writes: [(&[u8], &[u8]); 3] = [
            (b"holder-wrap/a", b"value-one"),
            (b"holder-wrap/b", b"value-two"),
            (b"holder-wrap/c", b"value-three"),
        ];
        for (index, (key, value)) in writes.iter().copied().enumerate() {
            producer.put(key, value).unwrap();
            assert_eq!(
                producer.highest_acknowledged_sequence(),
                (index + 1) as u64,
                "each acknowledged Put must occupy the next holder generation"
            );
        }

        assert_eq!(producer.wait_applied().unwrap(), 3);
        assert_eq!(producer.applied_watermark().sequence(), 3);
        assert!(!backend.is_failing(), "the injected failure was consumed");
        assert_eq!(
            backend.batch_count(),
            3,
            "capacity one permits exactly one successful record per batch"
        );
        assert_eq!(backend.op_count(), 6, "each Put writes one log and one row");

        let snapshot = backend.snapshot();
        let mut log_records = snapshot
            .iter()
            .filter_map(|(key, encoded)| match classify_backend_key(key) {
                BackendKey::Log(_) => Some(
                    CommitRecord::decode(key, encoded, max_record_bytes)
                        .expect("wrapped holder emitted a valid record"),
                ),
                BackendKey::Data { .. } | BackendKey::Foreign => None,
            })
            .collect::<Vec<_>>();
        log_records.sort_unstable_by_key(|record| record.sequence());
        assert_eq!(log_records.len(), writes.len());
        for (index, (record, (key, value))) in
            log_records.iter().zip(writes.iter().copied()).enumerate()
        {
            assert_eq!(record.sequence().get(), (index + 1) as u64);
            assert_eq!(
                record.mutations(),
                &[Mutation::Put {
                    table_id: DEFAULT_TABLE_ID,
                    key: key.to_vec(),
                    value: value.to_vec(),
                }],
                "holder reuse corrupted generation {}",
                index + 1
            );
            assert!(snapshot.iter().any(|(stored_key, stored_value)| {
                matches!(
                    classify_backend_key(stored_key),
                    BackendKey::Data {
                        table_id: DEFAULT_TABLE_ID,
                        key: stored_key,
                    } if stored_key == key
                ) && stored_value.as_slice() == value
            }));
        }

        drop(producer);
        assert_eq!(cache.applied_sequence(), 3);
        cache.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn concurrent_mode_rejects_a_single_producer_lease() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let cache =
            Cache::from_backend(Arc::new(MemBlobs::new()), CacheOptions::default()).unwrap();
        assert!(matches!(
            cache.single_producer(),
            Err(Error::SingleProducerModeDisabled)
        ));
        cache.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn native_mako_timestamp_is_carried_into_the_backend_record() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default()).unwrap();
        cache.put(b"timestamped", b"value").unwrap();
        let sequence = cache.flush().unwrap();

        let (log_key, encoded) = backend
            .snapshot()
            .into_iter()
            .find(|(key, _)| matches!(classify_backend_key(key), BackendKey::Log(_)))
            .expect("one backend transaction record");
        let record = CommitRecord::decode(
            &log_key,
            &encoded,
            CacheOptions::default().writeback.max_record_bytes,
        )
        .unwrap();
        assert_eq!(record.sequence().get(), sequence);
        assert_ne!(record.mako_timestamp().get(), 0);

        cache.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn unchecked_one_put_cache_path_emits_and_recovers_v4() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let mut options = CacheOptions::default();
        options.record_checksum = RecordChecksum::None;
        let cache = Cache::from_backend(Arc::clone(&backend), options).unwrap();
        cache
            .put(b"unchecked-fast-key", b"unchecked-fast-value")
            .unwrap();
        assert_eq!(cache.flush().unwrap(), 1);

        let (_, encoded) = backend
            .snapshot()
            .into_iter()
            .find(|(key, _)| matches!(classify_backend_key(key), BackendKey::Log(_)))
            .expect("one unchecked backend transaction record");
        assert_eq!(&encoded[..8], b"MAKONOC\0");
        assert_eq!(u16::from_be_bytes([encoded[8], encoded[9]]), 4);

        cache.close().unwrap();
        let reopened = Cache::from_backend(Arc::clone(&backend), options).unwrap();
        assert_eq!(
            reopened.get(b"unchecked-fast-key").unwrap().as_deref(),
            Some(&b"unchecked-fast-value"[..])
        );
        reopened.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_rejects_a_cache_sequence_gap() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let record = test_record(2, 101, b"gap");
        backend.write_batch(&record.backend_ops()).unwrap();

        assert!(matches!(
            Cache::from_backend(backend, CacheOptions::default()),
            Err(Error::BackendStateMismatch)
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_rejects_non_increasing_mako_timestamps() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        for (label, second_timestamp) in [("duplicate", 202), ("decreasing", 201)] {
            let backend = Arc::new(MemBlobs::new());
            let first = test_record(1, 202, b"first");
            let second = test_record(2, second_timestamp, label.as_bytes());
            backend.write_batch(&first.backend_ops()).unwrap();
            backend.write_batch(&second.backend_ops()).unwrap();

            assert!(matches!(
                Cache::from_backend(backend, CacheOptions::default()),
                Err(Error::BackendStateMismatch)
            ));
        }
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_reconstructs_the_in_memory_applied_frontier() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        const FIRST_TIMESTAMP: u32 = (1 << 25) - 1;
        const FRONTIER_TIMESTAMP: u32 = 1 << 25;
        let backend = Arc::new(MemBlobs::new());
        let first = test_record(1, FIRST_TIMESTAMP, b"first-timestamp");
        let second = test_record(2, FRONTIER_TIMESTAMP, b"sequence-frontier");
        backend.write_batch(&first.backend_ops()).unwrap();
        backend.write_batch(&second.backend_ops()).unwrap();

        let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default()).unwrap();
        assert_eq!(cache.applied_watermark().sequence(), 2);
        assert_eq!(
            cache.applied_watermark().mako_timestamp(),
            MakoTimestamp::new(FRONTIER_TIMESTAMP),
            "the timestamp identifies CacheSeq 2 rather than taking a numeric maximum"
        );

        cache.put(b"after-ordered-recovery", b"new").unwrap();
        cache.wait_applied().unwrap();
        let next = cache.applied_watermark();
        assert_eq!(next.sequence(), 3);
        assert!(
            next.mako_timestamp().unwrap().get() > FRONTIER_TIMESTAMP,
            "clock recovery must continue past the serialized CacheSeq frontier"
        );
        cache.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_advances_mako_timestamp_past_the_recovered_maximum() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        const RECOVERED_TIMESTAMP: u32 = 1 << 24;
        let backend = Arc::new(MemBlobs::new());
        let recovered = test_record(1, RECOVERED_TIMESTAMP, b"recovered");
        backend.write_batch(&recovered.backend_ops()).unwrap();

        let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default()).unwrap();
        cache.put(b"after-recovery", b"new").unwrap();
        cache.flush().unwrap();

        let (log_key, encoded) = backend
            .snapshot()
            .into_iter()
            .find(|(key, _)| {
                matches!(
                    classify_backend_key(key),
                    BackendKey::Log(sequence) if sequence.get() == 2
                )
            })
            .expect("post-recovery transaction record");
        let record = CommitRecord::decode(
            &log_key,
            &encoded,
            CacheOptions::default().writeback.max_record_bytes,
        )
        .unwrap();
        assert!(record.mako_timestamp().get() > RECOVERED_TIMESTAMP);

        cache.close().unwrap();
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_rejects_an_exhausted_recovered_mako_timestamp() {
        use std::sync::Arc;

        use mako_local::MAX_MAKO_TIMESTAMP;
        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let final_record = test_record(1, MAX_MAKO_TIMESTAMP, b"final-timestamp");
        backend.write_batch(&final_record.backend_ops()).unwrap();

        assert!(matches!(
            Cache::from_backend(backend, CacheOptions::default()),
            Err(Error::Native(LocalError::TimestampExhausted))
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn pinned_unknown_fail_stops_new_and_in_flight_read_only_transactions() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let cache = Cache::from_backend(backend, CacheOptions::default()).unwrap();
        let in_flight = cache.transaction().unwrap();

        cache
            .writeback
            .reserve(vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"uncertain".to_vec(),
                value: b"value".to_vec(),
            }])
            .unwrap()
            .bind(MakoTimestamp::new(1).unwrap())
            .unwrap()
            .pin_unknown()
            .unwrap();

        assert!(matches!(
            cache.transaction(),
            Err(Error::Apply(ApplyError::UnknownOutcome { sequence }))
                if sequence.get() == 1
        ));
        assert!(matches!(
            in_flight.commit(),
            Err(Error::Apply(ApplyError::UnknownOutcome { sequence }))
                if sequence.get() == 1
        ));

        assert!(matches!(
            cache.close(),
            Err(Error::Runtime(RuntimeError::Apply(
                ApplyError::UnknownOutcome { sequence }
            ))) if sequence.get() == 1
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn preparation_failure_surfaces_native_abort_cleanup_quarantine() {
        const ROLE: &str = "MAKO_CACHE_ABORT_CLEANUP_QUARANTINE_ROLE";
        if std::env::var_os(ROLE).is_none() {
            let status = std::process::Command::new(std::env::current_exe().unwrap())
                .arg("--exact")
                .arg("tests::preparation_failure_surfaces_native_abort_cleanup_quarantine")
                .env(ROLE, "1")
                .status()
                .expect("run abort-cleanup quarantine subprocess");
            assert!(status.success(), "quarantine subprocess failed: {status}");
            return;
        }

        if !mako_local::features().unwrap().test_cleanup_failures() {
            return;
        }

        std::thread::spawn(|| {
            use std::sync::Arc;

            use mako_local::{
                TestCleanupBoundary, WorkerHealth, arm_test_cleanup_failure, worker_health,
            };
            use mrx_core::fakes::MemBlobs;

            let backend = Arc::new(MemBlobs::new());
            let cache = Cache::from_backend(backend, CacheOptions::default()).unwrap();
            let mut transaction = cache.transaction().unwrap();
            transaction
                .put(b"preparation-cleanup", b"never-installed")
                .unwrap();

            cache
                .writeback
                .reserve(vec![Mutation::Put {
                    table_id: DEFAULT_TABLE_ID,
                    key: b"prior-unknown".to_vec(),
                    value: b"uncertain".to_vec(),
                }])
                .unwrap()
                .bind(MakoTimestamp::new(1).unwrap())
                .unwrap()
                .pin_unknown()
                .unwrap();
            arm_test_cleanup_failure(TestCleanupBoundary::Abort).unwrap();

            assert!(matches!(
                transaction.commit(),
                Err(Error::Native(LocalError::WorkerPoisoned))
            ));
            assert_eq!(worker_health().unwrap(), WorkerHealth::Poisoned);
            let _ = cache.close();
        })
        .join()
        .unwrap();
    }
}
