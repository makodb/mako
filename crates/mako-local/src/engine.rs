//! `mako-engine-api` contract implementation for the C ABI engine.
//!
//! Every method delegates to the inherent method of the same name. The cache
//! calls those inherent methods through the `mako-engine` facade; these impls
//! exist so the contract is checked by the compiler and so the shared
//! conformance suite can drive this engine through the trait.

use std::mem::MaybeUninit;
use std::num::{NonZeroU32, NonZeroU64};
use std::sync::atomic::{AtomicBool, AtomicU64};

use mako_engine_api::{
    CacheOrderMode, CommitHolderReport, CommitRecordChecksum, CommitRecordPreflight,
    CommitRecordReport, CommitRecordTarget, CommitReport, EngineDb, EngineTransaction, Features,
    FusedOnePutHolderAttempt, HolderPool, HolderView, LocalEngine, MakoTimestamp,
    NativeOrderedArenaControl, NativeOrderedHolderControl, NativeOrderedRecordOutcome,
    PreselectedHolderOutcome, PreselectedRecordOutcome, Result, SpscHolderControl,
    TestCleanupBoundary, TestCommitObserver, WorkerHealth,
};

use crate::{
    LocalDb, Scan, Table, Transaction, TrustedNativeOrderedArenaControl,
    TrustedNativeOrderedHolderControl, TrustedNativeOrderedOnePutRecordOutcome,
    TrustedOnePutHolderPool, TrustedOnePutHolderView,
    TrustedPreselectedUncheckedOnePutHolderOutcome, TrustedPreselectedUncheckedOnePutRecordOutcome,
    TrustedSpscOnePutHolderControl,
};

/// The C ABI engine: safe Rust over `mako_local_*` to C++ STO/MassTrans.
#[derive(Debug, Clone, Copy, Default)]
pub struct MakoLocalEngine;

impl LocalEngine for MakoLocalEngine {
    const NAME: &'static str = "mako-local/sto-masstrans";

    type Db = LocalDb;
    type HolderPool = TrustedOnePutHolderPool;
    type SpscHolderControl = TrustedSpscOnePutHolderControl;
    type NativeOrderedArenaControl = TrustedNativeOrderedArenaControl;
    type NativeOrderedHolderControl = TrustedNativeOrderedHolderControl;
    type PreselectedHolderOutcome = TrustedPreselectedUncheckedOnePutHolderOutcome;
    type PreselectedRecordOutcome = TrustedPreselectedUncheckedOnePutRecordOutcome;
    type NativeOrderedRecordOutcome = TrustedNativeOrderedOnePutRecordOutcome;

    fn features() -> Result<Features> {
        crate::features()
    }
    fn advance_mako_timestamp_past(observed: MakoTimestamp) -> Result<()> {
        crate::advance_mako_timestamp_past(observed)
    }
    fn worker_health() -> Result<WorkerHealth> {
        crate::worker_health()
    }
    fn quarantined_worker_count() -> Result<u64> {
        crate::quarantined_worker_count()
    }
    fn install_test_commit_observer(observer: TestCommitObserver) -> Result<()> {
        crate::install_test_commit_observer(observer)
    }
    fn clear_test_commit_observer() -> Result<()> {
        crate::clear_test_commit_observer()
    }
    fn arm_test_cleanup_failure(boundary: TestCleanupBoundary) -> Result<()> {
        crate::arm_test_cleanup_failure(boundary)
    }
    fn clear_test_cleanup_failure() -> Result<()> {
        crate::clear_test_cleanup_failure()
    }
}

impl LocalDb {
    /// Whether the database runtime can still admit transactions.
    ///
    /// The C ABI engine quarantines individual workers and has no shared
    /// runtime poison state. Query worker health separately for admission on
    /// the current thread.
    pub fn runtime_is_healthy(&self) -> bool {
        true
    }
}

impl EngineDb for LocalDb {
    type Engine = MakoLocalEngine;
    type Table<'db>
        = Table<'db>
    where
        Self: 'db;
    type Transaction<'db>
        = Transaction<'db>
    where
        Self: 'db;

    fn open() -> Result<Self> {
        LocalDb::open()
    }
    fn runtime_is_healthy(&self) -> bool {
        LocalDb::runtime_is_healthy(self)
    }
    unsafe fn claim_cache_order_namespace(&self, mode: CacheOrderMode) -> Result<()> {
        // SAFETY: forwarded unchanged.
        unsafe { LocalDb::claim_cache_order_namespace(self, mode) }
    }
    unsafe fn reseed_cache_order_namespace(&self, recovered_sequence: u64) -> Result<()> {
        // SAFETY: forwarded unchanged.
        unsafe { LocalDb::reseed_cache_order_namespace(self, recovered_sequence) }
    }
    fn open_table(&self, name: &[u8], table_id: u64) -> Result<Table<'_>> {
        LocalDb::open_table(self, name, table_id)
    }
    fn bind_trusted_table(&self, name: &[u8], table_id: u64) -> Result<()> {
        LocalDb::bind_trusted_table(self, name, table_id)
    }
    fn transaction(&self) -> Result<Transaction<'_>> {
        LocalDb::transaction(self)
    }
    fn trusted_bound_transaction(&self) -> Result<(Table<'_>, Transaction<'_>)> {
        LocalDb::trusted_bound_transaction(self)
    }
    fn order_record_validation_prefix(&self) {
        LocalDb::order_record_validation_prefix(self)
    }
}

