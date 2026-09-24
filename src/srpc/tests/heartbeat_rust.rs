use srpc::threading::SharedCell as Cell;
use std::mem::{align_of, offset_of, size_of};
use std::sync::Arc as Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use srpc::heartbeat::{
    HeartbeatTimeoutCallback,     heartbeat_time_us, HeartbeatConfig, HeartbeatManager,
};

static NOW_US: AtomicU64 = AtomicU64::new(0);
static CLOCK_TEST_LOCK: Mutex<()> = Mutex::new(());

fn test_now() -> u64 {
    NOW_US.load(Ordering::SeqCst)
}

fn set_now(value: u64) {
    NOW_US.store(value, Ordering::SeqCst);
}

#[test]
fn config_layout_and_callback_thread_traits_are_stable() {
    // Callback invocation is serialized by the manager's mutex.
    macro_rules! assert_not_auto_trait {
        ($type:ty, $auto_trait:ident) => {{
            trait AmbiguousIfImplemented<Marker> {
                fn marker() {}
            }
            impl<T: ?Sized> AmbiguousIfImplemented<()> for T {}
            impl<T: ?Sized + $auto_trait> AmbiguousIfImplemented<u8> for T {}
            let _ = <$type as AmbiguousIfImplemented<_>>::marker;
        }};
    }
    fn assert_send<T: Send>() {}
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send::<HeartbeatTimeoutCallback>();
    assert_not_auto_trait!(HeartbeatTimeoutCallback, Sync);
    assert_send_sync::<HeartbeatManager>();

    assert_eq!(size_of::<HeartbeatConfig>(), 16);
    assert_eq!(align_of::<HeartbeatConfig>(), 4);
    assert_eq!(offset_of!(HeartbeatConfig, enabled), 0);
    assert_eq!(offset_of!(HeartbeatConfig, interval_ms), 4);
    assert_eq!(offset_of!(HeartbeatConfig, timeout_ms), 8);
    assert_eq!(offset_of!(HeartbeatConfig, max_missed), 12);

}

#[test]
fn factories_and_disabled_behavior_are_exact() {
    assert_eq!(HeartbeatConfig::new(), HeartbeatConfig::defaults());
    assert_eq!(
        HeartbeatConfig::defaults(),
        HeartbeatConfig {
            enabled: true,
            interval_ms: 10_000,
            timeout_ms: 5_000,
            max_missed: 3,
        }
    );
    assert_eq!(
        HeartbeatConfig::aggressive(),
        HeartbeatConfig {
            enabled: true,
            interval_ms: 5_000,
            timeout_ms: 2_000,
            max_missed: 2,
        }
    );
    assert_eq!(
        HeartbeatConfig::relaxed(),
        HeartbeatConfig {
            enabled: true,
            interval_ms: 30_000,
            timeout_ms: 15_000,
            max_missed: 5,
        }
    );

    let disabled = HeartbeatManager::new(&HeartbeatConfig::disabled());
    assert!(!disabled.should_send_heartbeat_at(test_now()));
    disabled.on_heartbeat_sent_at(test_now());
    disabled.on_pong_received_at(test_now());
    assert!(!disabled.check_timeout_at(test_now()));
    assert!(!disabled.is_pending_pong());
    assert!(!disabled.is_timed_out());
}

#[test]
fn empty_callback_timeout_is_safe_and_wrapping_elapsed_is_exact() {
    let _clock_guard = CLOCK_TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let config = HeartbeatConfig {
        enabled: true,
        interval_ms: 1,
        timeout_ms: 0,
        max_missed: 1,
    };
    let manager = HeartbeatManager::new(&config);
    assert!(manager.on_timeout.get().lock().unwrap().is_none());

    set_now(u64::MAX - 5);
    manager.on_heartbeat_sent_at(test_now());
    set_now(4);
    assert!(manager.check_timeout_at(test_now()));
    assert!(manager.is_timed_out());
    assert_eq!(manager.missed_count(), 1);
    assert!(!manager.is_pending_pong());

    let wrapping_config = HeartbeatConfig {
        enabled: true,
        interval_ms: 1,
        timeout_ms: 2,
        max_missed: 2,
    };
    let wrapping_manager = HeartbeatManager::new(&wrapping_config);
    wrapping_manager.missed_count_field.set(u32::MAX);
    set_now(u64::MAX - 5);
    wrapping_manager.on_heartbeat_sent_at(test_now());
    // The wrapped delta is exactly 2,000 us: 1,994 - (u64::MAX - 5).
    set_now(1_994);
    assert!(!wrapping_manager.check_timeout_at(test_now()));
    assert_eq!(wrapping_manager.missed_count(), 0);
    assert!(!wrapping_manager.is_timed_out());
    assert!(!wrapping_manager.is_pending_pong());
}

#[test]
fn send_pong_missed_timeout_callback_and_reset_are_exact() {
    let _clock_guard = CLOCK_TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let config = HeartbeatConfig {
        enabled: true,
        interval_ms: 1,
        timeout_ms: 2,
        max_missed: 2,
    };
    let manager = HeartbeatManager::new(&config);
    let calls = Rc::new(Cell::new(0));
    let callback_calls = Rc::clone(&calls);
    manager.set_on_timeout(Some(Box::new(move || {
        callback_calls.set(callback_calls.get() + 1);
    })));

    set_now(1_000_000);
    assert!(heartbeat_time_us() > 0);
    assert!(manager.should_send_heartbeat_at(test_now()));
    assert_eq!(manager.time_until_next_heartbeat_ms_at(test_now()), 0);
    manager.on_heartbeat_sent_at(test_now());
    assert!(manager.is_pending_pong());
    assert!(!manager.should_send_heartbeat_at(test_now()));
    assert!(!manager.check_timeout_at(test_now()));

    set_now(1_001_999);
    assert!(!manager.check_timeout_at(test_now()));
    set_now(1_002_000);
    assert!(!manager.check_timeout_at(test_now()));
    assert_eq!(manager.missed_count(), 1);
    assert!(!manager.is_timed_out());

    set_now(1_003_000);
    assert!(manager.should_send_heartbeat_at(test_now()));
    manager.on_heartbeat_sent_at(test_now());
    set_now(1_005_000);
    assert!(manager.check_timeout_at(test_now()));
    assert_eq!(calls.get(), 1);
    assert_eq!(manager.missed_count(), 2);
    assert!(manager.is_timed_out());
    assert!(!manager.should_send_heartbeat_at(test_now()));

    manager.reset();
    assert_eq!(manager.missed_count(), 0);
    assert!(!manager.is_timed_out());
    assert!(!manager.is_pending_pong());

    set_now(2_000_000);
    manager.on_heartbeat_sent_at(test_now());
    manager.on_pong_received_at(test_now());
    assert_eq!(manager.missed_count(), 0);
    assert!(!manager.is_pending_pong());
    assert!(!manager.is_timed_out());
}

#[test]
fn set_config_resets_state() {
    let _clock_guard = CLOCK_TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let manager = HeartbeatManager::new(&HeartbeatConfig::aggressive());
    set_now(42);
    manager.on_heartbeat_sent_at(test_now());
    manager.missed_count_field.set(7);
    manager.timed_out.set(true);

    let relaxed = HeartbeatConfig::relaxed();
    manager.set_config(&relaxed);
    assert_eq!(manager.config(), relaxed);
    assert_eq!(manager.last_send_time.get(), 0);
    assert_eq!(manager.last_recv_time.get(), 0);
    assert_eq!(manager.missed_count(), 0);
    assert!(!manager.is_pending_pong());
    assert!(!manager.is_timed_out());
}
