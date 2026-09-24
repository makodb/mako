// Canonical Rust source for the srpc.request_queue module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use crate::threading::SharedCell;
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

pub type QueuedRequestCallback = Option<Box<dyn FnMut(i32) + Send>>;

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
    /// Encoded RPC body. The transport adds its own frame-size prefix.
    pub payload: Vec<u8>,
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
            payload: Vec::new(),
        }
    }

    pub fn is_expired(&self) -> bool {
        self.is_expired_at(queued_request_time_us())
    }

    pub fn is_expired_at(&self, now: u64) -> bool {
        let elapsed_us = now.wrapping_sub(self.timestamp_us);
        (elapsed_us / 1_000) > (self.ttl_ms as u64)
    }

    pub fn age_ms(&self) -> u32 {
        self.age_ms_at(queued_request_time_us())
    }

    pub fn age_ms_at(&self, now: u64) -> u32 {
        (now.wrapping_sub(self.timestamp_us) / 1_000) as u32
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
    if callback.is_some() {
        let _ = catch_unwind(AssertUnwindSafe(move || callback.as_mut().unwrap()(error)));
    }
}

#[repr(C)]
pub struct RequestQueue {
    pub config_: SharedCell<RequestQueueConfig>,
    pub queue_: Mutex<VecDeque<QueuedRequest>>,
}

pub struct RequestQueueAdmission {
    pub accepted: bool,
    pub retired: Vec<QueuedRequest>,
}

impl RequestQueueAdmission {
    pub fn notify(self) -> bool {
        for request in self.retired {
            rq_invoke_callback_safely(request.callback, kRequestQueueRejectedError);
        }
        self.accepted
    }
}

impl RequestQueue {
    #[allow(clippy::new_without_default)]
    pub fn new() -> RequestQueue {
        RequestQueue {
            config_: SharedCell::new(RequestQueueConfig::defaults()),
            queue_: Mutex::new(VecDeque::new()),
        }
    }

    pub fn with_config(config: self::RequestQueueConfig) -> RequestQueue {
        RequestQueue {
            config_: SharedCell::new(config),
            queue_: Mutex::new(VecDeque::new()),
        }
    }

    pub fn enqueue(&self, request: self::QueuedRequest) -> bool {
        self.enqueue_deferred(request).notify()
    }

    // The caller can couple admission to its own ownership lock, then deliver
    // callbacks and destroy retired request captures after releasing it.
    pub fn enqueue_deferred(&self, mut request: self::QueuedRequest) -> RequestQueueAdmission {
        let mut rejected: Option<QueuedRequest> = None;
        let mut evicted: Option<QueuedRequest> = None;
        let accepted: bool;
        {
            let mut guard = self.queue_.lock().unwrap();
            let config = self.config_.get();
            let full = guard.len() >= config.max_size;
            // Preserve zero-capacity DROP_OLDEST admission and the legacy
            // C++ fallback for unrecognized numeric strategy values.
            accepted = config.enabled && (!full ||
                (config.overflow_strategy != OverflowStrategy::DROP_NEWEST &&
                 config.overflow_strategy != OverflowStrategy::FAIL_FAST));
            if accepted {
                if full && config.overflow_strategy == OverflowStrategy::DROP_OLDEST {
                    evicted = guard.pop_front();
                }
                if request.ttl_ms == 0 {
                    request.ttl_ms = config.default_ttl_ms;
                }
                guard.push_back(request);
            } else {
                rejected = Some(request);
            }
        }
        let mut retired = Vec::new();
        if let Some(oldest) = evicted {
            retired.push(oldest);
        }
        if let Some(request) = rejected {
            retired.push(request);
        }
        RequestQueueAdmission { accepted, retired }
    }

    pub fn dequeue(&self) -> Option<QueuedRequest> {
        self.queue_.lock().unwrap().pop_front()
    }

    pub fn expire_stale(&self) -> usize {
        self.expire_stale_at(queued_request_time_us())
    }

    pub fn expire_stale_at(&self, now: u64) -> usize {
        let mut callbacks_to_invoke = Vec::<QueuedRequestCallback>::new();
        let mut removed = 0usize;
        {
            let mut guard = self.queue_.lock().unwrap();
            let initial_len = guard.len();
            for _ in 0..initial_len {
                let request = guard.pop_front().unwrap();
                if request.is_expired_at(now) {
                    removed = removed.wrapping_add(1);
                    if request.callback.is_some() {
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
        for request in self.drain() {
            rq_invoke_callback_safely(request.callback, error_code);
        }
    }

    pub fn drain(&self) -> VecDeque<QueuedRequest> {
        std::mem::take(&mut *self.queue_.lock().unwrap())
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
