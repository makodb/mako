//! Process-lifetime, thread-affine workers and the per-worker quarantine.
//!
//! The C++ engine binds one STO thread id to each OS thread for the life of
//! the process. This module reproduces that: the first use of a database on a
//! thread attaches a `sto-core` worker context and the shared Masstree worker
//! and leaks both, so their addresses are stable for every later transaction
//! and `MAX_WORKERS` bounds the threads a database may ever see.

use std::cell::{Cell, RefCell};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};

use mako_engine_api::{Error, Result, TestCleanupBoundary, TestCommitObserver, WorkerHealth};
use sto_core::{AttachError, CapacityError, RuntimeId, WorkerContext};

use crate::touched::TouchLog;

/// One leaked `sto-core` worker context with its single-active-transaction
/// guard.
pub(crate) struct WorkerSlot {
    context: NonNull<WorkerContext>,
    runtime: RuntimeId,
    active: Cell<bool>,
    touched: Cell<TouchLog>,
}

impl WorkerSlot {
    /// Reserve the worker for one transaction.
    ///
    /// # Safety
    ///
    /// The caller must call [`Self::release`] exactly once after the returned
    /// borrow's transaction has finished, and must be on the owning thread
    /// (slots are only reachable through the thread-local registry).
    pub(crate) unsafe fn acquire(&self) -> Result<&'static mut WorkerContext> {
        if self.active.replace(true) {
            return Err(Error::TransactionAlreadyActive);
        }
        // SAFETY: the context was leaked at attach and is never freed. The
        // `active` flag guarantees no other live `&mut` exists.
        Ok(unsafe { &mut *self.context.as_ptr() })
    }

    pub(crate) fn release(&self) {
        debug_assert!(self.active.get());
        self.active.set(false);
    }

    pub(crate) fn take_touched(&self) -> TouchLog {
        debug_assert!(self.active.get());
        self.touched.take()
    }

    pub(crate) fn recycle_touched(&self, mut log: TouchLog) {
        log.recycle();
        self.touched.set(log);
    }
}

struct ThreadState {
    masstree: Option<&'static masstree::Worker>,
    slots: Vec<&'static WorkerSlot>,
    poisoned: bool,
    #[cfg(feature = "test-hooks")]
    observer: Option<TestCommitObserver>,
    #[cfg(feature = "test-hooks")]
    install_event: Option<(TestCommitObserver, mako_engine_api::MakoTimestamp)>,
    #[cfg(feature = "test-hooks")]
    armed_cleanup_failure: Option<TestCleanupBoundary>,
}

impl ThreadState {
    fn poison(&mut self) {
        if !self.poisoned {
            self.poisoned = true;
            QUARANTINED_WORKERS.fetch_add(1, Ordering::Relaxed);
        }
    }
}

thread_local! {
    static THREAD: RefCell<ThreadState> = const {
        RefCell::new(ThreadState {
            masstree: None,
            slots: Vec::new(),
            poisoned: false,
            #[cfg(feature = "test-hooks")]
            observer: None,
            #[cfg(feature = "test-hooks")]
            install_event: None,
            #[cfg(feature = "test-hooks")]
            armed_cleanup_failure: None,
        })
    };
}

static QUARANTINED_WORKERS: AtomicU64 = AtomicU64::new(0);

#[cfg(all(test, feature = "test-hooks", not(miri)))]
mod lsan_tests {
    use crate::touched::TouchLog;

    /// The sanitizer runner must observe a leak failure with the two startup
    /// allocation exceptions active. This never runs in ordinary Cargo tests.
    #[test]
    #[ignore = "deliberately lost payload; run only as the LSan negative control"]
    fn lost_transaction_log_is_reported_with_startup_exceptions() {
        let mut log = TouchLog::default();
        let index = log.touch(1, &[0x6b; 53]);
        log[index].value.extend_from_slice(&[0x76; 1009]);
        std::hint::black_box(&log);
        std::mem::forget(log);
    }
}

