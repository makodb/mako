// Canonical Rust source for the srpc.heartbeat module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::{Cell, RefCell};

use crate::circuit_breaker::current_time_us;

pub fn heartbeat_time_us() -> u64 {
    current_time_us()
}

pub type HeartbeatTimeoutCallback = rusty::Function<dyn FnMut()>;

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(C)]
pub struct HeartbeatConfig {
    pub enabled: bool,
    pub interval_ms: u32,
    pub timeout_ms: u32,
    pub max_missed: u32,
}

impl HeartbeatConfig {
    #[allow(clippy::new_without_default)]
    pub fn new() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 10000u32,
            timeout_ms: 5000u32,
            max_missed: 3u32,
        }
    }

    pub fn defaults() -> HeartbeatConfig {
        HeartbeatConfig::new()
    }

    pub fn aggressive() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 5000u32,
            timeout_ms: 2000u32,
            max_missed: 2u32,
        }
    }

    pub fn relaxed() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 30000u32,
            timeout_ms: 15000u32,
            max_missed: 5u32,
        }
    }

    pub fn disabled() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: false,
            interval_ms: 0u32,
            timeout_ms: 0u32,
            max_missed: 0u32,
        }
    }
}

#[repr(C)]
pub struct HeartbeatManager {
    pub config_field: Cell<HeartbeatConfig>,
    pub last_send_time: Cell<u64>,
    pub last_recv_time: Cell<u64>,
    pub missed_count_field: Cell<u32>,
    pub pending_pong: Cell<bool>,
    pub timed_out: Cell<bool>,
    pub on_timeout: RefCell<HeartbeatTimeoutCallback>,
}

impl HeartbeatManager {
    pub fn new(config: &HeartbeatConfig) -> HeartbeatManager {
        HeartbeatManager {
            config_field: Cell::<HeartbeatConfig>::new(*config),
            last_send_time: Cell::<u64>::new(0u64),
            last_recv_time: Cell::<u64>::new(0u64),
            missed_count_field: Cell::<u32>::new(0u32),
            pending_pong: Cell::<bool>::new(false),
            timed_out: Cell::<bool>::new(false),
            on_timeout: RefCell::<HeartbeatTimeoutCallback>::new(Default::default()),
        }
    }

    pub fn set_config(&self, config: &HeartbeatConfig) {
        self.config_field.set(*config);
        self.reset();
    }

    pub fn set_on_timeout(&self, callback: self::HeartbeatTimeoutCallback) {
        self.on_timeout.replace(callback);
    }

    pub fn should_send_heartbeat(&self) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if self.pending_pong.get() {
            return false;
        }

        let now: u64 = heartbeat_time_us();
        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.get().interval_ms as u64) * 1000u64;

        now.wrapping_sub(last) >= interval_us
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
        self.missed_count_field.set(0u32);
        self.timed_out.set(false);
    }

    pub fn check_timeout(&self) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if !self.pending_pong.get() {
            return false;
        }

        let now: u64 = heartbeat_time_us();
        let sent: u64 = self.last_send_time.get();
        let timeout_us: u64 = (self.config_field.get().timeout_ms as u64) * 1000u64;

        if now.wrapping_sub(sent) >= timeout_us {
            self.pending_pong.set(false);
            let count: u32 = self.missed_count_field.get().wrapping_add(1u32);
            self.missed_count_field.set(count);

            if count >= self.config_field.get().max_missed {
                self.timed_out.set(true);
                let mut callback = self.on_timeout.borrow_mut();
                if !(*callback).is_empty() {
                    (*callback)();
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

        let now: u64 = heartbeat_time_us();
        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.get().interval_ms as u64) * 1000u64;
        let elapsed_us: u64 = now.wrapping_sub(last);

        if elapsed_us >= interval_us {
            return 0u32;
        }

        ((interval_us - elapsed_us) / 1000u64) as u32
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
        self.last_send_time.set(0u64);
        self.last_recv_time.set(0u64);
        self.missed_count_field.set(0u32);
        self.pending_pong.set(false);
        self.timed_out.set(false);
    }

    pub fn config(&self) -> HeartbeatConfig {
        self.config_field.get()
    }
}
