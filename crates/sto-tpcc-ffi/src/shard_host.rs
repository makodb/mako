//! Storage verbs of Mako's native sharding host on the Rust engine.
//!
//! The sharding crate reads and writes the rows of a migrating range through
//! a table of C callbacks. This module is the engine side of the storage
//! slots for a database that runs on this crate: the lookahead `scan_next`,
//! the one-row writes `put` and `remove`, and the registry that leads from
//! the crate's `(catalog table, owner)` to a native table and from the calling
//! OS thread to its engine handle.
//!
//! The module declares the C types it needs for itself. It neither depends on
//! nor links the sharding crate; the C++ glue assigns the exports to the
//! callback slots and checks the layout on its side.
//!
//! Every export contains panics and answers with a sharding status. An engine
//! outcome that is not definite stops the process, because the callback
//! contract has no status for "may or may not have happened".
//!
//! This module is the shell around `shard-host-core`, the crate Verus checks.
//! The decisions that crate holds are made there and only there: what a verb
//! answers for an engine result (`map_engine`), and which arguments
//! `scan_next` refuses, when a cursor ends the range and where the engine's
//! scan begins (`scan_flags_valid`, `scan_arguments`). The module adds no rule
//! of its own to them; `scripts/verify_shard_host_core.sh` refuses a second
//! definition of one of the core's names here.
use super::*;
use mako_clock::distributed::DistributedTerm;
// RETRY is answered only through `map_engine`; the tests name it.
#[cfg(test)]
use shard_host_core::SHARD_RETRY;
use shard_host_core::{
    map_engine, scan_arguments, scan_flags_valid, Mapped, ScanLower, ScanRefusal, ScanStart, Stage,
    ENGINE_FATAL, ENGINE_OK, SHARD_EXHAUSTED, SHARD_INVALID, SHARD_IO, SHARD_NOT_FOUND, SHARD_OK,
};
use std::{
    ffi::c_void,
    io::Write as _,
    sync::{PoisonError, RwLock},
};
use sto_masstree::{ReadVisibility, VersionedLookahead};

/// A borrowed byte span, laid out as `MakoShardBytes` of the sharding host's
/// C header. `data` is not looked at when `len` is zero.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MakoStoShardBytes {
    pub data: *const u8,
    pub len: usize,
}

/// Receives one row as coordinate, key and value, like `MakoShardRowSink`.
/// The spans are valid for the call only.
pub type MakoStoShardRowSink =
    unsafe extern "C" fn(*mut c_void, MakoStoShardBytes, MakoStoShardBytes, MakoStoShardBytes);

#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::size_of::<MakoStoShardBytes>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoShardBytes, data)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoShardBytes, len)];

// The verified crate names the engine's statuses by number, because Verus does
// not evaluate `Status::code` in a constant. The build fails here if one of
// its numbers is not the engine's.
const _: () = {
    assert!(shard_host_core::ENGINE_OK == Status::Ok.code());
    assert!(shard_host_core::ENGINE_RETRY == Status::Retry.code());
    assert!(shard_host_core::ENGINE_FATAL == Status::Fatal.code());
    assert!(shard_host_core::ENGINE_EXHAUSTED == Status::ResourceExhausted.code());
    assert!(shard_host_core::ENGINE_UNKNOWN == Status::Unknown.code());
    assert!(shard_host_core::ENGINE_COMMITTED_ERROR == Status::CommittedError.code());
};

/// The one engine status the verbs read that the mapping does not tell apart.
const ENGINE_MISS: i32 = Status::Miss.code();

/// The term of a non-replicated node. Migration is refused on a replicated
/// one, and the C++ glue refuses to hand these verbs to it.
const TERM: u32 = 0;

/// Directory entries one lookahead transaction walks at most.
///
/// A deleted key stays in the table's directory, and every entry a scan walks
/// is an item of its transaction, which may hold 1,024 of them under the C++
/// adapter. `scan_next` therefore crosses a run of deleted keys in read-only
/// transactions of this many entries, each certified and finished before the
/// next begins.
///
/// Every such transaction also reads the table's directory generation, which
/// advances whenever any thread first touches a key the table has never held.
/// During a migration of this slice only the calling thread uses the table.
/// Once ordinary workers run next to a migration, a lookahead that has to
/// cross many deleted keys will often fail certification and answer RETRY; a
/// lookahead that does not depend on the generation may be needed then. None
/// is built here.
const SCAN_HOP_ENTRIES: usize = 256;

/// The longest key and the longest value `put` and `remove` accept. The
/// protocol's write ledger and its log record keep both lengths in 16 bits.
const MAX_LOGGED_BYTES: usize = u16::MAX as usize;

// Operations of `sto_tpcc_protocol_mutate`.
const PROTOCOL_UPSERT: u8 = 0;
const PROTOCOL_DELETE: u8 = 2;
/// The action of a protocol request that hands over the log record.
const PROTOCOL_LOG: u32 = 5;

const FIRST_SCAN_NEXT: u8 = 1;
const FIRST_PUT: u8 = 2;
const FIRST_REMOVE: u8 = 4;

/// The binding of the calling OS thread, made by
/// `mako_sto_shard_thread_register`.
#[derive(Clone, Copy)]
struct Entered {
    /// The callback context as an address. A token only, never dereferenced.
    context: usize,
    owner: u32,
    /// This OS thread's engine handle. Owned by the C++ adapter.
    thread: *mut StoTpccThread,
}

thread_local! {
    static ENTERED: Cell<Option<Entered>> = const { Cell::new(None) };
    /// One bit per verb that has written its first-use line since this
    /// thread was registered.
    static FIRST_USE: Cell<u8> = const { Cell::new(0) };
}

#[cfg(test)]
thread_local! {
    /// Runs once between a lookahead's walk and its certification. Test only.
    static AFTER_WALK: Cell<Option<Box<dyn FnOnce()>>> = const { Cell::new(None) };
}

#[derive(Clone, Copy)]
struct TableEntry {
    /// The sharding catalog's id of the table.
    catalog_table: u64,
    owner: u32,
    /// Owned by the C++ adapter's index.
    table: *const StoTpccTable,
    /// The adapter's physical table id. It only labels the write in the
    /// protocol's log record.
    table_id: u16,
}

// SAFETY: The registry stores the pointer as plain data. It is dereferenced
// only inside a verb on a registered thread while the entry is registered,
// and the registration contract keeps the table alive for that long.
unsafe impl Send for TableEntry {}
// SAFETY: As above; the registry never reaches through the pointer.
unsafe impl Sync for TableEntry {}