/// The current thread's attachment to one database.
#[derive(Clone, Copy)]
pub(crate) struct Attachment {
    pub(crate) slot: &'static WorkerSlot,
    pub(crate) worker: &'static masstree::Worker,
}

fn map_attach_error(error: AttachError) -> Error {
    match error {
        AttachError::Capacity(CapacityError::OwnerIdExhausted | CapacityError::WorkerLimit) => {
            Error::ThreadLimit
        }
        AttachError::Capacity(CapacityError::VersionExhausted) => Error::VersionExhausted,
        AttachError::Capacity(_) => Error::OutOfMemory,
        AttachError::InvalidUse(_) => Error::Internal,
        AttachError::Poisoned(_) => Error::WorkerPoisoned,
    }
}

// These two allocation sites implement the documented process-lifetime
// attachment contract. Keep payload allocation outside them so LeakSanitizer
// can exempt only these ownership roots without hiding lost transaction bytes.
#[inline(never)]
fn retain_process_lifetime_native_worker(worker: masstree::Worker) -> &'static masstree::Worker {
    Box::leak(Box::new(worker))
}

#[inline(never)]
fn retain_process_lifetime_worker_slot(slot: WorkerSlot) -> &'static WorkerSlot {
    Box::leak(Box::new(slot))
}

/// Attach the current thread to `runtime`, reusing an earlier attachment.
pub(crate) fn attach(
    runtime: &std::sync::Arc<sto_core::Runtime>,
    directory: &masstree::Runtime,
) -> Result<Attachment> {
    THREAD.with(|state| {
        let mut state = state.borrow_mut();
        if state.poisoned {
            return Err(Error::WorkerPoisoned);
        }
        let worker = match state.masstree {
            Some(worker) => worker,
            None => {
                let worker = directory.attach().map_err(|_| Error::ThreadLimit)?;
                let worker = retain_process_lifetime_native_worker(worker);
                state.masstree = Some(worker);
                worker
            }
        };
        let runtime_id = runtime.id();
        if let Some(slot) = state.slots.iter().find(|slot| slot.runtime == runtime_id) {
            return Ok(Attachment { slot, worker });
        }
        crate::prefetch::initialize();
        let context = match runtime.attach() {
            Ok(context) => context,
            Err(error) => {
                if matches!(error, AttachError::Poisoned(_)) {
                    // THREAD is already borrowed here. Update this state
                    // directly instead of reentering poison_current_worker.
                    state.poison();
                }
                return Err(map_attach_error(error));
            }
        };
        let context = NonNull::from(Box::leak(Box::new(context)));
        let slot = retain_process_lifetime_worker_slot(WorkerSlot {
            context,
            runtime: runtime_id,
            active: Cell::new(false),
            touched: Cell::new(TouchLog::default()),
        });
        state.slots.push(slot);
        Ok(Attachment { slot, worker })
    })
}

/// Retire the current worker permanently after uncertain cleanup.
pub(crate) fn poison_current_worker() {
    THREAD.with(|state| state.borrow_mut().poison());
}

pub(crate) fn current_worker_poisoned() -> bool {
    THREAD.with(|state| state.borrow().poisoned)
}

pub(crate) fn health() -> WorkerHealth {
    THREAD.with(|state| {
        let state = state.borrow();
        if state.poisoned {
            WorkerHealth::Poisoned
        } else if state.slots.is_empty() {
            WorkerHealth::NotAttached
        } else {
            WorkerHealth::Healthy
        }
    })
}

pub(crate) fn quarantined_worker_count() -> u64 {
    QUARANTINED_WORKERS.load(Ordering::Relaxed)
}

