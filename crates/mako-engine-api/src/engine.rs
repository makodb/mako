//! The local engine contract.
//!
//! These traits describe exactly the operations `mako-cache` performs against
//! a local transaction engine. They are deliberately shaped like the operations
//! the cache already needed from the C ABI facade, including its build-private
//! "trusted" terminals, so that the C ABI engine implements them by delegation
//! and a native Rust engine implements them directly. The cache itself binds to
//! one engine at compile time through the `mako-engine` facade; these traits
//! exist so every engine is held to one contract and one conformance suite.
//!
//! Every `unsafe fn` carries the same contract as the facade method it mirrors;
//! see the engine's own documentation for the precise obligations.

use std::mem::MaybeUninit;
use std::num::{NonZeroU32, NonZeroU64};

use crate::commit::{
    CommitHolderReport, CommitRecordChecksum, CommitRecordPreflight, CommitRecordReport,
    CommitRecordTarget, CommitReport, FusedOnePutHolderAttempt,
};
use crate::error::Result;
use crate::features::Features;
use crate::modes::CacheOrderMode;
use crate::test_hooks::{TestCleanupBoundary, TestCommitObserver, WorkerHealth};
use crate::timestamp::MakoTimestamp;

/// One local transaction engine and its process-wide services.
pub trait LocalEngine: 'static + Sized {
    /// Stable identifier reported by the engine, for logs and identity checks.
    const NAME: &'static str;

    /// Database facade type.
    type Db: EngineDb<Engine = Self>;
    /// Zero-copy one-put holder ring used by the write-back publication path.
    type HolderPool: HolderPool<Engine = Self>;
    /// Stable control block for the fused SPSC one-put terminal.
    type SpscHolderControl: SpscHolderControl<Engine = Self>;
    /// Stable control block for the concurrent byte-arena terminal.
    type NativeOrderedArenaControl: NativeOrderedArenaControl;
    /// Stable control block for the concurrent holder terminal.
    type NativeOrderedHolderControl: NativeOrderedHolderControl<Engine = Self>;
    /// Compact result of a preselected one-put holder terminal.
    type PreselectedHolderOutcome: PreselectedHolderOutcome;
    /// Compact result of a preselected one-put byte-record terminal.
    type PreselectedRecordOutcome: PreselectedRecordOutcome;
    /// Compact result of a concurrent engine-ordered one-put terminal.
    type NativeOrderedRecordOutcome: NativeOrderedRecordOutcome;

    /// Capabilities of the engine, after verifying its identity.
    fn features() -> Result<Features>;
    /// Raise the process-wide Mako timestamp floor above `observed`.
    fn advance_mako_timestamp_past(observed: MakoTimestamp) -> Result<()>;
    /// Health of the current OS worker.
    fn worker_health() -> Result<WorkerHealth>;
    /// Number of workers retired by uncertain cleanup in this process.
    fn quarantined_worker_count() -> Result<u64>;
    /// Install the test-only synchronous commit observer for this worker.
    fn install_test_commit_observer(observer: TestCommitObserver) -> Result<()>;
    /// Remove the test-only commit observer.
    fn clear_test_commit_observer() -> Result<()>;
    /// Arm a one-shot test-only cleanup failure on the current worker.
    fn arm_test_cleanup_failure(boundary: TestCleanupBoundary) -> Result<()>;
    /// Disarm a pending test-only cleanup failure.
    fn clear_test_cleanup_failure() -> Result<()>;
}

/// One local database facade.
pub trait EngineDb: Send + Sync + Sized {
    /// Engine this database belongs to.
    type Engine: LocalEngine<Db = Self>;
    /// Borrowed table handle.
    type Table<'db>: Copy + Send + Sync
    where
        Self: 'db;
    /// Thread-affine transaction handle.
    type Transaction<'db>: EngineTransaction<'db, Engine = Self::Engine, Table = Self::Table<'db>>
    where
        Self: 'db;

