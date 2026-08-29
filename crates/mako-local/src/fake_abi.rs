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

pub(super) type RecordBindHook = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        mako_timestamp: u32,
        exact_record_bytes: usize,
        sequence_out: *mut u64,
        record_bytes_out: *mut *mut u8,
        record_capacity_out: *mut usize,
    ) -> i32,
>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Call {
    TableOpen,
    Begin,
    FastBegin,
    Get,
    Put,
    FastPut,
    Insert,
    Remove,
    Scan,
    ReverseScan,
    Commit,
    CommitWithHook,
    FastCommitDestroy,
    FastRecordPreflight,
    FastRecordCommitDestroy,
    Abort,
    FastAbortDestroy,
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
    RecordPreflight {
        status: c_int,
        exact_record_bytes: usize,
        op_count: u32,
    },
    CommitRecord {
        status: c_int,
        timestamp: Option<u32>,
        exact_record_bytes: usize,
        record: Vec<u8>,
        reported_written: Option<u8>,
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
    quarantine_transitions: u64,
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
        state.quarantine_transitions += 1;
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

pub(super) fn local_quarantine_transition_count() -> u64 {
    STATE.with(|state| state.borrow().quarantine_transitions)
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

pub(super) unsafe fn mako_local_db_options_size() -> usize {
    sys::MAKO_LOCAL_DB_OPTIONS_V0_SIZE as usize
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

pub(super) unsafe fn mako_local_db_open_with_options(
    options: *const sys::mako_local_db_options,
    out: *mut *mut sys::mako_local_db,
) -> c_int {
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: caller supplied the required writable output pointer.
    unsafe { out.write(ptr::null_mut()) };
    if options.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: the non-null options pointer is borrowed for this call.
    let options = unsafe { &*options };
    if options.struct_size < sys::MAKO_LOCAL_DB_OPTIONS_V0_SIZE || options.flags != 0 {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: `out` was validated and the legacy helper owns all fake state.
    unsafe { mako_local_db_open(out) }
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

pub(super) unsafe fn mako_rust_fast_txn_begin(
    _db: *mut sys::mako_local_db,
    _bound_table: *mut sys::mako_local_table,
    out: *mut *mut sys::mako_local_txn,
) -> c_int {
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: caller supplied the required writable output pointer.
    unsafe { out.write(ptr::null_mut()) };
    with_state(|state| {
        state.calls.push(Call::FastBegin);
        if state.poisoned {
            return sys::MAKO_LOCAL_WORKER_POISONED;
        }
        let (status, return_handle) = match state.steps.front() {
            Some(Step::Begin { .. }) => match state.steps.pop_front() {
                Some(Step::Begin {
                    status,
                    return_handle,
                }) => (status, return_handle),
                found => unexpected("fast begin", found),
            },
            _ => (sys::MAKO_LOCAL_OK, true),
        };
        observe_status(state, status);
        if status == sys::MAKO_LOCAL_OK && return_handle {
            let mut txn = Box::new(FakeTxn);
            let raw = ptr::from_mut(txn.as_mut()).cast::<sys::mako_local_txn>();
            state.txn = Some(txn);
            // SAFETY: checked above; fake state owns the allocation.
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

pub(super) unsafe fn mako_rust_fast_txn_put(
    _txn: *mut sys::mako_local_txn,
    _key: *const u8,
    _key_len: u32,
    _value: *const u8,
    _value_len: u32,
) -> u64 {
    let reply = byte_operation("fast put", Call::FastPut, |step| match step {
        Step::Put(reply) => Some(reply),
        _ => None,
    });
    u64::from(reply.status as u32) | (u64::from(reply.value) << 32)
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

pub(super) unsafe fn mako_rust_fast_txn_commit_and_destroy(_txn: *mut sys::mako_local_txn) -> u64 {
    with_state(|state| {
        state.calls.push(Call::FastCommitDestroy);
        let commit = match state.steps.pop_front() {
            Some(Step::Commit(status)) => status,
            found => unexpected("fast commit", found),
        };
        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast commit cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(commit as u32) | (u64::from(cleanup as u32) << 32)
    })
}

pub(super) unsafe fn mako_rust_fast_txn_commit_with_hook_and_destroy(
    _txn: *mut sys::mako_local_txn,
    hook: sys::mako_local_post_validate_hook,
    context: *mut c_void,
) -> u64 {
    with_state(|state| {
        state.calls.push(Call::FastCommitDestroy);
        let (commit, timestamp) = match state.steps.pop_front() {
            Some(Step::CommitWithHook { status, timestamp }) => (status, timestamp),
            found => unexpected("fast commit with hook", found),
        };
        let hook = hook.expect("trusted fast hook commit requires a callback");
        if let Some(timestamp) = timestamp {
            // SAFETY: safe wrapper keeps its stack callback context live for
            // this synchronous fake boundary.
            let _ = unsafe { hook(context, timestamp) };
        }
        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast commit cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(commit as u32) | (u64::from(cleanup as u32) << 32)
    })
}

pub(super) unsafe fn mako_rust_fast_txn_record_preflight(
    _txn: *mut sys::mako_local_txn,
    _max_record_bytes: usize,
    exact_record_bytes_out: *mut usize,
    op_count_out: *mut u32,
) -> c_int {
    if exact_record_bytes_out.is_null() || op_count_out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: required outputs were checked above.
    unsafe {
        exact_record_bytes_out.write(0);
        op_count_out.write(0);
    }
    with_state(|state| {
        state.calls.push(Call::FastRecordPreflight);
        let (status, exact_record_bytes, op_count) = match state.steps.pop_front() {
            Some(Step::RecordPreflight {
                status,
                exact_record_bytes,
                op_count,
            }) => (status, exact_record_bytes, op_count),
            found => unexpected("fast record preflight", found),
        };
        observe_status(state, status);
        // Deliberately write scripted outputs even on error so tests can prove
        // the safe wrapper ignores them unless status is OK.
        unsafe {
            exact_record_bytes_out.write(exact_record_bytes);
            op_count_out.write(op_count);
        }
        status
    })
}

pub(super) unsafe fn mako_rust_fast_txn_commit_record_and_destroy(
    _txn: *mut sys::mako_local_txn,
    hook: RecordBindHook,
    context: *mut c_void,
    record_written_out: *mut u8,
) -> u64 {
    if !record_written_out.is_null() {
        // SAFETY: the optional completion output is writable for this call.
        unsafe { record_written_out.write(0) };
    }
    with_state(|state| {
        state.calls.push(Call::FastRecordCommitDestroy);
        let (commit, timestamp, exact_record_bytes, record, reported_written) =
            match state.steps.pop_front() {
                Some(Step::CommitRecord {
                    status,
                    timestamp,
                    exact_record_bytes,
                    record,
                    reported_written,
                }) => (
                    status,
                    timestamp,
                    exact_record_bytes,
                    record,
                    reported_written,
                ),
                found => unexpected("fast record commit", found),
            };

        let mut sequence = 0u64;
        let mut record_bytes = ptr::null_mut();
        let mut record_capacity = 0usize;
        let accepted = match (hook, timestamp) {
            (Some(hook), Some(timestamp)) => {
                // SAFETY: the safe wrapper keeps the callback context and its
                // output scalars live throughout this synchronous fake call.
                (unsafe {
                    hook(
                        context,
                        timestamp,
                        exact_record_bytes,
                        &mut sequence,
                        &mut record_bytes,
                        &mut record_capacity,
                    )
                }) != 0
            }
            _ => false,
        };
        let valid_binding = accepted
            && sequence != 0
            && !record_bytes.is_null()
            && record_capacity >= exact_record_bytes
            && record.len() == exact_record_bytes;
        // A scripted zero witness models native rejecting serialization after
        // the bind callback. Leave the MaybeUninit destination genuinely
        // untouched so Miri can prove the safe wrapper never reads it merely
        // because a dense slot was assigned.
        let initialize_record = reported_written != Some(0);
        let actual_written = if valid_binding && initialize_record {
            if exact_record_bytes != 0 {
                // SAFETY: the callback reported at least the exact writable
                // capacity and the scripted source has exactly that extent.
                unsafe {
                    ptr::copy_nonoverlapping(record.as_ptr(), record_bytes, exact_record_bytes)
                };
            }
            1
        } else {
            0
        };
        if !record_written_out.is_null() {
            // SAFETY: checked immediately before the write.
            unsafe { record_written_out.write(reported_written.unwrap_or(actual_written)) };
        }

        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast record commit cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(commit as u32) | (u64::from(cleanup as u32) << 32)
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

pub(super) unsafe fn mako_rust_fast_txn_abort_and_destroy(_txn: *mut sys::mako_local_txn) -> u64 {
    with_state(|state| {
        state.calls.push(Call::FastAbortDestroy);
        let abort = match state.steps.pop_front() {
            Some(Step::Abort(status)) => status,
            found => unexpected("fast abort", found),
        };
        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast abort cleanup", found),
        };
        observe_status(state, abort);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(abort as u32) | (u64::from(cleanup as u32) << 32)
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
    use std::num::NonZeroU64;
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
        assert_eq!(local_quarantine_transition_count(), 1);
        assert!(quarantined_worker_count().unwrap() > before);
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

    fn exercise_malformed_fast_terminal_outputs() {
        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast-malformed-commit", 16).unwrap();
        let transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Commit(sys::MAKO_LOCAL_INTERNAL));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report();
        assert_eq!(
            report.disposition,
            CommitDisposition::Unknown(Error::Internal)
        );
        assert_eq!(report.cleanup, Err(Error::Internal));
        assert_call_count(Call::FastCommitDestroy, 1);
        assert_call_count(Call::Destroy, 0);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast-malformed-abort", 17).unwrap();
        let transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Abort(sys::MAKO_LOCAL_INTERNAL));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(transaction.abort(), Err(Error::Internal));
        assert_call_count(Call::FastAbortDestroy, 1);
        assert_call_count(Call::Destroy, 0);
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
        assert_eq!(local_quarantine_transition_count(), 1);
        assert!(quarantined_worker_count().unwrap() > before);
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
        assert_eq!(local_quarantine_transition_count(), 1);
        assert!(quarantined_worker_count().unwrap() > before);
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
        assert_eq!(local_quarantine_transition_count(), 1);
        assert!(quarantined_worker_count().unwrap() > before);
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
        assert_eq!(local_quarantine_transition_count(), 1);
        assert!(quarantined_worker_count().unwrap() > before);
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

    fn exercise_trusted_fast_transaction_path() {
        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast", 11).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));

        // Checked operations can safely share the same pooled facade.
        push(Step::Get(GetReply::absent()));
        assert_eq!(transaction.get(&table, b"other"), Ok(None));

        push(Step::CommitWithHook {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(47),
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report_with_hook(|timestamp| timestamp.get() == 47);
        assert_eq!(report.disposition, CommitDisposition::Committed);
        assert_eq!(report.cleanup, Ok(()));
        assert_call_count(Call::FastBegin, 1);
        assert_call_count(Call::FastPut, 1);
        assert_call_count(Call::Get, 1);
        assert_call_count(Call::FastCommitDestroy, 1);
        assert_call_count(Call::CommitWithHook, 0);
        assert_call_count(Call::Destroy, 0);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast-cross-table", 12).unwrap();
        let mut other_native = Box::new(FakeTable { id: 99 });
        let other_table = crate::Table {
            raw: std::ptr::NonNull::from(other_native.as_mut()).cast(),
            _db: std::marker::PhantomData,
        };
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 0,
        }));
        assert_eq!(transaction.put(&other_table, b"key", b"value"), Ok(false));
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        drop(transaction);
        assert_call_count(Call::FastPut, 0);
        assert_call_count(Call::Put, 1);
        assert_call_count(Call::FastAbortDestroy, 1);
        assert_call_count(Call::Abort, 0);
        assert_call_count(Call::Destroy, 0);
        drop(other_native);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast-abort", 13).unwrap();
        let transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(transaction.abort(), Ok(()));
        assert_call_count(Call::FastAbortDestroy, 1);
        assert_call_count(Call::Abort, 0);
        assert_call_count(Call::Destroy, 0);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast-terminal-put", 14).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply::status(sys::MAKO_LOCAL_CONFLICT)));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(
            transaction.put(&table, b"key", b"value"),
            Err(Error::Conflict)
        );
        let report = transaction.commit_report();
        assert_eq!(
            report.disposition,
            CommitDisposition::Aborted(Error::TransactionFinished)
        );
        assert_eq!(report.cleanup, Ok(()));
        assert_call_count(Call::FastCommitDestroy, 0);
        assert_call_count(Call::FastAbortDestroy, 0);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fast-malformed", 15).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        // Values above one set reserved packed-result bits. The wrapper must
        // fail closed through the checked abort/destroy lifecycle.
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 2,
        }));
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(
            transaction.put(&table, b"key", b"value"),
            Err(Error::Internal)
        );
        drop(transaction);
        assert_call_count(Call::FastPut, 1);
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_commit_record_happy_path_and_empty_plan() {
        reset();
        let db = open_db();
        let table = db.open_table("trusted-record", 18).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 8,
            op_count: 2,
        });
        let preflight = transaction.commit_record_preflight(64).unwrap();
        assert_eq!(preflight.exact_record_bytes(), 8);
        assert_eq!(preflight.op_count(), 2);
        assert!(!preflight.is_empty());
        let copied = preflight;
        let mut record = crate::UninitCommitRecord::try_for(preflight).unwrap();
        assert_eq!(record.capacity(), 8);
        assert!(!record.is_written());
        assert_eq!(record.written_bytes(), None);

        let expected = b"v3record".to_vec();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(53),
            exact_record_bytes: 8,
            record: expected.clone(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let mut callback_count = 0;
        let report = transaction.commit_report_with_record(&mut record, |timestamp, bounds| {
            callback_count += 1;
            assert_eq!(timestamp.get(), 53);
            assert_eq!(bounds, copied);
            NonZeroU64::new(7)
        });
        assert_eq!(callback_count, 1);
        assert_eq!(report.commit.disposition, CommitDisposition::Committed);
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        assert!(record.is_written());
        assert_eq!(record.written_bytes(), Some(expected.as_slice()));
        assert_eq!(record.into_written(), Some(expected));
        assert_call_count(Call::FastRecordPreflight, 1);
        assert_call_count(Call::FastRecordCommitDestroy, 1);
        assert_call_count(Call::FastCommitDestroy, 0);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-empty-record", 19).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 30,
            op_count: 0,
        });
        // Empty plans need no output allocation and therefore remain valid
        // even when the caller's nonempty-record cap is below the 30-byte v3
        // framing size.
        let preflight = transaction.commit_record_preflight(1).unwrap();
        assert!(preflight.is_empty());
        push(Step::Commit(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report();
        assert_eq!(report.disposition, CommitDisposition::Committed);
        assert_eq!(report.cleanup, Ok(()));
        assert_call_count(Call::FastRecordPreflight, 1);
        assert_call_count(Call::FastRecordCommitDestroy, 0);
        assert_call_count(Call::FastCommitDestroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_commit_record_preflight_fail_closed() {
        reset();
        let db = open_db();
        let table = db.open_table("record-preflight-limit", 20).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_VALUE_TOO_LARGE,
            // Error-path diagnostics must not be accepted as a successful plan.
            exact_record_bytes: 1_000,
            op_count: 1,
        });
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(
            transaction.commit_record_preflight(16),
            Err(Error::ValueTooLarge)
        );
        drop(transaction);
        assert_call_count(Call::FastRecordPreflight, 1);
        assert_call_count(Call::Abort, 1);
        assert_call_count(Call::Destroy, 1);
        drop(db);
        assert_drained();

        for (index, exact_record_bytes, op_count, max_record_bytes) in [
            (0, 0, 1, 64),
            (1, 65, 1, 64),
            (2, 30, crate::TRANSACTION_ITEM_BUDGET as u32 + 1, 64),
        ] {
            reset();
            let db = open_db();
            let table = db
                .open_table("record-preflight-malformed", 21 + index)
                .unwrap();
            let mut transaction = db.trusted_transaction(&table).unwrap();
            push(Step::RecordPreflight {
                status: sys::MAKO_LOCAL_OK,
                exact_record_bytes,
                op_count,
            });
            // Malformed successful outputs trigger the checked fail-closed
            // abort path, followed by the sole destroy probe from Drop.
            push(Step::Abort(sys::MAKO_LOCAL_OK));
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            assert_eq!(
                transaction.commit_record_preflight(max_record_bytes),
                Err(Error::Internal),
                "case {index}"
            );
            drop(transaction);
            assert_call_count(Call::FastRecordPreflight, 1);
            assert_call_count(Call::Abort, 1);
            assert_call_count(Call::Destroy, 1);
            drop(db);
            assert_drained();
        }

        reset();
        let db = open_db();
        let table = db.open_table("record-preflight-once", 24).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 30,
            op_count: 0,
        });
        assert!(transaction.commit_record_preflight(64).is_ok());
        assert_eq!(
            transaction.put(&table, b"after-preflight", b"must-not-stage"),
            Err(Error::Busy)
        );
        assert_call_count(Call::FastPut, 0);
        assert_eq!(transaction.commit_record_preflight(64), Err(Error::Busy));
        push(Step::Commit(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        assert_eq!(transaction.commit(), Ok(()));
        assert_call_count(Call::FastRecordPreflight, 1);
        drop(db);
        assert_drained();

        // A nonempty sealed plan cannot fall back to ordinary unlogged commit.
        reset();
        let db = open_db();
        let table = db.open_table("record-no-unlogged-fallback", 35).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 31,
            op_count: 1,
        });
        assert!(transaction.commit_record_preflight(64).is_ok());
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report();
        assert_eq!(
            report.disposition,
            CommitDisposition::Aborted(Error::InvalidArgument)
        );
        assert_eq!(report.cleanup, Ok(()));
        assert_call_count(Call::FastCommitDestroy, 0);
        assert_call_count(Call::FastAbortDestroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_commit_record_hook_outcomes() {
        // A validation conflict never invokes or binds the acquire callback.
        reset();
        let db = open_db();
        let table = db.open_table("record-conflict", 25).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 4,
            op_count: 1,
        });
        let bounds = transaction.commit_record_preflight(4).unwrap();
        let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_CONFLICT,
            timestamp: None,
            exact_record_bytes: 4,
            record: b"data".to_vec(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report_with_record(&mut record, |_, _| {
            panic!("conflicting commit must not acquire a record slot")
        });
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::Conflict)
        );
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert!(!record.is_written());
        drop(db);
        assert_drained();

        // Explicit rejection is a definite abort and leaves storage unreadable.
        reset();
        let db = open_db();
        let table = db.open_table("record-reject", 26).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 4,
            op_count: 1,
        });
        let bounds = transaction.commit_record_preflight(4).unwrap();
        let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            timestamp: Some(61),
            exact_record_bytes: 4,
            record: b"data".to_vec(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report_with_record(&mut record, |timestamp, _| {
            assert_eq!(timestamp.get(), 61);
            None
        });
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::CommitHookRejected)
        );
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert_eq!(record.written_bytes(), None);
        assert_eq!(record.into_written(), None);
        drop(db);
        assert_drained();

        // Panics are contained and have exactly the same rejection semantics.
        reset();
        let db = open_db();
        let table = db.open_table("record-panic", 27).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 4,
            op_count: 1,
        });
        let bounds = transaction.commit_record_preflight(4).unwrap();
        let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            timestamp: Some(62),
            exact_record_bytes: 4,
            record: b"data".to_vec(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction
            .commit_report_with_record(&mut record, |_, _| panic!("contained acquire panic"));
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::CommitHookRejected)
        );
        assert!(!report.record_bound);
        assert!(!record.is_written());
        drop(db);
        assert_drained();

        // Invalid timestamps and size drift are rejected before user code.
        for (index, timestamp, exact_record_bytes) in [(0, 0, 4), (1, 63, 5)] {
            reset();
            let db = open_db();
            let table = db.open_table("record-hook-malformed", 28 + index).unwrap();
            let mut transaction = db.trusted_transaction(&table).unwrap();
            push(Step::RecordPreflight {
                status: sys::MAKO_LOCAL_OK,
                exact_record_bytes: 4,
                op_count: 1,
            });
            let bounds = transaction.commit_record_preflight(4).unwrap();
            let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
            push(Step::CommitRecord {
                status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
                timestamp: Some(timestamp),
                exact_record_bytes,
                record: vec![0; exact_record_bytes],
                reported_written: None,
            });
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            let report = transaction.commit_report_with_record(&mut record, |_, _| {
                panic!("invalid native callback inputs must be rejected first")
            });
            assert_eq!(
                report.commit.disposition,
                CommitDisposition::Aborted(Error::CommitHookRejected),
                "case {index}"
            );
            assert!(!report.record_bound);
            assert!(!report.record_written);
            assert!(!record.is_written());
            drop(db);
            assert_drained();
        }
    }

    fn exercise_commit_record_completion_witness() {
        // Binding is irrevocable even when the post-gate serializer rejects
        // before install. Preserve native's definite rejection and successful
        // cleanup while exposing the bound/unwritten shape for fail-stop
        // pinning by the cache.
        reset();
        let db = open_db();
        let table = db.open_table("record-bound-unwritten", 36).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 4,
            op_count: 1,
        });
        let bounds = transaction.commit_record_preflight(4).unwrap();
        let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            timestamp: Some(70),
            exact_record_bytes: 4,
            record: b"data".to_vec(),
            reported_written: Some(0),
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let report = transaction.commit_report_with_record(&mut record, |_, _| NonZeroU64::new(99));
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::CommitHookRejected)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(!report.record_written);
        assert!(!record.is_written());
        drop(db);
        assert_drained();

        for (index, status, timestamp, reported_written, expect_bound) in [
            (0, sys::MAKO_LOCAL_OK, Some(71), Some(0), true),
            (1, sys::MAKO_LOCAL_OK, Some(72), Some(2), true),
            (2, sys::MAKO_LOCAL_OK, None, Some(1), false),
            (3, sys::MAKO_LOCAL_OK, None, Some(0), false),
            // Final validation cannot report a conflict after the bind gate.
            // Preserve the bound slot but reject the malformed terminal shape.
            (4, sys::MAKO_LOCAL_CONFLICT, Some(73), Some(0), true),
        ] {
            reset();
            let db = open_db();
            let table = db.open_table("record-witness", 30 + index).unwrap();
            let mut transaction = db.trusted_transaction(&table).unwrap();
            push(Step::RecordPreflight {
                status: sys::MAKO_LOCAL_OK,
                exact_record_bytes: 4,
                op_count: 1,
            });
            let bounds = transaction.commit_record_preflight(4).unwrap();
            let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
            push(Step::CommitRecord {
                status,
                timestamp,
                exact_record_bytes: 4,
                record: b"data".to_vec(),
                reported_written,
            });
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            let report = transaction
                .commit_report_with_record(&mut record, |_, _| NonZeroU64::new(100 + index));
            assert_eq!(
                report.commit.disposition,
                CommitDisposition::Unknown(Error::Internal),
                "case {index}"
            );
            assert_eq!(report.commit.cleanup, Err(Error::Internal));
            assert!(!report.completion_contract_valid, "case {index}");
            assert_eq!(report.record_bound, expect_bound);
            assert!(!report.record_written);
            assert!(!record.is_written());
            assert_eq!(record.written_bytes(), None);
            drop(db);
            assert_drained();
        }

        // Once the record is written, an uncertain native terminal still
        // returns the bytes so the ordered slot can be pinned and diagnosed.
        reset();
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        let table = db.open_table("record-written-unknown", 34).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 4,
            op_count: 1,
        });
        let bounds = transaction.commit_record_preflight(4).unwrap();
        let mut record = crate::UninitCommitRecord::try_for(bounds).unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_WORKER_POISONED,
            timestamp: Some(73),
            exact_record_bytes: 4,
            record: b"data".to_vec(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));
        let report =
            transaction.commit_report_with_record(&mut record, |_, _| NonZeroU64::new(104));
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Unknown(Error::WorkerPoisoned)
        );
        assert_eq!(report.commit.cleanup, Err(Error::WorkerPoisoned));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        assert_eq!(record.written_bytes(), Some(b"data".as_slice()));
        assert!(quarantined_worker_count().unwrap() > before);
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
        exercise_trusted_fast_transaction_path();
        exercise_malformed_fast_terminal_outputs();
        exercise_commit_record_happy_path_and_empty_plan();
        exercise_commit_record_preflight_fail_closed();
        exercise_commit_record_hook_outcomes();
        exercise_commit_record_completion_witness();
    }
}
