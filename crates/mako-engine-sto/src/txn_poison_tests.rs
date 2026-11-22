//! These native tests fail inside sto-core itself. They deliberately retain
//! poisoned runtimes and run on fresh OS workers, as production quarantine
//! requires. No simulated cleanup-failure flag is armed.

use super::*;
use std::sync::{mpsc, Arc, Mutex};
use sto_core::{RuntimeHealth, TxnCell};

pub(super) static SERIAL: Mutex<()> = Mutex::new(());

fn on_fresh_worker(test: impl FnOnce() + Send + 'static) {
    let _serial = SERIAL.lock().unwrap();
    std::thread::spawn(test).join().unwrap();
}

#[derive(Clone)]
struct PanicOnDrop(Arc<AtomicBool>);

impl PanicOnDrop {
    fn new(armed: bool) -> Self {
        Self(Arc::new(AtomicBool::new(armed)))
    }
}

impl Drop for PanicOnDrop {
    fn drop(&mut self) {
        assert!(
            !self.0.swap(false, Ordering::AcqRel),
            "injected core payload cleanup panic"
        );
    }
}

pub(super) fn stage_cleanup_panic(db: &LocalDb, transaction: &mut Transaction<'_>) {
    let cell = TxnCell::new(db.runtime_for_test(), PanicOnDrop::new(false)).unwrap();
    cell.set(transaction.inner_mut(), PanicOnDrop::new(true))
        .unwrap();
}

fn assert_quarantined(db: &LocalDb, previous_count: u64, health: RuntimeHealth) {
    assert_eq!(db.runtime_for_test().health(), health);
    assert!(!db.runtime_is_healthy());
    assert_eq!(worker::health(), mako_engine_api::WorkerHealth::Poisoned);
    assert_eq!(worker::quarantined_worker_count(), previous_count + 1);
    assert_eq!(db.transaction().unwrap_err(), Error::WorkerPoisoned);
    assert_eq!(worker::quarantined_worker_count(), previous_count + 1);
}

#[test]
fn explicit_abort_reports_real_core_cleanup_poison() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        let previous_count = worker::quarantined_worker_count();
        let mut transaction = db.transaction().unwrap();
        stage_cleanup_panic(&db, &mut transaction);

        assert_eq!(transaction.abort(), Err(Error::WorkerPoisoned));
        assert_quarantined(&db, previous_count, RuntimeHealth::Poisoned);
    });
}

#[test]
fn dropped_transaction_retires_worker_after_real_core_cleanup_panic() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        let previous_count = worker::quarantined_worker_count();
        let mut transaction = db.transaction().unwrap();
        stage_cleanup_panic(&db, &mut transaction);

        drop(transaction);
        assert_quarantined(&db, previous_count, RuntimeHealth::Poisoned);
    });
}

#[test]
fn rejected_record_preserves_real_abort_cleanup_uncertainty() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        let previous_count = worker::quarantined_worker_count();
        let mut transaction = db.transaction().unwrap();
        stage_cleanup_panic(&db, &mut transaction);

        // No record preflight exists, so the public terminal must abort. Its
        // cleanup failure takes precedence over InvalidArgument.
        let report = unsafe {
            transaction.commit_report_with_record_target(|_, _| {
                panic!("a rejected terminal must not acquire record storage")
            })
        };
        assert_eq!(report.commit, QUARANTINED_REPORT);
        assert!(!report.record_bound && !report.record_written);
        assert_quarantined(&db, previous_count, RuntimeHealth::Poisoned);
    });
}

#[test]
fn failed_operation_preserves_real_abort_cleanup_uncertainty() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        let table = db.open_table("operation", 1).unwrap();
        let previous_count = worker::quarantined_worker_count();
        let mut transaction = db.transaction().unwrap();
        stage_cleanup_panic(&db, &mut transaction);

        // Exhausting the public item budget follows fail_closed, another
        // operation-abort path that must retain the cleanup failure.
        transaction.budget_used = TRANSACTION_ITEM_BUDGET;
        assert_eq!(
            transaction.get(&table, b"excess"),
            Err(Error::WorkerPoisoned)
        );
        assert_quarantined(&db, previous_count, RuntimeHealth::Poisoned);
    });
}

