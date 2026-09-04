#![cfg(mtree_native_integration)]

use std::{ffi::c_void, ptr, slice};
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const DUPLICATE: i32 = 2;
const BUFFER_TOO_SMALL: i32 = 4;
const FATAL: i32 = 5;

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

unsafe fn get_with_buffer(
    thread: *mut StoTpccThread,
    table: *mut StoTpccTable,
    key: &[u8],
    capacity: usize,
    fill: u8,
) -> (i32, usize, Vec<u8>) {
    let mut bytes = vec![fill; capacity];
    let output = if bytes.is_empty() {
        ptr::null_mut()
    } else {
        bytes.as_mut_ptr()
    };
    let mut actual = usize::MAX;
    let status = unsafe {
        sto_tpcc_get(
            thread,
            table,
            key.as_ptr(),
            key.len(),
            output,
            bytes.len(),
            &mut actual,
        )
    };
    (status, actual, bytes)
}

unsafe fn put_fixed<const KEY_LENGTH: usize>(
    thread: *mut StoTpccThread,
    table: *mut StoTpccTable,
    keys: &[[u8; KEY_LENGTH]],
    values: &[&[u8]],
    mode: StoTpccFixedPutMode,
) -> (i32, StoTpccFixedPutResult) {
    assert_eq!(keys.len(), values.len());
    let descriptors: Vec<_> = values
        .iter()
        .map(|value| StoTpccFixedValue {
            data: if value.is_empty() {
                ptr::null()
            } else {
                value.as_ptr()
            },
            length: value.len(),
        })
        .collect();
    let mut result = StoTpccFixedPutResult {
        inserted: usize::MAX,
        first_duplicate: 0,
    };
    let status = unsafe {
        sto_tpcc_put_fixed(
            thread,
            table,
            if keys.is_empty() {
                ptr::null()
            } else {
                keys.as_ptr().cast()
            },
            keys.len(),
            KEY_LENGTH,
            if keys.is_empty() {
                ptr::null()
            } else {
                descriptors.as_ptr()
            },
            mode,
            &mut result,
        )
    };
    (status, result)
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

unsafe extern "C" fn collect_one_scan(
    context: *mut c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    // SAFETY: This callback has the identical synchronous row contract as the
    // collecting callback above.
    let _ = unsafe { collect_scan(context, key, key_length, value, value_length) };
    1
}

enum FixedMutationStep {
    Keep,
    Put(Vec<u8>),
    Remove,
    Fail,
}

struct FixedMutationContext {
    steps: Vec<FixedMutationStep>,
    observed: Vec<Option<Vec<u8>>>,
}

unsafe extern "C" fn apply_fixed_mutation(
    context: *mut c_void,
    index: usize,
    current: *const u8,
    current_length: usize,
    out_replacement: *mut *const u8,
    out_replacement_length: *mut usize,
) -> StoTpccFixedModifyAction {
    // SAFETY: This test retains the context and action bytes across the full
    // synchronous endpoint invocation.
    let context = unsafe { &mut *context.cast::<FixedMutationContext>() };
    let observed = if current.is_null() {
        assert_eq!(current_length, 0);
        None
    } else {
        Some(unsafe { slice::from_raw_parts(current, current_length) }.to_vec())
    };
    context.observed.push(observed);
    unsafe {
        *out_replacement = ptr::null();
        *out_replacement_length = 0;
    }
    match &context.steps[index] {
        FixedMutationStep::Keep => STO_TPCC_FIXED_MODIFY_KEEP,
        FixedMutationStep::Put(replacement) => {
            unsafe {
                *out_replacement = replacement.as_ptr();
                *out_replacement_length = replacement.len();
            }
            STO_TPCC_FIXED_MODIFY_PUT
        }
        FixedMutationStep::Remove => STO_TPCC_FIXED_MODIFY_REMOVE,
        FixedMutationStep::Fail => STO_TPCC_FIXED_MODIFY_FAILED,
    }
}

#[test]
fn bounded_atomic_value_config_round_trips_cell_boundaries_through_the_c_abi() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let config = StoTpccTableConfig {
            max_retained_records: 8,
            max_retained_key_bytes: 128,
            max_consumed_record_ids: 8,
            bounded_atomic_values: 1,
            ..StoTpccTableConfig::default()
        };
        let mut table = ptr::null_mut();
        expect(sto_tpcc_table_create(db, &config, &mut table), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let key = b"bounded";
        for length in [39, 160, 161, 38, 160] {
            let value = (0..length)
                .map(|index| (index as u8).wrapping_mul(19).wrapping_add(length as u8))
                .collect::<Vec<_>>();
            expect(sto_tpcc_txn_begin(thread), OK);
            expect(put(thread, table, key, &value), OK);
            expect(sto_tpcc_txn_commit(thread), OK);

            expect(sto_tpcc_txn_begin(thread), OK);
            let (status, actual, output) = get_with_buffer(thread, table, key, value.len(), 0xa5);
            expect(status, OK);
            assert_eq!(actual, value.len());
            assert_eq!(&output[..actual], value);
            expect(sto_tpcc_txn_commit(thread), OK);
        }

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(table), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}

#[test]
fn sealed_directory_keeps_existing_rows_mutable_and_rejects_new_keys() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let mut table = ptr::null_mut();
        expect(sto_tpcc_table_create(db, ptr::null(), &mut table), OK);

        let mut loader = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut loader), OK);
        expect(sto_tpcc_txn_begin(loader), OK);
        expect(put(loader, table, b"loaded", b"before"), OK);
        expect(sto_tpcc_txn_commit(loader), OK);
        expect(sto_tpcc_thread_destroy(loader), OK);

        expect(sto_tpcc_table_seal_directory_structure(table), OK);

        let mut worker = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut worker), OK);
        expect(sto_tpcc_txn_begin(worker), OK);
        assert_eq!(get(worker, table, b"loaded"), Ok(b"before".to_vec()));
        expect(put(worker, table, b"loaded", b"after"), OK);
        expect(sto_tpcc_txn_commit(worker), OK);

        expect(sto_tpcc_txn_begin(worker), OK);
        assert_eq!(put(worker, table, b"new", b"rejected"), FATAL);
        let error = last_error();
        assert!(
            error.contains("Masstree directory structure is sealed"),
            "unexpected error: {error}"
        );
        expect(sto_tpcc_txn_abort(worker), OK);

        expect(sto_tpcc_txn_begin(worker), OK);
        assert_eq!(get(worker, table, b"loaded"), Ok(b"after".to_vec()));
        expect(sto_tpcc_txn_commit(worker), OK);
        let mut rows = u64::MAX;
        expect(sto_tpcc_table_size(table, &mut rows), OK);
        assert_eq!(rows, 1);

        expect(sto_tpcc_thread_destroy(worker), OK);
        expect(sto_tpcc_table_destroy(table), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}

