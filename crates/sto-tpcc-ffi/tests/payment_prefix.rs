#![cfg(mtree_native_integration)]

use std::{mem, ptr};
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const FATAL: i32 = 5;
const CAPACITY: usize = 164;

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

fn encode_mako_value_i32(value: i32) -> Vec<u8> {
    let mut bytes = encode_i32(value);
    if bytes.len() == 1 {
        bytes.push(0);
    }
    bytes
}

fn inline(bytes: &mut Vec<u8>, value: &[u8], maximum: usize) {
    assert!(value.len() <= maximum);
    bytes.push(value.len() as u8);
    bytes.extend_from_slice(value);
    bytes.resize(bytes.len() + maximum + 1 - value.len(), 0);
}

struct CustomerFixture {
    bytes: Vec<u8>,
    balance: usize,
    ytd_payment: usize,
    payment_count_start: usize,
    payment_count_end: usize,
}

fn customer_fixture(payment_count: i32) -> CustomerFixture {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&0.125_f32.to_ne_bytes());
    bytes.extend_from_slice(b"GC");
    inline(&mut bytes, b"BAR", 16);
    inline(&mut bytes, b"ANN", 16);
    bytes.extend_from_slice(&50_000_f32.to_ne_bytes());
    let balance = bytes.len();
    bytes.extend_from_slice(&(-10_f32).to_ne_bytes());
    let ytd_payment = bytes.len();
    bytes.extend_from_slice(&10_f32.to_ne_bytes());
    let payment_count_start = bytes.len();
    bytes.extend_from_slice(&encode_i32(payment_count));
    let payment_count_end = bytes.len();
    bytes.extend_from_slice(&encode_i32(4));
    inline(&mut bytes, b"A1", 20);
    inline(&mut bytes, b"B2", 20);
    inline(&mut bytes, b"NYC", 20);
    bytes.extend_from_slice(b"NY");
    bytes.extend_from_slice(b"123456789");
    bytes.extend_from_slice(b"0123456789012345");
    bytes.extend_from_slice(&encode_u32(123_456));
    bytes.extend_from_slice(b"OE");
    CustomerFixture {
        bytes,
        balance,
        ytd_payment,
        payment_count_start,
        payment_count_end,
    }
}

fn patched_customer(fixture: &CustomerFixture, payment_count: i32, amount: f32) -> Vec<u8> {
    let mut expected = fixture.bytes.clone();
    expected[fixture.balance..fixture.balance + 4]
        .copy_from_slice(&(-10_f32 - amount).to_ne_bytes());
    expected[fixture.ytd_payment..fixture.ytd_payment + 4]
        .copy_from_slice(&(10_f32 + amount).to_ne_bytes());
    expected.splice(
        fixture.payment_count_start..fixture.payment_count_end,
        encode_i32(payment_count + 1),
    );
    expected
}

fn customer_key(prefix: &[u8; 8], customer_id: i32) -> [u8; 12] {
    let mut key = [0_u8; 12];
    key[..8].copy_from_slice(prefix);
    key[8..].copy_from_slice(&customer_id.to_be_bytes());
    key
}

fn name_bounds(prefix: &[u8; 8], marker: u8) -> ([u8; 40], [u8; 40]) {
    let mut lower = [0_u8; 40];
    lower[..8].copy_from_slice(prefix);
    lower[8..24].fill(marker);
    let mut upper = lower;
    upper[24..].fill(0xff);
    (lower, upper)
}

