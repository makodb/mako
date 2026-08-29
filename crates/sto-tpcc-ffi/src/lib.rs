#![allow(clippy::missing_safety_doc)]
//! Panic-contained C ABI used by Mako's shared TPC-C workload adapter.
//!
//! The surface deliberately exposes only opaque owning handles and byte
//! slices. `StoTpccThread` is thread-affine and contains a lifetime-erased STO
//! transaction whose actual borrow is tied to a boxed `WorkerContext`; the
//! invariant and the only lifetime-erasing operation are documented below.

use masstree::{Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig, Worker};
use std::{
    cell::RefCell,
    fmt::{self, Write as _},
    mem,
    panic::{catch_unwind, AssertUnwindSafe},
    ptr, slice,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    thread::{self, ThreadId},
};
use sto_core::{
    AbortReason, AccessError, Active, CommitFailure, CommitOutcome, DefiniteOutcome, Runtime,
    RuntimeConfig, Transaction, WorkerContext,
};
use sto_masstree::{InsertOutcome, ScanBound, ScanDirection, ScanRequest, Table, TableConfig};

const ERROR_CAPACITY: usize = 1_024;

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Status {
    Ok = 0,
    Miss = 1,
    Duplicate = 2,
    Retry = 3,
    BufferTooSmall = 4,
    Fatal = 5,
}

impl Status {
    const fn code(self) -> i32 {
        self as i32
    }
}

type FfiResult<T> = Result<T, Status>;

struct ErrorBuffer {
    bytes: [u8; ERROR_CAPACITY],
    len: usize,
}

impl ErrorBuffer {
    const fn new() -> Self {
        Self {
            bytes: [0; ERROR_CAPACITY],
            len: 0,
        }
    }

    fn clear(&mut self) {
        self.len = 0;
    }

    fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
}

impl fmt::Write for ErrorBuffer {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        let available = self.bytes.len().saturating_sub(self.len);
        let mut copied = available.min(text.len());
        while copied != 0 && !text.is_char_boundary(copied) {
            copied -= 1;
        }
        self.bytes[self.len..self.len + copied].copy_from_slice(&text.as_bytes()[..copied]);
        self.len += copied;
        Ok(())
    }
}

thread_local! {
    static LAST_ERROR: RefCell<ErrorBuffer> = const { RefCell::new(ErrorBuffer::new()) };
    // `thread::current()` clones Rust's thread descriptor, including an Arc
    // increment/decrement. Cache its globally unique ID once per OS thread so
    // the mandatory affinity check remains fail-closed without paying that
    // refcount traffic on every TPC-C point operation.
    static CURRENT_THREAD_ID: ThreadId = thread::current().id();
}

#[inline(always)]
fn current_thread_id() -> ThreadId {
    CURRENT_THREAD_ID.with(|id| *id)
}

#[cfg(test)]
fn clear_last_error() {
    LAST_ERROR.with(|slot| {
        if let Ok(mut error) = slot.try_borrow_mut() {
            error.clear();
        }
    });
}

fn set_last_error(arguments: fmt::Arguments<'_>) {
    LAST_ERROR.with(|slot| {
        if let Ok(mut error) = slot.try_borrow_mut() {
            error.clear();
            let _ = error.write_fmt(arguments);
        }
    });
}

fn fatal(arguments: fmt::Arguments<'_>) -> Status {
    set_last_error(arguments);
    Status::Fatal
}

fn boundary(operation: &'static str, body: impl FnOnce() -> FfiResult<Status>) -> i32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(status) | Err(status)) => status.code(),
        Err(_) => {
            set_last_error(format_args!(
                "{operation}: contained an unexpected Rust panic"
            ));
            Status::Fatal.code()
        }
    }
}

