//! Pure-Rust scripted stand-in for the native ABI.
//!
//! This module is compiled only into `mako-local`'s unit-test harness. Its
//! functions intentionally use Rust linkage: Miri can execute every pointer and
//! ownership path without crossing an FFI boundary or linking the C++ archive.

#![allow(dead_code)]

use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::OnceLock;

use super::sys;

static ENGINE_ID: &[u8] = b"mako-local/sto-masstrans\0";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Call {
    TableOpen,
    Begin,
    Get,
    Put,
    Insert,
    Remove,
    Scan,
    ReverseScan,
    Commit,
    CommitWithHook,
    Abort,
    Destroy,
    BytesFree,
    DbClose,
}

#[derive(Debug)]
pub(super) struct GetReply {
    pub status: c_int,
    pub bytes: Option<Vec<u8>>,
    pub reported_len: usize,
    pub found: u8,
}

impl GetReply {
    pub fn absent() -> Self {
        Self {
            status: sys::MAKO_LOCAL_OK,
            bytes: None,
            reported_len: 0,
            found: 0,
        }
    }

    pub fn present(bytes: Vec<u8>) -> Self {
        let reported_len = bytes.len();
        Self {
            status: sys::MAKO_LOCAL_OK,
            bytes: Some(bytes),
            reported_len,
            found: 1,
        }
    }
}

#[derive(Debug)]
pub(super) struct ByteReply {
    pub status: c_int,
    pub value: u8,
}

impl ByteReply {
    pub fn status(status: c_int) -> Self {
        Self { status, value: 0 }
    }
}

#[derive(Debug)]
pub(super) struct ScanReply {
    pub status: c_int,
    pub entries: Vec<sys::mako_local_scan_entry>,
    pub arena: Vec<u8>,
    pub entry_count: usize,
    pub arena_used: usize,
    pub arena_required: usize,
    pub done: u8,
}

impl ScanReply {
    pub fn empty() -> Self {
        Self {
            status: sys::MAKO_LOCAL_OK,
            entries: Vec::new(),
            arena: Vec::new(),
            entry_count: 0,
            arena_used: 0,
            arena_required: 0,
            done: 1,
        }
    }
}

#[derive(Debug)]
pub(super) enum Step {
    TableOpen {
        status: c_int,
        return_handle: bool,
    },
    Begin {
        status: c_int,
        return_handle: bool,
    },
    Get(GetReply),
    Put(ByteReply),
    Insert(ByteReply),
    Remove(ByteReply),
    Scan(ScanReply),
    ReverseScan(ScanReply),
    Commit(c_int),
    CommitWithHook {
        status: c_int,
        timestamp: Option<u32>,
    },
    Abort(c_int),
    Destroy(c_int),
}

#[derive(Debug, Default)]
struct FakeDb;

#[derive(Debug)]
struct FakeTable {
    id: u64,
}

#[derive(Debug, Default)]
struct FakeTxn;