fn name_key(lower: &[u8; 40], rank: u8) -> [u8; 40] {
    let mut key = *lower;
    key[39] = rank;
    key
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
    let mut bytes = [0_u8; CAPACITY];
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
fn payment_prefix_commits_aborts_scans_and_fails_atomically() {
    unsafe {
        assert_eq!(mem::size_of::<MakoStoTpccPaymentPrefixRequest>(), 120);
        assert_eq!(mem::size_of::<MakoStoTpccPaymentPrefixResult>(), 32);

        let db_config = StoTpccDbConfig {
            max_threads: 8,
            max_key_length: 64,
            max_items_per_txn: 128,
            max_locks_per_txn: 256,
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
        let scanned = StoTpccTableConfig {
            max_retained_records: 128,
            max_retained_key_bytes: 8_192,
            max_consumed_record_ids: 256,
            trusted_scan_value_generation: 1,
            ..StoTpccTableConfig::default()
        };
        let mut warehouse = ptr::null_mut();
        let mut district = ptr::null_mut();
        let mut customer = ptr::null_mut();
        let mut customer_name = ptr::null_mut();
        expect(sto_tpcc_table_create(db, &bounded, &mut warehouse), OK);
        expect(sto_tpcc_table_create(db, &bounded, &mut district), OK);
        expect(sto_tpcc_table_create(db, &bounded, &mut customer), OK);
        expect(sto_tpcc_table_create(db, &scanned, &mut customer_name), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let warehouse_key = 1_i32.to_be_bytes();
        let mut district_key = [0_u8; 8];
        district_key[..4].copy_from_slice(&warehouse_key);
        district_key[4..].copy_from_slice(&2_i32.to_be_bytes());
        let customer_prefix = district_key;
        let by_id = 7;
        let by_id_key = customer_key(&customer_prefix, by_id);
        let malformed_id = 8;
        let malformed_key = customer_key(&customer_prefix, malformed_id);
        let fixture = customer_fixture(63);
        let warehouse_initial = [100_f32.to_ne_bytes().as_slice(), b"warehouse"].concat();
        let district_initial = [200_f32.to_ne_bytes().as_slice(), b"district"].concat();

        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, warehouse, &warehouse_key, &warehouse_initial);
        insert(thread, district, &district_key, &district_initial);
        insert(thread, customer, &by_id_key, &fixture.bytes);
        insert(thread, customer, &malformed_key, &[0_u8; 4]);
        let (name_lower, name_upper) = name_bounds(&customer_prefix, b'L');
        for (rank, customer_id) in [11, 12, 13, 14].into_iter().enumerate() {
            let key = name_key(&name_lower, rank as u8 + 1);
            insert(
                thread,
                customer_name,
                &key,
                &encode_mako_value_i32(customer_id),
            );
            let row = customer_fixture(1);
            insert(
                thread,
                customer,
                &customer_key(&customer_prefix, customer_id),
                &row.bytes,
            );
        }
        expect(sto_tpcc_txn_commit(thread), OK);

        let mut warehouse_output = [0_u8; CAPACITY];
        let mut district_output = [0_u8; CAPACITY];
        let mut customer_output = [0_u8; CAPACITY];
        let mut result = MakoStoTpccPaymentPrefixResult {
            warehouse_length: usize::MAX,
            district_length: usize::MAX,
            customer_length: usize::MAX,
            customer_id: -1,
        };
        let mut request = MakoStoTpccPaymentPrefixRequest {
            warehouse_table: warehouse,
            district_table: district,
            customer_table: customer,
            customer_name_table: ptr::null(),
            warehouse_key: warehouse_key.as_ptr(),
            district_key: district_key.as_ptr(),
            customer_key_prefix: customer_prefix.as_ptr(),
            customer_name_lower_key: ptr::null(),
            customer_name_upper_key: ptr::null(),
            customer_id: by_id,
            payment_amount: 5.5,
            customer_by_name: 0,
            warehouse_output: warehouse_output.as_mut_ptr(),
            district_output: district_output.as_mut_ptr(),
            customer_output: customer_output.as_mut_ptr(),
            output_capacity: CAPACITY,
        };

        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_prefix_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.customer_id, by_id);
        assert_eq!(
            &customer_output[..result.customer_length],
            patched_customer(&fixture, 63, 5.5)
        );
        expect(sto_tpcc_txn_commit(thread), OK);
        warehouse_output.fill(0xee);
        district_output.fill(0xee);
        customer_output.fill(0xee);

        expect(sto_tpcc_txn_begin(thread), OK);
        let committed_warehouse = read(thread, warehouse, &warehouse_key);
        let committed_district = read(thread, district, &district_key);
        let committed_customer = read(thread, customer, &by_id_key);
        assert_eq!(
            f32::from_ne_bytes(committed_warehouse[..4].try_into().unwrap()),
            105.5
        );
        assert_eq!(
            f32::from_ne_bytes(committed_district[..4].try_into().unwrap()),
            205.5
        );
        assert_eq!(committed_customer, patched_customer(&fixture, 63, 5.5));
        expect(sto_tpcc_txn_commit(thread), OK);

        request.payment_amount = 2.0;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_prefix_trusted(thread, &request, &mut result),
            OK,
        );
        expect(sto_tpcc_txn_abort(thread), OK);
        warehouse_output.fill(0xdd);
        district_output.fill(0xdd);
        customer_output.fill(0xdd);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, warehouse, &warehouse_key), committed_warehouse);
        assert_eq!(read(thread, district, &district_key), committed_district);
        assert_eq!(read(thread, customer, &by_id_key), committed_customer);
        expect(sto_tpcc_txn_commit(thread), OK);

        request.customer_by_name = 1;
        request.customer_name_table = customer_name;
        request.customer_name_lower_key = name_lower.as_ptr();
        request.customer_name_upper_key = name_upper.as_ptr();
        request.customer_id = 0;
        result = MakoStoTpccPaymentPrefixResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_prefix_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.customer_id, 12);
        expect(sto_tpcc_txn_abort(thread), OK);

        request.customer_by_name = 0;
        request.customer_name_table = ptr::null();
        request.customer_name_lower_key = ptr::null();
        request.customer_name_upper_key = ptr::null();
        request.customer_id = 999;
        result = MakoStoTpccPaymentPrefixResult {
            warehouse_length: 1,
            district_length: 2,
            customer_length: 3,
            customer_id: 4,
        };
        let failure_sentinel = result;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_prefix_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert_eq!(result, failure_sentinel);
        expect(sto_tpcc_txn_commit(thread), FATAL);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, warehouse, &warehouse_key), committed_warehouse);
        expect(sto_tpcc_txn_commit(thread), OK);

        request.customer_id = malformed_id;
        result = failure_sentinel;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_prefix_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert_eq!(result, failure_sentinel);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, district, &district_key), committed_district);
        expect(sto_tpcc_txn_commit(thread), OK);

        request.customer_id = by_id;
        request.district_output = request.warehouse_output.add(CAPACITY - 1);
        result = failure_sentinel;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_payment_prefix_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert_eq!(result, failure_sentinel);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, warehouse, &warehouse_key), committed_warehouse);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(customer_name), OK);
        expect(sto_tpcc_table_destroy(customer), OK);
        expect(sto_tpcc_table_destroy(district), OK);
        expect(sto_tpcc_table_destroy(warehouse), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
