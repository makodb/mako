#![cfg(mtree_native_integration)]

use std::ptr;

use sto_tpcc_ffi::*;

const OK: i32 = 0;
const RETRY: i32 = 3;
const FATAL: i32 = 5;

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

fn key2(first: i32, second: i32) -> [u8; 8] {
    let mut key = [0_u8; 8];
    key[..4].copy_from_slice(&first.to_be_bytes());
    key[4..].copy_from_slice(&second.to_be_bytes());
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

fn order_line_value(item_id: i32) -> Vec<u8> {
    let mut bytes = encode_i32(item_id);
    bytes.extend_from_slice(&encode_u32(0));
    bytes.extend_from_slice(&1_f32.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(1));
    bytes.push(1);
    bytes
}

fn stock_value(quantity: i16) -> Vec<u8> {
    let mut bytes = quantity.to_ne_bytes().to_vec();
    bytes.extend_from_slice(&0_f32.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(0));
    bytes.extend_from_slice(&encode_i32(0));
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

#[test]
fn full_stock_level_matches_scalar_scan_dedup_threshold_and_failure_semantics() {
    unsafe {
        let db_config = StoTpccDbConfig {
            max_threads: 4,
            max_key_length: 64,
            max_items_per_txn: 1_024,
            max_locks_per_txn: 1_024,
        };
        let table_config = StoTpccTableConfig {
            max_retained_records: 1_024,
            max_retained_key_bytes: 64 * 1_024,
            max_consumed_record_ids: 2_048,
            trusted_scan_value_generation: 1,
            bounded_atomic_values: 1,
            ..StoTpccTableConfig::default()
        };
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(&db_config, &mut db), OK);
        let mut order_line = ptr::null_mut();
        let mut stock = ptr::null_mut();
        expect(
            sto_tpcc_table_create(db, &table_config, &mut order_line),
            OK,
        );
        expect(sto_tpcc_table_create(db, &table_config, &mut stock), OK);
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let mut request = MakoStoTpccStockLevelFullRequest {
            order_line_table: order_line,
            stock_table: stock,
            current_next_order_id: 0,
            warehouse_id: 1,
            district_id: 1,
            threshold: 10,
        };

        // Equal lower/upper bounds produce the scalar empty result and the
        // fused function owns the commit.
        let mut result = MakoStoTpccStockLevelFullResult {
            reported_value_bytes: usize::MAX,
            scanned_order_line_rows: u32::MAX,
            distinct_item_ids: u32::MAX,
            low_stock_count: u32::MAX,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_stock_level_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result, MakoStoTpccStockLevelFullResult::default());
        expect(sto_tpcc_txn_commit(thread), FATAL);

        // current_next_order_id < 20 clamps the lower order to zero. An
        // order-zero row is therefore included. Quantity equal to threshold
        // is not low stock because the scalar predicate is strictly less.
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, order_line, &key4(1, 1, 0, 1), &order_line_value(1));
        insert(thread, stock, &key2(1, 1), &stock_value(10));
        expect(sto_tpcc_txn_commit(thread), OK);
        request.current_next_order_id = 1;
        result = MakoStoTpccStockLevelFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_stock_level_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(
            result,
            MakoStoTpccStockLevelFullResult {
                reported_value_bytes: 0,
                scanned_order_line_rows: 1,
                distinct_item_ids: 1,
                low_stock_count: 0,
            }
        );

        // Two additional rows contain one duplicate and one new item. The
        // scan reports all rows, probes each distinct stock key once, and
        // counts the quantity below the threshold.
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, order_line, &key4(1, 1, 1, 1), &order_line_value(1));
        insert(thread, order_line, &key4(1, 1, 1, 2), &order_line_value(2));
        insert(thread, stock, &key2(1, 2), &stock_value(9));
        expect(sto_tpcc_txn_commit(thread), OK);
        request.current_next_order_id = 2;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_stock_level_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(
            result,
            MakoStoTpccStockLevelFullResult {
                reported_value_bytes: 0,
                scanned_order_line_rows: 3,
                distinct_item_ids: 2,
                low_stock_count: 1,
            }
        );

        // Fill exactly twenty 15-line orders in a separate district. This is
        // the scalar scan limit and the fixed set's maximum distinct input.
        expect(sto_tpcc_txn_begin(thread), OK);
        for item_index in 1_i32..=300 {
            let item_id = 1_000 + item_index;
            let order_id = (item_index - 1) / 15 + 1;
            let line_number = (item_index - 1) % 15 + 1;
            insert(
                thread,
                order_line,
                &key4(1, 2, order_id, line_number),
                &order_line_value(item_id),
            );
        }
        expect(sto_tpcc_txn_commit(thread), OK);
        expect(sto_tpcc_txn_begin(thread), OK);
        for item_index in 1_i32..=300 {
            let item_id = 1_000 + item_index;
            let quantity = if item_index <= 150 { 19 } else { 20 };
            insert(thread, stock, &key2(1, item_id), &stock_value(quantity));
        }
        expect(sto_tpcc_txn_commit(thread), OK);

        request.current_next_order_id = 21;
        request.district_id = 2;
        request.threshold = 20;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_stock_level_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(
            result,
            MakoStoTpccStockLevelFullResult {
                reported_value_bytes: 0,
                scanned_order_line_rows: 300,
                distinct_item_ids: 300,
                low_stock_count: 150,
            }
        );

        // A missing stock row maps to the scalar transaction retry. The guard
        // consumes the attempt and leaves caller result bytes unchanged.
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(
            thread,
            order_line,
            &key4(1, 3, 7, 1),
            &order_line_value(999),
        );
        expect(sto_tpcc_txn_commit(thread), OK);
        request.current_next_order_id = 8;
        request.district_id = 3;
        request.threshold = 15;
        result = MakoStoTpccStockLevelFullResult {
            reported_value_bytes: 91,
            scanned_order_line_rows: 92,
            distinct_item_ids: 93,
            low_stock_count: 94,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_stock_level_full_trusted(thread, &request, &mut result),
            RETRY,
        );
        assert_eq!(
            result,
            MakoStoTpccStockLevelFullResult {
                reported_value_bytes: 91,
                scanned_order_line_rows: 92,
                distinct_item_ids: 93,
                low_stock_count: 94,
            }
        );
        expect(sto_tpcc_txn_commit(thread), FATAL);

        // The same handle starts a clean attempt after the fused failure.
        request.current_next_order_id = 0;
        request.district_id = 4;
        result = MakoStoTpccStockLevelFullResult::default();
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_stock_level_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result, MakoStoTpccStockLevelFullResult::default());

        expect(sto_tpcc_thread_destroy(thread), OK);
        expect(sto_tpcc_table_destroy(stock), OK);
        expect(sto_tpcc_table_destroy(order_line), OK);
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
