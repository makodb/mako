#![cfg(feature = "test-support")]

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::hint::black_box;

struct TrackingAllocator;

thread_local! {
    static ENABLED: Cell<bool> = const { Cell::new(false) };
    static ALLOCATIONS: Cell<usize> = const { Cell::new(0) };
    static DEALLOCATIONS: Cell<usize> = const { Cell::new(0) };
}

unsafe impl GlobalAlloc for TrackingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // SAFETY: this wrapper delegates the exact request to the system
        // allocator and only updates allocation-free thread-local counters.
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            ENABLED.with(|enabled| {
                if enabled.get() {
                    ALLOCATIONS.with(|count| count.set(count.get() + 1));
                }
            });
        }
        pointer
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        ENABLED.with(|enabled| {
            if enabled.get() {
                DEALLOCATIONS.with(|count| count.set(count.get() + 1));
            }
        });
        // SAFETY: `pointer` and `layout` came from the delegated system
        // allocator call above.
        unsafe { System.dealloc(pointer, layout) };
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        // SAFETY: this wrapper delegates the exact request to the system
        // allocator and only updates allocation-free thread-local counters.
        let pointer = unsafe { System.alloc_zeroed(layout) };
        if !pointer.is_null() {
            ENABLED.with(|enabled| {
                if enabled.get() {
                    ALLOCATIONS.with(|count| count.set(count.get() + 1));
                }
            });
        }
        pointer
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        // SAFETY: this wrapper delegates the exact request to the system
        // allocator and only updates allocation-free thread-local counters.
        let replacement = unsafe { System.realloc(pointer, layout, new_size) };
        if !replacement.is_null() {
            ENABLED.with(|enabled| {
                if enabled.get() {
                    ALLOCATIONS.with(|count| count.set(count.get() + 1));
                    DEALLOCATIONS.with(|count| count.set(count.get() + 1));
                }
            });
        }
        replacement
    }
}

#[global_allocator]
static ALLOCATOR: TrackingAllocator = TrackingAllocator;

fn begin_tracking() {
    ALLOCATIONS.with(|count| count.set(0));
    DEALLOCATIONS.with(|count| count.set(0));
    ENABLED.with(|enabled| {
        assert!(!enabled.replace(true), "nested allocation audit");
    });
}

fn end_tracking() {
    ENABLED.with(|enabled| {
        assert!(enabled.replace(false), "allocation audit was not active");
    });
}

fn counts() -> (usize, usize) {
    (ALLOCATIONS.with(Cell::get), DEALLOCATIONS.with(Cell::get))
}

#[test]
fn detached_bind_is_allocation_free_and_the_tripwire_is_live() {
    begin_tracking();
    let allocation = black_box(Box::new(7_u64));
    black_box(&allocation);
    drop(allocation);
    end_tracking();
    let deliberate = counts();
    assert!(
        deliberate.0 > 0 && deliberate.1 > 0,
        "the test allocator did not observe a deliberate heap round trip"
    );

    mako_cache::test_support::probe_detached_bind(begin_tracking, end_tracking);
    assert_eq!(
        counts(),
        (0, 0),
        "DetachedPermit::bind allocated or freed memory while the native hook would hold locks"
    );
}