fn boundary_preserving_error(
    operation: &'static str,
    body: impl FnOnce() -> FfiResult<Status>,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(status) | Err(status)) => status.code(),
        Err(_) => {
            set_last_error(format_args!(
                "{operation}: contained an unexpected Rust panic"
            ));
            Status::Fatal.code()
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct StoTpccDbConfig {
    pub max_threads: u32,
    pub max_key_length: u32,
    pub max_items_per_txn: usize,
    pub max_locks_per_txn: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct StoTpccTableConfig {
    pub max_retained_records: u64,
    pub max_retained_key_bytes: u64,
    pub max_consumed_record_ids: u64,
    pub scan_chunk_records: usize,
    pub scan_initial_key_arena_bytes: usize,
    pub scan_max_key_arena_bytes: usize,
    pub max_scan_chunks: usize,
    pub max_scan_physical_records: usize,
}

pub struct StoTpccDb {
    sto: Arc<Runtime>,
    masstree: MasstreeRuntime,
}

struct TableState {
    table: Table,
    logical_rows: AtomicU64,
}

pub struct StoTpccTable {
    state: Arc<TableState>,
}

struct PendingSizeDelta {
    table: Arc<TableState>,
    delta: i64,
}

type ActiveTransaction = Transaction<'static, Active>;

pub struct StoTpccThread {
    // This field must be destroyed before `sto_worker`; the explicit Drop
    // implementation also takes and aborts it before field destruction.
    active: Option<ActiveTransaction>,
    sto_worker: Box<WorkerContext>,
    native_worker: Worker,
    owner: ThreadId,
    // A small linear vector avoids allocating a hash table in every TPC-C
    // transaction. Capacity is retained across attempts.
    pending_size: Vec<PendingSizeDelta>,
}

impl StoTpccThread {
    #[inline(always)]
    fn ensure_owner(&self) -> FfiResult<()> {
        if self.owner == current_thread_id() {
            Ok(())
        } else {
            Err(fatal(format_args!(
                "thread handle used or destroyed from a different OS thread"
            )))
        }
    }

    fn record_size_delta(&mut self, table: &Arc<TableState>, delta: i64) -> FfiResult<()> {
        if let Some(pending) = self
            .pending_size
            .iter_mut()
            .find(|pending| Arc::ptr_eq(&pending.table, table))
        {
            pending.delta = pending
                .delta
                .checked_add(delta)
                .ok_or_else(|| fatal(format_args!("transactional table-size delta overflow")))?;
            return Ok(());
        }

        self.pending_size.try_reserve(1).map_err(|_| {
            fatal(format_args!(
                "unable to reserve transactional table-size accounting"
            ))
        })?;
        self.pending_size.push(PendingSizeDelta {
            table: Arc::clone(table),
            delta,
        });
        Ok(())
    }

    fn apply_size_deltas(&self) -> FfiResult<()> {
        for pending in &self.pending_size {
            adjust_logical_rows(&pending.table.logical_rows, pending.delta)?;
        }
        Ok(())
    }
}

fn active_transaction(active: &mut Option<ActiveTransaction>) -> FfiResult<&mut ActiveTransaction> {
    active.as_mut().ok_or_else(|| {
        fatal(format_args!(
            "transactional operation requires an active transaction"
        ))
    })
}

impl Drop for StoTpccThread {
    fn drop(&mut self) {
        if let Some(transaction) = self.active.take() {
            let _ = transaction.abort();
        }
        self.pending_size.clear();
    }
}

fn adjust_logical_rows(rows: &AtomicU64, delta: i64) -> FfiResult<()> {
    if delta == 0 {
        return Ok(());
    }
    let mut current = rows.load(Ordering::Relaxed);
    loop {
        let next = if delta > 0 {
            current.checked_add(delta as u64)
        } else {
            current.checked_sub(delta.unsigned_abs())
        }
        .ok_or_else(|| {
            fatal(format_args!(
                "committed logical table-size invariant failed"
            ))
        })?;
        match rows.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return Ok(()),
            Err(observed) => current = observed,
        }
    }
}

fn status_from_access(operation: &str, error: AccessError) -> Status {
    let retry = matches!(
        error,
        AccessError::Conflict(_) | AccessError::InvalidUse(sto_core::InvalidUse::TransactionDoomed)
    );
    set_last_error(format_args!("{operation}: {error}"));
    if retry {
        Status::Retry
    } else {
        Status::Fatal
    }
}

fn status_from_abort(reason: AbortReason) -> Status {
    let retry = matches!(reason, AbortReason::Doomed | AbortReason::Conflict(_));
    set_last_error(format_args!("transaction commit aborted: {reason}"));
    if retry {
        Status::Retry
    } else {
        Status::Fatal
    }
}

unsafe fn required_ref<'a, T>(pointer: *const T, name: &str) -> FfiResult<&'a T> {
    if pointer.is_null() {
        return Err(fatal(format_args!("{name} must not be null")));
    }
    // SAFETY: The public C contract requires a live, aligned handle/config
    // pointer of the corresponding type for the duration of the call.
    Ok(unsafe { &*pointer })
}

unsafe fn required_mut<'a, T>(pointer: *mut T, name: &str) -> FfiResult<&'a mut T> {
    if pointer.is_null() {
        return Err(fatal(format_args!("{name} must not be null")));
    }
    // SAFETY: The public C contract requires exclusive access to the live,
    // aligned handle/output pointer for the duration of the call.
    Ok(unsafe { &mut *pointer })
}

unsafe fn bytes<'a>(pointer: *const u8, length: usize, name: &str) -> FfiResult<&'a [u8]> {
    if length == 0 {
        return Ok(&[]);
    }
    if pointer.is_null() {
        return Err(fatal(format_args!(
            "{name} must not be null when its length is nonzero"
        )));
    }
    // SAFETY: The C caller guarantees `length` readable bytes and no mutation
    // for the duration of the operation.
    Ok(unsafe { slice::from_raw_parts(pointer, length) })
}

unsafe fn output_bytes<'a>(
    pointer: *mut u8,
    capacity: usize,
    name: &str,
) -> FfiResult<&'a mut [u8]> {
    if capacity == 0 {
        return Ok(&mut []);
    }
    if pointer.is_null() {
        return Err(fatal(format_args!(
            "{name} must not be null when its capacity is nonzero"
        )));
    }
    // SAFETY: The C caller guarantees `capacity` uniquely writable bytes for
    // the duration of the operation.
    Ok(unsafe { slice::from_raw_parts_mut(pointer, capacity) })
}

