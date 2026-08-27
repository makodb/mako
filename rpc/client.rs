//! srpc.client — RPC client (formerly client.hpp + client.cpp).
//!
//! Owns ClientConnection (framing + reply dispatch), Client (the
//! user-facing facade), Future (async reply delivery), ClientPool and
//! the bulk reconnect helpers. Sits above the channel layer
//! (`tcp_channel`, `inmemory_channel`) which this module consumes
//! through the transport-agnostic `ChannelConnectionProxy`.
//!
//! # Clippy: what was taken and what was pinned (measured, not assumed)
//!
//! This file is the canonical Rust the C++ provider is generated FROM, so a
//! lint is only free when taking it leaves the emitted `srpc.client.cppm`
//! unchanged. Every finding below was measured the same way: apply the lint's
//! own suggestion, regenerate all 36 providers, and byte-compare the emitted
//! module.
//!
//! Taken (26 sites, all `clippy::explicit_auto_deref` on a lock guard):
//! measured individually AND together — the emitted module is byte-identical
//! either way, so the `(*guard)` spellings are simply gone.
//!
//! Pinned with an item-scoped `#[allow]` (80 findings across 14 families, 35
//! items). Not one of them is emission-neutral; each attribute carries the
//! specific measured consequence. Four are worse than churn — they change the
//! provider's ABI:
//!
//!   * `upper_case_acronyms` renames the emitted enumerator and the exported
//!     `DisconnectBehavior_QUEUE()` accessor;
//!   * `ptr_arg` retypes exported `clientpool_select` from
//!     `const rusty::Vec<..>&` to `std::span<..>`;
//!   * `wrong_self_convention` changes an emitted method signature;
//!   * `derivable_impls` inlines `FutureAttr::default_()` into the class and
//!     deletes its out-of-line definition, i.e. removes a provider symbol.
//!
//! And the largest family is also the most dangerous to take blindly: of the
//! 68 `explicit_auto_deref` sites, 42 change emitted C++, including inserting
//! a `std::move` out of a shared `Arc`'s field, binding a `RefMut` borrow
//! guard by value instead of by reference, and passing a pointer where a
//! value was passed. `clippy --fix` cannot help here — those suggestions are
//! `MachineApplicable` but the emitted-C++ consequence is invisible to it.
//!
//! No blanket `#![allow]` is used: the pins are per item so a future edit to
//! any other function is still linted.

#![allow(unsafe_code, non_camel_case_types, non_snake_case)]

use std::cell::{Cell, RefCell, RefMut};
use std::collections::{BTreeMap, HashMap};
// (`std::ffi::CStr` is deliberately NOT imported: see `clientconn_addr_to_string`.)
use std::sync::atomic::{AtomicBool, AtomicU64};
use std::sync::{Arc, Condvar, Mutex, Weak};
use std::time::Duration;
use cpp::srpc::rand as cpp_rand_facade;
use rusty as cpp;

// These are still supplied by historical inline C++ modules.  The `cpp::`
// imports make their named-module ownership explicit without inventing a Rust
// namespace that does not exist in the public C++ surface.
// These otherwise-unused source-owned imports keep the exact
// `srpc.callback_wrapper` / `srpc.reactor` / `srpc.serializable` providers
// visible to generated C++; the types themselves are reached through the
// checked type map and the crate paths.
#[allow(unused_imports)]
use cpp::srpc::callback_wrapper as _;
#[allow(unused_imports)]
use cpp::srpc::reactor as _;
#[allow(unused_imports)]
use cpp::srpc::serializable as cpp_serializable;
use rusty::RustyCellGet as _;
use rusty::RustyStdStringCStr as _;
use rusty::RustyHandleIsValid as _;



use crate::basetypes::{Counter, Time};
use crate::callbacks::CallbackManager;
use crate::channel::{
    channel_error_to_string, ChannelConnectionBase, ChannelConnectionProxy, ChannelError,
    ChannelFactoryBase, ChannelFactoryProxy, ChannelFrame, ConnectResult, OnClosedCallback,
    OnErrorCallback, OnFrameCallback,
};
use crate::circuit_breaker::{CircuitBreaker, CircuitBreakerConfig, CircuitState};
use crate::connection_metrics::ConnectionMetrics;
use crate::connection_state::{connection_state_to_string, ConnectionState, ConnectionStateMachine};
use crate::debugging::verify_failed;
use crate::errors::RpcError;
use crate::fiber_channel::{FiberChannel, OwnedFrame};
use crate::heartbeat::{HeartbeatConfig, HeartbeatManager, HeartbeatTimeoutCallback};
use crate::load_balancer::{LoadBalancer, LoadBalancerState, LoadBalancingStrategy};
use crate::logging::Log;
use crate::misc::OneTimeJob;
use crate::reconnect_policy::{ReconnectPolicy};
use crate::request_options::{RequestOptions, TimeoutType};
use crate::request_queue::{
    OverflowStrategy, QueuedRequest, QueuedRequestCallback, RequestQueue, RequestQueueConfig,
};
use crate::serializable::{
    BinaryReadArchive, BinaryWriteArchive, BufferSink, BufferSource, SinkProxy, SourceProxy,
};
use crate::tcp_channel::{make_tcp_factory_proxy, TcpFactory};

// Rustc-only facade identities with checked C++ type maps back to the public
// root-level `srpc::Fiber` and `srpc::PollThread` classes.
pub type Fiber = cpp::ReactorFiber;
pub type PollThread = cpp::ReactorPollThread;

pub type WeakClientConnection = Weak<ClientConnection>;
pub type FutureResult = Result<Arc<Future>, i32>;
pub type AsyncReplyCallback = rusty::Function<dyn FnMut(i32, *const u8, usize)>;
pub type OnReconnectCompleteCallbackFn = rusty::Function<dyn FnMut(bool)>;
pub type OnServerRestartCallbackFn = rusty::Function<dyn FnMut(u64, u64)>;
pub type OnConnectedCallbackFn = Box<dyn Fn() + Send + Sync>;
pub type OnErrorCallbackFn = Box<dyn Fn(RpcError, &LegacyStdString) + Send + Sync>;
pub type OnReconnectedCallbackFn = Box<dyn Fn(bool) + Send + Sync>;
pub type LegacyStdString = String;
pub type LegacyStdStringView<'a> = &'a str;
pub type LegacyCallbackWrapper<F> = rusty::CallbackWrapper<F>;

pub struct ClientCloneCell<T>(Mutex<T>);

impl<T> ClientCloneCell<T> {
    fn new(value: T) -> ClientCloneCell<T> {
        ClientCloneCell(Mutex::new(value))
    }

    fn set(&self, value: T) {
        *self.0.lock().unwrap() = value;
    }
}

impl<T: Clone> ClientCloneCell<T> {
    fn get(&self) -> T {
        self.0.lock().unwrap().clone()
    }
}

pub const CLIENT_ERR_AGAIN: i32 = 11;
pub const CLIENT_ERR_WOULD_BLOCK: i32 = CLIENT_ERR_AGAIN;
pub const CLIENT_ERR_BUSY: i32 = 16;
pub const CLIENT_ERR_CANCELED: i32 = 125;
pub const CLIENT_ERR_CONNECTION_ABORTED: i32 = 103;
pub const CLIENT_ERR_CONNECTION_REFUSED: i32 = 111;
pub const CLIENT_ERR_CONNECTION_RESET: i32 = 104;
pub const CLIENT_ERR_HOST_UNREACHABLE: i32 = 113;
pub const CLIENT_ERR_INVALID_ARGUMENT: i32 = 22;
pub const CLIENT_ERR_IO: i32 = 5;
pub const CLIENT_ERR_NETWORK_UNREACHABLE: i32 = 101;
pub const CLIENT_ERR_NOT_CONNECTED: i32 = 107;
pub const CLIENT_ERR_BROKEN_PIPE: i32 = 32;
pub const CLIENT_ERR_TIMED_OUT: i32 = 110;
#[cfg(target_os = "macos")]
pub const CLIENT_REQUEST_QUEUE_REJECTED_ERROR: i32 = 35;
#[cfg(not(target_os = "macos"))]
pub const CLIENT_REQUEST_QUEUE_REJECTED_ERROR: i32 = 11;
pub const CLIENT_INT_MIN: i32 = i32::MIN;
pub const CLIENT_RAND_MAX: i32 = i32::MAX;
pub const CLIENT_INTERNAL_HEARTBEAT_RPC_ID: i32 = i32::MIN;
pub const CLIENT_POLL_READ: i32 = 1;
pub const CLIENT_POLL_NO_CHANGE: i32 = -1;
pub type c_char = i8;

pub fn client_rand(min: i32, max: i32) -> i32 {
    // SAFETY: `srpc::RandomGenerator::rand` is a pure integer draw over the
    // half-open range; the foreign named-module boundary is what `unsafe`
    // records here, not a memory precondition.
    unsafe { cpp_rand_facade::RandomGenerator::rand(min, max) }
}

pub fn client_verify(value: bool) {
    if !value {
        verify_failed("rpc/client.rs", 0);
    }
}

// clippy::not_unsafe_ptr_arg_deref -- this became public with the module's
// surface; the raw-pointer contract is the historical C++ one and is
// documented at the deref itself. Marking the fn `unsafe` instead would
// wrap every call site in an `unsafe` block, which the emitter renders
// as an @unsafe comment block -- measured: changes emitted C++.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn client_log_line(level: i32, line: i32, file: *const i8, message: LegacyStdString) {
    // SAFETY: all canonical callers currently pass a null file pointer; the
    // owned message remains live through the synchronous logging call.
    unsafe { crate::logging::log_line(level, line, file, &message) }
}

pub fn client_text(text: &str) -> LegacyStdString {
    text.to_string()
}

pub fn client_text_str(prefix: &str, value: &str, suffix: &str) -> LegacyStdString {
    // The `LegacyStdString` annotation is load-bearing, not decoration: the
    // checked type map spells this alias `std::string`, and only a DECLARED
    // type carries that mapping onto a local. Left inferred, `to_string()`
    // lowers the local to `rusty::String` while the signature still says
    // `std::string` — the same Rust type in two C++ spellings.
    // `base/misc.cpp` annotates its own `LegacyStdString` local likewise.
    let mut message: LegacyStdString = prefix.to_string();
    message += value;
    message += suffix;
    message
}

pub fn client_text_i32(prefix: &str, value: i32, suffix: &str) -> LegacyStdString {
    client_text_str(prefix, &value.to_string(), suffix)
}

pub fn client_text_u32_str(
    prefix: &str,
    value: u32,
    middle: &str,
    text: &str,
    suffix: &str,
) -> LegacyStdString {
    let mut message = client_text_str(prefix, &value.to_string(), middle);
    message += text;
    message += suffix;
    message
}

pub fn client_text_u64_pair(
    prefix: &str,
    first: u64,
    middle: &str,
    second: u64,
    suffix: &str,
) -> LegacyStdString {
    let mut message = client_text_str(prefix, &first.to_string(), middle);
    message += &second.to_string();
    message += suffix;
    message
}

pub fn client_text_str_i32(
    prefix: &str,
    text: &str,
    middle: &str,
    value: i32,
    suffix: &str,
) -> LegacyStdString {
    let mut message = client_text_str(prefix, text, middle);
    message += &value.to_string();
    message += suffix;
    message
}

pub fn client_text_str_pair(
    prefix: &str,
    first: &str,
    middle: &str,
    second: &str,
    suffix: &str,
) -> LegacyStdString {
    let mut message = client_text_str(prefix, first, middle);
    message += second;
    message += suffix;
    message
}

pub struct ReplyBuffer {
    body: Vec<u8>,
    src: BufferSource,
}

pub fn client_sink_proxy(sink: &mut BufferSink) -> SinkProxy {
    // SAFETY: the archive proxy is used only while this uniquely borrowed
    // sink remains live in its enclosing request operation.
    unsafe { crate::serializable::make_sink_proxy_buffer(sink as *mut BufferSink) }
}

pub fn client_source_proxy(source: &mut BufferSource) -> SourceProxy {
    // SAFETY: the archive proxy is used only while this uniquely borrowed
    // source and its retained reply buffer remain live.
    unsafe { crate::serializable::make_source_proxy_buffer(source as *mut BufferSource) }
}

// @safe - value-init factory (empty body, null/0 cursor). The old excuse
// ("the DSL has no spelling for a null-pointer BufferSource literal") is
// expired: core::ptr::null() lowers to rusty::ptr::null() and is already
// used elsewhere in this file's DSL.
pub fn reply_buffer_empty() -> ReplyBuffer {
    ReplyBuffer {
        body: Vec::<u8>::new(),
        src: BufferSource::new(core::ptr::null(), 0usize),
    }
}

pub fn reply_buffer_fill(rb: &mut ReplyBuffer, bytes: &[u8]) {
    rb.body.clear();
    rb.body.extend_from_slice(bytes);
    rb.src = BufferSource::new(rb.body.as_ptr(), rb.body.len());
}

// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
#[allow(clippy::explicit_auto_deref)]
pub fn deserialize_from<T>(mut src: RefMut<ReplyBuffer>, value: &mut T) {
    let mut ar = BinaryReadArchive {
        source_: client_source_proxy(&mut (*src).src),
    };
    crate::serializable::Deserialize_::deserialize(value, &mut ar);
}

// clippy::upper_case_acronyms -- renaming the variant renames the emitted enumerator AND the exported DisconnectBehavior_QUEUE() accessor; measured. See the Task-2 measurement block above.
#[allow(clippy::upper_case_acronyms)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DisconnectBehavior {
    QUEUE,
    FAIL_FAST,
}

#[derive(Clone)]
pub struct BufferingConfig {
    behavior: DisconnectBehavior,
    max_pending: usize,
    default_ttl_ms: u32,
    overflow: OverflowStrategy,
    enabled: bool,
}

impl Copy for BufferingConfig {}

impl BufferingConfig {
    fn new() -> BufferingConfig {
        BufferingConfig {
            behavior: DisconnectBehavior::QUEUE,
            max_pending: 1000usize,
            default_ttl_ms: 30000u32,
            overflow: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    fn defaults() -> BufferingConfig {
        BufferingConfig::new()
    }

    fn disabled() -> BufferingConfig {
        BufferingConfig {
            behavior: DisconnectBehavior::FAIL_FAST,
            max_pending: 1000usize,
            default_ttl_ms: 30000u32,
            overflow: OverflowStrategy::DROP_OLDEST,
            enabled: false,
        }
    }

    // clippy::wrong_self_convention -- taking self by value changes the emitted method signature; measured. See the Task-2 measurement block above.
    #[allow(clippy::wrong_self_convention)]
    fn to_queue_config(&self) -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: self.max_pending,
            default_ttl_ms: self.default_ttl_ms,
            overflow_strategy: self.overflow,
            enabled: self.enabled,
        }
    }
}

#[derive(Clone)]
pub struct KeepaliveConfig {
    enabled: bool,
    idle_sec: i32,
    interval_sec: i32,
    count: i32,
}

impl Copy for KeepaliveConfig {}

impl KeepaliveConfig {
    fn new() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 60i32, interval_sec: 10i32, count: 5i32 }
    }

    fn aggressive() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 10i32, interval_sec: 2i32, count: 3i32 }
    }

    fn relaxed() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 60i32, interval_sec: 10i32, count: 5i32 }
    }

    fn disabled() -> KeepaliveConfig {
        KeepaliveConfig { enabled: false, idle_sec: 0i32, interval_sec: 0i32, count: 0i32 }
    }
}

#[derive(Clone)]
pub struct PoolConfig {
    min_connections: i32,
    max_connections: i32,
    idle_timeout_ms: u64,
    health_check_enabled: bool,
    unhealthy_threshold_percent: u64,
    min_requests_for_health: u64,
    load_balancing: LoadBalancingStrategy,
}

impl Copy for PoolConfig {}

