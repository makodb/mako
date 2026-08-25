use srpc::threading::SpinLock;

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn spin_lock_layout_traits_and_basic_state_are_pinned() {
    assert_eq!(core::mem::size_of::<SpinLock>(), 1_usize);
    assert_eq!(core::mem::align_of::<SpinLock>(), 1_usize);
    assert_send_sync::<SpinLock>();

    let lock = SpinLock::new();
    assert!(!lock.locked_field.load(std::sync::atomic::Ordering::Relaxed));
    lock.locked_field
        .store(true, std::sync::atomic::Ordering::Relaxed);
    lock.unlock();
    assert!(!lock.locked_field.load(std::sync::atomic::Ordering::Relaxed));
}

#[test]
fn pthread_wrapper_signatures_remain_explicitly_unsafe() {
    let _spin_init: unsafe fn(*mut rusty::PthreadSpinlock, i32) = srpc::threading::Pthread_spin_init;
    let _mutex_init: unsafe fn(*mut rusty::PthreadMutex, *const rusty::PthreadMutexAttr) =
        srpc::threading::Pthread_mutex_init;
    let _cond_wait: unsafe fn(*mut rusty::PthreadCond, *mut rusty::PthreadMutex) =
        srpc::threading::Pthread_cond_wait;
}
