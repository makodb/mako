#![cfg(mtree_native_integration)]

//! The sharding storage host's use of the engine, on a real native table.

use std::{ffi::c_void, ptr, slice};
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const RESOURCE_EXHAUSTED: i32 = 6;

// The statuses of the sharding host (`enum MakoShardStatus`).
const SHARD_OK: u32 = 0;
const SHARD_NOT_FOUND: u32 = 1;
const SHARD_INVALID: u32 = 3;
const SHARD_EXHAUSTED: u32 = 5;
const SHARD_IO: u32 = 6;

/// The owner every test registers as.
const OWNER: u32 = 0;

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

    /// A committed read-only point read: the status, the value, and the
    /// time-term of the version read (0 when the key has no decided version).
    fn get(&self, key: &[u8]) -> (i32, Vec<u8>, u32) {
        self.begin();
        let mut value = vec![0_u8; 70_000];
        let mut actual = usize::MAX;
        let mut time_term = u32::MAX;
        let status = unsafe {
            sto_tpcc_protocol_get(
                self.thread,
                self.table,
                key.as_ptr(),
                key.len(),
                value.as_mut_ptr(),
                value.len(),
                &mut actual,
                &mut time_term,
            )
        };
        value.truncate(actual);
        assert_eq!(
            unsafe { sto_tpcc_protocol_validate(self.thread) },
            OK,
            "{}",
            native_error()
        );
        let (finished, report) = self.finish(true);
        assert_eq!((finished, report.disposition), (OK, COMMITTED));
        (status, value, time_term)
    }

    /// A point read of a key the table has never held. It leaves the key in
    /// the directory as a deleted entry without history.
    fn miss(&self, key: &[u8]) {
        assert_eq!(self.get(key), (MISS, Vec::new(), 0), "{}", native_error());
    }

    /// The entries of the table's directory, deleted keys included.
    fn directory_entries(&self) -> u64 {
        let mut usage = StoTpccTableUsageInfo::default();
        assert_eq!(unsafe { sto_tpcc_table_usage(self.table, &mut usage) }, OK);
        usage.retained_records
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

/// Coordinate, key and value of one sink call.
type Delivered = (Vec<u8>, Vec<u8>, Vec<u8>);

/// Row sink: copies the row into the `Vec<Delivered>` behind the context.
unsafe extern "C" fn deliver(
    context: *mut c_void,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
    value: MakoStoShardBytes,
) {
    let copy = |span: MakoStoShardBytes| {
        if span.len == 0 {
            Vec::new()
        } else {
            unsafe { slice::from_raw_parts(span.data, span.len) }.to_vec()
        }
    };
    let delivered = unsafe { &mut *context.cast::<Vec<Delivered>>() };
    delivered.push((copy(coordinate), copy(key), copy(value)));
}

fn span(bytes: &[u8]) -> MakoStoShardBytes {
    MakoStoShardBytes {
        data: bytes.as_ptr(),
        len: bytes.len(),
    }
}

/// The arguments of one `scan_next` after the context and the table.
#[derive(Clone, Copy)]
struct Lookahead<'a> {
    lo: MakoStoShardBytes,
    has_hi: u32,
    hi: &'a [u8],
    has_after: u32,
    after_coordinate: &'a [u8],
    after_key: &'a [u8],
    sink: Option<MakoStoShardRowSink>,
}

impl<'a> Lookahead<'a> {
    /// As the sharding crate calls: `[lo, hi)`, the cursor a previous row.
    fn new(lo: &'a [u8], hi: Option<&'a [u8]>, after: Option<&'a [u8]>) -> Self {
        Self {
            lo: span(lo),
            has_hi: u32::from(hi.is_some()),
            hi: hi.unwrap_or_default(),
            has_after: u32::from(after.is_some()),
            after_coordinate: after.unwrap_or_default(),
            after_key: after.unwrap_or_default(),
            sink: Some(deliver),
        }
    }
}

/// A `Native` whose table and thread are registered with the storage host
/// under a catalog id of the test's own; the tests of this file share one
/// process and the host's registry. Dropping it ends the registration before
/// the handles are destroyed, also when the test fails: an entry left behind
/// would point at a destroyed table.
struct Host {
    catalog: u64,
    native: Native,
}

