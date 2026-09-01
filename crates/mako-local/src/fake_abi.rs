//! Pure-Rust scripted stand-in for the native ABI.
//!
//! This module is compiled only into `mako-local`'s unit-test harness. Its
//! functions intentionally use Rust linkage: Miri can execute every pointer and
//! ownership path without crossing an FFI boundary or linking the C++ archive.

#![allow(dead_code)]

use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::{CString, c_char, c_int, c_void};
use std::ptr;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use super::{
    FastNativeOrderedArenaResult, FastOnePutHolderPool, FastOnePutHolderView,
    FastPreselectedRecordResult, FastSpscHolderControl, TrustedNativeOrderedArenaControl, sys,
};

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
    FastNativeOrderedRecordCommitDestroy,
    FastUncheckedOnePutRecordCommitDestroy,
    FastNativeOrderedUncheckedOnePutRecordCommitDestroy,
    FastNativeOrderedUncheckedOnePutArenaCommitDestroy,
    FastSingleProducerUncheckedOnePutRecordCommitDestroy,
    FastPreselectedSingleProducerUncheckedOnePutRecordCommitDestroy,
    FastOnePutHolderPoolCreate,
    FastOnePutHolderPoolDestroy,
    FastPreselectedSingleProducerUncheckedOnePutHolderCommitDestroy,
    FastFusedSingleProducerOnePutHolderTryCommitDestroy,
    FastOnePutHolderPoolGetView,
    FastOnePutHolderPoolRelease,
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
    pub unchecked_record_bytes: u32,
}