/// Per-entry registration and removal only: several databases of one process
/// (the Rust tests) register here side by side.
static TABLES: RwLock<Vec<TableEntry>> = RwLock::new(Vec::new());

/// Writes one line to standard error with a single write, so that lines of
/// other threads cannot split it.
fn diagnostic(arguments: fmt::Arguments<'_>) {
    let line = format!("mako_sto_shard_host: {arguments}\n");
    let _ = std::io::stderr().write_all(line.as_bytes());
}

/// Stops the process: the engine cannot say whether the effect happened, and
/// the callback contract cannot report that.
fn fail_stop(operation: &str) -> ! {
    let error = LAST_ERROR.with(|slot| {
        slot.try_borrow()
            .map(|error| String::from_utf8_lossy(error.as_bytes()).into_owned())
            .unwrap_or_default()
    });
    diagnostic(format_args!(
        "{operation}: the engine outcome is not definite, stopping the process \
         (last engine error: {error})"
    ));
    std::process::abort()
}

fn shard_status(operation: &str, mapped: Mapped) -> u32 {
    match mapped {
        Mapped::Shard(status) => status,
        Mapped::FailStop => fail_stop(operation),
    }
}

/// Closes the attempt of a failed step and maps that step's engine status.
/// Closing is a no-op that answers OK when nothing is open.
///
/// # Safety
/// `thread` is the registered handle of the calling thread.
unsafe fn close_and_map(operation: &str, thread: *mut StoTpccThread, status: i32) -> u32 {
    let mut report = StoTpccProtocolReport::default();
    // SAFETY: Guaranteed by the caller; the report is a live local.
    if unsafe { sto_tpcc_protocol_finish(thread, 0, &mut report) } != ENGINE_OK {
        fail_stop(operation);
    }
    shard_status(
        operation,
        map_engine(Stage::Closed, status, report.disposition),
    )
}

/// Certifies the reads of the open attempt, which staged no write, and
/// finishes it committed. OK only when it did; otherwise the status the verb
/// answers, with the attempt closed.
///
/// A validated attempt without writes finishes committed without an install
/// and without reserving a timestamp.
///
/// # Safety
/// `thread` is the registered handle of the calling thread.
unsafe fn certify_read_only(operation: &str, thread: *mut StoTpccThread) -> u32 {
    // SAFETY: Guaranteed by the caller.
    let validated = unsafe { sto_tpcc_protocol_validate(thread) };
    if validated != ENGINE_OK {
        // SAFETY: As above.
        return unsafe { close_and_map(operation, thread, validated) };
    }
    let mut report = StoTpccProtocolReport::default();
    // SAFETY: As above; the report is a live local.
    let finished = unsafe { sto_tpcc_protocol_finish(thread, 1, &mut report) };
    shard_status(
        operation,
        map_engine(Stage::ReadOnlyFinish, finished, report.disposition),
    )
}

/// Runs the body of a registration export. A panic there has nothing to close.
fn contain_registration(operation: &str, body: impl FnOnce() -> u32) -> u32 {
    catch_unwind(AssertUnwindSafe(body)).unwrap_or_else(|_| {
        diagnostic(format_args!("{operation} contained a panic"));
        SHARD_IO
    })
}

/// Runs the body of a storage verb.
///
/// The protocol exports the verbs call contain their own panics, so a panic
/// that arrives here comes from the glue around them and never from after an
/// install. The attempt it may have left open is closed as aborted.
fn contain_verb(operation: &str, body: impl FnOnce() -> u32) -> u32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(status) => status,
        Err(_) => {
            diagnostic(format_args!("{operation} contained a panic"));
            let Some(entered) = ENTERED.get() else {
                return SHARD_IO;
            };
            let mut report = StoTpccProtocolReport::default();
            // SAFETY: The registration contract keeps this thread's handle
            // alive until the thread is unregistered.
            if unsafe { sto_tpcc_protocol_finish(entered.thread, 0, &mut report) } != ENGINE_OK {
                fail_stop(operation);
            }
            SHARD_IO
        }
    }
}

fn log_first_use(bit: u8, verb: &str, owner: u32, catalog_table: u64) {
    let used = FIRST_USE.get();
    if used & bit == 0 {
        FIRST_USE.set(used | bit);
        diagnostic(format_args!(
            "first {verb} owner={owner} table={catalog_table}"
        ));
    }
}

/// Registers the engine table that holds `catalog_table` on `owner`.
///
/// INVALID for a null or misaligned pointer, for a `table_id` outside
/// `1..0x8000` (the protocol's log record has no room for another), when the
/// key is registered with another pointer or id, and when the pointer is
/// registered under another key. OK without a change when the identical entry
/// exists. EXHAUSTED when the registry cannot grow. Any thread may call it.
///
/// Only a raw table may be registered: the verbs treat a row's coordinate as
/// its key. The caller checks that.
///
/// # Safety
/// `table` must stay a live table of this crate until
/// `mako_sto_shard_table_unregister` has removed the entry. Nothing is read
/// through it here.
#[no_mangle]
pub unsafe extern "C" fn mako_sto_shard_table_register(
    catalog_table: u64,
    owner: u32,
    table: *const StoTpccTable,
    table_id: u32,
) -> u32 {
    contain_registration("table_register", || {
        // The address is checked and stored. No reference is formed: the
        // first dereference happens inside a verb on a registered thread.
        if checked_raw_pointer_range(table, 1, "table").is_err() {
            return SHARD_INVALID;
        }
        let table_id = match u16::try_from(table_id) {
            Ok(table_id) if table_id != 0 && table_id < 0x8000 => table_id,
            _ => {
                set_last_error(format_args!(
                    "shard host: table id {table_id} is outside 1..0x8000"
                ));
                return SHARD_INVALID;
            }
        };
        let mut tables = TABLES.write().unwrap_or_else(PoisonError::into_inner);
        for entry in tables.iter() {
            let same_key = entry.catalog_table == catalog_table && entry.owner == owner;
            let same_table = ptr::eq(entry.table, table);
            if same_key && same_table && entry.table_id == table_id {
                return SHARD_OK;
            }
            if same_key || same_table {
                set_last_error(format_args!(
                    "shard host: table {catalog_table} of owner {owner} conflicts with \
                     the registration of table {} of owner {}",
                    entry.catalog_table, entry.owner
                ));
                return SHARD_INVALID;
            }
        }
        if tables.try_reserve(1).is_err() {
            return SHARD_EXHAUSTED;
        }
        tables.push(TableEntry {
            catalog_table,
            owner,
            table,
            table_id,
        });
        SHARD_OK
    })
}

