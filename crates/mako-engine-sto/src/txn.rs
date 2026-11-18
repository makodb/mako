//! Transactions: point operations, scans, the canonical write set, and every
//! commit terminal the cache uses.
//!
//! The wrapper records each operation it issues and the presence it observed,
//! then derives the canonical record from that log at commit exactly as the
//! C++ `visit_local_canonical_writes` walk does: keys in first-touch order,
//! the final effect per key, insert-then-remove elided, delete-then-put as an
//! update. `sto-core` itself never sees record bytes.

use std::hint::spin_loop;
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::num::{NonZeroU32, NonZeroU64};
use std::ptr;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering};

use mako_clock::{Clock, OrderPairAllocation, StampAllocation};
use mako_engine_api::record_format::UNCHECKED_ONE_PUT_OVERHEAD_BYTES;
use mako_engine_api::{
    CommitDisposition, CommitRecordChecksum, CommitRecordPreflight, CommitRecordReport,
    CommitRecordTarget, CommitReport, Error, FusedOnePutHolderAttempt, MakoTimestamp, Result,
    TestCleanupBoundary, TestCommitObserver, TestCommitPhase, MAX_KEY_BYTES, MAX_VALUE_BYTES,
    TRANSACTION_ITEM_BUDGET,
};
use sto_core::{
    AbortReason, AccessError, Active, BeginError, CapacityError, CommitFailure, CommitHook,
    CommitHookError, CommitOutcome, DefiniteOutcome, InvalidUse,
};
use sto_masstree::{
    InsertOutcome, ScanBound, ScanControl, ScanDirection, ScanRequest, ScanScratch,
};

use crate::db::{LocalDb, Table, TableEntry};
use crate::holder::{
    Holder, TrustedNativeOrderedArenaControl, TrustedNativeOrderedHolderControl,
    TrustedOnePutHolderPool, TrustedSpscOnePutHolderControl,
};
use crate::outcome::{
    TrustedNativeOrderedOnePutRecordOutcome, TrustedPreselectedUncheckedOnePutHolderOutcome,
    TrustedPreselectedUncheckedOnePutRecordOutcome, TrustedUncheckedOnePutRecordOutcome,
};
use crate::record::{self, OpKind, OpRef};
use crate::touched::{Staged, TouchLog};
use crate::worker::{self, Attachment};
use crate::TrustedFusedOnePutHolderAttempt;

type StoTransaction = sto_core::Transaction<'static, Active>;

#[path = "holder_commit.rs"]
mod holder_commit;

#[cfg(all(test, feature = "test-hooks", not(miri)))]
#[path = "txn_poison_tests.rs"]
mod poison_tests;

#[cfg(all(test, feature = "test-hooks", not(miri)))]
#[path = "txn_pool_tests.rs"]
mod pool_tests;

#[cfg(all(test, feature = "test-hooks", not(miri)))]
#[path = "txn_borrow_tests.rs"]
mod borrow_tests;

#[cfg(all(test, feature = "test-hooks", not(miri)))]
#[path = "txn_local_version_tests.rs"]
mod local_version_tests;

#[cfg(test)]
#[path = "txn_ownership_tests.rs"]
mod ownership_tests;

const SCAN_CHUNK_ENTRIES: usize = 64;
const MINIMUM_WRITE_ITEM_CHARGE: usize = 4;
const MASSTREE_SLICE_BYTES: usize = 8;
/// Largest one-put record the compact `u32` witness can carry.
const FAST_PUT_RECORD_BYTES_MAX: usize = (u64::MAX >> 33) as usize;

// Same-build layout of the cache's publication cells and arena blocks.
const PUBLICATION_CELL_BYTES: u32 = 64;
const PUBLICATION_TIMESTAMP_OFFSET: usize = 8;
const PUBLICATION_RECORD_BYTES_OFFSET: usize = 16;
const ARENA_BLOCK_STRIDE: u32 = 256;
const NATIVE_HOLDER_RECORD_TAG: usize = 1 << (usize::BITS - 1);
const TURN_PHASE_BITS: u32 = 2;
const TURN_FREE: u64 = 0;
const TURN_BOUND: u64 = 1;
const TURN_READY: u64 = 3;

#[inline(always)]
fn write_item_charge(key_len: usize) -> usize {
    MINIMUM_WRITE_ITEM_CHARGE + key_len.div_ceil(MASSTREE_SLICE_BYTES)
}

#[inline(always)]
fn turn_token(sequence: u64, ring_shift: u32, phase: u64) -> u64 {
    debug_assert!(ring_shift >= TURN_PHASE_BITS);
    ((sequence >> ring_shift) << TURN_PHASE_BITS) | phase
}

#[derive(Clone, Copy)]
struct OnePut {
    touched: usize,
    exact_bytes: u32,
}

/// One local optimistic transaction over the native Rust STO.
///
/// The type is neither `Send` nor `Sync`: it borrows the thread's worker.
/// Dropping an active transaction aborts it.
pub struct Transaction<'db> {
    db: &'db LocalDb,
    attachment: Attachment,
    inner: Option<StoTransaction>,
    active: bool,
    trusted: Option<&'db TableEntry>,
    touched: TouchLog,
    mutations: u32,
    has_predicate: bool,
    fast_eligible: bool,
    one_put: Option<OnePut>,
    candidate_bytes: Option<NonZeroU32>,
    plan: Option<CommitRecordPreflight>,
    budget_used: usize,
    _thread_affine: PhantomData<Rc<()>>,
}

impl std::fmt::Debug for Transaction<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Transaction")
            .field("active", &self.active)
            .field("touched", &self.touched.len())
            .field("plan", &self.plan)
            .finish_non_exhaustive()
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Effect {
    Active,
    Finished,
    Quarantined,
}

fn map_access(error: AccessError) -> (Error, Effect) {
    match error {
        AccessError::Conflict(_) => (Error::Conflict, Effect::Finished),
        AccessError::Capacity(CapacityError::VersionExhausted) => {
            (Error::VersionExhausted, Effect::Quarantined)
        }
        AccessError::Capacity(
            CapacityError::ItemLimit
            | CapacityError::LockLimit
            | CapacityError::KeyLimit
            | CapacityError::BufferLimit,
        ) => (Error::TransactionTooLarge, Effect::Finished),
        AccessError::Capacity(_) => (Error::OutOfMemory, Effect::Finished),
        AccessError::InvalidUse(
            InvalidUse::TransactionDoomed | InvalidUse::TransactionFinished,
        ) => (Error::TransactionFinished, Effect::Finished),
        AccessError::InvalidUse(_) => (Error::InvalidArgument, Effect::Active),
        AccessError::Unsupported(_) => (Error::FeatureUnavailable, Effect::Active),
        AccessError::Fault(_) | AccessError::Internal(_) => (Error::Internal, Effect::Finished),
        AccessError::Poisoned(_) => (Error::WorkerPoisoned, Effect::Quarantined),
    }
}

fn map_begin(error: BeginError) -> Error {
    match error {
        BeginError::Capacity(CapacityError::VersionExhausted) => Error::VersionExhausted,
        BeginError::Capacity(_) => Error::OutOfMemory,
        BeginError::InvalidUse(InvalidUse::WorkerBusy) => Error::TransactionAlreadyActive,
        BeginError::InvalidUse(_) => Error::Internal,
        BeginError::Unsupported(_) => Error::FeatureUnavailable,
        BeginError::Poisoned(_) => {
            worker::poison_current_worker();
            Error::WorkerPoisoned
        }
    }
}

fn map_abort(reason: AbortReason, hook_failure: Option<Error>) -> Error {
    match reason {
        AbortReason::HookRejected => hook_failure.unwrap_or(Error::CommitHookRejected),
        AbortReason::Conflict(_) => Error::Conflict,
        AbortReason::Capacity(CapacityError::VersionExhausted) => Error::VersionExhausted,
        AbortReason::Capacity(_) => Error::TransactionTooLarge,
        AbortReason::Explicit
        | AbortReason::Doomed
        | AbortReason::InvalidUse(_)
        | AbortReason::Unsupported(_)
        | AbortReason::Internal(_) => Error::Internal,
    }
}

/// Map a `sto-core` commit result to the two-part report, quarantining the
/// worker when cleanup could not be proved complete.
fn map_commit(
    outcome: std::result::Result<CommitOutcome, CommitFailure>,
    hook_failure: Option<Error>,
) -> CommitReport {
    match outcome {
        Ok(CommitOutcome::Committed(_)) => CommitReport {
            disposition: CommitDisposition::Committed,
            cleanup: Ok(()),
        },
        Ok(CommitOutcome::Aborted(reason)) => CommitReport {
            disposition: CommitDisposition::Aborted(map_abort(reason, hook_failure)),
            cleanup: Ok(()),
        },
        Err(CommitFailure::Poisoned { outcome, .. }) => {
            worker::poison_current_worker();
            CommitReport {
                disposition: match outcome {
                    DefiniteOutcome::Committed(_) => CommitDisposition::Committed,
                    DefiniteOutcome::Aborted(reason) => {
                        CommitDisposition::Aborted(map_abort(reason, hook_failure))
                    }
                },
                cleanup: Err(Error::WorkerPoisoned),
            }
        }
        Err(CommitFailure::Indeterminate(_)) => {
            worker::poison_current_worker();
            CommitReport {
                disposition: CommitDisposition::Unknown(Error::WorkerPoisoned),
                cleanup: Err(Error::WorkerPoisoned),
            }
        }
    }
}

const QUARANTINED_REPORT: CommitReport = CommitReport {
    disposition: CommitDisposition::Unknown(Error::WorkerPoisoned),
    cleanup: Err(Error::WorkerPoisoned),
};

#[inline(always)]
fn definite(error: Error) -> CommitReport {
    CommitReport {
        disposition: CommitDisposition::Aborted(error),
        cleanup: Ok(()),
    }
}