    /// Open a facade and attach the calling worker.
    fn open() -> Result<Self>;
    /// Whether this database's runtime can still admit transactions.
    ///
    /// This diagnostic snapshot does not attach or quarantine the caller.
    /// Worker-local quarantine is reported separately by `worker_health`.
    fn runtime_is_healthy(&self) -> bool;
    /// Claim the process-wide cache-order namespace in one immutable mode.
    ///
    /// # Safety
    /// The caller must exclusively own the fresh facade and must call this
    /// before any transaction or recovery work.
    unsafe fn claim_cache_order_namespace(&self, mode: CacheOrderMode) -> Result<()>;
    /// Reseed the claimed namespace's dense sequence after recovery.
    ///
    /// # Safety
    /// No foreground transaction or ordering terminal may exist.
    unsafe fn reseed_cache_order_namespace(&self, recovered_sequence: u64) -> Result<()>;
    /// Open or create a named table with a stable logical id.
    fn open_table(&self, name: &[u8], table_id: u64) -> Result<Self::Table<'_>>;
    /// Bind the one table used by the trusted cache transaction path.
    fn bind_trusted_table(&self, name: &[u8], table_id: u64) -> Result<()>;
    /// Begin an ordinary checked transaction.
    fn transaction(&self) -> Result<Self::Transaction<'_>>;
    /// Begin a trusted transaction bound to the table from `bind_trusted_table`.
    fn trusted_bound_transaction(&self) -> Result<(Self::Table<'_>, Self::Transaction<'_>)>;
    /// Mark one cut in the cache-order modification order for a read-only commit.
    fn order_record_validation_prefix(&self);
}