impl PoolConfig {
    fn new() -> PoolConfig {
        PoolConfig {
            min_connections: 1i32,
            max_connections: 4i32,
            idle_timeout_ms: 300000u64,
            health_check_enabled: true,
            unhealthy_threshold_percent: 50u64,
            min_requests_for_health: 10u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }

    fn defaults() -> PoolConfig {
        PoolConfig::new()
    }

    fn aggressive() -> PoolConfig {
        PoolConfig {
            min_connections: 2i32,
            max_connections: 8i32,
            idle_timeout_ms: 60000u64,
            health_check_enabled: true,
            unhealthy_threshold_percent: 70u64,
            min_requests_for_health: 5u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }

    fn conservative() -> PoolConfig {
        PoolConfig {
            min_connections: 1i32,
            max_connections: 2i32,
            idle_timeout_ms: 600000u64,
            health_check_enabled: true,
            unhealthy_threshold_percent: 30u64,
            min_requests_for_health: 20u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }

    fn no_health_check() -> PoolConfig {
        PoolConfig {
            min_connections: 1i32,
            max_connections: 4i32,
            idle_timeout_ms: 300000u64,
            health_check_enabled: false,
            unhealthy_threshold_percent: 50u64,
            min_requests_for_health: 10u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }
}

pub type FutureCallback = LegacyCallbackWrapper<rusty::Function<dyn Fn(Arc<Future>)>>;

pub struct FutureAttr {
    callback: FutureCallback,
}

impl FutureAttr {
    fn new(cb: FutureCallback) -> FutureAttr {
        FutureAttr { callback: cb }
    }
}

impl Clone for FutureAttr {
    fn clone(&self) -> FutureAttr {
        FutureAttr {
            callback: self.callback.clone(),
        }
    }
}

// clippy::derivable_impls -- measured: deriving inlines FutureAttr::default_() into the class and REMOVES its out-of-line definition, i.e. a provider symbol. See the Task-2 measurement block above.
#[allow(clippy::derivable_impls)]
impl Default for FutureAttr {
    fn default() -> FutureAttr {
        FutureAttr {
            callback: Default::default(),
        }
    }
}

pub struct FutureState {
    ready: bool,
    timed_out: bool,
    completion_callbacks: Vec<rusty::Function<dyn FnMut()>>,
}

impl FutureState {
    fn new() -> FutureState {
        FutureState { ready: false, timed_out: false, completion_callbacks: Vec::<rusty::Function<dyn FnMut()>>::new() }
    }
}

pub struct Future {
    xid_: i64,
    error_code_: Cell<i32>,
    attr_: FutureAttr,
    reply_: RefCell<ReplyBuffer>,
    timeout_: u64,
    state_: Mutex<FutureState>,
    ready_cond_: Condvar,
    options_: Cell<RequestOptions>,
    timeout_type_: Cell<TimeoutType>,
    retry_count_: Cell<u16>,
}

impl Future {
    fn new(xid: i64, attr: FutureAttr) -> Future {
        Future {
            xid_: xid,
            error_code_: Cell::new(0i32),
            attr_: attr,
            reply_: RefCell::<ReplyBuffer>::new(reply_buffer_empty()),
            timeout_: 1000000u64,
            state_: Mutex::<FutureState>::new(FutureState::new()),
            ready_cond_: Condvar::new(),
            options_: Cell::new(RequestOptions::defaults()),
            timeout_type_: Cell::new(TimeoutType::NONE),
            retry_count_: Cell::new(0u16),
        }
    }

    // clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
    #[allow(clippy::arc_with_non_send_sync)]
    fn create(xid: i64, attr: FutureAttr) -> Arc<Future> {
        Arc::new(Future::new(xid, attr))
    }

    fn ready(&self) -> bool {
        let guard = self.state_.lock().unwrap();
        guard.ready
    }

    fn wait(&self) {
        if self.timeout_ > 0u64 {
            let sec: f64 = (self.timeout_ as f64) / 1000000.0;
            self.timed_wait(sec);
            return;
        }
        let guard = self.state_.lock().unwrap();
        // rusty::Condvar is @safe; wait WHILE not-ready and not-timed-out.
        let _reacquired = self.ready_cond_.wait_while(guard, |s| !s.ready && !s.timed_out).unwrap();
    }

    // clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
    #[allow(clippy::explicit_auto_deref)]
    fn timed_wait(&self, sec: f64) {
        let guard = self.state_.lock().unwrap();
        let micros: u64 = (sec * 1000000.0) as u64;
        // Destructured in the `let`, not through `result.0` / `result.1`: a
        // tuple bound to its own name is emitted as a CONST local, so moving
        // the guard out of it afterwards selects `MutexGuard`'s deleted copy
        // constructor. The pattern lowers to a structured binding instead.
        let (mut guard, timeout_result) = self.ready_cond_.wait_timeout_while(
            guard,
            Duration::from_micros(micros),
            |s| !s.ready && !s.timed_out,
        ).unwrap();
        let condition_became_false: bool = !timeout_result.timed_out();
        if !condition_became_false && !(*guard).ready {
            (*guard).timed_out = true;
            self.error_code_.set(CLIENT_ERR_TIMED_OUT);
            self.timeout_type_.set(TimeoutType::RESPONSE_TIMEOUT);
        }
    }

    fn wait_with_options(&self) -> bool {
        let opts = self.get_options();
        if opts.timeout_ms == 0u64 {
            self.wait();
            return self.ready();
        }
        let sec: f64 = (opts.timeout_ms as f64) / 1000.0f64;
        self.timed_wait(sec);
        self.ready() && !self.timed_out()
    }

    fn timed_out(&self) -> bool {
        let guard = self.state_.lock().unwrap();
        guard.timed_out
    }

    fn add_completion_callback(&self, callback: rusty::Function<dyn FnMut()>) -> bool {
        let mut guard = self.state_.lock().unwrap();
        if guard.ready || guard.timed_out {
            return false;
        }
        guard.completion_callbacks.push(callback);
        true
    }

    fn get_reply(&self) -> RefMut<'_, ReplyBuffer> {
        self.wait();
        self.reply_.borrow_mut()
    }

    fn get_error_code(&self) -> i32 {
        if self.timeout_ > 0u64 {
            let x: f64 = (self.timeout_ as f64) / 1000000.0f64;
            self.timed_wait(x);
        } else {
            self.wait();
        }
        self.error_code_.get()
    }

    fn get_xid(&self) -> i64 {
        self.xid_
    }

    fn get_options(&self) -> RequestOptions {
        self.options_.get()
    }

    fn set_options(&self, opts: &RequestOptions) {
        self.options_.set(*opts)
    }

    fn get_timeout_type(&self) -> TimeoutType {
        self.timeout_type_.get()
    }

    fn set_timeout_type(&mut self, type_: TimeoutType) {
        self.timeout_type_.set(type_)
    }

    fn get_retry_count(&self) -> u16 {
        self.retry_count_.get()
    }

    fn increment_retry_count(&mut self) -> u16 {
        let current = self.retry_count_.get();
        self.retry_count_.set(current + 1u16);
        current + 1u16
    }

    fn should_retry(&self) -> bool {
        let opts = self.options_.get();
        opts.can_retry(self.retry_count_.get())
    }

    fn notify_ready(&self, self_arc: Arc<Future>) {
        let should_callback: bool;
        let mut completion_callbacks: Vec<rusty::Function<dyn FnMut()>>;
        {
            let mut guard = self.state_.lock().unwrap();
            if !guard.timed_out {
                guard.ready = true;
            }
            should_callback = guard.ready;
            completion_callbacks = std::mem::take(&mut guard.completion_callbacks);
        }
        // Notify waiters after dropping the lock.
        self.ready_cond_.notify_all();
        for callback in &mut completion_callbacks {
            if !callback.is_empty() {
                callback();
            }
        }
        if should_callback && self.attr_.callback.has_value() {
            let x = self.attr_.callback.clone();
            x.callable()(self_arc);
        }
    }

    fn safe_release(_fu: Arc<Future>) {
    }
}

pub const kAsyncSlotCount: usize = 16384;

pub struct ReconnectState {
    reconnecting_: AtomicBool,
    reconnect_abort_: AtomicBool,
    // auto-reconnect attempt counter — incremented before the
    // reconnect-thread spawn in on_channel_closed_fan_out; tests
    // inspect it to verify the fan-out reached the policy branch.
    channel_reconnect_attempts_: AtomicU64,
}

// clippy::reserve_after_initialization -- measured: emits Vec::with_capacity() and drops the reserve() call. See the Task-2 measurement block above.
#[allow(clippy::reserve_after_initialization)]
pub fn make_prefilled_cb_slots() -> Vec<Option<AsyncReplyCallback>> {
    let mut slots = Vec::<Option<AsyncReplyCallback>>::new();
    slots.reserve(kAsyncSlotCount);
    let mut i: usize = 0;
    while i < kAsyncSlotCount {
        slots.push(None);
        i += 1;
    }
    slots
}

pub struct ClientConnection {
    poll_thread_worker_: Arc<PollThread>,
    fiber_channel_: rusty::Mutex<Option<Box<FiberChannel>>>,
    direct_channel_: rusty::Mutex<Option<ChannelConnectionProxy>>,
    channel_mode_: Cell<bool>,
    factory_: rusty::Mutex<Option<ChannelFactoryProxy>>,
    xid_counter_: Counter,
    pending_fu_: rusty::Mutex<HashMap<i64, Arc<Future>>>,
    pending_cb_slots_: rusty::Mutex<Vec<Option<AsyncReplyCallback>>>,
    state_machine_: ConnectionStateMachine,
    reconnect_policy_: Cell<ReconnectPolicy>,
    reconnect_: ReconnectState,
    reconnect_address_: Cell<LegacyStdString>,
    buffering_config_: Cell<BufferingConfig>,
    pending_queue_: RequestQueue,
    server_instance_id_: Cell<u64>,
    on_server_restart_: RefCell<OnServerRestartCallbackFn>,
    keepalive_config_: Cell<KeepaliveConfig>,
    heartbeat_manager_: HeartbeatManager,
    circuit_breaker_: CircuitBreaker,
    callback_manager_: Arc<CallbackManager>,
    last_activity_time_: Cell<u64>,
    metrics_: ConnectionMetrics,
    weak_self_: WeakClientConnection,
    host_: LegacyStdString,
    packets_: u64,
    paused_: Cell<bool>,
    is_client_mode_: bool,
}

// The reactor's `OneTimeJob` callable is `Box<dyn FnMut() + Send + Sync>`
// (base/misc.cpp), and this module schedules two of them onto the poll
// thread -- the channel-mode close job in `ClientProxy::close` and the
// recv-loop spawn in `clientconn_start_recv_job` -- capturing an
// `Arc<ClientConnection>` / `Weak<ClientConnection>`.  `ClientConnection`
// carries `Cell` / `RefCell` interior mutability, so those captures are only
// well-formed with the assertions below.  This is the same statement
// `rpc/server.cpp` already makes for `RpcServiceContext` / `ServerConnection`.
//
// SAFETY: this states the module's long-standing single-poll-thread contract,
// unchanged from the retired C++ carrier.  Every `Cell`/`RefCell` field of
// `ClientConnection` is written from the connection's own poll thread; other
// threads read them as monotone latches or under the `rusty::Mutex` slots,
// exactly as the C++ carrier did.  The two jobs above are executed by that
// same poll thread, in queue order.
#[allow(unsafe_code)]
unsafe impl Send for ClientConnection {}
#[allow(unsafe_code)]
unsafe impl Sync for ClientConnection {}

impl Drop for ClientConnection {
    fn drop(&mut self) {
        self.reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release);
        self.reconnect_.reconnecting_.store(false, rusty::sync::atomic::Ordering::Release);
        self.invalidate_pending_futures();
    }
}

impl ClientConnection {
    fn new(poll_thread_worker: Arc<PollThread>) -> ClientConnection {
        ClientConnection {
            poll_thread_worker_: poll_thread_worker,
            fiber_channel_: rusty::Mutex::<Option<Box<FiberChannel>>>::new(None),
            direct_channel_: rusty::Mutex::<Option<ChannelConnectionProxy>>::new(None),
            channel_mode_: Cell::<bool>::new(false),
            factory_: rusty::Mutex::<Option<ChannelFactoryProxy>>::new(None),
            xid_counter_: Counter::new(0i64),
            pending_fu_: rusty::Mutex::<HashMap<i64, Arc<Future>>>::new(HashMap::<i64, Arc<Future>>::new()),
            pending_cb_slots_: rusty::Mutex::<Vec<Option<AsyncReplyCallback>>>::new(make_prefilled_cb_slots()),
            state_machine_: ConnectionStateMachine::new(),
            reconnect_policy_: Cell::<ReconnectPolicy>::new(ReconnectPolicy::new()),
            reconnect_: ReconnectState {
                reconnecting_: AtomicBool::new(false),
                reconnect_abort_: AtomicBool::new(false),
                channel_reconnect_attempts_: AtomicU64::new(0),
            },
            // `Default::default()` (not `LegacyStdString::default()`): the
            // alias is spelled `std::string` by the checked type map, and an
            // associated-function path on it emits `std::string::default_`,
            // which does not exist. In expected-type position the emitter
            // lowers `Default::default()` to `rusty::default_like<T>()`, the
            // same shape the `on_server_restart_` field below already uses.
            reconnect_address_: Cell::<LegacyStdString>::new(Default::default()),
            buffering_config_: Cell::<BufferingConfig>::new(BufferingConfig::defaults()),
            pending_queue_: make_pending_queue(&BufferingConfig::defaults().to_queue_config()),
            server_instance_id_: Cell::<u64>::new(0u64),
            on_server_restart_: RefCell::<OnServerRestartCallbackFn>::new(Default::default()),
            keepalive_config_: Cell::<KeepaliveConfig>::new(KeepaliveConfig::new()),
            heartbeat_manager_: HeartbeatManager::new(&HeartbeatConfig::disabled()),
            circuit_breaker_: CircuitBreaker::new(CircuitBreakerConfig::disabled()),
            callback_manager_: Arc::<CallbackManager>::new(CallbackManager::new()),
            last_activity_time_: Cell::<u64>::new(0u64),
            metrics_: ConnectionMetrics::new(),
            weak_self_: WeakClientConnection::new(),
            host_: Default::default(),
            packets_: 0u64,
            paused_: Cell::<bool>::new(false),
            is_client_mode_: false,
        }
    }

    // --- delegating methods (&mut self → non-const free fns) ---
    // recv-loop cluster: &self over interior-mutable state, so it is callable
    // directly through a shared Arc<ClientConnection> (no const_cast at the
    // fiber/job/channel-callback spawn sites).
    fn run_recv_loop(&self) { clientconn_run_recv_loop(self); }
    fn decode_response_and_notify(&self, bytes: *const u8, size: usize) { clientconn_decode_response_and_notify(self, bytes, size); }
    // clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
    // clippy::unnecessary_cast -- measured: drops the emitted rusty::detail::ptr_cast<const int8_t*>. See the Task-2 measurement block above.
    #[allow(clippy::explicit_auto_deref, clippy::unnecessary_cast)]
    fn on_channel_closed_fan_out(&self) {
        let prev_state = self.state_machine_.state();
        let abort_flag: bool = self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        let user_initiated_closing: bool =
            (prev_state as i32) == (ConnectionState::DISCONNECTING as i32)
            || (prev_state as i32) == (ConnectionState::DISCONNECTED as i32)
            || abort_flag;

        if !user_initiated_closing {
            self.invoke_error_callback(CLIENT_ERR_CONNECTION_RESET, &client_text("channel closed"));
            self.state_machine_.force_state(ConnectionState::FAILED);
        }

        self.heartbeat_manager_.reset();
        self.invalidate_pending_futures();

        if !user_initiated_closing {
            self.invoke_disconnected_callback();
        }

        // Trigger channel-mode auto-reconnect if the policy allows. The
        // counter is bumped the moment the fan-out reaches this branch (the
        // observability signal tests assert), then a spawn does the work
        // unless reconnect was aborted.
        let addr: LegacyStdString = self.reconnect_address_.get();
        if self.reconnect_policy_.get().auto_reconnect && !addr.is_empty() {
            self.reconnect_.channel_reconnect_attempts_.fetch_add(1, rusty::sync::atomic::Ordering::AcqRel);

            let reconnect_aborted: bool = self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
            if reconnect_aborted {
                return;
            }
            let weak_conn: WeakClientConnection = self.weak_self_.clone();
            rusty::thread::spawn(move || {
                let conn_opt = weak_conn.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                let conn_aborted: bool = (*conn).reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
                if !(*conn).reconnect_policy_.get().auto_reconnect || conn_aborted {
                    return;
                }
                let state = (*conn).connection_state();
                if (state as i32) == (ConnectionState::FAILED as i32)
                    || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                    if (*conn).is_factory_bound() {
                        client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text("srpc::ClientConnection: channel-mode auto-reconnect (factory) triggered after on_closed"));
                        // Reset the channel-mode latch + drop the stale FiberChannel
                        // before re-connecting (connect verifies !is_connected and
                        // bind_channel needs the slot empty). connect reads
                        // reconnect_address_ itself, so we just re-run it.
                        (*conn).reset_channel_mode_for_reconnect();
                        let reconnect_addr: LegacyStdString = (*conn).reconnect_address_.get();
                        let _ = (*conn).connect(reconnect_addr.c_str() as *const i8);
                        return;
                    }
                    client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text("srpc::ClientConnection: channel-mode auto-reconnect (legacy) triggered after on_closed"));
                    (*conn).reconnect(Default::default());
                }
            }).detach();
        }
    }
    // connect/bind cluster: &self over interior-mutable state (channels are
    // rusty::Mutex, reconnect_address_ is Cell), so reachable through a shared Arc.
    fn connect_via_factory(&self, addr: *const i8) -> i32 { clientconn_connect_via_factory(self, addr) }
    fn reset_channel_mode_for_reconnect(&self) {
        {
            let mut guard = self.fiber_channel_.lock().unwrap();
            *guard = None;
        }
        {
            let mut guard = self.direct_channel_.lock().unwrap();
            *guard = None;
        }
        self.channel_mode_.set(false);
        self.state_machine_.force_state(ConnectionState::DISCONNECTED);
    }
    fn connect(&self, addr: *const i8) -> i32 {
        client_verify(!self.state_machine_.is_connected());

        if !self.state_machine_.transition_to(ConnectionState::CONNECTING) {
            client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text_str("srpc::ClientConnection: cannot connect from state ",
                               connection_state_to_string(self.state_machine_.state()), ""));
            self.invoke_error_callback(CLIENT_ERR_INVALID_ARGUMENT, &client_text("invalid state for connect"));
            return CLIENT_ERR_INVALID_ARGUMENT;
        }

        // Channel mode is the only path: Client::connect auto-installs a TCP
        // factory before calling this. connect_via_factory issues
        // factory->connect(addr), hands the proxy to bind_channel_direct, and
        // records reconnect_address_ for the close-side reconnect spawn.
        if !self.is_factory_bound() {
            client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text("srpc::ClientConnection::connect: factory not bound. Channel mode requires a ChannelFactoryProxy installed via Client::set_channel_factory(...) or auto-installed by Client::connect (the latter happens unconditionally now)."));
            self.state_machine_.transition_to(ConnectionState::FAILED);
            self.invoke_error_callback(CLIENT_ERR_INVALID_ARGUMENT, &client_text("no channel factory bound"));
            return CLIENT_ERR_INVALID_ARGUMENT;
        }
        self.connect_via_factory(addr)
    }
    fn bind_channel(&self, channel: ChannelConnectionProxy) {
        if !channel.is_valid() {
            return;
        }
        // Move the proxy into a heap-allocated FiberChannel. FiberChannel is
        // move-deleted (its callbacks capture `this`), so the emitted C++
        // lowers `Box::new(FiberChannel::new(..))` to the in-place
        // `emplace_with` seam: the factory's returned prvalue constructs the
        // channel directly in its final heap slot (guaranteed copy elision).
        // bind_callbacks must run AFTER the Box holds that final address so
        // its [this]-captures pin to a stable location.
        {
            let mut guard = self.fiber_channel_.lock().unwrap();
            *guard = Some(Box::new(FiberChannel::new(channel)));
            let fc: &mut Box<FiberChannel> = (*guard).as_mut().unwrap();
            (*fc).bind_callbacks();
        }
        self.channel_mode_.set(true);

        // Capture a Weak so the parked recv-loop fiber doesn't extend the
        // connection lifetime (would cycle through fiber_channel_ ownership).
        let weak_self: WeakClientConnection = self.weak_self_.clone();
        // The recv-loop fiber must live on the reactor that fires the proxy's
        // on_frame/on_closed callbacks (single-threaded IntEvent signaling);
        // the caller picks the thread (see bind_channel_via_poll_thread).
        // SAFETY: foreign named-module boundary; the file pointer is null and
        // the closure owns everything it captures.
        unsafe { Fiber::create_run_impl(move || {
            let conn_opt = weak_self.upgrade();
            if conn_opt.is_none() {
                return;
            }
            let conn = conn_opt.unwrap();
            (*conn).run_recv_loop();
        }, core::ptr::null(), 0) };
    }
    fn bind_channel_via_poll_thread(&self, channel: ChannelConnectionProxy) { clientconn_bind_channel_via_poll_thread(self, channel); }
    // Direct on_frame / on_closed binding: bypasses FiberChannel and the
    // recv-loop fiber entirely, installing the callbacks on the proxy itself.
    // Both fire on whichever thread the channel layer dispatches from -- for
    // TCP that is the poll thread, the same one whose handle_read parses the
    // frames. send_frame remains callable from any thread (dispatch_frame_via
    // _channel uses it from user threads).
    //
    // Callbacks are installed BEFORE the proxy moves into `direct_channel_`.
    // Once it is in the slot, dropping the slot drops the callbacks, so any
    // in-flight dispatch must complete before the drop -- the same contract
    // the FiberChannel destructor honours.
    // clippy::type_complexity -- the same spelling rpc/fiber_channel.cpp uses for this callback; factoring it into an alias would emit a new `using`. See the Task-2 measurement block above.
    #[allow(clippy::type_complexity)]
    fn bind_channel_direct(&self, mut channel: ChannelConnectionProxy) {
        if !channel.is_valid() {
            return;
        }
        // The channel is owned by this connection, so callback teardown occurs
        // before its receiver storage is released. Store the pinned receiver
        // address as an integer so the cross-thread callback's capture itself
        // is Send+Sync (the same pattern used by FiberChannel).
        let frame_self = self as *const ClientConnection as usize;
        let closed_self = frame_self;
        {
            // Concrete `Box<..>`, not the ChannelConnectionProxy alias: through
            // the alias the pointer-like check fails and the calls lower to
            // `channel.set_on_frame(..)` (dot) instead of `->` (docs 7.50).
            let ch: &mut Box<dyn ChannelConnectionBase> = &mut channel;
            ch.set_on_frame(OnFrameCallback::from_callable(Box::new(move |f: &ChannelFrame| {
                // SAFETY: the owning connection retains and tears down this
                // callback before its own storage is released.
                unsafe { (*(frame_self as *const ClientConnection))
                    .decode_response_and_notify(f.payload, f.size) };
            })));
            ch.set_on_closed(OnClosedCallback::from_callable(Box::new(move |_reason: ChannelError| {
                // SAFETY: same owner/teardown invariant as the frame callback.
                unsafe { (*(closed_self as *const ClientConnection))
                    .on_channel_closed_fan_out() };
            })));
            // on_error is not surfaced in this mode: the channel-layer contract
            // follows a fatal error with on_closed, so the fan-out covers it.
            //
            // Bound to an ANNOTATED local first, exactly as `fiber_channel.cpp`
            // does. Inline, the empty-bodied closure is emitted twice — once
            // inside a `decltype(...)` for `rusty::Box<..>::new_`'s type
            // argument and once as its value — and two lambda expressions are
            // two distinct C++ types, so the call never resolves.
            let error_callback: Box<dyn Fn(ChannelError, &str) + Send + Sync> =
                Box::new(move |_err, _msg| {});
            ch.set_on_error(OnErrorCallback::from_callable(error_callback));
        }
        {
            let mut guard = self.direct_channel_.lock().unwrap();
            *guard = Some(channel);
        }
        self.channel_mode_.set(true);
    }
    fn bind_factory(&self, factory: ChannelFactoryProxy) {
        if !factory.is_valid() {
            return;
        }
        let mut guard = self.factory_.lock().unwrap();
        *guard = Some(factory);
    }
    fn abort_reconnect(&mut self) { self.reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release); }
    fn set_callback_manager(&mut self, callback_manager: &Arc<CallbackManager>) {
        if callback_manager.is_valid() {
            self.callback_manager_ = callback_manager.clone();
        }
    }

    // --- delegating methods (&self → const free fns) ---
    // clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
    #[allow(clippy::explicit_auto_deref)]
    fn invalidate_pending_futures(&self) {
        // Drain the disconnect buffer FIRST. A request queued by the
        // `!is_connected()` branch of clientconn_request_via_channel never
        // enters `pending_fu_` -- it returns as soon as the enqueue succeeds,
        // so the queued callback is the ONLY thing holding that future's
        // notification path. Draining `pending_cb_slots_` and `pending_fu_`
        // below cannot reach it, and without this the callback was simply
        // destroyed with the queue: the waiter got a 1s timeout and
        // ETIMEDOUT instead of a connection error, and a callback-style
        // caller (which is what mako's generated proxies use) was never
        // called at all.
        //
        // There is no double-notify: the buffered and in-flight paths are
        // disjoint by construction, as above.
        //
        // Safe from Drop as well as from close()/mark_closing(): this runs in
        // `Drop::drop` before any field is dropped, so the raw `conn_ptr` the
        // queued callback uses to reach `metrics_` is still live.
        self.pending_queue_.clear_all(CLIENT_ERR_NOT_CONNECTED);

        // Drain the slim async-callback slots first. Take each callback out
        // under the lock via Option::take (mem::take leaves None behind, so
        // the fixed-size slot vector keeps its shape), then fire them outside
        // the lock with CLIENT_ERR_NOT_CONNECTED + a null reply view.
        let mut drained_callbacks: Vec<AsyncReplyCallback> = Vec::new();
        {
            let mut cb_guard = self.pending_cb_slots_.lock().unwrap();
            let mut i: usize = 0;
            while i < cb_guard.len() {
                if cb_guard[i].is_some() {
                    drained_callbacks.push(std::mem::take(&mut cb_guard[i]).unwrap());
                }
                i += 1usize;
            }
        }
        for cb in &mut drained_callbacks {
            self.metrics_.record_request_dropped();
            cb(CLIENT_ERR_NOT_CONNECTED, core::ptr::null(), 0);
        }

        // Drain the pending-future map in one pass: HashMap::drain() empties
        // the map as it yields each (xid, Arc<Future>) entry, replacing the
        // prior iterate-then-clear. The lock is held through the notify loop
        // below, matching the original (the map is already empty by then).
        let mut futures: Vec<Arc<Future>> = Vec::new();
        let mut guard = self.pending_fu_.lock().unwrap();
        for (_xid, fu) in guard.drain() {
            futures.push(fu);
        }
        for fu in &futures {
            self.metrics_.record_request_dropped();
            (*fu).error_code_.set(CLIENT_ERR_NOT_CONNECTED);
            (*fu).notify_ready(fu.clone());
        }
    }
    // clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
    // clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
    #[allow(clippy::explicit_auto_deref, clippy::unnecessary_unwrap)]
    fn fail_pending_future(&self, xid: i64, err: i32) {
        let mut fu_opt: Option<Arc<Future>> = None;
        {
            let mut pending_guard = self.pending_fu_.lock().unwrap();
            let fu_ptr = (*pending_guard).get(&xid);
            if fu_ptr.is_some() {
                fu_opt = Some(fu_ptr.unwrap().clone());
                (*pending_guard).remove(&xid);
            }
        }
        if fu_opt.is_some() {
            let fu = fu_opt.unwrap();
            self.metrics_.record_request_dropped();
            (*fu).error_code_.set(err);
            (*fu).notify_ready(fu.clone());
        }
    }
    fn close(&self) {
        let prev_state = self.state_machine_.state();
        let was_connected: bool = self.state_machine_.is_connected();
        if was_connected {
            self.state_machine_.transition_to(ConnectionState::DISCONNECTING);
        }

        // Tear down the channel proxy(ies). The channel layer's close() is
        // idempotent + thread-safe per the facade contract. The `&mut` local
        // is load-bearing: deref-through-a-guard-chain drops the deref
        // (docs 7.50), and a `&` binding would lower to `const Box<T>&`
        // while close() is &mut self.
        {
            let mut guard = self.direct_channel_.lock().unwrap();
            if (*guard).is_some() {
                let ch: &mut Box<dyn ChannelConnectionBase> = (*guard).as_mut().unwrap();
                (*ch).close();
            }
        }
        {
            let mut guard = self.fiber_channel_.lock().unwrap();
            if (*guard).is_some() {
                let fc: &mut Box<FiberChannel> = (*guard).as_mut().unwrap();
                (*fc).close();
            }
        }

        if was_connected {
            self.state_machine_.transition_to(ConnectionState::DISCONNECTED);
        } else if !self.state_machine_.is_terminal() {
            self.state_machine_.force_state(ConnectionState::DISCONNECTED);
        }
        self.heartbeat_manager_.reset();
        self.invalidate_pending_futures();

        if (prev_state as i32) == (ConnectionState::CONNECTED as i32)
            || (prev_state as i32) == (ConnectionState::DISCONNECTING as i32) {
            self.invoke_disconnected_callback();
        }
    }
    fn mark_closing(&self) {
        self.reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release);
        if self.state_machine_.is_connected() {
            self.state_machine_.transition_to(ConnectionState::DISCONNECTING);
        }
        self.invalidate_pending_futures();
    }
    fn reconnect(&self, on_complete: OnReconnectCompleteCallbackFn) -> i32 { clientconn_reconnect(self, on_complete) }
    fn set_buffering_config(&self, config: &BufferingConfig) {
        self.buffering_config_.set(*config);
        if !self.pending_queue_.empty() {
            self.pending_queue_.clear_all(CLIENT_ERR_CONNECTION_ABORTED);
        }
        self.pending_queue_.update_config(config.to_queue_config());
    }
    fn set_heartbeat_config(&self, config: &HeartbeatConfig) {
        self.heartbeat_manager_.set_config(config);
        // Capture a weak self-handle by move so the escaping timeout closure
        // does not keep the connection alive (mirrors the legacy [weak_conn]
        // C++ lambda; a move closure's owned capture is escape-safe).
        let weak_conn: WeakClientConnection = self.weak_self_.clone();
        // Provider alias, not an inline turbofish — see `qr.callback` below.
        self.heartbeat_manager_.set_on_timeout(
            HeartbeatTimeoutCallback::from_callable(move || {
            let conn_opt = weak_conn.upgrade();
            if conn_opt.is_none() {
                return;
            }
            let conn = conn_opt.unwrap();
            if !(*conn).connected() {
                return;
            }
            client_log_line(Log::WARN, 0i32, core::ptr::null(), client_text_str("srpc::ClientConnection: heartbeat timeout for ", &(*conn).host(), ""));
            (*conn).handle_error();
            }),
        );
    }
    fn heartbeat_config(&self) -> HeartbeatConfig { self.heartbeat_manager_.config() }
    fn set_circuit_breaker_config(&self, config: &CircuitBreakerConfig) { self.circuit_breaker_.set_config(*config); }
    fn circuit_breaker_config(&self) -> CircuitBreakerConfig { self.circuit_breaker_.config() }
    fn enqueue_heartbeat_probe(&self) { clientconn_enqueue_heartbeat_probe(self); }
    fn allow_request_with_circuit_metrics(&self) -> bool {
        let before = self.circuit_breaker_.state();
        let allowed = self.circuit_breaker_.allow_request();
        let after = self.circuit_breaker_.state();
        self.record_circuit_state_transition(before, after);
        if !allowed {
            self.metrics_.record_circuit_open_rejection();
        }
        allowed
    }
    fn record_circuit_state_transition(&self, before: CircuitState, after: CircuitState) {
        if before == after {
            return;
        }
        match after {
            CircuitState::OPEN => self.metrics_.record_circuit_open_transition(),
            CircuitState::HALF_OPEN => self.metrics_.record_circuit_half_open_transition(),
            CircuitState::CLOSED => self.metrics_.record_circuit_closed_transition(),

        }
    }
    fn record_circuit_result(&self, err: i32) {
        let before = self.circuit_breaker_.state();
        if err == 0i32 {
            self.circuit_breaker_.record_success();
        } else if Self::should_trip_circuit_for_error(err) {
            self.circuit_breaker_.record_failure();
        }
        let after = self.circuit_breaker_.state();
        self.record_circuit_state_transition(before, after);
    }
    // `&LegacyStdString`, NOT `&str`: the incumbent module exported
    // `invoke_error_callback(int, std::string const&) const`, and `&str`
    // re-signatures it to `std::string_view`. That is the classic
    // natural-looking Rust-port improvement that silently breaks the C++ ABI,
    // so the parameter keeps the mapped `const std::string&` spelling and the
    // literal call sites build the owned string the incumbent also built.
    fn invoke_error_callback(&self, err: i32, message: &LegacyStdString) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_error(clientconn_map_system_error(err), message);
    }
    fn invoke_disconnected_callback(&self) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_disconnected();
    }
    fn invoke_reconnecting_callback(&self) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_reconnecting();
    }
    fn invoke_reconnected_callback(&self, success: bool) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_reconnected(success);
    }
    fn invoke_connected_callback(&self) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_connected();
    }
    unsafe fn dispatch_frame_via_channel(&self, body_bytes: *const u8, body_size: usize) -> ChannelError {
        clientconn_dispatch_frame_via_channel(self, body_bytes, body_size)
    }
    // clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
    #[allow(clippy::explicit_auto_deref)]
    fn handle_error(&self) {
        let prev_state = self.state_machine_.state();
        let abort_flag: bool = self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        let user_initiated_closing: bool =
            (prev_state as i32) == (ConnectionState::DISCONNECTING as i32)
            || (prev_state as i32) == (ConnectionState::DISCONNECTED as i32)
            || abort_flag;

        if !user_initiated_closing {
            self.invoke_error_callback(CLIENT_ERR_CONNECTION_RESET, &client_text("connection error"));
            self.state_machine_.force_state(ConnectionState::FAILED);
        }
        self.close();

        if user_initiated_closing {
            return;
        }
        self.invoke_disconnected_callback();

        // Trigger policy-driven reconnect automatically after transport failures.
        let reconnect_aborted: bool = self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if self.reconnect_policy_.get().auto_reconnect && !reconnect_aborted {
            let addr: LegacyStdString = self.reconnect_address_.get();
            if addr.is_empty() {
                return;
            }
            let weak_conn: WeakClientConnection = self.weak_self_.clone();
            rusty::thread::spawn(move || {
                let conn_opt = weak_conn.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                let conn_aborted: bool = (*conn).reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
                if !(*conn).reconnect_policy_.get().auto_reconnect || conn_aborted {
                    return;
                }
                let state = (*conn).connection_state();
                if (state as i32) == (ConnectionState::FAILED as i32)
                    || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                    client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text("srpc::ClientConnection: auto-reconnect triggered after connection failure"));
                    (*conn).reconnect(Default::default());
                }
            }).detach();
        }
    }
    fn check_pending_write_update(&self) -> bool {
        if self.state_machine_.is_connected() && !self.paused_.get() {
            if self.heartbeat_manager_.check_timeout() {
                return false;
            }
            if self.heartbeat_manager_.should_send_heartbeat() {
                self.enqueue_heartbeat_probe();
                self.heartbeat_manager_.on_heartbeat_sent();
                return true;
            }
        }
        false
    }
    fn handle_free(&self, xid: i64) {
        let mut guard = self.pending_fu_.lock().unwrap();
        if guard.remove(&xid).is_some() {
            self.metrics_.record_request_dropped();
        }
    }
    fn is_factory_bound(&self) -> bool { (*self.factory_.lock().unwrap()).is_some() }
    fn channel_reconnect_attempts_count(&self) -> u64 { self.reconnect_.channel_reconnect_attempts_.load(rusty::sync::atomic::Ordering::Acquire) }
    fn set_reconnect_policy(&self, policy: &ReconnectPolicy) { self.reconnect_policy_.set(*policy); }
    fn is_reconnecting(&self) -> bool { self.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire) }
    fn pending_future_count(&self) -> usize { self.pending_fu_.lock().unwrap().len() }
    fn replay_pending_requests_for_test(&self) -> usize { self.replay_pending_requests() }
    fn update_pending_queue_config_for_test(&self, config: &RequestQueueConfig) { self.pending_queue_.update_config(*config); }
    fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) { self.on_server_restart_.replace(callback); }
    fn check_server_instance(&self, new_id: u64) -> bool {
        let old_id = self.server_instance_id_.get();
        self.server_instance_id_.set(new_id);
        if old_id != 0u64 && old_id != new_id {
            client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text_u64_pair("Server restart detected: old_id=", old_id, " new_id=", new_id, ""));
            let mut cb_ref = self.on_server_restart_.borrow_mut();
            if !cb_ref.is_empty() {
                (*cb_ref)(old_id, new_id);
            }
            return true;
        }
        false
    }
    fn set_keepalive(&self, config: &KeepaliveConfig) { self.keepalive_config_.set(*config); }
    fn on_request_dispatched(&self, bytes: usize) {
        self.metrics_.record_bytes_sent(bytes as u64);
        self.update_last_activity(clientconn_monotonic_ms_now());
    }
    fn on_response_received(&self, bytes: usize) {
        self.metrics_.record_bytes_received(bytes as u64);
        self.update_last_activity(clientconn_monotonic_ms_now());
    }
    fn host(&self) -> LegacyStdString { self.host_.clone() }

    // --- static delegators ---
    fn should_trip_circuit_for_error(err: i32) -> bool {
        if err == 0i32 {
            return false;
        }
        if err == CLIENT_ERR_NOT_CONNECTED || err == CLIENT_ERR_CONNECTION_REFUSED || err == CLIENT_ERR_CONNECTION_RESET
            || err == CLIENT_ERR_CONNECTION_ABORTED || err == CLIENT_ERR_TIMED_OUT || err == CLIENT_ERR_HOST_UNREACHABLE
            || err == CLIENT_ERR_NETWORK_UNREACHABLE || err == CLIENT_ERR_BROKEN_PIPE {
            return true;
        }
        false
    }
    fn map_system_error(err: i32) -> RpcError { clientconn_map_system_error(err) }

    // --- generic request trio ---
    fn request<F>(&self, rpc_id: i32, attr: &FutureAttr, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) { clientconn_request_via_channel(self, rpc_id, attr, write_fn) }
    fn request_with_options<F>(&self, rpc_id: i32, options: &RequestOptions, attr: &FutureAttr, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) { clientconn_request_with_options(self, rpc_id, options, attr, write_fn) }
    fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: AsyncReplyCallback) -> Result<(), i32>
    where F: FnMut(&mut BinaryWriteArchive) { clientconn_request_async(self, rpc_id, write_fn, on_reply) }

    // --- trivial inline accessors ---
    fn is_channel_mode(&self) -> bool { self.channel_mode_.get() }
    fn install_self_weak_for_testing(&mut self, weak: WeakClientConnection) { self.weak_self_ = weak; }
    fn force_connected_for_testing(&mut self) { self.state_machine_.force_state(ConnectionState::CONNECTED); }
    fn set_reconnect_address_for_testing(&self, addr: LegacyStdString) { self.reconnect_address_.set(addr); }
    fn connected(&self) -> bool { self.state_machine_.is_connected() }
    fn connection_state(&self) -> ConnectionState { self.state_machine_.state() }
    fn reconnect_policy(&self) -> ReconnectPolicy { self.reconnect_policy_.get() }
    fn buffering_config(&self) -> BufferingConfig { self.buffering_config_.get() }
    fn pending_request_count(&self) -> usize { self.pending_queue_.size() }
    fn clear_pending_requests(&self, error_code: i32) { self.pending_queue_.clear_all(error_code); }
    fn server_instance_id(&self) -> u64 { self.server_instance_id_.get() }
    fn keepalive_config(&self) -> KeepaliveConfig { self.keepalive_config_.get() }
    fn circuit_breaker_state(&self) -> CircuitState { self.circuit_breaker_.state() }
    fn update_last_activity(&self, current_time_ms: u64) { self.last_activity_time_.set(current_time_ms); }
    fn last_activity_time(&self) -> u64 { self.last_activity_time_.get() }
    fn is_idle(&self, idle_ms: u64, current_time_ms: u64) -> bool {
        let last: u64 = self.last_activity_time_.get();
        if last == 0u64 { return false; }
        (current_time_ms - last) > idle_ms
    }
    fn validate_connection(&self) -> bool { self.state_machine_.is_connected() }
    fn metrics(&self) -> &ConnectionMetrics { &self.metrics_ }
    fn replay_pending_requests(&self) -> usize { 0usize }
    fn apply_keepalive_options(&mut self) {}
    fn fd(&self) -> i32 { -1 }
    fn pause(&self) { self.paused_.set(true); }
    fn resume(&self) { self.paused_.set(false); }
    fn poll_mode(&self) -> i32 { CLIENT_POLL_READ }
    fn content_size(&self) -> usize { 0usize }
    fn handle_write(&self) -> i32 { CLIENT_POLL_NO_CHANGE }
    fn handle_read(&self) -> bool { false }
    fn is_closed(&self) -> bool { self.state_machine_.is_terminal() }
}