#[test]
fn committed_cleanup_panic_keeps_the_definite_commit() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        let previous_count = worker::quarantined_worker_count();
        // The replaced committed value drops in sto-core's finish callback,
        // after the new value and OCC version have been published.
        let cell = TxnCell::new(db.runtime_for_test(), PanicOnDrop::new(true)).unwrap();
        let mut transaction = db.transaction().unwrap();
        cell.set(transaction.inner_mut(), PanicOnDrop::new(false))
            .unwrap();

        let report = transaction.commit_report();
        assert_eq!(report.disposition, CommitDisposition::Committed);
        assert_eq!(report.cleanup, Err(Error::WorkerPoisoned));
        assert_quarantined(&db, previous_count, RuntimeHealth::Poisoned);
    });
}

fn panic_with_gate_held(phase: TestCommitPhase, _: Option<MakoTimestamp>) {
    if phase == TestCommitPhase::MakoTimestampAllocated {
        assert!(mako_clock::global().general_gate_held());
        panic!("injected pre-install observer panic");
    }
}

#[test]
fn actual_hook_poison_retires_existing_and_new_workers_and_releases_gate() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("poison", 1).unwrap();
        let previous_count = worker::quarantined_worker_count();
        std::thread::scope(|scope| {
            let (attached_tx, attached_rx) = mpsc::sync_channel(0);
            let (poisoned_tx, poisoned_rx) = mpsc::sync_channel(0);
            let db = &db;
            let existing = scope.spawn(move || {
                db.transaction().unwrap().abort().unwrap();
                assert!(db.runtime_is_healthy());
                attached_tx.send(()).unwrap();
                poisoned_rx.recv().unwrap();
                assert!(!db.runtime_is_healthy());
                assert_eq!(worker::health(), mako_engine_api::WorkerHealth::Healthy);
                assert_eq!(db.transaction().unwrap_err(), Error::WorkerPoisoned);
                assert_eq!(worker::health(), mako_engine_api::WorkerHealth::Poisoned);
                assert_eq!(db.transaction().unwrap_err(), Error::WorkerPoisoned);
            });
            attached_rx.recv().unwrap();

            let (table, mut transaction) = db.trusted_bound_transaction().unwrap();
            transaction.put(&table, b"key", b"value").unwrap();
            transaction
                .commit_record_preflight_with_checksum(256, CommitRecordChecksum::None)
                .unwrap();
            worker::install_observer(panic_with_gate_held);
            let report = unsafe {
                transaction.commit_report_with_record_target(|_, _| {
                    panic!("observer must fail before record acquisition")
                })
            };
            worker::clear_observer();
            assert_eq!(
                report.commit.disposition,
                CommitDisposition::Aborted(Error::Internal)
            );
            assert_eq!(report.commit.cleanup, Err(Error::WorkerPoisoned));
            assert!(!report.record_bound && !report.record_written);
            assert!(!mako_clock::global().general_gate_held());
            assert_eq!(db.ticket_next.load(Ordering::Acquire), 1);
            assert_eq!(db.ticket_serving.load(Ordering::Acquire), 1);
            assert_quarantined(db, previous_count, RuntimeHealth::Poisoned);

            poisoned_tx.send(()).unwrap();
            existing.join().unwrap();
            assert_eq!(worker::quarantined_worker_count(), previous_count + 2);

            scope
                .spawn(|| {
                    assert_eq!(worker::health(), mako_engine_api::WorkerHealth::NotAttached);
                    assert_eq!(db.transaction().unwrap_err(), Error::WorkerPoisoned);
                    assert_eq!(worker::health(), mako_engine_api::WorkerHealth::Poisoned);
                    assert_eq!(db.transaction().unwrap_err(), Error::WorkerPoisoned);
                })
                .join()
                .unwrap();
            assert_eq!(worker::quarantined_worker_count(), previous_count + 3);
        });
    });
}

