//! Canonical Rust owner for the `srpc.threading` pthread wrappers and spin lock.

#![allow(non_camel_case_types, non_snake_case)]

use crate::debugging::verify_at;
use std::sync::Mutex;

/// A synchronized value snapshot shared by transport and caller threads.
/// The lock never escapes an operation, so callers can invoke callbacks or
/// suspend after reading without retaining a borrow into shared state.
pub struct SharedCell<T> {
    value_: Mutex<T>,
}

impl<T> SharedCell<T> {
    pub fn new(value: T) -> Self {
        Self { value_: Mutex::new(value) }
    }

    pub fn set(&self, value: T) {
        *self.value_.lock().unwrap() = value;
    }

    pub fn replace(&self, value: T) -> T {
        std::mem::replace(&mut *self.value_.lock().unwrap(), value)
    }
}

impl<T: Clone> SharedCell<T> {
    pub fn get(&self) -> T {
        self.value_.lock().unwrap().clone()
    }
}

pub type AtomicBool = std::sync::atomic::AtomicBool;
pub type Ordering = std::sync::atomic::Ordering;

// Opaque C pthread bindings. These values are never allocated or passed by
// value in Rust; the wrappers borrow pointers owned by native callers.
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct PthreadSpinlock {
    _opaque: [u8; 0],
}

#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct PthreadMutex {
    _opaque: [u8; 0],
}

#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct PthreadMutexAttr {
    _opaque: [u8; 0],
}

#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct PthreadCond {
    _opaque: [u8; 0],
}

#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct PthreadCondAttr {
    _opaque: [u8; 0],
}

#[allow(unsafe_code)]
unsafe extern "C" {
    fn pthread_spin_init(lock: *mut PthreadSpinlock, pshared: i32) -> i32;
    fn pthread_spin_lock(lock: *mut PthreadSpinlock) -> i32;
    fn pthread_spin_unlock(lock: *mut PthreadSpinlock) -> i32;
    fn pthread_spin_destroy(lock: *mut PthreadSpinlock) -> i32;

    fn pthread_mutex_init(
        mutex: *mut PthreadMutex,
        attr: *const PthreadMutexAttr,
    ) -> i32;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> i32;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> i32;
    fn pthread_mutex_destroy(mutex: *mut PthreadMutex) -> i32;

    fn pthread_cond_init(cond: *mut PthreadCond, attr: *const PthreadCondAttr)
        -> i32;
    fn pthread_cond_destroy(cond: *mut PthreadCond) -> i32;
    fn pthread_cond_signal(cond: *mut PthreadCond) -> i32;
    fn pthread_cond_broadcast(cond: *mut PthreadCond) -> i32;
    fn pthread_cond_wait(cond: *mut PthreadCond, mutex: *mut PthreadMutex) -> i32;

    fn srpc_cpu_pause();
}

/// Initialize caller-owned pthread spin-lock storage.
///
/// # Safety
///
/// `lock` must satisfy `pthread_spin_init`'s pointer and lifetime contract.
#[allow(unsafe_code)]
pub unsafe fn Pthread_spin_init(lock: *mut PthreadSpinlock, pshared: i32) {
    unsafe { verify_at(pthread_spin_init(lock, pshared) == 0, file!(), line!()) };
}

/// Lock a live initialized pthread spin lock.
///
/// # Safety
///
/// `lock` must point to a live initialized pthread spin lock.
#[allow(unsafe_code)]
pub unsafe fn Pthread_spin_lock(lock: *mut PthreadSpinlock) {
    unsafe { verify_at(pthread_spin_lock(lock) == 0, file!(), line!()) };
}

/// Unlock a pthread spin lock held by this thread.
///
/// # Safety
///
/// `lock` must point to a live spin lock held by the current thread.
#[allow(unsafe_code)]
pub unsafe fn Pthread_spin_unlock(lock: *mut PthreadSpinlock) {
    unsafe { verify_at(pthread_spin_unlock(lock) == 0, file!(), line!()) };
}

/// Destroy initialized, unlocked pthread spin-lock storage.
///
/// # Safety
///
/// `lock` must point to an initialized, unlocked pthread spin lock.
#[allow(unsafe_code)]
pub unsafe fn Pthread_spin_destroy(lock: *mut PthreadSpinlock) {
    unsafe { verify_at(pthread_spin_destroy(lock) == 0, file!(), line!()) };
}