impl Host {
    fn new(catalog: u64) -> Self {
        let host = Self {
            catalog,
            native: Native::new(),
        };
        unsafe {
            assert_eq!(
                mako_sto_shard_table_register(
                    catalog,
                    OWNER,
                    host.native.table,
                    u32::from(TABLE_ID)
                ),
                SHARD_OK,
                "{}",
                native_error()
            );
            assert_eq!(
                mako_sto_shard_thread_register(host.context(), OWNER, host.native.thread),
                SHARD_OK,
                "{}",
                native_error()
            );
        }
        host
    }

    /// The callback context: an address the host compares and never reads.
    fn context(&self) -> *mut c_void {
        self.native.db.cast()
    }

    /// Another address, for a caller the thread is not registered with.
    fn foreign_context(&self) -> *mut c_void {
        self.native.table.cast()
    }

    /// One `scan_next`. Checks what holds for every call: the sink is called
    /// once for OK and never otherwise, and a row's coordinate is its key.
    fn lookahead(
        &self,
        context: *mut c_void,
        catalog: u64,
        request: Lookahead<'_>,
    ) -> (u32, Vec<Row>) {
        let mut delivered: Vec<Delivered> = Vec::new();
        let status = unsafe {
            mako_sto_shard_scan_next(
                context,
                catalog,
                request.lo,
                request.has_hi,
                span(request.hi),
                request.has_after,
                span(request.after_coordinate),
                span(request.after_key),
                request.sink,
                (&raw mut delivered).cast(),
            )
        };
        assert_eq!(
            delivered.len(),
            usize::from(status == SHARD_OK),
            "status {status}: {}",
            native_error()
        );
        let rows = delivered
            .into_iter()
            .map(|(coordinate, key, value)| {
                assert_eq!(coordinate, key);
                (key, value)
            })
            .collect();
        (status, rows)
    }

    /// The first row of `[lo, hi)` after the cursor, on the registered table.
    /// A row must be one the sharding crate accepts: inside the range and
    /// strictly after the cursor.
    fn scan(&self, lo: &[u8], hi: Option<&[u8]>, after: Option<&[u8]>) -> (u32, Vec<Row>) {
        let found = self.lookahead(self.context(), self.catalog, Lookahead::new(lo, hi, after));
        for (key, _) in &found.1 {
            let key = key.as_slice();
            assert!(
                key >= lo && hi.is_none_or(|hi| key < hi),
                "row {key:?} is outside [{lo:?}, {hi:?})"
            );
            assert!(
                after.is_none_or(|after| key > after),
                "row {key:?} is not after the cursor {after:?}"
            );
        }
        found
    }

    /// One `put` with every argument chosen by the caller.
    fn put_as(
        &self,
        context: *mut c_void,
        catalog: u64,
        coordinate: &[u8],
        key: &[u8],
        value: &[u8],
    ) -> u32 {
        unsafe { mako_sto_shard_put(context, catalog, span(coordinate), span(key), span(value)) }
    }

    /// As the sharding crate calls for a raw table: the coordinate is the key.
    fn put(&self, key: &[u8], value: &[u8]) -> u32 {
        self.put_as(self.context(), self.catalog, key, key, value)
    }

    /// One `remove` with every argument chosen by the caller.
    fn remove_as(&self, context: *mut c_void, catalog: u64, coordinate: &[u8], key: &[u8]) -> u32 {
        unsafe { mako_sto_shard_remove(context, catalog, span(coordinate), span(key)) }
    }

    fn remove(&self, key: &[u8]) -> u32 {
        self.remove_as(self.context(), self.catalog, key, key)
    }

    /// Every row of `[lo, hi)`, each lookahead with the previous row as its
    /// cursor, until the host answers NOT_FOUND.
    fn walk(&self, lo: &[u8], hi: Option<&[u8]>) -> Vec<Row> {
        let mut rows: Vec<Row> = Vec::new();
        loop {
            let after = rows.last().map(|(key, _)| key.clone());
            let (status, mut next) = self.scan(lo, hi, after.as_deref());
            if status == SHARD_NOT_FOUND {
                return rows;
            }
            assert_eq!(status, SHARD_OK, "{}", native_error());
            rows.append(&mut next);
        }
    }