/// Reclaim an isolated healthy native fixture's process-lifetime attachments.
///
/// Production workers remain attached for the process lifetime. A short-lived
/// test thread otherwise loses its TLS roots at exit and prevents LeakSanitizer
/// from distinguishing those allocations from a lost transaction buffer.
///
/// # Safety
///
/// Every database, table, transaction, attachment, native scope, and borrowed
/// worker handle created on this fixture thread must already be gone. No
/// other thread may retain any of its databases. This is not recovery from
/// poison, and must never run on a fixture that intentionally quarantines.
#[cfg(all(test, feature = "test-hooks", not(miri)))]
pub(crate) unsafe fn drop_healthy_test_attachments() {
    let (slots, native_worker) = THREAD.with(|state| {
        let mut state = state.borrow_mut();
        assert!(!state.poisoned, "cannot reclaim a quarantined fixture");
        for slot in &state.slots {
            assert!(!slot.active.get(), "a fixture transaction is still active");
            // SAFETY: slots retain their leaked context, and the caller has
            // retired all mutable context borrows before entering teardown.
            let context = unsafe { slot.context.as_ref() };
            assert_eq!(context.runtime().health(), sto_core::RuntimeHealth::Healthy);
        }
        (std::mem::take(&mut state.slots), state.masstree.take())
    });
    for slot in slots {
        // SAFETY: these pointers came from Box::leak in attach. TLS no longer
        // contains them, and the caller excludes every other fixture alias.
        // Drop the context first, including its retained table/item pools.
        let runtime = unsafe { slot.context.as_ref() }.runtime().clone();
        unsafe { drop(Box::from_raw(slot.context.as_ptr())) };
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        unsafe { drop(Box::from_raw(std::ptr::from_ref(slot).cast_mut())) };
    }
    if let Some(native_worker) = native_worker {
        native_worker.quiesce().expect("quiesce a healthy fixture");
        // SAFETY: attach leaked this box, the TLS alias was removed, and all
        // fixture table and transaction borrows ended before this call.
        unsafe { drop(Box::from_raw(std::ptr::from_ref(native_worker).cast_mut())) };
    }
}

#[cfg(feature = "test-hooks")]
pub(crate) fn install_observer(observer: TestCommitObserver) {
    THREAD.with(|state| state.borrow_mut().observer = Some(observer));
}

#[cfg(feature = "test-hooks")]
pub(crate) fn clear_observer() {
    THREAD.with(|state| state.borrow_mut().observer = None);
}

#[cfg(feature = "test-hooks")]
pub(crate) fn prepare_install_event(
    observer: Option<TestCommitObserver>,
    timestamp: Option<mako_engine_api::MakoTimestamp>,
) {
    THREAD.with(|state| state.borrow_mut().install_event = observer.zip(timestamp));
}

#[cfg(feature = "test-hooks")]
pub(crate) fn observe_install(installed: usize) {
    if installed == 1 {
        let event = THREAD.with(|state| state.borrow().install_event);
        if let Some((observer, timestamp)) = event {
            observer(
                mako_engine_api::TestCommitPhase::FirstWriteInstalled,
                Some(timestamp),
            );
        }
    }
}

/// The installed test observer, if any.
#[inline]
pub(crate) fn observer() -> Option<TestCommitObserver> {
    #[cfg(feature = "test-hooks")]
    {
        THREAD.with(|state| state.borrow().observer)
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        None
    }
}

#[cfg(feature = "test-hooks")]
pub(crate) fn arm_cleanup_failure(boundary: TestCleanupBoundary) {
    THREAD.with(|state| state.borrow_mut().armed_cleanup_failure = Some(boundary));
}

#[cfg(feature = "test-hooks")]
pub(crate) fn clear_cleanup_failure() {
    THREAD.with(|state| state.borrow_mut().armed_cleanup_failure = None);
}

/// Consume a one-shot armed cleanup failure at `boundary`.
#[inline]
pub(crate) fn take_cleanup_failure(boundary: TestCleanupBoundary) -> bool {
    #[cfg(feature = "test-hooks")]
    {
        THREAD.with(|state| {
            let mut state = state.borrow_mut();
            if state.armed_cleanup_failure == Some(boundary) {
                state.armed_cleanup_failure = None;
                true
            } else {
                false
            }
        })
    }
    #[cfg(not(feature = "test-hooks"))]
    {
        let _ = boundary;
        false
    }
}