pub struct Client {
    connection_field: RefCell<Option<Arc<ClientConnection>>>,
    poll_thread_worker_field: Arc<PollThread>,
    is_client_mode_field: Cell<bool>,
    time_field: Cell<i64>,
    timeout_field: Cell<u64>,
    rpc_id_field: Cell<i32>,
    pending_keepalive_config_field: Cell<KeepaliveConfig>,
    pending_heartbeat_config_field: Cell<HeartbeatConfig>,
    pending_circuit_breaker_config_field: Cell<CircuitBreakerConfig>,
    pending_reconnect_policy_field: Cell<ReconnectPolicy>,
    callback_manager_field: Arc<CallbackManager>,
    pending_factory_field: rusty::Mutex<Option<ChannelFactoryProxy>>,
    // Per-Client empty metrics used as the no-connection fallback by
    // `metrics()` (returns a live ref). Per-instance rather than
    // program-global so a `static const ConnectionMetrics` isn't
    // needed in the DSL. Cheap because ConnectionMetrics is just 18
    // Atomic<u64> fields.
    empty_metrics_field: ConnectionMetrics,
}

impl Drop for Client {
    fn drop(&mut self) {
        self.close();
    }
}

impl Client {
    fn new(poll_thread_worker: Arc<PollThread>) -> Client {
        Client {
            connection_field: RefCell::<Option<Arc<ClientConnection>>>::new(None),
            poll_thread_worker_field: poll_thread_worker,
            is_client_mode_field: Cell::<bool>::new(false),
            time_field: Cell::<i64>::new(0i64),
            timeout_field: Cell::<u64>::new(0u64),
            rpc_id_field: Cell::<i32>::new(0i32),
            pending_keepalive_config_field: Cell::<KeepaliveConfig>::new(KeepaliveConfig::new()),
            pending_heartbeat_config_field: Cell::<HeartbeatConfig>::new(HeartbeatConfig::disabled()),
            pending_circuit_breaker_config_field: Cell::<CircuitBreakerConfig>::new(CircuitBreakerConfig::disabled()),
            pending_reconnect_policy_field: Cell::<ReconnectPolicy>::new(ReconnectPolicy::conservative()),
            callback_manager_field: Arc::<CallbackManager>::new(CallbackManager::new()),
            pending_factory_field: rusty::Mutex::<Option<ChannelFactoryProxy>>::new(None),
            empty_metrics_field: ConnectionMetrics::new(),
        }
    }

    // clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
    #[allow(clippy::arc_with_non_send_sync)]
    fn create(poll_thread_worker: Arc<PollThread>) -> Arc<Client> {
        Arc::<Client>::new(Client::new(poll_thread_worker))
    }

    fn set_client_mode(&self, v: bool) { self.is_client_mode_field.set(v); }
    fn client_mode(&self) -> bool { self.is_client_mode_field.get() }
    fn set_time(&self, v: i64) { self.time_field.set(v); }
    fn time(&self) -> i64 { self.time_field.get() }
    fn set_timeout(&self, v: u64) { self.timeout_field.set(v); }
    fn timeout(&self) -> u64 { self.timeout_field.get() }
    fn set_rpc_id(&self, v: i32) { self.rpc_id_field.set(v); }
    fn rpc_id(&self) -> i32 { self.rpc_id_field.get() }

    fn request<F>(&self, rpc_id: i32, attr: &FutureAttr, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request(rpc_id, attr, write_fn)
    }

    fn request_with_options<F>(&self, rpc_id: i32, options: &RequestOptions, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request_with_options(
            rpc_id,
            options,
            &FutureAttr { callback: Default::default() },
            write_fn,
        )
    }

    fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: AsyncReplyCallback) -> Result<(), i32>
    where F: FnMut(&mut BinaryWriteArchive) {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request_async(rpc_id, write_fn, on_reply)
    }

    fn set_valid(&self, _valid: bool) {}

    fn connect(&self, addr: *const i8, client: bool) -> i32 {
        let conn: Arc<ClientConnection> = Arc::new_cyclic(|weak_conn| {
            let mut value = ClientConnection::new(self.poll_thread_worker_field.clone());
            value.weak_self_ = weak_conn.clone();
            value.callback_manager_ = self.callback_manager_field.clone();
            value.is_client_mode_ = client;
            value
        });
        self.is_client_mode_field.set(client);

        conn.set_keepalive(&self.pending_keepalive_config_field.get());
        conn.set_heartbeat_config(&self.pending_heartbeat_config_field.get());
        conn.set_circuit_breaker_config(&self.pending_circuit_breaker_config_field.get());
        conn.set_reconnect_policy(&self.pending_reconnect_policy_field.get());

        if !self.has_pending_channel_factory() {
            let tcp_factory: Arc<TcpFactory> = Arc::<TcpFactory>::new(TcpFactory::new(self.poll_thread_worker_field.clone()));
            self.set_channel_factory(make_tcp_factory_proxy(tcp_factory));
        }

        {
            let mut guard = self.pending_factory_field.lock().unwrap();
            if guard.is_some() {
                let moved: ChannelFactoryProxy = guard.take().unwrap();
                conn.bind_factory(moved);
            }
        }

        let result: i32 = conn.connect(addr);

        if result == 0i32 {
            let mut store_guard = self.connection_field.borrow_mut();
            *store_guard = Some(conn);
        }

        result
    }

    // clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
    #[allow(clippy::arc_with_non_send_sync)]
    fn close(&self) {
        let guard = self.connection_field.borrow_mut();
        if guard.is_some() {
            let conn_ref = guard.as_ref().unwrap();
            let was_connected: bool = conn_ref.connected();
            conn_ref.mark_closing();
            if was_connected {
                let conn_arc: Arc<ClientConnection> = conn_ref.clone();
                // NOTE: keep the trailing-underscore C++ spelling here.
                // When written as Rust-idiomatic `::new(...)`, the transpiler
                // adds a spurious `-> Arc<PollThread>` return type to
                // the inner lambda (inferred from the next statement's
                // receiver type) and the lambda body becomes ill-typed.
                // Tracked as a transpiler bug; use `::new_(...)` until fixed.
                let close_job: Arc<OneTimeJob> =
                    Arc::<OneTimeJob>::new(OneTimeJob::new(Box::new(move || {
                        conn_arc.close();
                    })));
                // Implicit Arc<OneTimeJob> -> Arc<Job> upcast via rusty::Arc's
                // template ctor (U* convertible to T*).
                // SAFETY: foreign named-module boundary; the job handle is
                // freshly built and uniquely owned here.
                unsafe { self.poll_thread_worker_field.add(close_job) };
            }
        }
    }

    fn handle_free(&self, xid: i64) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().handle_free(xid);
        }
    }

    fn pause(&self) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().pause();
        }
    }

    fn resume(&self) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().resume();
        }
    }

    fn reconnect(&self, mut on_complete: OnReconnectCompleteCallbackFn) -> i32 {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            if !on_complete.is_empty() {
                on_complete(false);
            }
            return CLIENT_ERR_NOT_CONNECTED;
        }
        guard.as_ref().unwrap().reconnect(on_complete)
    }

    fn set_channel_factory(&self, factory: ChannelFactoryProxy) {
        if !factory.is_valid() {
            return;
        }
        let mut guard = self.pending_factory_field.lock().unwrap();
        *guard = Some(factory);
    }

    fn has_pending_channel_factory(&self) -> bool {
        let guard = self.pending_factory_field.lock().unwrap();
        guard.is_some()
    }

    fn pending_request_count(&self) -> usize {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().pending_request_count();
        }
        0usize
    }

    fn clear_pending_requests(&self, error_code: i32) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().clear_pending_requests(error_code);
        }
    }

    fn is_reconnecting(&self) -> bool {
        let guard = self.connection_field.borrow();
        guard.is_some() && guard.as_ref().unwrap().is_reconnecting()
    }

    fn host(&self) -> LegacyStdString {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().host();
        }
        // See `ClientConnection::new`: the alias maps to `std::string`, so
        // `LegacyStdString::new()` would emit `std::string::new_`.
        Default::default()
    }

    fn connected(&self) -> bool {
        let guard = self.connection_field.borrow();
        guard.is_some() && guard.as_ref().unwrap().connected()
    }

    fn connection_state(&self) -> ConnectionState {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().connection_state();
        }
        ConnectionState::NEW
    }

    fn try_reconnect_if_needed(&self) -> bool {
        let state: ConnectionState = self.connection_state();
        if (state as i32) == (ConnectionState::CONNECTED as i32) {
            return true;
        }
        if (state as i32) == (ConnectionState::FAILED as i32)
            || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
            let result: i32 = self.reconnect(Default::default());
            return result == 0i32;
        }
        false
    }

    fn connection(&self) -> Option<Arc<ClientConnection>> {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return Some(guard.as_ref().unwrap().clone());
        }
        None
    }

    fn server_instance_id(&self) -> u64 {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().server_instance_id();
        }
        0u64
    }

    fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_on_server_restart(callback);
        }
    }

    fn check_server_instance(&self, new_id: u64) -> bool {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().check_server_instance(new_id);
        }
        false
    }

    fn set_reconnect_policy(&self, policy: &ReconnectPolicy) {
        self.pending_reconnect_policy_field.set(*policy);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_reconnect_policy(policy);
        }
    }

    fn set_buffering_config(&self, config: &BufferingConfig) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_buffering_config(config);
        }
    }

    fn set_keepalive(&self, config: &KeepaliveConfig) {
        self.pending_keepalive_config_field.set(*config);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_keepalive(config);
        }
    }

    fn keepalive_config(&self) -> KeepaliveConfig {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().keepalive_config();
        }
        self.pending_keepalive_config_field.get()
    }

    fn set_heartbeat(&self, config: &HeartbeatConfig) {
        self.pending_heartbeat_config_field.set(*config);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_heartbeat_config(config);
        }
    }

    fn heartbeat_config(&self) -> HeartbeatConfig {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().heartbeat_config();
        }
        self.pending_heartbeat_config_field.get()
    }

    fn set_circuit_breaker(&self, config: &CircuitBreakerConfig) {
        self.pending_circuit_breaker_config_field.set(*config);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_circuit_breaker_config(config);
        }
    }

    fn circuit_breaker_config(&self) -> CircuitBreakerConfig {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().circuit_breaker_config();
        }
        self.pending_circuit_breaker_config_field.get()
    }

    fn circuit_breaker_state(&self) -> CircuitState {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().circuit_breaker_state();
        }
        CircuitState::CLOSED
    }

    fn is_idle(&self, idle_ms: u64, current_time_ms: u64) -> bool {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().is_idle(idle_ms, current_time_ms);
        }
        false
    }

    fn validate_connection(&self) -> bool {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().validate_connection();
        }
        false
    }

    fn metrics(&self) -> &ConnectionMetrics { &self.empty_metrics_field }

    fn has_connection(&self) -> bool {
        let guard = self.connection_field.borrow();
        guard.is_some()
    }

    fn add_on_connected(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_connected(cb);
    }
    fn add_on_disconnected(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_disconnected(cb);
    }
    fn add_on_error(&self, cb: OnErrorCallbackFn) {
        self.callback_manager_field.add_on_error(cb);
    }
    fn add_on_reconnecting(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_reconnecting(cb);
    }
    fn add_on_reconnected(&self, cb: OnReconnectedCallbackFn) {
        self.callback_manager_field.add_on_reconnected(cb);
    }
    fn clear_connection_callbacks(&self) {
        self.callback_manager_field.clear_all();
    }
}