/// Removes one registration. OK when it existed, NOT_FOUND otherwise.
///
/// Call it after every node that may run a verb on the table has stopped and
/// before the table is destroyed.
#[no_mangle]
pub extern "C" fn mako_sto_shard_table_unregister(catalog_table: u64, owner: u32) -> u32 {
    contain_registration("table_unregister", || {
        let mut tables = TABLES.write().unwrap_or_else(PoisonError::into_inner);
        match tables
            .iter()
            .position(|entry| entry.catalog_table == catalog_table && entry.owner == owner)
        {
            Some(index) => {
                tables.swap_remove(index);
                SHARD_OK
            }
            None => SHARD_NOT_FOUND,
        }
    })
}

/// Binds the calling OS thread's engine handle to `owner` for the verbs.
///
/// INVALID when the thread is already registered, when `thread` is null or
/// misaligned, when it was created on another OS thread, and when a
/// transaction is open on it.
///
/// # Safety
/// `thread` must be null or the live handle the calling OS thread created,
/// and must stay alive until `mako_sto_shard_thread_unregister` on this
/// thread. `context` is compared by address and never read.
#[no_mangle]
pub unsafe extern "C" fn mako_sto_shard_thread_register(
    context: *mut c_void,
    owner: u32,
    thread: *mut StoTpccThread,
) -> u32 {
    contain_registration("thread_register", || {
        if ENTERED.get().is_some() {
            set_last_error(format_args!(
                "shard host: this thread is already registered"
            ));
            return SHARD_INVALID;
        }
        // SAFETY: A non-null handle is live by the caller's contract.
        let Ok(handle) = (unsafe { required_ref(thread.cast_const(), "thread") }) else {
            return SHARD_INVALID;
        };
        if handle.ensure_owner().is_err() {
            return SHARD_INVALID;
        }
        if handle.active.is_some() || handle.sto_worker.is_active() {
            set_last_error(format_args!(
                "shard host: a transaction is open on the thread handle"
            ));
            return SHARD_INVALID;
        }
        ENTERED.set(Some(Entered {
            context: context.addr(),
            owner,
            thread,
        }));
        FIRST_USE.set(0);
        SHARD_OK
    })
}

/// Ends the binding made on this thread. INVALID when there is none or it
/// was made with another `context`.
///
/// Call it before the thread's engine handle is destroyed.
///
/// # Safety
/// `context` is compared by address and never read.
#[no_mangle]
pub unsafe extern "C" fn mako_sto_shard_thread_unregister(context: *mut c_void) -> u32 {
    contain_registration("thread_unregister", || match ENTERED.get() {
        Some(entered) if entered.context == context.addr() => {
            ENTERED.set(None);
            SHARD_OK
        }
        _ => SHARD_INVALID,
    })
}

/// The binding of the calling thread when it was made with `context`.
fn entered_for(context: *mut c_void) -> Option<Entered> {
    let entered = ENTERED
        .get()
        .filter(|entered| entered.context == context.addr());
    if entered.is_none() {
        set_last_error(format_args!(
            "shard host: the calling thread is not registered for this context"
        ));
    }
    entered
}

/// A copy of the registration of `catalog_table` on `owner`. The lock is not
/// held while the engine runs.
fn table_for(catalog_table: u64, owner: u32) -> Option<TableEntry> {
    let entry = TABLES
        .read()
        .unwrap_or_else(PoisonError::into_inner)
        .iter()
        .find(|entry| entry.catalog_table == catalog_table && entry.owner == owner)
        .copied();
    if entry.is_none() {
        set_last_error(format_args!(
            "shard host: table {catalog_table} of owner {owner} is not registered"
        ));
    }
    entry
}

/// Whether no transaction is open on the registered handle.
///
/// # Safety
/// `thread` is the registered handle of the calling thread.
unsafe fn handle_is_idle(thread: *mut StoTpccThread) -> bool {
    // SAFETY: Guaranteed by the caller. The shared borrow ends with this
    // function, before any export borrows the handle exclusively.
    let handle = unsafe { &*thread };
    let idle =
        handle.ensure_owner().is_ok() && handle.active.is_none() && !handle.sto_worker.is_active();
    if !idle {
        set_last_error(format_args!(
            "shard host: a transaction is open on the thread handle"
        ));
    }
    idle
}

fn span(bytes: &[u8]) -> MakoStoShardBytes {
    MakoStoShardBytes {
        data: bytes.as_ptr(),
        len: bytes.len(),
    }
}

/// The bytes of a span from the caller.
///
/// # Safety
/// A span of non-zero length must be readable for the returned lifetime.
unsafe fn span_bytes<'a>(span: MakoStoShardBytes, name: &str) -> Option<&'a [u8]> {
    // SAFETY: Forwarded; a zero length never looks at the pointer.
    unsafe { bytes(span.data, span.len, name) }.ok()
}

/// One lookahead transaction: begin, walk, certify, finish.
///
/// `Ok` is returned only after the transaction has finished committed, so a
/// row or a resume key in it was certified: when the reads were validated, no
/// key had been added to the table since the walk began and every walked
/// record still had the version that was read. `Err` carries the status the
/// verb answers; the attempt is closed.
///
/// # Safety
/// `thread` is the registered, idle handle of the calling thread and `table`
/// a registered table.
unsafe fn lookahead(
    thread: *mut StoTpccThread,
    table: *const StoTpccTable,
    lower: ScanBound<'_>,
    upper: ScanBound<'_>,
    visibility: ReadVisibility,
) -> Result<VersionedLookahead, u32> {
    const OPERATION: &str = "scan_next";
    // SAFETY: Guaranteed by the caller.
    let begun = unsafe { sto_tpcc_protocol_begin(thread, TERM) };
    if begun != ENGINE_OK {
        // SAFETY: As above.
        return Err(unsafe { close_and_map(OPERATION, thread, begun) });
    }
    let step = {
        // SAFETY: The caller's contract; the exclusive borrow of the handle
        // ends with this block, before the next export takes it again.
        let handle = unsafe { &mut *thread };
        // SAFETY: The table stays registered, and so alive, for the call.
        let table = &unsafe { &*table }.state.table;
        let worker = &handle.native_worker;
        handle.sto_worker.with_participant(|participant| {
            participant.with_transaction(|transaction| {
                table.lookahead_versioned(
                    transaction,
                    worker,
                    lower,
                    upper,
                    visibility,
                    SCAN_HOP_ENTRIES,
                )
            })
        })
    };
    let found = match step {
        None => {
            set_last_error(format_args!("shard host scan_next: no participant active"));
            // SAFETY: As above.
            return Err(unsafe { close_and_map(OPERATION, thread, ENGINE_FATAL) });
        }
        Some(Err(error)) => {
            let status = status_from_access("shard host scan_next", error);
            // SAFETY: As above.
            return Err(unsafe { close_and_map(OPERATION, thread, status.code()) });
        }
        Some(Ok(found)) => found,
    };
    #[cfg(test)]
    if let Some(after_walk) = AFTER_WALK.take() {
        after_walk();
    }
    // SAFETY: As above.
    match unsafe { certify_read_only(OPERATION, thread) } {
        SHARD_OK => Ok(found),
        status => Err(status),
    }
}

