use std::cell::Cell;
use std::mem::{align_of, offset_of, size_of};
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use rrr::heartbeat::{
    heartbeat_time_us, HeartbeatConfig, HeartbeatManager, HeartbeatTimeoutCallback,
};

static NOW_US: AtomicU64 = AtomicU64::new(0);
static CLOCK_TEST_LOCK: Mutex<()> = Mutex::new(());

// Link-time test double for circuit_breaker's already-audited terminal C seam.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_clock_monotonic_us() -> u64 {
    NOW_US.load(Ordering::SeqCst)
}

fn set_now(value: u64) {
    NOW_US.store(value, Ordering::SeqCst);
}

#[test]
fn layouts_and_public_callback_type_match_cpp() {
    // Compile-time negative assertions: the mutable callback and its owner are
    // neither Send nor Sync unless the erased trait object promises the bounds.
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
    assert_not_auto_trait!(HeartbeatTimeoutCallback, Send);
    assert_not_auto_trait!(HeartbeatTimeoutCallback, Sync);
    assert_not_auto_trait!(HeartbeatManager, Send);
    assert_not_auto_trait!(HeartbeatManager, Sync);

    assert_eq!(size_of::<HeartbeatConfig>(), 16);
    assert_eq!(align_of::<HeartbeatConfig>(), 4);
    assert_eq!(offset_of!(HeartbeatConfig, enabled), 0);
    assert_eq!(offset_of!(HeartbeatConfig, interval_ms), 4);
    assert_eq!(offset_of!(HeartbeatConfig, timeout_ms), 8);
    assert_eq!(offset_of!(HeartbeatConfig, max_missed), 12);

    assert_eq!(size_of::<HeartbeatTimeoutCallback>(), 48);
    assert_eq!(align_of::<HeartbeatTimeoutCallback>(), 16);
    assert_eq!(size_of::<HeartbeatManager>(), 112);
    assert_eq!(align_of::<HeartbeatManager>(), 16);
    assert_eq!(offset_of!(HeartbeatManager, config_field), 0);
    assert_eq!(offset_of!(HeartbeatManager, last_send_time), 16);
    assert_eq!(offset_of!(HeartbeatManager, last_recv_time), 24);
    assert_eq!(offset_of!(HeartbeatManager, missed_count_field), 32);
    assert_eq!(offset_of!(HeartbeatManager, pending_pong), 36);
    assert_eq!(offset_of!(HeartbeatManager, timed_out), 37);
    assert_eq!(offset_of!(HeartbeatManager, on_timeout), 48);
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
    assert!(!disabled.should_send_heartbeat());
    disabled.on_heartbeat_sent();
    disabled.on_pong_received();
    assert!(!disabled.check_timeout());
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
    assert!(manager.on_timeout.borrow().is_empty());

    set_now(u64::MAX - 5);
    manager.on_heartbeat_sent();
    set_now(4);
    assert!(manager.check_timeout());
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
    wrapping_manager.on_heartbeat_sent();
    // The wrapped delta is exactly 2,000 us: 1,994 - (u64::MAX - 5).
    set_now(1_994);
    assert!(!wrapping_manager.check_timeout());
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
    manager.set_on_timeout(HeartbeatTimeoutCallback::from_callable(move || {
        callback_calls.set(callback_calls.get() + 1);
    }));

    set_now(1_000_000);
    assert_eq!(heartbeat_time_us(), 1_000_000);
    assert!(manager.should_send_heartbeat());
    assert_eq!(manager.time_until_next_heartbeat_ms(), 0);
    manager.on_heartbeat_sent();
    assert!(manager.is_pending_pong());
    assert!(!manager.should_send_heartbeat());
    assert!(!manager.check_timeout());

    set_now(1_001_999);
    assert!(!manager.check_timeout());
    set_now(1_002_000);
    assert!(!manager.check_timeout());
    assert_eq!(manager.missed_count(), 1);
    assert!(!manager.is_timed_out());

    set_now(1_003_000);
    assert!(manager.should_send_heartbeat());
    manager.on_heartbeat_sent();
    set_now(1_005_000);
    assert!(manager.check_timeout());
    assert_eq!(calls.get(), 1);
    assert_eq!(manager.missed_count(), 2);
    assert!(manager.is_timed_out());
    assert!(!manager.should_send_heartbeat());

    manager.reset();
    assert_eq!(manager.missed_count(), 0);
    assert!(!manager.is_timed_out());
    assert!(!manager.is_pending_pong());

    set_now(2_000_000);
    manager.on_heartbeat_sent();
    manager.on_pong_received();
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
    manager.on_heartbeat_sent();
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
