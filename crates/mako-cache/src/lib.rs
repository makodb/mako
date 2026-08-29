//! Transactional Silo/MassTrans write-back cache over RocksDB.
//!
//! Native C++ Silo is the authoritative live state while this process runs.
//! Before commit, STO sizes and seals its canonical final write set while Rust
//! claims bounded queue capacity and allocates one exact output buffer. After
//! Silo locks the complete write set, then a per-database native gate orders
//! Mako timestamp assignment, final validation, and record binding. A failed
//! validation may leave a timestamp gap but never a cache-log slot. A
//! successful hook assigns the next dense, serialization-safe cache sequence.
//! Native retires that ordering turn, writes the complete checksummed record
//! directly into the exact buffer while retaining all write locks, and only
//! then installs the writes. Native success publishes the already-built record
//! as Ready in the volatile queue. The caller is acknowledged only when Ready
//! forms a dense prefix through that record; one background writer later
//! decodes and replays a bounded contiguous prefix in one atomic RocksDB
//! `WriteBatch`.
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

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::collections::BTreeMap;
use std::fmt;
use std::num::NonZeroU64;
use std::path::Path;
use std::sync::{Arc, Mutex, RwLock};

use mako_local::{CommitDisposition, LocalDb, UninitCommitRecord};
use mrx_core::{BlobError, Blobs};
use mrx_rocks::RocksBlobs;

#[cfg(test)]
mod failpoint;
mod record;
mod runtime;
/// Deterministic cache mutation controls used only by validation binaries.
#[cfg(feature = "test-support")]
#[doc(hidden)]
pub mod test_support;
mod writeback;

#[cfg(all(test, have_mako, have_rocksdb, target_family = "unix"))]
mod crash_tests;

#[cfg(all(test, have_mako))]
mod application_history_tests;

#[cfg(all(test, have_mako))]
mod milestone1_acceptance_tests;

pub use mako_local::{Error as LocalError, MakoTimestamp};
pub use mrx_rocks::Durability;
pub use record::{CommitSeq, RecordError};
pub use writeback::{
    AppliedWatermark, ApplyError, ConfigError as WritebackConfigError, ReserveError, ResolveError,
    WritebackConfig,
};

use record::{classify_backend_key, BackendKey, CommitRecord, Mutation, DEFAULT_TABLE_ID};
use runtime::{Runtime, RuntimeError};
use writeback::Writeback;

const DEFAULT_TABLE_NAME: &[u8] = b"mako-cache/default";

