#![cfg(mtree_native_integration)]
use std::ops::Bound;
use sto_masstree_api::native::*;
#[test]
fn binary_values_and_sequential_duplicate_mutations_preserve_cross_table_atomicity() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let a = worker.create_table(TableConfig::default()).unwrap();
    let b = worker.create_table(TableConfig::default()).unwrap();
    for size in [
        0, 1, 15, 16, 17, 31, 32, 33, 159, 160, 161, 256, 1024, 65537,
    ] {
        let bytes: Vec<u8> = (0..size).map(|i| (i % 251) as u8).collect();
        let mut tx = worker.begin().unwrap();
        assert!(tx.insert(&a, b"\0key", &bytes).unwrap());
        assert!(!tx.insert(&a, b"\0key", b"duplicate").unwrap());
        assert_eq!(tx.get(&a, b"\0key").unwrap(), Some(bytes.clone()));
        tx.put(&b, b"other", &bytes).unwrap();
        tx.commit().unwrap();
        let mut tx = worker.begin().unwrap();
        tx.put(&a, b"\0key", b"aborted").unwrap();
        tx.remove(&b, b"other").unwrap();
        tx.abort().unwrap();
        let mut tx = worker.begin().unwrap();
        assert_eq!(tx.get(&a, b"\0key").unwrap(), Some(bytes.clone()));
        assert_eq!(tx.get(&b, b"other").unwrap(), Some(bytes));
        assert!(tx.remove(&a, b"\0key").unwrap());
        assert!(!tx.remove(&a, b"\0key").unwrap());
        tx.commit().unwrap();
        assert_eq!(a.len(), 0);
        assert_eq!(b.len(), 1);
    }
}
#[test]
fn sealed_directory_keeps_rows_mutable_and_rejects_new_keys() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&table, b"a", b"old").unwrap();
    tx.commit().unwrap();
    table.seal_directory_structure().unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&table, b"a", b"new").unwrap();
    tx.commit().unwrap();
    let mut tx = worker.begin().unwrap();
    assert!(tx.put(&table, b"b", b"bad").is_err());
    assert!(tx.commit().is_err());
    let mut tx = worker.begin().unwrap();
    assert_eq!(
        tx.scan(&table, Bound::Unbounded, Bound::Unbounded, false, 10)
            .unwrap(),
        vec![(b"a".to_vec(), b"new".to_vec())]
    );
    tx.commit().unwrap();
}
