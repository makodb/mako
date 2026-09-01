#![cfg(mtree_native_integration)]

use std::ptr;

use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const RETRY: i32 = 3;
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

fn item_value(price: f32, image_id: i32) -> Vec<u8> {
    let mut bytes = Vec::new();
    inline_u8(&mut bytes, b"ITEM", 24);
    bytes.extend_from_slice(&price.to_ne_bytes());
    inline_u8(&mut bytes, b"DATA", 50);
    bytes.extend_from_slice(&encode_i32(image_id));
    bytes
}

fn stock_value(quantity: i16, ytd: f32, order_count: i32, remote_count: i32) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&quantity.to_ne_bytes());
    bytes.extend_from_slice(&ytd.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(order_count));
    bytes.extend_from_slice(&encode_i32(remote_count));
    bytes
}

fn key2(first: i32, second: i32) -> [u8; 8] {
    let mut key = [0_u8; 8];
    key[..4].copy_from_slice(&first.to_be_bytes());
    key[4..].copy_from_slice(&second.to_be_bytes());
    key
}

fn key3(first: i32, second: i32, third: i32) -> [u8; 12] {
    let mut key = [0_u8; 12];
    key[..4].copy_from_slice(&first.to_be_bytes());
    key[4..8].copy_from_slice(&second.to_be_bytes());
    key[8..].copy_from_slice(&third.to_be_bytes());
    key
}

fn key4(first: i32, second: i32, third: i32, fourth: i32) -> [u8; 16] {
    let mut key = [0_u8; 16];
    key[..4].copy_from_slice(&first.to_be_bytes());
    key[4..8].copy_from_slice(&second.to_be_bytes());
    key[8..12].copy_from_slice(&third.to_be_bytes());
    key[12..].copy_from_slice(&fourth.to_be_bytes());
    key
}

fn oorder_value(customer_id: i32, line_count: u8, entry_date: u32) -> Vec<u8> {
    let mut bytes = encode_i32(customer_id);
    bytes.extend_from_slice(&encode_i32(0));
    bytes.push(line_count);
    bytes.push(1);
    bytes.extend_from_slice(&encode_u32(entry_date));
    bytes
}

fn order_line_value(item_id: i32, amount: f32, quantity: u8) -> Vec<u8> {
    let mut bytes = encode_i32(item_id);
    bytes.extend_from_slice(&encode_u32(0));
    bytes.extend_from_slice(&amount.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(1));
    bytes.push(quantity);
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

unsafe fn read_status(
    thread: *mut StoTpccThread,
    table: *mut StoTpccTable,
    key: &[u8],
) -> (i32, Vec<u8>) {
    let mut bytes = [0_u8; READ_CAPACITY];
    let mut actual = usize::MAX;
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
        (status, bytes[..actual].to_vec())
    } else {
        (status, Vec::new())
    }
}

unsafe fn read(thread: *mut StoTpccThread, table: *mut StoTpccTable, key: &[u8]) -> Vec<u8> {
    let (status, bytes) = unsafe { read_status(thread, table, key) };
    expect(status, OK);
    bytes
}