impl<'db> Transaction<'db> {
    pub(crate) fn begin(
        db: &'db LocalDb,
        attachment: Attachment,
        trusted: Option<&'db TableEntry>,
    ) -> Result<Self> {
        if worker::current_worker_poisoned() {
            return Err(Error::WorkerPoisoned);
        }
        if worker::take_cleanup_failure(TestCleanupBoundary::Begin) {
            worker::poison_current_worker();
            return Err(Error::Internal);
        }
        // SAFETY: the slot belongs to this thread; `release` is called from
        // every path that consumes the transaction.
        let context = unsafe { attachment.slot.acquire()? };
        let inner = match context.begin() {
            Ok(inner) => inner,
            Err(error) => {
                attachment.slot.release();
                return Err(map_begin(error));
            }
        };
        Ok(Self {
            db,
            attachment,
            inner: Some(inner),
            active: true,
            trusted,
            touched: attachment.slot.take_touched(),
            mutations: 0,
            has_predicate: false,
            fast_eligible: true,
            one_put: None,
            candidate_bytes: None,
            plan: None,
            budget_used: 0,
            _thread_affine: PhantomData,
        })
    }

    // ---- bookkeeping -----------------------------------------------------

    #[inline]
    fn check_active(&self) -> Result<()> {
        if !self.active {
            return Err(Error::TransactionFinished);
        }
        if self.plan.is_some() {
            return Err(Error::Busy);
        }
        Ok(())
    }

    #[inline]
    fn check_table(&self, table: &Table<'db>) -> Result<()> {
        if table.entry.runtime != self.db.runtime_id() {
            return Err(Error::WrongDatabaseOrTable);
        }
        Ok(())
    }

    #[inline]
    fn inner_mut(&mut self) -> &mut StoTransaction {
        self.inner
            .as_mut()
            .expect("an active transaction retains its core transaction")
    }

    /// Take the core transaction, ending the wrapper's active state. The
    /// caller must release the worker slot once the core transaction is gone.
    fn take_inner(&mut self) -> Option<StoTransaction> {
        self.active = false;
        self.inner.take()
    }

    fn abort_inner(&mut self) -> Result<()> {
        if let Some(inner) = self.take_inner() {
            let _ = inner.abort();
            let cleanup = self.db.check_abort_cleanup();
            self.attachment.slot.release();
            self.touched.record_cleanup(cleanup.is_ok());
            cleanup?;
        }
        Ok(())
    }

    #[cold]
    fn fail_access<T>(&mut self, error: AccessError) -> Result<T> {
        let (error, effect) = map_access(error);
        match effect {
            Effect::Active => Err(error),
            Effect::Finished => {
                self.abort_inner()?;
                if worker::take_cleanup_failure(TestCleanupBoundary::Operation) {
                    self.touched.record_cleanup(false);
                    worker::poison_current_worker();
                    return Err(Error::WorkerPoisoned);
                }
                Err(error)
            }
            Effect::Quarantined => {
                let _ = self.abort_inner();
                worker::poison_current_worker();
                Err(error)
            }
        }
    }

    /// Abort after a wrapper-level rejection. An abort-cleanup failure is
    /// more severe than the rejection and replaces it, as in the C ABI engine.
    #[cold]
    fn fail_closed<T>(&mut self, error: Error) -> Result<T> {
        match self.abort_now() {
            Ok(()) => Err(error),
            Err(cleanup) => Err(cleanup),
        }
    }

    #[inline]
    fn charge(&mut self, credits: usize) -> bool {
        match self.budget_used.checked_add(credits) {
            Some(next) if next <= TRANSACTION_ITEM_BUDGET => {
                self.budget_used = next;
                true
            }
            _ => false,
        }
    }

    fn invalidate_witness(&mut self) {
        self.fast_eligible = false;
        self.one_put = None;
    }

    /// A read after a put retires the put's borrowed one-put witness.
    #[inline]
    fn invalidate_borrowed_witness(&mut self) {
        if self.one_put.is_some() {
            self.invalidate_witness();
        }
    }
    fn touch(&mut self, table_id: u64, key: &[u8]) -> usize {
        self.touched.touch(table_id, key)
    }

    fn recycle_touched(&mut self) {
        if self.touched.has_storage() {
            let log = std::mem::take(&mut self.touched);
            self.attachment.slot.recycle_touched(log);
        }
    }

