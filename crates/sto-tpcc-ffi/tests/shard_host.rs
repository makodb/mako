#![cfg(mtree_native_integration)]

//! The sharding storage host's use of the engine, on a real native table.

use std::{ffi::c_void, ptr, slice};
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const RESOURCE_EXHAUSTED: i32 = 6;

/// The adapter's physical id for the table. Any id the protocol ledger accepts.
const TABLE_ID: u16 = 201;
const PUT: u8 = 0;
const DELETE: u8 = 2;
const UNBOUNDED: i32 = 0;
const EXCLUDED: i32 = 2;
const ABORTED: u32 = 1;
const COMMITTED: u32 = 2;

fn native_error() -> String {
    let mut bytes = vec![0_u8; sto_tpcc_last_error_length() + 1];
    let mut actual = 0;
    let _ =
        unsafe { sto_tpcc_last_error_copy(bytes.as_mut_ptr().cast(), bytes.len(), &mut actual) };
    String::from_utf8_lossy(&bytes[..actual]).into_owned()
}

/// One database, table and thread handle with the limits the C++ adapter
/// gives a Rust-engine database (`db_config_for_worker_count` and
/// `tpcc_table_config_for` in `src/mako/storage/rust_sto_tpcc_wrapper.cc`),
/// so that a limit met here is one the sharding host meets.
struct Native {
    db: *mut StoTpccDb,
    table: *mut StoTpccTable,
    thread: *mut StoTpccThread,
}

impl Native {
    fn new() -> Self {
        let db_config = StoTpccDbConfig {
            max_key_length: 1024,
            max_items_per_txn: 1024,
            max_locks_per_txn: 2048,
            ..Default::default()
        };
        let table_config = StoTpccTableConfig {
            max_retained_records: u64::MAX,
            max_retained_key_bytes: u64::MAX,
            max_consumed_record_ids: u64::MAX,
            scan_chunk_records: 128,
            scan_initial_key_arena_bytes: 16 * 1024,
            scan_max_key_arena_bytes: 64 * 1024,
            max_scan_chunks: 32_768,
            max_scan_physical_records: 4_000_000,
            ..Default::default()
        };
        let mut native = Self {
            db: ptr::null_mut(),
            table: ptr::null_mut(),
            thread: ptr::null_mut(),
        };
        unsafe {
            assert_eq!(sto_tpcc_db_create(&db_config, &mut native.db), OK);
            assert_eq!(
                sto_tpcc_table_create(native.db, &table_config, &mut native.table),
                OK
            );
            assert_eq!(sto_tpcc_thread_create(native.db, &mut native.thread), OK);
        }
        native
    }

    fn begin(&self) {
        assert_eq!(
            unsafe { sto_tpcc_protocol_begin(self.thread, 0) },
            OK,
            "{}",
            native_error()
        );
    }

    fn finish(&self, committed: bool) -> (i32, StoTpccProtocolReport) {
        let mut report = StoTpccProtocolReport::default();
        let status =
            unsafe { sto_tpcc_protocol_finish(self.thread, u32::from(committed), &mut report) };
        (status, report)
    }

    /// One mutation as a protocol transaction of its own, which must commit.
    fn commit_mutation(&self, operation: u8, key: &[u8], value: &[u8]) {
        self.begin();
        assert_eq!(
            unsafe {
                sto_tpcc_protocol_mutate(
                    self.thread,
                    self.table,
                    TABLE_ID,
                    operation,
                    key.as_ptr(),
                    key.len(),
                    value.as_ptr(),
                    value.len(),
                    0,
                )
            },
            OK,
            "{}",
            native_error()
        );
        let mut report = StoTpccProtocolReport::default();
        assert_eq!(
            unsafe {
                sto_tpcc_protocol_commit(
                    self.thread,
                    0,
                    Some(discard_log),
                    ptr::null_mut(),
                    &mut report,
                )
            },
            OK,
            "{}",
            native_error()
        );
        assert_eq!(report.disposition, COMMITTED);
    }

    /// `sto_tpcc_protocol_scan` forward with limit 1 and no upper bound, in
    /// the attempt that is open on the thread. Returns the status, the
    /// `visited` output and the rows the callback received.
    fn scan_one(&self, lower_kind: i32, lower: &[u8]) -> (i32, usize, Vec<Row>) {
        let mut rows = Vec::new();
        let mut visited = usize::MAX;
        let status = unsafe {
            sto_tpcc_protocol_scan(
                self.thread,
                self.table,
                0,
                lower_kind,
                lower.as_ptr(),
                lower.len(),
                UNBOUNDED,
                ptr::null(),
                0,
                1,
                Some(collect),
                (&mut rows as *mut Vec<Row>).cast(),
                &mut visited,
            )
        };
        (status, visited, rows)
    }

