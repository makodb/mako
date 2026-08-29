#![cfg(mtree_native_integration)]

use std::{ffi::c_void, ptr, slice};
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const DUPLICATE: i32 = 2;
const BUFFER_TOO_SMALL: i32 = 4;

fn last_error() -> String {
    let length = sto_tpcc_last_error_length();
    let mut bytes = vec![0_i8; length + 1];
    let mut actual = usize::MAX;
    let status = unsafe { sto_tpcc_last_error_copy(bytes.as_mut_ptr(), bytes.len(), &mut actual) };
    assert_eq!(status, OK);
    assert_eq!(actual, length);
    String::from_utf8(bytes[..length].iter().map(|byte| *byte as u8).collect()).unwrap()
}

fn expect(actual: i32, expected: i32) {
    assert_eq!(actual, expected, "FFI error: {}", last_error());
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

unsafe fn get(
    thread: *mut StoTpccThread,
    table: *mut StoTpccTable,
    key: &[u8],
) -> Result<Vec<u8>, i32> {
    let mut bytes = vec![0_u8; 128];
    let mut actual = 0;
    let status = unsafe {
        sto_tpcc_get(
            thread,
            table,
            key.as_ptr(),
            key.len(),
            bytes.as_mut_ptr(),
            bytes.len(),
            &mut actual,
        )
    };
    if status == OK {
        bytes.truncate(actual);
        Ok(bytes)
    } else {
        Err(status)
    }
}

unsafe extern "C" fn collect_scan(
    context: *mut c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    // SAFETY: The test passes a live vector and the FFI guarantees row slices
    // for this callback invocation.
    let rows = unsafe { &mut *context.cast::<Vec<(Vec<u8>, Vec<u8>)>>() };
    let key = unsafe { slice::from_raw_parts(key, key_length) };
    let value = unsafe { slice::from_raw_parts(value, value_length) };
    rows.push((key.to_vec(), value.to_vec()));
    0
}

#[test]
fn ffi_crud_scan_read_your_writes_and_cross_table_atomicity() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);

        let table_config = StoTpccTableConfig {
            max_retained_records: 1_024,
            max_retained_key_bytes: 1 << 20,
            max_consumed_record_ids: 4_096,
            ..StoTpccTableConfig::default()
        };
        let mut first = ptr::null_mut();
        let mut second = ptr::null_mut();
        expect(sto_tpcc_table_create(db, &table_config, &mut first), OK);
        expect(sto_tpcc_table_create(db, &table_config, &mut second), OK);

        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"a", b"one"), OK);
        expect(
            sto_tpcc_insert(thread, second, b"b".as_ptr(), 1, b"two".as_ptr(), 3),
            OK,
        );
        assert_eq!(get(thread, first, b"a"), Ok(b"one".to_vec()));
        expect(sto_tpcc_txn_commit(thread), OK);

        let mut size = 0;
        expect(sto_tpcc_table_size(first, &mut size), OK);
        assert_eq!(size, 1);
        expect(sto_tpcc_table_size(second, &mut size), OK);
        assert_eq!(size, 1);

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_insert(thread, first, b"a".as_ptr(), 1, b"x".as_ptr(), 1),
            DUPLICATE,
        );
        expect(sto_tpcc_txn_abort(thread), OK);
        expect(sto_tpcc_txn_abort(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        let mut tiny = [0_u8; 1];
        let mut actual = 0;
        expect(
            sto_tpcc_get(
                thread,
                first,
                b"a".as_ptr(),
                1,
                tiny.as_mut_ptr(),
                tiny.len(),
                &mut actual,
            ),
            BUFFER_TOO_SMALL,
        );
        assert_eq!(actual, 3);
        assert_eq!(get(thread, first, b"a"), Ok(b"one".to_vec()));
        expect(sto_tpcc_txn_commit(thread), OK);

        // Abort must roll back both tables, including membership changes.
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"a", b"changed"), OK);
        expect(put(thread, second, b"c", b"three"), OK);
        expect(sto_tpcc_txn_abort(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, first, b"a"), Ok(b"one".to_vec()));
        assert_eq!(get(thread, second, b"c"), Err(MISS));
        expect(sto_tpcc_txn_commit(thread), OK);

        // The corresponding cross-table commit publishes both changes.
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"a", b"changed"), OK);
        expect(put(thread, second, b"c", b"three"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"c", b"3"), OK);
        expect(put(thread, first, b"d", b"4"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        let mut rows: Vec<(Vec<u8>, Vec<u8>)> = Vec::new();
        let mut visited = 0;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_scan(
                thread,
                first,
                0,
                1,
                b"a".as_ptr(),
                1,
                2,
                b"z".as_ptr(),
                1,
                10,
                Some(collect_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 3);
        assert_eq!(
            rows.iter().map(|row| row.0.as_slice()).collect::<Vec<_>>(),
            [b"a".as_slice(), b"c".as_slice(), b"d".as_slice()]
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        rows.clear();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_scan(
                thread,
                first,
                1,
                0,
                ptr::null(),
                0,
                0,
                ptr::null(),
                0,
                2,
                Some(collect_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 2);
        assert_eq!(rows[0].0, b"d");
        assert_eq!(rows[1].0, b"c");
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(sto_tpcc_remove(thread, first, b"d".as_ptr(), 1), OK);
        assert_eq!(get(thread, first, b"d"), Err(MISS));
        expect(sto_tpcc_txn_commit(thread), OK);
        expect(sto_tpcc_table_size(first, &mut size), OK);
        assert_eq!(size, 2);

        // Destruction owns the final active-transaction cleanup path.
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, first, b"uncommitted", b"value"), OK);
        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(second), OK);
        expect(sto_tpcc_table_destroy(first), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
