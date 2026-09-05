#![cfg(mtree_native_integration)]

use std::ptr;
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const FATAL: i32 = 5;
const READ_CAPACITY: usize = 512;

fn last_error() -> String {
    let length = sto_tpcc_last_error_length();
    let mut bytes = vec![0_i8; length + 1];
    let mut actual = usize::MAX;
    assert_eq!(
        unsafe { sto_tpcc_last_error_copy(bytes.as_mut_ptr(), bytes.len(), &mut actual) },
        OK
    );
    assert_eq!(actual, length);
    String::from_utf8(bytes[..length].iter().map(|byte| *byte as u8).collect()).unwrap()
}

fn expect(actual: i32, expected: i32) {
    assert_eq!(actual, expected, "FFI error: {}", last_error());
}

fn encode_u32(mut value: u32) -> Vec<u8> {
    let mut bytes = Vec::new();
    while value > 0x7f {
        bytes.push((value as u8 & 0x7f) | 0x80);
        value >>= 7;
    }
    bytes.push(value as u8);
    bytes
}

fn encode_i32(value: i32) -> Vec<u8> {
    encode_u32(((value as u32) << 1) ^ ((value >> 31) as u32))
}

fn inline_u8(bytes: &mut Vec<u8>, value: &[u8], maximum: usize) {
    assert!(value.len() <= maximum);
    bytes.push(value.len() as u8);
    bytes.extend_from_slice(value);
    bytes.resize(bytes.len() + maximum + 1 - value.len(), 0);
}

fn warehouse_value(name: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&100_f32.to_ne_bytes());
    bytes.extend_from_slice(&0.1_f32.to_ne_bytes());
    inline_u8(&mut bytes, name, 10);
    inline_u8(&mut bytes, b"STREET1", 20);
    inline_u8(&mut bytes, b"STREET2", 20);
    inline_u8(&mut bytes, b"CITY", 20);
    bytes.extend_from_slice(b"NY");
    bytes.extend_from_slice(b"123456789");
    bytes
}

fn district_value(name: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&200_f32.to_ne_bytes());
    bytes.extend_from_slice(&0.2_f32.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(3_001));
    inline_u8(&mut bytes, name, 10);
    inline_u8(&mut bytes, b"STREET1", 20);
    inline_u8(&mut bytes, b"STREET2", 20);
    inline_u8(&mut bytes, b"CITY", 20);
    bytes.extend_from_slice(b"NY");
    bytes.extend_from_slice(b"123456789");
    bytes
}

fn customer_value(credit: &[u8; 2]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&0.125_f32.to_ne_bytes());
    bytes.extend_from_slice(credit);
    inline_u8(&mut bytes, b"BAR", 16);
    inline_u8(&mut bytes, b"ANN", 16);
    bytes.extend_from_slice(&50_000_f32.to_ne_bytes());
    bytes.extend_from_slice(&(-10_f32).to_ne_bytes());
    bytes.extend_from_slice(&10_f32.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(1));
    bytes.extend_from_slice(&encode_i32(0));
    inline_u8(&mut bytes, b"A1", 20);
    inline_u8(&mut bytes, b"B2", 20);
    inline_u8(&mut bytes, b"NYC", 20);
    bytes.extend_from_slice(b"NY");
    bytes.extend_from_slice(b"123456789");
    bytes.extend_from_slice(b"0123456789012345");
    bytes.extend_from_slice(&encode_u32(123_456));
    bytes.extend_from_slice(b"OE");
    bytes
}

fn customer_data_value(value: &[u8]) -> [u8; 303] {
    assert!(value.len() <= 300);
    let mut bytes = [0_u8; 303];
    bytes[..2].copy_from_slice(&(value.len() as u16).to_ne_bytes());
    bytes[2..2 + value.len()].copy_from_slice(value);
    bytes
}

fn customer_key(customer_id: i32, district_id: i32) -> [u8; 12] {
    let mut key = [0_u8; 12];
    key[..4].copy_from_slice(&1_i32.to_be_bytes());
    key[4..8].copy_from_slice(&district_id.to_be_bytes());
    key[8..].copy_from_slice(&customer_id.to_be_bytes());
    key
}