    /// No attempt is open on the thread handle, and it begins and aborts a
    /// new one.
    fn assert_idle(&self) {
        let mut report = StoTpccProtocolReport::default();
        let mut empty = 0;
        assert_eq!(
            unsafe { sto_tpcc_protocol_state(self.native.thread, &mut report, &mut empty) },
            OK
        );
        assert_eq!((report.phase, empty), (0, 1));
        self.native.begin();
        assert_eq!(self.native.finish(false).0, OK, "{}", native_error());
    }
}

impl Drop for Host {
    fn drop(&mut self) {
        // The thread first, then the table; `native` is dropped after this.
        let released = unsafe {
            [
                mako_sto_shard_thread_unregister(self.context()),
                mako_sto_shard_table_unregister(self.catalog, OWNER),
            ]
        };
        if !std::thread::panicking() {
            assert_eq!(released, [SHARD_OK; 2]);
        }
    }
}

fn row(key: &[u8], value: &[u8]) -> Row {
    (key.to_vec(), value.to_vec())
}

/// The lookahead reaches the first live row behind far more deleted keys
/// than one transaction may hold items, which the protocol scan above cannot.
#[test]
fn scan_next_crosses_five_thousand_tombstones_and_returns_the_first_live_row() {
    const WITH_HISTORY: usize = 4_700;
    const WITHOUT_HISTORY: usize = 300;
    let host = Host::new(9_002);
    let native = &host.native;
    // Deleted rows: each key keeps the history of its put and its delete.
    for index in 0..WITH_HISTORY {
        let key = deleted_key(index);
        native.commit_mutation(PUT, &key, b"gone");
        native.commit_mutation(DELETE, &key, b"");
    }
    // Keys that were only ever read: deleted entries without history.
    for index in 0..WITHOUT_HISTORY {
        native.miss(format!("e{index:04}").as_bytes());
    }
    native.commit_mutation(PUT, b"live", b"row");
    assert_eq!(native.rows(), 1);
    assert_eq!(
        native.directory_entries(),
        (WITH_HISTORY + WITHOUT_HISTORY + 1) as u64
    );
    let live = vec![row(b"live", b"row")];

    // All 5,000 deleted keys lie between the start of the table and the row.
    assert_eq!(
        host.scan(b"", None, None),
        (SHARD_OK, live.clone()),
        "{}",
        native_error()
    );
    // With an upper bound below the live row the range holds deleted keys
    // only: the end of the range, not an error and not the row.
    assert_eq!(host.scan(b"", Some(b"k"), None), (SHARD_NOT_FOUND, vec![]));
    assert_eq!(
        host.scan(b"", Some(b"live"), None),
        (SHARD_NOT_FOUND, vec![])
    );
    assert_eq!(
        host.scan(b"", Some(b"live\0"), None),
        (SHARD_OK, live.clone())
    );
    // A cursor on a deleted key in the middle of the run, and a lower bound
    // there.
    let middle = deleted_key(WITH_HISTORY / 2);
    assert_eq!(
        host.scan(b"", None, Some(&middle)),
        (SHARD_OK, live.clone())
    );
    assert_eq!(host.scan(&middle, None, None), (SHARD_OK, live));
    assert_eq!(
        host.scan(b"", None, Some(b"live")),
        (SHARD_NOT_FOUND, vec![])
    );
    host.assert_idle();
    assert_eq!(native.rows(), 1);
}