#[derive(Default)]
struct State {
    attached: bool,
    poisoned: bool,
    db: Option<Box<FakeDb>>,
    table: Option<Box<FakeTable>>,
    txn: Option<Box<FakeTxn>>,
    foreign_bytes: Vec<Box<[u8]>>,
    steps: VecDeque<Step>,
    calls: Vec<Call>,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

static QUARANTINED_WORKERS: AtomicU64 = AtomicU64::new(0);
static STATUS_STRINGS: OnceLock<Vec<CString>> = OnceLock::new();
static UNKNOWN_STATUS: &[u8] = b"unknown status\0";

fn with_state<T>(f: impl FnOnce(&mut State) -> T) -> T {
    STATE.with(|state| f(&mut state.borrow_mut()))
}

fn poison(state: &mut State) {
    if !state.poisoned {
        state.poisoned = true;
        QUARANTINED_WORKERS.fetch_add(1, Ordering::Relaxed);
    }
}

fn observe_status(state: &mut State, status: c_int) {
    if status == sys::MAKO_LOCAL_WORKER_POISONED {
        poison(state);
    }
}

fn unexpected(expected: &str, found: Option<Step>) -> ! {
    panic!("fake ABI expected {expected}, found {found:?}")
}

pub(super) fn reset() {
    STATE.with(|state| *state.borrow_mut() = State::default());
}

pub(super) fn push(step: Step) {
    with_state(|state| state.steps.push_back(step));
}

pub(super) fn calls() -> Vec<Call> {
    STATE.with(|state| state.borrow().calls.clone())
}

pub(super) fn assert_drained() {
    STATE.with(|state| {
        let state = state.borrow();
        assert!(
            state.steps.is_empty(),
            "unconsumed fake steps: {:?}",
            state.steps
        );
        assert!(
            state.foreign_bytes.is_empty(),
            "wrapper leaked {} fake byte allocations",
            state.foreign_bytes.len()
        );
        assert!(
            state.txn.is_none() || state.poisoned,
            "a healthy fake worker retained a transaction facade"
        );
    });
}

pub(super) unsafe fn mako_local_abi_version() -> u32 {
    sys::MAKO_LOCAL_ABI_VERSION
}

pub(super) unsafe fn mako_local_engine_id() -> *const c_char {
    ENGINE_ID.as_ptr().cast()
}

pub(super) unsafe fn mako_local_build_fingerprint() -> *const u8 {
    super::identity_abi::EXPECTED_BUILD_FINGERPRINT.as_ptr()
}

pub(super) unsafe fn mako_local_build_fingerprint_size() -> usize {
    super::identity_abi::EXPECTED_BUILD_FINGERPRINT.len()
}

pub(super) unsafe fn mako_local_feature_bits() -> u64 {
    sys::MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS
        | sys::MAKO_LOCAL_FEATURE_READ_MY_WRITES
        | sys::MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS
        | sys::MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES
}

pub(super) unsafe fn mako_local_scan_options_size() -> usize {
    sys::MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE as usize
}

pub(super) unsafe fn mako_local_scan_entry_size() -> usize {
    std::mem::size_of::<sys::mako_local_scan_entry>()
}

pub(super) unsafe fn mako_local_status_string(status: c_int) -> *const c_char {
    let strings = STATUS_STRINGS.get_or_init(|| {
        sys::ALL_KNOWN_STATUSES
            .iter()
            .map(|status| CString::new(status.message()).expect("status messages contain no NUL"))
            .collect()
    });
    usize::try_from(status)
        .ok()
        .and_then(|index| strings.get(index))
        .map_or_else(
            || UNKNOWN_STATUS.as_ptr().cast::<c_char>(),
            |message| message.as_ptr(),
        )
}

pub(super) unsafe fn mako_local_thread_attach() -> c_int {
    with_state(|state| {
        if state.poisoned {
            sys::MAKO_LOCAL_WORKER_POISONED
        } else {
            state.attached = true;
            sys::MAKO_LOCAL_OK
        }
    })
}

pub(super) unsafe fn mako_local_worker_health() -> c_int {
    STATE.with(|state| {
        let state = state.borrow();
        if state.poisoned {
            sys::MAKO_LOCAL_WORKER_POISONED
        } else if state.attached {
            sys::MAKO_LOCAL_OK
        } else {
            sys::MAKO_LOCAL_NOT_ATTACHED
        }
    })
}

pub(super) unsafe fn mako_local_quarantined_worker_count() -> u64 {
    QUARANTINED_WORKERS.load(Ordering::Relaxed)
}

pub(super) unsafe fn mako_local_test_set_commit_observer(
    _observer: sys::mako_local_test_commit_observer,
    _context: *mut c_void,
) -> c_int {
    sys::MAKO_LOCAL_FEATURE_UNAVAILABLE
}

pub(super) unsafe fn mako_local_test_clear_commit_observer() -> c_int {
    sys::MAKO_LOCAL_FEATURE_UNAVAILABLE
}

pub(super) unsafe fn mako_local_test_arm_cleanup_failure(_boundary: u32) -> c_int {
    sys::MAKO_LOCAL_FEATURE_UNAVAILABLE
}

pub(super) unsafe fn mako_local_test_clear_cleanup_failure() -> c_int {
    sys::MAKO_LOCAL_FEATURE_UNAVAILABLE
}

pub(super) unsafe fn mako_local_advance_mako_timestamp_past(observed: u32) -> c_int {
    if observed == 0 || observed > sys::MAKO_LOCAL_MAX_MAKO_TIMESTAMP {
        sys::MAKO_LOCAL_INVALID_ARGUMENT
    } else if observed == sys::MAKO_LOCAL_MAX_MAKO_TIMESTAMP {
        sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED
    } else {
        sys::MAKO_LOCAL_OK
    }
}

pub(super) unsafe fn mako_local_db_open(out: *mut *mut sys::mako_local_db) -> c_int {
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: caller supplied the required writable output pointer.
    unsafe { out.write(ptr::null_mut()) };
    with_state(|state| {
        let mut db = Box::new(FakeDb);
        let raw = ptr::from_mut(db.as_mut()).cast::<sys::mako_local_db>();
        state.db = Some(db);
        // SAFETY: checked above; the allocation remains owned by fake state.
        unsafe { out.write(raw) };
        sys::MAKO_LOCAL_OK
    })
}

pub(super) unsafe fn mako_local_db_close(db: *mut sys::mako_local_db) -> c_int {
    with_state(|state| {
        state.calls.push(Call::DbClose);
        if db.is_null() {
            return sys::MAKO_LOCAL_OK;
        }
        if state.txn.is_some() {
            return sys::MAKO_LOCAL_BUSY;
        }
        state.table = None;
        state.db = None;
        sys::MAKO_LOCAL_OK
    })
}

pub(super) unsafe fn mako_local_table_open(
    _db: *mut sys::mako_local_db,
    _name: *const u8,
    _name_len: usize,
    table_id: u64,
    out: *mut *mut sys::mako_local_table,
) -> c_int {
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: caller supplied the required writable output pointer.
    unsafe { out.write(ptr::null_mut()) };
    with_state(|state| {
        state.calls.push(Call::TableOpen);
        if state.poisoned {
            return sys::MAKO_LOCAL_WORKER_POISONED;
        }
        let (status, return_handle) = match state.steps.front() {
            Some(Step::TableOpen { .. }) => match state.steps.pop_front() {
                Some(Step::TableOpen {
                    status,
                    return_handle,
                }) => (status, return_handle),
                found => unexpected("table open", found),
            },
            _ => (sys::MAKO_LOCAL_OK, true),
        };
        observe_status(state, status);
        if status != sys::MAKO_LOCAL_OK || !return_handle {
            return status;
        }
        let table = state
            .table
            .get_or_insert_with(|| Box::new(FakeTable { id: table_id }));
        if table.id != table_id {
            return sys::MAKO_LOCAL_WRONG_DB_OR_TABLE;
        }
        let raw = ptr::from_mut(table.as_mut()).cast::<sys::mako_local_table>();
        // SAFETY: checked above; the allocation remains owned by fake state.
        unsafe { out.write(raw) };
        sys::MAKO_LOCAL_OK
    })
}

pub(super) unsafe fn mako_local_table_id(table: *const sys::mako_local_table) -> u64 {
    if table.is_null() {
        return 0;
    }
    // SAFETY: fake table handles point at a live `FakeTable` allocation.
    unsafe { (*table.cast::<FakeTable>()).id }
}

pub(super) unsafe fn mako_local_txn_begin(
    _db: *mut sys::mako_local_db,
    out: *mut *mut sys::mako_local_txn,
) -> c_int {
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: caller supplied the required writable output pointer.
    unsafe { out.write(ptr::null_mut()) };
    with_state(|state| {
        state.calls.push(Call::Begin);
        if state.poisoned {
            return sys::MAKO_LOCAL_WORKER_POISONED;
        }
        let (status, return_handle) = match state.steps.front() {
            Some(Step::Begin { .. }) => match state.steps.pop_front() {
                Some(Step::Begin {
                    status,
                    return_handle,
                }) => (status, return_handle),
                found => unexpected("begin", found),
            },
            _ => (sys::MAKO_LOCAL_OK, true),
        };
        observe_status(state, status);
        if status == sys::MAKO_LOCAL_OK && return_handle {
            let mut txn = Box::new(FakeTxn);
            let raw = ptr::from_mut(txn.as_mut()).cast::<sys::mako_local_txn>();
            state.txn = Some(txn);
            // SAFETY: checked above; the allocation remains owned by fake state.
            unsafe { out.write(raw) };
        }
        status
    })
}

pub(super) unsafe fn mako_local_txn_get(
    _txn: *mut sys::mako_local_txn,
    _table: *mut sys::mako_local_table,
    _key: *const u8,
    _key_len: usize,
    value_out: *mut *mut u8,
    value_len_out: *mut usize,
    found_out: *mut u8,
) -> c_int {
    assert!(!value_out.is_null() && !value_len_out.is_null() && !found_out.is_null());
    // SAFETY: assertions above establish all required writable outputs.
    unsafe {
        value_out.write(ptr::null_mut());
        value_len_out.write(0);
        found_out.write(0);
    }
    with_state(|state| {
        state.calls.push(Call::Get);
        let reply = match state.steps.pop_front() {
            Some(Step::Get(reply)) => reply,
            found => unexpected("get", found),
        };
        observe_status(state, reply.status);
        let raw = reply.bytes.map(|mut bytes| {
            if bytes.is_empty() {
                bytes.push(0);
            }
            state.foreign_bytes.push(bytes.into_boxed_slice());
            state
                .foreign_bytes
                .last_mut()
                .expect("allocation was just registered")
                .as_mut_ptr()
        });
        // SAFETY: outputs were checked above and fake allocations remain live.
        unsafe {
            value_out.write(raw.unwrap_or(ptr::null_mut()));
            value_len_out.write(reply.reported_len);
            found_out.write(reply.found);
        }
        reply.status
    })
}

fn byte_operation(expected: &str, call: Call, select: fn(Step) -> Option<ByteReply>) -> ByteReply {
    with_state(|state| {
        state.calls.push(call);
        let reply = state
            .steps
            .pop_front()
            .and_then(select)
            .unwrap_or_else(|| panic!("fake ABI expected {expected}"));
        observe_status(state, reply.status);
        reply
    })
}

pub(super) unsafe fn mako_local_txn_put(
    _txn: *mut sys::mako_local_txn,
    _table: *mut sys::mako_local_table,
    _key: *const u8,
    _key_len: usize,
    _value: *const u8,
    _value_len: usize,
    created_out: *mut u8,
) -> c_int {
    assert!(!created_out.is_null());
    // SAFETY: checked above.
    unsafe { created_out.write(0) };
    let reply = byte_operation("put", Call::Put, |step| match step {
        Step::Put(reply) => Some(reply),
        _ => None,
    });
    // SAFETY: checked above.
    unsafe { created_out.write(reply.value) };
    reply.status
}

pub(super) unsafe fn mako_local_txn_insert(
    _txn: *mut sys::mako_local_txn,
    _table: *mut sys::mako_local_table,
    _key: *const u8,
    _key_len: usize,
    _value: *const u8,
    _value_len: usize,
    inserted_out: *mut u8,
) -> c_int {
    assert!(!inserted_out.is_null());
    // SAFETY: checked above.
    unsafe { inserted_out.write(0) };
    let reply = byte_operation("insert", Call::Insert, |step| match step {
        Step::Insert(reply) => Some(reply),
        _ => None,
    });
    // SAFETY: checked above.
    unsafe { inserted_out.write(reply.value) };
    reply.status
}

pub(super) unsafe fn mako_local_txn_remove(
    _txn: *mut sys::mako_local_txn,
    _table: *mut sys::mako_local_table,
    _key: *const u8,
    _key_len: usize,
    existed_out: *mut u8,
) -> c_int {
    assert!(!existed_out.is_null());
    // SAFETY: checked above.
    unsafe { existed_out.write(0) };
    let reply = byte_operation("remove", Call::Remove, |step| match step {
        Step::Remove(reply) => Some(reply),
        _ => None,
    });
    // SAFETY: checked above.
    unsafe { existed_out.write(reply.value) };
    reply.status
}

#[allow(clippy::too_many_arguments)]
unsafe fn scan_operation(
    reverse: bool,
    entries: *mut sys::mako_local_scan_entry,
    entries_capacity: usize,
    arena: *mut u8,
    arena_capacity: usize,
    entry_count_out: *mut usize,
    arena_used_out: *mut usize,
    arena_required_out: *mut usize,
    done_out: *mut u8,
) -> c_int {
    assert!(
        !entry_count_out.is_null()
            && !arena_used_out.is_null()
            && !arena_required_out.is_null()
            && !done_out.is_null()
    );
    // SAFETY: assertions above establish all required writable outputs.
    unsafe {
        entry_count_out.write(0);
        arena_used_out.write(0);
        arena_required_out.write(0);
        done_out.write(0);
    }
    with_state(|state| {
        state.calls.push(if reverse {
            Call::ReverseScan
        } else {
            Call::Scan
        });
        let reply = match state.steps.pop_front() {
            Some(Step::Scan(reply)) if !reverse => reply,
            Some(Step::ReverseScan(reply)) if reverse => reply,
            found => unexpected(if reverse { "reverse scan" } else { "scan" }, found),
        };
        observe_status(state, reply.status);

        let entry_writes = reply.entries.len().min(entries_capacity);
        if entry_writes != 0 {
            assert!(!entries.is_null());
            // SAFETY: the fake never writes beyond caller-provided capacity.
            unsafe { ptr::copy_nonoverlapping(reply.entries.as_ptr(), entries, entry_writes) };
        }
        let arena_writes = reply.arena.len().min(arena_capacity);
        if arena_writes != 0 {
            assert!(!arena.is_null());
            // SAFETY: the fake never writes beyond caller-provided capacity.
            unsafe { ptr::copy_nonoverlapping(reply.arena.as_ptr(), arena, arena_writes) };
        }
        // SAFETY: required outputs were checked above.
        unsafe {
            entry_count_out.write(reply.entry_count);
            arena_used_out.write(reply.arena_used);
            arena_required_out.write(reply.arena_required);
            done_out.write(reply.done);
        }
        reply.status
    })
}

#[allow(clippy::too_many_arguments)]
pub(super) unsafe fn mako_local_txn_scan_chunk(
    _txn: *mut sys::mako_local_txn,
    _table: *mut sys::mako_local_table,
    _options: *const sys::mako_local_scan_options,
    entries: *mut sys::mako_local_scan_entry,
    entries_capacity: usize,
    arena: *mut u8,
    arena_capacity: usize,
    entry_count_out: *mut usize,
    arena_used_out: *mut usize,
    arena_required_out: *mut usize,
    done_out: *mut u8,
) -> c_int {
    // SAFETY: forwarded exactly from the safe wrapper's caller-owned buffers.
    unsafe {
        scan_operation(
            false,
            entries,
            entries_capacity,
            arena,
            arena_capacity,
            entry_count_out,
            arena_used_out,
            arena_required_out,
            done_out,
        )
    }
}

#[allow(clippy::too_many_arguments)]
pub(super) unsafe fn mako_local_txn_rscan_chunk(
    _txn: *mut sys::mako_local_txn,
    _table: *mut sys::mako_local_table,
    _options: *const sys::mako_local_scan_options,
    entries: *mut sys::mako_local_scan_entry,
    entries_capacity: usize,
    arena: *mut u8,
    arena_capacity: usize,
    entry_count_out: *mut usize,
    arena_used_out: *mut usize,
    arena_required_out: *mut usize,
    done_out: *mut u8,
) -> c_int {
    // SAFETY: forwarded exactly from the safe wrapper's caller-owned buffers.
    unsafe {
        scan_operation(
            true,
            entries,
            entries_capacity,
            arena,
            arena_capacity,
            entry_count_out,
            arena_used_out,
            arena_required_out,
            done_out,
        )
    }
}

pub(super) unsafe fn mako_local_txn_commit(_txn: *mut sys::mako_local_txn) -> c_int {
    with_state(|state| {
        state.calls.push(Call::Commit);
        let status = match state.steps.pop_front() {
            Some(Step::Commit(status)) => status,
            found => unexpected("commit", found),
        };
        observe_status(state, status);
        status
    })
}

pub(super) unsafe fn mako_local_txn_commit_with_hook(
    _txn: *mut sys::mako_local_txn,
    hook: sys::mako_local_post_validate_hook,
    context: *mut c_void,
) -> c_int {
    with_state(|state| {
        state.calls.push(Call::CommitWithHook);
        let (status, timestamp) = match state.steps.pop_front() {
            Some(Step::CommitWithHook { status, timestamp }) => (status, timestamp),
            found => unexpected("commit with hook", found),
        };
        if let (Some(hook), Some(timestamp)) = (hook, timestamp) {
            // SAFETY: the safe wrapper supplied this synchronous callback and
            // stack context for the duration of the current call.
            let _ = unsafe { hook(context, timestamp) };
        }
        observe_status(state, status);
        status
    })
}

pub(super) unsafe fn mako_local_txn_abort(_txn: *mut sys::mako_local_txn) -> c_int {
    with_state(|state| {
        state.calls.push(Call::Abort);
        let status = match state.steps.pop_front() {
            Some(Step::Abort(status)) => status,
            found => unexpected("abort", found),
        };
        observe_status(state, status);
        status
    })
}

pub(super) unsafe fn mako_local_txn_destroy(_txn: *mut sys::mako_local_txn) -> c_int {
    with_state(|state| {
        state.calls.push(Call::Destroy);
        let status = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("destroy", found),
        };
        observe_status(state, status);
        if status == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        status
    })
}