fn db_config(raw: StoTpccDbConfig) -> (RuntimeConfig, MasstreeRuntimeConfig) {
    let mut sto = RuntimeConfig::new();
    let mut native = MasstreeRuntimeConfig::new();
    if raw.max_threads != 0 {
        sto = sto.with_max_workers(raw.max_threads as usize);
        native = native.with_max_threads(raw.max_threads);
    }
    if raw.max_key_length != 0 {
        native = native.with_max_key_length(raw.max_key_length);
    }
    if raw.max_items_per_txn != 0 {
        sto = sto.with_max_items_per_transaction(raw.max_items_per_txn);
    }
    if raw.max_locks_per_txn != 0 {
        sto = sto.with_max_locks_per_transaction(raw.max_locks_per_txn);
    }
    (sto, native)
}

fn table_config(raw: StoTpccTableConfig) -> TableConfig {
    let mut config = TableConfig::new();
    if raw.max_retained_records != 0 {
        config = config.with_max_retained_records(raw.max_retained_records);
    }
    if raw.max_retained_key_bytes != 0 {
        config = config.with_max_retained_key_bytes(raw.max_retained_key_bytes);
    }
    if raw.max_consumed_record_ids != 0 {
        config = config.with_max_consumed_record_ids(raw.max_consumed_record_ids);
    }
    if raw.scan_chunk_records != 0 {
        config = config.with_scan_chunk_records(raw.scan_chunk_records);
    }
    if raw.scan_initial_key_arena_bytes != 0 {
        config = config.with_scan_initial_key_arena_bytes(raw.scan_initial_key_arena_bytes);
    }
    if raw.scan_max_key_arena_bytes != 0 {
        config = config.with_scan_max_key_arena_bytes(raw.scan_max_key_arena_bytes);
    }
    if raw.max_scan_chunks != 0 {
        config = config.with_max_scan_chunks(raw.max_scan_chunks);
    }
    if raw.max_scan_physical_records != 0 {
        config = config.with_max_scan_physical_records(raw.max_scan_physical_records);
    }
    config
}