/// Order is the order of the key bytes, the cursor is exclusive on exactly
/// those bytes, and both bounds hold on the native directory.
#[test]
fn scan_next_orders_empty_nul_and_prefix_keys_and_honours_bounds() {
    let host = Host::new(9_003);
    let native = &host.native;
    // Loaded out of order; one row has the empty key and one an empty value.
    let all = vec![
        row(b"", b"empty key"),
        row(b"a", b""),
        row(b"a\0", b"one nul"),
        row(b"a\0\0", b"two nuls"),
        row(b"b", b"last"),
    ];
    for index in [3, 0, 4, 2, 1] {
        native.commit_mutation(PUT, &all[index].0, &all[index].1);
    }
    assert_eq!(native.rows(), 5);

    // A full walk, each lookahead with the previous row as its cursor.
    assert_eq!(host.walk(b"", None), all);
    // Without a cursor the lower bound is inclusive, the empty key included.
    assert_eq!(host.scan(b"", None, None), (SHARD_OK, vec![all[0].clone()]));
    assert_eq!(
        host.scan(b"a", None, None),
        (SHARD_OK, vec![all[1].clone()])
    );
    // A cursor is exclusive on its own bytes: the empty key, and a key whose
    // successor is the same key with a NUL appended.
    assert_eq!(
        host.scan(b"", None, Some(b"")),
        (SHARD_OK, vec![all[1].clone()])
    );
    assert_eq!(
        host.scan(b"", None, Some(b"a")),
        (SHARD_OK, vec![all[2].clone()])
    );
    assert_eq!(
        host.scan(b"a", None, Some(b"a")),
        (SHARD_OK, vec![all[2].clone()])
    );
    assert_eq!(
        host.scan(b"", None, Some(b"a\0")),
        (SHARD_OK, vec![all[3].clone()])
    );
    // A cursor below the range starts at the lower bound, inclusive.
    assert_eq!(
        host.scan(b"a\0", None, Some(b"a")),
        (SHARD_OK, vec![all[2].clone()])
    );
    assert_eq!(
        host.scan(b"a\0", None, Some(b"")),
        (SHARD_OK, vec![all[2].clone()])
    );
    // A cursor that is no key of the table.
    assert_eq!(
        host.scan(b"", None, Some(b"a\0\0\0")),
        (SHARD_OK, vec![all[4].clone()])
    );
    // A key equal to the upper bound is not in the range.
    assert_eq!(host.walk(b"a", Some(b"b")), all[1..4]);
    assert_eq!(host.walk(b"", Some(b"a\0")), all[..2]);
    assert_eq!(host.walk(b"a\0", Some(b"a\0\0")), all[2..3]);
    assert_eq!(
        host.scan(b"", Some(b"b"), Some(b"a\0\0")),
        (SHARD_NOT_FOUND, vec![])
    );
    assert_eq!(
        host.scan(b"", Some(b"a"), Some(b"")),
        (SHARD_NOT_FOUND, vec![])
    );
    // A cursor at or beyond the upper bound leaves nothing.
    assert_eq!(
        host.scan(b"", Some(b"a"), Some(b"a")),
        (SHARD_NOT_FOUND, vec![])
    );
    // Past the last row, and in a range that holds no key at all.
    assert_eq!(host.scan(b"", None, Some(b"b")), (SHARD_NOT_FOUND, vec![]));
    assert_eq!(host.scan(b"c", None, None), (SHARD_NOT_FOUND, vec![]));
    assert_eq!(
        host.scan(b"a\0\0\0", Some(b"b"), None),
        (SHARD_NOT_FOUND, vec![])
    );
    // A zero-length span may carry any pointer, null included.
    let mut null_lo = Lookahead::new(b"", None, None);
    null_lo.lo = MakoStoShardBytes {
        data: ptr::null(),
        len: 0,
    };
    assert_eq!(
        host.lookahead(host.context(), host.catalog, null_lo),
        (SHARD_OK, vec![all[0].clone()])
    );
    host.assert_idle();
}

