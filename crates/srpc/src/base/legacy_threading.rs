//! Legacy `rrr.threading` compatibility surface.
//!
//! The pthread helpers deliberately remain thin, verify-checked pass-throughs:
//! callers own the pthread object storage, libc owns every lock/wait operation,
//! and a nonzero pthread status is fatal through the imported legacy
//! `rrr::verify` contract.  Native Rust represents the otherwise opaque
//! pthread pointees as `c_void`; the C++ consumer profile maps the private
//! aliases below back to the exact platform pthread typedefs and injects
//! `<pthread.h>` in the global module fragment.
//!
//! `SpinLock` preserves the live C++ algorithm exactly: one strong acquire
//! attempt, at most 1000 relaxed-load pause iterations, then weak acquire
//! attempts separated by the runtime's one-shot 50-microsecond sleep.  The
//! release store is unchanged.  Its field name and visibility intentionally
//! match the current generated aggregate surface.
//!
//! Historical APIs deleted before this owner (`Pthread_create`/
//! `Pthread_join`, `Lockable`, `SpinMutex`, `SpinCondVar`, `Queue`,
//! `ThreadPool`, and `RunLater`) stay deleted.  Their removals were accompanied
//! by closed-world consumer migrations; recreating them here would reintroduce
//! an obsolete API rather than preserve the module being replaced.

#![allow(non_snake_case)]
#![allow(unsafe_code)]
#![allow(unused_unsafe)]

use cpp::rrr::debugging as cpp_debugging;
use cpp::rusty::sys::time as cpp_time;
// These are public because the legacy `export namespace rrr` made its two
// using-declarations reachable as `rrr::AtomicBool` and `rrr::Ordering`.
pub use std::sync::atomic::{AtomicBool, Ordering};

type LegacyPthreadSpinlock = core::ffi::c_void;
type LegacyPthreadMutex = core::ffi::c_void;
type LegacyPthreadMutexAttr = core::ffi::c_void;
type LegacyPthreadCond = core::ffi::c_void;
type LegacyPthreadCondAttr = core::ffi::c_void;

mod threading_ffi {
    use super::{
        LegacyPthreadCond, LegacyPthreadCondAttr, LegacyPthreadMutex, LegacyPthreadMutexAttr,
        LegacyPthreadSpinlock,
    };

    extern "C" {
        pub(super) fn pthread_spin_init(lock: *mut LegacyPthreadSpinlock, pshared: i32) -> i32;
        pub(super) fn pthread_spin_lock(lock: *mut LegacyPthreadSpinlock) -> i32;
        pub(super) fn pthread_spin_unlock(lock: *mut LegacyPthreadSpinlock) -> i32;
        pub(super) fn pthread_spin_destroy(lock: *mut LegacyPthreadSpinlock) -> i32;

        pub(super) fn pthread_mutex_init(
            mutex: *mut LegacyPthreadMutex,
            attr: *const LegacyPthreadMutexAttr,
        ) -> i32;
        pub(super) fn pthread_mutex_lock(mutex: *mut LegacyPthreadMutex) -> i32;
        pub(super) fn pthread_mutex_unlock(mutex: *mut LegacyPthreadMutex) -> i32;
        pub(super) fn pthread_mutex_destroy(mutex: *mut LegacyPthreadMutex) -> i32;

        pub(super) fn pthread_cond_init(
            cond: *mut LegacyPthreadCond,
            attr: *const LegacyPthreadCondAttr,
        ) -> i32;
        pub(super) fn pthread_cond_destroy(cond: *mut LegacyPthreadCond) -> i32;
        pub(super) fn pthread_cond_signal(cond: *mut LegacyPthreadCond) -> i32;
        pub(super) fn pthread_cond_broadcast(cond: *mut LegacyPthreadCond) -> i32;
        pub(super) fn pthread_cond_wait(
            cond: *mut LegacyPthreadCond,
            mutex: *mut LegacyPthreadMutex,
        ) -> i32;

        pub(super) fn srpc_cpu_pause();
    }
}

