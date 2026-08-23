//! Transactional Silo/MassTrans write-back cache over RocksDB.
//!
//! Native C++ Silo is the authoritative live state while this process runs.
//! A write transaction first prepares a complete owned record and claims
//! bounded capacity. After Silo locks and validates it, a native hook binds
//! that storage to an ordered cache sequence and records Mako's logical
//! transaction timestamp immediately before installation. Native success then
//! publishes the record into the volatile queue. Publication is the
//! acknowledgement boundary; one background writer later stores the
//! checksummed record and all of its data mutations in a single atomic RocksDB
//! `WriteBatch`.
//!
//! Consequently, [`Cache::flush`] and [`Cache::close`] are real durability
//! barriers, but an unflushed process crash may lose an acknowledged tail.
//! This first slice is deliberately unbounded in memory and exposes one
//! logical application table. Transactions may contain many keys.
//!
//! ```no_run
//! # fn main() -> Result<(), mako_cache::Error> {
//! let db = mako_cache::Db::open("/tmp/mako-cache", mako_cache::Options::default())?;
//! let mut tx = db.transaction()?;
//! tx.put(b"alice", b"10")?;
//! tx.put(b"bob", b"20")?;
//! tx.commit()?; // visible and queued, not necessarily in Rocks yet
//! db.flush()?;
//! db.close()?;
//! # Ok(())
//! # }
//! ```

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::collections::{BTreeMap, HashSet};
use std::fmt;
use std::path::Path;
use std::sync::{Arc, Mutex, RwLock};

use mako_local::{CommitDisposition, LocalDb};
use mrx_core::{BlobError, Blobs};
use mrx_rocks::RocksBlobs;

mod record;
mod runtime;
mod writeback;

pub use mako_local::{Error as LocalError, MakoTimestamp};
pub use mrx_rocks::Durability;
pub use record::{CommitSeq, RecordError};
pub use writeback::{
    ConfigError as WritebackConfigError, FlushError, ReserveError, ResolveError, WritebackConfig,
};

use record::{classify_backend_key, BackendKey, CommitRecord, Mutation, DEFAULT_TABLE_ID};
use runtime::{Runtime, RuntimeError};
use writeback::Writeback;

const DEFAULT_TABLE_NAME: &[u8] = b"mako-cache/default";

/// Cache-specific behavior independent of the concrete durable backend.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CacheOptions {
    /// Bounded transaction-log and retry settings.
    pub writeback: WritebackConfig,
    /// Reject startup unless the native engine advertises conventional
    /// read-your-writes behavior.
    ///
    /// Point read-your-writes is part of the default cache profile. Set this
    /// to `false` only when deliberately opening against a legacy native build.
    pub require_read_my_writes: bool,
}

impl Default for CacheOptions {
    fn default() -> Self {
        Self {
            writeback: WritebackConfig::default(),
            require_read_my_writes: true,
        }
    }
}

