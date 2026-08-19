// Canonical Rust source for the rrr.request_queue module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::Cell;
use std::collections::VecDeque;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Mutex;

use crate::circuit_breaker::current_time_us;

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(i32)]
pub enum OverflowStrategy {
    DROP_OLDEST = 0,
    DROP_NEWEST = 1,
    FAIL_FAST = 2,
}

#[allow(unreachable_patterns)]
pub fn overflow_strategy_to_string(strategy: OverflowStrategy) -> &'static str {
    match strategy {
        OverflowStrategy::DROP_OLDEST => "DROP_OLDEST",
        OverflowStrategy::DROP_NEWEST => "DROP_NEWEST",
        OverflowStrategy::FAIL_FAST => "FAIL_FAST",
        _ => "UNKNOWN",
    }
}

#[cfg(target_os = "macos")]
pub const kRequestQueueRejectedError: i32 = 35;
#[cfg(not(target_os = "macos"))]
pub const kRequestQueueRejectedError: i32 = 11;

#[cfg(target_os = "macos")]
pub const kRequestQueueExpiredError: i32 = 60;
#[cfg(not(target_os = "macos"))]
pub const kRequestQueueExpiredError: i32 = 110;

pub type QueuedRequestCallback = rusty::Function<dyn FnMut(i32)>;

pub fn queued_request_time_us() -> u64 {
    current_time_us()
}

#[repr(C)]
pub struct QueuedRequest {
    pub xid: i64,
    pub rpc_id: i32,
    pub timestamp_us: u64,
    pub retry_count: u32,
    pub callback: QueuedRequestCallback,
    pub ttl_ms: u32,
}

impl QueuedRequest {
    #[allow(clippy::new_without_default)]
    pub fn new() -> QueuedRequest {
        QueuedRequest {
            xid: 0,
            rpc_id: 0,
            timestamp_us: queued_request_time_us(),
            retry_count: 0,
            callback: Default::default(),
            ttl_ms: 30_000,
        }
    }

    pub fn is_expired(&self) -> bool {
        let elapsed_us = queued_request_time_us().wrapping_sub(self.timestamp_us);
        (elapsed_us / 1_000) > (self.ttl_ms as u64)
    }

    pub fn age_ms(&self) -> u32 {
        (queued_request_time_us().wrapping_sub(self.timestamp_us) / 1_000) as u32
    }
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(C)]
pub struct RequestQueueConfig {
    pub max_size: usize,
    pub default_ttl_ms: u32,
    pub overflow_strategy: OverflowStrategy,
    pub enabled: bool,
}

impl RequestQueueConfig {
    #[allow(clippy::new_without_default)]
    pub fn new() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 1_000,
            default_ttl_ms: 30_000,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn defaults() -> RequestQueueConfig {
        RequestQueueConfig::new()
    }

    pub fn small() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 10,
            default_ttl_ms: 5_000,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn large() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 10_000,
            default_ttl_ms: 60_000,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn disabled() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 0,
            default_ttl_ms: 30_000,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: false,
        }
    }
}

pub fn rq_invoke_callback_safely(mut callback: self::QueuedRequestCallback, error: i32) {
    if !callback.is_empty() {
        let _ = catch_unwind(AssertUnwindSafe(move || callback(error)));
    }
}

#[repr(C)]
pub struct RequestQueue {
    pub config_: Cell<RequestQueueConfig>,
    pub queue_: Mutex<VecDeque<QueuedRequest>>,
}

impl RequestQueue {
    #[allow(clippy::new_without_default)]
    pub fn new() -> RequestQueue {
        RequestQueue {
            config_: Cell::new(RequestQueueConfig::defaults()),
            queue_: Mutex::new(VecDeque::new()),
        }
    }

    pub fn with_config(config: self::RequestQueueConfig) -> RequestQueue {
        RequestQueue {
            config_: Cell::new(config),
            queue_: Mutex::new(VecDeque::new()),
        }
    }

    #[allow(unreachable_patterns)]
    pub fn enqueue(&self, mut request: self::QueuedRequest) -> bool {
        if !self.config_.get().enabled {
            rq_invoke_callback_safely(request.callback, kRequestQueueRejectedError);
            return false;
        }

        let mut guard = self.queue_.lock().unwrap();
        if guard.len() >= self.config_.get().max_size {
            match self.config_.get().overflow_strategy {
                OverflowStrategy::DROP_OLDEST => {
                    if let Some(oldest) = guard.pop_front() {
                        rq_invoke_callback_safely(oldest.callback, kRequestQueueRejectedError);
                    }
                }
                OverflowStrategy::DROP_NEWEST | OverflowStrategy::FAIL_FAST => {
                    rq_invoke_callback_safely(request.callback, kRequestQueueRejectedError);
                    return false;
                }
                _ => {}
            }
        }

        if request.ttl_ms == 0 {
            request.ttl_ms = self.config_.get().default_ttl_ms;
        }
        guard.push_back(request);
        true
    }

    pub fn dequeue(&mut self) -> Option<QueuedRequest> {
        self.queue_.lock().unwrap().pop_front()
    }

    pub fn expire_stale(&self) -> usize {
        let mut callbacks_to_invoke = Vec::<QueuedRequestCallback>::new();
        let mut removed = 0usize;
        {
            let mut guard = self.queue_.lock().unwrap();
            let initial_len = guard.len();
            for _ in 0..initial_len {
                let request = guard.pop_front().unwrap();
                if request.is_expired() {
                    removed = removed.wrapping_add(1);
                    if !request.callback.is_empty() {
                        callbacks_to_invoke.push(request.callback);
                    }
                } else {
                    guard.push_back(request);
                }
            }
        }
        for callback in callbacks_to_invoke {
            rq_invoke_callback_safely(callback, kRequestQueueExpiredError);
        }
        removed
    }

    pub fn size(&self) -> usize {
        self.queue_.lock().unwrap().len()
    }

    pub fn empty(&self) -> bool {
        self.queue_.lock().unwrap().is_empty()
    }

    pub fn full(&mut self) -> bool {
        let guard = self.queue_.lock().unwrap();
        guard.len() >= self.config_.get().max_size
    }

    pub fn remaining_capacity(&mut self) -> usize {
        let guard = self.queue_.lock().unwrap();
        let max_size = self.config_.get().max_size;
        if max_size > guard.len() {
            max_size - guard.len()
        } else {
            0
        }
    }

    pub fn clear_all(&self, error_code: i32) {
        let mut callbacks_to_invoke = Vec::<QueuedRequestCallback>::new();
        {
            let mut guard = self.queue_.lock().unwrap();
            while let Some(request) = guard.pop_front() {
                if !request.callback.is_empty() {
                    callbacks_to_invoke.push(request.callback);
                }
            }
        }
        for callback in callbacks_to_invoke {
            rq_invoke_callback_safely(callback, error_code);
        }
    }

    pub fn config(&self) -> RequestQueueConfig {
        self.config_.get()
    }

    pub fn enabled(&self) -> bool {
        self.config_.get().enabled
    }

    pub fn max_size(&self) -> usize {
        self.config_.get().max_size
    }

    pub fn update_config(&self, config: self::RequestQueueConfig) {
        let _guard = self.queue_.lock().unwrap();
        self.config_.set(config);
    }
}