/// # Safety
/// As `mako_sto_shard_scan_next`.
#[allow(clippy::too_many_arguments)]
unsafe fn scan_next(
    context: *mut c_void,
    catalog_table: u64,
    lo: MakoStoShardBytes,
    has_hi: u32,
    hi: MakoStoShardBytes,
    has_after: u32,
    after_coordinate: MakoStoShardBytes,
    after_key: MakoStoShardBytes,
    sink: Option<MakoStoShardRowSink>,
    sink_context: *mut c_void,
) -> u32 {
    let Some(entered) = entered_for(context) else {
        return SHARD_INVALID;
    };
    let Some(sink) = sink else {
        set_last_error(format_args!(
            "shard host scan_next: the row sink is missing"
        ));
        return SHARD_INVALID;
    };
    if !scan_flags_valid(has_hi, has_after) {
        set_last_error(format_args!(
            "shard host scan_next: has_hi and has_after must be 0 or 1"
        ));
        return SHARD_INVALID;
    }
    // SAFETY: The caller lends every span for the call.
    let spans = unsafe {
        (
            span_bytes(lo, "lo"),
            span_bytes(hi, "hi"),
            span_bytes(after_coordinate, "after_coordinate"),
            span_bytes(after_key, "after_key"),
        )
    };
    let (Some(lo), Some(hi), Some(after_coordinate), Some(after_key)) = spans else {
        return SHARD_INVALID;
    };
    let start = match scan_arguments(has_hi, has_after, lo, hi, after_coordinate, after_key) {
        Ok(start) => start,
        Err(refusal) => {
            set_last_error(format_args!(
                "shard host scan_next: {}",
                match refusal {
                    ScanRefusal::Flags => "has_hi and has_after must be 0 or 1",
                    ScanRefusal::EmptyRange => "the range is empty",
                    ScanRefusal::CursorIdentity => "the cursor's coordinate differs from its key",
                }
            ));
            return SHARD_INVALID;
        }
    };
    // Every refusal comes before the answer for an exhausted range: NOT_FOUND
    // certifies the end of the range and must not stand for an unknown table.
    let Some(entry) = table_for(catalog_table, entered.owner) else {
        return SHARD_IO;
    };
    // SAFETY: The registration contract keeps the handle alive.
    if !unsafe { handle_is_idle(entered.thread) } {
        return SHARD_INVALID;
    }
    // Acted on only here, after the refusals above: the cursor is at or
    // beyond the upper bound.
    let (first, upper) = match start {
        ScanStart::Exhausted => return SHARD_NOT_FOUND,
        ScanStart::Rows { lower, upper } => (lower, upper),
    };
    log_first_use(FIRST_SCAN_NEXT, "scan_next", entered.owner, catalog_table);

    let Ok(term) = DistributedTerm::new(TERM) else {
        return SHARD_IO;
    };
    // The newest committed version of every record.
    let visibility = ReadVisibility::latest(term);
    let upper = upper.map_or(ScanBound::Unbounded, ScanBound::Excluded);
    // The last deleted key a lookahead walked; the next one starts after it.
    let mut resume: Option<Box<[u8]>> = None;
    loop {
        let lower = match (resume.as_deref(), first) {
            (Some(walked), _) => ScanBound::Excluded(walked),
            (None, ScanLower::Excluded(cursor)) => ScanBound::Excluded(cursor),
            (None, ScanLower::Included(lo)) => ScanBound::Included(lo),
        };
        // SAFETY: The handle is registered and idle; the table is registered.
        match unsafe { lookahead(entered.thread, entry.table, lower, upper, visibility) } {
            Ok(VersionedLookahead::Row(row)) => {
                // The row owns its bytes, and its transaction has finished.
                // SAFETY: The sink is the caller's and takes borrowed spans.
                unsafe {
                    sink(
                        sink_context,
                        span(&row.key),
                        span(&row.key),
                        span(row.value.as_ref()),
                    );
                }
                return SHARD_OK;
            }
            Ok(VersionedLookahead::End) => return SHARD_NOT_FOUND,
            Ok(VersionedLookahead::Resume(walked)) => resume = Some(walked),
            Err(status) => return status,
        }
    }
}

/// Delivers the first live committed row of `table` on the registered owner
/// whose key lies in `[lo, hi)` (no upper bound when `has_hi` is 0) and, when
/// `has_after` is 1, is greater than `after_key`.
///
/// OK after exactly one call of `sink` with the key as coordinate and as key
/// and with the row's logical bytes. NOT_FOUND, without a sink call, only
/// when no live row is left in the range. RETRY for a conflict; the caller
/// decides whether to call again. INVALID for a thread that is not registered
/// for `context`, a malformed argument or a handle with an open transaction.
/// IO for a table that is not registered for the thread's owner. EXHAUSTED
/// for an engine limit; a bound or a cursor longer than the engine's
/// configured key length meets one.
///
/// A lookahead is one committed observation. Behind many deleted keys it is
/// a sequence of them, see `SCAN_HOP_ENTRIES`; a sequence of lookaheads is
/// not a snapshot.
///
/// # Safety
/// Every span of non-zero length must be readable for the call. `sink` must
/// not unwind and must not call back into this module or use the thread's
/// engine handle.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn mako_sto_shard_scan_next(
    context: *mut c_void,
    table: u64,
    lo: MakoStoShardBytes,
    has_hi: u32,
    hi: MakoStoShardBytes,
    has_after: u32,
    after_coordinate: MakoStoShardBytes,
    after_key: MakoStoShardBytes,
    sink: Option<MakoStoShardRowSink>,
    sink_context: *mut c_void,
) -> u32 {
    contain_verb("scan_next", || {
        // SAFETY: Forwarded from this export's contract.
        unsafe {
            scan_next(
                context,
                table,
                lo,
                has_hi,
                hi,
                has_after,
                after_coordinate,
                after_key,
                sink,
                sink_context,
            )
        }
    })
}