fn history_key(customer_id: i32, timestamp: u32) -> [u8; 24] {
    let mut key = [0_u8; 24];
    // Preserve txn_payment's established constructor argument order, which is
    // intentionally different from the history field declaration order.
    for (index, field) in [2_i32, 1, customer_id, 2, 1].into_iter().enumerate() {
        key[index * 4..index * 4 + 4].copy_from_slice(&field.to_be_bytes());
    }
    key[20..].copy_from_slice(&timestamp.to_be_bytes());
    key
}

fn history_value(amount: f32) -> [u8; 30] {
    let data = b"WAREHOUSE1    DISTRICT";
    let mut bytes = [0_u8; 30];
    bytes[..4].copy_from_slice(&amount.to_ne_bytes());
    bytes[4] = data.len() as u8;
    bytes[5..5 + data.len()].copy_from_slice(data);
    bytes
}

unsafe fn insert(thread: *mut StoTpccThread, table: *mut StoTpccTable, key: &[u8], value: &[u8]) {
    expect(
        unsafe {
            sto_tpcc_insert(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
            )
        },
        OK,
    );
}

unsafe fn read(thread: *mut StoTpccThread, table: *mut StoTpccTable, key: &[u8]) -> Vec<u8> {
    let mut bytes = [0_u8; READ_CAPACITY];
    let mut actual = usize::MAX;
    expect(
        unsafe {
            sto_tpcc_get(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                bytes.as_mut_ptr(),
                bytes.len(),
                &mut actual,
            )
        },
        OK,
    );
    bytes[..actual].to_vec()
}