/// `put` and `remove` commit through the participant protocol. A row that
/// was written by a protocol commit has history, and over such a row the
/// engine refuses a write that carries no decision.
#[test]
fn put_and_remove_commit_through_the_protocol_over_rows_with_history() {
    let host = Host::new(9_004);
    let native = &host.native;
    native.commit_mutation(PUT, b"k", b"first");
    let (status, value, written) = native.get(b"k");
    assert_eq!((status, value), (OK, b"first".to_vec()));
    assert_ne!(written, 0);

    // `put` over the row: a new version whose decision is later than the one
    // it replaces.
    assert_eq!(host.put(b"k", b"second"), SHARD_OK, "{}", native_error());
    let (status, value, overwritten) = native.get(b"k");
    assert_eq!((status, value), (OK, b"second".to_vec()));
    assert!(overwritten > written, "{overwritten} after {written}");
    assert_eq!(native.rows(), 1);

    // New rows: a key the table never held, the empty key, an empty value,
    // and the longest value the protocol's log record can carry.
    let longest = vec![0xa5_u8; 65_535];
    assert_eq!(host.put(b"n", b"new"), SHARD_OK, "{}", native_error());
    assert_eq!(host.put(b"", b"empty key"), SHARD_OK, "{}", native_error());
    assert_eq!(host.put(b"e", b""), SHARD_OK, "{}", native_error());
    assert_eq!(host.put(b"long", &longest), SHARD_OK, "{}", native_error());
    assert_eq!(native.rows(), 5);
    let (status, value, created) = native.get(b"n");
    assert_eq!((status, value), (OK, b"new".to_vec()));
    assert_ne!(created, 0);
    // The next lookahead on this thread sees every committed put.
    assert_eq!(
        host.walk(b"", None),
        vec![
            row(b"", b"empty key"),
            row(b"e", b""),
            row(b"k", b"second"),
            row(b"long", &longest),
            row(b"n", b"new"),
        ]
    );

    // `remove` of the row: a deleted version with a later decision.
    assert_eq!(host.remove(b"k"), SHARD_OK, "{}", native_error());
    let (status, value, removed) = native.get(b"k");
    assert_eq!((status, value), (MISS, Vec::new()));
    assert!(removed > overwritten, "{removed} after {overwritten}");
    assert_eq!(native.rows(), 4);
    // Removing an absent key succeeds and changes nothing: the key that was
    // just deleted, and a key the table never held.
    assert_eq!(host.remove(b"k"), SHARD_OK, "{}", native_error());
    assert_eq!(native.get(b"k"), (MISS, Vec::new(), removed));
    assert_eq!(host.remove(b"never"), SHARD_OK, "{}", native_error());
    assert_eq!(native.rows(), 4);
    // The empty key, and a put over the deleted row.
    assert_eq!(host.remove(b""), SHARD_OK, "{}", native_error());
    assert_eq!(host.put(b"k", b"third"), SHARD_OK, "{}", native_error());
    let (status, value, restored) = native.get(b"k");
    assert_eq!((status, value), (OK, b"third".to_vec()));
    assert!(restored > removed, "{restored} after {removed}");
    assert_eq!(native.rows(), 4);
    assert_eq!(
        host.walk(b"", None),
        vec![
            row(b"e", b""),
            row(b"k", b"third"),
            row(b"long", &longest),
            row(b"n", b"new"),
        ]
    );
    host.assert_idle();
}

/// An error is never the end of a range, and a refused call changes nothing.
#[test]
fn oversized_values_unregistered_tables_and_foreign_contexts_are_refused_without_effect() {
    const UNREGISTERED: u64 = 9_995;
    let host = Host::new(9_005);
    let native = &host.native;
    native.commit_mutation(PUT, b"k", b"value");
    let rows = vec![row(b"k", b"value")];

    // A table that is not registered for the thread's owner: IO, also where
    // a registered table would answer NOT_FOUND (past its last row, and with
    // the cursor at the upper bound).
    for request in [
        Lookahead::new(b"", None, None),
        Lookahead::new(b"", None, Some(b"k")),
        Lookahead::new(b"", Some(b"k"), Some(b"k")),
    ] {
        assert_eq!(
            host.lookahead(host.context(), UNREGISTERED, request),
            (SHARD_IO, vec![])
        );
        assert_eq!(
            host.lookahead(host.context(), host.catalog, request).0,
            if request.has_after == 0 {
                SHARD_OK
            } else {
                SHARD_NOT_FOUND
            }
        );
    }
    // A context the thread is not registered with.
    assert_eq!(
        host.lookahead(
            host.foreign_context(),
            host.catalog,
            Lookahead::new(b"", None, None)
        ),
        (SHARD_INVALID, vec![])
    );
    assert_eq!(
        host.lookahead(
            ptr::null_mut(),
            host.catalog,
            Lookahead::new(b"", None, None)
        ),
        (SHARD_INVALID, vec![])
    );

    // `put` and `remove` refuse the same callers, and a coordinate that is
    // not the key.
    let context = host.context();
    let foreign = host.foreign_context();
    assert_eq!(
        host.put_as(context, UNREGISTERED, b"k", b"k", b"other"),
        SHARD_IO
    );
    assert_eq!(host.remove_as(context, UNREGISTERED, b"k", b"k"), SHARD_IO);
    assert_eq!(
        host.put_as(foreign, host.catalog, b"k", b"k", b"other"),
        SHARD_INVALID
    );
    assert_eq!(
        host.remove_as(foreign, host.catalog, b"k", b"k"),
        SHARD_INVALID
    );
    assert_eq!(
        host.put_as(context, host.catalog, b"j", b"k", b"other"),
        SHARD_INVALID
    );
    assert_eq!(
        host.remove_as(context, host.catalog, b"j", b"k"),
        SHARD_INVALID
    );
    // The protocol's log record has 16-bit lengths: one byte more than
    // 65,535 in a value or a key is refused before the engine is asked.
    let oversized = vec![0x5a_u8; 65_536];
    assert_eq!(host.put(b"big", &oversized), SHARD_INVALID);
    assert_eq!(host.put(&oversized, b"value"), SHARD_INVALID);
    assert_eq!(host.remove(&oversized), SHARD_INVALID);
    // A key longer than the engine's configured maximum (1,024 bytes, as the
    // C++ adapter sets it) meets an engine limit: EXHAUSTED from every verb,
    // also for a lookahead with such a bound or cursor, which must not read
    // as the end of the range.
    let long_key = vec![b'x'; 1_025];
    assert_eq!(host.put(&long_key, b"value"), SHARD_EXHAUSTED);
    assert_eq!(host.remove(&long_key), SHARD_EXHAUSTED);
    for request in [
        Lookahead::new(&long_key, None, None),
        Lookahead::new(b"", None, Some(&long_key)),
        Lookahead::new(b"", Some(&long_key), None),
    ] {
        assert_eq!(
            host.lookahead(context, host.catalog, request),
            (SHARD_EXHAUSTED, vec![]),
            "{}",
            native_error()
        );
    }

    host.assert_idle();
    // Nothing reached the table: its one row, and not even a directory entry
    // for a refused key.
    assert_eq!(native.rows(), 1);
    assert_eq!(native.directory_entries(), 1);
    assert_eq!(host.walk(b"", None), rows);
    assert_eq!(native.get(b"k").1, b"value");
    assert_eq!(native.get(b"big"), (MISS, Vec::new(), 0));
}