/// Commit callback of a non-replicated node: the log record is dropped. A
/// transaction without a remote participant is asked for nothing else.
unsafe extern "C" fn shard_log_callback(
    _context: *mut c_void,
    request: *mut StoTpccProtocolRequest,
) -> i32 {
    // SAFETY: The engine passes a live request for the call.
    if unsafe { (*request).action } == PROTOCOL_LOG {
        ENGINE_OK
    } else {
        ENGINE_FATAL
    }
}

/// Refuses a key or a value the protocol's log record cannot carry, with one
/// line: the migration that needed the write is aborted by this answer.
fn refuse_oversized(
    verb: &str,
    entered: Entered,
    catalog_table: u64,
    what: &str,
    length: usize,
) -> u32 {
    set_last_error(format_args!(
        "shard host {verb}: a {what} of {length} bytes is longer than {MAX_LOGGED_BYTES}"
    ));
    diagnostic(format_args!(
        "{verb} refused owner={} table={catalog_table}: a {what} of {length} bytes is longer \
         than {MAX_LOGGED_BYTES}",
        entered.owner
    ));
    SHARD_INVALID
}

/// The checks `put` and `remove` share before they ask the engine, in the
/// order of `scan_next`: the calling thread's binding, the registered table
/// and the key. `Err` carries the refusal.
///
/// # Safety
/// As `mako_sto_shard_put`; the key is borrowed for the returned lifetime.
unsafe fn write_target<'a>(
    verb: &str,
    first_use: u8,
    context: *mut c_void,
    catalog_table: u64,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
) -> Result<(Entered, TableEntry, &'a [u8]), u32> {
    let Some(entered) = entered_for(context) else {
        return Err(SHARD_INVALID);
    };
    // SAFETY: The caller lends both spans for the call.
    let spans = unsafe { (span_bytes(coordinate, "coordinate"), span_bytes(key, "key")) };
    let (Some(coordinate), Some(key)) = spans else {
        return Err(SHARD_INVALID);
    };
    // The coordinate of a raw table's row is its key.
    if coordinate != key {
        set_last_error(format_args!(
            "shard host {verb}: the coordinate differs from the key"
        ));
        return Err(SHARD_INVALID);
    }
    let Some(entry) = table_for(catalog_table, entered.owner) else {
        return Err(SHARD_IO);
    };
    // SAFETY: The registration contract keeps the handle alive.
    if !unsafe { handle_is_idle(entered.thread) } {
        return Err(SHARD_INVALID);
    }
    log_first_use(first_use, verb, entered.owner, catalog_table);
    if key.len() > MAX_LOGGED_BYTES {
        return Err(refuse_oversized(
            verb,
            entered,
            catalog_table,
            "key",
            key.len(),
        ));
    }
    Ok((entered, entry, key))
}

/// Commits the one write staged in the open attempt: lock, reserve one
/// distributed timestamp, validate, install, hand over the log record,
/// finish. Anything but "committed" or a failure the engine reports with an
/// aborted attempt stops the process.
///
/// # Safety
/// `thread` is the registered handle of the calling thread.
unsafe fn commit_staged(operation: &str, thread: *mut StoTpccThread) -> u32 {
    let mut report = StoTpccProtocolReport::default();
    // SAFETY: Guaranteed by the caller; the callback reads its request only.
    let committed = unsafe {
        sto_tpcc_protocol_commit(
            thread,
            0,
            Some(shard_log_callback),
            ptr::null_mut(),
            &mut report,
        )
    };
    shard_status(
        operation,
        map_engine(Stage::Commit, committed, report.disposition),
    )
}

/// # Safety
/// As `mako_sto_shard_put`.
unsafe fn put(
    context: *mut c_void,
    catalog_table: u64,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
    value: MakoStoShardBytes,
) -> u32 {
    const OPERATION: &str = "put";
    // SAFETY: The caller lends the span for the call.
    let Some(value) = (unsafe { span_bytes(value, "value") }) else {
        return SHARD_INVALID;
    };
    // SAFETY: Forwarded.
    let target = unsafe {
        write_target(
            OPERATION,
            FIRST_PUT,
            context,
            catalog_table,
            coordinate,
            key,
        )
    };
    let (entered, entry, key) = match target {
        Ok(target) => target,
        Err(refusal) => return refusal,
    };
    if value.len() > MAX_LOGGED_BYTES {
        return refuse_oversized(OPERATION, entered, catalog_table, "value", value.len());
    }
    let thread = entered.thread;
    // SAFETY: The handle is registered and idle; the table is registered and
    // the engine copies the key and the value before it returns.
    unsafe {
        let begun = sto_tpcc_protocol_begin(thread, TERM);
        if begun != ENGINE_OK {
            return close_and_map(OPERATION, thread, begun);
        }
        // Reads the current version, records the write and stages it.
        let staged = sto_tpcc_protocol_mutate(
            thread,
            entry.table,
            entry.table_id,
            PROTOCOL_UPSERT,
            key.as_ptr(),
            key.len(),
            value.as_ptr(),
            value.len(),
            0,
        );
        if staged != ENGINE_OK {
            return close_and_map(OPERATION, thread, staged);
        }
        commit_staged(OPERATION, thread)
    }
}

/// # Safety
/// As `mako_sto_shard_remove`.
unsafe fn remove(
    context: *mut c_void,
    catalog_table: u64,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
) -> u32 {
    const OPERATION: &str = "remove";
    // SAFETY: Forwarded.
    let target = unsafe {
        write_target(
            OPERATION,
            FIRST_REMOVE,
            context,
            catalog_table,
            coordinate,
            key,
        )
    };
    let (entered, entry, key) = match target {
        Ok(target) => target,
        Err(refusal) => return refusal,
    };
    let thread = entered.thread;
    // SAFETY: As in `put`.
    unsafe {
        let begun = sto_tpcc_protocol_begin(thread, TERM);
        if begun != ENGINE_OK {
            return close_and_map(OPERATION, thread, begun);
        }
        let staged = sto_tpcc_protocol_mutate(
            thread,
            entry.table,
            entry.table_id,
            PROTOCOL_DELETE,
            key.as_ptr(),
            key.len(),
            ptr::null(),
            0,
            0,
        );
        match staged {
            ENGINE_OK => commit_staged(OPERATION, thread),
            // The key is absent and nothing was staged. The absence is a
            // read like any other: certify it and finish read-only.
            ENGINE_MISS => certify_read_only(OPERATION, thread),
            failed => close_and_map(OPERATION, thread, failed),
        }
    }
}