    fn canonical_ops(&self) -> Vec<OpRef<'_>> {
        self.touched
            .iter()
            .filter_map(|entry| match entry.staged {
                Staged::Live => Some(OpRef {
                    kind: OpKind::Put,
                    table_id: entry.table_id,
                    key: &entry.key,
                    value: &entry.value,
                }),
                Staged::Deleted if entry.existed_before == Some(true) => Some(OpRef {
                    kind: OpKind::Delete,
                    table_id: entry.table_id,
                    key: &entry.key,
                    value: &[],
                }),
                _ => None,
            })
            .collect()
    }

    /// The restricted ordering profile: exactly one local update covered by
    /// its own write lock, with no other observation.
    fn can_order_after_validation(&self) -> bool {
        if self.has_predicate {
            return false;
        }
        let mut writes = 0;
        for entry in &self.touched {
            if entry.inserted || entry.deleted {
                return false;
            }
            if entry.read && !entry.wrote {
                return false;
            }
            if entry.wrote {
                writes += 1;
                if writes != 1 {
                    return false;
                }
            } else if !entry.read {
                return false;
            }
        }
        writes == 1
    }

    // ---- point operations ------------------------------------------------

    /// Read a key; missing and present-empty are distinct.
    pub fn get(&mut self, table: &Table<'db>, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.candidate_bytes = None;
        if key.len() > MAX_KEY_BYTES {
            return Err(Error::ValueTooLarge);
        }
        self.check_active()?;
        self.check_table(table)?;
        self.invalidate_borrowed_witness();
        if !self.charge(1) {
            return self.fail_closed(Error::TransactionTooLarge);
        }
        let worker = self.attachment.worker;
        let result = table.entry.table.get(self.inner_mut(), worker, key);
        match result {
            Ok(value) => {
                let present = value.is_some();
                let index = self.touch(table.entry.id, key);
                let entry = &mut self.touched[index];
                entry.read = true;
                entry.existed_before.get_or_insert(present);
                Ok(value.map(|value| value.as_ref().to_vec()))
            }
            Err(error) => self.fail_access(error),
        }
    }

    /// Upsert a key, returning whether it was absent immediately before.
    #[inline]
    pub fn put(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        self.candidate_bytes = None;
        self.check_active()?;
        if key.len() > MAX_KEY_BYTES || value.len() > MAX_VALUE_BYTES {
            return Err(Error::ValueTooLarge);
        }
        self.check_table(table)?;
        let trusted = self
            .trusted
            .is_some_and(|bound| ptr::eq(bound, table.entry));
        let capture =
            trusted && self.fast_eligible && self.one_put.is_none() && self.mutations == 0;
        if !capture {
            self.invalidate_witness();
        }
        if !self.charge(write_item_charge(key.len())) {
            return self.fail_closed(Error::TransactionTooLarge);
        }
        let worker = self.attachment.worker;
        // Only the first trusted operation borrows. Its immutable allocation
        // is anchored before the Table call, including errors and unwinds.
        // Later operations retain this one allocation if they replace it and
        // use ordinary owned staging, never another borrowed generation.
        let borrow_first = capture
            && self.touched.can_borrow_first()
            && value.len() > 38
            && value.len() <= sto_masstree::STABLE_ATOMIC_VALUE_CAPACITY;
        let result = if borrow_first {
            let bytes = self.touched.prepare_first(table.entry.id, key, value);
            let inner = self
                .inner
                .as_mut()
                .expect("active transaction retains core");
            // SAFETY: TouchLog owns these bytes before core entry and neither
            // mutates nor frees them until cleanup is proved. Failed cleanup
            // pins the allocation independently of any caller-owned holder.
            unsafe {
                table
                    .entry
                    .table
                    .put_borrowed_with_previous_presence(inner, worker, key, bytes)
            }
        } else {
            table
                .entry
                .table
                .put_with_previous_presence(self.inner_mut(), worker, key, value)
        };
        let existed = match result {
            Ok(existed) => existed,
            Err(error) => return self.fail_access(error),
        };
        self.mutations += 1;
        let index = if borrow_first {
            self.touched.accept_first()
        } else {
            let index = self.touch(table.entry.id, key);
            self.touched.retire_value(index);
            index
        };
        let entry = &mut self.touched[index];
        entry.wrote = true;
        entry.staged = Staged::Live;
        if !borrow_first {
            entry.value.clear();
            entry.value.extend_from_slice(value);
        }
        if !existed {
            entry.inserted = true;
        }
        entry.existed_before.get_or_insert(existed);
        if capture {
            let exact = UNCHECKED_ONE_PUT_OVERHEAD_BYTES + key.len() + value.len();
            if exact <= FAST_PUT_RECORD_BYTES_MAX {
                let exact_bytes = u32::try_from(exact).expect("bounded by the u32 witness");
                self.one_put = Some(OnePut {
                    touched: index,
                    exact_bytes,
                });
                self.candidate_bytes = NonZeroU32::new(exact_bytes);
            } else {
                self.invalidate_witness();
            }
        }
        Ok(!existed)
    }

    /// Insert only when absent, returning whether insertion was staged.
    pub fn insert(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        self.candidate_bytes = None;
        if key.len() > MAX_KEY_BYTES || value.len() > MAX_VALUE_BYTES {
            return Err(Error::ValueTooLarge);
        }
        self.check_active()?;
        self.check_table(table)?;
        self.invalidate_witness();
        if !self.charge(write_item_charge(key.len())) {
            return self.fail_closed(Error::TransactionTooLarge);
        }
        let worker = self.attachment.worker;
        let result = table
            .entry
            .table
            .insert(self.inner_mut(), worker, key, value);
        let inserted = match result {
            Ok(InsertOutcome::Inserted) => true,
            Ok(InsertOutcome::AlreadyPresent(_)) => false,
            Err(error) => return self.fail_access(error),
        };
        let index = self.touch(table.entry.id, key);
        if inserted {
            self.touched.retire_value(index);
        }
        let entry = &mut self.touched[index];
        if inserted {
            self.mutations += 1;
            entry.wrote = true;
            entry.inserted = true;
            entry.staged = Staged::Live;
            entry.value.clear();
            entry.value.extend_from_slice(value);
            entry.existed_before.get_or_insert(false);
        } else {
            entry.read = true;
            entry.existed_before.get_or_insert(true);
        }
        Ok(inserted)
    }

    /// Remove a key, returning whether a live value existed.
    pub fn remove(&mut self, table: &Table<'db>, key: &[u8]) -> Result<bool> {
        self.candidate_bytes = None;
        if key.len() > MAX_KEY_BYTES {
            return Err(Error::ValueTooLarge);
        }
        self.check_active()?;
        self.check_table(table)?;
        self.invalidate_witness();
        if !self.charge(1) {
            return self.fail_closed(Error::TransactionTooLarge);
        }
        let worker = self.attachment.worker;
        let result = table
            .entry
            .table
            .remove_with_previous_presence(self.inner_mut(), worker, key);
        let existed = match result {
            Ok(existed) => existed,
            Err(error) => return self.fail_access(error),
        };
        let index = self.touch(table.entry.id, key);
        if existed {
            self.touched.retire_value(index);
        }
        let entry = &mut self.touched[index];
        if existed {
            self.mutations += 1;
            entry.wrote = true;
            entry.deleted = true;
            entry.staged = Staged::Deleted;
            entry.value.clear();
            entry.existed_before.get_or_insert(true);
        } else {
            entry.read = true;
            entry.existed_before.get_or_insert(false);
        }
        Ok(existed)
    }

    /// Scan `[lower, upper)` in ascending binary-key order.
    pub fn scan<'txn>(
        &'txn mut self,
        table: &Table<'db>,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>> {
        self.candidate_bytes = None;
        Scan::new(self, *table, ScanDirection::Forward, lower, upper)
    }

    /// Scan `[lower, upper)` in descending binary-key order.
    pub fn rscan<'txn>(
        &'txn mut self,
        table: &Table<'db>,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>> {
        self.candidate_bytes = None;
        Scan::new(self, *table, ScanDirection::Reverse, lower, upper)
    }

    // ---- record plans ----------------------------------------------------

    /// Exact one-put unchecked-v6 candidate, if it is still current.
    #[doc(hidden)]
    pub fn unchecked_one_put_record_candidate(&self) -> Option<CommitRecordPreflight> {
        if !self.active || self.trusted.is_none() || self.plan.is_some() {
            return None;
        }
        self.candidate_bytes.map(|bytes| {
            CommitRecordPreflight::from_parts(bytes.get() as usize, 1, CommitRecordChecksum::None)
        })
    }

    /// Compact one-put candidate for the cache's single-producer fast path.
    ///
    /// # Safety
    ///
    /// The caller owns an active trusted transaction and performs no other
    /// operation before the consuming terminal.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn trusted_unchecked_one_put_record_bytes_candidate(&self) -> Option<NonZeroU32> {
        debug_assert!(self.active);
        debug_assert!(self.trusted.is_some());
        debug_assert!(self.plan.is_none());
        self.candidate_bytes
    }

    /// Seal and size the canonical CRC32C (v5) record plan.
    #[doc(hidden)]
    pub fn commit_record_preflight(
        &mut self,
        max_record_bytes: usize,
    ) -> Result<CommitRecordPreflight> {
        self.commit_record_preflight_with_checksum(max_record_bytes, CommitRecordChecksum::Crc32c)
    }

    /// Seal and size the canonical record plan in an explicit integrity mode.
    #[doc(hidden)]
    pub fn commit_record_preflight_with_checksum(
        &mut self,
        max_record_bytes: usize,
        checksum: CommitRecordChecksum,
    ) -> Result<CommitRecordPreflight> {
        self.check_active()?;
        self.candidate_bytes = None;
        if self.trusted.is_none() {
            return Err(Error::FeatureUnavailable);
        }
        let ops = self.canonical_ops();
        let Some(exact_record_bytes) = record::exact_record_bytes(&ops, checksum) else {
            return self.fail_closed(Error::Internal);
        };
        let op_count = ops.len();
        drop(ops);
        if op_count > TRANSACTION_ITEM_BUDGET {
            return self.fail_closed(Error::Internal);
        }
        let preflight =
            CommitRecordPreflight::from_parts(exact_record_bytes, op_count as u32, checksum);
        if op_count != 0 && exact_record_bytes > max_record_bytes {
            // A cap rejection consumes the transaction so a caller cannot fall
            // back to an unlogged commit after observing the error.
            return self.fail_closed(Error::ValueTooLarge);
        }
        self.plan = Some(preflight);
        Ok(preflight)
    }

    // ---- ordinary commit and abort ---------------------------------------

    /// Validate and install the transaction's writes.
    #[inline]
    pub fn commit(self) -> Result<()> {
        let report = self.commit_report();
        match report.disposition {
            CommitDisposition::Committed => report.cleanup,
            CommitDisposition::Aborted(error) | CommitDisposition::Unknown(error) => Err(error),
        }
    }

    /// Commit while keeping visibility and cleanup as separate outcomes.
    #[inline]
    pub fn commit_report(mut self) -> CommitReport {
        if self.plan.is_some_and(|plan| !plan.is_empty()) {
            // A nonempty sealed plan may install only through a record
            // terminal; committing here would bypass durability.
            let cleanup = self.abort_now();
            return CommitReport {
                disposition: match cleanup {
                    Ok(()) => CommitDisposition::Aborted(Error::InvalidArgument),
                    Err(error) => CommitDisposition::Unknown(error),
                },
                cleanup,
            };
        }
        let Some(inner) = self.take_inner() else {
            return CommitReport {
                disposition: CommitDisposition::Aborted(Error::TransactionFinished),
                cleanup: Ok(()),
            };
        };
        let outcome = inner.commit();
        self.attachment.slot.release();
        let report = map_commit(outcome, None);
        self.apply_commit_cleanup_failure(report)
    }

    fn apply_commit_cleanup_failure(&mut self, report: CommitReport) -> CommitReport {
        let report = if worker::take_cleanup_failure(TestCleanupBoundary::Commit) {
            worker::poison_current_worker();
            QUARANTINED_REPORT
        } else {
            report
        };
        self.touched.record_cleanup(
            report.cleanup.is_ok() && !matches!(report.disposition, CommitDisposition::Unknown(_)),
        );
        report
    }

    fn abort_now(&mut self) -> Result<()> {
        if !self.active {
            return Ok(());
        }
        self.abort_inner()?;
        if worker::take_cleanup_failure(TestCleanupBoundary::Abort) {
            self.touched.record_cleanup(false);
            worker::poison_current_worker();
            return Err(Error::WorkerPoisoned);
        }
        Ok(())
    }

    /// Explicitly abort and consume this transaction.
    pub fn abort(mut self) -> Result<()> {
        self.abort_now()
    }

    // ---- record terminals ------------------------------------------------

    /// Definite pre-acceptance abort of a record terminal.
    #[cold]
    fn reject_record(&mut self, error: Error) -> CommitReport {
        match self.abort_now() {
            Ok(()) => definite(error),
            Err(_) => QUARANTINED_REPORT,
        }
    }

    fn one_put_op(&self) -> Option<(OnePut, OpRef<'_>)> {
        let one_put = self.one_put?;
        let entry = self.touched.get(one_put.touched)?;
        if entry.staged != Staged::Live {
            return None;
        }
        Some((
            one_put,
            OpRef {
                kind: OpKind::Put,
                table_id: entry.table_id,
                key: &entry.key,
                value: &entry.value,
            },
        ))
    }

    /// Validate a one-put candidate against the live witness and seal it.
    /// `expected_record_bytes` must equal the witness captured at `put`; the
    /// exact-bytes recomputation is already implied by that equality, so the
    /// terminal skips a second `exact_record_bytes` walk over the op.
    fn seal_one_put_candidate(&mut self, expected_record_bytes: usize) -> Option<OnePut> {
        if self.plan.is_some() || expected_record_bytes == 0 {
            return None;
        }
        let one_put = self.one_put?;
        if one_put.exact_bytes as usize != expected_record_bytes {
            return None;
        }
        let entry = self.touched.get(one_put.touched)?;
        if entry.staged != Staged::Live {
            return None;
        }
        self.plan = Some(CommitRecordPreflight::from_parts(
            expected_record_bytes,
            1,
            CommitRecordChecksum::None,
        ));
        Some(one_put)
    }

    /// Consume the core transaction for a record terminal, or report a
    /// finished handle. The caller releases the worker slot afterwards.
    fn take_for_terminal(&mut self) -> Option<StoTransaction> {
        self.take_inner()
    }

    /// Finish a terminal run: release the worker slot and apply the test-only
    /// commit cleanup failure.
    fn finish_run(&mut self, run: Run) -> Run {
        self.attachment.slot.release();
        let report = self.apply_commit_cleanup_failure(run.report);
        Run { report, ..run }
    }

    /// General record terminal over a caller-bound target.
    ///
    /// # Safety
    ///
    /// Any storage returned by `acquire` must stay valid through this call.
    #[doc(hidden)]
    pub unsafe fn commit_report_with_record_target<F>(mut self, acquire: F) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        let Some(preflight) = self.plan else {
            return unbound_record_report(self.reject_record(Error::InvalidArgument));
        };
        if self.trusted.is_none() || preflight.is_empty() {
            return unbound_record_report(self.reject_record(Error::InvalidArgument));
        }
        if !self.active {
            return unbound_record_report(definite(Error::TransactionFinished));
        }
        if !self.db.rust_sequence_allowed() {
            return unbound_record_report(self.reject_record(Error::InvalidArgument));
        }
        let gate = if self.db.per_worker_allowed() {
            Gate::Hlc
        } else {
            Gate::Ticket
        };
        let Some(inner) = self.take_for_terminal() else {
            return unbound_record_report(definite(Error::TransactionFinished));
        };
        let ops = self.canonical_ops();
        let mut acquire = Some(acquire);
        let run = run_terminal(
            inner,
            self.db,
            Spec {
                gate,
                gate_after_validation: false,
                order: Order::RustSequence,
                unhealthy: None,
                sink: Sink::Callback,
                acquire: Some(move |timestamp, preflight, _sequence| {
                    acquire
                        .take()
                        .and_then(|acquire| acquire(timestamp, preflight))
                }),
                preflight,
                ops: &ops,
                prepared_overflow_key: Vec::new(),
            },
        );
        let run = self.finish_run(run);
        TrustedUncheckedOnePutRecordOutcome {
            report: run.report,
            record_bound: run.bound,
            record_written: run.written,
        }
        .into_report()
    }

    /// One-put variant of [`Self::commit_report_with_record_target`].
    ///
    /// # Safety
    ///
    /// As above; `candidate` must be the current one-put plan. A stale
    /// candidate (any operation ran after it was taken) is refused before
    /// `acquire` runs, so the engine never binds storage for it.
    #[doc(hidden)]
    pub unsafe fn commit_report_with_unchecked_one_put_record_target<F>(
        mut self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        if !self.active {
            return unbound_record_report(definite(Error::TransactionFinished));
        }
        if !self.db.rust_sequence_allowed()
            || candidate.op_count() != 1
            || candidate.checksum() != CommitRecordChecksum::None
            || self
                .seal_one_put_candidate(candidate.exact_record_bytes())
                .is_none()
        {
            return unbound_record_report(self.reject_record(Error::InvalidArgument));
        }
        let gate = if self.db.per_worker_allowed() {
            Gate::Hlc
        } else {
            Gate::Ticket
        };
        let after_validation = self.can_order_after_validation();
        let Some(inner) = self.take_for_terminal() else {
            return unbound_record_report(definite(Error::TransactionFinished));
        };
        let (_, op) = self.one_put_op().expect("sealed candidate");
        let ops = [op];
        let mut acquire = Some(acquire);
        let run = run_terminal(
            inner,
            self.db,
            Spec {
                gate,
                gate_after_validation: after_validation,
                order: Order::RustSequence,
                unhealthy: None,
                sink: Sink::Callback,
                acquire: Some(move |timestamp, preflight, _sequence| {
                    acquire
                        .take()
                        .and_then(|acquire| acquire(timestamp, preflight))
                }),
                preflight: candidate,
                ops: &ops,
                prepared_overflow_key: Vec::new(),
            },
        );
        let run = self.finish_run(run);
        TrustedUncheckedOnePutRecordOutcome {
            report: run.report,
            record_bound: run.bound,
            record_written: run.written,
        }
        .into_report()
    }

    /// Concurrent engine-ordered one-put terminal with a bind callback.
    ///
    /// # Safety
    ///
    /// Storage from `acquire` must stay valid through the call; `unhealthy`
    /// and `next_bound` must be the queue's live words.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_native_ordered_unchecked_one_put_record_target<F>(
        mut self,
        candidate: CommitRecordPreflight,
        next_bound: &AtomicU64,
        unhealthy: &AtomicBool,
        acquire: F,
    ) -> TrustedNativeOrderedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight, NonZeroU64) -> Option<CommitRecordTarget>,
    {
        let _ = next_bound;
        if !self.active {
            return native_ordered_rejected(definite(Error::TransactionFinished));
        }
        if !self.db.packed_allowed()
            || candidate.op_count() != 1
            || candidate.checksum() != CommitRecordChecksum::None
            || self
                .seal_one_put_candidate(candidate.exact_record_bytes())
                .is_none()
        {
            return native_ordered_rejected(self.reject_record(Error::InvalidArgument));
        }
        let restricted = self.can_order_after_validation();
        let Some(inner) = self.take_for_terminal() else {
            return native_ordered_rejected(definite(Error::TransactionFinished));
        };
        let (_, op) = self.one_put_op().expect("sealed candidate");
        let ops = [op];
        let mut acquire = Some(acquire);
        let run = run_terminal(
            inner,
            self.db,
            Spec {
                gate: if restricted { Gate::None } else { Gate::Hlc },
                gate_after_validation: restricted,
                order: if restricted {
                    Order::RestrictedPair
                } else {
                    Order::NativeLocked
                },
                unhealthy: Some(unhealthy.as_ptr().cast::<u8>().cast_const()),
                sink: Sink::Callback,
                acquire: Some(move |timestamp, preflight, sequence| {
                    let sequence = NonZeroU64::new(sequence)?;
                    acquire
                        .take()
                        .and_then(|acquire| acquire(timestamp, preflight, sequence))
                }),
                preflight: candidate,
                ops: &ops,
                prepared_overflow_key: Vec::new(),
            },
        );
        let run = self.finish_run(run);
        TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                report: run.report,
                record_bound: run.bound,
                record_written: run.written,
            },
            ordered_sequence: run.sequence,
            timestamp_stamp: run.timestamp_stamp,
            target_bound: run.bound,
            native_holder_ready: 0,
        }
    }

    /// Concurrent engine-ordered one-put terminal writing into the byte arena.
    ///
    /// # Safety
    ///
    /// `control` must describe live, stable publication and arena storage.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_native_ordered_unchecked_one_put_arena(
        mut self,
        candidate: CommitRecordPreflight,
        control: &TrustedNativeOrderedArenaControl,
    ) -> TrustedNativeOrderedOnePutRecordOutcome {
        if !self.active {
            return native_ordered_rejected(definite(Error::TransactionFinished));
        }
        let exact = candidate.exact_record_bytes();
        if !self.db.packed_allowed()
            || candidate.op_count() != 1
            || candidate.checksum() != CommitRecordChecksum::None
            || !valid_arena_control(control, exact)
            || self.seal_one_put_candidate(exact).is_none()
        {
            return native_ordered_rejected(self.reject_record(Error::InvalidArgument));
        }
        let restricted = self.can_order_after_validation();
        let Some(inner) = self.take_for_terminal() else {
            return native_ordered_rejected(definite(Error::TransactionFinished));
        };
        let (_, op) = self.one_put_op().expect("sealed candidate");
        let ops = [op];
        let run = run_terminal(
            inner,
            self.db,
            Spec {
                gate: if restricted { Gate::None } else { Gate::Hlc },
                gate_after_validation: restricted,
                order: if restricted {
                    Order::RestrictedPair
                } else {
                    Order::NativeLocked
                },
                unhealthy: Some(control.unhealthy),
                sink: Sink::Arena(*control),
                acquire: no_acquire(),
                preflight: candidate,
                ops: &ops,
                prepared_overflow_key: Vec::new(),
            },
        );
        let run = self.finish_run(run);
        TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                report: run.report,
                record_bound: run.bound,
                record_written: run.written,
            },
            ordered_sequence: run.sequence,
            timestamp_stamp: run.timestamp_stamp,
            target_bound: run.bound,
            native_holder_ready: 0,
        }
    }

    /// Concurrent engine-ordered one-put terminal sealing a holder.
    ///
    /// # Safety
    ///
    /// `control` must describe live, stable publication storage and pool.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_native_ordered_unchecked_one_put_holder(
        mut self,
        candidate: CommitRecordPreflight,
        control: &TrustedNativeOrderedHolderControl,
    ) -> TrustedNativeOrderedOnePutRecordOutcome {
        if !self.active {
            return native_ordered_rejected(definite(Error::TransactionFinished));
        }
        let exact = candidate.exact_record_bytes();
        if !self.db.packed_allowed()
            || candidate.op_count() != 1
            || candidate.checksum() != CommitRecordChecksum::None
            || !valid_holder_control(control, exact)
            || self.seal_one_put_candidate(exact).is_none()
        {
            return native_ordered_rejected(self.reject_record(Error::InvalidArgument));
        }
        let restricted = self.can_order_after_validation();
        let (one_put, op) = self.one_put_op().expect("sealed candidate");
        // Long keys allocate before commit; the bind callback only swaps.
        let mut prepared_overflow_key = Vec::new();
        let key_len = op.key.len();
        let table_id = op.table_id;
        if key_len > crate::holder::INLINE_KEY_BYTES {
            if prepared_overflow_key.try_reserve_exact(key_len).is_err() {
                return native_ordered_rejected(self.reject_record(Error::OutOfMemory));
            }
            prepared_overflow_key.extend_from_slice(op.key);
        }
        let Some(inner) = self.take_for_terminal() else {
            return native_ordered_rejected(definite(Error::TransactionFinished));
        };
        let (_, op) = self.one_put_op().expect("sealed candidate");
        let ops = [op];
        let run = run_terminal(
            inner,
            self.db,
            Spec {
                gate: if restricted { Gate::None } else { Gate::Hlc },
                gate_after_validation: restricted,
                order: if restricted {
                    Order::RestrictedPair
                } else {
                    Order::NativeLocked
                },
                unhealthy: Some(control.unhealthy),
                sink: Sink::OrderedHolder(*control),
                acquire: no_acquire(),
                preflight: candidate,
                ops: &ops,
                prepared_overflow_key,
            },
        );
        let run = self.finish_run(run);
        let mut holder_sealed = false;
        let mut holder_ready = false;
        if run.sequence != 0 {
            if run.holder.is_null() || run.publication.is_null() {
                std::process::abort();
            }
            // SAFETY: the bind callback Acquired this generation's FREE turn
            // and Release-published BOUND; nothing else can touch the holder
            // until READY or a cache pin.
            let holder = unsafe { &mut *run.holder };
            self.seal_holder_value(
                holder,
                one_put.touched,
                run.sequence,
                table_id,
                run.timestamp_stamp,
            );
            holder_sealed = true;
            if run.report.disposition == CommitDisposition::Committed && run.report.cleanup.is_ok()
            {
                // SAFETY: as above; the publication pointer came from bind.
                unsafe {
                    publish_holder_ready(
                        run.publication,
                        control.publication_shift,
                        run.sequence,
                        run.timestamp_stamp,
                        exact,
                    )
                };
                holder_ready = true;
            }
        }
        TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                report: run.report,
                record_bound: run.bound,
                record_written: holder_sealed,
            },
            ordered_sequence: run.sequence,
            timestamp_stamp: run.timestamp_stamp,
            target_bound: run.bound,
            native_holder_ready: u64::from(holder_ready),
        }
    }

    /// Single-producer preselected one-put terminal writing the record bytes.
    ///
    /// # Safety
    ///
    /// `target` must be the exact preselected storage and the caller must hold
    /// whole-call exclusion over the cache-order namespace.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
        mut self,
        candidate: CommitRecordPreflight,
        target: CommitRecordTarget,
    ) -> TrustedPreselectedUncheckedOnePutRecordOutcome {
        if !self.active {
            return TrustedPreselectedUncheckedOnePutRecordOutcome {
                report: definite(Error::TransactionFinished),
                record_state: 0,
            };
        }
        let exact = candidate.exact_record_bytes();
        if !self.db.single_producer_allowed()
            || candidate.op_count() != 1
            || candidate.checksum() != CommitRecordChecksum::None
            || exact > FAST_PUT_RECORD_BYTES_MAX
            || target.exact_record_bytes() < exact
            || self.seal_one_put_candidate(exact).is_none()
        {
            return TrustedPreselectedUncheckedOnePutRecordOutcome {
                report: self.reject_record(Error::InvalidArgument),
                record_state: 0,
            };
        }
        let Some(inner) = self.take_for_terminal() else {
            return TrustedPreselectedUncheckedOnePutRecordOutcome {
                report: definite(Error::TransactionFinished),
                record_state: 0,
            };
        };
        let (_, op) = self.one_put_op().expect("sealed candidate");
        let ops = [op];
        let run = run_terminal(
            inner,
            self.db,
            Spec {
                gate: Gate::Hlc,
                gate_after_validation: false,
                order: Order::RustSequence,
                unhealthy: None,
                sink: Sink::Preselected(target),
                acquire: no_acquire(),
                preflight: candidate,
                ops: &ops,
                prepared_overflow_key: Vec::new(),
            },
        );
        let run = self.finish_run(run);
        TrustedPreselectedUncheckedOnePutRecordOutcome {
            report: run.report,
            record_state: (run.timestamp_stamp << 1) | u64::from(run.written),
        }
    }

    /// Shared body of the preselected holder terminals.
    ///
    /// # Safety
    ///
    /// `holder` must be the FREE holder exclusively leased for `sequence`.
    unsafe fn commit_preselected_holder(
        &mut self,
        exact_record_bytes: NonZeroU32,
        holder: *mut Holder,
        sequence: NonZeroU64,
        unhealthy: Option<*const u8>,
        per_worker: bool,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        if !self.active {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: definite(Error::TransactionFinished),
                holder_state: 0,
            };
        }
        let exact = exact_record_bytes.get() as usize;
        if !self.db.rust_sequence_allowed() || self.seal_one_put_candidate(exact).is_none() {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: self.reject_record(Error::InvalidArgument),
                holder_state: 0,
            };
        }
        let (one_put, op) = self.one_put_op().expect("sealed candidate");
        // SAFETY: the caller's lane lease makes this generation exclusively
        // ours until the cache publishes it.
        let holder_ref = unsafe { &mut *holder };
        if !holder_ref.is_free() {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: self.reject_record(Error::InvalidArgument),
                holder_state: 0,
            };
        }
        if let Err(error) = holder_ref.stage_key(op.key) {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: self.reject_record(error),
                holder_state: 0,
            };
        }
        let restricted = per_worker && self.can_order_after_validation();
        let table_id = op.table_id;
        let Some(inner) = self.take_for_terminal() else {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: definite(Error::TransactionFinished),
                holder_state: 0,
            };
        };
        // This terminal only accepts a timestamp. It never encodes a record,
        // allocates a sequence, or binds an arena/callback target, so retain
        // only the state its hook actually needs. The candidate, holder and
        // ordering-profile checks above are unchanged.
        let mut run = holder_commit::run(inner, unhealthy, restricted);
        self.attachment.slot.release();
        run.report = self.apply_commit_cleanup_failure(run.report);
        let mut holder_state = 0;
        if run.timestamp_stamp != 0 {
            // The accepted hook only captured the timestamp; transfer the
            // staged value and seal after the core has finished.
            self.seal_holder_value(
                holder_ref,
                one_put.touched,
                sequence.get(),
                table_id,
                run.timestamp_stamp,
            );
            holder_state = (run.timestamp_stamp << 1) | 1;
        }
        TrustedPreselectedUncheckedOnePutHolderOutcome {
            report: run.report,
            holder_state,
        }
    }
    #[inline]
    fn seal_holder_value(
        &mut self,
        holder: &mut Holder,
        index: usize,
        sequence: u64,
        table_id: u64,
        stamp: u64,
    ) {
        if self.touched.value_is_borrowed(index) {
            // Preserve the accepted holder/report contract on uncertainty,
            // without handing the core's borrowed owner to a caller's pool.
            self.seal_uncertain_holder_value(holder, index, sequence, table_id, stamp);
        } else {
            holder.seal(&mut self.touched[index].value, sequence, table_id, stamp);
        }
    }

    #[cold]
    #[inline(never)]
    fn seal_uncertain_holder_value(
        &self,
        holder: &mut Holder,
        index: usize,
        sequence: u64,
        table_id: u64,
        stamp: u64,
    ) {
        let mut independent = self.touched[index].value.clone();
        holder.seal(&mut independent, sequence, table_id, stamp);
    }

    /// Single-producer preselected one-put terminal sealing a holder.
    ///
    /// # Safety
    ///
    /// The producer lease must own `sequence` in `pool` through the call.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
        mut self,
        exact_record_bytes: NonZeroU32,
        pool: &TrustedOnePutHolderPool,
        sequence: NonZeroU64,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        if !self.db.single_producer_allowed() {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: self.reject_record(Error::InvalidArgument),
                holder_state: 0,
            };
        }
        let holder = pool.holder_ptr(sequence.get());
        // SAFETY: forwarded from this method's contract.
        unsafe { self.commit_preselected_holder(exact_record_bytes, holder, sequence, None, false) }
    }

    /// Per-worker preselected one-put terminal sealing a holder in one lane.
    ///
    /// # Safety
    ///
    /// The lane must own `sequence` and `control` must outlive the call.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
        mut self,
        exact_record_bytes: NonZeroU32,
        control: &TrustedSpscOnePutHolderControl,
        sequence: NonZeroU64,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        if !self.db.per_worker_allowed() || control.unhealthy.is_null() {
            return TrustedPreselectedUncheckedOnePutHolderOutcome {
                report: self.reject_record(Error::InvalidArgument),
                holder_state: 0,
            };
        }
        // SAFETY: the control lends a live pool for the whole call.
        let pool = unsafe { &*control.pool };
        let holder = pool.holder_ptr(sequence.get());
        // SAFETY: forwarded from this method's contract.
        unsafe {
            self.commit_preselected_holder(
                exact_record_bytes,
                holder,
                sequence,
                Some(control.unhealthy),
                true,
            )
        }
    }

    /// Fused SPSC one-put terminal that publishes ACK itself on success.
    ///
    /// Returns `true` when the engine consumed the transaction and published;
    /// the caller must then forget this facade. Otherwise `cold_attempt` is
    /// initialized and describes transaction and queue ownership.
    ///
    /// # Safety
    ///
    /// The caller is the unique producer of `control`'s lane and every pointer
    /// stays live and stable through the call.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
        &mut self,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
        cold_attempt: &mut MaybeUninit<TrustedFusedOnePutHolderAttempt>,
    ) -> bool {
        debug_assert!(self.trusted.is_some());
        debug_assert!(self.plan.is_none());
        debug_assert!(self.active);
        // SAFETY: the contract makes these aligned atomic words live.
        let acknowledged = unsafe { AtomicU64::from_ptr(control.acknowledged) };
        let unhealthy = unsafe { AtomicU8::from_ptr(control.unhealthy.cast_mut()) };
        let producer_next = unsafe { AtomicU64::from_ptr(producer_next) };
        let capacity_limit =
            unsafe { AtomicU64::from_ptr(capacity_limit.cast_mut()) }.load(Ordering::Relaxed);

        let exact_record_bytes = self.candidate_bytes.map_or(0, NonZeroU32::get);
        if !self.db.rust_sequence_allowed() {
            let report = self.reject_record(Error::InvalidArgument);
            let outcome = TrustedPreselectedUncheckedOnePutHolderOutcome {
                report,
                holder_state: 0,
            };
            producer_next.store(acknowledged.load(Ordering::Relaxed), Ordering::Relaxed);
            cold_attempt.write(match NonZeroU32::new(exact_record_bytes) {
                Some(exact_record_bytes) => FusedOnePutHolderAttempt::ConsumedOutcome {
                    outcome,
                    exact_record_bytes,
                },
                None => FusedOnePutHolderAttempt::ConsumedMalformed,
            });
            return false;
        }
        // `exact - 1 >= max` also rejects a missing candidate (zero wraps).
        if exact_record_bytes.wrapping_sub(1) >= control.max_record_bytes {
            if unhealthy.load(Ordering::Acquire) == 0 {
                producer_next.store(acknowledged.load(Ordering::Relaxed), Ordering::Relaxed);
            }
            cold_attempt.write(FusedOnePutHolderAttempt::UntouchedGeneral);
            return false;
        }
        let exact = NonZeroU32::new(exact_record_bytes).expect("checked above");
        if unhealthy.load(Ordering::Acquire) != 0 {
            cold_attempt.write(FusedOnePutHolderAttempt::UntouchedSlow {
                exact_record_bytes: exact,
            });
            return false;
        }
        let tail = acknowledged.load(Ordering::Relaxed);
        if tail >= capacity_limit {
            producer_next.store(tail, Ordering::Relaxed);
            cold_attempt.write(FusedOnePutHolderAttempt::UntouchedSlow {
                exact_record_bytes: exact,
            });
            return false;
        }

        let sequence = NonZeroU64::new(tail + 1).expect("tail + 1 is nonzero");
        // SAFETY: the control lends a live pool for the whole call.
        let pool = unsafe { &*control.pool };
        let holder = pool.holder_ptr(sequence.get());
        let per_worker = self.db.per_worker_allowed();
        // SAFETY: the unique lane producer owns this generation until ACK.
        let outcome = unsafe {
            self.commit_preselected_holder(
                exact,
                holder,
                sequence,
                Some(control.unhealthy),
                per_worker,
            )
        };
        if let Some(timestamp) = outcome.committed_timestamp() {
            if unhealthy.load(Ordering::Acquire) != 0 {
                producer_next.store(sequence.get(), Ordering::Relaxed);
                cold_attempt.write(FusedOnePutHolderAttempt::CommittedUnpublished {
                    timestamp,
                    exact_record_bytes: exact,
                });
                return false;
            }
            // The caller deliberately forgets this facade on success. Return
            // owned wrapper buffers now; no destructor will run afterward.
            self.recycle_touched();
            acknowledged.store(sequence.get(), Ordering::Release);
            return true;
        }
        producer_next.store(acknowledged.load(Ordering::Relaxed), Ordering::Relaxed);
        cold_attempt.write(FusedOnePutHolderAttempt::ConsumedOutcome {
            outcome,
            exact_record_bytes: exact,
        });
        false
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        if self.active {
            let _ = self.abort_inner();
            if worker::take_cleanup_failure(TestCleanupBoundary::Abort) {
                self.touched.record_cleanup(false);
                worker::poison_current_worker();
            }
        }
        // All terminals have consumed the core transaction before returning
        // this operation log to its worker. Retire every operation and keep
        // only bounded backing storage for the next transaction.
        self.recycle_touched();
    }
}