pub(super) unsafe fn mako_local_bytes_free(bytes: *mut c_void) {
    if bytes.is_null() {
        return;
    }
    with_state(|state| {
        state.calls.push(Call::BytesFree);
        let position = state
            .foreign_bytes
            .iter()
            .position(|allocation| allocation.as_ptr().cast::<c_void>() == bytes)
            .expect("fake byte allocation freed exactly once by its owner");
        state.foreign_bytes.swap_remove(position);
    });
}

#[cfg(test)]
mod tests {
    use std::thread;

    use super::*;
    use crate::{
        arm_test_cleanup_failure, clear_test_cleanup_failure, clear_test_commit_observer, features,
        install_test_commit_observer, quarantined_worker_count, worker_health, CommitDisposition,
        Error, LocalDb, TestCleanupBoundary, TestCommitPhase, WorkerHealth, MAX_VALUE_BYTES,
    };

    fn unavailable_commit_observer(_phase: TestCommitPhase, _timestamp: u32) {
        panic!("unavailable fake commit observer must not be invoked");
    }

    fn open_db() -> LocalDb {
        LocalDb::open().expect("fake database opens")
    }

    fn assert_call_count(call: Call, expected: usize) {
        assert_eq!(
            calls().into_iter().filter(|found| *found == call).count(),
            expected,
            "unexpected {call:?} call count in {:?}",
            calls()
        );
    }