/// Makes `value` the committed value of `key` in `table` on the registered
/// owner, as one protocol commit of this one key.
///
/// OK means exactly that: the key now has this value, visible to the next
/// lookahead on this thread. Every other status means the table is
/// unchanged; when the engine cannot say which of the two holds, the process
/// stops. RETRY for a conflict, EXHAUSTED for an engine limit (a key longer
/// than the engine's configured key length meets one), IO for a table that
/// is not registered for the thread's owner and for an engine failure.
/// INVALID for a thread that is not registered for `context`, a coordinate
/// that is not the key, and a handle with an open transaction.
///
/// INVALID also for a key or a value longer than 65,535 bytes. The protocol's
/// log record keeps both lengths in 16 bits, so such a row cannot be written
/// here, and a migration that carries one is aborted by this answer.
///
/// Every committed put takes one value of the process's distributed clock.
/// The log record of the write is dropped, which is right on a non-replicated
/// node only.
///
/// # Safety
/// Every span of non-zero length must be readable for the call.
#[no_mangle]
pub unsafe extern "C" fn mako_sto_shard_put(
    context: *mut c_void,
    table: u64,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
    value: MakoStoShardBytes,
) -> u32 {
    contain_verb("put", || {
        // SAFETY: Forwarded from this export's contract.
        unsafe { put(context, table, coordinate, key, value) }
    })
}

