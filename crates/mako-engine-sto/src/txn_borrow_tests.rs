//! Native adapter coverage for the first-PUT-only ownership lane.

use super::*;

fn healthy(test: impl FnOnce() + Send + 'static) {
    std::thread::spawn(|| {
        test();
        // SAFETY: each closure returns only after dropping all local handles.
        unsafe { worker::drop_healthy_test_attachments() };
    })
    .join()
    .unwrap();
}

#[test]
fn first_put_then_read_rewrite_delete_insert_and_scan_keep_canonical_values() {
    healthy(|| {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("borrow-mixed", 1).unwrap();
        let (table, mut tx) = db.trusted_bound_transaction().unwrap();
        tx.put(&table, b"key", &[1; 128]).unwrap();
        assert!(tx.touched.value_is_borrowed(0));
        assert_eq!(tx.get(&table, b"key").unwrap().unwrap(), [1; 128]);
        tx.put(&table, b"key", &[2; 128]).unwrap();
        assert!(!tx.touched.value_is_borrowed(0));
        assert_eq!(tx.get(&table, b"key").unwrap().unwrap(), [2; 128]);
        assert!(tx.remove(&table, b"key").unwrap());
        assert!(tx.insert(&table, b"key", &[3; 161]).unwrap());
        assert_eq!(tx.get(&table, b"key").unwrap().unwrap(), [3; 161]);
        {
            let mut scan = tx.scan(&table, b"", None).unwrap();
            let row = scan.next().unwrap().unwrap();
            assert_eq!(row.0, b"key");
            assert_eq!(row.1, [3; 161]);
            assert!(scan.next().is_none());
        }
        let ops = tx.canonical_ops();
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].value, [3; 161]);
        assert!(tx.commit_report().cleanup.is_ok());
        let mut verify = db.transaction().unwrap();
        assert_eq!(verify.get(&table, b"key").unwrap().unwrap(), [3; 161]);
        verify.abort().unwrap();
    });
}

#[test]
fn only_first_trusted_bounded_operation_borrows_and_cold_fallbacks_stay_owned() {
    healthy(|| {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("borrow-boundaries", 1).unwrap();
        for size in [0, 1, 38, 39, 127, 128, 159, 160, 161, 4096] {
            let (table, mut tx) = db.trusted_bound_transaction().unwrap();
            tx.put(&table, b"key", &vec![size as u8; size]).unwrap();
            assert_eq!(tx.touched.value_is_borrowed(0), (39..=160).contains(&size));
            tx.put(&table, b"other", &[7; 128]).unwrap();
            assert!(!tx.touched.value_is_borrowed(1));
            tx.abort().unwrap();
        }
        let (table, mut tx) = db.trusted_bound_transaction().unwrap();
        tx.get(&table, b"earlier-read").unwrap();
        tx.put(&table, b"key", &[5; 128]).unwrap();
        assert!(!tx.touched.value_is_borrowed(1));
        tx.abort().unwrap();
        let mut tx = db.transaction().unwrap();
        tx.put(&table, b"key", &[6; 128]).unwrap();
        assert!(!tx.touched.value_is_borrowed(0));
        drop(tx);
    });
}

#[test]
fn definite_hook_rejection_and_operation_abort_release_the_borrowed_payload() {
    healthy(|| {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("borrow-reject", 1).unwrap();
        let (table, mut tx) = db.trusted_bound_transaction().unwrap();
        tx.put(&table, b"key", &[8; 128]).unwrap();
        tx.commit_record_preflight_with_checksum(1024, CommitRecordChecksum::None)
            .unwrap();
        let report = unsafe { tx.commit_report_with_record_target(|_, _| None) };
        assert!(matches!(
            report.commit.disposition,
            CommitDisposition::Aborted(_)
        ));
        assert!(report.commit.cleanup.is_ok());
        let (table, mut tx) = db.trusted_bound_transaction().unwrap();
        tx.put(&table, b"key", &[9; 128]).unwrap();
        tx.budget_used = TRANSACTION_ITEM_BUDGET;
        assert_eq!(
            tx.get(&table, b"over-budget"),
            Err(Error::TransactionTooLarge)
        );
        assert!(!tx.touched.value_is_borrowed(0));
        drop(tx);
        db.transaction().unwrap().abort().unwrap();
    });
}