unsafe fn optional_copy<T: Copy + Default>(pointer: *const T) -> T {
    if pointer.is_null() {
        T::default()
    } else {
        // SAFETY: A non-null optional config obeys the same validity contract
        // as required config pointers.
        unsafe { *pointer }
    }
}

/// # Safety
/// `out_db` must be uniquely writable. A non-null `config` must be readable.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_db_create(
    config: *const StoTpccDbConfig,
    out_db: *mut *mut StoTpccDb,
) -> i32 {
    boundary("sto_tpcc_db_create", || {
        let output = unsafe { required_mut(out_db, "out_db")? };
        *output = ptr::null_mut();
        let raw = unsafe { optional_copy(config) };
        let (sto_config, masstree_config) = db_config(raw);
        let masstree = MasstreeRuntime::new(masstree_config)
            .map_err(|error| fatal(format_args!("unable to create Masstree runtime: {error}")))?;
        let sto = Runtime::new(sto_config)
            .map_err(|error| fatal(format_args!("unable to create STO runtime: {error}")))?;
        *output = Box::into_raw(Box::new(StoTpccDb { sto, masstree }));
        Ok(Status::Ok)
    })
}

/// # Safety
/// `db` must be null or a live handle returned by `sto_tpcc_db_create`, and it
/// must be destroyed exactly once.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_db_destroy(db: *mut StoTpccDb) -> i32 {
    boundary("sto_tpcc_db_destroy", || {
        if !db.is_null() {
            // SAFETY: Ownership of this allocation is returned exactly once by
            // the caller under the destruction contract.
            drop(unsafe { Box::from_raw(db) });
        }
        Ok(Status::Ok)
    })
}

/// # Safety
/// Handles must be live, and `out_table` must be uniquely writable. Create
/// tables before attaching a transaction handle on this OS thread.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_create(
    db: *mut StoTpccDb,
    config: *const StoTpccTableConfig,
    out_table: *mut *mut StoTpccTable,
) -> i32 {
    boundary("sto_tpcc_table_create", || {
        let db = unsafe { required_ref(db, "db")? };
        let output = unsafe { required_mut(out_table, "out_table")? };
        *output = ptr::null_mut();
        let raw = unsafe { optional_copy(config) };
        let worker = db.masstree.attach().map_err(|error| {
            fatal(format_args!(
                "unable to attach Masstree table creator: {error}"
            ))
        })?;
        let tree = db
            .masstree
            .create_tree(&worker)
            .map_err(|error| fatal(format_args!("unable to create Masstree table: {error}")))?;
        drop(worker);
        let table = Table::new(&db.sto, tree, table_config(raw))
            .map_err(|error| fatal(format_args!("unable to register STO table: {error}")))?;
        *output = Box::into_raw(Box::new(StoTpccTable {
            state: Arc::new(TableState {
                table,
                logical_rows: AtomicU64::new(0),
            }),
        }));
        Ok(Status::Ok)
    })
}

/// # Safety
/// `table` must be null or a live table handle and must be destroyed once. No
/// transaction may concurrently use it.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_destroy(table: *mut StoTpccTable) -> i32 {
    boundary("sto_tpcc_table_destroy", || {
        if !table.is_null() {
            // SAFETY: Ownership is returned exactly once by contract.
            drop(unsafe { Box::from_raw(table) });
        }
        Ok(Status::Ok)
    })
}

/// # Safety
/// `table` must be live and `out_rows` uniquely writable.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_size(
    table: *const StoTpccTable,
    out_rows: *mut u64,
) -> i32 {
    boundary("sto_tpcc_table_size", || {
        let table = unsafe { required_ref(table, "table")? };
        let output = unsafe { required_mut(out_rows, "out_rows")? };
        *output = table.state.logical_rows.load(Ordering::Relaxed);
        Ok(Status::Ok)
    })
}

