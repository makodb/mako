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

/// One leaked `sto-core` worker context with its single-active-transaction
/// guard.
pub(crate) struct WorkerSlot {
    context: NonNull<WorkerContext>,
    runtime: RuntimeId,
    active: Cell<bool>,
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
}

struct ThreadState {
    masstree: Option<&'static masstree::Worker>,
    slots: Vec<&'static WorkerSlot>,
    poisoned: bool,
    #[cfg(feature = "test-hooks")]
    observer: Option<TestCommitObserver>,
    #[cfg(feature = "test-hooks")]
    armed_cleanup_failure: Option<TestCleanupBoundary>,
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
            armed_cleanup_failure: None,
        })
    };
}

static QUARANTINED_WORKERS: AtomicU64 = AtomicU64::new(0);

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
        AttachError::Capacity(_) => Error::OutOfMemory,
        AttachError::InvalidUse(_) => Error::Internal,
        AttachError::Poisoned(_) => Error::WorkerPoisoned,
    }
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
                let worker: &'static masstree::Worker = Box::leak(Box::new(worker));
                state.masstree = Some(worker);
                worker
            }
        };
        let runtime_id = runtime.id();
        if let Some(slot) = state.slots.iter().find(|slot| slot.runtime == runtime_id) {
            return Ok(Attachment { slot, worker });
        }
        let context = runtime.attach().map_err(map_attach_error)?;
        let context = NonNull::from(Box::leak(Box::new(context)));
        let slot: &'static WorkerSlot = Box::leak(Box::new(WorkerSlot {
            context,
            runtime: runtime_id,
            active: Cell::new(false),
        }));
        state.slots.push(slot);
        Ok(Attachment { slot, worker })
    })
}

/// Retire the current worker permanently after uncertain cleanup.
pub(crate) fn poison_current_worker() {
    THREAD.with(|state| {
        let mut state = state.borrow_mut();
        if !state.poisoned {
            state.poisoned = true;
            QUARANTINED_WORKERS.fetch_add(1, Ordering::Relaxed);
        }
    });
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

#[cfg(feature = "test-hooks")]
pub(crate) fn install_observer(observer: TestCommitObserver) {
    THREAD.with(|state| state.borrow_mut().observer = Some(observer));
}

#[cfg(feature = "test-hooks")]
pub(crate) fn clear_observer() {
    THREAD.with(|state| state.borrow_mut().observer = None);
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
