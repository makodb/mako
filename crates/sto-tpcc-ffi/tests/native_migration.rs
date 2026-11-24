#![cfg(mtree_native_integration)]
use sto_tpcc_ffi::native::{migration::Registry, Database, DatabaseConfig, TableConfig};

#[test]
fn migration_worker_keeps_registered_storage_alive_through_scan_and_cleanup() {
    let database = Database::new(DatabaseConfig::default()).unwrap();
    let table = database
        .worker()
        .unwrap()
        .create_table(TableConfig::default())
        .unwrap();
    let registry = Registry::new(database.clone(), 0).unwrap();
    registry.register(101, None, &table, 1).unwrap();
    assert!(Registry::new(database.clone(), 0).is_err());
    let mut worker = registry.worker().unwrap();
    drop(table);
    drop(registry);
    drop(database);
    worker.put(101, b"a", b"a", b"one").unwrap();
    worker.put(101, b"b", b"b", b"two").unwrap();
    let first = worker.scan(101, b"", None, None).unwrap().unwrap();
    assert_eq!(
        (
            first.coordinate.as_slice(),
            first.key.as_slice(),
            first.value.as_slice()
        ),
        (b"a".as_slice(), b"a".as_slice(), b"one".as_slice())
    );
    let next = worker
        .scan(101, b"", None, Some((&first.coordinate, &first.key)))
        .unwrap()
        .unwrap();
    assert_eq!(next.key, b"b");
    worker.remove(101, b"a", b"a").unwrap();
    assert_eq!(
        worker.scan(101, b"", None, None).unwrap().unwrap().key,
        b"b"
    );
    drop(worker);
    // Last-worker drop unregisters the raw handles before dropping the tables.
    let database = Database::new(DatabaseConfig::default()).unwrap();
    assert!(Registry::new(database, 0).is_ok());
}

#[test]
fn warehouse_migration_uses_coordinate_then_key_and_rejects_foreign_database() {
    let database = Database::new(DatabaseConfig::default()).unwrap();
    let table = database
        .worker()
        .unwrap()
        .create_table(TableConfig::default())
        .unwrap();
    let registry = Registry::new(database, 3).unwrap();
    registry.register(102, Some((2, 4)), &table, 2).unwrap();
    assert!(registry.register(102, Some((2, 5)), &table, 2).is_err());
    let foreign = Database::new(DatabaseConfig::default()).unwrap();
    let foreign_table = foreign
        .worker()
        .unwrap()
        .create_table(TableConfig::default())
        .unwrap();
    assert!(registry.register(103, None, &foreign_table, 3).is_err());
    let mut worker = registry.worker().unwrap();
    let coordinate = 2u32.to_be_bytes();
    worker.put(102, &coordinate, b"key", b"value").unwrap();
    let row = worker
        .scan(102, &coordinate, Some(&3u32.to_be_bytes()), None)
        .unwrap()
        .unwrap();
    assert_eq!(row.coordinate, coordinate);
    assert_eq!(row.key, b"key");
    assert!(worker
        .scan(102, &3u32.to_be_bytes(), None, None)
        .unwrap()
        .is_none());
}