/// # Safety
/// `db` must be live and `out_thread` uniquely writable. The returned handle
/// must remain on this OS thread.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_thread_create(
    db: *mut StoTpccDb,
    out_thread: *mut *mut StoTpccThread,
) -> i32 {
    boundary("sto_tpcc_thread_create", || {
        let db = unsafe { required_ref(db, "db")? };
        let output = unsafe { required_mut(out_thread, "out_thread")? };
        *output = ptr::null_mut();
        let native_worker = db
            .masstree
            .attach()
            .map_err(|error| fatal(format_args!("unable to attach Masstree worker: {error}")))?;
        let sto_worker = db
            .sto
            .attach()
            .map_err(|error| fatal(format_args!("unable to attach STO worker: {error}")))?;
        *output = Box::into_raw(Box::new(StoTpccThread {
            active: None,
            sto_worker: Box::new(sto_worker),
            native_worker,
            owner: current_thread_id(),
            pending_size: Vec::new(),
        }));
        Ok(Status::Ok)
    })
}

/// # Safety
/// `thread_handle` must be null or a live handle, on its creating OS thread,
/// and must be destroyed exactly once. An active transaction is aborted.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_thread_destroy(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_thread_destroy", || {
        if thread_handle.is_null() {
            return Ok(Status::Ok);
        }
        // Check affinity before reclaiming the allocation: dropping either
        // worker on the wrong thread would violate its native contract.
        let handle = unsafe { required_ref(thread_handle, "thread")? };
        handle.ensure_owner()?;
        // SAFETY: After the affinity check, ownership is returned once by the
        // caller and Drop aborts any active transaction before its worker.
        drop(unsafe { Box::from_raw(thread_handle) });
        Ok(Status::Ok)
    })
}

/// # Safety
/// `thread_handle` must be a live, exclusively accessed, same-thread handle.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_txn_begin(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_txn_begin", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        if handle.active.is_some() {
            return Err(fatal(format_args!("a transaction is already active")));
        }
        handle.pending_size.clear();
        let transaction = handle
            .sto_worker
            .begin()
            .map_err(|error| fatal(format_args!("unable to begin transaction: {error}")))?;
        // SAFETY: `sto_worker` is boxed, so moving the outer handle never moves
        // the borrowed pointee. `active` is consumed or dropped before that box
        // in every path, and no code accesses the worker mutably while active.
        let transaction =
            unsafe { mem::transmute::<Transaction<'_, Active>, ActiveTransaction>(transaction) };
        handle.active = Some(transaction);
        Ok(Status::Ok)
    })
}

/// # Safety
/// `thread_handle` must be a live, exclusively accessed, same-thread handle.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_txn_commit(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_txn_commit", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let transaction = handle
            .active
            .take()
            .ok_or_else(|| fatal(format_args!("no transaction is active")))?;
        let outcome = transaction.commit();
        match outcome {
            Ok(CommitOutcome::Committed(_)) => {
                let size_result = handle.apply_size_deltas();
                handle.pending_size.clear();
                size_result?;
                Ok(Status::Ok)
            }
            Ok(CommitOutcome::Aborted(reason)) => {
                handle.pending_size.clear();
                Ok(status_from_abort(reason))
            }
            Err(error @ CommitFailure::Poisoned { outcome, .. }) => {
                if matches!(outcome, DefiniteOutcome::Committed(_)) {
                    let _ = handle.apply_size_deltas();
                }
                handle.pending_size.clear();
                Err(fatal(format_args!("transaction commit failed: {error}")))
            }
            Err(error @ CommitFailure::Indeterminate(_)) => {
                handle.pending_size.clear();
                Err(fatal(format_args!(
                    "transaction outcome is indeterminate: {error}"
                )))
            }
        }
    })
}

/// # Safety
/// `thread_handle` must be a live, exclusively accessed, same-thread handle.
/// The operation is intentionally idempotent when no transaction is active.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_txn_abort(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_txn_abort", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        if let Some(transaction) = handle.active.take() {
            let _ = transaction.abort();
        }
        handle.pending_size.clear();
        Ok(Status::Ok)
    })
}