/// Options for the concrete RocksDB-backed [`Db`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Options {
    /// Durability requested from each atomic RocksDB batch.
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
    /// The durable backend failed outside the asynchronous flush path.
    Backend(BlobError),
    /// A durable transaction record was malformed or too large.
    Record(RecordError),
    /// Write-back construction options were invalid.
    WritebackConfig(WritebackConfigError),
    /// A complete pre-commit reservation could not be made.
    Reserve(ReserveError),
    /// A bound durability reservation could not be resolved safely.
    ///
    /// In particular, a transaction that is known committed is retained and
    /// left unacknowledged when an earlier queue slot has an unknown outcome.
    Resolve(ResolveError),
    /// A durability barrier could not cover its acknowledged snapshot.
    Flush(FlushError),
    /// Starting the background writer failed.
    RuntimeStart(std::io::Error),
    /// Stopping or draining the background writer failed.
    Runtime(RuntimeError),
    /// The runtime-handle mutex was poisoned.
    RuntimeLockPoisoned,
    /// Rust could not reserve ownership for a mutation before native staging.
    AllocationFailed,
    /// The selected native feature profile is unavailable.
    MissingReadMyWrites,
    /// The RocksDB contains a key outside this cache's tagged format.
    ForeignBackendKey,
    /// A durable record or data key names a table unsupported by this slice.
    UnsupportedTable(u64),
    /// A durable log record and its materialized RocksDB data disagree.
    DurableStateMismatch,
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
    /// Native commit returned an outcome that cannot safely be called abort.
    ///
    /// The corresponding queue slot is permanently pinned; later commits and
    /// flushes fail rather than risk skipping a possibly visible transaction.
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
            Self::Flush(error) => write!(f, "{error}"),
            Self::RuntimeStart(error) => write!(f, "cannot start write-back worker: {error}"),
            Self::Runtime(error) => write!(f, "{error}"),
            Self::RuntimeLockPoisoned => write!(f, "the cache runtime lock is poisoned"),
            Self::AllocationFailed => write!(f, "could not allocate an owned transaction mutation"),
            Self::MissingReadMyWrites => {
                write!(
                    f,
                    "the native engine does not provide required read-your-writes"
                )
            }
            Self::ForeignBackendKey => write!(
                f,
                "RocksDB contains a key outside the mako-cache tagged format"
            ),
            Self::UnsupportedTable(table) => {
                write!(f, "durable state names unsupported table {table}")
            }
            Self::DurableStateMismatch => {
                write!(f, "durable transaction log and materialized data disagree")
            }
            Self::RecoveryDiverged => write!(f, "native replay diverged from durable history"),
            Self::CommittedButCleanupFailed { sequence, source } => write!(
                f,
                "transaction {} committed and was queued, but native cleanup failed: {source}",
                sequence.get()
            ),
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
            Self::Flush(error) => Some(error),
            Self::RuntimeStart(error) => Some(error),
            Self::Runtime(error) => Some(error),
            Self::CommittedButCleanupFailed { source, .. } => Some(source),
            Self::UnknownCommitOutcome { source, .. } => Some(source),
            Self::AbortCleanupFailed { cleanup, .. } => Some(cleanup),
            Self::RuntimeLockPoisoned
            | Self::AllocationFailed
            | Self::MissingReadMyWrites
            | Self::ForeignBackendKey
            | Self::UnsupportedTable(_)
            | Self::DurableStateMismatch
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

impl From<FlushError> for Error {
    fn from(value: FlushError) -> Self {
        Self::Flush(value)
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
/// failures and inspect the durable ground truth.
///
/// This local milestone supports one recovered durable cache namespace per
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

/// Production cache using RocksDB as its durable backend.
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
    /// permits only one pre-existing durable cache namespace; see [`Cache`].
    pub fn from_backend(backend: B, options: CacheOptions) -> Result<Self, Error> {
        let features = mako_local::features()?;
        if options.require_read_my_writes && !features.read_my_writes() {
            return Err(Error::MissingReadMyWrites);
        }

        let local = LocalDb::open()?;
        let durable_seed = recover(&local, &backend, options.writeback.max_record_bytes)?;
        let writeback = Arc::new(Writeback::new(backend, durable_seed, options.writeback)?);
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
        let native = self.local.transaction()?;
        Ok(Transaction {
            cache: self,
            table,
            native: Some(native),
            journal: Vec::new(),
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

    /// Make every transaction acknowledged before this call durable.
    ///
    /// `Sync`, `Wal`, and `None` define what a successful Rocks batch means;
    /// see [`Durability`].
    pub fn flush(&self) -> Result<u64, Error> {
        Ok(self.writeback.flush()?)
    }

    /// Highest contiguous cache sequence applied in this run.
    pub fn durable_sequence(&self) -> u64 {
        self.writeback.durable_sequence()
    }

    /// Highest successful native commit published to the volatile queue.
    pub fn highest_acknowledged_sequence(&self) -> u64 {
        self.writeback.highest_acknowledged()
    }

    /// Number of bound prepared or ready queue slots.
    pub fn queued_transactions(&self) -> usize {
        self.writeback.queue_len()
    }

    /// Access the durable backend for read-only diagnostics and tests.
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
    /// sequence above the durable watermark may be lost.
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
            None => Ok(self.writeback.durable_sequence()),
        }
    }
}

impl<B: Blobs + 'static> Drop for Cache<B> {
    fn drop(&mut self) {
        if let Err(error) = self.shutdown() {
            eprintln!("mako-cache: acknowledged transactions are NOT durable at shutdown: {error}");
        }
    }
}

/// One single-table cache transaction backed by native Silo.
pub struct Transaction<'db, B: Blobs + 'static> {
    cache: &'db Cache<B>,
    table: mako_local::Table<'db>,
    native: Option<mako_local::Transaction<'db>>,
    journal: Vec<Mutation>,
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

