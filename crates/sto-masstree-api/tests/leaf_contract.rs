#![cfg(mtree_native_integration)]
use std::ops::Bound;
use sto_masstree_api::native::*;

#[test]
fn missing_reads_do_not_allocate_even_when_row_budget_is_full() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker
        .create_table(TableConfig {
            max_retained_records: 1,
            ..Default::default()
        })
        .unwrap();
    let mut tx = worker.begin().unwrap();
    tx.put(&table, b"present", b"value").unwrap();
    tx.commit().unwrap();
    let before = db.native_row_bytes();
    for key in 0..1000u64 {
        let mut tx = worker.begin().unwrap();
        assert_eq!(tx.get(&table, &key.to_be_bytes()).unwrap(), None);
        assert!(!tx.remove(&table, &key.to_be_bytes()).unwrap());
        tx.commit().unwrap();
    }
    assert_eq!(db.native_row_bytes(), before);
}

fn populate(worker: &mut Worker, table: &Table) {
    for batch in 0..16u64 {
        let mut tx = worker.begin().unwrap();
        for key in batch * 64..(batch + 1) * 64 {
            tx.put(table, &(key * 2).to_be_bytes(), b"value").unwrap();
        }
        tx.commit().unwrap();
    }
}
fn concurrent_put(db: Database, table: Table, key: u64) {
    std::thread::spawn(move || {
        let mut worker = db.worker().unwrap();
        let mut tx = worker.begin().unwrap();
        tx.put(&table, &key.to_be_bytes(), b"new").unwrap();
        tx.commit().unwrap();
    })
    .join()
    .unwrap();
}
#[test]
fn scans_and_absence_validate_only_their_leaves_and_reject_real_phantoms() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    populate(&mut worker, &table);
    for reverse in [false, true] {
        let mut tx = worker.begin().unwrap();
        let upper = 8u64.to_be_bytes();
        assert_eq!(
            tx.scan(
                &table,
                Bound::Unbounded,
                Bound::Excluded(&upper),
                reverse,
                100
            )
            .unwrap()
            .len(),
            4
        );
        // An insertion far from these leaves must not invalidate this range.
        concurrent_put(db.clone(), table.clone(), if reverse { 1903 } else { 1901 });
        tx.commit().unwrap();
    }
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&table, &1u64.to_be_bytes()).unwrap(), None);
    concurrent_put(db.clone(), table.clone(), 1905);
    tx.commit().unwrap();
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&table, &1u64.to_be_bytes()).unwrap(), None);
    concurrent_put(db.clone(), table.clone(), 1);
    assert_eq!(tx.commit().unwrap_err().kind, ErrorKind::Conflict);
    let mut tx = worker.begin().unwrap();
    let upper = 8u64.to_be_bytes();
    tx.scan(
        &table,
        Bound::Unbounded,
        Bound::Excluded(&upper),
        false,
        100,
    )
    .unwrap();
    concurrent_put(db.clone(), table.clone(), 3);
    assert_eq!(tx.commit().unwrap_err().kind, ErrorKind::Conflict);
}

#[test]
fn own_insertions_and_splits_preserve_absence_and_scan_observations() {
    // Adapted from MassTransScanPredicateTracksOwnAndConcurrentPhantoms and
    // MassTransBoundedScanInclusivityAcrossMasstreeLeaves at 0d26e2e0b.
    let db = Database::new(DatabaseConfig {
        max_items_per_txn: 4096,
        ..Default::default()
    })
    .unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker
        .create_table(TableConfig {
            scan_chunk_records: 3,
            ..Default::default()
        })
        .unwrap();
    let mut tx = worker.begin().unwrap();
    assert!(tx
        .scan(&table, Bound::Unbounded, Bound::Unbounded, false, 1000)
        .unwrap()
        .is_empty());
    for i in 0..96u64 {
        let mut key = vec![0; 40];
        key.extend(i.to_be_bytes());
        assert_eq!(tx.get(&table, &key).unwrap(), None);
        tx.put(&table, &key, &i.to_le_bytes()).unwrap();
    }
    let forward = tx
        .scan(&table, Bound::Unbounded, Bound::Unbounded, false, 1000)
        .unwrap();
    let reverse = tx
        .scan(&table, Bound::Unbounded, Bound::Unbounded, true, 1000)
        .unwrap();
    assert_eq!(forward.len(), 96);
    assert_eq!(
        forward.iter().rev().collect::<Vec<_>>(),
        reverse.iter().collect::<Vec<_>>()
    );
    tx.commit().unwrap();
}