#[test]
fn fixed_mutation_batches_preserve_duplicate_order_size_and_abort_recovery() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let mut table = ptr::null_mut();
        expect(sto_tpcc_table_create(db, ptr::null(), &mut table), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let key4_a = [1, 2, 3, 4];
        let key4_b = [5, 6, 7, 8];
        let repeated = [key4_a, key4_a, key4_b, key4_b];
        let mut context = FixedMutationContext {
            steps: vec![
                FixedMutationStep::Put(b"first".to_vec()),
                FixedMutationStep::Put(b"second".to_vec()),
                FixedMutationStep::Put(b"temporary".to_vec()),
                FixedMutationStep::Remove,
            ],
            observed: Vec::new(),
        };
        let mut visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_modify_fixed(
                thread,
                table,
                repeated.as_ptr().cast(),
                repeated.len(),
                4,
                Some(apply_fixed_mutation),
                (&mut context as *mut FixedMutationContext).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, repeated.len());
        assert_eq!(
            context.observed,
            [
                None,
                Some(b"first".to_vec()),
                None,
                Some(b"temporary".to_vec()),
            ]
        );
        expect(sto_tpcc_txn_commit(thread), OK);
        let mut rows = u64::MAX;
        expect(sto_tpcc_table_size(table, &mut rows), OK);
        assert_eq!(rows, 1);

        let key8 = [8, 7, 6, 5, 4, 3, 2, 1];
        context = FixedMutationContext {
            steps: vec![FixedMutationStep::Put(b"eight".to_vec())],
            observed: Vec::new(),
        };
        visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_modify_fixed(
                thread,
                table,
                key8.as_ptr(),
                1,
                8,
                Some(apply_fixed_mutation),
                (&mut context as *mut FixedMutationContext).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(context.observed, [None]);
        expect(sto_tpcc_txn_commit(thread), OK);

        // Customer and order-line shaped keys exercise the wider mutation
        // dispatch used by the TPC-C workload. Each call must publish its PUT
        // and report the exact delivered prefix just like widths four/eight.
        let key12 = [12; 12];
        let key16 = [16; 16];
        for (key, width, replacement) in [
            (&key12[..], 12, &b"twelve"[..]),
            (&key16[..], 16, &b"sixteen"[..]),
        ] {
            context = FixedMutationContext {
                steps: vec![FixedMutationStep::Put(replacement.to_vec())],
                observed: Vec::new(),
            };
            visited = usize::MAX;
            expect(sto_tpcc_txn_begin(thread), OK);
            expect(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    key.as_ptr(),
                    1,
                    width,
                    Some(apply_fixed_mutation),
                    (&mut context as *mut FixedMutationContext).cast(),
                    &mut visited,
                ),
                OK,
            );
            assert_eq!(visited, 1);
            assert_eq!(context.observed, [None]);
            expect(sto_tpcc_txn_commit(thread), OK);

            expect(sto_tpcc_txn_begin(thread), OK);
            assert_eq!(get(thread, table, key), Ok(replacement.to_vec()));
            expect(sto_tpcc_txn_commit(thread), OK);
        }

        // A wider fixed mutation must observe a scalar write already staged in
        // the same transaction; Payment's fused customer update can otherwise
        // violate the backend's read-your-writes contract.
        context = FixedMutationContext {
            steps: vec![FixedMutationStep::Put(b"after-staged".to_vec())],
            observed: Vec::new(),
        };
        visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, table, &key12, b"staged"), OK);
        expect(
            sto_tpcc_modify_fixed(
                thread,
                table,
                key12.as_ptr(),
                1,
                12,
                Some(apply_fixed_mutation),
                (&mut context as *mut FixedMutationContext).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 1);
        assert_eq!(context.observed, [Some(b"staged".to_vec())]);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, table, &key12), Ok(b"after-staged".to_vec()));
        expect(sto_tpcc_txn_commit(thread), OK);

        // The first mutation and an unrelated pending insertion are both
        // rolled back when the second callback reports failure.
        context = FixedMutationContext {
            steps: vec![FixedMutationStep::Keep, FixedMutationStep::Fail],
            observed: Vec::new(),
        };
        visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, table, b"pending", b"value"), OK);
        assert_eq!(
            sto_tpcc_modify_fixed(
                thread,
                table,
                repeated.as_ptr().cast(),
                2,
                4,
                Some(apply_fixed_mutation),
                (&mut context as *mut FixedMutationContext).cast(),
                &mut visited,
            ),
            FATAL
        );
        assert_eq!(visited, 2);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, table, &key4_a), Ok(b"second".to_vec()));
        assert_eq!(get(thread, table, b"pending"), Err(MISS));
        assert_eq!(get(thread, table, &key8), Ok(b"eight".to_vec()));
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(table), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}

