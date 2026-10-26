//! Allocation accounting is a native test; frame ownership also has Miri tests.
#![cfg(not(miri))]
#![allow(
    unsafe_code,
    reason = "test-only transparent System allocator counting"
)]

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use sto_core::{CommitOutcome, Runtime, RuntimeConfig, TxnCell};

thread_local! {
    static ALLOCATIONS: Cell<Option<usize>> = const { Cell::new(None) };
}

struct CountingAllocator;

fn note_allocation() {
    let _ = ALLOCATIONS.try_with(|counter| {
        if let Some(count) = counter.get() {
            counter.set(Some(count + 1));
        }
    });
}

// SAFETY: Every method forwards the allocator's original pointer and layout
// unchanged. The counter neither allocates nor reads the allocation's bytes.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        note_allocation();
        unsafe { System.realloc(pointer, layout, size) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

fn allocations(operation: impl FnOnce()) -> usize {
    struct Reset;
    impl Drop for Reset {
        fn drop(&mut self) {
            ALLOCATIONS.with(|counter| counter.set(None));
        }
    }
    ALLOCATIONS.with(|counter| assert!(counter.replace(Some(0)).is_none()));
    let _reset = Reset;
    operation();
    ALLOCATIONS.with(|counter| counter.get().unwrap())
}

#[test]
fn reused_frames_do_not_allocate_at_transaction_boundaries() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 0_u64).unwrap();
    let mut worker = runtime.attach().unwrap();

    // An empty frame needs no item/lock/index storage, including its first
    // begin. Every frame allocation must already have happened at attach.
    assert_eq!(
        allocations(|| {
            for _ in 0..32 {
                worker.begin().unwrap().commit().unwrap();
                worker.begin().unwrap().abort();
                drop(worker.begin().unwrap());
            }
        }),
        0
    );

    // Fill both item and lock pools before counting. TxnCell intentionally
    // allocates one owned Arc value per set; the core must add no frame Box.
    for value in 0..32 {
        let mut transaction = worker.begin().unwrap();
        cell.set(&mut transaction, value).unwrap();
        transaction.commit().unwrap();
    }
    assert_eq!(
        allocations(|| {
            for value in 0..48 {
                let mut transaction = worker.begin().unwrap();
                cell.set(&mut transaction, value).unwrap();
                match value % 3 {
                    0 => assert!(matches!(
                        transaction.commit().unwrap(),
                        CommitOutcome::Committed(_)
                    )),
                    1 => {
                        transaction.abort();
                    }
                    _ => drop(transaction),
                }
            }
        }),
        48
    );
}