    fn exercise_operation_statuses() {
        let statuses = sys::ALL_KNOWN_STATUSES
            .iter()
            .map(|status| status.code())
            .chain([7_777]);

        for code in statuses {
            reset();
            let db = open_db();
            let table = db.open_table("status-matrix", 1).unwrap();
            let mut transaction = db.transaction().unwrap();
            push(Step::Put(ByteReply::status(code)));

            let effect = crate::operation_effect(code);
            match effect {
                crate::OperationEffect::Active => {
                    push(Step::Abort(sys::MAKO_LOCAL_OK));
                    push(Step::Destroy(sys::MAKO_LOCAL_OK));
                }
                crate::OperationEffect::Finished
                | crate::OperationEffect::Quarantined
                | crate::OperationEffect::Uncertain => {
                    let destroy = if code == sys::MAKO_LOCAL_WORKER_POISONED {
                        sys::MAKO_LOCAL_WORKER_POISONED
                    } else {
                        sys::MAKO_LOCAL_OK
                    };
                    push(Step::Destroy(destroy));
                }
            }

            let result = transaction.put(&table, b"key", b"value");
            let expected = match sys::KnownStatus::from_code(code) {
                Some(sys::KnownStatus::Ok) => Ok(false),
                Some(status) => Err(crate::known_status(status).unwrap_err()),
                None => Err(Error::UnknownStatus(code)),
            };
            assert_eq!(result, expected, "status {code}");

            if effect != crate::OperationEffect::Active {
                assert_eq!(
                    transaction.put(&table, b"no-native-retry", b"value"),
                    Err(Error::TransactionFinished)
                );
            }
            drop(transaction);

            assert_call_count(Call::Put, 1);
            assert_call_count(
                Call::Abort,
                usize::from(effect == crate::OperationEffect::Active),
            );
            assert_call_count(Call::Destroy, 1);
            drop(db);
            assert_drained();
        }
    }