/// Cache-specific behavior independent of the concrete backend.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CacheOptions {
    /// Bounded transaction-log and retry settings.
    pub writeback: WritebackConfig,
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
    // Writers share this fence, so native Silo commits remain concurrent.
    // Read-only/no-op commits take it exclusively to avoid acknowledging a
    // value while a writer's post-install outcome is still unresolved.
    commit_fence: RwLock<()>,
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
        let applied_seed = recover(&local, &backend, options.writeback.max_record_bytes)?;
        let writeback = Arc::new(Writeback::new_with_watermark(
            backend,
            applied_seed,
            options.writeback,
        )?);
        let runtime = Runtime::start(Arc::clone(&writeback)).map_err(Error::RuntimeStart)?;

        Ok(Self {
            local,
            writeback,
            runtime: Mutex::new(Some(runtime)),
            commit_fence: RwLock::new(()),
        })
    }

    /// Begin one optimistic transaction on the current OS thread.
    ///
    /// Transactions are structurally neither `Send` nor `Sync`; they must be
    /// completed on the worker that began them and should not cross `.await`.
    pub fn transaction(&self) -> Result<Transaction<'_, B>, Error> {
        // This is an early fail-fast check. Commit rechecks under its outcome
        // fence, which closes the race with an in-flight ambiguous writer.
        self.writeback.ensure_no_unknown()?;
        let table = self
            .local
            .open_table(DEFAULT_TABLE_NAME, DEFAULT_TABLE_ID)?;
        // The cache and table borrows establish the invariants required by
        // mako-local's build-private trusted Put path. Public cache semantics
        // are unchanged: reads/scans and conditional mutations still use the
        // checked ABI, and commit retains separate visibility/cleanup results.
        let native = self.local.trusted_transaction(&table)?;
        Ok(Transaction {
            cache: self,
            table,
            native: Some(native),
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

    /// Highest dense cache-sequence prefix acknowledged to callers.
    ///
    /// A later record may already be Ready internally while its publisher
    /// waits for an earlier bound transaction to resolve; such a suffix is not
    /// included here.
    pub fn highest_acknowledged_sequence(&self) -> u64 {
        self.writeback.highest_acknowledged()
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

impl<B: Blobs + 'static> Drop for Cache<B> {
    fn drop(&mut self) {
        if let Err(error) = self.shutdown() {
            eprintln!(
                "mako-cache: acknowledged transactions were not applied at shutdown: {error}"
            );
        }
    }
}

/// One single-table cache transaction backed by native Silo.
pub struct Transaction<'db, B: Blobs + 'static> {
    cache: &'db Cache<B>,
    table: mako_local::Table<'db>,
    native: Option<mako_local::Transaction<'db>>,
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
    pub fn commit(mut self) -> Result<(), Error> {
        let mut native = self
            .native
            .take()
            .expect("cache transaction already consumed");
        let preflight = native.commit_record_preflight(self.cache.writeback.max_record_bytes())?;

        if preflight.is_empty() {
            // Writers hold this fence shared from native commit through queue
            // resolution. Taking it exclusively prevents a read-only or
            // logical no-op result from being acknowledged while a possibly
            // observed writer is in the post-install, pre-resolution window.
            let _fence = self
                .cache
                .commit_fence
                .write()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            if let Err(error) = self.cache.writeback.ensure_no_unknown() {
                return Err(abort_after_precommit_failure(native, Error::Apply(error)));
            }
            return finish_read_only(native);
        }

        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::BeforeDetachedPreparation);

        // Capacity waits and the exact byte-buffer allocation both complete
        // before native Silo takes write locks. Native later fills that one
        // buffer directly from its canonical validated TransItems.
        let mut permit = match self
            .cache
            .writeback
            .reserve_native(preflight.exact_record_bytes())
        {
            Ok(permit) => permit,
            Err(error) => {
                return Err(abort_after_precommit_failure(native, Error::Reserve(error)));
            }
        };
        let mut record = match UninitCommitRecord::try_for(preflight) {
            Ok(record) => record,
            Err(error) => {
                return Err(abort_after_precommit_failure(native, Error::Native(error)));
            }
        };
        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::DetachedPrepared);

        // Writers share this fence and therefore still lock, validate, bind,
        // and install concurrently. It only excludes read-only acknowledgement
        // while a writer's native outcome has not yet resolved its queue slot.
        let _fence = self
            .cache
            .commit_fence
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Err(error) = self.cache.writeback.ensure_no_unknown() {
            return Err(abort_after_precommit_failure(native, Error::Apply(error)));
        }

        let mut bound = None;
        let mut bind_error = None;
        let record_report =
            native.commit_report_with_record(&mut record, |timestamp, native_preflight| {
                debug_assert_eq!(native_preflight, preflight);
                match permit.bind_native(timestamp) {
                    Ok(reservation) => {
                        let sequence = NonZeroU64::new(reservation.sequence().get())
                            .expect("cache sequences are nonzero");
                        bound = Some(reservation);
                        #[cfg(test)]
                        crate::failpoint::hit(crate::failpoint::Point::PreinstallBound);
                        Some(sequence)
                    }
                    Err(error) => {
                        bind_error = Some(error);
                        None
                    }
                }
            });
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

        if let Some(mut reservation) = bound {
            if !record_report.record_written {
                // A sequence was assigned but native did not prove complete
                // initialization. Dropping the reservation pins the ordered
                // slot; treating this as an abort could let later data pass a
                // possibly visible transaction with no replayable record.
                return Err(finish_unwritten_bound(reservation, record_report));
            }
            reservation.attach_native_record(
                record
                    .into_written()
                    .expect("a native completion witness exposes exact initialized bytes"),
            );
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

fn finish_unwritten_bound<B: Blobs>(
    reservation: writeback::BoundReservation<'_, B>,
    record_report: mako_local::CommitRecordReport,
) -> Error {
    assert!(record_report.record_bound);
    assert!(!record_report.record_written);
    let sequence = reservation.sequence();
    // Drop performs the fail-closed Prepared -> pinned transition before the
    // typed result becomes visible to the caller.
    drop(reservation);
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
            reservation(&writeback),
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
            reservation(&malformed),
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
            reservation(&impossible_conflict),
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
            Options::default().durability,
            Durability::Wal,
            "the production cache must not request a per-write disk sync"
        );
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
        if !mako_local::features().unwrap().test_cleanup_failures() {
            return;
        }

        std::thread::spawn(|| {
            use std::sync::Arc;

            use mako_local::{
                arm_test_cleanup_failure, worker_health, TestCleanupBoundary, WorkerHealth,
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