/// # Safety
/// Handles and byte ranges must be valid for the call. `out_actual` is
/// required; `out_value` must cover `value_capacity` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_get(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    out_value: *mut u8,
    value_capacity: usize,
    out_actual: *mut usize,
) -> i32 {
    boundary("sto_tpcc_get", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let output = unsafe { output_bytes(out_value, value_capacity, "out_value")? };
        let actual = unsafe { required_mut(out_actual, "out_actual")? };
        *actual = 0;
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        match table.state.table.get(transaction, native_worker, key) {
            Ok(None) => Ok(Status::Miss),
            Ok(Some(value)) => {
                *actual = value.len();
                if output.len() < value.len() {
                    set_last_error(format_args!(
                        "value buffer is too small: need {}, have {}",
                        value.len(),
                        output.len()
                    ));
                    return Ok(Status::BufferTooSmall);
                }
                output[..value.len()].copy_from_slice(&value);
                Ok(Status::Ok)
            }
            Err(error) => Ok(status_from_access("get", error)),
        }
    })
}

/// # Safety
/// Handles and both input byte ranges must be valid for the call.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_put(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("sto_tpcc_put", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let value = unsafe { bytes(value, value_length, "value")? };
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        match table
            .state
            .table
            .put(transaction, native_worker, key, value)
        {
            Ok(previous) => {
                if previous.is_none() {
                    handle.record_size_delta(&table.state, 1)?;
                }
                Ok(Status::Ok)
            }
            Err(error) => Ok(status_from_access("put", error)),
        }
    })
}

/// # Safety
/// Handles and both input byte ranges must be valid for the call.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_insert(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("sto_tpcc_insert", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let value = unsafe { bytes(value, value_length, "value")? };
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        match table
            .state
            .table
            .insert(transaction, native_worker, key, value)
        {
            Ok(InsertOutcome::Inserted) => {
                handle.record_size_delta(&table.state, 1)?;
                Ok(Status::Ok)
            }
            Ok(InsertOutcome::AlreadyPresent(_)) => Ok(Status::Duplicate),
            Err(error) => Ok(status_from_access("insert", error)),
        }
    })
}

/// # Safety
/// Handles and the key byte range must be valid for the call.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_remove(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
) -> i32 {
    boundary("sto_tpcc_remove", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        match table.state.table.remove(transaction, native_worker, key) {
            Ok(Some(_)) => {
                handle.record_size_delta(&table.state, -1)?;
                Ok(Status::Ok)
            }
            Ok(None) => Ok(Status::Miss),
            Err(error) => Ok(status_from_access("remove", error)),
        }
    })
}

pub type StoTpccScanCallback = unsafe extern "C" fn(
    context: *mut std::ffi::c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32;

fn scan_bound<'a>(kind: i32, key: &'a [u8], name: &str) -> FfiResult<ScanBound<'a>> {
    match kind {
        0 => Ok(ScanBound::Unbounded),
        1 => Ok(ScanBound::Included(key)),
        2 => Ok(ScanBound::Excluded(key)),
        _ => Err(fatal(format_args!("invalid {name} bound kind {kind}"))),
    }
}