#[test]
fn full_payment_commits_gc_bc_and_duplicate_history_as_a_noop() {
    unsafe {
        let db_config = StoTpccDbConfig {
            max_threads: 4,
            max_key_length: 64,
            max_items_per_txn: 64,
            max_locks_per_txn: 128,
        };
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(&db_config, &mut db), OK);
        let bounded = StoTpccTableConfig {
            max_retained_records: 128,
            max_retained_key_bytes: 8_192,
            max_consumed_record_ids: 256,
            bounded_atomic_values: 1,
            ..StoTpccTableConfig::default()
        };
        let ordinary = StoTpccTableConfig {
            max_retained_records: 128,
            max_retained_key_bytes: 8_192,
            max_consumed_record_ids: 256,
            ..StoTpccTableConfig::default()
        };
        let mut warehouse = ptr::null_mut();
        let mut district = ptr::null_mut();
        let mut customer = ptr::null_mut();
        let mut history = ptr::null_mut();
        expect(sto_tpcc_table_create(db, &bounded, &mut warehouse), OK);
        expect(sto_tpcc_table_create(db, &bounded, &mut district), OK);
        expect(sto_tpcc_table_create(db, &bounded, &mut customer), OK);
        expect(sto_tpcc_table_create(db, &ordinary, &mut history), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let warehouse_key = 1_i32.to_be_bytes();
        let mut district_key = [0_u8; 8];
        district_key[..4].copy_from_slice(&warehouse_key);
        district_key[4..].copy_from_slice(&2_i32.to_be_bytes());
        let customer_prefix = district_key;
        let gc_key = customer_key(7, 2);
        let bc_key = customer_key(8, 2);
        let bc_data_key = customer_key(8, 102);
        let malformed_bc_key = customer_key(9, 2);
        let malformed_bc_data_key = customer_key(9, 102);
        let mut malformed_customer_data = customer_data_value(b"IGNORED");
        malformed_customer_data[..2].copy_from_slice(&301_u16.to_ne_bytes());
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(
            thread,
            warehouse,
            &warehouse_key,
            &warehouse_value(b"WAREHOUSE1"),
        );
        insert(
            thread,
            district,
            &district_key,
            &district_value(b"DISTRICT"),
        );
        insert(thread, customer, &gc_key, &customer_value(b"GC"));
        insert(thread, customer, &bc_key, &customer_value(b"BC"));
        insert(thread, customer, &malformed_bc_key, &customer_value(b"BC"));
        insert(
            thread,
            customer,
            &bc_data_key,
            &customer_data_value(b"OLD DATA"),
        );
        insert(
            thread,
            customer,
            &malformed_bc_data_key,
            &malformed_customer_data,
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        let timestamp = 0x0102_0304;
        let mut request = MakoStoTpccPaymentFullRequest {
            warehouse_table: warehouse,
            district_table: district,
            customer_table: customer,
            customer_name_table: ptr::null(),
            history_table: history,
            warehouse_key: warehouse_key.as_ptr(),
            district_key: district_key.as_ptr(),
            customer_key_prefix: customer_prefix.as_ptr(),
            customer_name_lower_key: ptr::null(),
            customer_name_upper_key: ptr::null(),
            customer_id: 7,
            payment_amount: 5.5,
            timestamp,
            warehouse_id: 1,
            district_id: 2,
            customer_warehouse_id: 1,
            customer_district_id: 2,
            customer_by_name: 0,
        };
        let mut result = MakoStoTpccPaymentFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.history_value_length, 30);
        assert_eq!(result.customer_id, 7);
        expect(sto_tpcc_txn_commit(thread), FATAL);

        expect(sto_tpcc_txn_begin(thread), OK);
        let committed_warehouse = read(thread, warehouse, &warehouse_key);
        let committed_district = read(thread, district, &district_key);
        let committed_customer = read(thread, customer, &gc_key);
        assert_eq!(
            f32::from_ne_bytes(committed_warehouse[..4].try_into().unwrap()),
            105.5
        );
        assert_eq!(
            f32::from_ne_bytes(committed_district[..4].try_into().unwrap()),
            205.5
        );
        assert_eq!(
            f32::from_ne_bytes(committed_customer[46..50].try_into().unwrap()),
            -15.5
        );
        assert_eq!(
            f32::from_ne_bytes(committed_customer[50..54].try_into().unwrap()),
            15.5
        );
        assert_eq!(
            read(thread, history, &history_key(7, timestamp)),
            history_value(5.5)
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        // A repeated timestamp collides with the same history key. The legacy
        // tx_insert contract ignores that duplicate and still commits Payment.
        request.payment_amount = 7.0;
        result = MakoStoTpccPaymentFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_full_trusted(thread, &request, &mut result),
            OK,
        );
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            read(thread, history, &history_key(7, timestamp)),
            history_value(5.5)
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        request.customer_id = 8;
        request.payment_amount = 12.5;
        request.timestamp = timestamp + 1;
        result = MakoStoTpccPaymentFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.customer_id, 8);
        expect(sto_tpcc_txn_begin(thread), OK);
        let expected_customer_data = customer_data_value(b"8 102 1 2 1 12 | OLD DATA");
        assert_eq!(read(thread, customer, &bc_data_key), expected_customer_data);
        assert_eq!(
            read(thread, history, &history_key(8, timestamp + 1)),
            history_value(12.5)
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        request.customer_id = 9;
        request.payment_amount = 3.0;
        request.timestamp = timestamp + 2;
        result = MakoStoTpccPaymentFullResult {
            history_value_length: 111,
            customer_id: 222,
        };
        let failure_sentinel = result;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_full_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert_eq!(result, failure_sentinel);

        // History was staged before the malformed BC tail was discovered. The
        // armed guard must abort it, and the same handle must start cleanly.
        expect(sto_tpcc_txn_begin(thread), OK);
        let missing_history_key = history_key(9, timestamp + 2);
        let mut missing_output = [0_u8; 30];
        let mut missing_actual = usize::MAX;
        expect(
            sto_tpcc_get(
                thread,
                history,
                missing_history_key.as_ptr(),
                missing_history_key.len(),
                missing_output.as_mut_ptr(),
                missing_output.len(),
                &mut missing_actual,
            ),
            MISS,
        );
        assert_eq!(
            read(thread, customer, &malformed_bc_key),
            customer_value(b"BC")
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        request.customer_id = 7;
        request.payment_amount = 1.0;
        request.timestamp = timestamp + 3;
        result = MakoStoTpccPaymentFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.customer_id, 7);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            read(thread, history, &history_key(7, timestamp + 3)),
            history_value(1.0)
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(history), OK);
        expect(sto_tpcc_table_destroy(customer), OK);
        expect(sto_tpcc_table_destroy(district), OK);
        expect(sto_tpcc_table_destroy(warehouse), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
