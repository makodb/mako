//! Pending RPC requests buffered for reconnect and replay.
//!
//! This is the valid-Rust owner of the legacy `rrr.request_queue` module. The
//! public names, field order, constructors, and receiver mutability follow the
//! historical C++ surface. In particular, operations reached through a shared
//! `ClientConnection` keep `&self` receivers, while `dequeue` remains the one
//! mutating-receiver operation from that surface.

use crate::base::monotonic::monotonic_time_us;
use std::cell::Cell;
use std::collections::VecDeque;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Mutex;

/// Linux `EAGAIN`, the supported consumer ABI's queue-rejection error.
#[allow(non_upper_case_globals)]
pub const kRequestQueueRejectedError: i32 = 11;

/// Linux `ETIMEDOUT`, the supported consumer ABI's queue-expiration error.
#[allow(non_upper_case_globals)]
pub const kRequestQueueExpiredError: i32 = 110;

/// Strategy used when an enqueue observes a queue at or above capacity.
///
/// The explicit representation and discriminants preserve the original C++
/// `enum class` history. The wildcard arms below are deliberate: C++ permits
/// an out-of-range value to reach the generated switch, whose historical
/// no-default behavior falls through and enqueues the request.
#[allow(non_camel_case_types)]
#[repr(i32)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
pub enum OverflowStrategy {
    DROP_OLDEST = 0,
    DROP_NEWEST = 1,
    FAIL_FAST = 2,
}

#[allow(unreachable_patterns)]
pub fn overflow_strategy_to_string(strategy: self::OverflowStrategy) -> &'static str {
    match strategy {
        OverflowStrategy::DROP_OLDEST => "DROP_OLDEST",
        OverflowStrategy::DROP_NEWEST => "DROP_NEWEST",
        OverflowStrategy::FAIL_FAST => "FAIL_FAST",
        _ => "UNKNOWN",
    }
}

/// Completion callback carried by a queued request.
///
/// In native Rust the `Option` makes nullability explicit. The C++ consumer
/// lowers this exact source shape transparently to the nullable state already
/// carried by `rusty::Function<void(int32_t)>`; there is no extra discriminator.
pub type QueuedRequestCallback = Option<Box<dyn FnMut(i32)>>;

/// Monotonic microseconds from the crate's shared process-local epoch.
pub fn queued_request_time_us() -> u64 {
    monotonic_time_us()
}

/// One RPC request waiting to be transmitted.
///
/// Field order is the post-Marshal legacy layout. The callback stays move-only,
/// which is why the historical copy-based `peek` operation is not present.
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
    pub fn new() -> QueuedRequest {
        QueuedRequest {
            xid: 0_i64,
            rpc_id: 0_i32,
            timestamp_us: queued_request_time_us(),
            retry_count: 0_u32,
            callback: None,
            ttl_ms: 30_000_u32,
        }
    }

    pub fn is_expired(&self) -> bool {
        let now_us: u64 = queued_request_time_us();
        let elapsed_us: u64 = now_us.wrapping_sub(self.timestamp_us);
        elapsed_us / 1_000_u64 > self.ttl_ms as u64
    }

    pub fn age_ms(&self) -> u32 {
        let now_us: u64 = queued_request_time_us();
        (now_us.wrapping_sub(self.timestamp_us) / 1_000_u64) as u32
    }
}

/// Queue capacity, default TTL, overflow policy, and enablement.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
pub struct RequestQueueConfig {
    pub max_size: usize,
    pub default_ttl_ms: u32,
    pub overflow_strategy: self::OverflowStrategy,
    pub enabled: bool,
}

impl RequestQueueConfig {
    pub fn new() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 1_000_usize,
            default_ttl_ms: 30_000_u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn defaults() -> RequestQueueConfig {
        RequestQueueConfig::new()
    }

    pub fn small() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 10_usize,
            default_ttl_ms: 5_000_u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn large() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 10_000_usize,
            default_ttl_ms: 60_000_u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn disabled() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 0_usize,
            default_ttl_ms: 30_000_u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: false,
        }
    }
}

fn rq_invoke_callback_safely(mut callback: QueuedRequestCallback, error_code: i32) {
    if let Some(callback) = &mut callback {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            callback(error_code);
        }));
    }
}