/// Initialize caller-owned pthread mutex storage.
///
/// # Safety
///
/// `mutex` and non-null `attr` must satisfy `pthread_mutex_init`'s contract.
#[allow(unsafe_code)]
pub unsafe fn Pthread_mutex_init(
    mutex: *mut PthreadMutex,
    attr: *const PthreadMutexAttr,
) {
    unsafe { verify_at(pthread_mutex_init(mutex, attr) == 0, file!(), line!()) };
}

/// Lock a live initialized pthread mutex.
///
/// # Safety
///
/// `mutex` must point to a live initialized pthread mutex.
#[allow(unsafe_code)]
pub unsafe fn Pthread_mutex_lock(mutex: *mut PthreadMutex) {
    unsafe { verify_at(pthread_mutex_lock(mutex) == 0, file!(), line!()) };
}

/// Unlock a pthread mutex held by this thread.
///
/// # Safety
///
/// `mutex` must point to a live pthread mutex held by this thread.
#[allow(unsafe_code)]
pub unsafe fn Pthread_mutex_unlock(mutex: *mut PthreadMutex) {
    unsafe { verify_at(pthread_mutex_unlock(mutex) == 0, file!(), line!()) };
}

/// Destroy initialized, unlocked pthread mutex storage.
///
/// # Safety
///
/// `mutex` must point to an initialized, unlocked pthread mutex.
#[allow(unsafe_code)]
pub unsafe fn Pthread_mutex_destroy(mutex: *mut PthreadMutex) {
    unsafe { verify_at(pthread_mutex_destroy(mutex) == 0, file!(), line!()) };
}

/// Initialize caller-owned pthread condition-variable storage.
///
/// # Safety
///
/// `cond` and non-null `attr` must satisfy `pthread_cond_init`'s contract.
#[allow(unsafe_code)]
pub unsafe fn Pthread_cond_init(
    cond: *mut PthreadCond,
    attr: *const PthreadCondAttr,
) {
    unsafe { verify_at(pthread_cond_init(cond, attr) == 0, file!(), line!()) };
}

/// Destroy an initialized condition variable with no waiters.
///
/// # Safety
///
/// `cond` must point to an initialized condition variable with no waiters.
#[allow(unsafe_code)]
pub unsafe fn Pthread_cond_destroy(cond: *mut PthreadCond) {
    unsafe { verify_at(pthread_cond_destroy(cond) == 0, file!(), line!()) };
}

/// Signal a live initialized pthread condition variable.
///
/// # Safety
///
/// `cond` must point to a live initialized condition variable.
#[allow(unsafe_code)]
pub unsafe fn Pthread_cond_signal(cond: *mut PthreadCond) {
    unsafe { verify_at(pthread_cond_signal(cond) == 0, file!(), line!()) };
}

/// Broadcast to a live initialized pthread condition variable.
///
/// # Safety
///
/// `cond` must point to a live initialized condition variable.
#[allow(unsafe_code)]
pub unsafe fn Pthread_cond_broadcast(cond: *mut PthreadCond) {
    unsafe { verify_at(pthread_cond_broadcast(cond) == 0, file!(), line!()) };
}

/// Wait on a live condition-variable and mutex pair.
///
/// # Safety
///
/// Both pointers must be live, initialized, and satisfy `pthread_cond_wait`'s
/// locking and lifetime contract.
#[allow(unsafe_code)]
pub unsafe fn Pthread_cond_wait(cond: *mut PthreadCond, mutex: *mut PthreadMutex) {
    unsafe { verify_at(pthread_cond_wait(cond, mutex) == 0, file!(), line!()) };
}

pub fn cpu_pause() {
    #[allow(unsafe_code)]
    unsafe {
        srpc_cpu_pause();
    }
}

#[repr(C)]
pub struct SpinLock {
    pub locked_field: AtomicBool,
}

impl SpinLock {
    #[allow(clippy::new_without_default)]
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

        let mut wait = 1_000_i32;
        while wait > 0_i32 && self.locked_field.load(Ordering::Relaxed) {
            cpu_pause();
            wait -= 1_i32;
        }

        while self
            .locked_field
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            std::thread::sleep(std::time::Duration::from_micros(50_u64));
        }
    }

    pub fn unlock(&self) {
        self.locked_field.store(false, Ordering::Release);
    }
}

/// Spawn an SRPC worker whose uncaught panic terminates the process.
/// Detached reconnect workers must not silently lose a failed task.
pub fn spawn_abort_on_panic<F>(body: F) -> std::thread::JoinHandle<()>
where
    F: FnOnce() + Send + 'static,
{
    std::thread::spawn(move || {
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(body));
        if result.is_err() {
            std::process::abort();
        }
    })
}