/// One thread-affine engine transaction.
pub trait EngineTransaction<'db>: Sized {
    /// Engine this transaction belongs to.
    type Engine: LocalEngine;
    /// Table handle type accepted by point operations.
    type Table: Copy;
    /// Owned-row range iterator.
    type Scan<'txn>: Iterator<Item = Result<(Vec<u8>, Vec<u8>)>>
    where
        Self: 'txn;

    /// Read a key, distinguishing missing from present-empty.
    fn get(&mut self, table: &Self::Table, key: &[u8]) -> Result<Option<Vec<u8>>>;
    /// Upsert a key, returning whether it was newly created.
    fn put(&mut self, table: &Self::Table, key: &[u8], value: &[u8]) -> Result<bool>;
    /// Insert only when absent, returning whether it was staged.
    fn insert(&mut self, table: &Self::Table, key: &[u8], value: &[u8]) -> Result<bool>;
    /// Remove a key, returning whether a live value existed.
    fn remove(&mut self, table: &Self::Table, key: &[u8]) -> Result<bool>;
    /// Scan `[lower, upper)` ascending; `None` is unbounded above.
    fn scan<'txn>(
        &'txn mut self,
        table: &Self::Table,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Self::Scan<'txn>>;
    /// Scan `[lower, upper)` descending.
    fn rscan<'txn>(
        &'txn mut self,
        table: &Self::Table,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Self::Scan<'txn>>;
    /// Abort and consume the transaction.
    fn abort(self) -> Result<()>;
    /// Commit without a record; conflicts are ordinary errors.
    fn commit(self) -> Result<()>;
    /// Commit keeping visibility and cleanup as separate outcomes.
    fn commit_report(self) -> CommitReport;
    /// Seal and size the canonical record plan in an explicit integrity mode.
    fn commit_record_preflight_with_checksum(
        &mut self,
        max_record_bytes: usize,
        checksum: CommitRecordChecksum,
    ) -> Result<CommitRecordPreflight>;
    /// Plan for a transaction whose only mutation is one direct put, if the
    /// engine still holds that witness.
    fn unchecked_one_put_record_candidate(&self) -> Option<CommitRecordPreflight>;
    /// Compact extent of the one-put candidate.
    ///
    /// # Safety
    /// The caller owns an active trusted transaction and performs no other
    /// operation before the consuming terminal.
    unsafe fn trusted_unchecked_one_put_record_bytes_candidate(&self) -> Option<NonZeroU32>;
    /// General record terminal: validate, allocate order, bind storage through
    /// `acquire`, serialize, install.
    ///
    /// # Safety
    /// Any storage returned by `acquire` must stay valid through this call.
    unsafe fn commit_report_with_record_target<F>(self, acquire: F) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>;
    /// One-put variant of [`Self::commit_report_with_record_target`].
    ///
    /// # Safety
    /// As above; `candidate` must be the current one-put plan.
    unsafe fn commit_report_with_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>;
    /// Concurrent engine-ordered one-put terminal with a bind callback.
    ///
    /// # Safety
    /// See the engine's terminal contract; storage from `acquire` must stay
    /// valid through the call.
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        next_bound: &std::sync::atomic::AtomicU64,
        unhealthy: &std::sync::atomic::AtomicBool,
        acquire: F,
    ) -> <Self::Engine as LocalEngine>::NativeOrderedRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight, NonZeroU64) -> Option<CommitRecordTarget>;
    /// Concurrent engine-ordered one-put terminal writing into the byte arena.
    ///
    /// # Safety
    /// `control` must describe live, stable publication and arena storage.
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_arena(
        self,
        candidate: CommitRecordPreflight,
        control: &<Self::Engine as LocalEngine>::NativeOrderedArenaControl,
    ) -> <Self::Engine as LocalEngine>::NativeOrderedRecordOutcome;
    /// Concurrent engine-ordered one-put terminal sealing a holder.
    ///
    /// # Safety
    /// `control` must describe live, stable publication storage and pool.
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_holder(
        self,
        candidate: CommitRecordPreflight,
        control: &<Self::Engine as LocalEngine>::NativeOrderedHolderControl,
    ) -> <Self::Engine as LocalEngine>::NativeOrderedRecordOutcome;
    /// Single-producer preselected one-put terminal writing the record bytes.
    ///
    /// # Safety
    /// `target` must be the exact preselected storage and the caller must
    /// hold whole-call exclusion over the cache-order namespace.
    unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
        self,
        candidate: CommitRecordPreflight,
        target: CommitRecordTarget,
    ) -> <Self::Engine as LocalEngine>::PreselectedRecordOutcome;
    /// Single-producer preselected one-put terminal sealing a holder.
    ///
    /// # Safety
    /// The producer lease must own `sequence` in `pool` through the call.
    unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
        self,
        exact_record_bytes: NonZeroU32,
        pool: &<Self::Engine as LocalEngine>::HolderPool,
        sequence: NonZeroU64,
    ) -> <Self::Engine as LocalEngine>::PreselectedHolderOutcome;
    /// Per-worker preselected one-put terminal sealing a holder in one lane.
    ///
    /// # Safety
    /// The lane must own `sequence` and `control` must outlive the call.
    unsafe fn commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
        self,
        exact_record_bytes: NonZeroU32,
        control: &<Self::Engine as LocalEngine>::SpscHolderControl,
        sequence: NonZeroU64,
    ) -> <Self::Engine as LocalEngine>::PreselectedHolderOutcome;
    /// Fused SPSC one-put terminal that publishes ACK itself on success.
    ///
    /// Returns `true` when the engine consumed the transaction and published;
    /// the caller must then forget its facade. Otherwise `cold_attempt` is
    /// initialized and owns the transaction and queue progress.
    ///
    /// # Safety
    /// The caller is the unique producer of `control`'s lane and every pointer
    /// stays live and stable through the call.
    unsafe fn try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
        &mut self,
        control: &<Self::Engine as LocalEngine>::SpscHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
        cold_attempt: &mut MaybeUninit<
            FusedOnePutHolderAttempt<<Self::Engine as LocalEngine>::PreselectedHolderOutcome>,
        >,
    ) -> bool;
}

/// Zero-copy holder ring shared between the foreground commit and the
/// background write-back consumer.
pub trait HolderPool: Send + Sync + Sized {
    /// Engine this pool belongs to.
    type Engine: LocalEngine<HolderPool = Self>;
    /// Exact-generation borrowed view of one sealed holder.
    type View<'pool>: HolderView
    where
        Self: 'pool;

    /// Allocate a pool for one publication ring of `capacity` generations.
    fn new(capacity: usize, key_reserve_bytes: u32, value_reserve_bytes: u32) -> Result<Self>;
    /// Number of generations addressable before a ring position repeats.
    fn capacity(&self) -> usize;
    /// Borrow one sealed generation after queue publication.
    ///
    /// # Safety
    /// Sole-consumer ownership of `expected_sequence` after an Acquire of its
    /// publication; no overlapping view or release.
    unsafe fn view(&self, expected_sequence: NonZeroU64) -> Result<Self::View<'_>>;
    /// Release a generation after backend retirement.
    ///
    /// # Safety
    /// Every backend use of the view's bytes must be complete.
    unsafe fn release(&self, view: Self::View<'_>) -> Result<()>;
}

/// Borrowed sealed one-put holder contents.
pub trait HolderView {
    /// Lane-local sequence of this generation.
    fn sequence(&self) -> NonZeroU64;
    /// Stable logical table id.
    fn table_id(&self) -> u64;
    /// Accepted Mako timestamp.
    fn mako_timestamp(&self) -> MakoTimestamp;
    /// Raw application key.
    fn key(&self) -> &[u8];
    /// Raw application value.
    fn value(&self) -> &[u8];
}

/// Stable control block for the fused SPSC one-put terminal.
pub trait SpscHolderControl: Send + Sync + Sized {
    /// Engine this control belongs to.
    type Engine: LocalEngine<SpscHolderControl = Self>;