/// Whatever a verb answers, it leaves no attempt open on the thread handle.
#[test]
fn verbs_leave_the_thread_handle_idle_on_every_path() {
    const UNREGISTERED: u64 = 9_996;
    let host = Host::new(9_006);
    let native = &host.native;
    native.commit_mutation(PUT, b"k", b"value");
    let first = Lookahead::new(b"", None, None);
    let found = (SHARD_OK, vec![row(b"k", b"value")]);

    // A row, the end of the range, and a range the cursor has left.
    assert_eq!(host.scan(b"", None, None), found);
    host.assert_idle();
    assert_eq!(host.scan(b"", None, Some(b"k")), (SHARD_NOT_FOUND, vec![]));
    host.assert_idle();
    assert_eq!(
        host.scan(b"", Some(b"k"), Some(b"k")),
        (SHARD_NOT_FOUND, vec![])
    );
    host.assert_idle();

    // Malformed arguments.
    let malformed = [
        Lookahead {
            sink: None,
            ..first
        },
        Lookahead { has_hi: 2, ..first },
        Lookahead {
            has_after: 2,
            ..first
        },
        // An empty range and a reversed one.
        Lookahead::new(b"k", Some(b"k"), None),
        Lookahead::new(b"l", Some(b"k"), None),
        // A raw table's cursor has its key as its coordinate.
        Lookahead {
            has_after: 1,
            after_coordinate: b"j",
            after_key: b"k",
            ..first
        },
    ];
    for (index, request) in malformed.into_iter().enumerate() {
        assert_eq!(
            host.lookahead(host.context(), host.catalog, request),
            (SHARD_INVALID, vec![]),
            "malformed request {index}"
        );
        host.assert_idle();
    }

    // An unknown table and a foreign context.
    assert_eq!(
        host.lookahead(host.context(), UNREGISTERED, first),
        (SHARD_IO, vec![])
    );
    host.assert_idle();
    assert_eq!(
        host.lookahead(host.foreign_context(), host.catalog, first),
        (SHARD_INVALID, vec![])
    );
    host.assert_idle();

    // `put`: a new key and an overwrite. `remove`: a row, the key it has
    // just deleted, and a key the table never held.
    for value in [&b"new"[..], b"overwritten"] {
        assert_eq!(host.put(b"p", value), SHARD_OK, "{}", native_error());
        host.assert_idle();
    }
    for key in [&b"p"[..], b"p", b"never"] {
        assert_eq!(host.remove(key), SHARD_OK, "{}", native_error());
        host.assert_idle();
    }
    // Refused writes.
    let oversized = vec![0_u8; 65_536];
    let context = host.context();
    let refused = [
        host.put(b"p", &oversized),
        host.remove(&oversized),
        host.put_as(context, host.catalog, b"j", b"p", b"value"),
        host.remove_as(context, host.catalog, b"j", b"k"),
        host.put_as(host.foreign_context(), host.catalog, b"p", b"p", b"value"),
        host.remove_as(host.foreign_context(), host.catalog, b"k", b"k"),
    ];
    assert_eq!(refused, [SHARD_INVALID; 6]);
    host.assert_idle();
    assert_eq!(
        host.put_as(context, UNREGISTERED, b"p", b"p", b"value"),
        SHARD_IO
    );
    host.assert_idle();
    assert_eq!(host.remove_as(context, UNREGISTERED, b"k", b"k"), SHARD_IO);
    host.assert_idle();
    assert_eq!(native.rows(), 1);

    // A handle with an attempt open is refused, and that attempt is left as
    // it was for its owner to finish.
    native.begin();
    assert_eq!(host.scan(b"", None, None), (SHARD_INVALID, vec![]));
    assert_eq!(host.put(b"p", b"value"), SHARD_INVALID);
    assert_eq!(host.remove(b"k"), SHARD_INVALID);
    let mut report = StoTpccProtocolReport::default();
    let mut empty = 0;
    assert_eq!(
        unsafe { sto_tpcc_protocol_state(native.thread, &mut report, &mut empty) },
        OK
    );
    assert_eq!((report.phase, empty), (1, 1));
    let (status, report) = native.finish(false);
    assert_eq!((status, report.disposition), (OK, ABORTED));
    host.assert_idle();

    // The registration itself: a registered thread cannot register again,
    // only its own context ends the binding, and a handle with an attempt
    // open cannot be registered.
    unsafe {
        assert_eq!(
            mako_sto_shard_thread_register(host.context(), OWNER, native.thread),
            SHARD_INVALID
        );
        assert_eq!(
            mako_sto_shard_thread_unregister(host.foreign_context()),
            SHARD_INVALID
        );
        assert_eq!(host.scan(b"", None, None), found);
        assert_eq!(mako_sto_shard_thread_unregister(host.context()), SHARD_OK);
        assert_eq!(host.scan(b"", None, None), (SHARD_INVALID, vec![]));
        native.begin();
        assert_eq!(
            mako_sto_shard_thread_register(host.context(), OWNER, native.thread),
            SHARD_INVALID
        );
        assert_eq!(native.finish(false).0, OK);
        assert_eq!(
            mako_sto_shard_thread_register(host.context(), OWNER, native.thread),
            SHARD_OK
        );
    }
    assert_eq!(host.scan(b"", None, None), found);
    host.assert_idle();
    assert_eq!(native.rows(), 1);
}

/// The lookahead selects the newest committed version of a row and skips a
/// deleted row although its history still holds a value.
#[test]
fn scan_next_returns_the_newest_committed_version_and_no_deleted_row_with_history() {
    let host = Host::new(9_007);
    let native = &host.native;
    native.commit_mutation(PUT, b"k1", b"first");
    native.commit_mutation(PUT, b"k1", b"second, longer than the first");
    native.commit_mutation(PUT, b"k1", b"third");
    native.commit_mutation(PUT, b"k2", b"deleted later");
    native.commit_mutation(DELETE, b"k2", b"");
    native.commit_mutation(PUT, b"k3", b"only");
    assert_eq!(native.rows(), 2);

    assert_eq!(
        host.walk(b"", None),
        vec![row(b"k1", b"third"), row(b"k3", b"only")]
    );
    // The deleted row is not delivered when a lookahead starts on it either.
    assert_eq!(
        host.scan(b"k2", None, None),
        (SHARD_OK, vec![row(b"k3", b"only")])
    );
    assert_eq!(
        host.scan(b"k2", Some(b"k3"), None),
        (SHARD_NOT_FOUND, vec![])
    );
    host.assert_idle();
}