impl<'db> EngineTransaction<'db> for Transaction<'db> {
    type Engine = MakoLocalEngine;
    type Table = Table<'db>;
    type Scan<'txn>
        = Scan<'txn, 'db>
    where
        Self: 'txn;

    fn get(&mut self, table: &Table<'db>, key: &[u8]) -> Result<Option<Vec<u8>>> {
        Transaction::get(self, table, key)
    }
    fn put(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        Transaction::put(self, table, key, value)
    }
    fn insert(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        Transaction::insert(self, table, key, value)
    }
    fn remove(&mut self, table: &Table<'db>, key: &[u8]) -> Result<bool> {
        Transaction::remove(self, table, key)
    }
    fn scan<'txn>(
        &'txn mut self,
        table: &Table<'db>,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>> {
        Transaction::scan(self, table, lower, upper)
    }
    fn rscan<'txn>(
        &'txn mut self,
        table: &Table<'db>,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>> {
        Transaction::rscan(self, table, lower, upper)
    }
    fn abort(self) -> Result<()> {
        Transaction::abort(self)
    }
    fn commit(self) -> Result<()> {
        Transaction::commit(self)
    }
    fn commit_report(self) -> CommitReport {
        Transaction::commit_report(self)
    }
    fn commit_record_preflight_with_checksum(
        &mut self,
        max_record_bytes: usize,
        checksum: CommitRecordChecksum,
    ) -> Result<CommitRecordPreflight> {
        Transaction::commit_record_preflight_with_checksum(self, max_record_bytes, checksum)
    }
    fn unchecked_one_put_record_candidate(&self) -> Option<CommitRecordPreflight> {
        Transaction::unchecked_one_put_record_candidate(self)
    }
    unsafe fn trusted_unchecked_one_put_record_bytes_candidate(&self) -> Option<NonZeroU32> {
        // SAFETY: forwarded unchanged.
        unsafe { Transaction::trusted_unchecked_one_put_record_bytes_candidate(self) }
    }
    unsafe fn commit_report_with_record_target<F>(self, acquire: F) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        // SAFETY: forwarded unchanged.
        unsafe { Transaction::commit_report_with_record_target(self, acquire) }
    }
    unsafe fn commit_report_with_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_report_with_unchecked_one_put_record_target(
                self, candidate, acquire,
            )
        }
    }
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        next_bound: &AtomicU64,
        unhealthy: &AtomicBool,
        acquire: F,
    ) -> TrustedNativeOrderedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight, NonZeroU64) -> Option<CommitRecordTarget>,
    {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_trusted_native_ordered_unchecked_one_put_record_target(
                self, candidate, next_bound, unhealthy, acquire,
            )
        }
    }
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_arena(
        self,
        candidate: CommitRecordPreflight,
        control: &TrustedNativeOrderedArenaControl,
    ) -> TrustedNativeOrderedOnePutRecordOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_trusted_native_ordered_unchecked_one_put_arena(
                self, candidate, control,
            )
        }
    }
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_holder(
        self,
        candidate: CommitRecordPreflight,
        control: &TrustedNativeOrderedHolderControl,
    ) -> TrustedNativeOrderedOnePutRecordOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_trusted_native_ordered_unchecked_one_put_holder(
                self, candidate, control,
            )
        }
    }
    unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
        self,
        candidate: CommitRecordPreflight,
        target: CommitRecordTarget,
    ) -> TrustedPreselectedUncheckedOnePutRecordOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
                self, candidate, target,
            )
        }
    }
    unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
        self,
        exact_record_bytes: NonZeroU32,
        pool: &TrustedOnePutHolderPool,
        sequence: NonZeroU64,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
                self,
                exact_record_bytes,
                pool,
                sequence,
            )
        }
    }
    unsafe fn commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
        self,
        exact_record_bytes: NonZeroU32,
        control: &TrustedSpscOnePutHolderControl,
        sequence: NonZeroU64,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
                self,
                exact_record_bytes,
                control,
                sequence,
            )
        }
    }
    unsafe fn try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
        &mut self,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
        cold_attempt: &mut MaybeUninit<
            FusedOnePutHolderAttempt<TrustedPreselectedUncheckedOnePutHolderOutcome>,
        >,
    ) -> bool {
        // SAFETY: forwarded unchanged.
        unsafe {
            Transaction::try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
                self, control, producer_next, capacity_limit, cold_attempt,
            )
        }
    }
}

