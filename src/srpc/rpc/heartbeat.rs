// Canonical Rust source for the srpc.heartbeat module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use crate::threading::SharedCell;
use std::sync::{Arc, Mutex};

use crate::circuit_breaker::current_time_us;

pub fn heartbeat_time_us() -> u64 {
    current_time_us()
}

pub type HeartbeatTimeoutCallback = Option<Box<dyn FnMut() + Send>>;

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
    transition_lock_: Mutex<()>,
    pub config_field: SharedCell<HeartbeatConfig>,
    pub last_send_time: SharedCell<u64>,
    pub last_recv_time: SharedCell<u64>,
    pub missed_count_field: SharedCell<u32>,
    pub pending_pong: SharedCell<bool>,
    pub timed_out: SharedCell<bool>,
    pub on_timeout: SharedCell<Arc<Mutex<HeartbeatTimeoutCallback>>>,
}

impl HeartbeatManager {
    pub fn new(config: &HeartbeatConfig) -> HeartbeatManager {
        HeartbeatManager {
            transition_lock_: Mutex::new(()),
            config_field: SharedCell::<HeartbeatConfig>::new(*config),
            last_send_time: SharedCell::<u64>::new(0u64),
            last_recv_time: SharedCell::<u64>::new(0u64),
            missed_count_field: SharedCell::<u32>::new(0u32),
            pending_pong: SharedCell::<bool>::new(false),
            timed_out: SharedCell::<bool>::new(false),
            on_timeout: SharedCell::new(Arc::new(Mutex::new(Default::default()))),
        }
    }

    pub fn set_config(&self, config: &HeartbeatConfig) {
        let _transition = self.transition_lock_.lock().unwrap();
        self.config_field.set(*config);
        self.reset_state();
    }

    pub fn set_on_timeout(&self, callback: self::HeartbeatTimeoutCallback) {
        // Replace the owner, not the callback currently being invoked. A
        // callback may install its successor without taking its own lock.
        self.on_timeout.set(Arc::new(Mutex::new(callback)));
    }

    pub fn should_send_heartbeat(&self) -> bool {
        self.should_send_heartbeat_at(heartbeat_time_us())
    }

    pub fn should_send_heartbeat_at(&self, now: u64) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if self.pending_pong.get() {
            return false;
        }

        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.get().interval_ms as u64) * 1000u64;

        now.wrapping_sub(last) >= interval_us
    }

    pub fn on_heartbeat_sent(&self) {
        self.on_heartbeat_sent_at(heartbeat_time_us())
    }

    pub fn on_heartbeat_sent_at(&self, now: u64) {
        let _transition = self.transition_lock_.lock().unwrap();
        if !self.config_field.get().enabled {
            return;
        }
        self.last_send_time.set(now);
        self.pending_pong.set(true);
    }

    pub fn on_pong_received(&self) {
        self.on_pong_received_at(heartbeat_time_us())
    }

    pub fn on_pong_received_at(&self, now: u64) {
        let _transition = self.transition_lock_.lock().unwrap();
        if !self.config_field.get().enabled {
            return;
        }
        self.last_recv_time.set(now);
        self.pending_pong.set(false);
        self.missed_count_field.set(0u32);
        self.timed_out.set(false);
    }

    pub fn check_timeout(&self) -> bool {
        self.check_timeout_at(heartbeat_time_us())
    }

    pub fn check_timeout_at(&self, now: u64) -> bool {
        let transition = self.transition_lock_.lock().unwrap();
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if !self.pending_pong.get() {
            return false;
        }

        let sent: u64 = self.last_send_time.get();
        let timeout_us: u64 = (self.config_field.get().timeout_ms as u64) * 1000u64;

        if now.wrapping_sub(sent) >= timeout_us {
            self.pending_pong.set(false);
            let count: u32 = self.missed_count_field.get().wrapping_add(1u32);
            self.missed_count_field.set(count);

            if count >= self.config_field.get().max_missed {
                self.timed_out.set(true);
                drop(transition);
                // Clone the callback owner outside its invocation lock. This
                // serializes FnMut calls while permitting reset or replacement
                // from inside the callback without retaining the state lock.
                let callback: Arc<Mutex<HeartbeatTimeoutCallback>> = self.on_timeout.get();
                let mut invocation = callback.lock().unwrap();
                if invocation.is_some() {
                    invocation.as_mut().unwrap()();
                }
                return true;
            }
        }
        false
    }

    pub fn time_until_next_heartbeat_ms(&self) -> u32 {
        self.time_until_next_heartbeat_ms_at(heartbeat_time_us())
    }

    pub fn time_until_next_heartbeat_ms_at(&self, now: u64) -> u32 {
        if !self.config_field.get().enabled || self.timed_out.get() || self.pending_pong.get() {
            return self.config_field.get().interval_ms;
        }

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
        let _transition = self.transition_lock_.lock().unwrap();
        self.reset_state();
    }

    fn reset_state(&self) {
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