    /// Upsert a key, returning whether it was newly created.
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<bool, Error> {
        check_native_lengths(key, Some(value))?;
        self.journal
            .try_reserve(1)
            .map_err(|_| Error::AllocationFailed)?;
        let mutation = Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: copy_bytes(key)?,
            value: copy_bytes(value)?,
        };
        let table = self.table;
        let created = self.native_mut().put(&table, key, value)?;
        self.journal.push(mutation);
        Ok(created)
    }

    /// Insert a key only when absent, returning whether it was staged.
    pub fn insert(&mut self, key: &[u8], value: &[u8]) -> Result<bool, Error> {
        check_native_lengths(key, Some(value))?;
        self.journal
            .try_reserve(1)
            .map_err(|_| Error::AllocationFailed)?;
        let mutation = Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: copy_bytes(key)?,
            value: copy_bytes(value)?,
        };
        let table = self.table;
        let inserted = self.native_mut().insert(&table, key, value)?;
        if inserted {
            self.journal.push(mutation);
        }
        Ok(inserted)
    }

    /// Remove a key, returning whether a live value existed.
    pub fn remove(&mut self, key: &[u8]) -> Result<bool, Error> {
        check_native_lengths(key, None)?;
        self.journal
            .try_reserve(1)
            .map_err(|_| Error::AllocationFailed)?;
        let mutation = Mutation::Delete {
            table_id: DEFAULT_TABLE_ID,
            key: copy_bytes(key)?,
        };
        let table = self.table;
        let existed = self.native_mut().remove(&table, key)?;
        if existed {
            self.journal.push(mutation);
        }
        Ok(existed)
    }

    /// Validate/install in Silo, then publish the preallocated durability
    /// record before returning success.
    pub fn commit(mut self) -> Result<(), Error> {
        let native = self
            .native
            .take()
            .expect("cache transaction already consumed");

        if self.journal.is_empty() {
            // Writers hold this fence shared from native commit through queue
            // resolution. Taking it exclusively prevents a read-only result
            // from being acknowledged while a possibly observed writer is in
            // the post-install, pre-resolution window.
            let _fence = self
                .cache
                .commit_fence
                .write()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            self.cache.writeback.ensure_no_unknown()?;
            return finish_read_only(native);
        }

        // Preparation may allocate or wait for bounded capacity, so it must
        // finish before native Silo takes write locks.
        let mut permit = self
            .cache
            .writeback
            .reserve(std::mem::take(&mut self.journal))?;

        // Writers share this fence and therefore still lock, validate, bind,
        // and install concurrently. It only excludes read-only acknowledgement
        // while a writer's native outcome has not yet resolved its queue slot.
        let _fence = self
            .cache
            .commit_fence
            .read()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        self.cache.writeback.ensure_no_unknown()?;

        let mut bound = None;
        let mut bind_error = None;
        let report = native.commit_report_with_hook(|timestamp| match permit.bind(timestamp) {
            Ok(reservation) => {
                bound = Some(reservation);
                true
            }
            Err(error) => {
                bind_error = Some(error);
                false
            }
        });

        if let Some(reservation) = bound {
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

        // A nonempty journal implies a native write set. If no hook ran, Silo
        // never entered its install phase, so every failure is a definite
        // visibility abort even when the generic wrapper labels the status
        // Unknown. A committed result without a hook would be a native contract
        // violation and, critically, an uncovered visible write.
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

    /// Explicitly abort without creating a durability obligation.
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

fn check_native_lengths(key: &[u8], value: Option<&[u8]>) -> Result<(), Error> {
    if key.len() > mako_local::MAX_KEY_BYTES
        || value.is_some_and(|value| value.len() > mako_local::MAX_VALUE_BYTES)
    {
        return Err(Error::Native(LocalError::ValueTooLarge));
    }
    Ok(())
}

fn copy_bytes(bytes: &[u8]) -> Result<Vec<u8>, Error> {
    let mut owned = Vec::new();
    owned
        .try_reserve_exact(bytes.len())
        .map_err(|_| Error::AllocationFailed)?;
    owned.extend_from_slice(bytes);
    Ok(owned)
}

fn recover<B: Blobs>(local: &LocalDb, backend: &B, max_bytes: usize) -> Result<u64, Error> {
    let mut keys = Vec::<Vec<u8>>::new();
    backend.for_each_key(&mut |key| keys.push(key.to_vec()))?;

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

    let mut records = Vec::new();
    records
        .try_reserve_exact(log_keys.len())
        .map_err(|_| Error::AllocationFailed)?;
    let mut timestamps = HashSet::new();
    timestamps
        .try_reserve(log_keys.len())
        .map_err(|_| Error::AllocationFailed)?;
    let mut previous = 0u64;
    for (sequence, key) in log_keys {
        let expected = previous.checked_add(1).ok_or(Error::DurableStateMismatch)?;
        if sequence.get() != expected {
            return Err(Error::DurableStateMismatch);
        }
        let value = backend.get(&key)?.ok_or(Error::DurableStateMismatch)?;
        let record = CommitRecord::decode(&key, &value, max_bytes)?;
        if !timestamps.insert(record.mako_timestamp()) {
            return Err(Error::DurableStateMismatch);
        }
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
    }

    validate_materialized_data(backend, &records, data_keys)?;

    // Mako's logical counter is process-local. Raise it above every durable
    // timestamp before replay completes and the recovered cache is exposed,
    // otherwise a process restart could reuse transaction timestamps.
    if let Some(timestamp) = records.iter().map(CommitRecord::mako_timestamp).max() {
        mako_local::advance_mako_timestamp_past(timestamp)?;
    }
    replay_records(local, &records)?;
    Ok(previous)
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
            .ok_or(Error::DurableStateMismatch)?;
        let actual = backend
            .get(&backend_key)?
            .ok_or(Error::DurableStateMismatch)?;
        if actual != expected {
            return Err(Error::DurableStateMismatch);
        }
    }
    if final_state.values().any(Option::is_some) {
        return Err(Error::DurableStateMismatch);
    }
    Ok(())
}

fn replay_records(local: &LocalDb, records: &[CommitRecord]) -> Result<(), Error> {
    let table = local.open_table(DEFAULT_TABLE_NAME, DEFAULT_TABLE_ID)?;
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
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(have_mako)]
    fn test_record(sequence: u64, timestamp: u32, key: &[u8]) -> CommitRecord {
        crate::record::PreparedCommitRecord::prepare(
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: key.to_vec(),
                value: b"value".to_vec(),
            }],
            CacheOptions::default().writeback.max_record_bytes,
        )
        .unwrap()
        .bind(
            CommitSeq::new(sequence).expect("test sequence is nonzero"),
            MakoTimestamp::new(timestamp).expect("test timestamp is nonzero"),
        )
        .finalize()
    }

    #[test]
    fn default_profile_requires_read_your_writes() {
        assert!(CacheOptions::default().require_read_my_writes);
    }

    #[test]
    fn oversized_inputs_are_rejected_before_native_staging() {
        let key = vec![0; mako_local::MAX_KEY_BYTES + 1];
        assert!(matches!(
            check_native_lengths(&key, None),
            Err(Error::Native(LocalError::ValueTooLarge))
        ));
        let value = vec![0; mako_local::MAX_VALUE_BYTES + 1];
        assert!(matches!(
            check_native_lengths(b"key", Some(&value)),
            Err(Error::Native(LocalError::ValueTooLarge))
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn native_mako_timestamp_is_carried_into_the_durable_record() {
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
            .expect("one durable transaction record");
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
            Err(Error::DurableStateMismatch)
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_rejects_duplicate_mako_timestamps() {
        use std::sync::Arc;

        use mrx_core::fakes::MemBlobs;

        let backend = Arc::new(MemBlobs::new());
        let first = test_record(1, 202, b"first");
        let second = test_record(2, 202, b"second");
        backend.write_batch(&first.backend_ops()).unwrap();
        backend.write_batch(&second.backend_ops()).unwrap();

        assert!(matches!(
            Cache::from_backend(backend, CacheOptions::default()),
            Err(Error::DurableStateMismatch)
        ));
    }

    #[cfg(have_mako)]
    #[test]
    fn recovery_advances_mako_timestamp_past_the_durable_maximum() {
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
    fn recovery_rejects_an_exhausted_durable_mako_timestamp() {
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
            Err(Error::Flush(FlushError::UnknownOutcome { sequence }))
                if sequence.get() == 1
        ));
        assert!(matches!(
            in_flight.commit(),
            Err(Error::Flush(FlushError::UnknownOutcome { sequence }))
                if sequence.get() == 1
        ));

        assert!(matches!(
            cache.close(),
            Err(Error::Runtime(RuntimeError::Flush(
                FlushError::UnknownOutcome { sequence }
            ))) if sequence.get() == 1
        ));
    }
}