#[test]
fn full_new_order_commits_exact_bytes_and_rolls_back_collisions() {
    unsafe {
        let db_config = StoTpccDbConfig {
            max_threads: 4,
            max_key_length: 64,
            max_items_per_txn: 128,
            max_locks_per_txn: 256,
        };
        let table_config = StoTpccTableConfig {
            max_retained_records: 512,
            max_retained_key_bytes: 32_768,
            max_consumed_record_ids: 1_024,
            ..StoTpccTableConfig::default()
        };
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(&db_config, &mut db), OK);
        let mut tables = [ptr::null_mut(); 9];
        for table in &mut tables {
            expect(sto_tpcc_table_create(db, &table_config, table), OK);
        }
        let [warehouse, district, customer, item, stock, new_order, oorder, oorder_idx, order_line] =
            tables;
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let warehouse_key = 1_i32.to_be_bytes();
        let district_key = key2(1, 2);
        let customer_key = key3(1, 2, 7);
        expect(sto_tpcc_txn_begin(thread), OK);
        // Fused NewOrder needs only transactional existence for these rows,
        // matching the C++ Release build where their sanity checks disappear.
        // Deliberately malformed payloads ensure neither lookup path decodes
        // or copies them.
        insert(thread, warehouse, &warehouse_key, b"malformed warehouse");
        insert(thread, district, &district_key, b"malformed district");
        insert(thread, customer, &customer_key, b"malformed customer");
        for item_id in 1_i32..=15 {
            let price = item_id as f32 + 1.0;
            let quantity = match item_id {
                3 => 15_i16,
                4 => 9_i16,
                _ => 50_i16,
            };
            insert(
                thread,
                item,
                &item_id.to_be_bytes(),
                &item_value(price, item_id + 100),
            );
            insert(
                thread,
                stock,
                &key2(1, item_id),
                &stock_value(quantity, 0.0, 11, 3),
            );
        }
        expect(sto_tpcc_txn_commit(thread), OK);

        let item_ids = [1_u32, 2, 1, 3, 4];
        let quantities = [5_u32, 6, 7, 8, 9];
        let mut request = MakoStoTpccNewOrderFullRequest {
            warehouse_table: warehouse,
            district_table: district,
            customer_table: customer,
            item_table: item,
            stock_table: stock,
            new_order_table: new_order,
            oorder_table: oorder,
            oorder_c_id_idx_table: oorder_idx,
            order_line_table: order_line,
            item_ids: item_ids.as_ptr(),
            quantities: quantities.as_ptr(),
            warehouse_id: 1,
            district_id: 2,
            customer_id: 7,
            order_id: 3_001,
            entry_date: 123,
            line_count: item_ids.len() as u32,
        };
        let expected_oorder = oorder_value(7, 5, 123);
        let expected_order_lines = [
            order_line_value(1, 10.0, 5),
            order_line_value(2, 18.0, 6),
            order_line_value(1, 14.0, 7),
            order_line_value(3, 32.0, 8),
            order_line_value(4, 45.0, 9),
        ];
        let expected_reported_bytes =
            12 + expected_oorder.len() + expected_order_lines.iter().map(Vec::len).sum::<usize>();
        let mut result = MakoStoTpccNewOrderFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_new_order_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.reported_value_bytes, expected_reported_bytes);
        expect(sto_tpcc_txn_commit(thread), FATAL);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, new_order, &key3(1, 2, 3_001)), [b' '; 12]);
        assert_eq!(read(thread, oorder, &key3(1, 2, 3_001)), expected_oorder);
        assert_eq!(read(thread, oorder_idx, &key4(1, 2, 7, 3_001)), [0, 0]);
        for (index, expected) in expected_order_lines.iter().enumerate() {
            assert_eq!(
                read(thread, order_line, &key4(1, 2, 3_001, index as i32 + 1)),
                *expected
            );
        }
        assert_eq!(
            read(thread, stock, &key2(1, 1)),
            stock_value(38, 12.0, 11, 3)
        );
        assert_eq!(
            read(thread, stock, &key2(1, 2)),
            stock_value(44, 6.0, 11, 3)
        );
        assert_eq!(
            read(thread, stock, &key2(1, 3)),
            stock_value(98, 8.0, 11, 3)
        );
        assert_eq!(
            read(thread, stock, &key2(1, 4)),
            stock_value(91, 9.0, 11, 3)
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        // A duplicate header is an ignored insert, matching tx_insert_many.
        let preexisting_header = *b"PREEXISTING!";
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, new_order, &key3(1, 2, 3_002), &preexisting_header);
        expect(sto_tpcc_txn_commit(thread), OK);
        request.order_id = 3_002;
        result = MakoStoTpccNewOrderFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_new_order_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.reported_value_bytes, expected_reported_bytes);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(
            read(thread, new_order, &key3(1, 2, 3_002)),
            preexisting_header
        );
        assert_eq!(read(thread, oorder, &key3(1, 2, 3_002)), expected_oorder);
        expect(sto_tpcc_txn_commit(thread), OK);

        // Exercise the ABI's maximum fixed arrays and the line-number 15 key.
        let fifteen_item_ids: [u32; 15] = std::array::from_fn(|index| index as u32 + 1);
        let fifteen_quantities = [1_u32; 15];
        request.item_ids = fifteen_item_ids.as_ptr();
        request.quantities = fifteen_quantities.as_ptr();
        request.line_count = 15;
        request.order_id = 3_003;
        result = MakoStoTpccNewOrderFullResult::default();
        let expected_oorder_15 = oorder_value(7, 15, 123);
        let expected_line_15 = order_line_value(15, 16.0, 1);
        let expected_reported_15 = 12
            + expected_oorder_15.len()
            + (1_i32..=15)
                .map(|item_id| order_line_value(item_id, item_id as f32 + 1.0, 1).len())
                .sum::<usize>();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_new_order_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result.reported_value_bytes, expected_reported_15);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, oorder, &key3(1, 2, 3_003)), expected_oorder_15);
        assert_eq!(
            read(thread, order_line, &key4(1, 2, 3_003, 15)),
            expected_line_15
        );
        expect(sto_tpcc_txn_commit(thread), OK);

        // An order-line collision aborts stock changes and every new header.
        request.item_ids = item_ids.as_ptr();
        request.quantities = quantities.as_ptr();
        request.line_count = item_ids.len() as u32;
        let collision_key = key4(1, 2, 3_004, 1);
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, order_line, &collision_key, b"collision");
        let stock_before_retry = read(thread, stock, &key2(1, 1));
        expect(sto_tpcc_txn_commit(thread), OK);
        request.order_id = 3_004;
        result = MakoStoTpccNewOrderFullResult {
            reported_value_bytes: usize::MAX,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_new_order_full_trusted(thread, &request, &mut result),
            RETRY,
        );
        assert_eq!(result.reported_value_bytes, usize::MAX);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, stock, &key2(1, 1)), stock_before_retry);
        assert_eq!(read_status(thread, new_order, &key3(1, 2, 3_004)).0, MISS);
        assert_eq!(read_status(thread, oorder, &key3(1, 2, 3_004)).0, MISS);
        assert_eq!(read(thread, order_line, &collision_key), b"collision");
        expect(sto_tpcc_txn_commit(thread), OK);

        // Invalid input also consumes the attempt and leaves the result alone.
        let invalid_quantities = [0_u32, 6, 7, 8, 9];
        request.quantities = invalid_quantities.as_ptr();
        request.order_id = 3_005;
        result.reported_value_bytes = 77;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_new_order_full_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert_eq!(result.reported_value_bytes, 77);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read_status(thread, new_order, &key3(1, 2, 3_005)).0, MISS);
        expect(sto_tpcc_txn_commit(thread), OK);

        // A cached token for a tombstoned required row remains a missing-row
        // fatal error, rather than weakening the required-presence contract.
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            sto_tpcc_remove(thread, customer, customer_key.as_ptr(), customer_key.len()),
            OK,
        );
        expect(sto_tpcc_txn_commit(thread), OK);
        request.quantities = quantities.as_ptr();
        request.order_id = 3_006;
        result.reported_value_bytes = 91;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_new_order_full_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert!(last_error().contains("new-order customer: required row is missing"));
        assert_eq!(result.reported_value_bytes, 91);
        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read_status(thread, new_order, &key3(1, 2, 3_006)).0, MISS);
        expect(sto_tpcc_txn_commit(thread), OK);

        expect(sto_tpcc_thread_destroy(thread), OK);
        for table in tables.into_iter().rev() {
            expect(sto_tpcc_table_destroy(table), OK);
        }
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