/// Run one record terminal through `sto-core`'s commit hook seams.
fn run_terminal<'a, F>(inner: StoTransaction, db: &'a LocalDb, mut spec: Spec<'a, F>) -> Run
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight, u64) -> Option<CommitRecordTarget>,
{
    let mut driver = Driver {
        db,
        clock: mako_clock::global(),
        spec: &mut spec,
        gate_held: false,
        ticket: 0,
        minted_stamp: 0,
        timestamp_stamp: 0,
        sequence: 0,
        bound: false,
        written: false,
        failure: None,
        record: ptr::null_mut(),
        holder: ptr::null_mut(),
        publication: ptr::null_mut(),
        observer: worker::observer(),
    };
    #[cfg(feature = "test-hooks")]
    let outcome = sto_core::test_install::with_observer(worker::observe_install, || {
        inner.commit_with_hook(&mut driver)
    });
    #[cfg(not(feature = "test-hooks"))]
    let outcome = inner.commit_with_hook(&mut driver);
    // The driver is dropped after `commit_with_hook` returns, so a gate still
    // held by a validation abort is released only after every lock is.
    let run = Run {
        report: map_commit(outcome, driver.failure),
        timestamp_stamp: driver.timestamp_stamp,
        sequence: driver.sequence,
        bound: driver.bound,
        written: driver.written,
        holder: driver.holder,
        publication: driver.publication,
    };
    drop(driver);
    run
}

