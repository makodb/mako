use srpc::rpc::heartbeat::{heartbeat_time_us, HeartbeatConfig, HeartbeatManager};
use std::cell::Cell;
use std::rc::Rc;

fn zero_config() -> HeartbeatConfig {
    HeartbeatConfig {
        enabled: false,
        interval_ms: 0,
        timeout_ms: 0,
        max_missed: 0,
    }
}

fn expire_pending(manager: &HeartbeatManager) {
    let timeout_us = (manager.config().timeout_ms as u64) * 1_000;
    manager.last_send_time.set(heartbeat_time_us() - timeout_us);
}

#[test]
fn presets_exactly_match_the_legacy_values() {
    let defaults = HeartbeatConfig::defaults();
    assert!(defaults.enabled);
    assert_eq!(defaults.interval_ms, 10_000);
    assert_eq!(defaults.timeout_ms, 5_000);
    assert_eq!(defaults.max_missed, 3);

    let via_new = HeartbeatConfig::new();
    assert_eq!(via_new.enabled, defaults.enabled);
    assert_eq!(via_new.interval_ms, defaults.interval_ms);
    assert_eq!(via_new.timeout_ms, defaults.timeout_ms);
    assert_eq!(via_new.max_missed, defaults.max_missed);

    let aggressive = HeartbeatConfig::aggressive();
    assert!(aggressive.enabled);
    assert_eq!(aggressive.interval_ms, 5_000);
    assert_eq!(aggressive.timeout_ms, 2_000);
    assert_eq!(aggressive.max_missed, 2);

    let relaxed = HeartbeatConfig::relaxed();
    assert!(relaxed.enabled);
    assert_eq!(relaxed.interval_ms, 30_000);
    assert_eq!(relaxed.timeout_ms, 15_000);
    assert_eq!(relaxed.max_missed, 5);

    let disabled = HeartbeatConfig::disabled();
    assert!(!disabled.enabled);
    assert_eq!(disabled.interval_ms, 0);
    assert_eq!(disabled.timeout_ms, 0);
    assert_eq!(disabled.max_missed, 0);
}

#[test]
fn zero_initialized_config_and_disabled_preset_are_inert() {
    let zeroed = HeartbeatManager::new(&zero_config());
    assert!(!zeroed.should_send_heartbeat());
    zeroed.on_heartbeat_sent();
    assert!(!zeroed.is_pending_pong());
    assert!(!zeroed.check_timeout());

    let disabled = HeartbeatManager::new(&HeartbeatConfig::disabled());
    assert!(!disabled.should_send_heartbeat());
    assert!(!disabled.is_timed_out());
    assert_eq!(disabled.missed_count(), 0);
}

#[test]
fn interval_and_pending_pong_gate_heartbeat_sends() {
    let mut config = HeartbeatConfig::defaults();
    config.interval_ms = 50;
    let manager = HeartbeatManager::new(&config);

    assert!(manager.should_send_heartbeat(), "the first ping is due");
    manager.on_heartbeat_sent();
    assert!(manager.is_pending_pong());
    assert!(!manager.should_send_heartbeat());
    assert_eq!(manager.time_until_next_heartbeat_ms(), 50);

    manager.on_pong_received();
    assert!(!manager.is_pending_pong());
    assert!(!manager.should_send_heartbeat());
    manager.last_send_time.set(heartbeat_time_us() - 50_000);
    assert!(manager.should_send_heartbeat());
    assert_eq!(manager.time_until_next_heartbeat_ms(), 0);
}

#[test]
fn misses_accumulate_and_timeout_callback_fires_once() {
    let mut config = HeartbeatConfig::defaults();
    config.timeout_ms = 10;
    config.max_missed = 2;
    let manager = HeartbeatManager::new(&config);
    let callback_count = Rc::new(Cell::new(0_u32));
    let sink = Rc::clone(&callback_count);
    manager.set_on_timeout(Box::new(move || sink.set(sink.get() + 1)));

    manager.on_heartbeat_sent();
    expire_pending(&manager);
    assert!(!manager.check_timeout());
    assert_eq!(manager.missed_count(), 1);
    assert!(!manager.is_pending_pong());

    manager.on_heartbeat_sent();
    expire_pending(&manager);
    assert!(manager.check_timeout());
    assert_eq!(manager.missed_count(), 2);
    assert!(manager.is_timed_out());
    assert_eq!(callback_count.get(), 1);

    assert!(!manager.check_timeout());
    assert_eq!(callback_count.get(), 1);
}

#[test]
fn pong_reset_and_config_replacement_clear_runtime_state() {
    let mut config = HeartbeatConfig::defaults();
    config.timeout_ms = 0;
    config.max_missed = 1;
    let manager = HeartbeatManager::new(&config);

    manager.on_heartbeat_sent();
    assert!(manager.check_timeout());
    assert!(manager.is_timed_out());
    manager.on_pong_received();
    assert!(!manager.is_timed_out());
    assert_eq!(manager.missed_count(), 0);
    assert!(!manager.is_pending_pong());
    assert_ne!(manager.last_recv_time.get(), 0);

    manager.on_heartbeat_sent();
    let replacement = HeartbeatConfig::relaxed();
    manager.set_config(&replacement);
    assert!(!manager.is_timed_out());
    assert_eq!(manager.missed_count(), 0);
    assert!(!manager.is_pending_pong());
    assert_eq!(manager.last_send_time.get(), 0);
    assert_eq!(manager.last_recv_time.get(), 0);
    assert_eq!(manager.config().interval_ms, 30_000);
}

#[test]
fn replacing_the_callback_discards_the_old_observer() {
    let mut config = HeartbeatConfig::defaults();
    config.timeout_ms = 0;
    config.max_missed = 1;
    let manager = HeartbeatManager::new(&config);
    let first = Rc::new(Cell::new(0_u32));
    let first_sink = Rc::clone(&first);
    manager.set_on_timeout(Box::new(move || first_sink.set(first_sink.get() + 1)));

    let second = Rc::new(Cell::new(0_u32));
    let second_sink = Rc::clone(&second);
    manager.set_on_timeout(Box::new(move || second_sink.set(second_sink.get() + 1)));

    manager.on_heartbeat_sent();
    assert!(manager.check_timeout());
    assert_eq!(first.get(), 0);
    assert_eq!(second.get(), 1);
}