impl HolderPool for TrustedOnePutHolderPool {
    type Engine = MakoLocalEngine;
    type View<'pool>
        = TrustedOnePutHolderView<'pool>
    where
        Self: 'pool;

    fn new(capacity: usize, key_reserve_bytes: u32, value_reserve_bytes: u32) -> Result<Self> {
        TrustedOnePutHolderPool::new(capacity, key_reserve_bytes, value_reserve_bytes)
    }
    fn capacity(&self) -> usize {
        TrustedOnePutHolderPool::capacity(self)
    }
    unsafe fn view(&self, expected_sequence: NonZeroU64) -> Result<TrustedOnePutHolderView<'_>> {
        // SAFETY: forwarded unchanged.
        unsafe { TrustedOnePutHolderPool::view(self, expected_sequence) }
    }
    unsafe fn release(&self, view: TrustedOnePutHolderView<'_>) -> Result<()> {
        // SAFETY: forwarded unchanged.
        unsafe { TrustedOnePutHolderPool::release(self, view) }
    }
}

impl HolderView for TrustedOnePutHolderView<'_> {
    fn sequence(&self) -> NonZeroU64 {
        TrustedOnePutHolderView::sequence(self)
    }
    fn table_id(&self) -> u64 {
        TrustedOnePutHolderView::table_id(self)
    }
    fn mako_timestamp(&self) -> MakoTimestamp {
        TrustedOnePutHolderView::mako_timestamp(self)
    }
    fn key(&self) -> &[u8] {
        TrustedOnePutHolderView::key(self)
    }
    fn value(&self) -> &[u8] {
        TrustedOnePutHolderView::value(self)
    }
}

impl SpscHolderControl for TrustedSpscOnePutHolderControl {
    type Engine = MakoLocalEngine;

    unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        acknowledged: *mut u64,
        unhealthy: *const u8,
        capacity: NonZeroU64,
        max_record_bytes: NonZeroU32,
    ) -> Self {
        // SAFETY: forwarded unchanged.
        unsafe {
            TrustedSpscOnePutHolderControl::new(
                pool,
                acknowledged,
                unhealthy,
                capacity,
                max_record_bytes,
            )
        }
    }
    fn max_record_bytes(&self) -> u32 {
        TrustedSpscOnePutHolderControl::max_record_bytes(self)
    }
}

impl NativeOrderedArenaControl for TrustedNativeOrderedArenaControl {
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
    ) -> Self {
        // SAFETY: forwarded unchanged.
        unsafe {
            TrustedNativeOrderedArenaControl::from_raw_parts(
                next_bound,
                unhealthy,
                publication_base,
                arena_base,
                publication_mask,
                publication_shift,
                publication_stride,
                arena_stride,
                arena_block_bytes,
            )
        }
    }
}

impl NativeOrderedHolderControl for TrustedNativeOrderedHolderControl {
    type Engine = MakoLocalEngine;

    unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        unhealthy: *const u8,
        publication_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        max_record_bytes: u32,
    ) -> Self {
        // SAFETY: forwarded unchanged.
        unsafe {
            TrustedNativeOrderedHolderControl::new(
                pool,
                unhealthy,
                publication_base,
                publication_mask,
                publication_shift,
                publication_stride,
                max_record_bytes,
            )
        }
    }
}

impl PreselectedHolderOutcome for TrustedPreselectedUncheckedOnePutHolderOutcome {
    fn committed_timestamp(self) -> Option<MakoTimestamp> {
        Self::committed_timestamp(self)
    }
    fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        Self::accepted_timestamp(self)
    }
    fn holder_sealed(self) -> bool {
        Self::holder_sealed(self)
    }
    fn is_committed(self) -> bool {
        Self::is_committed(self)
    }
    fn into_report(self) -> CommitHolderReport {
        Self::into_report(self)
    }
}

impl PreselectedRecordOutcome for TrustedPreselectedUncheckedOnePutRecordOutcome {
    fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        Self::accepted_timestamp(self)
    }
    fn record_written(self) -> bool {
        Self::record_written(self)
    }
    fn is_committed(self) -> bool {
        Self::is_committed(self)
    }
    fn into_report(self) -> CommitRecordReport {
        Self::into_report(self)
    }
}

impl NativeOrderedRecordOutcome for TrustedNativeOrderedOnePutRecordOutcome {
    fn order_witness_valid(self) -> bool {
        Self::order_witness_valid(self)
    }
    fn accepted_order(self) -> Option<(MakoTimestamp, NonZeroU64)> {
        Self::accepted_order(self)
    }
    fn is_committed(self) -> bool {
        Self::is_committed(self)
    }
    fn native_holder_ready(self) -> bool {
        Self::native_holder_ready(self)
    }
    fn into_report(self) -> CommitRecordReport {
        Self::into_report(self)
    }
}
