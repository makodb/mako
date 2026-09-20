#![cfg(mtree_native_integration)]

use std::ptr;
use sto_tpcc_ffi::*;

unsafe extern "C" fn collect_scan_row(
    context: *mut std::ffi::c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    // SAFETY: The test supplies a live vector and the scan endpoint lends both
    // byte ranges for exactly this callback invocation.
    let rows = unsafe { &mut *context.cast::<Vec<(Vec<u8>, Vec<u8>)>>() };
    rows.push((
        unsafe { std::slice::from_raw_parts(key, key_length) }.to_vec(),
        unsafe { std::slice::from_raw_parts(value, value_length) }.to_vec(),
    ));
    0
}

#[test]
fn trusted_endpoints_share_public_transaction_semantics() {
    unsafe {
        let mut db = ptr::null_mut();
        assert_eq!(sto_tpcc_db_create(ptr::null(), &mut db), 0);
        let mut table = ptr::null_mut();
        let table_config = StoTpccTableConfig {
            bounded_atomic_values: 1,
            ..StoTpccTableConfig::default()
        };
        assert_eq!(sto_tpcc_table_create(db, &table_config, &mut table), 0);
        let mut payment_table = ptr::null_mut();
        assert_eq!(
            sto_tpcc_table_create_with_cache_policy(
                db,
                &table_config,
                STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE,
                &mut payment_table,
            ),
            0
        );
        let mut thread = ptr::null_mut();
        assert_eq!(sto_tpcc_thread_create(db, &mut thread), 0);

        let key = b"trusted-key";
        let first = b"first-value";
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        assert_eq!(
            mako_sto_tpcc_insert_trusted(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                first.as_ptr(),
                first.len(),
            ),
            0
        );
        let mut output = [0_u8; 64];
        let mut actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                output.as_mut_ptr(),
                output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&output[..actual], first);

        let mut rows = Vec::new();
        let mut visited = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_scan_trusted(
                thread,
                table,
                0,
                1,
                key.as_ptr(),
                key.len(),
                0,
                ptr::null(),
                0,
                8,
                Some(collect_scan_row),
                (&mut rows as *mut Vec<(Vec<u8>, Vec<u8>)>).cast(),
                &mut visited,
            ),
            0
        );
        assert_eq!(visited, 1);
        assert_eq!(rows, [(key.to_vec(), first.to_vec())]);
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        let second = b"second-value";
        assert_eq!(sto_tpcc_txn_begin(thread), 0);
        assert_eq!(
            mako_sto_tpcc_put_trusted(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                second.as_ptr(),
                second.len(),
            ),
            0
        );
        assert_eq!(sto_tpcc_txn_commit(thread), 0);

        assert_eq!(sto_tpcc_txn_begin(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            sto_tpcc_get(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                output.as_mut_ptr(),
                output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&output[..actual], second);
        assert_eq!(sto_tpcc_txn_commit(thread), 0);

        let borrowed_key = b"borrowed-bounded-key";
        let mut borrowed = (0..159)
            .map(|index| (index as u8).wrapping_mul(29).wrapping_add(11))
            .collect::<Vec<_>>();
        let expected_borrowed = borrowed.clone();
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        assert_eq!(
            mako_sto_tpcc_insert_borrowed_trusted(
                thread,
                table,
                borrowed_key.as_ptr(),
                borrowed_key.len(),
                borrowed.as_ptr(),
                borrowed.len(),
            ),
            0
        );
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                table,
                borrowed_key.as_ptr(),
                borrowed_key.len(),
                output.as_mut_ptr(),
                output.len(),
                &mut actual,
            ),
            4
        );
        assert_eq!(actual, borrowed.len());
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        borrowed.fill(0xee);
        let mut bounded_output = [0_u8; 160];
        assert_eq!(sto_tpcc_txn_begin(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            sto_tpcc_get(
                thread,
                table,
                borrowed_key.as_ptr(),
                borrowed_key.len(),
                bounded_output.as_mut_ptr(),
                bounded_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&bounded_output[..actual], expected_borrowed);
        assert_eq!(sto_tpcc_txn_commit(thread), 0);

        // The owning INSERT endpoint must tolerate the loader pattern that
        // reuses one encoding buffer several times before commit.
        let reused_key_a = b"owned-insert-a";
        let reused_key_b = b"owned-insert-b";
        let mut reused_value = vec![0x31_u8; 96];
        let expected_reused_a = reused_value.clone();
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        assert_eq!(
            mako_sto_tpcc_insert_trusted(
                thread,
                table,
                reused_key_a.as_ptr(),
                reused_key_a.len(),
                reused_value.as_ptr(),
                reused_value.len(),
            ),
            0
        );
        reused_value.fill(0x72);
        let expected_reused_b = reused_value.clone();
        assert_eq!(
            mako_sto_tpcc_insert_trusted(
                thread,
                table,
                reused_key_b.as_ptr(),
                reused_key_b.len(),
                reused_value.as_ptr(),
                reused_value.len(),
            ),
            0
        );
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                table,
                reused_key_a.as_ptr(),
                reused_key_a.len(),
                bounded_output.as_mut_ptr(),
                bounded_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&bounded_output[..actual], expected_reused_a);
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                table,
                reused_key_b.as_ptr(),
                reused_key_b.len(),
                bounded_output.as_mut_ptr(),
                bounded_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&bounded_output[..actual], expected_reused_b);
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        // Match Payment's hot sequence: an initial GET populates the
        // read-then-write slot, PUT borrows the caller's bounded payload, and
        // a second GET must observe all staged bytes before commit.
        let payment_key = b"payment-shaped-key";
        let payment_initial = vec![0x19_u8; 127];
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        assert_eq!(
            mako_sto_tpcc_insert_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_initial.as_ptr(),
                payment_initial.len(),
            ),
            0
        );
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        let mut payment_output = [0_u8; 160];
        let mut payment_update = (0..159)
            .map(|index| (index as u8).wrapping_mul(17).wrapping_add(3))
            .collect::<Vec<_>>();
        let expected_payment_update = payment_update.clone();
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_output.as_mut_ptr(),
                payment_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&payment_output[..actual], payment_initial);
        assert_eq!(
            mako_sto_tpcc_put_borrowed_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_update.as_ptr(),
                payment_update.len(),
            ),
            0
        );
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_output.as_mut_ptr(),
                payment_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&payment_output[..actual], expected_payment_update);
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        payment_update.fill(0xee);
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_output.as_mut_ptr(),
                payment_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&payment_output[..actual], expected_payment_update);
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        let mut aborted_update = vec![0xa5_u8; 144];
        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_output.as_mut_ptr(),
                payment_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(
            mako_sto_tpcc_put_borrowed_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                aborted_update.as_ptr(),
                aborted_update.len(),
            ),
            0
        );
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_output.as_mut_ptr(),
                payment_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&payment_output[..actual], aborted_update);
        assert_eq!(sto_tpcc_txn_abort(thread), 0);
        aborted_update.fill(0x44);

        assert_eq!(mako_sto_tpcc_txn_begin_trusted(thread), 0);
        actual = usize::MAX;
        assert_eq!(
            mako_sto_tpcc_get_trusted(
                thread,
                payment_table,
                payment_key.as_ptr(),
                payment_key.len(),
                payment_output.as_mut_ptr(),
                payment_output.len(),
                &mut actual,
            ),
            0
        );
        assert_eq!(&payment_output[..actual], expected_payment_update);
        assert_eq!(mako_sto_tpcc_txn_commit_trusted(thread), 0);

        // The checked public ABI remains fail-closed; no trusted precondition
        // is imposed on callers of the installed header.
        actual = usize::MAX;
        assert_eq!(
            sto_tpcc_get(
                ptr::null_mut(),
                table,
                key.as_ptr(),
                key.len(),
                output.as_mut_ptr(),
                output.len(),
                &mut actual,
            ),
            5
        );
        assert_eq!(actual, 0);

        assert_eq!(sto_tpcc_thread_destroy(thread), 0);
        assert_eq!(sto_tpcc_table_destroy(payment_table), 0);
        assert_eq!(sto_tpcc_table_destroy(table), 0);
        assert_eq!(sto_tpcc_db_destroy(db), 0);
    }
}