fn unbound_record_report(report: CommitReport) -> CommitRecordReport {
    CommitRecordReport {
        commit: report,
        completion_contract_valid: true,
        record_bound: false,
        record_written: false,
    }
}

fn native_ordered_rejected(report: CommitReport) -> TrustedNativeOrderedOnePutRecordOutcome {
    TrustedNativeOrderedOnePutRecordOutcome {
        inner: TrustedUncheckedOnePutRecordOutcome {
            report,
            record_bound: false,
            record_written: false,
        },
        ordered_sequence: 0,
        timestamp_stamp: 0,
        target_bound: false,
        native_holder_ready: 0,
    }
}

type NoAcquire = fn(MakoTimestamp, CommitRecordPreflight, u64) -> Option<CommitRecordTarget>;

fn no_acquire() -> Option<NoAcquire> {
    None
}

fn valid_ring(base: *mut u8, mask: usize, shift: u32, stride: u32, extent: usize) -> bool {
    if base.is_null() || mask == usize::MAX || stride == 0 || extent == 0 {
        return false;
    }
    let capacity = mask + 1;
    if !capacity.is_power_of_two()
        || capacity < 4
        || shift >= usize::BITS
        || (1usize << shift) != capacity
    {
        return false;
    }
    mask.checked_mul(stride as usize)
        .and_then(|last| last.checked_add(extent))
        .is_some_and(|last_end| (base as usize).checked_add(last_end).is_some())
}