/// # Safety
/// All handles/ranges and `out_visited` must be valid. `callback` must not
/// unwind, retain row pointers, or re-enter operations on `thread_handle`.
#[allow(clippy::too_many_arguments)]
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_scan(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    direction: i32,
    lower_kind: i32,
    lower_key: *const u8,
    lower_key_length: usize,
    upper_kind: i32,
    upper_key: *const u8,
    upper_key_length: usize,
    limit: usize,
    callback: Option<StoTpccScanCallback>,
    callback_context: *mut std::ffi::c_void,
    out_visited: *mut usize,
) -> i32 {
    boundary("sto_tpcc_scan", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let lower_key = if lower_kind == 0 {
            &[][..]
        } else {
            unsafe { bytes(lower_key, lower_key_length, "lower_key")? }
        };
        let upper_key = if upper_kind == 0 {
            &[][..]
        } else {
            unsafe { bytes(upper_key, upper_key_length, "upper_key")? }
        };
        let lower = scan_bound(lower_kind, lower_key, "lower")?;
        let upper = scan_bound(upper_kind, upper_key, "upper")?;
        let direction = match direction {
            0 => ScanDirection::Forward,
            1 => ScanDirection::Reverse,
            other => {
                return Err(fatal(format_args!("invalid scan direction {other}")));
            }
        };
        let callback = callback.ok_or_else(|| fatal(format_args!("callback must not be null")))?;
        let visited = unsafe { required_mut(out_visited, "out_visited")? };
        *visited = 0;
        let request = ScanRequest::new(direction, limit)
            .with_lower(lower)
            .with_upper(upper);
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        let records = match table.state.table.scan(transaction, native_worker, request) {
            Ok(records) => records,
            Err(error) => return Ok(status_from_access("scan", error)),
        };
        for record in records {
            // SAFETY: The callback is a valid function pointer by the ABI. Row
            // slices remain alive for this invocation and the header forbids
            // retaining them or unwinding through Rust.
            let stop = unsafe {
                callback(
                    callback_context,
                    record.key().as_ptr(),
                    record.key().len(),
                    record.value().as_ptr(),
                    record.value().len(),
                )
            };
            *visited += 1;
            if stop != 0 {
                break;
            }
        }
        Ok(Status::Ok)
    })
}

/// Returns the current thread's error-message byte length, excluding NUL.
#[no_mangle]
pub extern "C" fn sto_tpcc_last_error_length() -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        LAST_ERROR.with(|slot| slot.try_borrow().map_or(0, |error| error.len))
    }))
    .unwrap_or(0)
}

/// # Safety
/// `out_actual` must be uniquely writable. A nonzero capacity requires that
/// `out_message` cover that many writable bytes.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_last_error_copy(
    out_message: *mut std::ffi::c_char,
    message_capacity: usize,
    out_actual: *mut usize,
) -> i32 {
    boundary_preserving_error("sto_tpcc_last_error_copy", || {
        if out_actual.is_null() {
            return Err(fatal(format_args!("out_actual must not be null")));
        }
        let snapshot = LAST_ERROR.with(|slot| {
            let error = slot.try_borrow().map_err(|_| Status::Fatal)?;
            let mut snapshot = ErrorBuffer::new();
            snapshot.bytes[..error.len].copy_from_slice(error.as_bytes());
            snapshot.len = error.len;
            Ok::<_, Status>(snapshot)
        })?;
        // SAFETY: Validity and exclusivity are required by the C contract.
        unsafe { *out_actual = snapshot.len };
        let required = snapshot.len.saturating_add(1);
        if message_capacity < required {
            return Ok(Status::BufferTooSmall);
        }
        if out_message.is_null() {
            return Err(fatal(format_args!(
                "out_message must not be null when its capacity is nonzero"
            )));
        }
        // SAFETY: Capacity was checked and the caller supplies a uniquely
        // writable message buffer.
        unsafe {
            ptr::copy_nonoverlapping(snapshot.bytes.as_ptr(), out_message.cast(), snapshot.len);
            *out_message.add(snapshot.len) = 0;
        }
        Ok(Status::Ok)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stable_status_numbers_match_header() {
        assert_eq!(Status::Ok.code(), 0);
        assert_eq!(Status::Miss.code(), 1);
        assert_eq!(Status::Duplicate.code(), 2);
        assert_eq!(Status::Retry.code(), 3);
        assert_eq!(Status::BufferTooSmall.code(), 4);
        assert_eq!(Status::Fatal.code(), 5);
    }

    #[test]
    fn last_error_is_bounded_and_utf8() {
        clear_last_error();
        set_last_error(format_args!("{}", "é".repeat(ERROR_CAPACITY)));
        LAST_ERROR.with(|slot| {
            let error = slot.borrow();
            assert!(error.len <= ERROR_CAPACITY);
            assert!(std::str::from_utf8(error.as_bytes()).is_ok());
        });
    }

    #[test]
    fn cached_thread_id_is_stable_and_distinct() {
        let owner = current_thread_id();
        assert_eq!(owner, current_thread_id());
        let other = thread::spawn(current_thread_id).join().unwrap();
        assert_ne!(owner, other);
    }
}