#[test]
fn original_same_key_composition_exhaustive_triples() {
    // Port of MassTransSameKeyCompositionExhaustiveTriples at 0d26e2e0b.
    // All 6^3 operation triples, both initial states, commit and abort: 864.
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let initial = b"initial".to_vec();
    let small = b"small\0binary".to_vec();
    let large = vec![b'L'; 65537];
    let mut sequence = 0u64;
    for present in [false, true] {
        for commit in [false, true] {
            for first in 0..6 {
                for second in 0..6 {
                    for third in 0..6 {
                        let key = sequence.to_be_bytes();
                        sequence += 1;
                        let initial = present.then(|| initial.clone());
                        if let Some(value) = &initial {
                            let mut tx = worker.begin().unwrap();
                            tx.put(&table, &key, value).unwrap();
                            tx.commit().unwrap();
                        }
                        let mut expected = initial.clone();
                        let mut tx = worker.begin().unwrap();
                        for operation in [first, second, third] {
                            match operation {
                                0 => assert_eq!(tx.get(&table, &key).unwrap(), expected),
                                1 | 2 => {
                                    let value = if operation == 1 { &small } else { &large };
                                    tx.put(&table, &key, value).unwrap();
                                    expected = Some(value.clone());
                                }
                                3 | 4 => {
                                    let value = if operation == 3 { &small } else { &large };
                                    assert_eq!(
                                        tx.insert(&table, &key, value).unwrap(),
                                        expected.is_none()
                                    );
                                    if expected.is_none() {
                                        expected = Some(value.clone());
                                    }
                                }
                                5 => {
                                    assert_eq!(
                                        tx.remove(&table, &key).unwrap(),
                                        expected.is_some()
                                    );
                                    expected = None;
                                }
                                _ => unreachable!(),
                            }
                            assert_eq!(tx.get(&table, &key).unwrap(), expected,
                                "sequence {sequence}, operations {first}/{second}/{third}, after {operation}");
                        }
                        if commit {
                            tx.commit().unwrap();
                        } else {
                            tx.abort().unwrap();
                        }
                        let mut tx = worker.begin().unwrap();
                        assert_eq!(
                            tx.get(&table, &key).unwrap(),
                            if commit { expected } else { initial }
                        );
                        tx.commit().unwrap();
                    }
                }
            }
        }
    }
    assert_eq!(sequence, 864);
}

#[test]
fn copied_reads_and_write_only_put_preserve_conflicts_and_read_your_writes() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut buffer = Vec::new();
    let mut tx = worker.begin().unwrap();
    assert!(!tx.get_into(&table, b"key", &mut buffer).unwrap());
    tx.put(&table, b"key", b"original").unwrap();
    assert!(tx.get_into(&table, b"key", &mut buffer).unwrap());
    assert_eq!(buffer, b"original");
    tx.commit().unwrap();
    let mut tx = worker.begin().unwrap();
    assert!(tx.get_into(&table, b"key", &mut buffer).unwrap());
    let (other_db, other_table) = (db.clone(), table.clone());
    std::thread::spawn(move || {
        let mut worker = other_db.worker().unwrap();
        let mut tx = worker.begin().unwrap();
        tx.put(&other_table, b"key", b"concurrent").unwrap();
        tx.commit().unwrap();
    })
    .join()
    .unwrap();
    tx.put(&table, b"key", b"stale").unwrap();
    assert!(tx.get_into(&table, b"key", &mut buffer).unwrap());
    assert_eq!(buffer, b"stale");
    assert_eq!(tx.commit().unwrap_err().kind, ErrorKind::Conflict);
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&table, b"key").unwrap().unwrap(), b"concurrent");
    tx.commit().unwrap();
}

#[test]
fn scoped_access_closes_rcu_before_return_and_aborts_errors_and_panics() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    let mut tx = worker.begin().unwrap();
    tx.with_access(|tx| {
        tx.put(&table, b"key", b"committed")?;
        assert_eq!(tx.get(&table, b"key")?.unwrap(), b"committed");
        Ok(())
    })
    .unwrap();
    // Opening another scope proves the first region already closed.
    tx.with_access(|tx| {
        assert_eq!(tx.get(&table, b"key")?.unwrap(), b"committed");
        Ok(())
    })
    .unwrap();
    tx.commit().unwrap();
    let mut tx = worker.begin().unwrap();
    assert!(tx
        .with_access(|tx| {
            tx.put(&table, b"key", b"error")?;
            Err::<(), _>(Error {
                kind: ErrorKind::InvalidUse,
                message: "injected".into(),
            })
        })
        .is_err());
    assert!(tx.commit().is_err());
    let mut tx = worker.begin().unwrap();
    assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _: Result<()> = tx.with_access(|tx| {
            tx.put(&table, b"key", b"panic")?;
            panic!("injected callback panic");
        });
    }))
    .is_err());
    assert!(tx.commit().is_err());
    let mut tx = worker.begin().unwrap();
    tx.with_access(|tx| {
        assert_eq!(tx.get(&table, b"key")?.unwrap(), b"committed");
        Ok(())
    })
    .unwrap();
    tx.commit().unwrap();
}

#[test]
fn stale_leaf_predicates_survive_later_staging_until_commit_rejects_them() {
    let db = Database::new(DatabaseConfig::default()).unwrap();
    let mut worker = db.worker().unwrap();
    let table = worker.create_table(TableConfig::default()).unwrap();
    for scan in [false, true] {
        let mut tx = worker.begin().unwrap();
        if scan {
            tx.scan(&table, Bound::Unbounded, Bound::Unbounded, false, 100)
                .unwrap();
        } else {
            assert_eq!(tx.get(&table, b"foreign").unwrap(), None);
        }
        let (db2, table2) = (db.clone(), table.clone());
        std::thread::spawn(move || {
            let mut worker = db2.worker().unwrap();
            let mut other = worker.begin().unwrap();
            other
                .put(&table2, if scan { b"phantom" } else { b"foreign" }, b"new")
                .unwrap();
            other.commit().unwrap();
        })
        .join()
        .unwrap();
        // Re-observing this stale leaf must not update its saved generation.
        assert_eq!(tx.get(&table, b"still-missing").unwrap(), None);
        tx.put(
            &table,
            if scan { b"later-scan" } else { b"later-point" },
            b"never",
        )
        .unwrap();
        assert_eq!(tx.commit().unwrap_err().kind, ErrorKind::Conflict);
    }
    let mut tx = worker.begin().unwrap();
    assert_eq!(tx.get(&table, b"later-point").unwrap(), None);
    assert_eq!(tx.get(&table, b"later-scan").unwrap(), None);
    tx.commit().unwrap();
}