/// Thread-safe queue of pending RPC requests.
///
/// The field order and `Cell`/`Mutex` carriers match the final intentional
/// legacy layout. Expiration and clear callbacks are moved out while holding
/// the queue lock and invoked after that guard has been dropped. Enqueue's
/// overflow callbacks intentionally run under the lock, preserving the
/// historical callback ordering and reentrancy contract.
#[repr(C)]
pub struct RequestQueue {
    pub config_: Cell<RequestQueueConfig>,
    pub queue_: Mutex<VecDeque<QueuedRequest>>,
}

impl RequestQueue {
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> RequestQueue {
        RequestQueue {
            config_: Cell::new(RequestQueueConfig::defaults()),
            queue_: Mutex::new(VecDeque::new()),
        }
    }

    #[cfg_attr(any(), cpp_ctor)]
    pub fn with_config(config: self::RequestQueueConfig) -> RequestQueue {
        RequestQueue {
            config_: Cell::new(config),
            queue_: Mutex::new(VecDeque::new()),
        }
    }

    #[allow(unused_mut)]
    pub fn enqueue(&self, mut request: self::QueuedRequest) -> bool {
        if !self.config_.get().enabled {
            rq_invoke_callback_safely(request.callback, kRequestQueueRejectedError);
            return false;
        }

        let mut guard = self.queue_.lock().unwrap();
        let config: RequestQueueConfig = self.config_.get();
        if guard.len() >= config.max_size {
            #[allow(unreachable_patterns)]
            match config.overflow_strategy {
                OverflowStrategy::DROP_OLDEST => {
                    if !guard.is_empty() {
                        let mut oldest = guard.pop_front().unwrap();
                        rq_invoke_callback_safely(oldest.callback, kRequestQueueRejectedError);
                    }
                }
                OverflowStrategy::DROP_NEWEST => {
                    rq_invoke_callback_safely(request.callback, kRequestQueueRejectedError);
                    return false;
                }
                OverflowStrategy::FAIL_FAST => {
                    rq_invoke_callback_safely(request.callback, kRequestQueueRejectedError);
                    return false;
                }
                _ => {}
            }
        }

        if request.ttl_ms == 0_u32 {
            request.ttl_ms = config.default_ttl_ms;
        }
        guard.push_back(request);
        true
    }

    pub fn dequeue(&mut self) -> Option<QueuedRequest> {
        let mut guard = self.queue_.lock().unwrap();
        guard.pop_front()
    }

    #[allow(unused_mut)]
    pub fn expire_stale(&self) -> usize {
        let mut callbacks_to_invoke: Vec<QueuedRequestCallback> = Vec::new();
        let mut removed: usize = 0_usize;
        {
            let mut guard = self.queue_.lock().unwrap();
            let mut index: usize = 0_usize;
            while index < guard.len() {
                if guard[index].is_expired() {
                    let mut request = guard.remove(index).unwrap();
                    callbacks_to_invoke.push(request.callback);
                    removed = removed.wrapping_add(1_usize);
                } else {
                    index = index.wrapping_add(1_usize);
                }
            }
        }

        for callback in callbacks_to_invoke {
            rq_invoke_callback_safely(callback, kRequestQueueExpiredError);
        }
        removed
    }

    pub fn size(&self) -> usize {
        let guard = self.queue_.lock().unwrap();
        guard.len()
    }

    pub fn empty(&self) -> bool {
        let guard = self.queue_.lock().unwrap();
        guard.is_empty()
    }

    pub fn full(&self) -> bool {
        let guard = self.queue_.lock().unwrap();
        guard.len() >= self.config_.get().max_size
    }

    pub fn remaining_capacity(&self) -> usize {
        let guard = self.queue_.lock().unwrap();
        let max_size: usize = self.config_.get().max_size;
        if max_size > guard.len() {
            max_size - guard.len()
        } else {
            0_usize
        }
    }

    #[allow(unused_mut)]
    pub fn clear_all(&self, error_code: i32) {
        let mut callbacks_to_invoke: Vec<QueuedRequestCallback> = Vec::new();
        {
            let mut guard = self.queue_.lock().unwrap();
            while !guard.is_empty() {
                let mut request = guard.pop_front().unwrap();
                callbacks_to_invoke.push(request.callback);
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