/// Initialize caller-owned pthread spin-lock storage and verify libc's status.
pub fn Pthread_spin_init(lock: *mut LegacyPthreadSpinlock, pshared: i32) {
    let status = unsafe { threading_ffi::pthread_spin_init(lock, pshared) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Acquire a caller-owned pthread spin lock and verify libc's status.
pub fn Pthread_spin_lock(lock: *mut LegacyPthreadSpinlock) {
    let status = unsafe { threading_ffi::pthread_spin_lock(lock) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Release a caller-owned pthread spin lock and verify libc's status.
pub fn Pthread_spin_unlock(lock: *mut LegacyPthreadSpinlock) {
    let status = unsafe { threading_ffi::pthread_spin_unlock(lock) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Destroy caller-owned pthread spin-lock storage and verify libc's status.
pub fn Pthread_spin_destroy(lock: *mut LegacyPthreadSpinlock) {
    let status = unsafe { threading_ffi::pthread_spin_destroy(lock) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Initialize caller-owned pthread mutex storage and verify libc's status.
pub fn Pthread_mutex_init(mutex: *mut LegacyPthreadMutex, attr: *const LegacyPthreadMutexAttr) {
    let status = unsafe { threading_ffi::pthread_mutex_init(mutex, attr) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Acquire a caller-owned pthread mutex and verify libc's status.
pub fn Pthread_mutex_lock(mutex: *mut LegacyPthreadMutex) {
    let status = unsafe { threading_ffi::pthread_mutex_lock(mutex) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Release a caller-owned pthread mutex and verify libc's status.
pub fn Pthread_mutex_unlock(mutex: *mut LegacyPthreadMutex) {
    let status = unsafe { threading_ffi::pthread_mutex_unlock(mutex) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Destroy caller-owned pthread mutex storage and verify libc's status.
pub fn Pthread_mutex_destroy(mutex: *mut LegacyPthreadMutex) {
    let status = unsafe { threading_ffi::pthread_mutex_destroy(mutex) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Initialize caller-owned pthread condition-variable storage.
pub fn Pthread_cond_init(cond: *mut LegacyPthreadCond, attr: *const LegacyPthreadCondAttr) {
    let status = unsafe { threading_ffi::pthread_cond_init(cond, attr) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Destroy caller-owned pthread condition-variable storage.
pub fn Pthread_cond_destroy(cond: *mut LegacyPthreadCond) {
    let status = unsafe { threading_ffi::pthread_cond_destroy(cond) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Wake one pthread condition-variable waiter and verify libc's status.
pub fn Pthread_cond_signal(cond: *mut LegacyPthreadCond) {
    let status = unsafe { threading_ffi::pthread_cond_signal(cond) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Wake all pthread condition-variable waiters and verify libc's status.
pub fn Pthread_cond_broadcast(cond: *mut LegacyPthreadCond) {
    let status = unsafe { threading_ffi::pthread_cond_broadcast(cond) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Atomically release `mutex`, wait on `cond`, and reacquire `mutex`.
pub fn Pthread_cond_wait(cond: *mut LegacyPthreadCond, mutex: *mut LegacyPthreadMutex) {
    let status = unsafe { threading_ffi::pthread_cond_wait(cond, mutex) };
    unsafe { cpp_debugging::verify(&(status == 0_i32)) };
}

/// Execute the existing architecture-specific pause/yield C seam.
pub fn cpu_pause() {
    unsafe { threading_ffi::srpc_cpu_pause() };
}

/// Atomic-flag busy-wait lock used by two debug-lock consumers.
pub struct SpinLock {
    pub locked_field: AtomicBool,
}

impl SpinLock {
    pub fn new() -> SpinLock {
        SpinLock {
            locked_field: AtomicBool::new(false),
        }
    }

    pub fn lock(&self) {
        if self
            .locked_field
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            return;
        }

        let mut wait: i32 = 1000_i32;
        while wait > 0_i32 && self.locked_field.load(Ordering::Relaxed) {
            cpu_pause();
            wait -= 1_i32;
        }

        while self
            .locked_field
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            unsafe { cpp_time::sleep_us(50_u64) };
        }
    }

    pub fn unlock(&self) {
        self.locked_field.store(false, Ordering::Release);
    }
}

// Cargo-only definitions for the reserved `cpp::` imports.  The C++ consumer
// suppresses this shim and resolves both functions through a fail-closed
// module-local symbol index.
mod cpp {
    pub mod rrr {
        pub mod debugging {
            pub fn verify(expression: &bool) {
                assert!(*expression);
            }
        }
    }

    pub mod rusty {
        pub mod sys {
            pub mod time {
                pub fn sleep_us(microseconds: u64) {
                    std::thread::sleep(std::time::Duration::from_micros(microseconds));
                }
            }
        }
    }
}