/// Makes `key` absent from `table` on the registered owner, as one protocol
/// commit of this one key. OK also when the key was absent already.
///
/// The statuses are those of `mako_sto_shard_put`, with the same limit of
/// 65,535 bytes on the key. A committed delete of a row takes one value of
/// the process's distributed clock; the deleted key stays in the table's
/// directory.
///
/// # Safety
/// Every span of non-zero length must be readable for the call.
#[no_mangle]
pub unsafe extern "C" fn mako_sto_shard_remove(
    context: *mut c_void,
    table: u64,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
) -> u32 {
    contain_verb("remove", || {
        // SAFETY: Forwarded from this export's contract.
        unsafe { remove(context, table, coordinate, key) }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr::NonNull;

    /// Row sink that counts its calls in the `usize` behind the context.
    unsafe extern "C" fn count_rows(
        context: *mut c_void,
        _coordinate: MakoStoShardBytes,
        _key: MakoStoShardBytes,
        _value: MakoStoShardBytes,
    ) {
        // SAFETY: Every test passes a live `usize`.
        unsafe { *context.cast::<usize>() += 1 };
    }

    #[test]
    fn status_numbers_and_span_layout_are_stable() {
        // `enum MakoShardStatus` in src/cluster/native_sharding.h.
        assert_eq!(
            [
                SHARD_OK,
                SHARD_NOT_FOUND,
                SHARD_RETRY,
                SHARD_INVALID,
                SHARD_EXHAUSTED,
                SHARD_IO
            ],
            [0, 1, 2, 3, 5, 6]
        );
        // `MakoShardBytes` in the same header, passed by value.
        #[cfg(target_pointer_width = "64")]
        assert_eq!(
            [
                mem::size_of::<MakoStoShardBytes>(),
                mem::align_of::<MakoStoShardBytes>(),
                mem::offset_of!(MakoStoShardBytes, data),
                mem::offset_of!(MakoStoShardBytes, len),
            ],
            [16, 8, 0, 8]
        );
    }

    #[test]
    fn table_registry_rejects_conflicts_and_bad_ids() {
        // Catalog ids of this test alone; the registry is process-wide.
        const FIRST: u64 = 0x7e57_0000_0000_0001;
        const SECOND: u64 = 0x7e57_0000_0000_0002;
        // Two aligned addresses that are never dereferenced: registration
        // stores a pointer without forming a reference.
        let one = NonNull::<StoTpccTable>::dangling().as_ptr().cast_const();
        let two = one.wrapping_add(1);
        let misaligned = one.cast::<u8>().wrapping_add(1).cast::<StoTpccTable>();
        let registered = || {
            TABLES
                .read()
                .unwrap()
                .iter()
                .filter(|entry| entry.catalog_table == FIRST || entry.catalog_table == SECOND)
                .count()
        };
        unsafe {
            // Pointers and ids the registry cannot take.
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 0, ptr::null(), 1),
                SHARD_INVALID
            );
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 0, misaligned, 1),
                SHARD_INVALID
            );
            for bad_id in [0, 0x8000, 0x8001, 0x1_0001, u32::MAX] {
                assert_eq!(
                    mako_sto_shard_table_register(FIRST, 0, one, bad_id),
                    SHARD_INVALID,
                    "table id {bad_id:#x}"
                );
            }
            assert_eq!(registered(), 0);

            // The largest id is accepted, and the identical entry again.
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 0, one, 0x7fff),
                SHARD_OK
            );
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 0, one, 0x7fff),
                SHARD_OK
            );
            assert_eq!(registered(), 1);

            // The key with another pointer or another id.
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 0, two, 0x7fff),
                SHARD_INVALID
            );
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 0, one, 1),
                SHARD_INVALID
            );
            // The pointer under another key: another table, another owner.
            assert_eq!(
                mako_sto_shard_table_register(SECOND, 0, one, 0x7fff),
                SHARD_INVALID
            );
            assert_eq!(
                mako_sto_shard_table_register(FIRST, 1, one, 0x7fff),
                SHARD_INVALID
            );
            assert_eq!(registered(), 1);

            // The same catalog table on another owner is another entry.
            assert_eq!(mako_sto_shard_table_register(FIRST, 1, two, 1), SHARD_OK);
            assert_eq!(registered(), 2);
            assert_eq!(
                table_for(FIRST, 0).map(|entry| entry.table_id),
                Some(0x7fff)
            );
            assert_eq!(table_for(FIRST, 1).map(|entry| entry.table), Some(two));
            assert!(table_for(SECOND, 0).is_none());

            // Removal is per entry and frees the pointer for another key.
            assert_eq!(mako_sto_shard_table_unregister(FIRST, 0), SHARD_OK);
            assert_eq!(mako_sto_shard_table_unregister(FIRST, 0), SHARD_NOT_FOUND);
            assert_eq!(mako_sto_shard_table_unregister(SECOND, 0), SHARD_NOT_FOUND);
            assert_eq!(registered(), 1);
            assert_eq!(mako_sto_shard_table_register(SECOND, 0, one, 2), SHARD_OK);
            assert_eq!(mako_sto_shard_table_unregister(SECOND, 0), SHARD_OK);
            assert_eq!(mako_sto_shard_table_unregister(FIRST, 1), SHARD_OK);
            assert_eq!(registered(), 0);
        }
    }

    #[test]
    fn verbs_refuse_an_unentered_thread() {
        let mut token = 0_u8;
        let context = (&raw mut token).cast::<c_void>();
        let mut rows = 0_usize;
        let empty = span(&[]);
        unsafe {
            assert_eq!(
                mako_sto_shard_scan_next(
                    context,
                    1,
                    empty,
                    0,
                    empty,
                    0,
                    empty,
                    empty,
                    Some(count_rows),
                    (&raw mut rows).cast(),
                ),
                SHARD_INVALID
            );
            assert_eq!(
                mako_sto_shard_put(context, 1, empty, empty, empty),
                SHARD_INVALID
            );
            assert_eq!(
                mako_sto_shard_remove(context, 1, empty, empty),
                SHARD_INVALID
            );
            // Nothing was registered, so there is nothing to end, and a null
            // handle cannot enter.
            assert_eq!(mako_sto_shard_thread_unregister(context), SHARD_INVALID);
            assert_eq!(
                mako_sto_shard_thread_register(context, 0, ptr::null_mut()),
                SHARD_INVALID
            );
            assert_eq!(mako_sto_shard_thread_unregister(context), SHARD_INVALID);
        }
        assert_eq!(rows, 0);
        assert!(ENTERED.get().is_none());
    }

    #[test]
    fn engine_status_mapping_is_total_and_stops_on_indefinite_outcomes() {
        use Mapped::FailStop as STOP;
        const IO: Mapped = Mapped::Shard(SHARD_IO);
        const OK: Mapped = Mapped::Shard(SHARD_OK);
        const RETRY: Mapped = Mapped::Shard(SHARD_RETRY);
        const EXHAUSTED: Mapped = Mapped::Shard(SHARD_EXHAUSTED);
        // One row per engine status from -1 to 9, one column per report
        // disposition: none, aborted, committed, uncertain.
        //
        // A failed step whose attempt was closed as aborted.
        let closed = [
            [IO; 4],        // -1, not a status
            [IO; 4],        // OK: a successful step is never closed as failed
            [IO; 4],        // MISS
            [IO; 4],        // DUPLICATE
            [RETRY; 4],     // RETRY
            [IO; 4],        // BUFFER_TOO_SMALL
            [IO; 4],        // FATAL
            [EXHAUSTED; 4], // RESOURCE_EXHAUSTED
            [STOP; 4],      // UNKNOWN
            [STOP; 4],      // COMMITTED_ERROR
            [IO; 4],        // 9, not a status
        ];
        // The result of a commit: definite only as committed, or as one of
        // the three failures the coordinator reports with an aborted attempt.
        let commit = [
            [STOP; 4],
            [STOP, STOP, OK, STOP],
            [STOP; 4],
            [STOP; 4],
            [STOP, RETRY, STOP, STOP],
            [STOP; 4],
            [STOP, IO, STOP, STOP],
            [STOP, EXHAUSTED, STOP, STOP],
            [STOP; 4],
            [STOP; 4],
            [STOP; 4],
        ];
        // The committing finish of a read-only attempt.
        let read_only_finish = [
            [IO; 4],
            [IO, IO, OK, IO],
            [IO; 4],
            [IO; 4],
            [RETRY; 4],
            [IO; 4],
            [IO; 4],
            [EXHAUSTED; 4],
            [STOP; 4],
            [STOP; 4],
            [IO; 4],
        ];
        for (stage, expected) in [
            (Stage::Closed, closed),
            (Stage::Commit, commit),
            (Stage::ReadOnlyFinish, read_only_finish),
        ] {
            for (row, status) in (-1..=9).enumerate() {
                for (column, disposition) in (0..=3).enumerate() {
                    assert_eq!(
                        map_engine(stage, status, disposition),
                        expected[row][column],
                        "{stage:?}, engine status {status}, disposition {disposition}"
                    );
                }
            }
        }
    }

    /// One native database with a registered table and thread, for the tests
    /// that reach between a lookahead's walk and its certification.
    #[cfg(mtree_native_integration)]
    struct Engine {
        db: *mut StoTpccDb,
        table: *mut StoTpccTable,
        thread: *mut StoTpccThread,
    }

    #[cfg(mtree_native_integration)]
    const CATALOG: u64 = 0x7e57_0000_0000_0003;
    #[cfg(mtree_native_integration)]
    const OWNER: u32 = 1;

    #[cfg(mtree_native_integration)]
    type Row = (Vec<u8>, Vec<u8>, Vec<u8>);

    /// Row sink that copies the row into the `Vec<Row>` behind the context.
    #[cfg(mtree_native_integration)]
    unsafe extern "C" fn collect_rows(
        context: *mut c_void,
        coordinate: MakoStoShardBytes,
        key: MakoStoShardBytes,
        value: MakoStoShardBytes,
    ) {
        // SAFETY: Every caller passes a live `Vec<Row>` and the spans are
        // valid for the call.
        unsafe {
            (*context.cast::<Vec<Row>>()).push((
                span_bytes(coordinate, "coordinate").unwrap().to_vec(),
                span_bytes(key, "key").unwrap().to_vec(),
                span_bytes(value, "value").unwrap().to_vec(),
            ));
        }
    }

    /// Commits `key` as a row without history on `thread`.
    #[cfg(mtree_native_integration)]
    unsafe fn commit_plain(thread: *mut StoTpccThread, table: *mut StoTpccTable, key: &[u8]) {
        unsafe {
            assert_eq!(sto_tpcc_txn_begin(thread), ENGINE_OK);
            assert_eq!(
                sto_tpcc_put(
                    thread,
                    table,
                    key.as_ptr(),
                    key.len(),
                    b"value".as_ptr(),
                    b"value".len(),
                ),
                ENGINE_OK
            );
            assert_eq!(sto_tpcc_txn_commit(thread), ENGINE_OK);
        }
    }

    #[cfg(mtree_native_integration)]
    impl Engine {
        /// A table with the one live row `m`.
        fn new() -> Self {
            // The limits of the neighbouring native unit tests.
            let config = StoTpccDbConfig {
                max_threads: 8,
                max_key_length: 128,
                max_items_per_txn: 1024,
                max_locks_per_txn: 2048,
                max_registry_bytes: 0,
            };
            let mut engine = Self {
                db: ptr::null_mut(),
                table: ptr::null_mut(),
                thread: ptr::null_mut(),
            };
            unsafe {
                assert_eq!(sto_tpcc_db_create(&config, &mut engine.db), ENGINE_OK);
                assert_eq!(
                    sto_tpcc_table_create(engine.db, ptr::null(), &mut engine.table),
                    ENGINE_OK
                );
                assert_eq!(
                    sto_tpcc_thread_create(engine.db, &mut engine.thread),
                    ENGINE_OK
                );
                commit_plain(engine.thread, engine.table, b"m");
                assert_eq!(
                    mako_sto_shard_table_register(CATALOG, OWNER, engine.table, 201),
                    SHARD_OK
                );
                assert_eq!(
                    mako_sto_shard_thread_register(engine.context(), OWNER, engine.thread),
                    SHARD_OK
                );
            }
            engine
        }

        fn context(&self) -> *mut c_void {
            self.db.cast()
        }

        /// The first row of the whole table after `after`.
        fn scan_next(&self, after: Option<&[u8]>) -> (u32, Vec<Row>) {
            let mut rows: Vec<Row> = Vec::new();
            let cursor = span(after.unwrap_or_default());
            let status = unsafe {
                mako_sto_shard_scan_next(
                    self.context(),
                    CATALOG,
                    span(&[]),
                    0,
                    span(&[]),
                    u32::from(after.is_some()),
                    cursor,
                    cursor,
                    Some(collect_rows),
                    (&raw mut rows).cast(),
                )
            };
            (status, rows)
        }

        /// No attempt is open, and the handle begins and aborts a new one.
        fn assert_idle(&self) {
            let mut report = StoTpccProtocolReport::default();
            let mut empty = 0;
            unsafe {
                assert!(handle_is_idle(self.thread));
                assert_eq!(
                    sto_tpcc_protocol_state(self.thread, &mut report, &mut empty),
                    ENGINE_OK
                );
                assert_eq!((report.phase, empty), (0, 1));
                assert_eq!(sto_tpcc_protocol_begin(self.thread, 0), ENGINE_OK);
                assert_eq!(
                    sto_tpcc_protocol_finish(self.thread, 0, &mut report),
                    ENGINE_OK
                );
            }
        }
    }

    #[cfg(mtree_native_integration)]
    impl Drop for Engine {
        fn drop(&mut self) {
            AFTER_WALK.set(None);
            unsafe {
                let mut report = StoTpccProtocolReport::default();
                let _ = sto_tpcc_protocol_finish(self.thread, 0, &mut report);
                let released = [
                    mako_sto_shard_thread_unregister(self.context()),
                    mako_sto_shard_table_unregister(CATALOG, OWNER),
                ];
                let destroyed = [
                    sto_tpcc_thread_destroy(self.thread),
                    sto_tpcc_table_destroy(self.table),
                    sto_tpcc_db_destroy(self.db),
                ];
                if !thread::panicking() {
                    assert_eq!(released, [SHARD_OK; 2]);
                    assert_eq!(destroyed, [ENGINE_OK; 3]);
                }
            }
        }
    }

    #[cfg(mtree_native_integration)]
    fn row(key: &[u8]) -> Row {
        (key.to_vec(), key.to_vec(), b"value".to_vec())
    }

    /// A lookahead's row is delivered only when its transaction certified.
    ///
    /// On one thread every lookahead certifies trivially, so a `scan_next`
    /// that closed its transaction as aborted and delivered the row anyway
    /// would pass every other test. Here another engine thread commits a key
    /// the table has never held after the walk and before the certification.
    /// That advances the table's directory generation, which the walk read.
    #[cfg(mtree_native_integration)]
    #[test]
    fn scan_next_returns_retry_when_a_key_is_interned_between_walk_and_validation() {
        let engine = Engine::new();
        // Raw pointers are not `Send`; pass exposed addresses as the
        // neighbouring native tests do.
        let db_address = engine.db.expose_provenance();
        let table_address = engine.table.expose_provenance();
        AFTER_WALK.set(Some(Box::new(move || {
            thread::spawn(move || unsafe {
                let db = ptr::with_exposed_provenance_mut::<StoTpccDb>(db_address);
                let table = ptr::with_exposed_provenance_mut::<StoTpccTable>(table_address);
                let mut worker = ptr::null_mut();
                assert_eq!(sto_tpcc_thread_create(db, &mut worker), ENGINE_OK);
                commit_plain(worker, table, b"z");
                assert_eq!(sto_tpcc_thread_destroy(worker), ENGINE_OK);
            })
            .join()
            .unwrap();
        })));

        assert_eq!(engine.scan_next(None), (SHARD_RETRY, Vec::new()));
        // The seam ran, and the failed attempt is closed.
        assert!(AFTER_WALK.take().is_none());
        engine.assert_idle();

        // Undisturbed, the same lookahead certifies, and the other thread's
        // row is there.
        assert_eq!(engine.scan_next(None), (SHARD_OK, vec![row(b"m")]));
        assert_eq!(engine.scan_next(Some(b"m")), (SHARD_OK, vec![row(b"z")]));
        assert_eq!(engine.scan_next(Some(b"z")), (SHARD_NOT_FOUND, Vec::new()));
        engine.assert_idle();
    }

    /// A panic in the glue of a verb does not cross the C boundary: the verb
    /// answers IO, delivers nothing and closes the attempt it had open.
    #[cfg(mtree_native_integration)]
    #[test]
    fn a_panic_inside_scan_next_is_contained_and_closes_the_attempt() {
        let engine = Engine::new();
        AFTER_WALK.set(Some(Box::new(|| {
            panic!("injected between walk and certification")
        })));
        assert_eq!(engine.scan_next(None), (SHARD_IO, Vec::new()));
        assert!(AFTER_WALK.take().is_none());
        engine.assert_idle();
        assert_eq!(engine.scan_next(None), (SHARD_OK, vec![row(b"m")]));
        engine.assert_idle();
    }
}