fn valid_arena_control(control: &TrustedNativeOrderedArenaControl, exact: usize) -> bool {
    if control.next_bound.is_null()
        || control.unhealthy.is_null()
        || control.arena_base.is_null()
        || !(control.next_bound as usize).is_multiple_of(std::mem::align_of::<u64>())
        || !(control.publication_base as usize).is_multiple_of(64)
        || !(control.arena_base as usize).is_multiple_of(64)
    {
        return false;
    }
    control.publication_stride == PUBLICATION_CELL_BYTES
        && control.arena_stride == ARENA_BLOCK_STRIDE
        && control.arena_block_bytes != 0
        && control.arena_block_bytes <= ARENA_BLOCK_STRIDE
        && exact != 0
        && exact <= control.arena_block_bytes as usize
        && exact <= FAST_PUT_RECORD_BYTES_MAX
        && valid_ring(
            control.publication_base,
            control.publication_mask,
            control.publication_shift,
            control.publication_stride,
            PUBLICATION_CELL_BYTES as usize,
        )
        && valid_ring(
            control.arena_base,
            control.publication_mask,
            control.publication_shift,
            control.arena_stride,
            control.arena_block_bytes as usize,
        )
}

fn valid_holder_control(control: &TrustedNativeOrderedHolderControl, exact: usize) -> bool {
    if control.pool.is_null()
        || control.unhealthy.is_null()
        || !(control.publication_base as usize).is_multiple_of(64)
    {
        return false;
    }
    // SAFETY: the control lends a live pool for the whole call.
    let pool = unsafe { &*control.pool };
    pool.capacity().is_power_of_two()
        && control.publication_mask == pool.capacity() - 1
        && control.publication_stride == PUBLICATION_CELL_BYTES
        && exact != 0
        && exact < NATIVE_HOLDER_RECORD_TAG
        && exact <= control.max_record_bytes as usize
        && exact <= FAST_PUT_RECORD_BYTES_MAX
        && valid_ring(
            control.publication_base,
            control.publication_mask,
            control.publication_shift,
            control.publication_stride,
            PUBLICATION_CELL_BYTES as usize,
        )
}

/// Publish the sealed holder's exact metadata and flip BOUND to READY.
///
/// # Safety
///
/// `publication` must be the cell bound for `sequence` by this transaction.
unsafe fn publish_holder_ready(
    publication: *mut u8,
    shift: u32,
    sequence: u64,
    timestamp_stamp: u64,
    exact: usize,
) {
    // SAFETY: the cell's turn word is an aligned `AtomicU64` at offset zero.
    let turn = unsafe { AtomicU64::from_ptr(publication.cast::<u64>()) };
    let bound = turn_token(sequence, shift, TURN_BOUND);
    let ready = turn_token(sequence, shift, TURN_READY);
    if turn.load(Ordering::Acquire) != bound || exact >= NATIVE_HOLDER_RECORD_TAG {
        std::process::abort();
    }
    // SAFETY: BOUND grants this generation's unique scalar writes.
    unsafe {
        publication
            .add(PUBLICATION_TIMESTAMP_OFFSET)
            .cast::<u64>()
            .write_unaligned(timestamp_stamp);
        publication
            .add(PUBLICATION_RECORD_BYTES_OFFSET)
            .cast::<usize>()
            .write_unaligned(NATIVE_HOLDER_RECORD_TAG | exact);
    }
    turn.store(ready, Ordering::Release);
}

// ---- the commit hook driver --------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum Gate {
    None,
    /// The process HLC certification gate.
    Hlc,
    /// The per-database FIFO ticket followed by the HLC gate.
    Ticket,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Order {
    /// The sequence comes from the caller (target or lane).
    RustSequence,
    /// The dense sequence is taken under the held gate after the timestamp.
    NativeLocked,
    /// Restricted `(timestamp, sequence)` pair after validation, no gate.
    RestrictedPair,
}

enum Sink {
    Callback,
    Preselected(CommitRecordTarget),
    Arena(TrustedNativeOrderedArenaControl),
    OrderedHolder(TrustedNativeOrderedHolderControl),
}

struct Spec<'a, F> {
    gate: Gate,
    gate_after_validation: bool,
    order: Order,
    unhealthy: Option<*const u8>,
    sink: Sink,
    acquire: Option<F>,
    preflight: CommitRecordPreflight,
    ops: &'a [OpRef<'a>],
    prepared_overflow_key: Vec<u8>,
}

struct Run {
    report: CommitReport,
    timestamp_stamp: u64,
    sequence: u64,
    bound: bool,
    written: bool,
    holder: *mut Holder,
    publication: *mut u8,
}

struct Driver<'a, 'b, F> {
    db: &'a LocalDb,
    clock: &'static Clock,
    spec: &'b mut Spec<'a, F>,
    gate_held: bool,
    ticket: u64,
    /// Stamp minted for this commit, before the terminal exposes it.
    minted_stamp: u64,
    /// Stamp exposed to the caller once the order was accepted.
    timestamp_stamp: u64,
    sequence: u64,
    bound: bool,
    written: bool,
    failure: Option<Error>,
    record: *mut u8,
    holder: *mut Holder,
    publication: *mut u8,
    observer: Option<TestCommitObserver>,
}

