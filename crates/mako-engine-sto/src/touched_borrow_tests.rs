//! Native-free ownership tests. All retained owners are reclaimed explicitly
//! after the simulated borrower retires, so Miri leak checks remain enabled.

use super::*;
use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc,
};

#[derive(Clone)]
struct ObservedBytes {
    bytes: &'static [u8],
    expected: u8,
    drops: Arc<AtomicUsize>,
}

impl Drop for ObservedBytes {
    fn drop(&mut self) {
        if !self.bytes.is_empty() {
            assert!(self.bytes.iter().all(|byte| *byte == self.expected));
            self.drops.fetch_add(1, Ordering::Relaxed);
        }
    }
}

unsafe fn observed(bytes: &[u8], expected: u8, drops: &Arc<AtomicUsize>) -> ObservedBytes {
    ObservedBytes {
        // SAFETY: each test retains the matching owner until this value and
        // every core-owned clone have been destroyed.
        bytes: unsafe { std::mem::transmute::<&[u8], &'static [u8]>(bytes) },
        expected,
        drops: drops.clone(),
    }
}

#[test]
fn clean_first_put_preserves_pool_allocation_and_resets_lease() {
    let mut log = TouchLog::default();
    let pointer = log.prepare_first(1, b"first", &[0x41; 128]).as_ptr();
    assert!(log.is_empty());
    assert_eq!(log.accept_first(), 0);
    assert_eq!(log[0].value.as_ptr(), pointer);
    assert!(!log.can_borrow_first());
    log.record_cleanup(true);
    log.recycle();
    assert!(log.can_borrow_first());
    assert_eq!(
        log.prepare_first(2, b"next", &[0x42; 128]).as_ptr(),
        pointer
    );
    log.accept_first();
    log.record_cleanup(true);
}

#[test]
fn repeated_mutations_keep_only_one_immutable_retired_allocation() {
    let mut log = TouchLog::default();
    let drops = Arc::new(AtomicUsize::new(0));
    let bytes = log.prepare_first(1, b"key", &[0x51; 128]);
    let pointer = bytes.as_ptr();
    let borrower = unsafe { observed(bytes, 0x51, &drops) };
    log.accept_first();
    for iteration in 0..128 {
        log.retire_value(0);
        log[0].value.clear();
        log[0].value.resize(256 + iteration, iteration as u8);
        assert_eq!(log.retired.as_ptr(), pointer);
        assert_eq!(log.retired.len(), 128);
        assert!(!log.can_borrow_first());
    }
    drop(borrower);
    assert_eq!(drops.load(Ordering::Relaxed), 1);
    log.record_cleanup(true);
    log.recycle();
}

#[test]
fn prepared_error_keeps_noncanonical_bytes_alive_when_next_touch_reuses_slot() {
    let mut log = TouchLog::default();
    let drops = Arc::new(AtomicUsize::new(0));
    let bytes = log.prepare_first(1, b"rejected", &[0x61; 128]);
    let borrower = unsafe { observed(bytes, 0x61, &drops) };
    // Simulate an operation error before canonical acceptance, followed by
    // another legal operation on the still-active wrapper.
    assert!(log.is_empty());
    let next = log.touch(2, b"accepted");
    log[next].value.extend_from_slice(&[0x62; 160]);
    assert_eq!(log.len(), 1);
    assert_eq!(log[0].key, b"accepted");
    drop(borrower);
    log.record_cleanup(true);
}

#[test]
fn panic_after_actual_core_staging_keeps_prepared_owner_until_abort() {
    let runtime = sto_core::Runtime::new(Default::default()).unwrap();
    let mut worker = runtime.attach().unwrap();
    let drops = Arc::new(AtomicUsize::new(0));
    let cell = sto_core::TxnCell::new(
        &runtime,
        ObservedBytes {
            bytes: &[],
            expected: 0,
            drops: drops.clone(),
        },
    )
    .unwrap();
    let mut transaction = worker.begin().unwrap();
    let mut log = TouchLog::default();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let bytes = log.prepare_first(1, b"key", &[0x71; 128]);
        cell.set(&mut transaction, unsafe { observed(bytes, 0x71, &drops) })
            .unwrap();
        panic!("after staging but before accepting the canonical entry");
    }));
    assert!(result.is_err());
    assert!(log.is_empty());
    transaction.abort();
    assert!(drops.load(Ordering::Relaxed) > 0);
    assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    log.record_cleanup(true);
}

#[test]
fn uncertainty_retains_exact_owner_independently_of_holder_copy() {
    for retire in [false, true] {
        let mut log = TouchLog::default();
        let drops = Arc::new(AtomicUsize::new(0));
        let bytes = log.prepare_first(1, b"key", &[0x81; 128]);
        let pointer = bytes.as_ptr();
        let borrower = unsafe { observed(bytes, 0x81, &drops) };
        log.accept_first();
        if retire {
            log.retire_value(0);
            log[0].value.extend_from_slice(b"new value");
        }
        // The anomalous holder gets independent bytes. Destroying it cannot
        // destroy the original allocation referenced by quarantined core.
        let holder_copy = log[0].value.clone();
        let retained = log.take_unreleased_payload().unwrap();
        assert_eq!(retained.as_ptr(), pointer);
        drop(holder_copy);
        drop(log);
        drop(borrower);
        assert_eq!(drops.load(Ordering::Relaxed), 1);
        drop(retained);
    }
}

#[test]
fn clean_holder_transfer_and_delayed_old_drop_do_not_touch_new_generation() {
    let mut log = TouchLog::default();
    let pointer = log.prepare_first(1, b"key", &[0x91; 128]).as_ptr();
    log.accept_first();
    log.record_cleanup(true);
    let mut holder = Vec::new();
    std::mem::swap(&mut holder, &mut log[0].value);
    assert_eq!(holder.as_ptr(), pointer);
    let mut worker_pool = std::mem::take(&mut log);
    worker_pool.recycle();
    worker_pool.prepare_first(2, b"next", &[0x92; 128]);
    worker_pool.accept_first();
    drop(log);
    assert_eq!(worker_pool[0].value, [0x92; 128]);
    worker_pool.record_cleanup(true);
    drop(worker_pool);
    assert_eq!(holder, [0x91; 128]);
}