impl ByteReply {
    pub fn status(status: c_int) -> Self {
        Self {
            status,
            value: 0,
            unchecked_record_bytes: 0,
        }
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
    CommitPreselectedRecord {
        status: c_int,
        timestamp: Option<u32>,
        exact_record_bytes: usize,
        record: Vec<u8>,
        reported_record_state: Option<u64>,
    },
    CommitPreselectedHolder {
        status: c_int,
        timestamp: Option<u32>,
        exact_record_bytes: usize,
        table_id: u64,
        key: Vec<u8>,
        value: Vec<u8>,
        reported_holder_state: Option<u64>,
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

#[derive(Debug)]
struct FakeTxn {
    fast_path_eligible: bool,
    unchecked_record_bytes: u32,
}

impl Default for FakeTxn {
    fn default() -> Self {
        Self {
            fast_path_eligible: true,
            unchecked_record_bytes: 0,
        }
    }
}

#[derive(Debug, Default)]
struct FakeOnePutHolder {
    sequence: u64,
    table_id: u64,
    mako_timestamp: u32,
    key: Vec<u8>,
    value: Vec<u8>,
    sealed: bool,
}

#[derive(Debug)]
struct FakeOnePutHolderPool {
    mask: usize,
    holders: Vec<FakeOnePutHolder>,
}

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
    last_record_checksum_mode: Option<u32>,
    last_unchecked_record_bytes: Option<u32>,
    holder_pools: usize,
    fused_latch_unhealthy_after_commit: bool,
    cache_order_claimed: bool,
    cache_order_mode: u32,
    cache_order_state: u64,
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
    STATE.with(|state| {
        let mut reset = State::default();
        reset.cache_order_state = UINT64_C_ONE << 29;
        *state.borrow_mut() = reset;
    });
}

pub(super) fn push(step: Step) {
    with_state(|state| state.steps.push_back(step));
}

pub(super) fn calls() -> Vec<Call> {
    STATE.with(|state| state.borrow().calls.clone())
}

pub(super) fn last_record_checksum_mode() -> Option<u32> {
    with_state(|state| state.last_record_checksum_mode)
}

pub(super) fn last_unchecked_record_bytes() -> Option<u32> {
    with_state(|state| state.last_unchecked_record_bytes)
}

fn latch_fused_unhealthy_after_commit_once() {
    with_state(|state| state.fused_latch_unhealthy_after_commit = true);
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
        assert_eq!(
            state.holder_pools, 0,
            "wrapper leaked {} fake one-Put holder pool(s)",
            state.holder_pools
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
    if observed == 0 {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    if observed >= sys::MAKO_LOCAL_MAX_MAKO_TIMESTAMP {
        return sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
    }

    let desired = observed + 1;
    with_state(|state| {
        let current = ((state.cache_order_state & CACHE_ORDER_TIMESTAMP_MASK)
            >> CACHE_ORDER_TIMESTAMP_SHIFT) as u32;
        if current == 0 || current > sys::MAKO_LOCAL_MAX_MAKO_TIMESTAMP {
            return sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
        }
        if current < desired {
            state.cache_order_state = (state.cache_order_state & !CACHE_ORDER_TIMESTAMP_MASK)
                | (u64::from(desired) << CACHE_ORDER_TIMESTAMP_SHIFT);
        }
        sys::MAKO_LOCAL_OK
    })
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
        state.cache_order_claimed = false;
        state.cache_order_mode = 0;
        sys::MAKO_LOCAL_OK
    })
}

pub(super) unsafe fn mako_rust_fast_db_order_record_validation_prefix(db: *mut sys::mako_local_db) {
    assert!(!db.is_null(), "the private cache-order cut needs a db");
    with_state(|state| {
        assert!(
            fake_packed_order_allowed(state),
            "the private cache-order cut needs a Concurrent namespace claim"
        );
    });
}

const UINT64_C_ONE: u64 = 1;
const CACHE_ORDER_FIELD_MASK: u64 = (UINT64_C_ONE << 29) - 1;
const CACHE_ORDER_TIMESTAMP_SHIFT: u32 = 29;
const CACHE_ORDER_TIMESTAMP_MASK: u64 = CACHE_ORDER_FIELD_MASK << CACHE_ORDER_TIMESTAMP_SHIFT;
const CACHE_ORDER_CONCURRENT: u32 = super::CacheOrderMode::Concurrent as u32;
const CACHE_ORDER_SINGLE_PRODUCER: u32 = super::CacheOrderMode::SingleProducer as u32;

fn assign_fake_cache_order_pair(state: &mut State, timestamp: Option<u32>) -> Option<(u64, u32)> {
    let timestamp = timestamp
        .filter(|raw| *raw != 0 && *raw <= crate::MAX_MAKO_TIMESTAMP)?;
    if !state.cache_order_claimed || state.cache_order_mode != CACHE_ORDER_CONCURRENT {
        return None;
    }
    let sequence = (state.cache_order_state & CACHE_ORDER_FIELD_MASK).checked_add(1)?;
    if sequence > u64::from(crate::MAX_MAKO_TIMESTAMP) {
        return None;
    }
    let next_timestamp = u64::from(timestamp) + 1;
    state.cache_order_state = (state.cache_order_state
        & !(CACHE_ORDER_FIELD_MASK | CACHE_ORDER_TIMESTAMP_MASK))
        | sequence
        | (next_timestamp << CACHE_ORDER_TIMESTAMP_SHIFT);
    Some((sequence, timestamp))
}

fn fake_packed_order_allowed(state: &State) -> bool {
    state.cache_order_claimed && state.cache_order_mode == CACHE_ORDER_CONCURRENT
}

fn fake_rust_sequence_order_allowed(state: &State) -> bool {
    !state.cache_order_claimed || state.cache_order_mode == CACHE_ORDER_SINGLE_PRODUCER
}

fn reject_fake_fast_terminal(state: &mut State) -> u64 {
    state.txn = None;
    u64::from(sys::MAKO_LOCAL_INVALID_ARGUMENT as u32)
        | (u64::from(sys::MAKO_LOCAL_OK as u32) << 32)
}

pub(super) unsafe fn mako_rust_fast_db_claim_cache_order_namespace(
    db: *mut sys::mako_local_db,
    foreground_mode: u32,
) -> c_int {
    with_state(|state| {
        if db.is_null()
            || (foreground_mode != CACHE_ORDER_CONCURRENT
                && foreground_mode != CACHE_ORDER_SINGLE_PRODUCER)
            || state.cache_order_claimed
        {
            return if db.is_null() {
                sys::MAKO_LOCAL_INVALID_ARGUMENT
            } else if foreground_mode != CACHE_ORDER_CONCURRENT
                && foreground_mode != CACHE_ORDER_SINGLE_PRODUCER
            {
                sys::MAKO_LOCAL_INVALID_ARGUMENT
            } else {
                sys::MAKO_LOCAL_BUSY
            };
        }
        state.cache_order_claimed = true;
        state.cache_order_mode = foreground_mode;
        if state.cache_order_state == 0 {
            state.cache_order_state = UINT64_C_ONE << 29;
        }
        state.cache_order_state &= !CACHE_ORDER_FIELD_MASK;
        sys::MAKO_LOCAL_OK
    })
}

pub(super) unsafe fn mako_rust_fast_db_reseed_cache_order_namespace(
    db: *mut sys::mako_local_db,
    recovered_sequence: u64,
) -> c_int {
    with_state(|state| {
        if db.is_null() || !state.cache_order_claimed {
            return sys::MAKO_LOCAL_INVALID_ARGUMENT;
        }
        if recovered_sequence > u64::from(sys::MAKO_LOCAL_MAX_MAKO_TIMESTAMP) {
            return sys::MAKO_LOCAL_INVALID_ARGUMENT;
        }
        state.cache_order_state =
            (state.cache_order_state & !CACHE_ORDER_FIELD_MASK) | recovered_sequence;
        sys::MAKO_LOCAL_OK
    })
}

pub(super) unsafe fn mako_rust_fast_db_cache_order_snapshot(
    db: *const sys::mako_local_db,
) -> u64 {
    with_state(|state| {
        if db.is_null() || !state.cache_order_claimed {
            0
        } else {
            state.cache_order_state
        }
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
            state.txn = Some(Box::new(FakeTxn::default()));
            let raw = ptr::from_mut(
                state
                    .txn
                    .as_deref_mut()
                    .expect("transaction was just registered"),
            )
            .cast::<sys::mako_local_txn>();
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
            state.txn = Some(Box::new(FakeTxn::default()));
            let raw = ptr::from_mut(
                state
                    .txn
                    .as_deref_mut()
                    .expect("transaction was just registered"),
            )
            .cast::<sys::mako_local_txn>();
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
    txn: *mut sys::mako_local_txn,
    _key: *const u8,
    _key_len: u32,
    _value: *const u8,
    _value_len: u32,
) -> u64 {
    let reply = byte_operation("fast put", Call::FastPut, |step| match step {
        Step::Put(reply) => Some(reply),
        _ => None,
    });
    assert!(!txn.is_null());
    // SAFETY: fast begin returned this live fake allocation and the safe
    // wrapper uniquely borrows it for the operation.
    let fake = unsafe { &mut *txn.cast::<FakeTxn>() };
    if reply.status == sys::MAKO_LOCAL_OK
        && fake.fast_path_eligible
        && fake.unchecked_record_bytes == 0
        && reply.unchecked_record_bytes != 0
    {
        fake.unchecked_record_bytes = reply.unchecked_record_bytes;
    } else {
        // Match native's conservative direct-witness retirement after a
        // second/malformed fast mutation.
        fake.fast_path_eligible = false;
        fake.unchecked_record_bytes = 0;
    }
    u64::from(reply.status as u32)
        | (u64::from(reply.value) << 32)
        | (u64::from(reply.unchecked_record_bytes) << 33)
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
    txn: *mut sys::mako_local_txn,
    max_record_bytes: usize,
    exact_record_bytes_out: *mut usize,
    op_count_out: *mut u32,
) -> c_int {
    // SAFETY: this compatibility spelling has the same pointer contract and
    // selects the native ABI's default CRC32C mode.
    unsafe {
        mako_rust_fast_txn_record_preflight_with_checksum(
            txn,
            max_record_bytes,
            1,
            exact_record_bytes_out,
            op_count_out,
        )
    }
}

pub(super) unsafe fn mako_rust_fast_txn_record_preflight_with_checksum(
    _txn: *mut sys::mako_local_txn,
    _max_record_bytes: usize,
    checksum_mode: u32,
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
        state.last_record_checksum_mode = Some(checksum_mode);
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
        if !fake_rust_sequence_order_allowed(state) {
            return reject_fake_fast_terminal(state);
        }
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

#[allow(clippy::too_many_arguments)]
pub(super) unsafe fn mako_rust_fast_txn_commit_native_ordered_record_and_destroy(
    _txn: *mut sys::mako_local_txn,
    unhealthy: *const u8,
    hook: RecordBindHook,
    context: *mut c_void,
    ordered_sequence_out: *mut u64,
    ordered_timestamp_out: *mut u32,
    record_written_out: *mut u8,
) -> u64 {
    if !ordered_sequence_out.is_null() {
        unsafe { ordered_sequence_out.write(0) };
    }
    if !ordered_timestamp_out.is_null() {
        unsafe { ordered_timestamp_out.write(0) };
    }
    if !record_written_out.is_null() {
        unsafe { record_written_out.write(0) };
    }
    with_state(|state| {
        state.calls.push(Call::FastNativeOrderedRecordCommitDestroy);
        if !fake_packed_order_allowed(state) {
            return reject_fake_fast_terminal(state);
        }
        let (commit, timestamp, exact_record_bytes, record, reported_written) =
            match state.steps.pop_front() {
                Some(Step::CommitRecord {
                    status,
                    timestamp,
                    exact_record_bytes,
                    record,
                    reported_written,
                }) => (status, timestamp, exact_record_bytes, record, reported_written),
                found => unexpected("fast native-ordered record commit", found),
            };

        let healthy = !unhealthy.is_null() && unsafe { unhealthy.read() } == 0;
        let assigned = healthy
            .then(|| assign_fake_cache_order_pair(state, timestamp))
            .flatten();
        if let Some((sequence, timestamp)) = assigned {
            if !ordered_sequence_out.is_null() {
                unsafe { ordered_sequence_out.write(sequence) };
            }
            if !ordered_timestamp_out.is_null() {
                unsafe { ordered_timestamp_out.write(timestamp) };
            }
        }

        let mut returned_sequence = assigned.map_or(0, |(sequence, _)| sequence);
        let mut record_bytes = ptr::null_mut();
        let mut record_capacity = 0usize;
        let accepted = match (assigned, hook) {
            (Some((_, timestamp)), Some(hook)) => unsafe {
                hook(
                    context,
                    timestamp,
                    exact_record_bytes,
                    &mut returned_sequence,
                    &mut record_bytes,
                    &mut record_capacity,
                ) != 0
            },
            _ => false,
        };
        let valid_binding = accepted
            && assigned.is_some_and(|(sequence, _)| sequence == returned_sequence)
            && !record_bytes.is_null()
            && record_capacity >= exact_record_bytes
            && record.len() == exact_record_bytes;
        let initialize_record = reported_written != Some(0);
        let actual_written = if valid_binding && initialize_record {
            if exact_record_bytes != 0 {
                unsafe {
                    ptr::copy_nonoverlapping(record.as_ptr(), record_bytes, exact_record_bytes)
                };
            }
            1
        } else {
            0
        };
        if !record_written_out.is_null() {
            unsafe { record_written_out.write(reported_written.unwrap_or(actual_written)) };
        }

        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast native-ordered record cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(commit as u32) | (u64::from(cleanup as u32) << 32)
    })
}

unsafe fn fast_unchecked_one_put_record_commit_and_destroy(
    call: Call,
    _txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    hook: RecordBindHook,
    context: *mut c_void,
    record_written_out: *mut u8,
) -> u64 {
    if !record_written_out.is_null() {
        // SAFETY: the optional completion output is writable for this call.
        unsafe { record_written_out.write(0) };
    }
    with_state(|state| {
        state.calls.push(call);
        state.last_unchecked_record_bytes = Some(expected_record_bytes);
        if !fake_rust_sequence_order_allowed(state) {
            return reject_fake_fast_terminal(state);
        }
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
                found => unexpected("fast unchecked one-put record commit", found),
            };

        let exact_candidate = exact_record_bytes == expected_record_bytes as usize;
        let mut sequence = 0u64;
        let mut record_bytes = ptr::null_mut();
        let mut record_capacity = 0usize;
        let accepted = match (exact_candidate, hook, timestamp) {
            (true, Some(hook), Some(timestamp)) => {
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
            found => unexpected("fast unchecked one-put record commit cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(commit as u32) | (u64::from(cleanup as u32) << 32)
    })
}

pub(super) unsafe fn mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
    txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    hook: RecordBindHook,
    context: *mut c_void,
    record_written_out: *mut u8,
) -> u64 {
    // SAFETY: forwarded unchanged to the shared fake terminal.
    unsafe {
        fast_unchecked_one_put_record_commit_and_destroy(
            Call::FastUncheckedOnePutRecordCommitDestroy,
            txn,
            expected_record_bytes,
            hook,
            context,
            record_written_out,
        )
    }
}

#[allow(clippy::too_many_arguments)]
pub(super) unsafe fn mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_record_and_destroy(
    _txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    next_bound: *mut u64,
    unhealthy: *const u8,
    hook: RecordBindHook,
    context: *mut c_void,
    ordered_sequence_out: *mut u64,
    ordered_timestamp_out: *mut u32,
    record_written_out: *mut u8,
) -> u64 {
    if !ordered_sequence_out.is_null() {
        // SAFETY: the wrapper supplies a live scalar output.
        unsafe { ordered_sequence_out.write(0) };
    }
    if !ordered_timestamp_out.is_null() {
        // SAFETY: the wrapper supplies a live scalar output.
        unsafe { ordered_timestamp_out.write(0) };
    }
    if !record_written_out.is_null() {
        // SAFETY: the wrapper supplies a live scalar output.
        unsafe { record_written_out.write(0) };
    }
    with_state(|state| {
        state
            .calls
            .push(Call::FastNativeOrderedUncheckedOnePutRecordCommitDestroy);
        state.last_unchecked_record_bytes = Some(expected_record_bytes);
        if !fake_packed_order_allowed(state) {
            return reject_fake_fast_terminal(state);
        }
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
                found => unexpected("fast native-ordered one-put record commit", found),
            };

        let exact_candidate = exact_record_bytes == expected_record_bytes as usize;
        let healthy = !unhealthy.is_null() && unsafe { unhealthy.read() } == 0;
        let compatibility_field_valid = !next_bound.is_null()
            && (next_bound as usize) % std::mem::align_of::<u64>() == 0;
        let assigned = (exact_candidate && healthy && compatibility_field_valid)
            .then(|| assign_fake_cache_order_pair(state, timestamp))
            .flatten();
        if let Some((sequence, timestamp)) = assigned {
            if !ordered_sequence_out.is_null() {
                unsafe { ordered_sequence_out.write(sequence) };
            }
            if !ordered_timestamp_out.is_null() {
                unsafe { ordered_timestamp_out.write(timestamp) };
            }
        }

        let mut returned_sequence = assigned.map_or(0, |(sequence, _)| sequence);
        let mut record_bytes = ptr::null_mut();
        let mut record_capacity = 0usize;
        let accepted = match (assigned, hook) {
            (Some((_, timestamp)), Some(hook)) => {
                // SAFETY: the wrapper retains every pointer and callback value
                // throughout this synchronous fake invocation.
                (unsafe {
                    hook(
                        context,
                        timestamp,
                        exact_record_bytes,
                        &mut returned_sequence,
                        &mut record_bytes,
                        &mut record_capacity,
                    )
                }) != 0
            }
            _ => false,
        };
        let valid_binding = accepted
            && assigned.is_some_and(|(sequence, _)| returned_sequence == sequence)
            && !record_bytes.is_null()
            && record_capacity >= exact_record_bytes
            && record.len() == exact_record_bytes;
        let initialize_record = reported_written != Some(0);
        let actual_written = if valid_binding && initialize_record {
            if exact_record_bytes != 0 {
                unsafe {
                    ptr::copy_nonoverlapping(record.as_ptr(), record_bytes, exact_record_bytes)
                };
            }
            1
        } else {
            0
        };
        if !record_written_out.is_null() {
            unsafe { record_written_out.write(reported_written.unwrap_or(actual_written)) };
        }

        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast native-ordered one-put cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        u64::from(commit as u32) | (u64::from(cleanup as u32) << 32)
    })
}

pub(super) unsafe fn mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_arena_and_destroy(
    _txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    control: *const TrustedNativeOrderedArenaControl,
) -> FastNativeOrderedArenaResult {
    with_state(|state| {
        state
            .calls
            .push(Call::FastNativeOrderedUncheckedOnePutArenaCommitDestroy);
        state.last_unchecked_record_bytes = Some(expected_record_bytes);
        if !fake_packed_order_allowed(state) {
            let terminal = reject_fake_fast_terminal(state);
            return FastNativeOrderedArenaResult {
                terminal,
                ordered_sequence: 0,
                record_state: 0,
            };
        }
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
                found => unexpected("fast native-ordered one-put arena commit", found),
            };

        // SAFETY: the wrapper lends its live repr(C) control for this entire
        // synchronous fake call.
        let control = unsafe { control.as_ref() };
        let layout_valid = control.is_some_and(|control| {
            let capacity = control.publication_mask.checked_add(1);
            !control.next_bound.is_null()
                && !control.unhealthy.is_null()
                && !control.publication_base.is_null()
                && !control.arena_base.is_null()
                && (control.next_bound as usize) % std::mem::align_of::<u64>() == 0
                && (control.publication_base as usize) % 64 == 0
                && (control.arena_base as usize) % 64 == 0
                && capacity.is_some_and(|capacity| {
                    capacity >= 4
                        && capacity.is_power_of_two()
                        && 1usize.checked_shl(control.publication_shift) == Some(capacity)
                })
                && control.publication_stride == 64
                && control.arena_stride == 256
                && control.arena_block_bytes != 0
                && control.arena_block_bytes <= 256
                && expected_record_bytes != 0
                && expected_record_bytes <= control.arena_block_bytes
        });

        let exact_candidate = exact_record_bytes == expected_record_bytes as usize;
        let mut sequence = 0u64;
        let mut assigned = false;
        if layout_valid && exact_candidate {
            let control = control.unwrap();
            // SAFETY: construction promises this byte is one live AtomicBool
            // and layout validation above rejected a null pointer.
            let health = unsafe {
                AtomicBool::from_ptr(control.unhealthy.cast_mut().cast::<bool>())
            };
            let healthy = !health.load(Ordering::Acquire);
            if healthy {
                if let Some((next, _)) = assign_fake_cache_order_pair(state, timestamp) {
                    sequence = next;
                    assigned = true;

                    let index = sequence as usize & control.publication_mask;
                    // SAFETY: the unsafe control contract provides both full
                    // rings and the validated index/strides stay in bounds.
                    let publication = unsafe {
                        control
                            .publication_base
                            .add(index * control.publication_stride as usize)
                    };
                    let turn = publication.cast::<u64>();
                    let free = (sequence >> control.publication_shift) << 2;
                    let bound = free | 1;
                    // SAFETY: byte zero is the aligned AtomicU64 turn word.
                    let turn = unsafe { AtomicU64::from_ptr(turn) };
                    assert_eq!(turn.load(Ordering::Acquire), free);
                    // SAFETY: the exact FREE generation grants unique access
                    // to the native record-extent field at byte 16.
                    unsafe { publication.add(16).cast::<usize>().write(0) };
                    turn.store(bound, Ordering::Release);
                }
            }
        }

        let initialize_record = reported_written != Some(0);
        let actual_written = if assigned
            && initialize_record
            && record.len() == exact_record_bytes
        {
            let control = control.unwrap();
            let index = sequence as usize & control.publication_mask;
            // SAFETY: assignment bound this exact ring generation and the
            // validated arena block covers expected_record_bytes.
            let destination = unsafe {
                control
                    .arena_base
                    .add(index * control.arena_stride as usize)
            };
            if exact_record_bytes != 0 {
                unsafe {
                    ptr::copy_nonoverlapping(record.as_ptr(), destination, exact_record_bytes)
                };
            }
            1u8
        } else {
            0u8
        };
        let reported_state = reported_written.unwrap_or(actual_written);
        let record_state = if assigned {
            u64::from(timestamp.unwrap()) | (u64::from(reported_state) << 32)
        } else {
            u64::from(reported_state) << 32
        };

        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast native-ordered one-put arena cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        FastNativeOrderedArenaResult {
            terminal: u64::from(commit as u32) | (u64::from(cleanup as u32) << 32),
            ordered_sequence: sequence,
            record_state,
        }
    })
}

pub(super) unsafe fn mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
    txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    hook: RecordBindHook,
    context: *mut c_void,
    record_written_out: *mut u8,
) -> u64 {
    // The fake has no native ticket words; a distinct call tag verifies that
    // the safe cache capability selected the intended private ABI spelling.
    // SAFETY: forwarded unchanged to the shared fake terminal.
    unsafe {
        fast_unchecked_one_put_record_commit_and_destroy(
            Call::FastSingleProducerUncheckedOnePutRecordCommitDestroy,
            txn,
            expected_record_bytes,
            hook,
            context,
            record_written_out,
        )
    }
}

pub(super) unsafe fn mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
    _txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    sequence: u64,
    record_out: *mut u8,
    record_capacity: usize,
) -> FastPreselectedRecordResult {
    with_state(|state| {
        state
            .calls
            .push(Call::FastPreselectedSingleProducerUncheckedOnePutRecordCommitDestroy);
        state.last_unchecked_record_bytes = Some(expected_record_bytes);
        if !fake_rust_sequence_order_allowed(state) {
            return FastPreselectedRecordResult {
                terminal: reject_fake_fast_terminal(state),
                record_state: 0,
            };
        }
        let (commit, timestamp, exact_record_bytes, record, reported_record_state) =
            match state.steps.pop_front() {
                Some(Step::CommitPreselectedRecord {
                    status,
                    timestamp,
                    exact_record_bytes,
                    record,
                    reported_record_state,
                }) => (
                    status,
                    timestamp,
                    exact_record_bytes,
                    record,
                    reported_record_state,
                ),
                found => unexpected("fast preselected one-put record commit", found),
            };

        let exact_candidate = exact_record_bytes == expected_record_bytes as usize;
        let valid_target = expected_record_bytes != 0
            && sequence != 0
            && !record_out.is_null()
            && record_capacity >= exact_record_bytes
            && record.len() == exact_record_bytes;
        let actual_record_state = if exact_candidate && valid_target {
            timestamp.map_or(0, |timestamp| u64::from(timestamp) | (1u64 << 32))
        } else {
            0
        };
        // A direct override lets the contract tests model a corrupt same-build
        // ABI, including reserved bits and impossible timestamp/witness pairs.
        let record_state = reported_record_state.unwrap_or(actual_record_state);
        if record_state & (1u64 << 32) != 0 && exact_candidate && valid_target {
            if exact_record_bytes != 0 {
                // SAFETY: the Rust wrapper supplied a stable target whose
                // capacity covers this exact scripted source extent.
                unsafe {
                    ptr::copy_nonoverlapping(record.as_ptr(), record_out, exact_record_bytes)
                };
            }
        }

        let cleanup = match state.steps.pop_front() {
            Some(Step::Destroy(status)) => status,
            found => unexpected("fast preselected one-put record commit cleanup", found),
        };
        observe_status(state, commit);
        observe_status(state, cleanup);
        if cleanup == sys::MAKO_LOCAL_OK {
            state.txn = None;
        }
        FastPreselectedRecordResult {
            terminal: u64::from(commit as u32) | (u64::from(cleanup as u32) << 32),
            record_state,
        }
    })
}

pub(super) unsafe fn mako_rust_fast_one_put_holder_pool_create(
    capacity: usize,
    key_reserve_bytes: u32,
    value_reserve_bytes: u32,
    out: *mut *mut FastOnePutHolderPool,
) -> c_int {
    with_state(|state| state.calls.push(Call::FastOnePutHolderPoolCreate));
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: checked non-null above; deterministic nulling matches native.
    unsafe { out.write(ptr::null_mut()) };
    if capacity == 0 || !capacity.is_power_of_two() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    if key_reserve_bytes as usize > super::MAX_KEY_BYTES
        || value_reserve_bytes as usize > super::MAX_VALUE_BYTES
    {
        return sys::MAKO_LOCAL_VALUE_TOO_LARGE;
    }

    let pool = Box::new(FakeOnePutHolderPool {
        mask: capacity - 1,
        holders: (0..capacity).map(|_| FakeOnePutHolder::default()).collect(),
    });
    let raw = Box::into_raw(pool).cast::<FastOnePutHolderPool>();
    // SAFETY: `out` was checked and now receives the unique fake allocation.
    unsafe { out.write(raw) };
    with_state(|state| state.holder_pools += 1);
    sys::MAKO_LOCAL_OK
}

pub(super) unsafe fn mako_rust_fast_one_put_holder_pool_destroy(
    pool: *mut FastOnePutHolderPool,
) -> c_int {
    with_state(|state| state.calls.push(Call::FastOnePutHolderPoolDestroy));
    if pool.is_null() {
        return sys::MAKO_LOCAL_OK;
    }
    // SAFETY: the private wrapper passes a live allocation returned by create
    // and externally quiesces all holder users before destruction.
    let fake = unsafe { &*pool.cast::<FakeOnePutHolderPool>() };
    if fake.holders.iter().any(|holder| holder.sealed) {
        return sys::MAKO_LOCAL_BUSY;
    }
    // SAFETY: the successful quiescent destroy consumes this allocation once.
    drop(unsafe { Box::from_raw(pool.cast::<FakeOnePutHolderPool>()) });
    with_state(|state| {
        state.holder_pools = state
            .holder_pools
            .checked_sub(1)
            .expect("fake holder pool destruction underflow")
    });
    sys::MAKO_LOCAL_OK
}

pub(super) unsafe fn mako_rust_fast_one_put_holder_pool_get_hot_layout(
    pool: *mut FastOnePutHolderPool,
    holder_base_out: *mut *mut c_void,
    holder_mask_out: *mut usize,
) -> c_int {
    if holder_base_out.is_null() || holder_mask_out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: both outputs were checked above.
    unsafe {
        holder_base_out.write(ptr::null_mut());
        holder_mask_out.write(0);
    }
    if pool.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: the private wrapper supplies its live fake pool allocation.
    let fake = unsafe { &mut *pool.cast::<FakeOnePutHolderPool>() };
    if fake.holders.is_empty() || !fake.holders.len().is_power_of_two() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // The boxed pool keeps this Vec allocation stable until pool destruction.
    // SAFETY: both outputs remain live for this synchronous call.
    unsafe {
        holder_base_out.write(fake.holders.as_mut_ptr().cast::<c_void>());
        holder_mask_out.write(fake.mask);
    }
    sys::MAKO_LOCAL_OK
}

unsafe fn fake_preselected_one_put_holder_commit(
    state: &mut State,
    expected_record_bytes: u32,
    pool: *mut FastOnePutHolderPool,
    sequence: u64,
) -> FastPreselectedRecordResult {
    let (commit, timestamp, exact_record_bytes, table_id, key, value, reported_holder_state) =
        match state.steps.pop_front() {
            Some(Step::CommitPreselectedHolder {
                status,
                timestamp,
                exact_record_bytes,
                table_id,
                key,
                value,
                reported_holder_state,
            }) => (
                status,
                timestamp,
                exact_record_bytes,
                table_id,
                key,
                value,
                reported_holder_state,
            ),
            found => unexpected("fast preselected one-put holder commit", found),
        };

    let canonical_bytes = super::UNCHECKED_ONE_PUT_RECORD_OVERHEAD_BYTES
        .checked_add(key.len())
        .and_then(|bytes| bytes.checked_add(value.len()));
    let valid_candidate = !pool.is_null()
        && sequence != 0
        && expected_record_bytes != 0
        && exact_record_bytes == expected_record_bytes as usize
        && canonical_bytes == Some(exact_record_bytes)
        && key.len() <= super::MAX_KEY_BYTES
        && value.len() <= super::MAX_VALUE_BYTES;

    let mut actual_holder_state = 0;
    if valid_candidate {
        // SAFETY: the wrapper supplies the live pool created above.
        let fake = unsafe { &mut *pool.cast::<FakeOnePutHolderPool>() };
        let index = (sequence as usize).wrapping_sub(1) & fake.mask;
        let holder = &mut fake.holders[index];
        if !holder.sealed {
            if let Some(timestamp) = timestamp {
                holder.sequence = sequence;
                holder.table_id = table_id;
                holder.mako_timestamp = timestamp;
                holder.key = key;
                holder.value = value;
                holder.sealed = true;
                actual_holder_state = u64::from(timestamp) | (1u64 << 32);
            }
        }
    }
    // An explicit override models an internally corrupt same-build ABI. The
    // independently retained fake holder still follows actual native
    // acceptance, allowing tests to inspect fail-closed combinations.
    let holder_state = reported_holder_state.unwrap_or(actual_holder_state);

    let cleanup = match state.steps.pop_front() {
        Some(Step::Destroy(status)) => status,
        found => unexpected("fast preselected one-put holder commit cleanup", found),
    };
    observe_status(state, commit);
    observe_status(state, cleanup);
    if cleanup == sys::MAKO_LOCAL_OK {
        state.txn = None;
    }
    FastPreselectedRecordResult {
        terminal: u64::from(commit as u32) | (u64::from(cleanup as u32) << 32),
        record_state: holder_state,
    }
}

pub(super) unsafe fn mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
    _txn: *mut sys::mako_local_txn,
    expected_record_bytes: u32,
    pool: *mut FastOnePutHolderPool,
    sequence: u64,
) -> FastPreselectedRecordResult {
    with_state(|state| {
        state
            .calls
            .push(Call::FastPreselectedSingleProducerUncheckedOnePutHolderCommitDestroy);
        state.last_unchecked_record_bytes = Some(expected_record_bytes);
        if !fake_rust_sequence_order_allowed(state) {
            return FastPreselectedRecordResult {
                terminal: reject_fake_fast_terminal(state),
                record_state: 0,
            };
        }
        // SAFETY: the outer fake ABI entry inherits the private holder and
        // unique-generation contract from the safe wrapper.
        unsafe {
            fake_preselected_one_put_holder_commit(state, expected_record_bytes, pool, sequence)
        }
    })
}

pub(super) unsafe fn mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
    txn: *mut sys::mako_local_txn,
    acknowledged: *mut u64,
    unhealthy: *const u8,
    control: *mut FastSpscHolderControl,
    capacity_limit: u64,
) -> u64 {
    const CONSUMED_PUBLISHED: u32 = 0;
    const UNTOUCHED_NEED_GENERAL: u32 = 1;
    const UNTOUCHED_NEED_SLOW: u32 = 2;
    const CONSUMED_COMMITTED_UNPUBLISHED: u32 = 3;
    const CONSUMED_OUTCOME: u32 = 4;

    assert!(!txn.is_null());
    assert!(!control.is_null());
    assert!(!acknowledged.is_null());
    assert!(!unhealthy.is_null());
    // SAFETY: the private wrapper supplies live stable pointers for this
    // synchronous call.
    let control = unsafe { &*control };
    assert!(!control.pool.is_null());
    assert!(!control.holder_base.is_null());
    // SAFETY: the control retains the matching live fake pool.
    let fake_pool = unsafe { &*control.pool.cast::<FakeOnePutHolderPool>() };
    assert_eq!(
        control.holder_base,
        fake_pool.holders.as_ptr().cast_mut().cast()
    );
    assert_eq!(control.holder_mask, fake_pool.mask);
    assert!(!control.acknowledged.is_null());
    assert!(!control.unhealthy.is_null());
    assert_eq!(acknowledged, control.acknowledged);
    assert_eq!(unhealthy, control.unhealthy);
    assert_ne!(control.capacity, 0);
    assert_eq!(control.reserved, 0);

    // Read the fake witness before any consuming path can release `txn`.
    // SAFETY: fast begin returned this live allocation and the safe wrapper
    // still owns the active transaction on all entry paths.
    let exact_record_bytes = {
        let fake = unsafe { &*txn.cast::<FakeTxn>() };
        if fake.fast_path_eligible {
            fake.unchecked_record_bytes
        } else {
            0
        }
    };
    let mode_rejected = with_state(|state| {
        state
            .calls
            .push(Call::FastFusedSingleProducerOnePutHolderTryCommitDestroy);
        if !fake_rust_sequence_order_allowed(state) {
            state.txn = None;
            true
        } else {
            false
        }
    });
    if mode_rejected {
        // SAFETY: the consumed-outcome code initializes this live cold result.
        unsafe {
            control.cold_out.get().write(FastPreselectedRecordResult {
                terminal: u64::from(sys::MAKO_LOCAL_INVALID_ARGUMENT as u32)
                    | (u64::from(sys::MAKO_LOCAL_OK as u32) << 32),
                record_state: u64::from(exact_record_bytes) << 33,
            })
        };
        return u64::from(CONSUMED_OUTCOME);
    }

    // SAFETY: these are the live inner pointers supplied by the private
    // single-producer wrapper for this synchronous call.
    let unhealthy = unsafe { AtomicBool::from_ptr(unhealthy.cast_mut().cast::<bool>()) };
    let acknowledged = unsafe { AtomicU64::from_ptr(acknowledged) };
    if exact_record_bytes == 0 || exact_record_bytes > control.max_record_bytes {
        return u64::from(UNTOUCHED_NEED_GENERAL);
    }

    if unhealthy.load(Ordering::Acquire) {
        return u64::from(UNTOUCHED_NEED_SLOW) | (u64::from(exact_record_bytes) << 32);
    }
    // ACK is the canonical healthy tail. producer_next is synchronized only
    // by Rust's cold decoder when it needs the richer cursor protocol.
    let tail = acknowledged.load(Ordering::Relaxed);
    if tail >= capacity_limit {
        return u64::from(UNTOUCHED_NEED_SLOW) | (u64::from(exact_record_bytes) << 32);
    }
    let sequence = tail + 1;
    let outcome = with_state(|state| {
        state.last_unchecked_record_bytes = Some(exact_record_bytes);
        // SAFETY: the capacity check retains this unique future holder and the
        // scripted fake obeys the same consuming terminal contract.
        unsafe {
            fake_preselected_one_put_holder_commit(
                state,
                exact_record_bytes,
                control.pool,
                sequence,
            )
        }
    });
    const PACKED_OK: u64 =
        (sys::MAKO_LOCAL_OK as u32 as u64) | ((sys::MAKO_LOCAL_OK as u32 as u64) << 32);
    let timestamp = outcome.record_state as u32;
    if outcome.terminal == PACKED_OK && outcome.record_state >> 32 == 1 && timestamp != 0 {
        if with_state(|state| std::mem::take(&mut state.fused_latch_unhealthy_after_commit)) {
            unhealthy.store(true, Ordering::Release);
        }
        if unhealthy.load(Ordering::Acquire) {
            // SAFETY: code 3 initializes this live output. Upper bits carry
            // native's exact extent and are masked by the cold decoder.
            unsafe {
                control.cold_out.get().write(FastPreselectedRecordResult {
                    terminal: outcome.terminal,
                    record_state: outcome.record_state | (u64::from(exact_record_bytes) << 33),
                })
            };
            return u64::from(CONSUMED_COMMITTED_UNPUBLISHED) | (u64::from(timestamp) << 32);
        }
        // SAFETY: this is the live inner pointer returned by AtomicU64::as_ptr.
        acknowledged.store(sequence, Ordering::Release);
        return u64::from(CONSUMED_PUBLISHED);
    }

    // SAFETY: code 4 is one of the two routes that initializes this output.
    unsafe {
        control.cold_out.get().write(FastPreselectedRecordResult {
            terminal: outcome.terminal,
            record_state: outcome.record_state | (u64::from(exact_record_bytes) << 33),
        })
    };
    u64::from(CONSUMED_OUTCOME)
}