pub struct PoolState {
    cache: BTreeMap<LegacyStdString, Vec<Arc<Client>>>,
    lb_state: BTreeMap<LegacyStdString, LoadBalancerState>,
}

impl PoolState {
    fn new() -> PoolState {
        PoolState {
            cache: BTreeMap::<LegacyStdString, Vec<Arc<Client>>>::new(),
            lb_state: BTreeMap::<LegacyStdString, LoadBalancerState>::new(),
        }
    }
}

// `config_` is a Mutex, not a Cell. PoolConfig is ~40 bytes of plain
// members, so a Cell read racing a `set_pool_config` write is a torn
// read — UB, not merely a stale value. set_pool_config is public and
// callable at any time, so that race is reachable.
//
// LOCK ORDER INVARIANT: never acquire `config_` while holding `state_`.
// Every kernel snapshots the config FIRST and then takes `state_`, so the
// two locks are never held together and there is no ordering hazard. This
// is why the health check takes its config as an argument
// (`clientpool_is_client_healthy_with`) rather than reading `config_`
// itself — it is called from inside the `state_` critical section, and
// re-reading there would invert the order against `get_client`.
pub struct ClientPool {
    poll_thread_worker_: Option<Arc<PollThread>>,
    state_: rusty::Mutex<PoolState>,
    config_: rusty::Mutex<PoolConfig>,
}

impl Drop for ClientPool {
    // clippy::for_kv_map -- measured: emits `.values()` in place of the tuple-destructuring for loop. See the Task-2 measurement block above.
    // clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
    #[allow(clippy::for_kv_map, clippy::unnecessary_unwrap)]
    fn drop(&mut self) {
        let guard = self.state_.lock().unwrap();
        for (_addr, clients) in guard.cache.iter() {
            for client in clients {
                (*client).close();
            }
        }
        if self.poll_thread_worker_.is_some() {
            // SAFETY: foreign named-module boundary; the worker handle is
            // live for the duration of the call.
            unsafe { (*self.poll_thread_worker_.as_ref().unwrap()).shutdown() };
        }
    }
}

impl ClientPool {
    fn new(poll_thread_worker: Option<Arc<PollThread>>, config: PoolConfig) -> ClientPool {
        client_verify(config.min_connections > 0);
        client_verify(config.max_connections >= config.min_connections);
        let mut ptw: Option<Arc<PollThread>> = poll_thread_worker;
        if ptw.is_none() {
            // SAFETY: foreign named-module boundary; no caller precondition.
            ptw = Some(unsafe { PollThread::create() });
        }
        ClientPool {
            poll_thread_worker_: ptw,
            state_: rusty::Mutex::<PoolState>::new(PoolState::new()),
            config_: rusty::Mutex::<PoolConfig>::new(config),
        }
    }

    fn set_pool_config(&self, config: PoolConfig) {
        let mut guard = self.config_.lock().unwrap();
        (*guard) = config;
    }

    fn pool_config(&self) -> PoolConfig {
        let guard = self.config_.lock().unwrap();
        *guard
    }

    fn is_client_healthy(&self, client: &Arc<Client>) -> bool {
        clientpool_is_client_healthy_with(self.pool_config(), client)
    }

    fn get_healthy_client_count(&self, addr: &LegacyStdString) -> usize {
        clientpool_get_healthy_client_count(self, addr)
    }

    // clippy::for_kv_map -- measured: emits `.values()` in place of the tuple-destructuring for loop. See the Task-2 measurement block above.
    #[allow(clippy::for_kv_map)]
    fn total_client_count(&self) -> usize {
        let guard = self.state_.lock().unwrap();
        let mut count: usize = 0;
        for (_addr, clients) in guard.cache.iter() {
            count += clients.len();
        }
        count
    }

    fn address_count(&self) -> usize {
        let guard = self.state_.lock().unwrap();
        guard.cache.len()
    }

    fn remove_unhealthy_clients(&self, addr: &LegacyStdString) -> usize {
        clientpool_remove_unhealthy_clients(self, addr)
    }

    fn close_idle_clients(&self, addr: &LegacyStdString, current_time_ms: u64) -> usize {
        clientpool_close_idle_clients(self, addr, current_time_ms)
    }

    fn remove_all_unhealthy(&self) -> usize {
        clientpool_remove_all_unhealthy(self)
    }

    fn close_all_idle(&self, current_time_ms: u64) -> usize {
        clientpool_close_all_idle(self, current_time_ms)
    }

    fn get_client(&self, addr: &LegacyStdString) -> Option<Arc<Client>> {
        clientpool_get_client(self, addr)
    }
}

pub fn make_pending_queue(c: &RequestQueueConfig) -> RequestQueue {
    RequestQueue::with_config(*c)
}

pub fn clientconn_monotonic_ms_now() -> u64 { rusty::sys::time::clock_monotonic_us() / 1000 }

// clippy::unnecessary_cast -- measured: drops the emitted rusty::detail::ptr_cast<const int8_t*>. See the Task-2 measurement block above.
#[allow(clippy::unnecessary_cast)]
pub fn clientconn_reconnect(self_: &ClientConnection, mut on_complete: OnReconnectCompleteCallbackFn) -> i32 {
    // Reset the abort latch before delegating (folded in from the former
    // const `reconnect` facade): the Client::reconnect path needs a stale
    // abort=true from a prior close() cleared, and the close-fan-out spawn
    // path only reaches here with abort already false, so the reset is a
    // no-op there. `reconnect_` is a mutable atomic, so const self suffices.
    self_.reconnect_.reconnect_abort_.store(false, rusty::sync::atomic::Ordering::Release);

    let mut complete_callback = |result: i32| -> i32 {
        if !on_complete.is_empty() {
            on_complete(result == 0i32);
        }
        result
    };

    let aborted: bool = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    if aborted {
        return complete_callback(CLIENT_ERR_CANCELED);
    }

    let wait_for_inflight_reconnect = || -> i32 {
        loop {
            let busy: bool = self_.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire);
            if !busy {
                break;
            }
            let cancel: bool = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
            if cancel {
                return CLIENT_ERR_CANCELED;
            }
            if self_.state_machine_.is_connected() {
                return 0i32;
            }
            Time::sleep(5000u64);
        }
        if self_.state_machine_.is_connected() {
            return 0i32;
        }
        CLIENT_INT_MIN
    };

    let reconnecting: bool = self_.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire);
    if reconnecting {
        let waited: i32 = wait_for_inflight_reconnect();
        if waited != CLIENT_INT_MIN {
            return complete_callback(waited);
        }
    }

    // Check if we have an address to reconnect to
    if self_.reconnect_address_.get().is_empty() {
        client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text("srpc::ClientConnection: no address to reconnect to"));
        return complete_callback(CLIENT_ERR_INVALID_ARGUMENT);
    }

    // Can only reconnect from FAILED or DISCONNECTED state
    if !self_.state_machine_.can_connect() {
        client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text_str("srpc::ClientConnection: cannot reconnect from state ",
                  connection_state_to_string(self_.state_machine_.state()), ""));
        return complete_callback(CLIENT_ERR_INVALID_ARGUMENT);
    }

    loop {
        let expected: bool = false;
        let won: bool = {
            self_.reconnect_.reconnecting_.compare_exchange(expected, true,
                rusty::sync::atomic::Ordering::AcqRel,
                rusty::sync::atomic::Ordering::Acquire).is_ok()
        };
        if won {
            break;
        }
        let waited: i32 = wait_for_inflight_reconnect();
        if waited != CLIENT_INT_MIN {
            return complete_callback(waited);
        }
    }
    self_.invoke_reconnecting_callback();

    let mut complete_reconnect = |success: bool, result: i32| -> i32 {
        self_.reconnect_.reconnecting_.store(false, rusty::sync::atomic::Ordering::Release);
        self_.invoke_reconnected_callback(success);

        if success {
            client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text_str("srpc::ClientConnection: reconnected to ", &self_.reconnect_address_.get(), ""));

            // Record reconnection in metrics
            self_.metrics_.record_reconnect();

            // Sweep the disconnect-buffering queue. Entries that ran past
            // their TTL while the connection was down resolve their
            // futures with `kRequestQueueExpiredError` and bump
            // `queue_dropped_requests`. Non-stale entries remain in the
            // queue for a future replay path.
            self_.pending_queue_.expire_stale();
            return complete_callback(0i32);
        }
        if result == CLIENT_ERR_CANCELED {
            client_log_line(Log::DEBUG, 0i32, core::ptr::null(), client_text_str("srpc::ClientConnection: reconnect cancelled for ",
                      &self_.reconnect_address_.get(), ""));
        } else {
            client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text_str_i32("srpc::ClientConnection: reconnection failed to ",
                      &self_.reconnect_address_.get(), ": ", result, ""));
        }
        complete_callback(result)
    };

    let reconnect_once = || -> i32 {
        let cancel: bool = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if cancel {
            return CLIENT_ERR_CANCELED;
        }
        // 4g3c2: `socket_ = -1` reset removed. socket_ is unused in
        // channel mode (the channel proxy's TcpConnection owns the fd);
        // the `connect()` call below routes through `connect_via_factory`
        // which produces a fresh proxy + fresh fd internally.
        let address = self_.reconnect_address_.get();
        self_.connect(address.c_str() as *const i8)
    };

    let abort_now: bool = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    if abort_now {
        return complete_reconnect(false, CLIENT_ERR_CANCELED);
    }

    // Another reconnect attempt can complete between the pre-CAS state check and
    // this thread acquiring reconnect ownership.
    if self_.state_machine_.is_connected() {
        return complete_reconnect(true, 0i32);
    }

    if !self_.state_machine_.can_connect() {
        return complete_reconnect(false, CLIENT_ERR_INVALID_ARGUMENT);
    }

    // First attempt happens immediately.
    let mut result: i32 = reconnect_once();
    if result == 0i32 {
        return complete_reconnect(true, 0i32);
    }

    // Follow configured backoff/retry policy for subsequent attempts.
    let policy: ReconnectPolicy = self_.reconnect_policy_.get();
    let calc = crate::reconnect_policy::ReconnectCalculator::new(&policy);
    while calc.should_retry() {
        let cancel: bool = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if cancel {
            return complete_reconnect(false, CLIENT_ERR_CANCELED);
        }

        let delay_ms: u32 = calc.next_delay_ms();
        if delay_ms > 0u32 {
            Time::sleep((delay_ms as u64) * 1000u64);
        }

        let cancel2: bool = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if cancel2 {
            return complete_reconnect(false, CLIENT_ERR_CANCELED);
        }

        // Another path may have re-established connection while sleeping.
        if self_.state_machine_.is_connected() {
            return complete_reconnect(true, 0i32);
        }

        if !self_.state_machine_.can_connect() {
            return complete_reconnect(false, CLIENT_ERR_INVALID_ARGUMENT);
        }

        client_log_line(Log::DEBUG, 0i32, core::ptr::null(), client_text_u32_str("srpc::ClientConnection: reconnect retry #",
                  calc.retry_count(), " to ", &self_.reconnect_address_.get(), ""));
        result = reconnect_once();
        if result == 0i32 {
            return complete_reconnect(true, 0i32);
        }
    }

    complete_reconnect(false, result)
}