    /// Snapshot stable lane-wide inputs.
    ///
    /// # Safety
    /// `pool`, `acknowledged`, and `unhealthy` must stay live and stable until
    /// the last terminal call using this value returns.
    unsafe fn new(
        pool: &<Self::Engine as LocalEngine>::HolderPool,
        acknowledged: *mut u64,
        unhealthy: *const u8,
        capacity: NonZeroU64,
        max_record_bytes: NonZeroU32,
    ) -> Self;
    /// Maximum record extent supplied to the engine.
    fn max_record_bytes(&self) -> u32;
}

/// Stable control block for the concurrent byte-arena terminal.
pub trait NativeOrderedArenaControl: Sized {
    /// Describe the publication and arena layout.
    ///
    /// # Safety
    /// Every pointer must stay live and stable through each terminal call.
    #[allow(clippy::too_many_arguments)]
    unsafe fn from_raw_parts(
        next_bound: *mut u64,
        unhealthy: *const u8,
        publication_base: *mut u8,
        arena_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        arena_stride: u32,
        arena_block_bytes: u32,
    ) -> Self;
}

/// Stable control block for the concurrent holder terminal.
pub trait NativeOrderedHolderControl: Sized {
    /// Engine this control belongs to.
    type Engine: LocalEngine<NativeOrderedHolderControl = Self>;

    /// Describe the publication layout and holder pool.
    ///
    /// # Safety
    /// Every pointer and `pool` must stay live and stable through each call.
    unsafe fn new(
        pool: &<Self::Engine as LocalEngine>::HolderPool,
        unhealthy: *const u8,
        publication_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        max_record_bytes: u32,
    ) -> Self;
}

/// Compact result of a preselected one-put holder terminal.
pub trait PreselectedHolderOutcome: Copy + std::fmt::Debug {
    /// Timestamp when the engine committed and sealed the holder.
    fn committed_timestamp(self) -> Option<MakoTimestamp>;
    /// Timestamp the engine accepted, committed or not.
    fn accepted_timestamp(self) -> Option<MakoTimestamp>;
    /// Whether the payload was sealed into the holder.
    fn holder_sealed(self) -> bool;
    /// Whether this is the ordinary committed outcome.
    fn is_committed(self) -> bool;
    /// Full fail-closed decode.
    fn into_report(self) -> CommitHolderReport;
}

/// Compact result of a preselected one-put byte-record terminal.
pub trait PreselectedRecordOutcome: Copy + std::fmt::Debug {
    /// Timestamp the engine accepted, committed or not.
    fn accepted_timestamp(self) -> Option<MakoTimestamp>;
    /// Whether the record bytes were completely written.
    fn record_written(self) -> bool;
    /// Whether this is the ordinary committed outcome.
    fn is_committed(self) -> bool;
    /// Full fail-closed decode.
    fn into_report(self) -> CommitRecordReport;
}

/// Compact result of a concurrent engine-ordered one-put terminal.
pub trait NativeOrderedRecordOutcome: Copy + std::fmt::Debug {
    /// Whether the `(timestamp, sequence)` witness pair is internally consistent.
    fn order_witness_valid(self) -> bool;
    /// The accepted order, when the engine accepted one.
    fn accepted_order(self) -> Option<(MakoTimestamp, NonZeroU64)>;
    /// Whether this is the ordinary committed outcome.
    fn is_committed(self) -> bool;
    /// Whether the engine published the holder READY itself.
    fn native_holder_ready(self) -> bool;
    /// Full fail-closed decode.
    fn into_report(self) -> CommitRecordReport;
}