pub(super) unsafe fn mako_rust_fast_one_put_holder_pool_get_view(
    pool: *const FastOnePutHolderPool,
    expected_sequence: u64,
    out: *mut FastOnePutHolderView,
) -> c_int {
    with_state(|state| state.calls.push(Call::FastOnePutHolderPoolGetView));
    if out.is_null() {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: checked output; zeroing on all failures matches native.
    unsafe {
        out.write(FastOnePutHolderView {
            sequence: 0,
            table_id: 0,
            key: ptr::null(),
            value: ptr::null(),
            key_len: 0,
            value_len: 0,
            mako_timestamp: 0,
            reserved: 0,
        })
    };
    if pool.is_null() || expected_sequence == 0 {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: exact-generation caller retains the live fake allocation.
    let fake = unsafe { &*pool.cast::<FakeOnePutHolderPool>() };
    let index = (expected_sequence as usize).wrapping_sub(1) & fake.mask;
    let holder = &fake.holders[index];
    if !holder.sealed || holder.sequence != expected_sequence {
        return sys::MAKO_LOCAL_BUSY;
    }
    let key_len = match u32::try_from(holder.key.len()) {
        Ok(len) => len,
        Err(_) => return sys::MAKO_LOCAL_INTERNAL,
    };
    let value_len = match u32::try_from(holder.value.len()) {
        Ok(len) => len,
        Err(_) => return sys::MAKO_LOCAL_INTERNAL,
    };
    // SAFETY: the sealed holder owns stable key/value vectors until release.
    unsafe {
        out.write(FastOnePutHolderView {
            sequence: holder.sequence,
            table_id: holder.table_id,
            key: holder.key.as_ptr(),
            value: holder.value.as_ptr(),
            key_len,
            value_len,
            mako_timestamp: holder.mako_timestamp,
            reserved: 0,
        })
    };
    sys::MAKO_LOCAL_OK
}

pub(super) unsafe fn mako_rust_fast_one_put_holder_pool_release(
    pool: *mut FastOnePutHolderPool,
    expected_sequence: u64,
) -> c_int {
    with_state(|state| state.calls.push(Call::FastOnePutHolderPoolRelease));
    if pool.is_null() || expected_sequence == 0 {
        return sys::MAKO_LOCAL_INVALID_ARGUMENT;
    }
    // SAFETY: the serialized consumer retains the live fake allocation.
    let fake = unsafe { &mut *pool.cast::<FakeOnePutHolderPool>() };
    let index = (expected_sequence as usize).wrapping_sub(1) & fake.mask;
    let holder = &mut fake.holders[index];
    if !holder.sealed || holder.sequence != expected_sequence {
        return sys::MAKO_LOCAL_BUSY;
    }
    holder.sequence = 0;
    holder.table_id = 0;
    holder.mako_timestamp = 0;
    holder.key.clear();
    holder.value.clear();
    holder.sealed = false;
    sys::MAKO_LOCAL_OK
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
        CommitDisposition, Error, LocalDb, MAX_VALUE_BYTES, TestCleanupBoundary, TestCommitPhase,
        WorkerHealth, arm_test_cleanup_failure, clear_test_cleanup_failure,
        clear_test_commit_observer, features, install_test_commit_observer,
        quarantined_worker_count, worker_health,
    };

    fn unavailable_commit_observer(_phase: TestCommitPhase, _timestamp: u32) {
        panic!("unavailable fake commit observer must not be invoked");
    }

    fn open_db() -> LocalDb {
        LocalDb::open().expect("fake database opens")
    }

    fn claim_cache_order_mode(
        db: &LocalDb,
        mode: crate::CacheOrderMode,
        recovered_sequence: u64,
    ) {
        unsafe {
            db.claim_cache_order_namespace(mode).unwrap()
        };
        // SAFETY: each fake test exclusively owns this newly opened facade and
        // invokes this helper before beginning any ordered transaction.
        unsafe {
            db.reseed_cache_order_namespace(recovered_sequence)
                .unwrap()
        };
    }

    fn claim_cache_order(db: &LocalDb, recovered_sequence: u64) {
        claim_cache_order_mode(
            db,
            crate::CacheOrderMode::Concurrent,
            recovered_sequence,
        );
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
                    unchecked_record_bytes: 0,
                })),
                Call::Insert => push(Step::Insert(ByteReply {
                    status: sys::MAKO_LOCAL_OK,
                    value: u8::MAX,
                    unchecked_record_bytes: 0,
                })),
                Call::Remove => push(Step::Remove(ByteReply {
                    status: sys::MAKO_LOCAL_OK,
                    value: 2,
                    unchecked_record_bytes: 0,
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
            unchecked_record_bytes: 0,
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
            unchecked_record_bytes: 0,
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
            unchecked_record_bytes: 0,
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
        // A non-boolean created field spills into a record-size bit but cannot
        // match this Put's exact v4 extent. The wrapper must fail closed
        // through the checked abort/destroy lifecycle.
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 2,
            unchecked_record_bytes: 0,
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
        assert_eq!(preflight.checksum(), crate::CommitRecordChecksum::Crc32c);
        assert_eq!(last_record_checksum_mode(), Some(1));
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
            exact_record_bytes: 26,
            op_count: 0,
        });
        // Empty plans need no output allocation and therefore remain valid
        // even when the caller's nonempty-record cap is below the selected
        // 26-byte unchecked-v4 framing size.
        let preflight = transaction
            .commit_record_preflight_with_checksum(1, crate::CommitRecordChecksum::None)
            .unwrap();
        assert!(preflight.is_empty());
        assert_eq!(preflight.exact_record_bytes(), 26);
        assert_eq!(preflight.checksum(), crate::CommitRecordChecksum::None);
        assert_eq!(last_record_checksum_mode(), Some(0));
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

    fn exercise_commit_record_raw_target_path() {
        reset();
        let db = open_db();
        let table = db.open_table("trusted-record-target", 39).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::RecordPreflight {
            status: sys::MAKO_LOCAL_OK,
            exact_record_bytes: 8,
            op_count: 1,
        });
        let preflight = transaction.commit_record_preflight(64).unwrap();
        let expected = b"rawbytes".to_vec();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(54),
            exact_record_bytes: 8,
            record: expected.clone(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));

        let mut storage = vec![std::mem::MaybeUninit::<u8>::uninit(); 8];
        let target = crate::CommitRecordTarget::from_raw_parts;
        // SAFETY: `storage` stays allocated and exclusively owned until the
        // synchronous terminal returns, and this callback cannot unwind.
        let report = unsafe {
            transaction.commit_report_with_record_target(|timestamp, bounds| {
                assert_eq!(timestamp.get(), 54);
                assert_eq!(bounds, preflight);
                let sequence = NonZeroU64::new(8).unwrap();
                let bytes = std::ptr::NonNull::new(storage.as_mut_ptr().cast::<u8>()).unwrap();
                Some(target(sequence, bytes, storage.len()))
            })
        };
        assert_eq!(report.commit.disposition, CommitDisposition::Committed);
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        // SAFETY: the exact native completion witness above initialized every
        // byte in the target extent.
        let written =
            unsafe { std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), storage.len()) };
        assert_eq!(written, expected);
        drop(db);
        assert_drained();
    }

    fn exercise_unchecked_one_put_record_terminal() {
        reset();
        let db = open_db();
        let table = db.open_table("trusted-fused-record-target", 40).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        const EXACT_BYTES: u32 = 43 + 3 + 5;
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let candidate = transaction
            .unchecked_one_put_record_candidate()
            .expect("one trusted fast Put advertises a direct v4 candidate");
        assert_eq!(candidate.exact_record_bytes(), EXACT_BYTES as usize);
        assert_eq!(candidate.op_count(), 1);
        assert_eq!(candidate.checksum(), crate::CommitRecordChecksum::None);
        assert_call_count(Call::FastRecordPreflight, 0);

        let expected = vec![0x5a; EXACT_BYTES as usize];
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(55),
            exact_record_bytes: EXACT_BYTES as usize,
            record: expected.clone(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let mut storage = vec![std::mem::MaybeUninit::<u8>::uninit(); EXACT_BYTES as usize];
        let target = crate::CommitRecordTarget::from_raw_parts;
        // SAFETY: storage remains stable and exclusively writable through the
        // synchronous terminal; the callback does not unwind.
        let outcome = unsafe {
            transaction.commit_trusted_unchecked_one_put_record_target(
                candidate,
                |timestamp, bounds| {
                    assert_eq!(timestamp.get(), 55);
                    assert_eq!(bounds, candidate);
                    let sequence = NonZeroU64::new(9).unwrap();
                    let bytes = std::ptr::NonNull::new(storage.as_mut_ptr().cast::<u8>()).unwrap();
                    Some(target(sequence, bytes, storage.len()))
                },
            )
        };
        assert!(outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(report.commit.disposition, CommitDisposition::Committed);
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        assert_eq!(last_unchecked_record_bytes(), Some(EXACT_BYTES));
        assert_call_count(Call::FastUncheckedOnePutRecordCommitDestroy, 1);
        assert_call_count(Call::FastRecordPreflight, 0);
        assert_call_count(Call::FastRecordCommitDestroy, 0);
        // SAFETY: the exact native completion witness initialized the target.
        let written =
            unsafe { std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), storage.len()) };
        assert_eq!(written, expected);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fused-stale", 41).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let stale = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::Get(GetReply::absent()));
        assert_eq!(transaction.get(&table, b"probe"), Ok(None));
        assert_eq!(transaction.unchecked_one_put_record_candidate(), None);
        push(Step::Abort(sys::MAKO_LOCAL_OK));
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        // SAFETY: this stale candidate is intentionally exercised; rejection
        // occurs before the never-called target callback.
        let report = unsafe {
            transaction.commit_report_with_unchecked_one_put_record_target(stale, |_, _| {
                panic!("stale direct candidate must reject before binding")
            })
        };
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::InvalidArgument)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert_call_count(Call::FastUncheckedOnePutRecordCommitDestroy, 0);
        assert_call_count(Call::FastAbortDestroy, 1);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fused-native-recheck", 42).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_INVALID_ARGUMENT,
            timestamp: None,
            exact_record_bytes: EXACT_BYTES as usize,
            record: vec![0; EXACT_BYTES as usize],
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        // SAFETY: candidate is the final trusted one-Put value. Native's
        // modeled shape recheck rejects before this callback.
        let outcome = unsafe {
            transaction.commit_trusted_unchecked_one_put_record_target(candidate, |_, _| {
                panic!("native revalidation failure must reject before binding")
            })
        };
        assert!(!outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::InvalidArgument)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert_call_count(Call::FastUncheckedOnePutRecordCommitDestroy, 1);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        let table = db.open_table("trusted-fused-bind-rejection", 43).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            timestamp: Some(56),
            exact_record_bytes: EXACT_BYTES as usize,
            record: vec![0x5a; EXACT_BYTES as usize],
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        // SAFETY: returning None lends no storage and deliberately exercises
        // the fused trampoline's fail-closed preinstall rejection.
        let report = unsafe {
            transaction.commit_report_with_unchecked_one_put_record_target(
                candidate,
                |timestamp, plan| {
                    assert_eq!(timestamp.get(), 56);
                    assert_eq!(plan, candidate);
                    None
                },
            )
        };
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::CommitHookRejected)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert_call_count(Call::FastUncheckedOnePutRecordCommitDestroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_native_ordered_one_put_record_terminal() {
        const EXACT_BYTES: u32 = 43 + 3 + 5;

        reset();
        let db = open_db();
        claim_cache_order(&db, 10);
        let table = db.open_table("trusted-native-ordered-record", 90).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let expected = vec![0x3d; EXACT_BYTES as usize];
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(91),
            exact_record_bytes: EXACT_BYTES as usize,
            record: expected.clone(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let next_bound = AtomicU64::new(10);
        let unhealthy = AtomicBool::new(false);
        let mut storage = vec![std::mem::MaybeUninit::<u8>::uninit(); EXACT_BYTES as usize];
        let outcome = unsafe {
            transaction.commit_trusted_native_ordered_unchecked_one_put_record_target(
                candidate,
                &next_bound,
                &unhealthy,
                |timestamp, bounds, sequence| {
                    assert_eq!(timestamp.get(), 91);
                    assert_eq!(bounds, candidate);
                    assert_eq!(sequence.get(), 11);
                    let bytes =
                        std::ptr::NonNull::new(storage.as_mut_ptr().cast::<u8>()).unwrap();
                    Some(crate::CommitRecordTarget::from_raw_parts(
                        sequence,
                        bytes,
                        storage.len(),
                    ))
                },
            )
        };
        assert!(outcome.order_witness_valid());
        assert_eq!(
            outcome.accepted_order().map(|(timestamp, sequence)| (
                timestamp.get(),
                sequence.get()
            )),
            Some((91, 11))
        );
        assert!(outcome.is_committed());
        let report = outcome.into_report();
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        assert_eq!(next_bound.load(Ordering::Acquire), 10);
        assert_call_count(Call::FastNativeOrderedUncheckedOnePutRecordCommitDestroy, 1);
        let written = unsafe {
            std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), storage.len())
        };
        assert_eq!(written, expected);
        drop(db);
        assert_drained();

        // A claimed completion witness without a target must never make the
        // compact fast-path predicate publish uninitialized arena bytes.
        reset();
        let db = open_db();
        claim_cache_order(&db, 20);
        let table = db.open_table("trusted-native-ordered-malformed", 91).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(92),
            exact_record_bytes: EXACT_BYTES as usize,
            record: vec![0x7e; EXACT_BYTES as usize],
            reported_written: Some(1),
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let next_bound = AtomicU64::new(20);
        let unhealthy = AtomicBool::new(false);
        let outcome = unsafe {
            transaction.commit_trusted_native_ordered_unchecked_one_put_record_target(
                candidate,
                &next_bound,
                &unhealthy,
                |_, _, _| None,
            )
        };
        assert!(!outcome.is_committed());
        assert_eq!(outcome.accepted_order().unwrap().1.get(), 21);
        let report = outcome.into_report();
        assert!(!report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(!report.record_written);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        claim_cache_order(&db, u64::from(crate::MAX_MAKO_TIMESTAMP));
        let table = db.open_table("trusted-native-ordered-exhausted", 92).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            timestamp: Some(93),
            exact_record_bytes: EXACT_BYTES as usize,
            record: vec![0; EXACT_BYTES as usize],
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let next_bound = AtomicU64::new(u64::from(crate::MAX_MAKO_TIMESTAMP));
        let unhealthy = AtomicBool::new(false);
        let outcome = unsafe {
            transaction.commit_trusted_native_ordered_unchecked_one_put_record_target(
                candidate,
                &next_bound,
                &unhealthy,
                |_, _, _| panic!("an exhausted tail must reject before target binding"),
            )
        };
        assert!(outcome.order_witness_valid());
        assert!(outcome.accepted_order().is_none());
        let report = outcome.into_report();
        assert!(report.completion_contract_valid);
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert_eq!(
            next_bound.load(Ordering::Acquire),
            u64::from(crate::MAX_MAKO_TIMESTAMP)
        );
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        claim_cache_order(&db, 30);
        let table = db.open_table("trusted-native-ordered-bad-timestamp", 93).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            timestamp: Some(crate::MAX_MAKO_TIMESTAMP + 1),
            exact_record_bytes: EXACT_BYTES as usize,
            record: vec![0; EXACT_BYTES as usize],
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let next_bound = AtomicU64::new(30);
        let unhealthy = AtomicBool::new(false);
        let outcome = unsafe {
            transaction.commit_trusted_native_ordered_unchecked_one_put_record_target(
                candidate,
                &next_bound,
                &unhealthy,
                |_, _, _| None,
            )
        };
        assert!(outcome.order_witness_valid());
        assert!(outcome.accepted_order().is_none());
        let report = outcome.into_report();
        assert!(report.completion_contract_valid);
        assert!(!report.record_bound);
        assert!(!report.record_written);
        drop(db);
        assert_drained();
    }

    fn exercise_native_ordered_one_put_arena_terminal() {
        #[repr(C, align(64))]
        struct TestPublicationCell {
            turn: AtomicU64,
            mako_timestamp: std::cell::UnsafeCell<u32>,
            record_bytes: std::cell::UnsafeCell<usize>,
            padding: [u8; 40],
        }

        #[repr(C, align(64))]
        struct TestArenaBlock {
            bytes: [std::mem::MaybeUninit<u8>; 256],
        }

        const _: () = {
            assert!(std::mem::size_of::<TestPublicationCell>() == 64);
            assert!(std::mem::align_of::<TestPublicationCell>() == 64);
            assert!(std::mem::offset_of!(TestPublicationCell, turn) == 0);
            assert!(std::mem::offset_of!(TestPublicationCell, mako_timestamp) == 8);
            assert!(std::mem::offset_of!(TestPublicationCell, record_bytes) == 16);
            assert!(std::mem::size_of::<TestArenaBlock>() == 256);
            assert!(std::mem::align_of::<TestArenaBlock>() == 64);
        };

        const EXACT_BYTES: u32 = 43 + 3 + 5;
        let mut cells = Box::new(std::array::from_fn::<_, 4, _>(|_| TestPublicationCell {
            turn: AtomicU64::new(u64::MAX),
            mako_timestamp: std::cell::UnsafeCell::new(0),
            record_bytes: std::cell::UnsafeCell::new(usize::MAX),
            padding: [0; 40],
        }));
        let mut arena = Box::new(std::array::from_fn::<_, 4, _>(|_| TestArenaBlock {
            bytes: [std::mem::MaybeUninit::uninit(); 256],
        }));

        reset();
        let db = open_db();
        claim_cache_order(&db, 10);
        let table = db.open_table("trusted-native-ordered-arena", 94).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let expected = vec![0x4d; EXACT_BYTES as usize];
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(95),
            exact_record_bytes: EXACT_BYTES as usize,
            record: expected.clone(),
            reported_written: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let next_bound = AtomicU64::new(10);
        let unhealthy = AtomicBool::new(false);
        let index = 11usize & 3;
        cells[index].turn.store((11 >> 2) << 2, Ordering::Relaxed);
        let control = unsafe {
            crate::TrustedNativeOrderedArenaControl::from_raw_parts(
                next_bound.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                cells.as_mut_ptr().cast::<u8>(),
                arena.as_mut_ptr().cast::<u8>(),
                3,
                2,
                64,
                256,
                256,
            )
        };
        let outcome = unsafe {
            transaction.commit_trusted_native_ordered_unchecked_one_put_arena(
                candidate, &control,
            )
        };
        assert!(outcome.order_witness_valid());
        assert_eq!(
            outcome.accepted_order().map(|(timestamp, sequence)| (
                timestamp.get(),
                sequence.get()
            )),
            Some((95, 11))
        );
        assert!(outcome.is_committed());
        let report = outcome.into_report();
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        assert_eq!(next_bound.load(Ordering::Acquire), 10);
        assert_eq!(cells[index].turn.load(Ordering::Acquire), ((11 >> 2) << 2) | 1);
        assert_eq!(unsafe { cells[index].record_bytes.get().read() }, 0);
        let written = unsafe {
            std::slice::from_raw_parts(
                arena[index].bytes.as_ptr().cast::<u8>(),
                EXACT_BYTES as usize,
            )
        };
        assert_eq!(written, expected);
        assert_call_count(Call::FastNativeOrderedUncheckedOnePutArenaCommitDestroy, 1);
        drop(db);
        assert_drained();
    }

    fn exercise_unchecked_one_put_outcome_contract() {
        const EXACT_BYTES: u32 = 43 + 3 + 5;

        // The compact trusted outcome must preserve the general terminal's
        // fail-closed handling of malformed witness and binding combinations.
        // These scripts deliberately model returns that the same-build native
        // terminal promises never to produce.
        for (index, status, timestamp, reported_written, expect_bound) in [
            (0, sys::MAKO_LOCAL_OK, Some(80), Some(0), true),
            (1, sys::MAKO_LOCAL_OK, Some(81), Some(2), true),
            // Final validation cannot conflict after the ordered bind gate.
            (2, sys::MAKO_LOCAL_CONFLICT, Some(82), Some(0), true),
            // A completion witness without a successful bind is impossible.
            (3, sys::MAKO_LOCAL_OK, None, Some(1), false),
        ] {
            reset();
            let db = open_db();
            let table = db
                .open_table("trusted-fused-malformed", 44 + index)
                .unwrap();
            let mut transaction = db.trusted_transaction(&table).unwrap();
            push(Step::Put(ByteReply {
                status: sys::MAKO_LOCAL_OK,
                value: 1,
                unchecked_record_bytes: EXACT_BYTES,
            }));
            assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
            let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
            push(Step::CommitRecord {
                status,
                timestamp,
                exact_record_bytes: EXACT_BYTES as usize,
                record: vec![0x6b; EXACT_BYTES as usize],
                reported_written,
            });
            push(Step::Destroy(sys::MAKO_LOCAL_OK));
            let mut storage = vec![std::mem::MaybeUninit::<u8>::uninit(); EXACT_BYTES as usize];
            // SAFETY: storage remains stable and exclusively writable through
            // the synchronous terminal. The same-build precondition is
            // supplied by this scripted fake, and the callback cannot unwind.
            let outcome = unsafe {
                transaction.commit_trusted_unchecked_one_put_record_target(
                    candidate,
                    |_, bounds| {
                        assert_eq!(bounds, candidate);
                        let sequence = NonZeroU64::new(200 + index).unwrap();
                        let bytes =
                            std::ptr::NonNull::new(storage.as_mut_ptr().cast::<u8>()).unwrap();
                        Some(crate::CommitRecordTarget::from_raw_parts(
                            sequence,
                            bytes,
                            storage.len(),
                        ))
                    },
                )
            };
            assert!(!outcome.is_committed(), "case {index}");
            let report = outcome.into_report();
            assert_eq!(
                report.commit.disposition,
                CommitDisposition::Unknown(Error::Internal),
                "case {index}"
            );
            assert_eq!(report.commit.cleanup, Err(Error::Internal), "case {index}");
            assert!(!report.completion_contract_valid, "case {index}");
            assert_eq!(report.record_bound, expect_bound, "case {index}");
            assert!(!report.record_written, "case {index}");
            drop(db);
            assert_drained();
        }

        // Worker poisoning after native filled a bound record is a valid
        // uncertain-visibility shape. Preserve the written witness so the
        // ordered slot and bytes remain available for fail-stop diagnostics.
        reset();
        let before = quarantined_worker_count().unwrap();
        let db = open_db();
        let table = db.open_table("trusted-fused-written-poisoned", 48).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let expected = vec![0x7c; EXACT_BYTES as usize];
        push(Step::CommitRecord {
            status: sys::MAKO_LOCAL_WORKER_POISONED,
            timestamp: Some(84),
            exact_record_bytes: EXACT_BYTES as usize,
            record: expected.clone(),
            reported_written: Some(1),
        });
        push(Step::Destroy(sys::MAKO_LOCAL_WORKER_POISONED));
        let mut storage = vec![std::mem::MaybeUninit::<u8>::uninit(); EXACT_BYTES as usize];
        // SAFETY: storage remains stable and exclusively writable through the
        // synchronous fake terminal, and the callback cannot unwind.
        let outcome = unsafe {
            transaction.commit_trusted_unchecked_one_put_record_target(candidate, |_, bounds| {
                assert_eq!(bounds, candidate);
                let sequence = NonZeroU64::new(204).unwrap();
                let bytes = std::ptr::NonNull::new(storage.as_mut_ptr().cast::<u8>()).unwrap();
                Some(crate::CommitRecordTarget::from_raw_parts(
                    sequence,
                    bytes,
                    storage.len(),
                ))
            })
        };
        assert!(!outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Unknown(Error::WorkerPoisoned)
        );
        assert_eq!(report.commit.cleanup, Err(Error::WorkerPoisoned));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        // SAFETY: the accepted exact completion witness proves native
        // initialized the full target even though visibility is uncertain.
        let written =
            unsafe { std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), storage.len()) };
        assert_eq!(written, expected);
        assert!(quarantined_worker_count().unwrap() > before);
        drop(db);
        assert_drained();
    }

    fn run_preselected_one_put_case(
        table_id: u64,
        commit: c_int,
        timestamp: Option<u32>,
        reported_record_state: Option<u64>,
        cleanup: c_int,
    ) -> (
        crate::TrustedPreselectedUncheckedOnePutRecordOutcome,
        Vec<std::mem::MaybeUninit<u8>>,
        Vec<u8>,
    ) {
        const EXACT_BYTES: u32 = 43 + 3 + 5;

        reset();
        let db = open_db();
        let table = db
            .open_table("trusted-preselected-one-put", table_id)
            .unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, b"key", b"value"), Ok(true));
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let expected = vec![0x8d; EXACT_BYTES as usize];
        push(Step::CommitPreselectedRecord {
            status: commit,
            timestamp,
            exact_record_bytes: EXACT_BYTES as usize,
            record: expected.clone(),
            reported_record_state,
        });
        push(Step::Destroy(cleanup));
        let mut storage = vec![std::mem::MaybeUninit::<u8>::uninit(); EXACT_BYTES as usize];
        let sequence = NonZeroU64::new(1_000 + table_id).unwrap();
        let bytes = std::ptr::NonNull::new(storage.as_mut_ptr().cast::<u8>()).unwrap();
        // SAFETY: this test owns the only transaction and record terminal for
        // the fake LocalDb. `storage` has the exact advertised extent and stays
        // stable and exclusively writable through the synchronous call.
        let outcome = unsafe {
            let target = crate::CommitRecordTarget::from_raw_parts(sequence, bytes, storage.len());
            transaction.commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
                candidate, target,
            )
        };
        assert_eq!(last_unchecked_record_bytes(), Some(EXACT_BYTES));
        assert_call_count(
            Call::FastPreselectedSingleProducerUncheckedOnePutRecordCommitDestroy,
            1,
        );
        assert_call_count(
            Call::FastSingleProducerUncheckedOnePutRecordCommitDestroy,
            0,
        );
        drop(db);
        assert_drained();
        (outcome, storage, expected)
    }

    fn exercise_preselected_one_put_record_terminal() {
        let (outcome, storage, expected) = run_preselected_one_put_case(
            49,
            sys::MAKO_LOCAL_OK,
            Some(90),
            None,
            sys::MAKO_LOCAL_OK,
        );
        assert_eq!(
            outcome.accepted_timestamp().map(crate::MakoTimestamp::get),
            Some(90)
        );
        assert!(outcome.record_written());
        assert!(outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(report.commit.disposition, CommitDisposition::Committed);
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        // SAFETY: the valid completion witness proves the fake initialized the
        // complete exact target before returning.
        let written =
            unsafe { std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), storage.len()) };
        assert_eq!(written, expected);

        // A clean conflict before the native serializer accepts the target
        // leaves the preselected reservation reusable.
        let (outcome, _, _) = run_preselected_one_put_case(
            50,
            sys::MAKO_LOCAL_CONFLICT,
            None,
            None,
            sys::MAKO_LOCAL_OK,
        );
        assert_eq!(outcome.accepted_timestamp(), None);
        assert!(!outcome.record_written());
        assert!(!outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::Conflict)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(!report.record_bound);
        assert!(!report.record_written);

        // Once the target is accepted and filled, a poisoned terminal preserves
        // its timestamp and witness so the cache can bind and pin the slot.
        let before = quarantined_worker_count().unwrap();
        let (outcome, storage, expected) = run_preselected_one_put_case(
            51,
            sys::MAKO_LOCAL_WORKER_POISONED,
            Some(91),
            None,
            sys::MAKO_LOCAL_WORKER_POISONED,
        );
        assert_eq!(
            outcome.accepted_timestamp().map(crate::MakoTimestamp::get),
            Some(91)
        );
        assert!(outcome.record_written());
        assert!(!outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Unknown(Error::WorkerPoisoned)
        );
        assert_eq!(report.commit.cleanup, Err(Error::WorkerPoisoned));
        assert!(report.completion_contract_valid);
        assert!(report.record_bound);
        assert!(report.record_written);
        // SAFETY: this valid written witness covers the entire exact target.
        let written =
            unsafe { std::slice::from_raw_parts(storage.as_ptr().cast::<u8>(), storage.len()) };
        assert_eq!(written, expected);
        assert!(quarantined_worker_count().unwrap() > before);

        for (index, status, timestamp, record_state, expect_accepted) in [
            // Installed without accepting or writing a durable record.
            (0, sys::MAKO_LOCAL_OK, Some(92), 0, false),
            // Accepted timestamp without the completion witness.
            (1, sys::MAKO_LOCAL_OK, Some(93), 93, true),
            // Completion witness without an accepted timestamp.
            (2, sys::MAKO_LOCAL_OK, None, 1u64 << 32, false),
            // A final conflict after acceptance contradicts the commit gate.
            (
                3,
                sys::MAKO_LOCAL_CONFLICT,
                Some(94),
                94 | (1u64 << 32),
                true,
            ),
            // Bits above the written witness are reserved.
            (
                4,
                sys::MAKO_LOCAL_OK,
                Some(95),
                95 | (1u64 << 32) | (1u64 << 33),
                true,
            ),
            // A nonzero timestamp outside Mako's representable base range.
            (
                5,
                sys::MAKO_LOCAL_OK,
                Some(crate::MAX_MAKO_TIMESTAMP + 1),
                u64::from(crate::MAX_MAKO_TIMESTAMP + 1) | (1u64 << 32),
                false,
            ),
        ] {
            let (outcome, _, _) = run_preselected_one_put_case(
                52 + index,
                status,
                timestamp,
                Some(record_state),
                sys::MAKO_LOCAL_OK,
            );
            assert_eq!(outcome.accepted_timestamp().is_some(), expect_accepted);
            assert!(!outcome.is_committed(), "case {index}");
            let report = outcome.into_report();
            assert_eq!(
                report.commit.disposition,
                CommitDisposition::Unknown(Error::Internal),
                "case {index}"
            );
            assert_eq!(report.commit.cleanup, Err(Error::Internal), "case {index}");
            assert!(!report.completion_contract_valid, "case {index}");
            assert_eq!(report.record_bound, expect_accepted, "case {index}");
            assert!(!report.record_written, "case {index}");
        }
    }

    fn run_preselected_one_put_holder_case(
        table_id: u64,
        sequence: u64,
        commit: c_int,
        timestamp: Option<u32>,
        reported_holder_state: Option<u64>,
        cleanup: c_int,
    ) -> (
        LocalDb,
        crate::TrustedOnePutHolderPool,
        crate::TrustedPreselectedUncheckedOnePutHolderOutcome,
        NonZeroU64,
    ) {
        const KEY: &[u8] = b"holder\0key";
        const VALUE: &[u8] = b"holder-value\xff";
        const EXACT_BYTES: u32 = 43 + KEY.len() as u32 + VALUE.len() as u32;

        reset();
        let pool = crate::TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let db = open_db();
        let table = db.open_table("trusted-one-put-holder", table_id).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, KEY, VALUE), Ok(true));
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        push(Step::CommitPreselectedHolder {
            status: commit,
            timestamp,
            exact_record_bytes: EXACT_BYTES as usize,
            table_id,
            key: KEY.to_vec(),
            value: VALUE.to_vec(),
            reported_holder_state,
        });
        push(Step::Destroy(cleanup));
        let sequence = NonZeroU64::new(sequence).unwrap();
        // SAFETY: the fake test owns the sole producer and exact pool
        // generation, and this is the candidate's immediate consuming call.
        let outcome = unsafe {
            transaction.commit_trusted_preselected_single_producer_unchecked_one_put_holder(
                candidate, &pool, sequence,
            )
        };
        assert_eq!(last_unchecked_record_bytes(), Some(EXACT_BYTES));
        assert_call_count(
            Call::FastPreselectedSingleProducerUncheckedOnePutHolderCommitDestroy,
            1,
        );
        assert_call_count(
            Call::FastPreselectedSingleProducerUncheckedOnePutRecordCommitDestroy,
            0,
        );
        (db, pool, outcome, sequence)
    }

    fn release_actual_holder(pool: &crate::TrustedOnePutHolderPool, sequence: NonZeroU64) {
        // SAFETY: each scripted accepted terminal is synchronously published
        // for this single-thread fake before this sole-consumer observation.
        let view = unsafe { pool.view(sequence) }.expect("accepted fake holder has a view");
        // No fake backend borrow remains after this point.
        unsafe { pool.release(view) }.expect("exact fake holder release succeeds");
    }

    fn exercise_preselected_one_put_holder_terminal() {
        reset();
        assert!(matches!(
            crate::TrustedOnePutHolderPool::new(0, 0, 0),
            Err(Error::InvalidArgument)
        ));
        assert!(matches!(
            crate::TrustedOnePutHolderPool::new(3, 0, 0),
            Err(Error::InvalidArgument)
        ));
        assert!(matches!(
            crate::TrustedOnePutHolderPool::new(4, 0, (MAX_VALUE_BYTES + 1) as u32),
            Err(Error::ValueTooLarge)
        ));
        let pool = crate::TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        assert_eq!(pool.capacity(), 4);
        drop(pool);
        assert_call_count(Call::FastOnePutHolderPoolCreate, 1);
        assert_call_count(Call::FastOnePutHolderPoolDestroy, 1);
        assert_drained();

        let (db, pool, outcome, sequence) = run_preselected_one_put_holder_case(
            70,
            1001,
            sys::MAKO_LOCAL_OK,
            Some(120),
            None,
            sys::MAKO_LOCAL_OK,
        );
        assert_eq!(
            outcome.accepted_timestamp().map(crate::MakoTimestamp::get),
            Some(120)
        );
        assert!(outcome.holder_sealed());
        assert!(outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(report.commit.disposition, CommitDisposition::Committed);
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(report.holder_bound);
        assert!(report.holder_sealed);

        let wrong = NonZeroU64::new(sequence.get() + 1).unwrap();
        // SAFETY: the sole consumer is checking an exact generation after the
        // scripted publication; native rejects the unsealed adjacent slot.
        assert!(matches!(unsafe { pool.get_view(wrong) }, Err(Error::Busy)));
        // SAFETY: same sole-consumer publication proof for the accepted slot.
        let view = unsafe { pool.view(sequence) }.unwrap();
        assert_eq!(view.sequence(), sequence);
        assert_eq!(view.table_id(), 70);
        assert_eq!(view.mako_timestamp().get(), 120);
        assert_eq!(view.key(), b"holder\0key");
        assert_eq!(view.value(), b"holder-value\xff");
        // Native destruction must reject a sealed holder without invalidating
        // either this live view or the pool.
        assert_eq!(
            unsafe { mako_rust_fast_one_put_holder_pool_destroy(pool.raw.as_ptr()) },
            sys::MAKO_LOCAL_BUSY
        );
        // SAFETY: all slice borrows above ended and the fake backend is done.
        unsafe { pool.release(view) }.unwrap();
        // SAFETY: this exact generation was just released and must now reject.
        assert!(matches!(
            unsafe { pool.get_view(sequence) },
            Err(Error::Busy)
        ));
        assert_eq!(
            unsafe {
                mako_rust_fast_one_put_holder_pool_release(pool.raw.as_ptr(), sequence.get())
            },
            sys::MAKO_LOCAL_BUSY
        );
        drop(pool);
        drop(db);
        assert_drained();

        // A clean pre-acceptance conflict neither consumes the sequence nor
        // seals a holder generation.
        let (db, pool, outcome, sequence) = run_preselected_one_put_holder_case(
            71,
            1002,
            sys::MAKO_LOCAL_CONFLICT,
            None,
            None,
            sys::MAKO_LOCAL_OK,
        );
        assert_eq!(outcome.accepted_timestamp(), None);
        assert!(!outcome.holder_sealed());
        assert!(!outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::Conflict)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(!report.holder_bound);
        assert!(!report.holder_sealed);
        // SAFETY: no accepted publication exists for this exact generation.
        assert!(matches!(
            unsafe { pool.get_view(sequence) },
            Err(Error::Busy)
        ));
        drop(pool);
        drop(db);
        assert_drained();

        // Long-key holder preparation may fail before native enters commit.
        // Native completes abort cleanup and returns a definite unbound OOM,
        // which must not poison the dense sequence.
        let (db, pool, outcome, sequence) = run_preselected_one_put_holder_case(
            72,
            1003,
            sys::MAKO_LOCAL_OUT_OF_MEMORY,
            None,
            None,
            sys::MAKO_LOCAL_OK,
        );
        assert_eq!(outcome.accepted_timestamp(), None);
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::OutOfMemory)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.completion_contract_valid);
        assert!(!report.holder_bound);
        assert!(!report.holder_sealed);
        // SAFETY: a pre-acceptance allocation failure leaves this holder free.
        assert!(matches!(unsafe { pool.view(sequence) }, Err(Error::Busy)));
        drop(pool);
        drop(db);
        assert_drained();

        // Cleanup uncertainty after acceptance keeps a complete holder for the
        // cache to publish and pin.
        let before = quarantined_worker_count().unwrap();
        let (db, pool, outcome, sequence) = run_preselected_one_put_holder_case(
            73,
            1004,
            sys::MAKO_LOCAL_WORKER_POISONED,
            Some(121),
            None,
            sys::MAKO_LOCAL_WORKER_POISONED,
        );
        assert_eq!(
            outcome.accepted_timestamp().map(crate::MakoTimestamp::get),
            Some(121)
        );
        assert!(outcome.holder_sealed());
        assert!(!outcome.is_committed());
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Unknown(Error::WorkerPoisoned)
        );
        assert_eq!(report.commit.cleanup, Err(Error::WorkerPoisoned));
        assert!(report.completion_contract_valid);
        assert!(report.holder_bound);
        assert!(report.holder_sealed);
        assert!(quarantined_worker_count().unwrap() > before);
        release_actual_holder(&pool, sequence);
        drop(pool);
        drop(db);
        assert_drained();

        for (index, status, timestamp, holder_state, expect_bound) in [
            // Installed without exposing the accepted holder generation.
            (0, sys::MAKO_LOCAL_OK, Some(122), 0, false),
            // Accepted timestamp without the sealed witness.
            (1, sys::MAKO_LOCAL_OK, Some(123), 123, true),
            // Sealed witness without an accepted timestamp.
            (2, sys::MAKO_LOCAL_OK, None, 1u64 << 32, false),
            // A final conflict after holder acceptance is impossible.
            (
                3,
                sys::MAKO_LOCAL_CONFLICT,
                Some(124),
                124 | (1u64 << 32),
                true,
            ),
            // Bits above the sealed witness are reserved.
            (
                4,
                sys::MAKO_LOCAL_OK,
                Some(125),
                125 | (1u64 << 32) | (1u64 << 33),
                true,
            ),
            // Timestamp exceeds Mako's representable base range.
            (
                5,
                sys::MAKO_LOCAL_OK,
                Some(crate::MAX_MAKO_TIMESTAMP + 1),
                u64::from(crate::MAX_MAKO_TIMESTAMP + 1) | (1u64 << 32),
                false,
            ),
        ] {
            let (db, pool, outcome, sequence) = run_preselected_one_put_holder_case(
                74 + index,
                1101 + index,
                status,
                timestamp,
                Some(holder_state),
                sys::MAKO_LOCAL_OK,
            );
            assert_eq!(outcome.accepted_timestamp().is_some(), expect_bound);
            assert!(!outcome.is_committed(), "case {index}");
            let report = outcome.into_report();
            assert_eq!(
                report.commit.disposition,
                CommitDisposition::Unknown(Error::Internal),
                "case {index}"
            );
            assert_eq!(report.commit.cleanup, Err(Error::Internal), "case {index}");
            assert!(!report.completion_contract_valid, "case {index}");
            assert_eq!(report.holder_bound, expect_bound, "case {index}");
            assert!(!report.holder_sealed, "case {index}");
            // The fake holder follows the actual scripted acceptance, not the
            // corrupted reported state. Release it so pool Drop remains a
            // useful sealed-generation diagnostic.
            if let Some(timestamp) = timestamp {
                if crate::MakoTimestamp::new(timestamp).is_some() {
                    release_actual_holder(&pool, sequence);
                } else {
                    // The safe view correctly rejects this deliberately
                    // malformed native timestamp. Use the fake raw boundary
                    // only to clean up the test allocation afterward.
                    assert_eq!(
                        unsafe {
                            mako_rust_fast_one_put_holder_pool_release(
                                pool.raw.as_ptr(),
                                sequence.get(),
                            )
                        },
                        sys::MAKO_LOCAL_OK
                    );
                }
            }
            drop(pool);
            drop(db);
            assert_drained();
        }
    }

    fn exercise_fused_one_put_holder_terminal() {
        const KEY: &[u8] = b"fused-key";
        const VALUE: &[u8] = b"fused-value";
        const EXACT_BYTES: u32 = 43 + KEY.len() as u32 + VALUE.len() as u32;

        // Exact healthy success consumes the facade and publishes ACK as the
        // canonical producer tail without returning the cold two-word outcome.
        reset();
        let pool = crate::TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let acknowledged = AtomicU64::new(0);
        let unhealthy = AtomicBool::new(false);
        // SAFETY: every object remains in this stack frame through the calls;
        // these are the exact atomic inner pointers and matching pool/capacity.
        let control = unsafe {
            crate::TrustedSpscOnePutHolderControl::new(
                &pool,
                acknowledged.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                NonZeroU64::new(4).unwrap(),
                std::num::NonZeroU32::new(EXACT_BYTES).unwrap(),
            )
        };
        let next = AtomicU64::new(0);
        let capacity_limit = AtomicU64::new(4);
        let db = open_db();
        let table = db.open_table("trusted-fused-holder", 80).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        assert_eq!(transaction.put(&table, KEY, VALUE), Ok(true));
        push(Step::CommitPreselectedHolder {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(140),
            exact_record_bytes: EXACT_BYTES as usize,
            table_id: 80,
            key: KEY.to_vec(),
            value: VALUE.to_vec(),
            reported_holder_state: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        // SAFETY: this test owns the only transaction/producer and all stable
        // control words for the synchronous call.
        let attempt = unsafe {
            transaction.try_commit_trusted_fused_single_producer_one_put_holder(
                &control,
                next.as_ptr(),
                capacity_limit.as_ptr(),
            )
        };
        assert!(matches!(
            attempt,
            crate::TrustedFusedOnePutHolderAttempt::Published
        ));
        assert_eq!(next.load(Ordering::Relaxed), 0);
        assert_eq!(acknowledged.load(Ordering::Acquire), 1);
        assert_call_count(Call::FastFusedSingleProducerOnePutHolderTryCommitDestroy, 1);
        assert_call_count(
            Call::FastPreselectedSingleProducerUncheckedOnePutHolderCommitDestroy,
            0,
        );
        release_actual_holder(&pool, NonZeroU64::new(1).unwrap());
        drop(transaction);
        drop(control);
        drop(pool);
        drop(db);
        assert_drained();

        // A full cached window is explicitly untouched. Refreshing the cached
        // capacity limit and retrying the same facade then commits sequence 5.
        reset();
        let pool = crate::TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let acknowledged = AtomicU64::new(4);
        let unhealthy = AtomicBool::new(false);
        let control = unsafe {
            crate::TrustedSpscOnePutHolderControl::new(
                &pool,
                acknowledged.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                NonZeroU64::new(4).unwrap(),
                std::num::NonZeroU32::new(EXACT_BYTES).unwrap(),
            )
        };
        let next = AtomicU64::new(4);
        let capacity_limit = AtomicU64::new(4);
        let db = open_db();
        let table = db.open_table("trusted-fused-holder-retry", 81).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, KEY, VALUE).unwrap();
        let attempt = unsafe {
            transaction.try_commit_trusted_fused_single_producer_one_put_holder(
                &control,
                next.as_ptr(),
                capacity_limit.as_ptr(),
            )
        };
        assert!(matches!(
            attempt,
            crate::TrustedFusedOnePutHolderAttempt::UntouchedSlow {
                exact_record_bytes
            } if exact_record_bytes.get() == EXACT_BYTES
        ));
        assert_eq!(next.load(Ordering::Relaxed), 4);
        assert_eq!(acknowledged.load(Ordering::Relaxed), 4);
        capacity_limit.store(8, Ordering::Relaxed);
        push(Step::CommitPreselectedHolder {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(141),
            exact_record_bytes: EXACT_BYTES as usize,
            table_id: 81,
            key: KEY.to_vec(),
            value: VALUE.to_vec(),
            reported_holder_state: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let attempt = unsafe {
            transaction.try_commit_trusted_fused_single_producer_one_put_holder(
                &control,
                next.as_ptr(),
                capacity_limit.as_ptr(),
            )
        };
        assert!(matches!(
            attempt,
            crate::TrustedFusedOnePutHolderAttempt::Published
        ));
        assert_eq!(next.load(Ordering::Relaxed), 4);
        assert_eq!(acknowledged.load(Ordering::Acquire), 5);
        release_actual_holder(&pool, NonZeroU64::new(5).unwrap());
        drop(transaction);
        drop(control);
        drop(pool);
        drop(db);
        assert_drained();

        // Model the fail-stop bit latching after native acceptance. ACK remains
        // unchanged; after native's second Acquire returns the diversion, the
        // Rust decoder advances its cursor and receives the exact timestamp and
        // extent for cold attachment.
        reset();
        let pool = crate::TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let acknowledged = AtomicU64::new(0);
        let unhealthy = AtomicBool::new(false);
        let control = unsafe {
            crate::TrustedSpscOnePutHolderControl::new(
                &pool,
                acknowledged.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                NonZeroU64::new(4).unwrap(),
                std::num::NonZeroU32::new(EXACT_BYTES).unwrap(),
            )
        };
        let next = AtomicU64::new(0);
        let capacity_limit = AtomicU64::new(4);
        let db = open_db();
        let table = db.open_table("trusted-fused-holder-race", 82).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, KEY, VALUE).unwrap();
        // Rust retires its conservative mirror before rejecting this call,
        // while native's unchanged one-Put witness remains authoritative.
        // The consumed cold result must therefore carry native's exact extent.
        let oversized_key = vec![0; crate::MAX_KEY_BYTES + 1];
        assert_eq!(
            transaction.put(&table, &oversized_key, VALUE),
            Err(crate::Error::ValueTooLarge)
        );
        push(Step::CommitPreselectedHolder {
            status: sys::MAKO_LOCAL_OK,
            timestamp: Some(142),
            exact_record_bytes: EXACT_BYTES as usize,
            table_id: 82,
            key: KEY.to_vec(),
            value: VALUE.to_vec(),
            reported_holder_state: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        latch_fused_unhealthy_after_commit_once();
        let attempt = unsafe {
            transaction.try_commit_trusted_fused_single_producer_one_put_holder(
                &control,
                next.as_ptr(),
                capacity_limit.as_ptr(),
            )
        };
        assert!(matches!(
            attempt,
            crate::TrustedFusedOnePutHolderAttempt::CommittedUnpublished {
                timestamp,
                exact_record_bytes,
            } if timestamp.get() == 142 && exact_record_bytes.get() == EXACT_BYTES
        ));
        assert_eq!(next.load(Ordering::Relaxed), 1);
        assert_eq!(acknowledged.load(Ordering::Acquire), 0);
        assert!(unhealthy.load(Ordering::Acquire));
        release_actual_holder(&pool, NonZeroU64::new(1).unwrap());
        drop(transaction);
        drop(control);
        drop(pool);
        drop(db);
        assert_drained();

        // A definite conflict is consuming but does not advance the retained
        // future generation. The full terminal remains available cold.
        reset();
        let pool = crate::TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let acknowledged = AtomicU64::new(0);
        let unhealthy = AtomicBool::new(false);
        let control = unsafe {
            crate::TrustedSpscOnePutHolderControl::new(
                &pool,
                acknowledged.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                NonZeroU64::new(4).unwrap(),
                std::num::NonZeroU32::new(EXACT_BYTES).unwrap(),
            )
        };
        let next = AtomicU64::new(0);
        let capacity_limit = AtomicU64::new(4);
        let db = open_db();
        let table = db.open_table("trusted-fused-holder-conflict", 83).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, KEY, VALUE).unwrap();
        let oversized_key = vec![0; crate::MAX_KEY_BYTES + 1];
        assert_eq!(
            transaction.put(&table, &oversized_key, VALUE),
            Err(crate::Error::ValueTooLarge)
        );
        push(Step::CommitPreselectedHolder {
            status: sys::MAKO_LOCAL_CONFLICT,
            timestamp: None,
            exact_record_bytes: EXACT_BYTES as usize,
            table_id: 83,
            key: KEY.to_vec(),
            value: VALUE.to_vec(),
            reported_holder_state: None,
        });
        push(Step::Destroy(sys::MAKO_LOCAL_OK));
        let attempt = unsafe {
            transaction.try_commit_trusted_fused_single_producer_one_put_holder(
                &control,
                next.as_ptr(),
                capacity_limit.as_ptr(),
            )
        };
        let crate::TrustedFusedOnePutHolderAttempt::ConsumedOutcome {
            outcome,
            exact_record_bytes,
        } = attempt
        else {
            panic!("conflict must return the consumed cold outcome")
        };
        assert_eq!(exact_record_bytes.get(), EXACT_BYTES);
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::Conflict)
        );
        assert!(report.completion_contract_valid);
        assert!(!report.holder_bound);
        assert_eq!(next.load(Ordering::Relaxed), 0);
        assert_eq!(acknowledged.load(Ordering::Relaxed), 0);
        drop(transaction);
        drop(control);
        drop(pool);
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

    fn exercise_cache_order_mode_and_clock_contract() {
        const EXACT_BYTES: u32 = 43 + 3 + 5;

        reset();
        let db = open_db();
        claim_cache_order(&db, 7);
        assert_eq!(
            unsafe { db.claim_cache_order_namespace(crate::CacheOrderMode::Concurrent) },
            Err(Error::Busy)
        );
        db.order_record_validation_prefix();
        let observed = crate::MakoTimestamp::new(17).unwrap();
        crate::advance_mako_timestamp_past(observed).unwrap();
        let concurrent_snapshot = db.cache_order_snapshot();
        assert_eq!(concurrent_snapshot & CACHE_ORDER_FIELD_MASK, 7);
        assert_eq!(
            (concurrent_snapshot & CACHE_ORDER_TIMESTAMP_MASK) >> CACHE_ORDER_TIMESTAMP_SHIFT,
            18
        );
        db.close().unwrap();
        assert_drained();

        let db = open_db();
        claim_cache_order_mode(&db, crate::CacheOrderMode::SingleProducer, 9);
        let single_producer_snapshot = db.cache_order_snapshot();
        assert_eq!(single_producer_snapshot & CACHE_ORDER_FIELD_MASK, 9);
        assert_eq!(
            (single_producer_snapshot & CACHE_ORDER_TIMESTAMP_MASK)
                >> CACHE_ORDER_TIMESTAMP_SHIFT,
            18
        );
        let cut = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            db.order_record_validation_prefix();
        }));
        assert!(cut.is_err(), "SingleProducer mode must reject the packed cut");
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        claim_cache_order(&db, 0);
        let table = db.open_table("mode-generic-reject", 110).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let outcome = unsafe {
            transaction.commit_trusted_unchecked_one_put_record_target(candidate, |_, _| {
                panic!("Concurrent mode must reject before the legacy callback")
            })
        };
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::InvalidArgument)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(!report.record_bound);
        assert!(!report.record_written);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        claim_cache_order(&db, 0);
        let table = db.open_table("mode-single-producer-reject", 111).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let outcome = unsafe {
            transaction.commit_trusted_single_producer_unchecked_one_put_record_target(
                candidate,
                |_, _| panic!("Concurrent mode must reject before the SP callback"),
            )
        };
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::InvalidArgument)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(!report.record_bound);
        assert!(!report.record_written);
        drop(db);
        assert_drained();

        reset();
        let db = open_db();
        claim_cache_order_mode(&db, crate::CacheOrderMode::SingleProducer, 13);
        let packed_reject_snapshot = db.cache_order_snapshot();
        let table = db.open_table("mode-packed-reject", 112).unwrap();
        let mut transaction = db.trusted_transaction(&table).unwrap();
        push(Step::Put(ByteReply {
            status: sys::MAKO_LOCAL_OK,
            value: 1,
            unchecked_record_bytes: EXACT_BYTES,
        }));
        transaction.put(&table, b"key", b"value").unwrap();
        let candidate = transaction.unchecked_one_put_record_candidate().unwrap();
        let next_bound = AtomicU64::new(13);
        let unhealthy = AtomicBool::new(false);
        let outcome = unsafe {
            transaction.commit_trusted_native_ordered_unchecked_one_put_record_target(
                candidate,
                &next_bound,
                &unhealthy,
                |_, _, _| panic!("SingleProducer mode must reject before packed binding"),
            )
        };
        assert!(outcome.order_witness_valid());
        assert_eq!(outcome.accepted_order(), None);
        let report = outcome.into_report();
        assert_eq!(
            report.commit.disposition,
            CommitDisposition::Aborted(Error::InvalidArgument)
        );
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(!report.record_bound);
        assert!(!report.record_written);
        assert_eq!(next_bound.load(Ordering::Acquire), 13);
        assert_eq!(db.cache_order_snapshot(), packed_reject_snapshot);
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
        exercise_commit_record_raw_target_path();
        exercise_unchecked_one_put_record_terminal();
        exercise_native_ordered_one_put_record_terminal();
        exercise_native_ordered_one_put_arena_terminal();
        exercise_unchecked_one_put_outcome_contract();
        exercise_preselected_one_put_record_terminal();
        exercise_preselected_one_put_holder_terminal();
        exercise_fused_one_put_holder_terminal();
        exercise_commit_record_preflight_fail_closed();
        exercise_commit_record_hook_outcomes();
        exercise_commit_record_completion_witness();
        exercise_cache_order_mode_and_clock_contract();
    }
}
