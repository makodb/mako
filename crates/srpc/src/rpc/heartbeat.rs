//! Single-connection heartbeat policy.
//!
//! Names, fields, presets, and behavior intentionally match the
//! `rrr.heartbeat` C++ module.  This file is also intended to be the source
//! from which that module is generated.

use crate::base::monotonic::monotonic_time_us;
use std::cell::{Cell, RefCell};

/// Monotonic microseconds from the crate's shared nonzero epoch.
pub fn heartbeat_time_us() -> u64 {
    monotonic_time_us()
}

/// Callback fired once when the missed-heartbeat limit is reached.
pub type HeartbeatTimeoutCallback = Box<dyn FnMut()>;

pub struct HeartbeatConfig {
    pub enabled: bool,
    pub interval_ms: u32,
    pub timeout_ms: u32,
    pub max_missed: u32,
}

impl Copy for HeartbeatConfig {}

impl Clone for HeartbeatConfig {
    fn clone(&self) -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: self.enabled,
            interval_ms: self.interval_ms,
            timeout_ms: self.timeout_ms,
            max_missed: self.max_missed,
        }
    }
}

impl HeartbeatConfig {
    pub fn new() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 10_000,
            timeout_ms: 5_000,
            max_missed: 3,
        }
    }

    pub fn defaults() -> HeartbeatConfig {
        HeartbeatConfig::new()
    }

    pub fn aggressive() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 5_000,
            timeout_ms: 2_000,
            max_missed: 2,
        }
    }

    pub fn relaxed() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 30_000,
            timeout_ms: 15_000,
            max_missed: 5,
        }
    }

    pub fn disabled() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: false,
            interval_ms: 0,
            timeout_ms: 0,
            max_missed: 0,
        }
    }
}

/// Heartbeat state for one connection.
///
/// `Option<Box<dyn FnMut()>>` is the valid-Rust spelling of the nullable
/// callback.  The C++ consumer lowering represents it directly as nullable
/// `rusty::Function<void()>`, without changing the legacy layout.
pub struct HeartbeatManager {
    pub config_field: Cell<HeartbeatConfig>,
    pub last_send_time: Cell<u64>,
    pub last_recv_time: Cell<u64>,
    pub missed_count_field: Cell<u32>,
    pub pending_pong: Cell<bool>,
    pub timed_out: Cell<bool>,
    pub on_timeout: RefCell<Option<Box<dyn FnMut()>>>,
}

impl HeartbeatManager {
    pub fn new(config: &self::HeartbeatConfig) -> HeartbeatManager {
        HeartbeatManager {
            config_field: Cell::new(config.clone()),
            last_send_time: Cell::new(0),
            last_recv_time: Cell::new(0),
            missed_count_field: Cell::new(0),
            pending_pong: Cell::new(false),
            timed_out: Cell::new(false),
            on_timeout: RefCell::new(None),
        }
    }

    pub fn set_config(&self, config: &self::HeartbeatConfig) {
        self.config_field.set(config.clone());
        self.reset();
    }

    pub fn set_on_timeout(&self, callback: self::HeartbeatTimeoutCallback) {
        self.on_timeout.replace(Some(callback));
    }

    pub fn should_send_heartbeat(&self) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if self.pending_pong.get() {
            return false;
        }

        let now = heartbeat_time_us();
        let last = self.last_send_time.get();
        let interval_us = (self.config_field.get().interval_ms as u64) * 1_000;
        now - last >= interval_us
    }

    pub fn on_heartbeat_sent(&self) {
        if !self.config_field.get().enabled {
            return;
        }
        self.last_send_time.set(heartbeat_time_us());
        self.pending_pong.set(true);
    }

    pub fn on_pong_received(&self) {
        if !self.config_field.get().enabled {
            return;
        }
        self.last_recv_time.set(heartbeat_time_us());
        self.pending_pong.set(false);
        self.missed_count_field.set(0);
        self.timed_out.set(false);
    }

    pub fn check_timeout(&self) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if !self.pending_pong.get() {
            return false;
        }

        let now = heartbeat_time_us();
        let sent = self.last_send_time.get();
        let timeout_us = (self.config_field.get().timeout_ms as u64) * 1_000;

        if now - sent >= timeout_us {
            self.pending_pong.set(false);
            let count = self.missed_count_field.get() + 1;
            self.missed_count_field.set(count);

            if count >= self.config_field.get().max_missed {
                self.timed_out.set(true);
                let mut callback_slot = self.on_timeout.borrow_mut();
                if let Some(callback) = callback_slot.as_mut() {
                    callback();
                }
                return true;
            }
        }
        false
    }

    pub fn time_until_next_heartbeat_ms(&self) -> u32 {
        if !self.config_field.get().enabled || self.timed_out.get() || self.pending_pong.get() {
            return self.config_field.get().interval_ms;
        }

        let now = heartbeat_time_us();
        let last = self.last_send_time.get();
        let interval_us = (self.config_field.get().interval_ms as u64) * 1_000;

        if now - last >= interval_us {
            return 0;
        }
        ((interval_us - (now - last)) / 1_000) as u32
    }

    pub fn is_timed_out(&self) -> bool {
        self.timed_out.get()
    }

    pub fn missed_count(&self) -> u32 {
        self.missed_count_field.get()
    }

    pub fn is_pending_pong(&self) -> bool {
        self.pending_pong.get()
    }

    pub fn reset(&self) {
        self.last_send_time.set(0);
        self.last_recv_time.set(0);
        self.missed_count_field.set(0);
        self.pending_pong.set(false);
        self.timed_out.set(false);
    }

    pub fn config(&self) -> HeartbeatConfig {
        self.config_field.get()
    }
}
