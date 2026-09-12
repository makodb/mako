#![cfg(mtree_native_integration)]

use std::ptr;
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const FATAL: i32 = 5;
const RESOURCE_EXHAUSTED: i32 = 6;

fn expect(actual: i32, expected: i32) {
    let mut error = [0_i8; 512];
    let mut length = 0;
    unsafe { sto_tpcc_last_error_copy(error.as_mut_ptr(), error.len(), &mut length) };
    let diagnostic = String::from_utf8_lossy(
        &error[..length.min(error.len() - 1)]
            .iter()
            .map(|byte| *byte as u8)
            .collect::<Vec<_>>(),
    )
    .into_owned();
    assert_eq!(actual, expected, "{diagnostic}");
}

unsafe fn database(budget: u64) -> *mut StoTpccDb {
    let config = StoTpccDbConfig {
        max_threads: 4,
        max_key_length: 64,
        max_items_per_txn: 32,
        max_locks_per_txn: 64,
        max_registry_bytes: budget,
    };
    let mut db = ptr::null_mut();
    expect(unsafe { sto_tpcc_db_create(&config, &mut db) }, OK);
    db
}

unsafe fn table(db: *mut StoTpccDb, config: &StoTpccTableConfig) -> *mut StoTpccTable {
    let mut table = ptr::null_mut();
    expect(unsafe { sto_tpcc_table_create(db, config, &mut table) }, OK);
    table
}

unsafe fn worker(db: *mut StoTpccDb) -> *mut StoTpccThread {
    let mut thread = ptr::null_mut();
    expect(unsafe { sto_tpcc_thread_create(db, &mut thread) }, OK);
    thread
}

unsafe fn usage(table: *const StoTpccTable) -> StoTpccTableUsageInfo {
    let mut usage = StoTpccTableUsageInfo::default();
    expect(unsafe { sto_tpcc_table_usage(table, &mut usage) }, OK);
    usage
}

unsafe fn db_usage(db: *const StoTpccDb) -> StoTpccDbUsageInfo {
    let mut usage = StoTpccDbUsageInfo::default();
    expect(unsafe { sto_tpcc_db_usage(db, &mut usage) }, OK);
    usage
}

unsafe fn put(
    thread: *mut StoTpccThread,
    table: *mut StoTpccTable,
    key: &[u8],
    value: &[u8],
) -> i32 {
    unsafe {
        sto_tpcc_put(
            thread,
            table,
            key.as_ptr(),
            key.len(),
            value.as_ptr(),
            value.len(),
        )
    }
}

unsafe fn read(thread: *mut StoTpccThread, table: *mut StoTpccTable, key: &[u8]) -> (i32, Vec<u8>) {
    let mut value = [0_u8; 32];
    let mut length = usize::MAX;
    let status = unsafe {
        sto_tpcc_get(
            thread,
            table,
            key.as_ptr(),
            key.len(),
            value.as_mut_ptr(),
            value.len(),
            &mut length,
        )
    };
    (status, value[..length.min(value.len())].to_vec())
}