fn panic_after_install(phase: TestCommitPhase, _: Option<MakoTimestamp>) {
    if phase == TestCommitPhase::AllWritesInstalled {
        panic!("injected post-install observer panic");
    }
}

#[test]
fn post_install_panic_keeps_record_bound_and_outcome_unknown() {
    on_fresh_worker(|| {
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("indeterminate", 1).unwrap();
        let previous_count = worker::quarantined_worker_count();
        let (table, mut transaction) = db.trusted_bound_transaction().unwrap();
        transaction.put(&table, b"key", b"value").unwrap();
        let plan = transaction
            .commit_record_preflight_with_checksum(256, CommitRecordChecksum::None)
            .unwrap();
        let mut record = vec![0; plan.exact_record_bytes()];
        worker::install_observer(panic_after_install);
        let report = unsafe {
            transaction.commit_report_with_record_target(|_, plan| {
                Some(CommitRecordTarget::from_raw_parts(
                    NonZeroU64::new(1).unwrap(),
                    std::ptr::NonNull::new(record.as_mut_ptr()).unwrap(),
                    plan.exact_record_bytes(),
                ))
            })
        };
        worker::clear_observer();

        assert_eq!(report.commit, QUARANTINED_REPORT);
        assert!(report.record_bound && report.record_written);
        assert!(!mako_clock::global().general_gate_held());
        assert_quarantined(&db, previous_count, RuntimeHealth::Indeterminate);
    });
}

static FIRST_INSTALLED: AtomicBool = AtomicBool::new(false);
static ALL_INSTALLED: AtomicBool = AtomicBool::new(false);

fn panic_between_installs(phase: TestCommitPhase, timestamp: Option<MakoTimestamp>) {
    match phase {
        TestCommitPhase::FirstWriteInstalled => {
            assert!(timestamp.is_some());
            assert!(!FIRST_INSTALLED.swap(true, Ordering::AcqRel));
            panic!("injected first-of-two installed writes panic");
        }
        TestCommitPhase::AllWritesInstalled => ALL_INSTALLED.store(true, Ordering::Release),
        _ => {}
    }
}

#[test]
fn mid_install_panic_is_indeterminate_and_does_not_emit_all_installed() {
    on_fresh_worker(|| {
        assert!(crate::features()
            .unwrap()
            .test_commit_mid_install_observer());
        FIRST_INSTALLED.store(false, Ordering::Release);
        ALL_INSTALLED.store(false, Ordering::Release);
        let db = LocalDb::open().unwrap();
        db.bind_trusted_table("partial-install", 1).unwrap();
        let previous_count = worker::quarantined_worker_count();
        let (table, mut transaction) = db.trusted_bound_transaction().unwrap();
        transaction.put(&table, b"first", b"one").unwrap();
        transaction.put(&table, b"second", b"two").unwrap();
        let plan = transaction
            .commit_record_preflight_with_checksum(256, CommitRecordChecksum::None)
            .unwrap();
        let mut record = vec![0; plan.exact_record_bytes()];
        worker::install_observer(panic_between_installs);
        let report = unsafe {
            transaction.commit_report_with_record_target(|_, plan| {
                Some(CommitRecordTarget::from_raw_parts(
                    NonZeroU64::new(1).unwrap(),
                    std::ptr::NonNull::new(record.as_mut_ptr()).unwrap(),
                    plan.exact_record_bytes(),
                ))
            })
        };
        worker::clear_observer();

        assert!(FIRST_INSTALLED.load(Ordering::Acquire));
        assert!(!ALL_INSTALLED.load(Ordering::Acquire));
        assert_eq!(report.commit, QUARANTINED_REPORT);
        assert!(report.record_bound && report.record_written);
        assert!(!mako_clock::global().general_gate_held());
        assert_quarantined(&db, previous_count, RuntimeHealth::Indeterminate);
    });
}
