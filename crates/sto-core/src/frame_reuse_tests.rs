//! Ownership checks for the worker's reusable general transaction frame.

use super::item_index_tests::{cache_resource, cached_bindings};
use crate::transaction::TransactionFrame;
use crate::{
    AccessError, CommitHook, CommitHookError, CommitOutcome, InvalidUse, Runtime, RuntimeConfig,
    RuntimeHealth, TxnCell, WorkerContext,
};
use std::panic::{catch_unwind, AssertUnwindSafe};

fn idle_frame(worker: &WorkerContext) -> *const TransactionFrame {
    std::ptr::from_ref(worker.transaction_frame.as_deref().unwrap())
}

#[test]
fn frame_reuse_preserves_one_allocation_across_commit_abort_and_drop() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 0_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let address = idle_frame(&worker);
    let runtime_owners = std::sync::Arc::strong_count(&runtime);
    let mut visible = 0;
    for generation in 1..=48 {
        let mut transaction = worker.begin().unwrap();
        assert!(transaction
            .worker
            .as_ref()
            .unwrap()
            .transaction_frame
            .is_none());
        assert_eq!(
            std::ptr::from_ref(transaction.frame.as_deref().unwrap()),
            address
        );
        assert_eq!(cell.get(&mut transaction).unwrap(), visible);
        cell.set(&mut transaction, generation).unwrap();
        assert_eq!(cell.get(&mut transaction).unwrap(), generation);
        match generation % 3 {
            0 => {
                assert!(matches!(
                    transaction.commit().unwrap(),
                    CommitOutcome::Committed(_)
                ));
                visible = generation;
            }
            1 => {
                transaction.abort();
            }
            _ => drop(transaction),
        }
        assert!(!worker.transaction_active);
        assert_eq!(idle_frame(&worker), address);
        assert_eq!(std::sync::Arc::strong_count(&runtime), runtime_owners);
    }
    let mut verify = worker.begin().unwrap();
    assert_eq!(cell.get(&mut verify).unwrap(), visible);
    verify.commit().unwrap();
    drop(worker);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn frame_reuse_resets_metadata_after_typed_materialized_and_doomed_attempts() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let resource = cache_resource(&runtime);
    let cell = TxnCell::new(&runtime, 7_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let address = idle_frame(&worker);
    for materialize in [false, true, false, true] {
        let mut transaction = worker.begin().unwrap();
        let frame = transaction.frame.as_deref().unwrap();
        assert_eq!(std::ptr::from_ref(frame), address);
        assert_eq!(frame.item_count, 0);
        assert_eq!(frame.item_index.len, 0);
        assert!(!frame.batch_active);
        assert!(frame.last_accessed_item_slot.is_none());
        assert!(!frame.doomed);
        assert!(cached_bindings(&transaction).is_empty());
        assert!(!transaction.has_items_for(&resource));
        transaction.with_item(&resource, 1, |_| Ok(())).unwrap();
        assert!(transaction.frame.as_ref().unwrap().batch_active);
        if materialize {
            assert_eq!(cell.get(&mut transaction).unwrap(), 7);
            assert!(!transaction.frame.as_ref().unwrap().batch_active);
        }
        let failure: Result<(), AccessError> =
            transaction.with_item(&resource, 1, |_| Err(InvalidUse::IllegalItemState.into()));
        assert_eq!(failure, Err(InvalidUse::IllegalItemState.into()));
        assert!(transaction.is_doomed());
        let frame = transaction.frame.as_deref().unwrap();
        let capacities = (frame.items.capacity(), frame.item_index.entries.capacity());
        transaction.abort();
        assert_eq!(idle_frame(&worker), address);
        let frame = worker.transaction_frame.as_deref().unwrap();
        assert_eq!(
            (frame.items.capacity(), frame.item_index.entries.capacity()),
            capacities
        );
    }
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn rejected_begin_leaves_the_idle_frame_owned_and_reusable() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let mut worker = runtime.attach().unwrap();
    let address = idle_frame(&worker);
    assert!(worker.begin_with(crate::IsolationMode::Opaque).is_err());
    assert!(!worker.transaction_active);
    assert_eq!(idle_frame(&worker), address);
    worker.begin().unwrap().commit().unwrap();
    assert_eq!(idle_frame(&worker), address);
}

#[test]
fn frame_reuse_survives_caller_unwind_and_keeps_terminal_pool_separate() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 5_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let address = idle_frame(&worker);
    let unwind = catch_unwind(AssertUnwindSafe(|| {
        let mut transaction = worker.begin().unwrap();
        cell.set(&mut transaction, 99).unwrap();
        panic!("caller unwinds while owning a reusable frame");
    }));
    assert!(unwind.is_err());
    assert_eq!(idle_frame(&worker), address);
    let terminal_address = std::ptr::from_ref(worker.terminal_read_scratch.as_deref().unwrap());
    drop(worker.begin_terminal_read_batch().unwrap());
    assert_eq!(idle_frame(&worker), address);
    assert_eq!(
        std::ptr::from_ref(worker.terminal_read_scratch.as_deref().unwrap()),
        terminal_address
    );
    let mut verify = worker.begin().unwrap();
    assert_eq!(cell.get(&mut verify).unwrap(), 5);
    verify.commit().unwrap();
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

struct PanickingReservation;

impl CommitHook for PanickingReservation {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        panic!("reservation failure before irreversible publication");
    }

    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        panic!("failed reservation must not reach pre-install");
    }
}

#[test]
fn frame_reuse_after_clean_failure_does_not_bypass_runtime_poison() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 5_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let address = idle_frame(&worker);
    let mut transaction = worker.begin().unwrap();
    cell.set(&mut transaction, 99).unwrap();
    assert!(transaction
        .commit_with_hook(&mut PanickingReservation)
        .is_err());
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    assert!(!worker.transaction_active);
    // Clean rollback can recycle storage, but cannot authorize another begin.
    assert_eq!(idle_frame(&worker), address);
    assert!(worker.begin().is_err());
    std::thread::scope(|scope| {
        scope
            .spawn(|| assert!(runtime.attach().is_err()))
            .join()
            .unwrap();
    });
}