impl<F> Driver<'_, '_, F>
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight, u64) -> Option<CommitRecordTarget>,
{
    #[inline(always)]
    fn notify(&self, phase: TestCommitPhase, stamp: u64) {
        if let Some(observer) = self.observer {
            observer(phase, mako_clock::expand(stamp));
        }
    }

    #[inline(always)]
    fn unhealthy(&self) -> bool {
        match self.spec.unhealthy {
            // SAFETY: the terminal contract keeps the health byte live.
            Some(pointer) => {
                unsafe { AtomicU8::from_ptr(pointer.cast_mut()) }.load(Ordering::Acquire) != 0
            }
            None => false,
        }
    }

    fn enter_gate(&mut self) {
        debug_assert!(!self.gate_held);
        match self.spec.gate {
            Gate::None => return,
            Gate::Ticket => {
                let ticket = self.db.ticket_next.fetch_add(1, Ordering::Release);
                while self.db.ticket_serving.load(Ordering::Acquire) != ticket {
                    spin_loop();
                }
                self.ticket = ticket;
            }
            Gate::Hlc => {}
        }
        self.clock.enter_general();
        self.gate_held = true;
    }

    fn leave_gate(&mut self) {
        if !self.gate_held {
            return;
        }
        self.gate_held = false;
        self.clock.leave_general();
        if self.spec.gate == Gate::Ticket {
            self.db
                .ticket_serving
                .store(self.ticket + 1, Ordering::Release);
        }
    }

    #[cold]
    fn fail(&mut self, error: Error) -> std::result::Result<(), CommitHookError> {
        self.failure = Some(error);
        Err(CommitHookError::Rejected)
    }

    /// Serialize the record into `record`, which holds `exact` writable bytes.
    fn serialize(&mut self, record: *mut u8) -> bool {
        let exact = self.spec.preflight.exact_record_bytes();
        // SAFETY: the sink guarantees `record` is valid for `exact` bytes of
        // writes through this synchronous call.
        unsafe {
            record::encode_record(
                record,
                exact,
                self.sequence,
                self.minted_stamp,
                self.spec.preflight.checksum(),
                self.spec.ops,
            )
        }
    }

    fn pre_install_inner(&mut self) -> std::result::Result<(), CommitHookError> {
        match self.spec.order {
            Order::RestrictedPair => {
                if self.unhealthy() {
                    // Preserve timestamp-before-hook precedence on rejection.
                    if self.clock.try_allocate_timestamp().is_none() {
                        return self.fail(Error::TimestampExhausted);
                    }
                    return self.fail(Error::CommitHookRejected);
                }
                loop {
                    match self.clock.try_allocate_order_pair() {
                        OrderPairAllocation::Accepted { sequence, stamp } => {
                            self.minted_stamp = stamp;
                            self.sequence = sequence;
                            self.timestamp_stamp = stamp;
                            break;
                        }
                        OrderPairAllocation::GeneralLocked => spin_loop(),
                        OrderPairAllocation::TimestampExhausted => {
                            return self.fail(Error::TimestampExhausted);
                        }
                        OrderPairAllocation::SequenceExhausted => {
                            return self.fail(Error::CommitHookRejected);
                        }
                    }
                }
            }
            Order::RustSequence | Order::NativeLocked => {
                if self.spec.gate_after_validation {
                    self.enter_gate();
                }
                match self.clock.try_allocate_timestamp() {
                    Some(stamp) => self.minted_stamp = stamp,
                    None => return self.fail(Error::TimestampExhausted),
                }
            }
        }
        self.notify(TestCommitPhase::MakoTimestampAllocated, self.minted_stamp);
        let public = mako_clock::expand(self.minted_stamp).expect("minted stamps expand");

        // The bind hook runs under the gate.
        if self.spec.order == Order::NativeLocked {
            if self.unhealthy() {
                return self.fail(Error::CommitHookRejected);
            }
            match self.clock.try_allocate_locked_sequence() {
                Some(sequence) => {
                    self.sequence = sequence;
                    self.timestamp_stamp = self.minted_stamp;
                }
                None => return self.fail(Error::CommitHookRejected),
            }
        }
        match self.spec.sink {
            Sink::Callback if self.spec.order == Order::RustSequence => {
                let Some(acquire) = self.spec.acquire.take() else {
                    return self.fail(Error::CommitHookRejected);
                };
                let Some(target) = acquire(public, self.spec.preflight, 0) else {
                    return self.fail(Error::CommitHookRejected);
                };
                if target.exact_record_bytes() < self.spec.preflight.exact_record_bytes() {
                    return self.fail(Error::CommitHookRejected);
                }
                self.sequence = target.sequence().get();
                self.record = target.bytes().as_ptr();
                self.bound = true;
            }
            Sink::Preselected(target) => {
                self.sequence = target.sequence().get();
                if !self.serialize(target.bytes().as_ptr()) {
                    return self.fail(Error::CommitHookRejected);
                }
                self.timestamp_stamp = self.minted_stamp;
                self.bound = true;
                self.written = true;
            }
            _ => {}
        }
        // The ordered record is now bound; later transactions keep their own
        // locks while waiting, so release the gate before serialization.
        self.leave_gate();

        match self.spec.sink {
            Sink::Callback if self.spec.order == Order::RustSequence => {
                if !self.serialize(self.record) {
                    return self.fail(Error::CommitHookRejected);
                }
                self.written = true;
            }
            Sink::Callback => {
                let Some(acquire) = self.spec.acquire.take() else {
                    return self.fail(Error::CommitHookRejected);
                };
                let Some(target) = acquire(public, self.spec.preflight, self.sequence) else {
                    return self.fail(Error::CommitHookRejected);
                };
                if target.sequence().get() != self.sequence
                    || target.exact_record_bytes() < self.spec.preflight.exact_record_bytes()
                {
                    return self.fail(Error::CommitHookRejected);
                }
                self.record = target.bytes().as_ptr();
                self.bound = true;
                if !self.serialize(self.record) {
                    return self.fail(Error::CommitHookRejected);
                }
                self.written = true;
            }
            Sink::Arena(control) => {
                // SAFETY: the terminal validated the layout before commit and
                // the dense sequence names a retired FREE generation.
                unsafe { self.bind_arena(&control) };
                if !self.serialize(self.record) {
                    return self.fail(Error::CommitHookRejected);
                }
                self.written = true;
            }
            Sink::OrderedHolder(control) => {
                // SAFETY: as above, plus the holder pool is live.
                unsafe { self.bind_ordered_holder(&control) };
            }
            Sink::Preselected(_) => {}
        }
        #[cfg(feature = "test-hooks")]
        worker::prepare_install_event(
            self.observer.filter(|_| self.spec.ops.len() > 1),
            Some(public),
        );
        self.notify(TestCommitPhase::PreinstallAccepted, self.minted_stamp);
        Ok(())
    }

    /// Acquire the exact FREE cell for `self.sequence`, publish BOUND, and
    /// select the arena block. An impossible cell state terminates.
    unsafe fn bind_arena(&mut self, control: &TrustedNativeOrderedArenaControl) {
        let sequence = self.sequence;
        if sequence == 0
            || control.publication_shift < TURN_PHASE_BITS
            || self.spec.preflight.exact_record_bytes() > control.arena_block_bytes as usize
        {
            std::process::abort();
        }
        let index = (sequence as usize) & control.publication_mask;
        // SAFETY: the validated ring layout covers `index`.
        let publication = unsafe {
            control
                .publication_base
                .add(index * control.publication_stride as usize)
        };
        let record = unsafe {
            control
                .arena_base
                .add(index * control.arena_stride as usize)
        };
        let turn = unsafe { AtomicU64::from_ptr(publication.cast::<u64>()) };
        let free = turn_token(sequence, control.publication_shift, TURN_FREE);
        let bound = turn_token(sequence, control.publication_shift, TURN_BOUND);
        if turn.load(Ordering::Acquire) != free {
            std::process::abort();
        }
        unsafe {
            publication
                .add(PUBLICATION_RECORD_BYTES_OFFSET)
                .cast::<usize>()
                .write_unaligned(0);
        }
        turn.store(bound, Ordering::Release);
        self.record = record;
        self.publication = publication;
        self.bound = true;
    }

    /// Acquire the exact FREE cell, stage the key into the FREE holder, and
    /// publish BOUND. Value transfer and READY follow after the core returns.
    unsafe fn bind_ordered_holder(&mut self, control: &TrustedNativeOrderedHolderControl) {
        let sequence = self.sequence;
        let exact = self.spec.preflight.exact_record_bytes();
        if sequence == 0
            || control.publication_shift < TURN_PHASE_BITS
            || exact == 0
            || exact > control.max_record_bytes as usize
        {
            std::process::abort();
        }
        let index = (sequence as usize) & control.publication_mask;
        // SAFETY: the validated ring layout covers `index`; the pool is live.
        let publication = unsafe {
            control
                .publication_base
                .add(index * control.publication_stride as usize)
        };
        let turn = unsafe { AtomicU64::from_ptr(publication.cast::<u64>()) };
        let free = turn_token(sequence, control.publication_shift, TURN_FREE);
        let bound = turn_token(sequence, control.publication_shift, TURN_BOUND);
        if turn.load(Ordering::Acquire) != free {
            std::process::abort();
        }
        let pool = unsafe { &*control.pool };
        let holder_ptr = pool.holder_ptr(sequence);
        let holder = unsafe { &mut *holder_ptr };
        if !holder.is_free() {
            std::process::abort();
        }
        let key = self.spec.ops[0].key;
        if key.len() > crate::holder::INLINE_KEY_BYTES {
            if self.spec.prepared_overflow_key.len() != key.len() {
                std::process::abort();
            }
            holder.adopt_overflow_key(std::mem::take(&mut self.spec.prepared_overflow_key));
        } else {
            holder.stage_inline_key(key);
        }
        unsafe {
            publication
                .add(PUBLICATION_RECORD_BYTES_OFFSET)
                .cast::<usize>()
                .write_unaligned(0);
        }
        turn.store(bound, Ordering::Release);
        self.publication = publication;
        self.holder = holder_ptr;
        self.bound = true;
    }
}