    fn exercise_worker_health_and_drop_quarantine() {
        reset();
        assert_eq!(worker_health(), Ok(WorkerHealth::NotAttached));
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        assert_eq!(worker_health(), Ok(WorkerHealth::Healthy));
        let _table = db.open_table("drop-quarantine", 2).unwrap();
        let transaction = db.transaction().unwrap();
        push(Step::Abort(sys::MAKO_LOCAL_WORKER_POISONED));
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));
        drop(transaction);

        assert_eq!(worker_health(), Ok(WorkerHealth::Poisoned));
        assert_eq!(quarantined_worker_count().unwrap(), before + 1);
        assert!(matches!(db.transaction(), Err(Error::WorkerPoisoned)));
        assert_call_count(Call::Begin, 1);
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);

        let other = thread::spawn(worker_health).join().unwrap();
        assert_eq!(other, Ok(WorkerHealth::NotAttached));

        drop(db);
        assert_drained();
    }

    fn exercise_fake_capabilities_are_honest() {
        reset();
        let capabilities = features().unwrap();
        assert!(!capabilities.test_commit_observer());
        assert!(!capabilities.test_cleanup_failures());
        assert_eq!(
            install_test_commit_observer(unavailable_commit_observer),
            Err(Error::FeatureUnavailable)
        );
        assert_eq!(clear_test_commit_observer(), Err(Error::FeatureUnavailable));
        assert_eq!(
            arm_test_cleanup_failure(TestCleanupBoundary::Begin),
            Err(Error::FeatureUnavailable)
        );
        assert_eq!(clear_test_cleanup_failure(), Err(Error::FeatureUnavailable));
        assert_drained();
    }

    fn exercise_commit_and_cleanup_independence() {
        let statuses = sys::ALL_KNOWN_STATUSES
            .iter()
            .map(|status| status.code())
            .chain([-909]);
        for code in statuses {
            reset();
            let db = open_db();
            let transaction = db.transaction().unwrap();
            push(Step::Commit(code));
            let destroy = if code == sys::MAKO_LOCAL_WORKER_POISONED {
                sys::MAKO_LOCAL_WORKER_POISONED
            } else {
                sys::MAKO_LOCAL_OK
            };
            push(Step::Destroy(destroy));

            let report = transaction.commit_report();
            assert_eq!(report.disposition, crate::commit_disposition(code));
            assert_eq!(
                report.cleanup,
                if destroy == sys::MAKO_LOCAL_OK {
                    Ok(())
                } else {
                    Err(Error::WorkerPoisoned)
                }
            );
            assert_call_count(Call::Commit, 1);
            assert_call_count(Call::Abort, 0);
            assert_call_count(Call::Destroy, 1);
            drop(db);
            assert_drained();
        }

        reset();
        let db = open_db();
        let transaction = db.transaction().unwrap();
        push(Step::Commit(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));
        let report = transaction.commit_report();
        assert_eq!(report.disposition, CommitDisposition::Committed);
        assert_eq!(report.cleanup, Err(Error::WorkerPoisoned));
        assert_eq!(worker_health(), Ok(WorkerHealth::Poisoned));
        assert_call_count(Call::Commit, 1);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_commit_with_hook_stack_callback() {
        reset();
        let db = open_db();
        let transaction = db.transaction().unwrap();
        push(Step::CommitWithHook {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(41),
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));

        let mut callback_count = 0;
        let report = transaction.commit_report_with_hook(|timestamp| {
            callback_count += 1;
            assert_eq!(timestamp.get(), 41);
            true
        });
        assert_eq!(callback_count, 1);
        assert_eq!(report.disposition, CommitDisposition::Committed);
        assert_eq!(report.cleanup, Ok(()));
        assert_call_count(Call::CommitWithHook, 1);
        assert_call_count(Call::Commit, 0);
        assert_call_count(Call::Abort, 0);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_explicit_abort_paths() {
        reset();
        let db = open_db();
        let transaction = db.transaction().unwrap();
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(transaction.abort(), Ok(()));
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        assert_eq!(worker_health(), Ok(WorkerHealth::Healthy));
        drop(db);
        assert_drained();

        reset();
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        let transaction = db.transaction().unwrap();
        push(Step::Abort(sys::MAKO_LOCAL_WORKER_POISONED));
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));
        assert_eq!(transaction.abort(), Err(Error::WorkerPoisoned));
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        assert_eq!(worker_health(), Ok(WorkerHealth::Poisoned));
        assert_eq!(quarantined_worker_count().unwrap(), before + 1);
        assert!(matches!(db.transaction(), Err(Error::WorkerPoisoned)));
        assert_call_count(Call::Begin, 1);
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_malformed_get_outputs() {
        let cases = [
            GetReply {
                status: sys::MAKO_LOCAL_OK,
                bytes: Some(vec![b'x']),
                reported_len: 1,
                found: 2,
            },
            GetReply {
                status: sys::MAKO_LOCAL_OK,
                bytes: Some(Vec::new()),
                reported_len: 0,
                found: 0,
            },
            GetReply {
                status: sys::MAKO_LOCAL_OK,
                bytes: None,
                reported_len: 1,
                found: 0,
            },
            GetReply {
                status: sys::MAKO_LOCAL_OK,
                bytes: None,
                reported_len: 0,
                found: 1,
            },
            GetReply {
                status: sys::MAKO_LOCAL_OK,
                bytes: Some(vec![0; MAX_VALUE_BYTES + 1]),
                reported_len: MAX_VALUE_BYTES + 1,
                found: 1,
            },
        ];

        for reply in cases {
            reset();
            let db = open_db();
            let table = db.open_table("malformed-get", 3).unwrap();
            let mut transaction = db.transaction().unwrap();
            push(Step::Get(reply));
            push(Step::Abort(sys::MAKO_LOCAL_OK));
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            assert_eq!(transaction.get(&table, b"key"), Err(Error::Internal));
            assert_eq!(
                transaction.get(&table, b"no-native-retry"),
                Err(Error::TransactionFinished)
            );
            drop(transaction);
            assert_call_count(Call::Get, 1);
            assert_call_count(Call::Abort, 1);
            assert_call_count(Call::Destroy, 1);
            drop(db);
            assert_drained();
        }

        reset();
        let db = open_db();
        let table = db.open_table("owned-get", 4).unwrap();
        let mut transaction = db.transaction().unwrap();
        push(Step::Get(GetReply::present(Vec::new())));
        push(Step::Get(GetReply::present(b"owned-value".to_vec())));
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(transaction.get(&table, b"empty"), Ok(Some(Vec::new())));
        assert_call_count(Call::BytesFree, 1);
        assert_eq!(
            transaction.get(&table, b"nonempty"),
            Ok(Some(b"owned-value".to_vec()))
        );
        assert_call_count(Call::Get, 2);
        assert_call_count(Call::BytesFree, 2);
        drop(transaction);
        drop(db);
        assert_drained();
    }

    fn exercise_malformed_boolean_outputs() {
        for operation in [Call::Put, Call::Insert, Call::Remove] {
            reset();
            let db = open_db();
            let table = db.open_table("malformed-bool", 5).unwrap();
            let mut transaction = db.transaction().unwrap();
            match operation {
                Call::Put => push(Step::Put(ByteReply {
                    status: sys::MAKO_LOCAL_OK,
                    value: 2,
                })),
                Call::Insert => push(Step::Insert(ByteReply {
                    status: sys::MAKO_LOCAL_OK,
                    value: u8::MAX,
                })),
                Call::Remove => push(Step::Remove(ByteReply {
                    status: sys::MAKO_LOCAL_OK,
                    value: 2,
                })),
                _ => unreachable!(),
            }
            push(Step::Abort(sys::MAKO_LOCAL_OK));
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            let result = match operation {
                Call::Put => transaction.put(&table, b"key", b"value"),
                Call::Insert => transaction.insert(&table, b"key", b"value"),
                Call::Remove => transaction.remove(&table, b"key"),
                _ => unreachable!(),
            };
            assert_eq!(result, Err(Error::Internal));
            drop(transaction);
            assert_call_count(operation, 1);
            assert_call_count(Call::Abort, 1);
            assert_call_count(Call::Destroy, 1);
            drop(db);
            assert_drained();
        }
    }

    fn exercise_malformed_point_cleanup_quarantine() {
        reset();
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        let table = db.open_table("malformed-point-quarantine", 9).unwrap();
        let mut transaction = db.transaction().unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 2,
        }));
        push(Step::Abort(sys::MAKO_LOCAL_WORKER_POISONED));
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));

        assert_eq!(
            transaction.put(&table, b"key", b"value"),
            Err(Error::WorkerPoisoned)
        );
        assert_eq!(
            transaction.put(&table, b"no-native-retry", b"value"),
            Err(Error::TransactionFinished)
        );
        drop(transaction);
        assert_eq!(worker_health(), Ok(WorkerHealth::Poisoned));
        assert_eq!(quarantined_worker_count().unwrap(), before + 1);
        assert_call_count(Call::Put, 1);
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();
    }

    fn malformed_scan_replies() -> Vec<ScanReply> {
        vec![
            ScanReply {
                arena_required: 1,
                ..ScanReply::empty()
            },
            ScanReply {
                entry_count: 65,
                ..ScanReply::empty()
            },
            ScanReply {
                arena_used: 4 * 1024 + 1,
                ..ScanReply::empty()
            },
            ScanReply {
                done: 2,
                ..ScanReply::empty()
            },
            ScanReply {
                done: 0,
                ..ScanReply::empty()
            },
            ScanReply {
                entries: vec![sys::mako_local_scan_entry {
                    key_offset: 2,
                    key_length: 1,
                    value_offset: 0,
                    value_length: 1,
                }],
                arena: vec![b'v'],
                entry_count: 1,
                arena_used: 1,
                ..ScanReply::empty()
            },
            ScanReply {
                entries: vec![
                    sys::mako_local_scan_entry {
                        key_offset: 0,
                        key_length: 1,
                        value_offset: 0,
                        value_length: 0,
                    },
                    sys::mako_local_scan_entry {
                        key_offset: 1,
                        key_length: 1,
                        value_offset: 0,
                        value_length: 0,
                    },
                ],
                arena: b"ba".to_vec(),
                entry_count: 2,
                arena_used: 2,
                ..ScanReply::empty()
            },
            ScanReply {
                status: sys::MAKO_LOCAL_BUFFER_TOO_SMALL,
                arena_required: 1,
                done: 0,
                ..ScanReply::empty()
            },
        ]
    }

    fn exercise_malformed_scan_outputs() {
        for (index, reply) in malformed_scan_replies().into_iter().enumerate() {
            reset();
            let db = open_db();
            let table = db.open_table("malformed-scan", 6).unwrap();
            let mut transaction = db.transaction().unwrap();
            push(Step::Scan(reply));
            push(Step::Abort(sys::MAKO_LOCAL_OK));
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            {
                let mut scan = transaction.scan(&table, b"", None).unwrap();
                assert_eq!(scan.next(), Some(Err(Error::Internal)), "case {index}");
                assert_eq!(scan.next(), None);
            }
            drop(transaction);
            assert_call_count(Call::Scan, 1);
            assert_call_count(Call::Abort, 1);
            assert_call_count(Call::Destroy, 1);
            drop(db);
            assert_drained();
        }

        reset();
        let db = open_db();
        let table = db.open_table("malformed-rscan", 7).unwrap();
        let mut transaction = db.transaction().unwrap();
        push(Step::ReverseScan(ScanReply {
            arena_required: 1,
            ..ScanReply::empty()
        }));
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        {
            let mut scan = transaction.rscan(&table, b"", None).unwrap();
            assert_eq!(scan.next(), Some(Err(Error::Internal)));
        }
        drop(transaction);
        assert_call_count(Call::ReverseScan, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_malformed_scan_cleanup_quarantine() {
        reset();
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        let table = db.open_table("malformed-scan-quarantine", 10).unwrap();
        let mut transaction = db.transaction().unwrap();
        push(Step::Scan(ScanReply {
            arena_required: 1,
            ..ScanReply::empty()
        }));
        push(Step::Abort(sys::MAKO_LOCAL_WORKER_POISONED));
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));
        {
            let mut scan = transaction.scan(&table, b"", None).unwrap();
            assert_eq!(scan.next(), Some(Err(Error::WorkerPoisoned)));
            assert_eq!(scan.next(), None);
        }
        assert_eq!(
            transaction.put(&table, b"no-native-retry", b"value"),
            Err(Error::TransactionFinished)
        );
        drop(transaction);
        assert_eq!(worker_health(), Ok(WorkerHealth::Poisoned));
        assert_eq!(quarantined_worker_count().unwrap(), before + 1);
        assert_call_count(Call::Scan, 1);
        assert_call_count(Call::Put, 0);
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_malformed_begin_success() {
        reset();
        let db = open_db();
        push(Step::Begin {
            status: sys::MAKO_LOCAL_OK,
            return_handle: false,
        });
        assert!(matches!(db.transaction(), Err(Error::Internal)));
        assert_call_count(Call::Begin, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_handleless_poisoned_begin() {
        reset();
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        push(Step::Begin {
            status: sys::MAKO_LOCAL_WORKER_POISONED,
            return_handle: false,
        });
        assert!(matches!(db.transaction(), Err(Error::WorkerPoisoned)));
        assert_eq!(worker_health(), Ok(WorkerHealth::Poisoned));
        assert_eq!(quarantined_worker_count().unwrap(), before + 1);
        assert!(matches!(db.transaction(), Err(Error::WorkerPoisoned)));
        assert_call_count(Call::Begin, 1);
        assert_call_count(Call::Abort, 0);
        assert_call_count(Call::Destroy, 0);
        drop(db);
        assert_drained();
    }

    fn exercise_malformed_table_success() {
        reset();
        let db = open_db();
        push(Step::TableOpen {
            status: sys::MAKO_LOCAL_OK,
            return_handle: false,
        });
        assert!(matches!(
            db.open_table("null-table", 8),
            Err(Error::Internal)
        ));
        assert_call_count(Call::TableOpen, 1);
        drop(db);
        assert_drained();
    }

    #[test]
    fn fake_abi_exhaustively_checks_lifecycle_and_outputs() {
        exercise_operation_statuses();
        exercise_worker_health_and_drop_quarantine();
        exercise_fake_capabilities_are_honest();
        exercise_commit_and_cleanup_independence();
        exercise_commit_with_hook_stack_callback();
        exercise_explicit_abort_paths();
        exercise_malformed_get_outputs();
        exercise_malformed_boolean_outputs();
        exercise_malformed_point_cleanup_quarantine();
        exercise_malformed_scan_outputs();
        exercise_malformed_scan_cleanup_quarantine();
        exercise_malformed_begin_success();
        exercise_handleless_poisoned_begin();
        exercise_malformed_table_success();
    }
}