    fn rows(&self) -> u64 {
        let mut rows = u64::MAX;
        assert_eq!(unsafe { sto_tpcc_table_size(self.table, &mut rows) }, OK);
        rows
    }
}

impl Drop for Native {
    fn drop(&mut self) {
        unsafe {
            // A failed assertion can leave an attempt open; close it so that
            // the handle can be destroyed while the test unwinds.
            let mut report = StoTpccProtocolReport::default();
            let _ = sto_tpcc_protocol_finish(self.thread, 0, &mut report);
            let destroyed = [
                sto_tpcc_thread_destroy(self.thread),
                sto_tpcc_table_destroy(self.table),
                sto_tpcc_db_destroy(self.db),
            ];
            if !std::thread::panicking() {
                assert_eq!(destroyed, [OK; 3]);
            }
        }
    }
}

type Row = (Vec<u8>, Vec<u8>);

/// Scan callback: copies the row and asks for the next one.
unsafe extern "C" fn collect(
    context: *mut c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
    _time_term: u32,
) -> i32 {
    let rows = unsafe { &mut *context.cast::<Vec<Row>>() };
    rows.push((
        unsafe { slice::from_raw_parts(key, key_length) }.to_vec(),
        unsafe { slice::from_raw_parts(value, value_length) }.to_vec(),
    ));
    0
}

/// Commit callback of a non-replicated node: the log record is dropped. A
/// transaction without a remote participant asks for nothing else.
unsafe extern "C" fn discard_log(
    _context: *mut c_void,
    request: *mut StoTpccProtocolRequest,
) -> i32 {
    if unsafe { (*request).action } == 5 {
        OK
    } else {
        5
    }
}

fn deleted_key(index: usize) -> Vec<u8> {
    format!("d{index:04}").into_bytes()
}

/// The engine limit the sharding host's hop loop exists for.
///
/// A deleted key stays in the table's directory as a tombstone, and a scan
/// makes every directory entry it walks an item of its transaction. The
/// existing protocol scan therefore cannot return the first live row behind
/// more deleted keys than one transaction may hold items, however small its
/// limit is.
#[test]
fn protocol_scan_with_limit_one_cannot_cross_more_tombstones_than_the_item_cap() {
    const DELETED: usize = 1_100;
    const SHORT_RUN: usize = 100;
    let native = Native::new();
    for index in 0..DELETED {
        let key = deleted_key(index);
        native.commit_mutation(PUT, &key, b"gone");
        native.commit_mutation(DELETE, &key, b"");
    }
    native.commit_mutation(PUT, b"live", b"row");
    assert_eq!(native.rows(), 1);
    let live: Vec<Row> = vec![(b"live".to_vec(), b"row".to_vec())];

    // Control: behind the last hundred deleted keys the same scan returns the
    // live row, and its attempt certifies and finishes as a read-only one.
    native.begin();
    assert_eq!(
        native.scan_one(EXCLUDED, &deleted_key(DELETED - SHORT_RUN - 1)),
        (OK, 1, live),
        "{}",
        native_error()
    );
    assert_eq!(
        unsafe { sto_tpcc_protocol_validate(native.thread) },
        OK,
        "{}",
        native_error()
    );
    let (status, report) = native.finish(true);
    assert_eq!((status, report.disposition), (OK, COMMITTED));

    // From the start of the table the scan has 1,100 deleted keys in front of
    // the live row and fails on the transaction's item limit, not on a scan
    // limit of the table. It delivers no row.
    native.begin();
    let (status, visited, rows) = native.scan_one(UNBOUNDED, b"");
    let error = native_error();
    assert_eq!(
        (status, visited, rows),
        (RESOURCE_EXHAUSTED, 0, Vec::new()),
        "{error}"
    );
    assert!(error.contains("ItemLimit"), "{error}");
    // The failed attempt is closed as aborted and leaves the handle usable.
    let (status, report) = native.finish(false);
    assert_eq!((status, report.disposition), (OK, ABORTED));
    native.begin();
    assert_eq!(native.finish(false).0, OK);
    assert_eq!(native.rows(), 1);
}