impl<F> CommitHook for Driver<'_, '_, F>
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight, u64) -> Option<CommitRecordTarget>,
{
    fn reserve_upper_metadata(&mut self) -> std::result::Result<(), CommitHookError> {
        self.notify(TestCommitPhase::WritesetLocked, 0);
        if self.spec.order == Order::RestrictedPair {
            crate::prefetch::restricted_clock(self.clock);
        }
        if self.spec.gate != Gate::None && !self.spec.gate_after_validation {
            self.enter_gate();
        }
        Ok(())
    }

    fn pre_install(&mut self) -> std::result::Result<(), CommitHookError> {
        self.notify(TestCommitPhase::LocalValidationComplete, 0);
        self.pre_install_inner()
    }

    fn post_install(&mut self) {
        self.notify(TestCommitPhase::AllWritesInstalled, self.minted_stamp);
    }
}

impl<F> Drop for Driver<'_, '_, F> {
    fn drop(&mut self) {
        #[cfg(feature = "test-hooks")]
        worker::prepare_install_event(None, None);
        // A validation abort never reaches pre_install; release the gate only
        // now, after the core has released every write lock.
        if self.gate_held {
            self.gate_held = false;
            self.clock.leave_general();
            if self.spec.gate == Gate::Ticket {
                self.db
                    .ticket_serving
                    .store(self.ticket + 1, Ordering::Release);
            }
        }
    }
}

// ---- scans ---------------------------------------------------------------

/// A fallible transactional range iterator over `[lower, upper)`.
pub struct Scan<'txn, 'db> {
    transaction: &'txn mut Transaction<'db>,
    table: Table<'db>,
    direction: ScanDirection,
    lower: Vec<u8>,
    upper: Option<Vec<u8>>,
    resume: Option<Vec<u8>>,
    scratch: ScanScratch,
    current: std::vec::IntoIter<(Vec<u8>, Vec<u8>)>,
    done: bool,
}

impl<'txn, 'db> Scan<'txn, 'db> {
    fn new(
        transaction: &'txn mut Transaction<'db>,
        table: Table<'db>,
        direction: ScanDirection,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Self> {
        transaction.check_active()?;
        transaction.check_table(&table)?;
        if lower.len() > MAX_KEY_BYTES || upper.is_some_and(|bound| bound.len() > MAX_KEY_BYTES) {
            return Err(Error::ValueTooLarge);
        }
        transaction.has_predicate = true;
        transaction.invalidate_borrowed_witness();
        let done = upper.is_some_and(|upper| lower >= upper);
        Ok(Self {
            transaction,
            table,
            direction,
            lower: lower.to_vec(),
            upper: upper.map(<[u8]>::to_vec),
            resume: None,
            scratch: ScanScratch::default(),
            current: Vec::new().into_iter(),
            done,
        })
    }

    fn key_is_valid(&self, key: &[u8], previous: Option<&[u8]>) -> bool {
        if key < self.lower.as_slice() || self.upper.as_deref().is_some_and(|upper| key >= upper) {
            return false;
        }
        match (self.direction, previous) {
            (ScanDirection::Forward, Some(previous)) => key > previous,
            (ScanDirection::Reverse, Some(previous)) => key < previous,
            (_, None) => true,
        }
    }

    fn refill(&mut self) -> Result<()> {
        let used = self.transaction.budget_used;
        if used > TRANSACTION_ITEM_BUDGET {
            self.done = true;
            return self.transaction.fail_closed(Error::TransactionTooLarge);
        }
        let remaining = TRANSACTION_ITEM_BUDGET - used;
        let limit = SCAN_CHUNK_ENTRIES.min(remaining + 1);
        let mut request = ScanRequest::new(self.direction, limit);
        match self.direction {
            ScanDirection::Forward => {
                request = request
                    .with_lower(match self.resume.as_deref() {
                        Some(resume) => ScanBound::Excluded(resume),
                        None => ScanBound::Included(&self.lower),
                    })
                    .with_upper(match self.upper.as_deref() {
                        Some(upper) => ScanBound::Excluded(upper),
                        None => ScanBound::Unbounded,
                    });
            }
            ScanDirection::Reverse => {
                request = request
                    .with_lower(ScanBound::Included(&self.lower))
                    .with_upper(match (self.resume.as_deref(), self.upper.as_deref()) {
                        (Some(resume), _) => ScanBound::Excluded(resume),
                        (None, Some(upper)) => ScanBound::Excluded(upper),
                        (None, None) => ScanBound::Unbounded,
                    });
            }
        }
        let mut items: Vec<(Vec<u8>, Vec<u8>)> = Vec::new();
        let worker = self.transaction.attachment.worker;
        let outcome = self.table.entry.table.visit_scan_bytes_with_scratch(
            self.transaction.inner_mut(),
            worker,
            request,
            &mut self.scratch,
            |row| {
                items.push((row.key().to_vec(), row.value().to_vec()));
                ScanControl::Continue
            },
        );
        let outcome = match outcome {
            Ok(outcome) => outcome,
            Err(error) => {
                self.done = true;
                return self.transaction.fail_access(error);
            }
        };
        if outcome.visited() > remaining {
            self.done = true;
            return self.transaction.fail_closed(Error::TransactionTooLarge);
        }
        self.transaction.budget_used += outcome.visited();
        let mut previous = self.resume.clone();
        for (key, _) in &items {
            if key.len() > MAX_KEY_BYTES || !self.key_is_valid(key, previous.as_deref()) {
                self.done = true;
                return self.transaction.fail_closed(Error::Internal);
            }
            previous = Some(key.clone());
        }
        let table_id = self.table.entry.id;
        for (key, _) in &items {
            let index = self.transaction.touch(table_id, key);
            let entry = &mut self.transaction.touched[index];
            entry.read = true;
            entry.existed_before.get_or_insert(true);
        }
        self.done = outcome.visited() < limit || items.len() < limit;
        if let Some((last, _)) = items.last() {
            self.resume = Some(last.clone());
        }
        self.current = items.into_iter();
        Ok(())
    }
}

impl Iterator for Scan<'_, '_> {
    type Item = Result<(Vec<u8>, Vec<u8>)>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(item) = self.current.next() {
            return Some(Ok(item));
        }
        if self.done {
            return None;
        }
        match self.refill() {
            Ok(()) => self.current.next().map(Ok),
            Err(error) => {
                self.done = true;
                Some(Err(error))
            }
        }
    }
}

// ---- the contract impl ---------------------------------------------------

impl<'db> mako_engine_api::EngineTransaction<'db> for Transaction<'db> {
    type Engine = crate::MakoStoEngine;
    type Table = Table<'db>;
    type Scan<'txn>
        = Scan<'txn, 'db>
    where
        Self: 'txn;

    fn get(&mut self, table: &Self::Table, key: &[u8]) -> Result<Option<Vec<u8>>> {
        Self::get(self, table, key)
    }
    fn put(&mut self, table: &Self::Table, key: &[u8], value: &[u8]) -> Result<bool> {
        Self::put(self, table, key, value)
    }
    fn insert(&mut self, table: &Self::Table, key: &[u8], value: &[u8]) -> Result<bool> {
        Self::insert(self, table, key, value)
    }
    fn remove(&mut self, table: &Self::Table, key: &[u8]) -> Result<bool> {
        Self::remove(self, table, key)
    }
    fn scan<'txn>(
        &'txn mut self,
        table: &Self::Table,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Self::Scan<'txn>> {
        Self::scan(self, table, lower, upper)
    }
    fn rscan<'txn>(
        &'txn mut self,
        table: &Self::Table,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Self::Scan<'txn>> {
        Self::rscan(self, table, lower, upper)
    }
    fn abort(self) -> Result<()> {
        Self::abort(self)
    }
    fn commit(self) -> Result<()> {
        Self::commit(self)
    }
    fn commit_report(self) -> CommitReport {
        Self::commit_report(self)
    }
    fn commit_record_preflight_with_checksum(
        &mut self,
        max_record_bytes: usize,
        checksum: CommitRecordChecksum,
    ) -> Result<CommitRecordPreflight> {
        Self::commit_record_preflight_with_checksum(self, max_record_bytes, checksum)
    }
    fn unchecked_one_put_record_candidate(&self) -> Option<CommitRecordPreflight> {
        Self::unchecked_one_put_record_candidate(self)
    }
    unsafe fn trusted_unchecked_one_put_record_bytes_candidate(&self) -> Option<NonZeroU32> {
        // SAFETY: forwarded unchanged.
        unsafe { Self::trusted_unchecked_one_put_record_bytes_candidate(self) }
    }
    unsafe fn commit_report_with_record_target<F>(self, acquire: F) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        // SAFETY: forwarded unchanged.
        unsafe { Self::commit_report_with_record_target(self, acquire) }
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
            Self::commit_report_with_unchecked_one_put_record_target(self, candidate, acquire)
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
            Self::commit_trusted_native_ordered_unchecked_one_put_record_target(
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
            Self::commit_trusted_native_ordered_unchecked_one_put_arena(self, candidate, control)
        }
    }
    unsafe fn commit_trusted_native_ordered_unchecked_one_put_holder(
        self,
        candidate: CommitRecordPreflight,
        control: &TrustedNativeOrderedHolderControl,
    ) -> TrustedNativeOrderedOnePutRecordOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Self::commit_trusted_native_ordered_unchecked_one_put_holder(self, candidate, control)
        }
    }
    unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
        self,
        candidate: CommitRecordPreflight,
        target: CommitRecordTarget,
    ) -> TrustedPreselectedUncheckedOnePutRecordOutcome {
        // SAFETY: forwarded unchanged.
        unsafe {
            Self::commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
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
            Self::commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
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
            Self::commit_trusted_preselected_per_worker_unchecked_one_put_holder_bytes(
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
        cold_attempt: &mut MaybeUninit<TrustedFusedOnePutHolderAttempt>,
    ) -> bool {
        // SAFETY: forwarded unchanged.
        unsafe {
            Self::try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
                self,
                control,
                producer_next,
                capacity_limit,
                cold_attempt,
            )
        }
    }
}
