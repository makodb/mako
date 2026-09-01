#![cfg(mtree_native_integration)]

use std::ptr;

use sto_tpcc_ffi::*;

const OK: i32 = 0;
const MISS: i32 = 1;
const RETRY: i32 = 3;
const FATAL: i32 = 5;
const READ_CAPACITY: usize = 128;

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

fn oorder_value(
    customer_id: i32,
    carrier_id: i32,
    line_count: u8,
    all_local: bool,
    entry_date: u32,
) -> Vec<u8> {
    let mut bytes = encode_i32(customer_id);
    bytes.extend_from_slice(&encode_i32(carrier_id));
    bytes.push(line_count);
    bytes.push(u8::from(all_local));
    bytes.extend_from_slice(&encode_u32(entry_date));
    bytes
}

fn order_line_value(
    item_id: i32,
    delivery_date: u32,
    amount: f32,
    supply_warehouse: i32,
    quantity: u8,
) -> Vec<u8> {
    let mut bytes = encode_i32(item_id);
    bytes.extend_from_slice(&encode_u32(delivery_date));
    bytes.extend_from_slice(&amount.to_ne_bytes());
    bytes.extend_from_slice(&encode_i32(supply_warehouse));
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

unsafe fn table_size(table: *mut StoTpccTable) -> u64 {
    let mut rows = u64::MAX;
    expect(unsafe { sto_tpcc_table_size(table, &mut rows) }, OK);
    rows
}

fn balance(bytes: &[u8]) -> f32 {
    f32::from_ne_bytes(
        bytes
            .try_into()
            .expect("customer-balance rows contain exactly four bytes"),
    )
}

#[test]
fn full_delivery_preserves_scalar_empty_zero_line_and_rollback_semantics() {
    unsafe {
        let db_config = StoTpccDbConfig {
            max_threads: 4,
            max_key_length: 64,
            max_items_per_txn: 512,
            max_locks_per_txn: 512,
        };
        let table_config = StoTpccTableConfig {
            max_retained_records: 512,
            max_retained_key_bytes: 32_768,
            max_consumed_record_ids: 1_024,
            trusted_scan_value_generation: 1,
            bounded_atomic_values: 1,
            ..StoTpccTableConfig::default()
        };
        let mut db = ptr::null_mut();
        expect(sto_tpcc_db_create(&db_config, &mut db), OK);
        let mut tables = [ptr::null_mut(); 4];
        for table in &mut tables {
            expect(sto_tpcc_table_create(db, &table_config, table), OK);
        }
        let [new_order, oorder, order_line, customer] = tables;
        let mut thread = ptr::null_mut();
        expect(sto_tpcc_thread_create(db, &mut thread), OK);

        let mut cursors = [0_i32; 10];
        let mut request = MakoStoTpccDeliveryFullRequest {
            new_order_table: new_order,
            oorder_table: oorder,
            order_line_table: order_line,
            customer_table: customer,
            last_no_o_ids: cursors.as_mut_ptr(),
            warehouse_id: 1,
            carrier_id: 7,
            timestamp: 900_001,
        };

        // An empty Delivery still commits inside Rust and leaves all cursors
        // untouched. A second commit proves that no active attempt leaks.
        let mut result = MakoStoTpccDeliveryFullResult {
            reported_value_bytes: usize::MAX,
            delivered_districts: u32::MAX,
            updated_order_lines: u32::MAX,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_delivery_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(result, MakoStoTpccDeliveryFullResult::default());
        assert_eq!(cursors, [0; 10]);
        expect(sto_tpcc_txn_commit(thread), FATAL);

        // District 1 deliberately has no order-line rows. District 2 has the
        // maximum legal scan size. A second district-1 new-order row verifies
        // that only the first row at or after the cursor is selected.
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, new_order, &key3(1, 1, 10), &[b' '; 12]);
        insert(thread, new_order, &key3(1, 1, 11), &[b' '; 12]);
        insert(thread, new_order, &key3(1, 2, 20), &[b' '; 12]);
        insert(
            thread,
            oorder,
            &key3(1, 1, 10),
            &oorder_value(101, 0, 5, true, 111),
        );
        insert(
            thread,
            oorder,
            &key3(1, 1, 11),
            &oorder_value(111, 0, 5, true, 112),
        );
        insert(
            thread,
            oorder,
            &key3(1, 2, 20),
            &oorder_value(102, 0, 15, true, 222),
        );
        insert(thread, customer, &key3(1, 201, 101), &5_f32.to_ne_bytes());
        insert(
            thread,
            customer,
            &key3(1, 202, 102),
            &(-10_f32).to_ne_bytes(),
        );
        for line_number in 1_i32..=15 {
            insert(
                thread,
                order_line,
                &key4(1, 2, 20, line_number),
                &order_line_value(
                    1_000 + line_number,
                    0,
                    line_number as f32,
                    1,
                    line_number as u8,
                ),
            );
        }
        expect(sto_tpcc_txn_commit(thread), OK);
        assert_eq!(table_size(new_order), 3);

        result = MakoStoTpccDeliveryFullResult {
            reported_value_bytes: usize::MAX,
            delivered_districts: u32::MAX,
            updated_order_lines: u32::MAX,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_delivery_full_trusted(thread, &request, &mut result),
            OK,
        );
        assert_eq!(
            result,
            MakoStoTpccDeliveryFullResult {
                reported_value_bytes: 0,
                delivered_districts: 2,
                updated_order_lines: 15,
            }
        );
        assert_eq!(cursors[0], 11);
        assert_eq!(cursors[1], 21);
        assert_eq!(table_size(new_order), 1);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read_status(thread, new_order, &key3(1, 1, 10)).0, MISS);
        assert_eq!(read_status(thread, new_order, &key3(1, 2, 20)).0, MISS);
        assert_eq!(read(thread, new_order, &key3(1, 1, 11)), [b' '; 12]);
        assert_eq!(
            read(thread, oorder, &key3(1, 1, 10)),
            oorder_value(101, 7, 5, true, 111)
        );
        assert_eq!(
            read(thread, oorder, &key3(1, 1, 11)),
            oorder_value(111, 0, 5, true, 112)
        );
        assert_eq!(
            read(thread, oorder, &key3(1, 2, 20)),
            oorder_value(102, 7, 15, true, 222)
        );
        assert_eq!(balance(&read(thread, customer, &key3(1, 201, 101))), 5.0);
        assert_eq!(balance(&read(thread, customer, &key3(1, 202, 102))), 110.0);
        for line_number in 1_i32..=15 {
            assert_eq!(
                read(thread, order_line, &key4(1, 2, 20, line_number)),
                order_line_value(
                    1_000 + line_number,
                    request.timestamp,
                    line_number as f32,
                    1,
                    line_number as u8,
                )
            );
        }
        expect(sto_tpcc_txn_commit(thread), OK);

        // District 3 stages a normal one-line delivery. District 4 advances
        // its cursor and then fails on a missing oorder. Both cursor writes
        // survive, while every STO mutation and size delta rolls back.
        expect(sto_tpcc_txn_begin(thread), OK);
        insert(thread, new_order, &key3(1, 3, 30), &[b' '; 12]);
        insert(thread, new_order, &key3(1, 4, 40), &[b' '; 12]);
        insert(
            thread,
            oorder,
            &key3(1, 3, 30),
            &oorder_value(103, 0, 5, false, 333),
        );
        insert(
            thread,
            order_line,
            &key4(1, 3, 30, 1),
            &order_line_value(2_001, 0, 4.25, 1, 3),
        );
        insert(thread, customer, &key3(1, 203, 103), &1.5_f32.to_ne_bytes());
        expect(sto_tpcc_txn_commit(thread), OK);
        assert_eq!(table_size(new_order), 3);

        cursors[0] = 100;
        result = MakoStoTpccDeliveryFullResult {
            reported_value_bytes: 71,
            delivered_districts: 72,
            updated_order_lines: 73,
        };
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_delivery_full_trusted(thread, &request, &mut result),
            RETRY,
        );
        assert_eq!(
            result,
            MakoStoTpccDeliveryFullResult {
                reported_value_bytes: 71,
                delivered_districts: 72,
                updated_order_lines: 73,
            }
        );
        assert_eq!(cursors[2], 31);
        assert_eq!(cursors[3], 41);
        assert_eq!(table_size(new_order), 3);
        expect(sto_tpcc_txn_commit(thread), FATAL);

        expect(sto_tpcc_txn_begin(thread), OK);
        assert_eq!(read(thread, new_order, &key3(1, 3, 30)), [b' '; 12]);
        assert_eq!(read(thread, new_order, &key3(1, 4, 40)), [b' '; 12]);
        assert_eq!(
            read(thread, oorder, &key3(1, 3, 30)),
            oorder_value(103, 0, 5, false, 333)
        );
        assert_eq!(
            read(thread, order_line, &key4(1, 3, 30, 1)),
            order_line_value(2_001, 0, 4.25, 1, 3)
        );
        assert_eq!(balance(&read(thread, customer, &key3(1, 203, 103))), 1.5);
        expect(sto_tpcc_txn_commit(thread), OK);

        // Invalid input also consumes the active attempt and leaves result and
        // cursors unchanged.
        request.carrier_id = 0;
        let cursors_before_invalid = cursors;
        expect(sto_tpcc_txn_begin(thread), OK);
        expect(
            mako_sto_tpcc_delivery_full_trusted(thread, &request, &mut result),
            FATAL,
        );
        assert_eq!(cursors, cursors_before_invalid);
        assert_eq!(result.reported_value_bytes, 71);
        expect(sto_tpcc_txn_commit(thread), FATAL);

        expect(sto_tpcc_thread_destroy(thread), OK);
        for table in tables.into_iter().rev() {
            expect(sto_tpcc_table_destroy(table), OK);
        }
        expect(sto_tpcc_db_destroy(db), OK);
    }
}