#[test]
fn scalar_capacity_aborts_staged_changes_and_preserves_existing_access() {
    unsafe {
        // Exercise each independent quota without requiring large allocations.
        for config in [
            StoTpccTableConfig {
                max_retained_records: 2,
                max_retained_key_bytes: 64,
                max_consumed_record_ids: 16,
                ..Default::default()
            },
            StoTpccTableConfig {
                max_retained_records: 16,
                max_retained_key_bytes: 2,
                max_consumed_record_ids: 16,
                ..Default::default()
            },
            StoTpccTableConfig {
                max_retained_records: 16,
                max_retained_key_bytes: 64,
                max_consumed_record_ids: 2,
                ..Default::default()
            },
        ] {
            let db = database(0);
            let table = table(db, &config);
            let thread = worker(db);
            expect(sto_tpcc_txn_begin(thread), OK);
            expect(put(thread, table, b"a", b"original"), OK);
            expect(sto_tpcc_txn_commit(thread), OK);

            expect(sto_tpcc_txn_begin(thread), OK);
            expect(put(thread, table, b"a", b"must rollback"), OK);
            expect(put(thread, table, b"b", b"must rollback"), OK);
            expect(put(thread, table, b"c", b"no room"), RESOURCE_EXHAUSTED);
            // The failed operation already ended the attempt and native scope.
            expect(sto_tpcc_txn_commit(thread), FATAL);
            expect(sto_tpcc_txn_begin(thread), OK);
            assert_eq!(read(thread, table, b"a"), (OK, b"original".to_vec()));
            assert_eq!(read(thread, table, b"b"), (MISS, Vec::new()));
            expect(put(thread, table, b"a", b"updated"), OK);
            expect(sto_tpcc_txn_commit(thread), OK);
            // Missing-key reads also need a stable membership record. Their
            // capacity error must end the attempt just like a write error.
            expect(sto_tpcc_txn_begin(thread), OK);
            expect(read(thread, table, b"c").0, RESOURCE_EXHAUSTED);
            expect(sto_tpcc_txn_begin(thread), OK);
            assert_eq!(read(thread, table, b"a"), (OK, b"updated".to_vec()));
            expect(sto_tpcc_txn_commit(thread), OK);
            let mut rows = u64::MAX;
            expect(sto_tpcc_table_size(table, &mut rows), OK);
            assert_eq!(rows, 1);
            let usage = usage(table);
            assert_eq!(usage.retained_records, 2);
            assert_eq!(usage.retained_key_bytes, 2);
            assert_eq!(usage.max_retained_records, config.max_retained_records);
            assert_eq!(usage.max_retained_key_bytes, config.max_retained_key_bytes);
            assert_eq!(
                usage.max_consumed_record_ids,
                config.max_consumed_record_ids
            );
            assert!(usage.consumed_record_ids <= config.max_consumed_record_ids);
            expect(sto_tpcc_txn_abort(thread), OK);
            expect(sto_tpcc_thread_destroy(thread), OK);
            expect(sto_tpcc_table_destroy(table), OK);
            expect(sto_tpcc_db_destroy(db), OK);
        }
    }
}

#[test]
fn registry_budget_is_shared_and_exhaustion_does_not_poison_existing_tables() {
    unsafe {
        let config = StoTpccTableConfig {
            max_retained_records: 2048,
            max_retained_key_bytes: u64::MAX,
            max_consumed_record_ids: 2048,
            ..Default::default()
        };
        // Measure public accounting, so the test does not depend on private
        // chunk sizes or platform-specific Rust layouts.
        let calibration = database(0);
        let first = table(calibration, &config);
        let second = table(calibration, &config);
        let thread = worker(calibration);
        let empty_bytes = db_usage(calibration).allocated_registry_bytes;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"a", b"first"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);
        let budget = db_usage(calibration).allocated_registry_bytes;
        assert!(budget > empty_bytes);
        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(first), OK);
        expect(sto_tpcc_table_destroy(second), OK);
        expect(sto_tpcc_db_destroy(calibration), OK);

        let db = database(budget);
        let first = table(db, &config);
        let second = table(db, &config);
        let thread = worker(db);
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"a", b"first"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);
        assert_eq!(db_usage(db).allocated_registry_bytes, budget);
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"a", b"must rollback"), OK);
        expect(put(thread, second, b"b", b"no room"), RESOURCE_EXHAUSTED);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, first, b"a"), (OK, b"first".to_vec()));
        expect(put(thread, first, b"c", b"fits existing chunk"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);
        let total = db_usage(db);
        assert_eq!(total.max_registry_bytes, budget);
        assert!(total.allocated_registry_bytes <= budget);
        assert_eq!(
            usage(first).allocated_registry_bytes + usage(second).allocated_registry_bytes,
            total.allocated_registry_bytes
        );
        assert_eq!(usage(second).retained_records, 0);
        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(first), OK);
        expect(sto_tpcc_table_destroy(second), OK);
        assert_eq!(db_usage(db).allocated_registry_bytes, 0);
        expect(sto_tpcc_db_destroy(db), OK);

        let db = database(1);
        let mut rejected = ptr::dangling_mut();
        expect(
            sto_tpcc_table_create(db, &config, &mut rejected),
            RESOURCE_EXHAUSTED,
        );
        assert!(rejected.is_null());
        assert_eq!(db_usage(db).allocated_registry_bytes, 0);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
