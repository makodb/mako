use srpc::base::legacy_threading::{
    cpu_pause, AtomicBool as LegacyAtomicBool, Ordering as LegacyOrdering, Pthread_cond_broadcast,
    Pthread_cond_destroy, Pthread_cond_init, Pthread_cond_signal, Pthread_cond_wait,
    Pthread_mutex_destroy, Pthread_mutex_init, Pthread_mutex_lock, Pthread_mutex_unlock,
    Pthread_spin_destroy, Pthread_spin_init, Pthread_spin_lock, Pthread_spin_unlock, SpinLock,
};
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

static CPU_PAUSES: AtomicUsize = AtomicUsize::new(0_usize);
static TEST_LOCK: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn srpc_cpu_pause() {
    CPU_PAUSES.fetch_add(1_usize, Ordering::Relaxed);
    std::hint::spin_loop();
}

// The native tests intentionally treat pthread objects as opaque.  The C++
// compile probe checks their exact nominal types and sizes; this oversized,
// strongly aligned storage only lets the Rust-side test exercise libc's real
// lock/wait behavior without reproducing a platform pthread layout in Rust.
#[repr(C, align(64))]
struct OpaquePthread {
    bytes: UnsafeCell<[u8; 128]>,
}

impl OpaquePthread {
    fn zeroed() -> OpaquePthread {
        OpaquePthread {
            bytes: UnsafeCell::new([0_u8; 128]),
        }
    }

    fn as_mut_void(&self) -> *mut c_void {
        self.bytes.get().cast::<c_void>()
    }
}

#[test]
fn pthread_spin_wrappers_delegate_to_real_libc_objects() {
    let storage = OpaquePthread::zeroed();
    let lock = storage.as_mut_void();

    Pthread_spin_init(lock, 0_i32);
    Pthread_spin_lock(lock);
    Pthread_spin_unlock(lock);
    Pthread_spin_destroy(lock);
}

#[test]
fn pthread_mutex_and_condvar_preserve_release_wait_reacquire() {
    let mutex = Box::new(OpaquePthread::zeroed());
    let cond = Box::new(OpaquePthread::zeroed());
    let waiter_entered = Arc::new(AtomicBool::new(false));
    let predicate = Arc::new(AtomicBool::new(false));

    Pthread_mutex_init(mutex.as_mut_void(), ptr::null());
    Pthread_cond_init(cond.as_mut_void(), ptr::null());

    let waiter_mutex = mutex.as_mut_void() as usize;
    let waiter_cond = cond.as_mut_void() as usize;
    let waiter_entered_copy = Arc::clone(&waiter_entered);
    let predicate_copy = Arc::clone(&predicate);
    let waiter = std::thread::spawn(move || {
        let waiter_mutex = waiter_mutex as *mut c_void;
        let waiter_cond = waiter_cond as *mut c_void;
        Pthread_mutex_lock(waiter_mutex);
        waiter_entered_copy.store(true, Ordering::Release);
        while !predicate_copy.load(Ordering::Acquire) {
            Pthread_cond_wait(waiter_cond, waiter_mutex);
        }
        Pthread_mutex_unlock(waiter_mutex);
    });

    let deadline = Instant::now() + Duration::from_secs(2);
    while !waiter_entered.load(Ordering::Acquire) {
        assert!(
            Instant::now() < deadline,
            "waiter did not acquire the mutex"
        );
        std::thread::yield_now();
    }

    Pthread_mutex_lock(mutex.as_mut_void());
    predicate.store(true, Ordering::Release);
    Pthread_cond_signal(cond.as_mut_void());
    Pthread_mutex_unlock(mutex.as_mut_void());
    waiter.join().unwrap();

    Pthread_cond_broadcast(cond.as_mut_void());
    Pthread_cond_destroy(cond.as_mut_void());
    Pthread_mutex_destroy(mutex.as_mut_void());
}

#[test]
fn cpu_pause_delegates_to_the_architecture_seam() {
    let _guard = TEST_LOCK.lock().unwrap();
    CPU_PAUSES.store(0_usize, Ordering::Relaxed);
    cpu_pause();
    assert_eq!(CPU_PAUSES.load(Ordering::Relaxed), 1_usize);
}

#[test]
fn spin_lock_fast_path_excludes_and_release_reopens() {
    let lock = SpinLock::new();
    assert!(!lock.locked_field.load(Ordering::Relaxed));

    lock.lock();
    assert!(lock.locked_field.load(Ordering::Relaxed));
    assert!(
        lock.locked_field
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err(),
        "a held lock must exclude a second acquisition"
    );

    lock.unlock();
    assert!(!lock.locked_field.load(Ordering::Relaxed));
    lock.lock();
    lock.unlock();
}

#[test]
fn contended_spin_lock_pauses_exactly_one_thousand_times_before_sleeping() {
    let _guard = TEST_LOCK.lock().unwrap();
    CPU_PAUSES.store(0_usize, Ordering::Relaxed);

    let lock = Arc::new(SpinLock::new());
    lock.lock();
    let waiter_lock = Arc::clone(&lock);
    let acquired = Arc::new(AtomicBool::new(false));
    let acquired_copy = Arc::clone(&acquired);
    let waiter = std::thread::spawn(move || {
        waiter_lock.lock();
        acquired_copy.store(true, Ordering::Release);
        waiter_lock.unlock();
    });

    let deadline = Instant::now() + Duration::from_secs(2);
    while CPU_PAUSES.load(Ordering::Acquire) < 1000_usize {
        assert!(
            Instant::now() < deadline,
            "waiter did not reach the sleep path"
        );
        std::thread::yield_now();
    }
    assert_eq!(CPU_PAUSES.load(Ordering::Relaxed), 1000_usize);
    assert!(!acquired.load(Ordering::Acquire));

    lock.unlock();
    waiter.join().unwrap();
    assert!(acquired.load(Ordering::Acquire));
    assert_eq!(CPU_PAUSES.load(Ordering::Relaxed), 1000_usize);
}

#[test]
fn current_surface_and_audited_historical_deletions_are_explicit() {
    let owner = include_str!("../src/base/legacy_threading.rs");

    assert_eq!(owner.matches("pub fn Pthread_").count(), 13);
    assert_eq!(owner.matches("pub fn cpu_pause").count(), 1);
    assert_eq!(owner.matches("pub struct SpinLock").count(), 1);
    assert_eq!(LegacyOrdering::Acquire, Ordering::Acquire);
    let exported_atomic = LegacyAtomicBool::new(false);
    assert!(!exported_atomic.load(LegacyOrdering::Relaxed));

    for retired in [
        "pub fn Pthread_create",
        "pub fn Pthread_join",
        "pub struct Lockable",
        "pub struct SpinMutex",
        "pub struct SpinCondVar",
        "pub struct ThreadPool",
        "pub struct RunLater",
    ] {
        assert!(
            !owner.contains(retired),
            "retired API resurfaced: {retired}"
        );
    }
}
