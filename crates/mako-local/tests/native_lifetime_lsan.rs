#![cfg(have_mako)]

use std::hint::black_box;
use std::mem;

use mako_local::{LocalDb, Table};

const KEY: &[u8] = b"owned-read-result";
const VALUE_BYTES: usize = 1301;

// Keep the real read-result allocation and intentional forget in one stable
// stack frame. Native process-lifetime suppressions must not hide this owned
// Rust payload, whose native temporary buffer is freed by Transaction::get.
#[inline(never)]
fn lose_owned_read_value(db: &LocalDb, table: &Table<'_>, expected: &[u8]) {
    let mut transaction = db.transaction().expect("begin read transaction");
    let value = transaction
        .get(table, KEY)
        .expect("read committed value")
        .expect("committed key exists")
        .into_boxed_slice();
    assert_eq!(value.as_ref(), expected);
    transaction.commit().expect("commit read transaction");

    // A boxed slice fixes the leaked allocation's size, even if get's Vec
    // acquired spare capacity. Only this payload is deliberately forgotten.
    mem::forget(black_box(value));
}

#[test]
#[ignore = "deliberately leaks one 1301-byte owned read buffer; run only as the native LSan negative control"]
fn lost_transaction_buffer_is_reported_with_native_lifetime_rules() {
    let expected = black_box([0xa5; VALUE_BYTES]);
    let db = LocalDb::open().expect("open native database");
    {
        let table = db
            .open_table("rust_native_lifetime_lsan", 20_100)
            .expect("open native table");
        let mut transaction = db.transaction().expect("begin seed transaction");
        transaction
            .put(&table, KEY, &expected)
            .expect("write seed value");
        transaction.commit().expect("commit seed transaction");

        // Use a separate transaction: no read-my-writes feature is required,
        // so this probe exercises both STO_RMW=OFF and STO_RMW=ON builds.
        lose_owned_read_value(&db, &table, &expected);
    }
    db.close().expect("close native database facade");
}