fn panic_after_install(phase: TestCommitPhase, _: Option<MakoTimestamp>) {
    if phase == TestCommitPhase::AllWritesInstalled {
        panic!("borrow test after install");
    }
}

#[derive(Clone)]
struct CommitCleanupPanic(std::sync::Arc<AtomicBool>);

impl Drop for CommitCleanupPanic {
    fn drop(&mut self) {
        assert!(
            !self.0.swap(false, Ordering::AcqRel),
            "committed old-value cleanup panic"
        );
    }
}

#[test]
fn real_cleanup_panic_pins_original_for_abort_drop_operation_and_commit() {
    let _serial = super::poison_tests::SERIAL.lock().unwrap();
    for terminal in 0..4 {
        std::thread::spawn(move || {
            let db = LocalDb::open().unwrap();
            db.bind_trusted_table("borrow-cleanup", 1).unwrap();
            let (table, mut tx) = db.trusted_bound_transaction().unwrap();
            tx.put(&table, b"key", &[0xb9; 128]).unwrap();
            let original = tx.touched[0].value.as_ptr();
            if terminal == 3 {
                let cell = sto_core::TxnCell::new(
                    db.runtime_for_test(),
                    CommitCleanupPanic(std::sync::Arc::new(AtomicBool::new(true))),
                )
                .unwrap();
                cell.set(
                    tx.inner_mut(),
                    CommitCleanupPanic(std::sync::Arc::new(AtomicBool::new(false))),
                )
                .unwrap();
            } else {
                super::poison_tests::stage_cleanup_panic(&db, &mut tx);
            }
            match terminal {
                0 => assert_eq!(tx.abort(), Err(Error::WorkerPoisoned)),
                1 => drop(tx),
                2 => {
                    tx.budget_used = TRANSACTION_ITEM_BUDGET;
                    assert_eq!(tx.get(&table, b"excess"), Err(Error::WorkerPoisoned));
                    drop(tx);
                }
                3 => {
                    let report = tx.commit_report();
                    assert_eq!(report.disposition, CommitDisposition::Committed);
                    assert_eq!(report.cleanup, Err(Error::WorkerPoisoned));
                }
                _ => unreachable!(),
            }
            assert_eq!(
                db.runtime_for_test().health(),
                sto_core::RuntimeHealth::Poisoned
            );
            // SAFETY: the failed cleanup may retain this core borrow forever;
            // the wrapper must pin its owner before any facade is destroyed.
            assert_eq!(
                unsafe { std::slice::from_raw_parts(original, 128) },
                [0xb9; 128]
            );
        })
        .join()
        .unwrap();
    }
}

#[test]
fn uncertain_holder_keeps_original_backing_alive_after_pool_destruction() {
    // Existing poison regressions assert a process-global quarantine count.
    let _serial = super::poison_tests::SERIAL.lock().unwrap();
    std::thread::spawn(|| {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("borrow-uncertain-holder", 1).unwrap();
        let pool = TrustedOnePutHolderPool::new(4, 0, 0).unwrap();
        let (table, mut tx) = db.trusted_bound_transaction().unwrap();
        tx.put(&table, b"key", &[0xa9; 128]).unwrap();
        let original = tx.touched[0].value.as_ptr();
        let exact = tx.candidate_bytes.unwrap();
        worker::install_observer(panic_after_install);
        let result = unsafe {
            tx.commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
                exact,
                &pool,
                NonZeroU64::new(1).unwrap(),
            )
        };
        worker::clear_observer();
        assert_eq!(result.report, QUARANTINED_REPORT);
        assert_ne!(result.holder_state, 0);
        let view = unsafe { pool.view(NonZeroU64::new(1).unwrap()) }.unwrap();
        assert_eq!(view.value(), [0xa9; 128]);
        assert_ne!(view.value().as_ptr(), original);
        drop(pool);
        // SAFETY: failed cleanup must retain the original owner independently
        // of the destroyed holder. ASan detects a violated lifetime here.
        assert_eq!(
            unsafe { std::slice::from_raw_parts(original, 128) },
            [0xa9; 128]
        );
    })
    .join()
    .unwrap();
}