// clippy::borrowed_box -- the concrete Box spelling is load-bearing: through &T the pointer-like check fails and the calls lower to `.` instead of `->` (docs 7.50); measured. See the Task-2 measurement block above.
// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
#[allow(clippy::borrowed_box, clippy::explicit_auto_deref)]
pub fn clientconn_request_via_channel<F>(conn: &ClientConnection, rpc_id: i32,
                                     attr: &FutureAttr, mut write_fn: F) -> FutureResult
where F: FnMut(&mut BinaryWriteArchive) {
    if !conn.allow_request_with_circuit_metrics() {
        return FutureResult::Err(CLIENT_ERR_BUSY);
    }
    conn.pending_queue_.expire_stale();
    if !conn.state_machine_.is_connected() {
        let buffering_cfg = conn.buffering_config_.get();
        if buffering_cfg.enabled && buffering_cfg.behavior == DisconnectBehavior::QUEUE {
            let fu = Future::create(conn.xid_counter_.next(1i64), attr.clone());
            let fu_for_cb = fu.clone();
            let mut qr = QueuedRequest::new();
            qr.xid = (*fu).xid_;
            qr.rpc_id = rpc_id;
            qr.ttl_ms = buffering_cfg.default_ttl_ms;
            // Raw self-pointer capture (== the old [this]); the callback
            // outlives this call but not the connection.
            let conn_ptr: *const ClientConnection = &raw const *conn;
            let cb_fn = move |err: i32| {
                // SAFETY: the queue belongs to this connection and is drained
                // before the connection storage is released -- by
                // invalidate_pending_futures(), which Drop::drop calls before
                // any field is dropped. That drain is what makes this raw
                // `conn_ptr` deref sound; until it was added the callback was
                // never invoked at all, so this note described an invariant
                // nothing established.
                unsafe { (*conn_ptr).metrics_.record_queue_drop() };
                (*fu_for_cb).error_code_.set(err);
                (*fu_for_cb).notify_ready(fu_for_cb.clone());
            };
            // Through the provider's alias, not an inline
            // `rusty::Function::<dyn FnMut(i32)>` turbofish: in EXPRESSION
            // position the emitter lowers `dyn FnMut(i32)` to
            // `std::function<void(int32_t)>` and then re-wraps it, yielding the
            // undefined `rusty::Function<std::function<void(int32_t)>>`. The
            // alias declaration lowers correctly (`rusty::Function<void(int32_t)>`)
            // and names the identical Rust type.
            qr.callback = QueuedRequestCallback::from_callable(cb_fn);
            if conn.pending_queue_.enqueue(qr) {
                return FutureResult::Ok(fu);
            }
            return FutureResult::Err(CLIENT_REQUEST_QUEUE_REJECTED_ERROR);
        }
        conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
        return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
    }
    {
        let direct_guard = conn.direct_channel_.lock().unwrap();
        if (*direct_guard).is_some() {
            let proxy: &Box<dyn ChannelConnectionBase> = (*direct_guard).as_ref().unwrap();
            if proxy.is_closed() {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        } else {
            let guard2 = conn.fiber_channel_.lock().unwrap();
            let mut chan_dead = (*guard2).is_none();
            if !chan_dead {
                let fc: &Box<FiberChannel> = (*guard2).as_ref().unwrap();
                if fc.is_closed() {
                    chan_dead = true;
                }
            }
            if chan_dead {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        }
    }

    let fu = Future::create(conn.xid_counter_.next(1i64), attr.clone());
    {
        let mut pending_guard = conn.pending_fu_.lock().unwrap();
        (*pending_guard).insert((*fu).xid_, fu.clone());
    }

    // sconn_reply's archive shape: aggregate literals + the &mut alias
    // (bare reference args pass as lvalues where a by-value local would
    // be move-wrapped at its last use).
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar_store = BinaryWriteArchive { sink_: client_sink_proxy(&mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    crate::serializable::Serialize_::serialize(&crate::basetypes::v64::new((*fu).xid_), ar);
    crate::serializable::Serialize_::serialize(&rpc_id, ar);
    write_fn(ar);

    let ch_err = unsafe {
        conn.dispatch_frame_via_channel(body_sink.bytes.as_ptr(), body_sink.bytes.len())
    };
    if ch_err != ChannelError::None {
        {
            let mut pending_guard2 = conn.pending_fu_.lock().unwrap();
            (*pending_guard2).remove(&(*fu).xid_);
        }
        conn.record_circuit_result(CLIENT_ERR_IO);
        return FutureResult::Err(CLIENT_ERR_IO);
    }

    conn.metrics_.record_request_sent();
    conn.on_request_dispatched(body_sink.bytes.len());
    FutureResult::Ok(fu)
}

// clippy::borrowed_box -- the concrete Box spelling is load-bearing: through &T the pointer-like check fails and the calls lower to `.` instead of `->` (docs 7.50); measured. See the Task-2 measurement block above.
#[allow(clippy::borrowed_box)]
pub fn clientconn_request_async<F>(conn: &ClientConnection, rpc_id: i32,
                               mut write_fn: F, on_reply: AsyncReplyCallback)
                               -> Result<(), i32>
where F: FnMut(&mut BinaryWriteArchive) {
    if !conn.allow_request_with_circuit_metrics() {
        return Result::<(), i32>::Err(CLIENT_ERR_BUSY);
    }
    if !conn.state_machine_.is_connected() {
        conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
        return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
    }
    {
        let direct_guard = conn.direct_channel_.lock().unwrap();
        if (*direct_guard).is_some() {
            let proxy: &Box<dyn ChannelConnectionBase> = (*direct_guard).as_ref().unwrap();
            if proxy.is_closed() {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        } else {
            let guard2 = conn.fiber_channel_.lock().unwrap();
            let mut chan_dead = (*guard2).is_none();
            if !chan_dead {
                let fc: &Box<FiberChannel> = (*guard2).as_ref().unwrap();
                if fc.is_closed() {
                    chan_dead = true;
                }
            }
            if chan_dead {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        }
    }

    let xid: i64 = conn.xid_counter_.next(1i64);
    let slot: usize = (xid as usize) % kAsyncSlotCount;
    {
        let mut guard = conn.pending_cb_slots_.lock().unwrap();
        if (*guard)[slot].is_some() {
            conn.record_circuit_result(CLIENT_ERR_BUSY);
            return Result::<(), i32>::Err(CLIENT_ERR_BUSY);
        }
        (*guard)[slot] = Some(on_reply);
    }

    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar_store = BinaryWriteArchive { sink_: client_sink_proxy(&mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    crate::serializable::Serialize_::serialize(&crate::basetypes::v64::new(xid), ar);
    crate::serializable::Serialize_::serialize(&rpc_id, ar);
    write_fn(ar);

    let ch_err = unsafe {
        conn.dispatch_frame_via_channel(body_sink.bytes.as_ptr(), body_sink.bytes.len())
    };
    if ch_err != ChannelError::None {
        let mut guard = conn.pending_cb_slots_.lock().unwrap();
        (*guard)[slot] = None;
        conn.record_circuit_result(CLIENT_ERR_IO);
        return Result::<(), i32>::Err(CLIENT_ERR_IO);
    }
    conn.metrics_.record_request_sent();
    conn.on_request_dispatched(body_sink.bytes.len());
    Result::<(), i32>::Ok(())
}

// @safe - BinaryWriteArchive stopped being "a hand-written type with a
// real C++ constructor" when serializable.cpp made it a single-field DSL
// aggregate, so a struct literal builds it — the same literal the three
// other archive sites in this file already spell inline. The parameter
// stays `*mut BufferSink` (not `&mut`) so the emitted signature keeps a
// POINTER, which is what the caller's `&mut args_sink` lowers to -- and it
// is what the incumbent module exported (`make_write_archive(BufferSink*)`).
// The `&mut` spelling this comment already warned against had crept back in
// and re-signatured the symbol to `make_write_archive(BufferSink&)`.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn make_write_archive(sink: *mut BufferSink) -> BinaryWriteArchive {
    // SAFETY: the only caller passes `&mut` on a live local sink.
    BinaryWriteArchive { sink_: client_sink_proxy(unsafe { &mut *sink }) }
}

// @unsafe - copies the attempt's unread reply region into the coordinator
// future's buffer. Two simultaneous RefCell borrows (two DISTINCT
// Futures, so no re-entrant borrow) plus a raw sub-slice of the borrowed
// body — spelled exactly as clientconn_decode_response_and_notify below
// already spells the same fill: `ptr::add` + `core::slice::from_raw_parts`
// inside `unsafe`, which is what retired the "span has no DSL form"
// excuse. Takes REFERENCES, not pointers: `&Arc<Future>` lowers to
// `const Arc<Future>&`, and the caller's `&attempt_fu` collapses to
// the handle itself.
// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
#[allow(clippy::explicit_auto_deref)]
pub fn request_copy_reply(final_fu: &Arc<Future>, attempt_fu: &Arc<Future>) {
    let attempt_reply = (*attempt_fu).reply_.borrow_mut();
    let reply_size: usize = (*attempt_reply).src.remaining();
    if reply_size > 0usize {
        let base: *const u8 = (*attempt_reply).body.as_ptr();
        let start: usize = (*attempt_reply).src.pos();
        let mut final_reply = (*final_fu).reply_.borrow_mut();
        reply_buffer_fill(&mut *final_reply, unsafe {
            core::slice::from_raw_parts(base.add(start), reply_size)
        });
    }
}

// Pure classification of an errno; captures nothing. The original
// `#if CLIENT_ERR_WOULD_BLOCK != CLIENT_ERR_AGAIN` guard existed only to avoid a duplicate
// switch case label; in an if-else `|| err == CLIENT_ERR_WOULD_BLOCK` is a
// harmless redundancy on Linux (CLIENT_ERR_AGAIN == CLIENT_ERR_WOULD_BLOCK) and keeps the
// intent without a preprocessor conditional — exactly the reshape
// already shipped in clientconn_map_system_error above. It lives in
// THIS block rather than one of its own so the call below stays a
// same-block call.
pub fn classify_request_failure(err: i32) -> TimeoutType {
    if err == CLIENT_ERR_NOT_CONNECTED || err == CLIENT_ERR_CONNECTION_REFUSED || err == CLIENT_ERR_CONNECTION_RESET
        || err == CLIENT_ERR_CONNECTION_ABORTED || err == CLIENT_ERR_HOST_UNREACHABLE || err == CLIENT_ERR_NETWORK_UNREACHABLE {
        return TimeoutType::CONNECT_TIMEOUT;
    }
    if err == CLIENT_ERR_TIMED_OUT || err == CLIENT_ERR_AGAIN || err == CLIENT_ERR_WOULD_BLOCK {
        return TimeoutType::REQUEST_TIMEOUT;
    }
    TimeoutType::NONE
}

// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
// clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
#[allow(clippy::explicit_auto_deref, clippy::unnecessary_unwrap)]
pub fn clientconn_request_with_options<F>(self_: &ClientConnection, rpc_id: i32,
                                      options: &RequestOptions,
                                      attr: &FutureAttr, mut write_fn: F) -> FutureResult
where F: FnMut(&mut BinaryWriteArchive) {
    // Serialize args once so retries can replay identical payload safely.
    // Turbofish, matching the three other `BufferSink` literals in this file:
    // a bare `Vec::new()` in a struct-literal field takes its emitted element
    // type from an unrelated binding instead of from `bytes: Vec<u8>`.
    let mut args_sink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar: BinaryWriteArchive = make_write_archive(&raw mut args_sink);
    let ar_ref: &mut BinaryWriteArchive = &mut ar;
    write_fn(ar_ref);
    // Keep the replay payload as bytes (was a reinterpret_cast'd
    // LegacyStdString round-trip).
    let args_bytes: Vec<u8> = args_sink.bytes.clone();

    // Non-idempotent operations must never be retried even if max_retries is set.
    let mut effective_options: RequestOptions = *options;
    if !effective_options.idempotent {
        effective_options.max_retries = 0u16;
    }

    // Return a coordinator future immediately; internal attempts run async.
    let final_fu: Arc<Future> = Future::create(self_.xid_counter_.next(1), attr.clone());
    let mut waiter_options: RequestOptions = effective_options;
    waiter_options.timeout_ms = 0u64;  // Internal attempts own timeout behavior.
    (*final_fu).set_options(&waiter_options);

    let weak_conn = self_.weak_self_.clone();
    // The spawned closure MOVES what it captures, so the coordinator
    // future needs its own handle: without this clone the `move ||`
    // capture leaves the `Ok(final_fu)` below returning a moved-from
    // (null) Arc. The hand-written original captured `final_fu` by copy.
    let final_fu_task: Arc<Future> = final_fu.clone();
    rusty::thread::spawn(move || {
        let start_us: u64 = Time::now(true);
        let retry_count = Cell::new(0u16);

        let finish_terminal = |err: i32, timeout_type: TimeoutType| {
            let conn_opt = weak_conn.upgrade();
            if conn_opt.is_some() {
                let conn = conn_opt.unwrap();
                if timeout_type == TimeoutType::CONNECT_TIMEOUT
                    || timeout_type == TimeoutType::REQUEST_TIMEOUT
                    || timeout_type == TimeoutType::RESPONSE_TIMEOUT
                    || timeout_type == TimeoutType::TOTAL_TIMEOUT {
                    (*conn).metrics_.record_request_timeout();
                } else if err != 0i32 {
                    (*conn).metrics_.record_request_failed();
                }
            }
            if timeout_type != TimeoutType::NONE {
                let mut state_guard = (*final_fu_task).state_.lock().unwrap();
                (*state_guard).timed_out = true;
            }
            (*final_fu_task).error_code_.set(err);
            (*final_fu_task).timeout_type_.set(timeout_type);
            (*final_fu_task).retry_count_.set(retry_count.get());
            (*final_fu_task).notify_ready(final_fu_task.clone());
        };

        let set_terminal_timeout = |timeout_type: TimeoutType| {
            finish_terminal(CLIENT_ERR_TIMED_OUT, timeout_type);
        };

        loop {
            let elapsed_ms: u64 = (Time::now(true) - start_us) / 1000u64;
            if effective_options.is_total_timeout_exceeded(elapsed_ms) {
                set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                return;
            }

            let conn_opt = weak_conn.upgrade();
            if conn_opt.is_none() {
                finish_terminal(CLIENT_ERR_NOT_CONNECTED, TimeoutType::CONNECT_TIMEOUT);
                return;
            }

            let conn = conn_opt.unwrap();
            let replay = |m: &mut BinaryWriteArchive| {
                if !args_bytes.is_empty() {
                    unsafe { (*m).write_bytes(args_bytes.as_ptr(), args_bytes.len()) };
                }
            };
            // (Default::default() infers only in typed-let position, not
            // as a bare argument.)
            let empty_attr: FutureAttr = Default::default();
            let attempt_result = (*conn).request(rpc_id, &empty_attr, replay);
            let attempt_fu: Arc<Future> = match attempt_result {
                Ok(future) => future,
                Err(err) => {
                    finish_terminal(err, classify_request_failure(err));
                    return;
                }
            };
            let mut attempt_options: RequestOptions = effective_options;
            if effective_options.total_timeout_ms > 0u64 {
                let remaining_ms: u64 = effective_options.remaining_time_ms(elapsed_ms);
                if remaining_ms == 0u64 {
                    (*conn).handle_free((*attempt_fu).xid_);
                    set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                    return;
                }
                if attempt_options.timeout_ms == 0u64 || attempt_options.timeout_ms > remaining_ms {
                    attempt_options.timeout_ms = remaining_ms;
                }
            }
            (*attempt_fu).set_options(&attempt_options);
            if (*attempt_fu).wait_with_options() {
                (*final_fu_task).error_code_.set((*attempt_fu).error_code_.get());
                (*final_fu_task).retry_count_.set(retry_count.get());
                if (*attempt_fu).error_code_.get() == 0i32 {
                    request_copy_reply(&final_fu_task, &attempt_fu);
                }
                (*final_fu_task).notify_ready(final_fu_task.clone());
                return;
            }

            // Timed-out attempts are no longer useful; release pending map slot.
            (*conn).handle_free((*attempt_fu).xid_);

            if !effective_options.can_retry(retry_count.get()) {
                set_terminal_timeout((*attempt_fu).get_timeout_type());
                return;
            }

            (*conn).metrics_.record_retry_attempt();
            let backoff_delay_ms: u64 = effective_options.calculate_delay_ms(retry_count.get());
            if backoff_delay_ms > 0u64 {
                if effective_options.total_timeout_ms > 0u64 {
                    let elapsed_before_sleep: u64 = (Time::now(true) - start_us) / 1000u64;
                    let remaining_ms: u64 = effective_options.remaining_time_ms(elapsed_before_sleep);
                    if remaining_ms == 0u64 || backoff_delay_ms >= remaining_ms {
                        set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                        return;
                    }
                }
                Time::sleep(backoff_delay_ms * 1000u64);
            }

            retry_count.set(retry_count.get() + 1u16);
            (*final_fu_task).retry_count_.set(retry_count.get());
        }
    }).detach();

    FutureResult::Ok(final_fu)
}

/// Hand one already-encoded frame body to the bound channel.
///
/// # Safety
///
/// `body_bytes` must point at `body_size` readable bytes that stay live for
/// the duration of the call; the channel copies out of them synchronously.
pub unsafe fn clientconn_dispatch_frame_via_channel(conn: &ClientConnection,
                                                body_bytes: *const u8,
                                                body_size: usize) -> ChannelError {
    if !conn.channel_mode_.get() {
        return ChannelError::ConnectionReset;
    }
    {
        let mut guard = conn.direct_channel_.lock().unwrap();
        if (*guard).is_some() {
            let p: &mut Box<dyn ChannelConnectionBase> = (*guard).as_mut().unwrap();
            return unsafe { p.send_frame(&ChannelFrame { payload: body_bytes, size: body_size }) };
        }
    }
    let mut guard2 = conn.fiber_channel_.lock().unwrap();
    if (*guard2).is_none() {
        return ChannelError::ConnectionReset;
    }
    let p2: &mut Box<FiberChannel> = (*guard2).as_mut().unwrap();
    unsafe { p2.send_frame(&ChannelFrame { payload: body_bytes, size: body_size }) }
}

pub fn clientconn_enqueue_heartbeat_probe(conn: &ClientConnection) {
    // Build the heartbeat frame body and dispatch through the channel
    // proxy. Same archive shape as the server's sconn_reply: aggregate
    // struct literals + the &mut alias so serialize's Archive& binds.
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar_store = BinaryWriteArchive { sink_: client_sink_proxy(&mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    crate::serializable::Serialize_::serialize(
        &crate::basetypes::v64::new(conn.xid_counter_.next(1i64)),
        ar,
    );
    crate::serializable::Serialize_::serialize(&CLIENT_INTERNAL_HEARTBEAT_RPC_ID, ar);
    // Send-side errors are ignored here (same as the legacy fd path).
    let _ = unsafe {
        conn.dispatch_frame_via_channel(body_sink.bytes.as_ptr(), body_sink.bytes.len())
    };
}

// clippy::not_unsafe_ptr_arg_deref -- this became public with the module's
// surface; the raw-pointer contract is the historical C++ one and is
// documented at the deref itself. Marking the fn `unsafe` instead would
// wrap every call site in an `unsafe` block, which the emitter renders
// as an @unsafe comment block -- measured: changes emitted C++.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn clientconn_addr_to_string(addr: *const i8) -> LegacyStdString {
    if addr.is_null() {
        // See `ClientConnection::new`: the alias maps to `std::string`.
        return Default::default();
    }
    // Byte-for-byte copy up to the NUL — the same shape `base/logging.cpp`'s
    // `log_basename` uses, and the same bytes the historical
    // `std::string(addr)` produced.
    //
    // `CStr::from_ptr(..).to_string_lossy().into_owned()` is not spellable
    // here: the checked map spells `CStr` `std::string`, so the associated
    // function emits the non-existent `std::string::from_ptr`, and every
    // `CStr` method behind it has the same problem. `rusty::LoggingString` is
    // the byte model that maps to `std::string` and carries C++'s
    // `push_back`; `client_text` then hands back the module's own
    // `LegacyStdString` (`&LoggingString` derefs to `&str` in rustc and
    // converts to `std::string_view` in C++).
    let mut scratch: rusty::LoggingString = Default::default();
    let mut index: usize = 0;
    // SAFETY: all callers uphold the historical C-string input contract;
    // `index` is advanced only until the first NUL byte.
    while unsafe { *addr.add(index) } != 0i8 {
        // SAFETY: as above — `index` is still before the terminator.
        scratch.push_back(unsafe { *addr.add(index) });
        index += 1;
    }
    client_text(&scratch)
}

pub fn clientconn_connect_via_factory(conn: &ClientConnection, addr_i8: *const i8) -> i32 {
    let addr_str: LegacyStdString = clientconn_addr_to_string(addr_i8);
    {
        let mut guard = conn.factory_.lock().unwrap();
        if (*guard).is_none() {
            client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text("srpc::ClientConnection::connect_via_factory: factory unbound at the moment of connect (race against bind_factory)"));
            conn.state_machine_.transition_to(ConnectionState::FAILED);
            conn.invoke_error_callback(CLIENT_ERR_NOT_CONNECTED, &client_text("factory unbound"));
            return CLIENT_ERR_NOT_CONNECTED;
        }
        let bound: &mut Box<dyn ChannelFactoryBase> = (*guard).as_mut().unwrap();
        let mut result: ConnectResult = bound.connect(&addr_str);
        if result.error != ChannelError::None || result.connection.is_none() {
            let err_name = channel_error_to_string(result.error);
            let err_str: LegacyStdString = client_text_str("factory connect failed: ", err_name, "");
            client_log_line(Log::ERROR, 0i32, core::ptr::null(), client_text_str_pair("srpc::ClientConnection: ", &err_str, " (addr=", &addr_str, ")"));
            conn.state_machine_.transition_to(ConnectionState::FAILED);
            // Map the channel error onto an errno-shaped value the
            // legacy call sites expect.
            let mut rc: i32 = CLIENT_ERR_NOT_CONNECTED;
            if result.error == ChannelError::ConnectionRefused {
                rc = CLIENT_ERR_CONNECTION_REFUSED;
            } else if result.error == ChannelError::AddressInvalid {
                rc = CLIENT_ERR_INVALID_ARGUMENT;
            }
            conn.invoke_error_callback(rc, &err_str);
            return rc;
        }
        let conn_proxy = result.connection.take().unwrap();
        conn.bind_channel_direct(conn_proxy);
    }

    // Record address for the close fan-out's reconnect spawn — it
    // re-runs the factory connect with the same target.
    conn.reconnect_address_.set(addr_str);

    // Mirror the fd path's terminal transition: the channel layer's
    // own state (proxy.is_closed()) becomes the source of truth, but
    // we still drive the legacy state machine through CONNECTED so
    // existing health-check / metric APIs keep working.
    if !conn.state_machine_.transition_to(ConnectionState::CONNECTED) {
        conn.state_machine_.force_state(ConnectionState::CONNECTED);
    }
    // Record connect timestamp so metrics_.connect_time_ms() is
    // non-zero from the moment a request can be issued; seed
    // last_activity_time_ so is_idle() measures time since connect.
    {
        let now: u64 = clientconn_monotonic_ms_now();
        conn.metrics_.record_connect(now);
        conn.update_last_activity(now);
    }
    conn.invoke_connected_callback();
    0i32
}

pub fn clientconn_make_fiber_channel(ch: ChannelConnectionProxy) -> Box<FiberChannel> {
    // Lowers to the in-place `emplace_with` seam: FiberChannel's moves are
    // deleted, so the factory's returned prvalue constructs it directly in
    // the heap slot (guaranteed copy elision).
    Box::new(FiberChannel::new(ch))
}

// clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
#[allow(clippy::unnecessary_unwrap)]
pub fn clientconn_recv_job_entry(weak_self: WeakClientConnection) {
    let conn_opt = weak_self.upgrade();
    if conn_opt.is_some() {
        let c = conn_opt.unwrap();
        (*c).run_recv_loop();
    }
}

// clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
#[allow(clippy::arc_with_non_send_sync)]
pub fn clientconn_bind_channel_via_poll_thread(conn: &ClientConnection,
                                           channel: ChannelConnectionProxy) {
    if !channel.is_valid() {
        return;
    }
    // Move the proxy into the heap-allocated FiberChannel and flip the
    // latch on the calling thread — pure data mutations; the recv-loop
    // fiber doesn't observe them until the OneTimeJob below is
    // submitted. bind_callbacks() runs after the Box address is final.
    {
        let mut guard = conn.fiber_channel_.lock().unwrap();
        *guard = Some(clientconn_make_fiber_channel(channel));
        let fc: &mut Box<FiberChannel> = (*guard).as_mut().unwrap();
        fc.bind_callbacks();
    }
    conn.channel_mode_.set(true);

    let weak_self: WeakClientConnection = conn.weak_self_.clone();

    // Schedule the recv-loop fiber spawn onto the poll thread. The
    // poll thread's `trigger_job` calls `Fiber::create_run` from its
    // own reactor, so the resulting fiber's IntEvent waits and the
    // `on_frame` callback's signal both land on the same thread.
    // (The closure is bound to a local first: the inline-argument
    // closure path mis-infers a return type here — the ::new_ note at
    // ClientProxy::close — while the let-bound path emits it clean.)
    let job_fn = move || {
        clientconn_recv_job_entry(weak_self.clone());
    };
    let recv_job: Arc<OneTimeJob> =
        Arc::<OneTimeJob>::new(OneTimeJob::new(Box::new(job_fn)));
    // Implicit Arc<OneTimeJob> -> Arc<Job> upcast for the queue.
    let pt: &Arc<PollThread> = &conn.poll_thread_worker_;
    // SAFETY: foreign named-module boundary; the job handle is freshly built
    // and uniquely owned here.
    unsafe { pt.add(recv_job) };
}

// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
#[allow(clippy::explicit_auto_deref)]
pub fn clientconn_fiber_channel_ptr(slot: &Option<Box<FiberChannel>>) -> *mut FiberChannel {
    // The borrow is taken into a named reference first, and it keeps the
    // explicit `&**` (the `Box` deref must be written out; the emitter does
    // not insert Rust's deref coercion). Written inline as
    // `&**slot.as_ref().unwrap() as *const FiberChannel`, the emitter drops
    // the leading `&` and casts the DEREFERENCED value to a pointer type.
    let borrowed: &FiberChannel = &**slot.as_ref().unwrap();
    (borrowed as *const FiberChannel).cast_mut()
}

// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
#[allow(clippy::explicit_auto_deref)]
pub fn clientconn_run_recv_loop(conn: &ClientConnection) {
    let fc: *mut FiberChannel;
    {
        let guard = conn.fiber_channel_.lock().unwrap();
        if (*guard).is_none() {
            return;
        }
        fc = clientconn_fiber_channel_ptr(&*guard);
    }
    loop {
        // SAFETY: `fc` points at the stable boxed channel retained by this
        // connection for the lifetime of the receive loop.
        let frame_opt: Option<OwnedFrame> = unsafe { (*fc).recv_frame() };
        if frame_opt.is_none() {
            // Channel closed. Run the close-side fan-out (sub-leaf 4d):
            // cancel pending futures with CLIENT_ERR_NOT_CONNECTED, fire error /
            // disconnected callbacks, and trigger auto-reconnect if the
            // policy allows. The fiber then exits, dropping its
            // Arc<ClientConnection> capture.
            conn.on_channel_closed_fan_out();
            return;
        }
        let frame = frame_opt.unwrap();
        conn.decode_response_and_notify(frame.bytes.as_ptr(), frame.bytes.len());
    }
}

// clippy::explicit_auto_deref -- measured: 42 of the 68 sites change emitted C++ (std::move out of an Arc field, a by-value bind of a borrow guard, a pointer where a value was passed). See the Task-2 measurement block above.
// clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
// clippy::not_unsafe_ptr_arg_deref -- this became public with the module's
// surface; the raw-pointer contract is the historical C++ one and is
// documented at the deref itself. Marking the fn `unsafe` instead would
// wrap every call site in an `unsafe` block, which the emitter renders as an
// @unsafe comment block -- measured: changes emitted C++.
#[allow(clippy::explicit_auto_deref, clippy::unnecessary_unwrap, clippy::not_unsafe_ptr_arg_deref)]
pub fn clientconn_decode_response_and_notify(conn: &ClientConnection,
                                         bytes: *const u8, size: usize) {
    // Account for every inbound frame body byte and bump the activity
    // clock so metrics_.bytes_received() and is_idle() reflect real
    // I/O regardless of which dispatch slot the reply maps onto.
    conn.on_response_received(size);
    let mut src = BufferSource::new(bytes, size);
    let mut ar = BinaryReadArchive { source_: client_source_proxy(&mut src) };

    let mut v_reply_xid = crate::basetypes::v64::new(0i64);
    let mut v_error_code = crate::basetypes::v32::new(0i32);
    // In channel mode the extended-header flag is consumed by the
    // framing layer; the server always emits the extended form.
    let mut v_server_instance_id = crate::basetypes::v64::new(0i64);
    crate::serializable::Deserialize_::deserialize(&mut v_reply_xid, &mut ar);
    crate::serializable::Deserialize_::deserialize(&mut v_error_code, &mut ar);
    crate::serializable::Deserialize_::deserialize(&mut v_server_instance_id, &mut ar);
    conn.check_server_instance(v_server_instance_id.get() as u64);

    let parsed_header_size: usize = src.pos();
    let response_payload_bytes: usize = size - parsed_header_size;
    conn.heartbeat_manager_.on_pong_received();

    {
        let slot: usize = (v_reply_xid.get() as usize) % kAsyncSlotCount;
        let mut cb_opt: Option<AsyncReplyCallback> = None;
        {
            let mut guard = conn.pending_cb_slots_.lock().unwrap();
            if (*guard)[slot].is_some() {
                cb_opt = core::mem::take(&mut (*guard)[slot]);
            }
        }
        if cb_opt.is_some() {
            let mut cb = cb_opt.unwrap();
            let err_code: i32 = v_error_code.get();
            if err_code == 0i32 {
                conn.metrics_.record_request_completed();
            } else {
                conn.metrics_.record_request_failed();
            }
            conn.record_circuit_result(err_code);
            cb(err_code, unsafe { bytes.add(parsed_header_size) },
               response_payload_bytes);
            return;
        }
    }

    let mut fu_opt: Option<Arc<Future>> = None;
    {
        let mut guard = conn.pending_fu_.lock().unwrap();
        let fu_ptr = (*guard).get(&v_reply_xid.get());
        if fu_ptr.is_some() {
            fu_opt = Some(fu_ptr.unwrap().clone());
            (*guard).remove(&v_reply_xid.get());
        }
    }

    if fu_opt.is_some() {
        let fu = fu_opt.unwrap();
        client_verify((*fu).xid_ == v_reply_xid.get());
        (*fu).error_code_.set(v_error_code.get());
        if response_payload_bytes > 0usize {
            let mut rb_guard = (*fu).reply_.borrow_mut();
            reply_buffer_fill(&mut *rb_guard, unsafe {
                core::slice::from_raw_parts(
                    bytes.add(parsed_header_size),
                    response_payload_bytes)
            });
        }
        if v_error_code.get() == 0i32 {
            conn.metrics_.record_request_completed();
        } else {
            conn.metrics_.record_request_failed();
        }
        conn.record_circuit_result(v_error_code.get());
        (*fu).notify_ready(fu.clone());
    }
    // No matching future (timed out or replaced) -> drop the payload.
    // With channel-mode framing the input bytes are owned by the
    // caller and freed on return -- nothing to drain.
}

pub fn clientconn_map_system_error(err: i32) -> RpcError {
    if err == 0i32 { return RpcError::OK; }
    if err == CLIENT_ERR_NOT_CONNECTED { return RpcError::NOT_CONNECTED; }
    if err == CLIENT_ERR_CONNECTION_REFUSED { return RpcError::CONNECTION_REFUSED; }
    if err == CLIENT_ERR_CONNECTION_RESET { return RpcError::CONNECTION_RESET; }
    if err == CLIENT_ERR_NETWORK_UNREACHABLE { return RpcError::NETWORK_UNREACHABLE; }
    if err == CLIENT_ERR_HOST_UNREACHABLE { return RpcError::HOST_UNREACHABLE; }
    if err == CLIENT_ERR_CONNECTION_ABORTED || err == CLIENT_ERR_BROKEN_PIPE { return RpcError::CONNECTION_CLOSED; }
    if err == CLIENT_ERR_BUSY { return RpcError::CIRCUIT_OPEN; }
    if err == CLIENT_ERR_TIMED_OUT { return RpcError::RESPONSE_TIMEOUT; }
    if err == CLIENT_ERR_AGAIN || err == CLIENT_ERR_WOULD_BLOCK { return RpcError::REQUEST_TIMEOUT; }
    if err == CLIENT_ERR_INVALID_ARGUMENT { return RpcError::INVALID_ARGUMENT; }
    RpcError::UNKNOWN_ERROR
}

pub fn clientpool_is_client_healthy_with(cfg: PoolConfig, client: &Arc<Client>) -> bool {
    if !cfg.health_check_enabled {
        return true;
    }
    if !(*client).connected() {
        return false;
    }
    let requests_sent: u64 = (*client).metrics().requests_sent();
    if requests_sent < cfg.min_requests_for_health {
        return true;
    }
    let success_rate: u64 = (*client).metrics().success_rate_percent();
    success_rate >= cfg.unhealthy_threshold_percent
}

// clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
#[allow(clippy::unnecessary_unwrap)]
pub fn clientpool_get_healthy_client_count(self_: &ClientPool, addr: &LegacyStdString) -> usize {
    // Config snapshot BEFORE `state_`, per the lock-order invariant.
    let cfg: PoolConfig = self_.pool_config();
    let guard = self_.state_.lock().unwrap();
    let mut count: usize = 0usize;
    let clients_opt = guard.cache.get(addr);
    if clients_opt.is_some() {
        let clients: &Vec<Arc<Client>> = clients_opt.unwrap();
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            if clientpool_is_client_healthy_with(cfg, &(*clients)[i]) {
                count += 1usize;
            }
            i += 1usize;
        }
    }
    count
}

// clippy::reserve_after_initialization -- measured: emits Vec::with_capacity() and drops the reserve() call. See the Task-2 measurement block above.
// clippy::unnecessary_get_then_check -- measured: emits contains_key() where the C++ surface has get().is_some(). See the Task-2 measurement block above.
#[allow(clippy::reserve_after_initialization, clippy::unnecessary_get_then_check)]
pub fn clientpool_remove_unhealthy_clients(self_: &ClientPool, addr: &LegacyStdString) -> usize {
    // Config snapshot BEFORE `state_`, per the lock-order invariant.
    let cfg: PoolConfig = self_.pool_config();
    let mut guard = self_.state_.lock().unwrap();
    let mut removed: usize = 0usize;
    // Probe with get(): an intermediate `let opt = ...get_mut(..)` binding
    // lowers to `auto&` on a temporary Option (won't compile). The chained
    // one-step unwrap below binds the inner &mut directly (§7.37).
    let has_entry: bool = guard.cache.get(addr).is_some();
    if has_entry {
        let clients: &mut Vec<Arc<Client>> = guard.cache.get_mut(addr).unwrap();
        // Remove unhealthy clients, but keep at least min_connections.
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>::new();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - removed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if !clientpool_is_client_healthy_with(cfg, client) {
                (*client).close();
                removed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;

        // Remove empty entries from cache
        if (*clients).is_empty() {
            guard.cache.remove(addr);
        }
    }
    removed
}

// clippy::reserve_after_initialization -- measured: emits Vec::with_capacity() and drops the reserve() call. See the Task-2 measurement block above.
// clippy::unnecessary_get_then_check -- measured: emits contains_key() where the C++ surface has get().is_some(). See the Task-2 measurement block above.
#[allow(clippy::reserve_after_initialization, clippy::unnecessary_get_then_check)]
pub fn clientpool_close_idle_clients(self_: &ClientPool, addr: &LegacyStdString, current_time_ms: u64) -> usize {
    let cfg: PoolConfig = self_.pool_config();

    // If idle timeout is 0, no timeout
    if cfg.idle_timeout_ms == 0u64 {
        return 0usize;
    }

    let mut guard = self_.state_.lock().unwrap();
    let mut closed: usize = 0usize;
    let has_entry: bool = guard.cache.get(addr).is_some();
    if has_entry {
        let clients: &mut Vec<Arc<Client>> = guard.cache.get_mut(addr).unwrap();
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>::new();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - closed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if (*client).is_idle(cfg.idle_timeout_ms, current_time_ms) {
                (*client).close();
                closed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;

        if (*clients).is_empty() {
            guard.cache.remove(addr);
        }
    }
    closed
}

// clippy::reserve_after_initialization -- measured: emits Vec::with_capacity() and drops the reserve() call. See the Task-2 measurement block above.
// clippy::unnecessary_get_then_check -- measured: emits contains_key() where the C++ surface has get().is_some(). See the Task-2 measurement block above.
#[allow(clippy::reserve_after_initialization, clippy::unnecessary_get_then_check)]
pub fn clientpool_remove_all_unhealthy(self_: &ClientPool) -> usize {
    // Config snapshot BEFORE `state_`, per the lock-order invariant on
    // ClientPool. This read used to sit after the lock, which was the one
    // site in the pool that acquired the two in the opposite order from
    // get_client.
    let cfg: PoolConfig = self_.pool_config();
    let mut guard = self_.state_.lock().unwrap();
    let mut total_removed: usize = 0usize;

    let mut keys: Vec<LegacyStdString> = Vec::<LegacyStdString>::new();
    {
        let mut it = guard.cache.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            keys.push(kv.0.clone());
        }
    }
    let mut empty_keys: Vec<LegacyStdString> = Vec::<LegacyStdString>::new();
    let mut k: usize = 0usize;
    while k < keys.len() {
        let addr: &LegacyStdString = &keys[k];
        let has_entry: bool = guard.cache.get(addr).is_some();
        if !has_entry {
            k += 1usize;
            continue;
        }
        let clients: &mut Vec<Arc<Client>> = guard.cache.get_mut(addr).unwrap();
        let mut removed: usize = 0usize;
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>::new();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - removed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if !clientpool_is_client_healthy_with(cfg, client) {
                (*client).close();
                removed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;
        total_removed += removed;
        if (*clients).is_empty() {
            empty_keys.push(addr.clone());
        }
        k += 1usize;
    }
    let mut j: usize = 0usize;
    while j < empty_keys.len() {
        let key: &LegacyStdString = &empty_keys[j];
        guard.cache.remove(key);
        j += 1usize;
    }
    total_removed
}

// clippy::reserve_after_initialization -- measured: emits Vec::with_capacity() and drops the reserve() call. See the Task-2 measurement block above.
// clippy::unnecessary_get_then_check -- measured: emits contains_key() where the C++ surface has get().is_some(). See the Task-2 measurement block above.
#[allow(clippy::reserve_after_initialization, clippy::unnecessary_get_then_check)]
pub fn clientpool_close_all_idle(self_: &ClientPool, current_time_ms: u64) -> usize {
    let cfg: PoolConfig = self_.pool_config();
    if cfg.idle_timeout_ms == 0u64 {
        return 0usize;
    }

    let mut guard = self_.state_.lock().unwrap();
    let mut total_closed: usize = 0usize;

    let mut keys: Vec<LegacyStdString> = Vec::<LegacyStdString>::new();
    {
        let mut it = guard.cache.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            keys.push(kv.0.clone());
        }
    }
    let mut empty_keys: Vec<LegacyStdString> = Vec::<LegacyStdString>::new();
    let mut k: usize = 0usize;
    while k < keys.len() {
        let addr: &LegacyStdString = &keys[k];
        let has_entry: bool = guard.cache.get(addr).is_some();
        if !has_entry {
            k += 1usize;
            continue;
        }
        let clients: &mut Vec<Arc<Client>> = guard.cache.get_mut(addr).unwrap();
        let mut closed: usize = 0usize;
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>::new();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - closed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if (*client).is_idle(cfg.idle_timeout_ms, current_time_ms) {
                (*client).close();
                closed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;
        total_closed += closed;
        if (*clients).is_empty() {
            empty_keys.push(addr.clone());
        }
        k += 1usize;
    }
    let mut j: usize = 0usize;
    while j < empty_keys.len() {
        let key: &LegacyStdString = &empty_keys[j];
        guard.cache.remove(key);
        j += 1usize;
    }
    total_closed
}

// The `const int8_t*` the srpc wire type wants is spelled `addr.c_str()
// as *const i8`, which lowers to the same reinterpret_cast the old
// kernel wrote by hand. (The historical carrier kept caller and callee in
// one inline-Rust region; the canonical file has no regions.)
// clippy::unnecessary_cast -- measured: drops the emitted rusty::detail::ptr_cast<const int8_t*>. See the Task-2 measurement block above.
#[allow(clippy::unnecessary_cast)]
pub fn clientpool_connect_client(client: &Arc<Client>, addr: &LegacyStdString) -> i32 {
    client.connect(addr.c_str() as *const i8, true)
}

// clippy::ptr_arg -- measured: changes the exported clientpool_select signature from const rusty::Vec<..>& to std::span<..>. See the Task-2 measurement block above.
#[allow(clippy::ptr_arg)]
pub fn clientpool_select(
    strategy: LoadBalancingStrategy,
    clients: &Vec<Arc<Client>>,
    state: &LoadBalancerState,
    rand_value: usize,
) -> usize {
    if clients.is_empty() {
        return 0usize;
    }
    if strategy == LoadBalancingStrategy::ROUND_ROBIN {
        return LoadBalancer::select_round_robin(clients.len(), state);
    }
    if strategy == LoadBalancingStrategy::LEAST_CONNECTIONS {
        let mut best_idx = 0usize;
        let mut min_pending = u64::MAX;
        let mut i = 0usize;
        while i < clients.len() {
            let pending = (*clients[i]).metrics().in_flight_requests();
            if pending < min_pending {
                min_pending = pending;
                best_idx = i;
            }
            i += 1usize;
        }
        return best_idx;
    }
    if strategy == LoadBalancingStrategy::LEAST_LATENCY {
        let mut best_idx = 0usize;
        let mut min_latency = u64::MAX;
        let mut i = 0usize;
        while i < clients.len() {
            let metrics = (*clients[i]).metrics();
            let latency = metrics.avg_latency_us();
            let completed = metrics.requests_completed();
            if !(latency == 0u64 && completed == 0u64) && latency < min_latency {
                min_latency = latency;
                best_idx = i;
            }
            i += 1usize;
        }
        return best_idx;
    }
    LoadBalancer::select_random(clients.len(), rand_value)
}

// clippy::unnecessary_get_then_check -- measured: emits contains_key() where the C++ surface has get().is_some(). See the Task-2 measurement block above.
#[allow(clippy::unnecessary_get_then_check)]
pub fn clientpool_get_client(self_: &ClientPool, addr: &LegacyStdString) -> Option<Arc<Client>> {
    let mut sp_cl: Option<Arc<Client>> = None;
    let cfg: PoolConfig = self_.pool_config();
    let num_connections: i32 = cfg.min_connections;

    let mut guard = self_.state_.lock().unwrap();

    // Get or create load balancer state for this address. select() takes
    // &LoadBalancerState (round-robin advances through a Cell), so the
    // shared get() probe is enough.
    let has_lb: bool = guard.lb_state.get(addr).is_some();
    if !has_lb {
        guard.lb_state.insert(addr.clone(), LoadBalancerState::new());
    }
    let has_cached: bool = guard.cache.get(addr).is_some();
    if has_cached {
        let start_idx: usize = {
            let lb_state: &LoadBalancerState = guard.lb_state.get(addr).unwrap();
            let clients: &Vec<Arc<Client>> = guard.cache.get(addr).unwrap();
            clientpool_select(
                cfg.load_balancing,
                clients,
                lb_state,
                client_rand(0i32, CLIENT_RAND_MAX) as usize,
            )
        };
        let clients: &mut Vec<Arc<Client>> = guard.cache.get_mut(addr).unwrap();
        let client_count: i32 = (*clients).len() as i32;

        let mut i: i32 = 0i32;
        while i < client_count {
            let idx: usize = (start_idx + i as usize) % (client_count as usize);
            let client: &Arc<Client> = &(*clients)[idx];

            // Check if client is connected and healthy
            if (*client).connected() && clientpool_is_client_healthy_with(cfg, client) {
                sp_cl = Some(client.clone());
                break;
            }

            // Try to reconnect failed/disconnected clients
            let state: ConnectionState = (*client).connection_state();
            if (state as i32) == (ConnectionState::FAILED as i32)
                || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                let state_name = connection_state_to_string(state);
                client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text_str_pair("ClientPool: client to ", addr, " in state ", state_name, ", attempting reconnect"));
                if (*client).try_reconnect_if_needed() {
                    client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text_str("ClientPool: reconnected to ", addr, " successfully"));
                    sp_cl = Some(client.clone());
                    break;
                } else {
                    client_log_line(Log::WARN, 0i32, core::ptr::null(), client_text_str("ClientPool: reconnect to ", addr, " failed"));
                }
            }
            i += 1i32;
        }

        // If no healthy client found after trying reconnects, recreate all connections
        if sp_cl.is_none() {
            client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text_str("ClientPool: all clients to ", addr, " failed, recreating connections"));
            // Close old connections
            let mut ci: usize = 0usize;
            while ci < (*clients).len() {
                (*(*clients)[ci]).close();
                ci += 1usize;
            }
            (*clients).clear();

            // Create new connections (use min_connections)
            let mut ok: bool = true;
            let mut n: i32 = 0i32;
            while n < num_connections {
                let client: Arc<Client> =
                    Client::create(self_.poll_thread_worker_.as_ref().unwrap().clone());
                (*client).set_client_mode(true);
                if clientpool_connect_client(&client, addr) != 0i32 {
                    client_log_line(Log::WARN, 0i32, core::ptr::null(), client_text_str("ClientPool: failed to create new connection to ", addr, ""));
                    ok = false;
                    break;
                }
                (*clients).push(client);
                n += 1i32;
            }

            if ok && !(*clients).is_empty() {
                let pick: usize =
                    client_rand(0i32, (*clients).len() as i32 - 1i32) as usize;
                sp_cl = Some((*clients)[pick].clone());
            } else {
                // Remove from cache if we can't connect
                guard.cache.remove(addr);
            }
        }
    } else {
        // No cached connections - create new ones
        let mut parallel_clients: Vec<Arc<Client>> = Vec::<Arc<Client>>::new();
        let mut ok: bool = true;
        let mut n2: i32 = 0i32;
        while n2 < num_connections {
            let client: Arc<Client> =
                Client::create(self_.poll_thread_worker_.as_ref().unwrap().clone());
            (*client).set_client_mode(true);  // Jetpack: mark as client
            if clientpool_connect_client(&client, addr) != 0i32 {
                ok = false;
                break;
            }
            parallel_clients.push(client);
            n2 += 1i32;
        }
        if ok {
            let pick2: usize =
                client_rand(0i32, parallel_clients.len() as i32 - 1i32) as usize;
            sp_cl = Some(parallel_clients[pick2].clone());
            guard.cache.insert(addr.clone(), parallel_clients);
        }
        // If not ok, parallel_clients cleans up via the Arc drops
    }
    sp_cl
}