#[test]
fn fixed_put_packs_variable_values_and_reports_sequential_duplicates() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let mut table = ptr::null_mut();
        expect(sto_tpcc_table_create(db, ptr::null(), &mut table), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let keys4 = [[1; 4], [2; 4]];
        let keys8 = [[3; 8], [4; 8]];
        let keys12 = [[5; 12], [6; 12]];
        let keys16 = [[7; 16], [8; 16]];
        expect(sto_tpcc_txn_begin(thread), OK);
        for (status, result) in [
            put_fixed(
                thread,
                table,
                &keys4,
                &[b"four", b""],
                STO_TPCC_FIXED_PUT_UPSERT,
            ),
            put_fixed(
                thread,
                table,
                &keys8,
                &[b"eight-a", b"eight-b"],
                STO_TPCC_FIXED_PUT_UPSERT,
            ),
            put_fixed(
                thread,
                table,
                &keys12,
                &[b"twelve-a", b"twelve-b"],
                STO_TPCC_FIXED_PUT_UPSERT,
            ),
            put_fixed(
                thread,
                table,
                &keys16,
                &[b"sixteen-a", b"sixteen-b"],
                STO_TPCC_FIXED_PUT_UPSERT,
            ),
        ] {
            expect(status, OK);
            assert_eq!(result.inserted, 2);
            assert_eq!(result.first_duplicate, usize::MAX);
        }
        expect(sto_tpcc_txn_commit(thread), OK);

        let duplicate = [9; 4];
        let insert_keys = [duplicate, duplicate, keys4[0], [10; 4]];
        expect(sto_tpcc_txn_begin(thread), OK);
        let (status, result) = put_fixed(
            thread,
            table,
            &insert_keys,
            &[b"first", b"ignored", b"existing", b"last"],
            STO_TPCC_FIXED_PUT_INSERT,
        );
        expect(status, DUPLICATE);
        assert_eq!(result.inserted, 2);
        assert_eq!(result.first_duplicate, 1);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, table, &duplicate), Ok(b"first".to_vec()));
        assert_eq!(get(thread, table, &keys4[0]), Ok(b"four".to_vec()));
        expect(sto_tpcc_txn_commit(thread), OK);

        // Aborted values disappear logically; the retry reuses the physical
        // tombstones created by pre-interning the exactly unique misses.
        let retry_keys = [[11; 16], [12; 16]];
        expect(sto_tpcc_txn_begin(thread), OK);
        let (status, result) = put_fixed(
            thread,
            table,
            &retry_keys,
            &[b"aborted-a", b"aborted-b"],
            STO_TPCC_FIXED_PUT_INSERT,
        );
        expect(status, OK);
        assert_eq!(result.inserted, 2);
        expect(sto_tpcc_txn_abort(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        let (status, result) = put_fixed(
            thread,
            table,
            &retry_keys,
            &[b"retry-a", b"retry-b"],
            STO_TPCC_FIXED_PUT_INSERT,
        );
        expect(status, OK);
        assert_eq!(result.inserted, 2);
        expect(sto_tpcc_txn_commit(thread), OK);

        let mut rows = u64::MAX;
        expect(sto_tpcc_table_size(table, &mut rows), OK);
        assert_eq!(rows, 12);

        // Invalid packed metadata is rejected before touching the active
        // transaction or caller output.
        let bad_values = [
            StoTpccFixedValue {
                data: ptr::null(),
                length: 1,
            },
            StoTpccFixedValue {
                data: ptr::null(),
                length: 0,
            },
        ];
        let mut output = StoTpccFixedPutResult {
            inserted: 123,
            first_duplicate: 456,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            sto_tpcc_put_fixed(
                thread,
                table,
                keys4.as_ptr().cast(),
                keys4.len(),
                4,
                bad_values.as_ptr(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut output,
            ),
            FATAL
        );
        assert_eq!(output.inserted, 123);
        assert_eq!(output.first_duplicate, 456);
        expect(sto_tpcc_txn_abort(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(table), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}

#[test]
fn heterogeneous_insert_many_preserves_order_duplicates_and_validation_atomicity() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let mut first = ptr::null_mut();
        let mut second = ptr::null_mut();
        expect(sto_tpcc_table_create(db, ptr::null(), &mut first), OK);
        expect(sto_tpcc_table_create(db, ptr::null(), &mut second), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, second, b"existing", b"original"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        let operations = [
            StoTpccInsertOperation {
                table: first,
                key: b"same".as_ptr(),
                key_length: 4,
                value: b"first".as_ptr(),
                value_length: 5,
            },
            StoTpccInsertOperation {
                table: first,
                key: b"same".as_ptr(),
                key_length: 4,
                value: b"ignored-staged-duplicate".as_ptr(),
                value_length: 24,
            },
            StoTpccInsertOperation {
                table: second,
                key: b"existing".as_ptr(),
                key_length: 8,
                value: b"ignored-existing-duplicate".as_ptr(),
                value_length: 26,
            },
            StoTpccInsertOperation {
                table: second,
                key: b"later".as_ptr(),
                key_length: 5,
                value: b"last".as_ptr(),
                value_length: 4,
            },
        ];
        let mut result = StoTpccFixedPutResult {
            inserted: usize::MAX,
            first_duplicate: usize::MAX,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_insert_many(thread, operations.as_ptr(), operations.len(), &mut result),
            DUPLICATE,
        );
        assert_eq!(result.inserted, 2);
        assert_eq!(result.first_duplicate, 1);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, first, b"same"), Ok(b"first".to_vec()));
        assert_eq!(get(thread, second, b"existing"), Ok(b"original".to_vec()));
        assert_eq!(get(thread, second, b"later"), Ok(b"last".to_vec()));
        expect(sto_tpcc_txn_commit(thread), OK);

        // Every descriptor is validated before the first logical write. A bad
        // later descriptor therefore leaves both the output and transaction
        // contents untouched.
        let invalid = [
            StoTpccInsertOperation {
                table: first,
                key: b"must-not-stick".as_ptr(),
                key_length: 14,
                value: b"value".as_ptr(),
                value_length: 5,
            },
            StoTpccInsertOperation {
                table: second,
                key: b"bad".as_ptr(),
                key_length: 3,
                value: ptr::null(),
                value_length: 1,
            },
        ];
        result = StoTpccFixedPutResult {
            inserted: 123,
            first_duplicate: 456,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            sto_tpcc_insert_many(thread, invalid.as_ptr(), invalid.len(), &mut result),
            FATAL
        );
        assert_eq!(result.inserted, 123);
        assert_eq!(result.first_duplicate, 456);
        expect(sto_tpcc_txn_abort(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, first, b"must-not-stick"), Err(MISS));
        result = StoTpccFixedPutResult {
            inserted: usize::MAX,
            first_duplicate: 0,
        };
        expect(
            sto_tpcc_insert_many(thread, ptr::null(), 0, &mut result),
            OK,
        );
        assert_eq!(result.inserted, 0);
        assert_eq!(result.first_duplicate, usize::MAX);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(second), OK);
        expect(sto_tpcc_table_destroy(first), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
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

        // Exercise the worker-local resolved-record cache on a live get->put
        // and prove exact table/key matching prevents capability aliasing.
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, first, b"a"), Ok(b"one".to_vec()));
        expect(put(thread, first, b"a", b"one"), OK);
        assert_eq!(get(thread, first, b"same"), Err(MISS));
        expect(put(thread, second, b"same", b"second-same"), OK);
        assert_eq!(get(thread, first, b"prefix\0"), Err(MISS));
        expect(put(thread, first, b"prefix\0x", b"longer-key"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, first, b"same"), Err(MISS));
        assert_eq!(get(thread, second, b"same"), Ok(b"second-same".to_vec()));
        assert_eq!(get(thread, first, b"prefix\0"), Err(MISS));
        assert_eq!(get(thread, first, b"prefix\0x"), Ok(b"longer-key".to_vec()));
        expect(sto_tpcc_remove(thread, second, b"same".as_ptr(), 4), OK);
        expect(sto_tpcc_remove(thread, first, b"prefix\0x".as_ptr(), 8), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

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

        // Only a row actually delivered to a stopping callback becomes a
        // reusable resolved-cache entry. The delivered row can be updated in
        // the same transaction without changing callback accounting.
        rows.clear();
        visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_scan(
                thread,
                first,
                0,
                0,
                ptr::null(),
                0,
                0,
                ptr::null(),
                0,
                10,
                Some(collect_one_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 1);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].0, b"a");
        expect(put(thread, first, b"a", b"changed"), OK);
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

#[test]
fn borrowed_point_and_scan_bytes_preserve_ffi_results_and_read_your_writes() {
    unsafe {
        let inline = &b"inline\0value"[..];
        let staged_inline = &b"staged\0inline"[..];
        let large: Vec<u8> = (0..257)
            .map(|index| ((index * 31 + 7) % 251) as u8)
            .collect();
        let staged_large: Vec<u8> = (0..513)
            .map(|index| ((index * 17 + 11) % 253) as u8)
            .collect();

        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let mut table = ptr::null_mut();
        expect(sto_tpcc_table_create(db, ptr::null(), &mut table), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, table, b"a-inline", inline), OK);
        expect(put(thread, table, b"b-large", &large), OK);
        expect(put(thread, table, b"c-live", b"committed"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        // Committed inline and shared values copy directly into the caller's
        // final buffer. BufferTooSmall reports the exact size without touching
        // the undersized output; its retry uses the resolved-record cache.
        expect(sto_tpcc_txn_begin(thread), OK);
        let (status, actual, inline_bytes) =
            get_with_buffer(thread, table, b"a-inline", inline.len(), 0xa5);
        assert_eq!(status, OK);
        assert_eq!(actual, inline.len());
        assert_eq!(inline_bytes, inline);

        let (status, actual, untouched) =
            get_with_buffer(thread, table, b"a-inline", inline.len() - 1, 0x9d);
        assert_eq!(status, BUFFER_TOO_SMALL);
        assert_eq!(actual, inline.len());
        assert_eq!(untouched, vec![0x9d; inline.len() - 1]);

        let (status, actual, untouched) = get_with_buffer(thread, table, b"b-large", 13, 0xa5);
        assert_eq!(status, BUFFER_TOO_SMALL);
        assert_eq!(actual, large.len());
        assert_eq!(untouched, vec![0xa5; 13]);
        let (status, actual, large_bytes) =
            get_with_buffer(thread, table, b"b-large", large.len(), 0);
        assert_eq!(status, OK);
        assert_eq!(actual, large.len());
        assert_eq!(large_bytes, large);

        // A logical miss exposes no value bytes, reports length zero, and is
        // repeatable through the stable cached tombstone token.
        let (status, actual, untouched) = get_with_buffer(thread, table, b"d-missing", 7, 0x3c);
        assert_eq!(status, MISS);
        assert_eq!(actual, 0);
        assert_eq!(untouched, vec![0x3c; 7]);
        assert_eq!(
            get_with_buffer(thread, table, b"d-missing", 0, 0),
            (MISS, 0, Vec::new())
        );

        // The byte scan sees committed inline and large values and filters the
        // physical tombstone interned by the preceding point miss.
        let mut rows = Vec::<(Vec<u8>, Vec<u8>)>::new();
        let mut visited = usize::MAX;
        expect(
            sto_tpcc_scan(
                thread,
                table,
                0,
                0,
                ptr::null(),
                0,
                0,
                ptr::null(),
                0,
                16,
                Some(collect_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 3);
        assert_eq!(
            rows,
            [
                (b"a-inline".to_vec(), inline.to_vec()),
                (b"b-large".to_vec(), large.clone()),
                (b"c-live".to_vec(), b"committed".to_vec()),
            ]
        );
        expect(sto_tpcc_txn_abort(thread), OK);

        // Staged inline/shared updates, a tombstone, and new values are all
        // visible through point and scan visitors before commit.
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, table, b"a-inline", staged_inline), OK);
        let (status, actual, bytes) =
            get_with_buffer(thread, table, b"a-inline", staged_inline.len(), 0);
        assert_eq!((status, actual), (OK, staged_inline.len()));
        assert_eq!(bytes, staged_inline);

        expect(put(thread, table, b"b-large", &staged_large), OK);
        let (status, actual, untouched) = get_with_buffer(thread, table, b"b-large", 1, 0x7e);
        assert_eq!((status, actual), (BUFFER_TOO_SMALL, staged_large.len()));
        assert_eq!(untouched, [0x7e]);
        let (status, actual, bytes) =
            get_with_buffer(thread, table, b"b-large", staged_large.len(), 0);
        assert_eq!((status, actual), (OK, staged_large.len()));
        assert_eq!(bytes, staged_large);

        expect(sto_tpcc_remove(thread, table, b"c-live".as_ptr(), 6), OK);
        assert_eq!(
            get_with_buffer(thread, table, b"c-live", 0, 0),
            (MISS, 0, Vec::new())
        );
        expect(put(thread, table, b"d-missing", b"revived"), OK);
        assert_eq!(get(thread, table, b"d-missing"), Ok(b"revived".to_vec()));
        expect(put(thread, table, b"e-new", b"new"), OK);

        rows.clear();
        visited = usize::MAX;
        expect(
            sto_tpcc_scan(
                thread,
                table,
                0,
                0,
                ptr::null(),
                0,
                0,
                ptr::null(),
                0,
                16,
                Some(collect_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 4);
        assert_eq!(
            rows,
            [
                (b"a-inline".to_vec(), staged_inline.to_vec()),
                (b"b-large".to_vec(), staged_large.clone()),
                (b"d-missing".to_vec(), b"revived".to_vec()),
                (b"e-new".to_vec(), b"new".to_vec()),
            ]
        );
        expect(sto_tpcc_txn_abort(thread), OK);

        // Aborting the staged attempt leaves the original committed bytes and
        // tombstone visibility intact.
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(get(thread, table, b"a-inline"), Ok(inline.to_vec()));
        let (status, actual, bytes) = get_with_buffer(thread, table, b"b-large", large.len(), 0);
        assert_eq!((status, actual), (OK, large.len()));
        assert_eq!(bytes, large);
        assert_eq!(get(thread, table, b"c-live"), Ok(b"committed".to_vec()));
        assert_eq!(get(thread, table, b"d-missing"), Err(MISS));
        assert_eq!(get(thread, table, b"e-new"), Err(MISS));
        expect(sto_tpcc_txn_abort(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(table), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}

#[test]
fn streaming_scan_counts_rows_delivered_before_a_later_error_and_reuses_thread_scratch() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);
        let config = StoTpccTableConfig {
            max_retained_records: 64,
            max_retained_key_bytes: 1 << 12,
            max_consumed_record_ids: 64,
            scan_chunk_records: 1,
            max_scan_chunks: 8,
            max_scan_physical_records: 2,
            ..StoTpccTableConfig::default()
        };
        let mut table = ptr::null_mut();
        expect(sto_tpcc_table_create(db, &config, &mut table), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        for (key, value) in [(b"a", b"A"), (b"b", b"B"), (b"c", b"C")] {
            expect(put(thread, table, key, value), OK);
        }
        expect(sto_tpcc_txn_commit(thread), OK);
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(sto_tpcc_remove(thread, table, b"b".as_ptr(), 1), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        let mut rows = Vec::<(Vec<u8>, Vec<u8>)>::new();
        let mut visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            sto_tpcc_scan(
                thread,
                table,
                0,
                0,
                ptr::null(),
                0,
                0,
                ptr::null(),
                0,
                8,
                Some(collect_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            FATAL
        );
        assert_eq!(visited, 1);
        assert_eq!(rows, [(b"a".to_vec(), b"A".to_vec())]);
        expect(sto_tpcc_txn_abort(thread), OK);

        // The failed multi-chunk scan leaves its buffers reusable. A bounded
        // subsequent scan on the same thread succeeds and preserves stop/count
        // semantics without reconstructing the native scratch owner.
        rows.clear();
        visited = usize::MAX;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_scan(
                thread,
                table,
                0,
                0,
                ptr::null(),
                0,
                0,
                ptr::null(),
                0,
                1,
                Some(collect_one_scan),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            OK,
        );
        assert_eq!(visited, 1);
        assert_eq!(rows, [(b"a".to_vec(), b"A".to_vec())]);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(table), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}

#[test]
fn additive_cache_policy_creator_preserves_crud_and_rejects_unknown_values() {
    unsafe {
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(ptr::null(), &mut db), OK);

        let mut compatibility_full = ptr::null_mut();
        expect(
            sto_tpcc_table_create(db, ptr::null(), &mut compatibility_full),
            OK,
        );
        let mut last_only = ptr::null_mut();
        expect(
            sto_tpcc_table_create_with_cache_policy(
                db,
                ptr::null(),
                STO_TPCC_RESOLVED_CACHE_LAST_ONLY,
                &mut last_only,
            ),
            OK,
        );
        let mut read_then_write = ptr::null_mut();
        expect(
            sto_tpcc_table_create_with_cache_policy(
                db,
                ptr::null(),
                STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE,
                &mut read_then_write,
            ),
            OK,
        );
        let mut uncached = ptr::null_mut();
        expect(
            sto_tpcc_table_create_with_cache_policy(
                db,
                ptr::null(),
                STO_TPCC_RESOLVED_CACHE_NONE,
                &mut uncached,
            ),
            OK,
        );
        let mut dense_item = ptr::null_mut();
        expect(
            sto_tpcc_table_create_with_cache_policy(
                db,
                ptr::null(),
                STO_TPCC_RESOLVED_CACHE_DENSE_ITEM,
                &mut dense_item,
            ),
            OK,
        );
        let mut dense_stock = ptr::null_mut();
        expect(
            sto_tpcc_table_create_with_cache_policy(
                db,
                ptr::null(),
                STO_TPCC_RESOLVED_CACHE_DENSE_STOCK,
                &mut dense_stock,
            ),
            OK,
        );

        let mut invalid = compatibility_full;
        assert_eq!(
            sto_tpcc_table_create_with_cache_policy(db, ptr::null(), 77, &mut invalid),
            FATAL
        );
        assert!(invalid.is_null());
        assert!(last_error().contains("invalid resolved-cache policy 77"));

        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(put(thread, compatibility_full, b"full", b"value"), OK);
        expect(put(thread, last_only, b"last", b"value"), OK);
        expect(put(thread, read_then_write, b"handoff", b"value"), OK);
        expect(put(thread, uncached, b"none", b"value"), OK);
        expect(put(thread, dense_item, b"dense-item", b"value"), OK);
        expect(put(thread, dense_stock, b"dense-stock", b"value"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            get(thread, compatibility_full, b"full"),
            Ok(b"value".to_vec())
        );
        assert_eq!(get(thread, last_only, b"last"), Ok(b"value".to_vec()));
        expect(put(thread, last_only, b"last", b"updated"), OK);
        assert_eq!(
            get(thread, read_then_write, b"handoff"),
            Ok(b"value".to_vec())
        );
        expect(put(thread, read_then_write, b"handoff", b"updated"), OK);
        assert_eq!(get(thread, uncached, b"none"), Ok(b"value".to_vec()));
        assert_eq!(
            get(thread, dense_item, b"dense-item"),
            Ok(b"value".to_vec())
        );
        assert_eq!(
            get(thread, dense_stock, b"dense-stock"),
            Ok(b"value".to_vec())
        );
        expect(put(thread, dense_stock, b"dense-stock", b"updated"), OK);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(dense_stock), OK);
        expect(sto_tpcc_table_destroy(dense_item), OK);
        expect(sto_tpcc_table_destroy(uncached), OK);
        expect(sto_tpcc_table_destroy(read_then_write), OK);
        expect(sto_tpcc_table_destroy(last_only), OK);
        expect(sto_tpcc_table_destroy(compatibility_full), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
