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
//! unchanged. Every number below was measured the same way: lift the
//! `#[allow]`, let `cargo clippy --fix` apply the lint's own suggestion,
//! regenerate all 37 providers with the pinned transpiler, and byte-compare
//! the emitted modules. Last measured 2026-09-11 with clippy 0.1.97 and
//! rusty-cpp 3e1d9505. Re-measure after any transpiler or toolchain bump:
//! the figures recorded here before that pass (68 sites, 42 emission
//! changes, 80 findings across 14 families on 35 items) had been taken
//! against an older pin and had drifted on every count.
//!
//! Taken: `(*x)` spellings whose pin proved emission-neutral -- byte-identical
//! emitted modules with the lint applied, checked one pin at a time and then
//! all together -- are simply gone. The 2026-09-11 pass took one here
//! (`deserialize_from`) and four in `misc/serializable.rs`, and removed seven
//! `explicit_auto_deref` pins whose lint no longer fires on this code at all
//! (six here, one in `rpc/server.rs`): an allow that suppresses nothing is
//! clutter that reads as a warning.
//!
//! Pinned with an item-scoped `#[allow]`: 43 attributes on 42 items across 15
//! families. Lifting every one of them yields 70 findings across 11 families
//! on 33 items. Four pinned families -- `borrowed_box`, `unnecessary_cast`,
//! `upper_case_acronyms`, `wrong_self_convention` -- produce no finding under
//! this clippy, so those pins are inert today, and the two ABI hazards they
//! once guarded (renaming the emitted enumerator and `DisconnectBehavior_QUEUE()`
//! accessor; changing an emitted method signature) cannot currently be
//! re-measured because `--fix` has nothing to apply. Two live families still
//! change the provider's ABI when taken, both re-verified 2026-09-11:
//!
//!   * `ptr_arg` retypes exported `clientpool_select` from
//!     `const rusty::Vec<rusty::Arc<Client>>&` to
//!     `std::span<const rusty::Arc<Client>>` -- declaration, definition and
//!     the call site (6 emitted lines). Its suggestion is not
//!     `MachineApplicable`, so measuring it means applying it by hand;
//!   * `derivable_impls` inlines `FutureAttr::default_()` into the class and
//!     deletes its out-of-line definition, i.e. removes a provider symbol
//!     (7 emitted lines).
//!
//! And the largest family: 40 `explicit_auto_deref` sites in this file (43
//! crate-wide) under 16 item-scoped pins (19 crate-wide), every one of which
//! changes emitted C++ when taken. Three shapes, each named at its pin below:
//! a `const T&` that binds to the `Arc`/`Box`/guard handle instead of the
//! pointee once the emitter's `deref_if_pointer_like` unwrap is gone (the
//! common case -- `const FiberChannel& fiber = fc;`); a lock guard bound by
//! value, `auto` in place of `const auto&&`; and a `std::move` out of a
//! shared `Arc`'s field. `clippy --fix` cannot help here -- those suggestions
//! are `MachineApplicable` but the emitted-C++ consequence is invisible to it.
//!
//! No blanket `#![allow]` is used: the pins are per item so a future edit to
//! any other function is still linted.

#![allow(unsafe_code, non_camel_case_types, non_snake_case)]

#[allow(unused_imports)]
use crate::reactor as _;
#[allow(unused_imports)]
use crate::threading as _;

use std::cell::{Cell, RefCell};
use std::collections::{BTreeMap, HashMap, VecDeque};
// (`std::ffi::CStr` is deliberately NOT imported: see `clientconn_addr_to_string`.)
use std::sync::atomic::{AtomicBool, AtomicU64};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, Weak};
use std::time::Duration;
use crate::rand::randgen_range;

// Retain the C++ module dependency for the canonical callback template.
#[allow(unused_imports)]
use crate::callback_wrapper as _;



use crate::basetypes::{Counter, Time};
use crate::callbacks::CallbackManager;
type NullableCallbackManager = Option<Arc<CallbackManager>>;
use crate::channel::{
    channel_error_to_string, ChannelConnectionBase, ChannelConnectionProxy, ChannelError,
    ChannelFactoryBase, ChannelFactoryProxy, ChannelFrame, ConnectResult, OnClosedCallback,
    OnErrorCallback, OnFrameCallback,
    NullableChannelConnectionProxy, NullableChannelFactoryProxy,
};
use crate::circuit_breaker::{CircuitBreaker, CircuitBreakerConfig, CircuitState};
use crate::connection_metrics::ConnectionMetrics;
use crate::connection_state::{connection_state_to_string, ConnectionState, ConnectionStateMachine};
use crate::debugging::verify_failed;
use crate::errors::RpcError;
use crate::fiber_channel::{FiberChannel, OwnedFrame};
use crate::heartbeat::{HeartbeatConfig, HeartbeatManager, HeartbeatTimeoutCallback};
use crate::load_balancer::{LoadBalancer, LoadBalancerState, LoadBalancingStrategy};
use crate::logging::{log_line, Log};
use crate::misc::OneTimeJob;
use crate::reconnect_policy::{ReconnectPolicy};
use crate::request_options::{RequestOptions, TimeoutType};
use crate::request_queue::{
    OverflowStrategy, QueuedRequest, QueuedRequestCallback, RequestQueue, RequestQueueConfig,
    kRequestQueueExpiredError, rq_invoke_callback_safely,
};
use crate::serializable::{
    BinaryReadArchive, BinaryWriteArchive, BufferSink, BufferSource, SinkProxy, SourceProxy,
};
use crate::tcp_channel::{make_tcp_factory_proxy, TcpFactory};

// The public aliases share the canonical reactor implementation.
pub type Fiber = crate::reactor::Fiber;
pub type PollThread = crate::reactor::PollThread;

pub type WeakClientConnection = Weak<ClientConnection>;
pub type FutureResult = Result<Arc<Future>, i32>;
pub type AsyncReplyCallback = Option<Box<dyn FnMut(i32, *const u8, usize) + Send>>;
pub type OnReconnectCompleteCallbackFn = Option<Box<dyn FnMut(bool) + Send>>;
pub type OnServerRestartCallbackFn = Option<Box<dyn FnMut(u64, u64) + Send>>;
pub type OnConnectedCallbackFn = Box<dyn Fn() + Send + Sync>;
pub type OnErrorCallbackFn = Box<dyn Fn(RpcError, &str) + Send + Sync>;
pub type OnReconnectedCallbackFn = Box<dyn Fn(bool) + Send + Sync>;

// Use the canonical sparse integer values for wire headers.
type v32 = crate::basetypes::v32;
type v64 = crate::basetypes::v64;
pub type LegacyCallbackWrapper<F> = crate::callback_wrapper::detail::CallbackWrapper<F>;

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
    randgen_range(min, max)
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
pub fn client_log_line(level: i32, line: i32, file: *const i8, message: String) {
    // SAFETY: all canonical callers currently pass a null file pointer; the
    // owned message remains live through the synchronous logging call.
    unsafe { log_line(level, line, file, &message) }
}

pub fn client_text(text: &str) -> String {
    text.to_string()
}

pub fn client_text_str(prefix: &str, value: &str, suffix: &str) -> String {
    let mut message: String = prefix.to_string();
    message += value;
    message += suffix;
    message
}

pub fn client_text_i32(prefix: &str, value: i32, suffix: &str) -> String {
    client_text_str(prefix, &value.to_string(), suffix)
}

pub fn client_text_u32_str(
    prefix: &str,
    value: u32,
    middle: &str,
    text: &str,
    suffix: &str,
) -> String {
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
) -> String {
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
) -> String {
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
) -> String {
    let mut message = client_text_str(prefix, first, middle);
    message += second;
    message += suffix;
    message
}

pub struct ReplyBuffer {
    body: Vec<u8>,
    src: BufferSource,
}

// SAFETY: src is empty or points into this buffer's owned Vec allocation.
// Moving the Vec leaves that allocation stable; filling replaces src before
// returning. Access through Future is serialized by its reply mutex.
unsafe impl Send for ReplyBuffer {}

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

pub fn deserialize_from<T: crate::serializable::Deserialize>(mut src: MutexGuard<ReplyBuffer>, value: &mut T) {
    let mut ar = BinaryReadArchive::new(client_source_proxy(&mut src.src));
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
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
    pub behavior: DisconnectBehavior,
    pub max_pending: usize,
    pub default_ttl_ms: u32,
    pub overflow: OverflowStrategy,
    pub enabled: bool,
}

impl Copy for BufferingConfig {}

impl BufferingConfig {
    pub fn new() -> BufferingConfig {
        BufferingConfig {
            behavior: DisconnectBehavior::QUEUE,
            max_pending: 1000usize,
            default_ttl_ms: 30000u32,
            overflow: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    pub fn defaults() -> BufferingConfig {
        BufferingConfig::new()
    }

    pub fn disabled() -> BufferingConfig {
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
    pub fn to_queue_config(&self) -> RequestQueueConfig {
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
    pub enabled: bool,
    pub idle_sec: i32,
    pub interval_sec: i32,
    pub count: i32,
}

impl Copy for KeepaliveConfig {}

impl KeepaliveConfig {
    pub fn new() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 60i32, interval_sec: 10i32, count: 5i32 }
    }

    pub fn aggressive() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 10i32, interval_sec: 2i32, count: 3i32 }
    }

    pub fn relaxed() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 60i32, interval_sec: 10i32, count: 5i32 }
    }

    pub fn disabled() -> KeepaliveConfig {
        KeepaliveConfig { enabled: false, idle_sec: 0i32, interval_sec: 0i32, count: 0i32 }
    }
}

#[derive(Clone)]
pub struct PoolConfig {
    pub min_connections: i32,
    pub max_connections: i32,
    pub idle_timeout_ms: u64,
    pub health_check_enabled: bool,
    pub unhealthy_threshold_percent: u64,
    pub min_requests_for_health: u64,
    pub load_balancing: LoadBalancingStrategy,
}

impl Copy for PoolConfig {}

impl PoolConfig {
    pub fn new() -> PoolConfig {
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

    pub fn defaults() -> PoolConfig {
        PoolConfig::new()
    }

    pub fn aggressive() -> PoolConfig {
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

    pub fn conservative() -> PoolConfig {
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

    pub fn no_health_check() -> PoolConfig {
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

pub type FutureCallback = LegacyCallbackWrapper<Box<dyn Fn(Arc<Future>) + Send + Sync>>;

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
    completion_callbacks: Vec<Option<Box<dyn FnMut() + Send>>>,
}

impl FutureState {
    fn new() -> FutureState {
        FutureState { ready: false, timed_out: false, completion_callbacks: Vec::<Option<Box<dyn FnMut() + Send>>>::new() }
    }
}

pub struct Future {
    xid_: i64,
    error_code_: ClientCloneCell<i32>,
    attr_: FutureAttr,
    reply_: Mutex<ReplyBuffer>,
    timeout_: u64,
    state_: Mutex<FutureState>,
    ready_cond_: Condvar,
    options_: ClientCloneCell<RequestOptions>,
    timeout_type_: ClientCloneCell<TimeoutType>,
    retry_count_: ClientCloneCell<u16>,
}

impl Future {
    fn new(xid: i64, attr: FutureAttr) -> Future {
        Future {
            xid_: xid,
            error_code_: ClientCloneCell::new(0i32),
            attr_: attr,
            reply_: Mutex::<ReplyBuffer>::new(reply_buffer_empty()),
            timeout_: 1000000u64,
            state_: Mutex::<FutureState>::new(FutureState::new()),
            ready_cond_: Condvar::new(),
            options_: ClientCloneCell::new(RequestOptions::defaults()),
            timeout_type_: ClientCloneCell::new(TimeoutType::NONE),
            retry_count_: ClientCloneCell::new(0u16),
        }
    }

    // clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
    #[allow(clippy::arc_with_non_send_sync)]
    fn create(xid: i64, attr: FutureAttr) -> Arc<Future> {
        Arc::new(Future::new(xid, attr))
    }

    pub fn ready(&self) -> bool {
        let guard = self.state_.lock().unwrap();
        guard.ready
    }

    pub fn wait(&self) {
        if self.timeout_ > 0u64 {
            let sec: f64 = (self.timeout_ as f64) / 1000000.0;
            self.timed_wait(sec);
            return;
        }
        let guard = self.state_.lock().unwrap();
        // std::sync::Condvar is @safe; wait WHILE not-ready and not-timed-out.
        let _reacquired = self.ready_cond_.wait_while(guard, |s| !s.ready && !s.timed_out).unwrap();
    }

    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it replaces the guard's `.ready`/`.timed_out` accesses with structural-dispatch `decltype(auto)` lambdas instead of `deref_if_pointer_like(guard)` (4 emitted lines in srpc.client.cppm).
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

    pub fn wait_with_options(&self) -> bool {
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

    fn add_completion_callback(&self, callback: Option<Box<dyn FnMut() + Send>>) -> bool {
        let mut guard = self.state_.lock().unwrap();
        if guard.ready || guard.timed_out {
            return false;
        }
        guard.completion_callbacks.push(callback);
        true
    }

    pub fn get_reply(&self) -> MutexGuard<'_, ReplyBuffer> {
        self.wait();
        self.reply_.lock().unwrap()
    }

    pub fn get_error_code(&self) -> i32 {
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

    pub fn set_options(&self, opts: &RequestOptions) {
        self.options_.set(*opts)
    }

    pub fn get_timeout_type(&self) -> TimeoutType {
        self.timeout_type_.get()
    }

    fn set_timeout_type(&mut self, type_: TimeoutType) {
        self.timeout_type_.set(type_)
    }

    pub fn get_retry_count(&self) -> u16 {
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
        let mut completion_callbacks: Vec<Option<Box<dyn FnMut() + Send>>>;
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
            if callback.is_some() {
                callback.as_mut().unwrap()();
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

// Why this constant exists: every request and heartbeat serializes into a fresh
// `BufferSink` whose Vec starts empty, and the header alone is three to four
// small `write_bytes` appends -- so an empty Vec re-allocates on nearly every
// append while it doubles its way up. Measured on the TCP fast-path benchmark,
// amortized Vec growth plus the allocator traffic it induces was 20-30% of
// BOTH servers' samples (`RawVecInner::grow_amortized` in the C++ lane's vec
// port, `finish_grow` under rustc). Seeding the hot sinks with one 64-byte
// allocation covers the whole header-plus-small-payload class in a single
// alloc; larger payloads grow amortized from 64 exactly as before. 64 is
// deliberately modest: these sinks are per-call locals, not pooled buffers.
// The same seeding is spelled kReplySinkInitialCapacity in rpc/server.rs:
// per-module (the flat-import contract does not admit a cross-module
// root-level const), and per-NAME (two modules exporting one name into
// namespace srpc is an import-time ambiguity for any TU importing both --
// measured: the dual-compile importer and rpcbench both failed to compile).
// Like `kAsyncSlotCount` above, this is a strong `R` symbol in the module
// object (P1815 attaches module-scope consts to the module; see
// EXPECTED_TOTAL_PROVIDER_SYMBOLS' comment in check_srpc_crate_mode.py), so
// it has a pinned row in ABI_SPECS["srpc.client"].
pub const kRequestSinkInitialCapacity: usize = 64;


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
    // Lock order is lifecycle, channel slots, queue, queued futures, async
    // slots, pending futures. No channel or user callout holds this lock.
    lifecycle_: Mutex<ClientBindingState>,
    // Box constructs the move-disabled C++ FiberChannel in its final slot;
    // Arc then shares that slot with suspended receivers and replacement.
    #[allow(clippy::redundant_allocation)]
    fiber_channel_: std::sync::Mutex<Option<Arc<Box<FiberChannel>>>>,
    direct_channel_: std::sync::Mutex<Option<Arc<ChannelConnectionProxy>>>,
    closing_: AtomicBool,
    channel_mode_: ClientCloneCell<bool>,
    factory_: Mutex<Option<Arc<Mutex<ChannelFactoryProxy>>>>,
    xid_counter_: Counter,
    pending_fu_: std::sync::Mutex<HashMap<i64, Arc<Future>>>,
    queued_fu_: Arc<Mutex<HashMap<i64, Arc<Future>>>>,
    replaying_: Arc<AtomicBool>,
    pending_cb_slots_: std::sync::Mutex<Vec<Option<AsyncReplyCallback>>>,
    // This private state machine never installs StateChangeCallback. Lifecycle
    // changes its stored state directly; CallbackManager notifications run
    // after release instead of calling callback-capable transition methods.
    state_machine_: ConnectionStateMachine,
    reconnect_policy_: ClientCloneCell<ReconnectPolicy>,
    reconnect_: ReconnectState,
    reconnect_address_: ClientCloneCell<String>,
    buffering_config_: ClientCloneCell<BufferingConfig>,
    pending_queue_: RequestQueue,
    server_instance_id_: ClientCloneCell<u64>,
    on_server_restart_: Mutex<Arc<Mutex<OnServerRestartCallbackFn>>>,
    keepalive_config_: ClientCloneCell<KeepaliveConfig>,
    heartbeat_manager_: HeartbeatManager,
    circuit_breaker_: CircuitBreaker,
    callback_manager_: NullableCallbackManager,
    last_activity_time_: ClientCloneCell<u64>,
    metrics_: Arc<ConnectionMetrics>,
    weak_self_: WeakClientConnection,
    host_: String,
    packets_: u64,
    paused_: ClientCloneCell<bool>,
    is_client_mode_: bool,
}

struct ClientBindingState {
    generation: u64,
    active: bool,
}

struct ClientPendingBatch {
    queued: VecDeque<QueuedRequest>,
    buffered: HashMap<i64, Arc<Future>>,
    callbacks: Vec<AsyncReplyCallback>,
    futures: HashMap<i64, Arc<Future>>,
}

struct ClientReplayScope {
    running: Arc<AtomicBool>,
}

impl Drop for ClientReplayScope {
    fn drop(&mut self) {
        self.running.store(false, std::sync::atomic::Ordering::Release);
    }
}

// Auto traits enforce that shared connection state and callback captures are
// synchronized. Channel operations pin ownership and run outside slot locks.

impl Drop for ClientConnection {
    fn drop(&mut self) {
        self.reconnect_.reconnect_abort_.store(true, std::sync::atomic::Ordering::Release);
        self.reconnect_.reconnecting_.store(false, std::sync::atomic::Ordering::Release);
        self.invalidate_pending_futures();
    }
}

impl ClientConnection {
    fn new(poll_thread_worker: Arc<PollThread>) -> ClientConnection {
        ClientConnection {
            poll_thread_worker_: poll_thread_worker,
            lifecycle_: Mutex::new(ClientBindingState { generation: 0, active: false }),
            fiber_channel_: std::sync::Mutex::<Option<Arc<Box<FiberChannel>>>>::new(None),
            direct_channel_: std::sync::Mutex::<Option<Arc<ChannelConnectionProxy>>>::new(None),
            closing_: AtomicBool::new(false),
            channel_mode_: ClientCloneCell::<bool>::new(false),
            factory_: Mutex::new(None),
            xid_counter_: Counter::new(0i64),
            pending_fu_: std::sync::Mutex::<HashMap<i64, Arc<Future>>>::new(HashMap::<i64, Arc<Future>>::new()),
            queued_fu_: Arc::new(Mutex::new(HashMap::new())),
            replaying_: Arc::new(AtomicBool::new(false)),
            pending_cb_slots_: std::sync::Mutex::<Vec<Option<AsyncReplyCallback>>>::new(make_prefilled_cb_slots()),
            state_machine_: ConnectionStateMachine::new(),
            reconnect_policy_: ClientCloneCell::<ReconnectPolicy>::new(ReconnectPolicy::new()),
            reconnect_: ReconnectState {
                reconnecting_: AtomicBool::new(false),
                reconnect_abort_: AtomicBool::new(false),
                channel_reconnect_attempts_: AtomicU64::new(0),
            },
            // In expected-type position the emitter lowers `Default::default()`
            // to `rusty::default_like<T>()`, the same shape the
            // `on_server_restart_` field below already uses.
            reconnect_address_: ClientCloneCell::<String>::new(Default::default()),
            buffering_config_: ClientCloneCell::<BufferingConfig>::new(BufferingConfig::defaults()),
            pending_queue_: make_pending_queue(&BufferingConfig::defaults().to_queue_config()),
            server_instance_id_: ClientCloneCell::<u64>::new(0u64),
            on_server_restart_: Mutex::new(Arc::new(Mutex::<OnServerRestartCallbackFn>::new(Default::default()))),
            keepalive_config_: ClientCloneCell::<KeepaliveConfig>::new(KeepaliveConfig::new()),
            heartbeat_manager_: HeartbeatManager::new(&HeartbeatConfig::disabled()),
            circuit_breaker_: CircuitBreaker::new(CircuitBreakerConfig::disabled()),
            callback_manager_: Some(Arc::<CallbackManager>::new(CallbackManager::new())),
            last_activity_time_: ClientCloneCell::<u64>::new(0u64),
            metrics_: Arc::new(ConnectionMetrics::new()),
            weak_self_: WeakClientConnection::new(),
            host_: Default::default(),
            packets_: 0u64,
            paused_: ClientCloneCell::<bool>::new(false),
            is_client_mode_: false,
        }
    }

    // --- delegating methods (&mut self → non-const free fns) ---
    // recv-loop cluster: &self over interior-mutable state, so it is callable
    // directly through a shared Arc<ClientConnection> (no const_cast at the
    // fiber/job/channel-callback spawn sites).
    fn run_recv_loop(&self) { clientconn_run_recv_loop(self); }
    fn decode_response_and_notify(&self, bytes: *const u8, size: usize) { clientconn_decode_response_and_notify(self, bytes, size); }
    // clippy::unnecessary_cast -- measured: drops the emitted rusty::detail::ptr_cast<const int8_t*>. See the Task-2 measurement block above.
    #[allow(clippy::unnecessary_cast)]
    fn on_channel_closed_fan_out(&self) {
        let generation = self.lifecycle_.lock().unwrap().generation;
        self.on_binding_closed(generation);
    }

    // Keep the explicit Arc payload accesses used by the existing reconnect worker.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it replaces the deref_if_pointer_like unwraps on `conn` with a raw `(*conn)` and wraps the resulting bool in a pointer-like check (4 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref)]
    fn on_binding_closed(&self, generation: u64) {
        let batch;
        let user_initiated_closing;
        {
            let mut lifecycle = self.lifecycle_.lock().unwrap();
            if lifecycle.generation != generation
                || (!lifecycle.active && self.state_machine_.state() != ConnectionState::CONNECTING) {
                return;
            }
            lifecycle.active = false;
            let prev_state = self.state_machine_.state();
            user_initiated_closing = prev_state == ConnectionState::DISCONNECTING
                || prev_state == ConnectionState::DISCONNECTED
                || self.reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire);
            if !user_initiated_closing {
                self.state_machine_.state_field.set(ConnectionState::FAILED);
            }
            self.heartbeat_manager_.reset();
            batch = self.detach_pending_futures();
        }
        if !user_initiated_closing {
            self.invoke_error_callback(CLIENT_ERR_CONNECTION_RESET, &client_text("channel closed"));
        }
        self.notify_pending_futures(batch);
        if !user_initiated_closing {
            self.invoke_disconnected_callback();
        }

        // Trigger channel-mode auto-reconnect if the policy allows. The
        // counter is bumped the moment the fan-out reaches this branch (the
        // observability signal tests assert), then a spawn does the work
        // unless reconnect was aborted.
        let addr: String = self.reconnect_address_.get();
        if self.reconnect_policy_.get().auto_reconnect && !addr.is_empty() {
            self.reconnect_.channel_reconnect_attempts_.fetch_add(1, std::sync::atomic::Ordering::AcqRel);

            let reconnect_aborted: bool = self.reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire);
            if reconnect_aborted {
                return;
            }
            let weak_conn: WeakClientConnection = self.weak_self_.clone();
            drop(crate::threading::spawn_abort_on_panic(move || {
                let conn_opt = weak_conn.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                let conn_aborted: bool = (*conn).reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire);
                if !(*conn).reconnect_policy_.get().auto_reconnect || conn_aborted {
                    return;
                }
                let state = (*conn).connection_state();
                if (state as i32) == (ConnectionState::FAILED as i32)
                    || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                    (*conn).reconnect(Default::default());
                }
            }));
        }
    }
    // Shared connection operations synchronize channel slots and configuration.
    fn connect_via_factory(&self, addr: *const i8) -> i32 { clientconn_connect_via_factory(self, addr) }
    fn reset_channel_mode_for_reconnect(&self) {
        self.close();
    }
    fn binding_is_current(&self, generation: u64) -> bool {
        let lifecycle = self.lifecycle_.lock().unwrap();
        lifecycle.active && lifecycle.generation == generation
    }

    fn connect(&self, addr: *const i8) -> i32 {
        let generation = Cell::new(0u64);
        let result = self.connect_attempt(addr, &generation);
        if result != 0 {
            return result;
        }
        self.invoke_connected_callback();
        if self.binding_is_current(generation.get()) { 0 } else { CLIENT_ERR_CANCELED }
    }

    fn connect_attempt(&self, addr: *const i8, attempt_generation: &Cell<u64>) -> i32 {
        let generation;
        let admitted = {
            let mut lifecycle = self.lifecycle_.lock().unwrap();
            generation = lifecycle.generation + 1;
            attempt_generation.set(generation);
            if self.state_machine_.can_connect() {
                lifecycle.generation += 1;
                lifecycle.active = false;
                self.closing_.store(false, std::sync::atomic::Ordering::Release);
                self.state_machine_.state_field.set(ConnectionState::CONNECTING);
                true
            } else {
                false
            }
        };
        if !admitted {
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
        clientconn_connect_factory_for_binding(self, addr, generation)
    }
    fn bind_channel(&self, channel: NullableChannelConnectionProxy) {
        if channel.is_none() {
            return;
        }
        let channel = channel.unwrap();
        let channel = self.replace_fiber_channel(channel);
        self.channel_mode_.set(true);

        // Capture a Weak so the parked recv-loop fiber doesn't extend the
        // connection lifetime (would cycle through fiber_channel_ ownership).
        let weak_self: WeakClientConnection = self.weak_self_.clone();
        // The recv-loop fiber must live on the reactor that fires the proxy's
        // on_frame/on_closed callbacks (single-threaded IntEvent signaling);
        // the caller picks the thread (see bind_channel_via_poll_thread).
        // SAFETY: foreign named-module boundary; the file pointer is null and
        // the closure owns everything it captures.
        Fiber::create_run(move || {
            let conn_opt = weak_self.upgrade();
            if conn_opt.is_none() {
                return;
            }
            let conn = conn_opt.unwrap();
            clientconn_run_recv_loop_on_channel(&conn, channel.clone());
        });
    }
    // Preserve the independently pinned channel allocation when cloning ownership.
    #[allow(clippy::redundant_allocation)]
    fn fiber_channel(&self) -> Option<Arc<Box<FiberChannel>>> {
        self.fiber_channel_.lock().unwrap().clone()
    }
    // Callback installation needs exclusive Box access before Arc publication.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber` to the Arc<Box<..>> handle `old` instead of the channel; both unwraps dropped (2 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref, clippy::redundant_allocation)]
    fn replace_fiber_channel(&self, channel: ChannelConnectionProxy) -> Arc<Box<FiberChannel>> {
        let config = self.keepalive_config_.get();
        let proxy: &dyn ChannelConnectionBase = &*channel;
        let _ = proxy.set_keepalive(config.enabled, config.idle_sec, config.interval_sec, config.count);
        let mut boxed = clientconn_make_fiber_channel(channel);
        boxed.bind_callbacks();
        let channel = Arc::new(boxed);
        let retired;
        let retired_direct;
        let batch;
        {
            let mut lifecycle = self.lifecycle_.lock().unwrap();
            lifecycle.generation += 1;
            lifecycle.active = true;
            retired = self.fiber_channel_.lock().unwrap().replace(channel.clone());
            retired_direct = self.direct_channel_.lock().unwrap().take();
            self.closing_.store(false, std::sync::atomic::Ordering::Release);
            self.channel_mode_.set(true);
            batch = self.detach_pending_futures();
        }
        if let Some(old) = retired {
            let fiber: &FiberChannel = &**old;
            fiber.close();
        }
        if let Some(old) = retired_direct {
            let proxy: &dyn ChannelConnectionBase = &**old;
            proxy.close();
        }
        self.notify_pending_futures(batch);
        channel
    }
    fn bind_channel_via_poll_thread(&self, channel: NullableChannelConnectionProxy) { clientconn_bind_channel_via_poll_thread(self, channel); }
    // Direct on_frame / on_closed binding: bypasses FiberChannel and the
    // recv-loop fiber entirely, installing the callbacks on the proxy itself.
    // Both fire on whichever thread the channel layer dispatches from -- for
    // TCP that is the poll thread, the same one whose handle_read parses the
    // frames. send_frame remains callable from any thread (dispatch_frame_via
    // _channel uses it from user threads).
    //
    // Callbacks are installed before the proxy moves into `direct_channel_`.
    // Each dispatch clones the channel's Arc before releasing the slot lock.
    // Removing the slot cannot destroy a channel still executing a callback.
    // clippy::type_complexity -- the same spelling rpc/fiber_channel.cpp uses for this callback; factoring it into an alias would emit a new `using`. See the Task-2 measurement block above.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const ClientConnection& receiver` to the `owner` handle (twice) and `const FiberChannel& fiber` to the `old` handle instead of their pointees (6 emitted lines in srpc.client.cppm).
    #[allow(clippy::type_complexity, clippy::explicit_auto_deref)]
    fn bind_channel_direct(&self, channel: NullableChannelConnectionProxy, generation: u64) -> bool {
        if channel.is_none() {
            return false;
        }
        let mut channel = channel.unwrap();
        let config = self.keepalive_config_.get();
        let proxy: &dyn ChannelConnectionBase = &*channel;
        let _ = proxy.set_keepalive(config.enabled, config.idle_sec, config.interval_sec, config.count);
        // A dispatch can outlive the slot that originally owned the channel.
        // Upgrade the weak receiver for each callback to pin it for that call.
        let frame_self: WeakClientConnection = self.weak_self_.clone();
        let closed_self: WeakClientConnection = self.weak_self_.clone();
        {
            // Concrete `Box<..>`, not the ChannelConnectionProxy alias: through
            // the alias the pointer-like check fails and the calls lower to
            // `channel.set_on_frame(..)` (dot) instead of `->` (docs 7.50).
            let ch: &mut Box<dyn ChannelConnectionBase> = &mut channel;
            let frame_callback: Box<dyn Fn(&ChannelFrame) + Send + Sync> = Box::new(move |f: &ChannelFrame| {
                if let Some(connection) = frame_self.upgrade() {
                    let owner: Arc<ClientConnection> = connection;
                    let receiver: &ClientConnection = &*owner;
                    clientconn_decode_response_for_binding(receiver, generation, f.payload, f.size);
                }
            });
            ch.set_on_frame(OnFrameCallback::from_callable(frame_callback));
            let closed_callback: Box<dyn Fn(ChannelError) + Send + Sync> = Box::new(move |_reason: ChannelError| {
                if let Some(connection) = closed_self.upgrade() {
                    let owner: Arc<ClientConnection> = connection;
                    let receiver: &ClientConnection = &*owner;
                    receiver.on_binding_closed(generation);
                }
            });
            ch.set_on_closed(OnClosedCallback::from_callable(closed_callback));
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
        let proxy: &dyn ChannelConnectionBase = &*channel;
        if proxy.is_closed() {
            self.on_binding_closed(generation);
            return false;
        }
        let channel = Arc::new(channel);
        let retired_direct;
        let retired_fiber;
        {
            let mut lifecycle = self.lifecycle_.lock().unwrap();
            if lifecycle.generation != generation
                || self.state_machine_.state() != ConnectionState::CONNECTING {
                return false;
            }
            retired_direct = self.direct_channel_.lock().unwrap().replace(channel);
            retired_fiber = self.fiber_channel_.lock().unwrap().take();
            lifecycle.active = true;
            self.closing_.store(false, std::sync::atomic::Ordering::Release);
            self.channel_mode_.set(true);
            self.state_machine_.state_field.set(ConnectionState::CONNECTED);
        }
        if let Some(old) = retired_direct {
            let proxy: &dyn ChannelConnectionBase = &**old;
            proxy.close();
        }
        if let Some(old) = retired_fiber {
            let fiber: &FiberChannel = &**old;
            fiber.close();
        }
        true
    }
    fn direct_channel(&self) -> Option<Arc<ChannelConnectionProxy>> {
        let guard = self.direct_channel_.lock().unwrap();
        (*guard).clone()
    }
    fn bind_factory(&self, factory: NullableChannelFactoryProxy) {
        if factory.is_none() {
            return;
        }
        let factory = factory.unwrap();
        let factory = Arc::new(Mutex::new(factory));
        let retired = self.factory_.lock().unwrap().replace(factory);
        drop(retired);
    }
    fn abort_reconnect(&mut self) { self.reconnect_.reconnect_abort_.store(true, std::sync::atomic::Ordering::Release); }
    fn set_callback_manager(&mut self, callback_manager: &NullableCallbackManager) {
        if callback_manager.is_some() {
            self.callback_manager_ = callback_manager.clone();
        }
    }

    // --- delegating methods (&self → const free fns) ---
    fn invalidate_pending_futures(&self) {
        let batch = {
            let _lifecycle = self.lifecycle_.lock().unwrap();
            self.detach_pending_futures()
        };
        self.notify_pending_futures(batch);
    }

    // Called only with lifecycle held. Transfer every completion owner before
    // the first callback can reconnect or publish a replacement request.
    fn detach_pending_futures(&self) -> ClientPendingBatch {
        let queued = self.pending_queue_.drain();
        let buffered = std::mem::take(&mut *self.queued_fu_.lock().unwrap());
        let mut callbacks = Vec::new();
        {
            let mut slots = self.pending_cb_slots_.lock().unwrap();
            for slot in &mut *slots {
                if let Some(callback) = slot.take() {
                    callbacks.push(callback);
                }
            }
        }
        let futures = std::mem::take(&mut *self.pending_fu_.lock().unwrap());
        ClientPendingBatch { queued, buffered, callbacks, futures }
    }

    fn notify_pending_futures(&self, batch: ClientPendingBatch) {
        // Queue callbacks now find their old xid absent. They cannot remove
        // ownership admitted by a reentrant reconnect because xids are unique.
        for request in batch.queued {
            rq_invoke_callback_safely(request.callback, CLIENT_ERR_NOT_CONNECTED);
        }
        for (_xid, future) in batch.buffered {
            self.metrics_.record_queue_drop();
            future.error_code_.set(CLIENT_ERR_NOT_CONNECTED);
            future.notify_ready(future.clone());
        }
        for mut callback in batch.callbacks {
            self.metrics_.record_request_dropped();
            callback.as_mut().unwrap()(CLIENT_ERR_NOT_CONNECTED, core::ptr::null(), 0);
        }
        for (_xid, future) in batch.futures {
            self.metrics_.record_request_dropped();
            future.error_code_.set(CLIENT_ERR_NOT_CONNECTED);
            future.notify_ready(future.clone());
        }
    }
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it replaces the deref_if_pointer_like unwrap on `fu` with a raw `(*fu)` for the `error_code_` store (2 emitted lines in srpc.client.cppm).
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
    pub fn close(&self) {
        self.close_binding(None);
    }

    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber_ref` to the handle `channel` instead of the channel (2 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref)]
    fn close_binding(&self, expected_generation: Option<u64>) {
        let direct;
        let fiber;
        let batch;
        let notify_disconnected;
        {
            let mut lifecycle = self.lifecycle_.lock().unwrap();
            if let Some(expected) = expected_generation {
                if lifecycle.generation != expected {
                    return;
                }
            }
            if self.closing_.swap(true, std::sync::atomic::Ordering::AcqRel) {
                return;
            }
            lifecycle.active = false;
            lifecycle.generation += 1;
            let previous = self.state_machine_.state();
            notify_disconnected = previous == ConnectionState::CONNECTED
                || previous == ConnectionState::DISCONNECTING;
            self.state_machine_.state_field.set(ConnectionState::DISCONNECTED);
            direct = std::mem::take(&mut *self.direct_channel_.lock().unwrap());
            fiber = std::mem::take(&mut *self.fiber_channel_.lock().unwrap());
            self.channel_mode_.set(false);
            self.heartbeat_manager_.reset();
            batch = self.detach_pending_futures();
        }
        if let Some(channel) = direct {
            let channel_ref: &dyn ChannelConnectionBase = &**channel;
            channel_ref.close();
        }
        if let Some(channel) = fiber {
            let fiber_ref: &FiberChannel = &**channel;
            fiber_ref.close();
        }
        self.notify_pending_futures(batch);
        if notify_disconnected {
            self.invoke_disconnected_callback();
        }
    }
    fn mark_closing(&self) -> u64 {
        let generation;
        let batch = {
            let mut lifecycle = self.lifecycle_.lock().unwrap();
            if self.state_machine_.state() == ConnectionState::DISCONNECTING {
                return lifecycle.generation;
            }
            lifecycle.active = false;
            lifecycle.generation += 1;
            generation = lifecycle.generation;
            self.reconnect_.reconnect_abort_.store(true, std::sync::atomic::Ordering::Release);
            if !self.state_machine_.is_terminal() {
                self.state_machine_.state_field.set(ConnectionState::DISCONNECTING);
            }
            self.detach_pending_futures()
        };
        self.notify_pending_futures(batch);
        generation
    }
    /// Returns CLIENT_ERR_BUSY if another reconnect owns the attempt, including
    /// calls from its callbacks. Returns CLIENT_ERR_CANCELED if a completion
    /// callback replaces or closes the binding established by this attempt.
    pub fn reconnect(&self, on_complete: OnReconnectCompleteCallbackFn) -> i32 { clientconn_reconnect(self, on_complete) }
    pub fn set_buffering_config(&self, config: &BufferingConfig) {
        self.buffering_config_.set(*config);
        if !self.pending_queue_.empty() {
            self.pending_queue_.clear_all(CLIENT_ERR_CONNECTION_ABORTED);
        }
        self.pending_queue_.update_config(config.to_queue_config());
    }
    pub fn set_heartbeat_config(&self, config: &HeartbeatConfig) {
        self.heartbeat_manager_.set_config(config);
        // Capture a weak self-handle by move so the escaping timeout closure
        // does not keep the connection alive (mirrors the legacy [weak_conn]
        // C++ lambda; a move closure's owned capture is escape-safe).
        let weak_conn: WeakClientConnection = self.weak_self_.clone();
        let mut on_timeout: HeartbeatTimeoutCallback = Some(Box::new(move || {
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
        }));
        // Keep the take: C++ lowering otherwise makes this local const and copies it.
        self.heartbeat_manager_.set_on_timeout(on_timeout.take());
    }
    pub fn heartbeat_config(&self) -> HeartbeatConfig { self.heartbeat_manager_.config() }
    pub fn set_circuit_breaker_config(&self, config: &CircuitBreakerConfig) { self.circuit_breaker_.set_config(*config); }
    pub fn circuit_breaker_config(&self) -> CircuitBreakerConfig { self.circuit_breaker_.config() }
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
    // Takes the owned string by reference (`const rusty::String&` in C++),
    // matching the `OnErrorCallbackFn` surface it forwards to; the literal
    // call sites build the owned string.
    fn invoke_error_callback(&self, err: i32, message: &str) {
        if let Some(manager) = self.callback_manager_.as_ref() {
            (**manager).invoke_on_error(clientconn_map_system_error(err), message);
        }
    }
    fn invoke_disconnected_callback(&self) {
        if let Some(manager) = self.callback_manager_.as_ref() {
            (**manager).invoke_on_disconnected();
        }
    }
    fn invoke_reconnecting_callback(&self) {
        if let Some(manager) = self.callback_manager_.as_ref() {
            (**manager).invoke_on_reconnecting();
        }
    }
    fn invoke_reconnected_callback(&self, success: bool) {
        if let Some(manager) = self.callback_manager_.as_ref() {
            (**manager).invoke_on_reconnected(success);
        }
    }
    fn invoke_connected_callback(&self) {
        if let Some(manager) = self.callback_manager_.as_ref() {
            (**manager).invoke_on_connected();
        }
    }
    unsafe fn dispatch_frame_via_channel(&self, body_bytes: *const u8, body_size: usize) -> ChannelError {
        clientconn_dispatch_frame_via_channel(self, body_bytes, body_size)
    }
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it replaces the deref_if_pointer_like unwraps on `conn` with a raw `(*conn)` and wraps the resulting bool in a pointer-like check (4 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref)]
    fn handle_error(&self) {
        let prev_state = self.state_machine_.state();
        let abort_flag: bool = self.reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire);
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
        let reconnect_aborted: bool = self.reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire);
        if self.reconnect_policy_.get().auto_reconnect && !reconnect_aborted {
            let addr: String = self.reconnect_address_.get();
            if addr.is_empty() {
                return;
            }
            let weak_conn: WeakClientConnection = self.weak_self_.clone();
            drop(crate::threading::spawn_abort_on_panic(move || {
                let conn_opt = weak_conn.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                let conn_aborted: bool = (*conn).reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire);
                if !(*conn).reconnect_policy_.get().auto_reconnect || conn_aborted {
                    return;
                }
                let state = (*conn).connection_state();
                if (state as i32) == (ConnectionState::FAILED as i32)
                    || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                    client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text("srpc::ClientConnection: auto-reconnect triggered after connection failure"));
                    (*conn).reconnect(Default::default());
                }
            }));
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
    pub fn handle_free(&self, xid: i64) {
        let mut guard = self.pending_fu_.lock().unwrap();
        if guard.remove(&xid).is_some() {
            self.metrics_.record_request_dropped();
        }
    }
    fn is_factory_bound(&self) -> bool { (*self.factory_.lock().unwrap()).is_some() }
    fn channel_reconnect_attempts_count(&self) -> u64 { self.reconnect_.channel_reconnect_attempts_.load(std::sync::atomic::Ordering::Acquire) }
    pub fn set_reconnect_policy(&self, policy: &ReconnectPolicy) { self.reconnect_policy_.set(*policy); }
    pub fn is_reconnecting(&self) -> bool { self.reconnect_.reconnecting_.load(std::sync::atomic::Ordering::Acquire) }
    pub fn pending_future_count(&self) -> usize { self.pending_fu_.lock().unwrap().len() }
    fn replay_pending_requests_for_test(&self) -> usize { self.replay_pending_requests() }
    fn update_pending_queue_config_for_test(&self, config: &RequestQueueConfig) { self.pending_queue_.update_config(*config); }
    pub fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) {
        let replacement = Arc::<Mutex<OnServerRestartCallbackFn>>::new(
            Mutex::<OnServerRestartCallbackFn>::new(callback));
        let retired = {
            let mut guard = self.on_server_restart_.lock().unwrap();
            std::mem::replace(&mut *guard, replacement)
        };
        drop(retired);
    }
    fn check_server_instance(&self, new_id: u64) -> bool {
        let old_id = self.server_instance_id_.get();
        self.server_instance_id_.set(new_id);
        if old_id != 0u64 && old_id != new_id {
            client_log_line(Log::INFO, 0i32, core::ptr::null(), client_text_u64_pair("Server restart detected: old_id=", old_id, " new_id=", new_id, ""));
            let callback: Arc<Mutex<OnServerRestartCallbackFn>> = self.on_server_restart_.lock().unwrap().clone();
            let mut cb_ref = callback.lock().unwrap();
            if cb_ref.is_some() {
                cb_ref.as_mut().unwrap()(old_id, new_id);
            }
            return true;
        }
        false
    }
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber` to the handle `channel` instead of the channel (2 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref)]
    pub fn set_keepalive(&self, config: &KeepaliveConfig) {
        self.keepalive_config_.set(*config);
        if let Some(channel) = self.direct_channel() {
            let proxy: &dyn ChannelConnectionBase = &**channel;
            let _ = proxy.set_keepalive(config.enabled, config.idle_sec, config.interval_sec, config.count);
        } else if let Some(channel) = self.fiber_channel() {
            let fiber: &FiberChannel = &**channel;
            let proxy: &dyn ChannelConnectionBase = &*fiber.ch_;
            let _ = proxy.set_keepalive(config.enabled, config.idle_sec, config.interval_sec, config.count);
        }
    }
    fn on_request_dispatched(&self, bytes: usize) {
        self.metrics_.record_bytes_sent(bytes as u64);
        self.update_last_activity(clientconn_monotonic_ms_now());
    }
    fn on_response_received(&self, bytes: usize) {
        self.metrics_.record_bytes_received(bytes as u64);
        self.update_last_activity(clientconn_monotonic_ms_now());
    }
    fn host(&self) -> String { self.host_.clone() }

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
    pub fn request_with_options<F>(&self, rpc_id: i32, options: &RequestOptions, attr: &FutureAttr, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) { clientconn_request_with_options(self, rpc_id, options, attr, write_fn) }
    pub fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: AsyncReplyCallback) -> Result<(), i32>
    where F: FnMut(&mut BinaryWriteArchive) { clientconn_request_async(self, rpc_id, write_fn, on_reply) }

    // --- trivial inline accessors ---
    fn is_channel_mode(&self) -> bool { self.channel_mode_.get() }
    fn install_self_weak_for_testing(&mut self, weak: WeakClientConnection) { self.weak_self_ = weak; }
    fn force_connected_for_testing(&mut self) { self.state_machine_.force_state(ConnectionState::CONNECTED); }
    fn set_reconnect_address_for_testing(&self, addr: String) { self.reconnect_address_.set(addr); }
    pub fn connected(&self) -> bool { self.state_machine_.is_connected() }
    pub fn connection_state(&self) -> ConnectionState { self.state_machine_.state() }
    fn reconnect_policy(&self) -> ReconnectPolicy { self.reconnect_policy_.get() }
    fn buffering_config(&self) -> BufferingConfig { self.buffering_config_.get() }
    pub fn pending_request_count(&self) -> usize { self.pending_queue_.size() }
    pub fn clear_pending_requests(&self, error_code: i32) { self.pending_queue_.clear_all(error_code); }
    pub fn server_instance_id(&self) -> u64 { self.server_instance_id_.get() }
    pub fn keepalive_config(&self) -> KeepaliveConfig { self.keepalive_config_.get() }
    pub fn circuit_breaker_state(&self) -> CircuitState { self.circuit_breaker_.state() }
    fn update_last_activity(&self, current_time_ms: u64) { self.last_activity_time_.set(current_time_ms); }
    fn last_activity_time(&self) -> u64 { self.last_activity_time_.get() }
    pub fn is_idle(&self, idle_ms: u64, current_time_ms: u64) -> bool {
        let last: u64 = self.last_activity_time_.get();
        if last == 0u64 { return false; }
        (current_time_ms - last) > idle_ms
    }
    fn validate_connection(&self) -> bool { self.state_machine_.is_connected() }
    // Explicit Arc dereference preserves the borrowed counter reference in C++.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it returns the `metrics_` Arc handle where `const ConnectionMetrics&` is declared (2 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref)]
    pub fn metrics(&self) -> &ConnectionMetrics { &*self.metrics_ }
    pub fn replay_pending_requests(&self) -> usize { clientconn_replay_pending_requests(self) }
    fn pause(&self) { self.paused_.set(true); }
    fn resume(&self) { self.paused_.set(false); }
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
    pending_factory_field: std::sync::Mutex<Option<ChannelFactoryProxy>>,
    // The Client retains the same counters as its connection so references
    // remain valid through close and reconnect, including callback reentry.
    metrics_field: Arc<ConnectionMetrics>,
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
            pending_factory_field: std::sync::Mutex::<Option<ChannelFactoryProxy>>::new(None),
            metrics_field: Arc::new(ConnectionMetrics::new()),
        }
    }

    // clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
    #[allow(clippy::arc_with_non_send_sync)]
    pub fn create(poll_thread_worker: Arc<PollThread>) -> Arc<Client> {
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

    pub fn request<F>(&self, rpc_id: i32, attr: &FutureAttr, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) {
        let guard = self.connection();
        if guard.is_none() {
            return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request(rpc_id, attr, write_fn)
    }

    pub fn request_with_options<F>(&self, rpc_id: i32, options: &RequestOptions, write_fn: F) -> FutureResult
    where F: FnMut(&mut BinaryWriteArchive) {
        let guard = self.connection();
        if guard.is_none() {
            return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        self.rpc_id_field.set(rpc_id);
        let attr: FutureAttr = FutureAttr { callback: Default::default() };
        guard.as_ref().unwrap().request_with_options(
            rpc_id,
            options,
            &attr,
            write_fn,
        )
    }

    pub fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: AsyncReplyCallback) -> Result<(), i32>
    where F: FnMut(&mut BinaryWriteArchive) {
        let guard = self.connection();
        if guard.is_none() {
            return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request_async(rpc_id, write_fn, on_reply)
    }


    pub fn connect(&self, addr: *const i8, client: bool) -> i32 {
        let conn: Arc<ClientConnection> = Arc::new_cyclic(|weak_conn| {
            let mut value = ClientConnection::new(self.poll_thread_worker_field.clone());
            value.weak_self_ = weak_conn.clone();
            value.callback_manager_ = Some(self.callback_manager_field.clone());
            value.metrics_ = self.metrics_field.clone();
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
            self.set_channel_factory(Some(make_tcp_factory_proxy(tcp_factory)));
        }

        {
            let mut guard = self.pending_factory_field.lock().unwrap();
            if guard.is_some() {
                let moved: ChannelFactoryProxy = guard.take().unwrap();
                conn.bind_factory(Some(moved));
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
    pub fn close(&self) {
        if let Some(conn_ref) = self.connection() {
            let generation = conn_ref.mark_closing();
            let conn_arc: Arc<ClientConnection> = conn_ref.clone();
            let close_job: Arc<OneTimeJob> =
                Arc::<OneTimeJob>::new(OneTimeJob::new(Box::new(move || {
                    conn_arc.close_binding(Some(generation));
                })));
            let close_job_erased: Arc<dyn crate::misc::Job> = close_job;
            self.poll_thread_worker_field.add(close_job_erased);
        }
    }

    pub fn handle_free(&self, xid: i64) {
        if let Some(conn) = self.connection() {
            conn.handle_free(xid);
        }
    }

    fn pause(&self) {
        if let Some(conn) = self.connection() {
            conn.pause();
        }
    }

    fn resume(&self) {
        if let Some(conn) = self.connection() {
            conn.resume();
        }
    }

    /// A concurrent or reentrant owned attempt returns CLIENT_ERR_BUSY.
    /// A callback that replaces the completed binding makes this call return
    /// CLIENT_ERR_CANCELED.
    pub fn reconnect(&self, mut on_complete: OnReconnectCompleteCallbackFn) -> i32 {
        let guard = self.connection();
        if guard.is_none() {
            if let Some(callback) = on_complete.as_mut() {
                callback(false);
            }
            return CLIENT_ERR_NOT_CONNECTED;
        }
        guard.as_ref().unwrap().reconnect(on_complete)
    }

    pub fn set_channel_factory(&self, factory: NullableChannelFactoryProxy) {
        if factory.is_none() {
            return;
        }
        let factory = factory.unwrap();
        let mut guard = self.pending_factory_field.lock().unwrap();
        *guard = Some(factory);
    }

    fn has_pending_channel_factory(&self) -> bool {
        let guard = self.pending_factory_field.lock().unwrap();
        guard.is_some()
    }

    pub fn pending_request_count(&self) -> usize {
        if let Some(conn) = self.connection() {
            return conn.pending_request_count();
        }
        0usize
    }

    pub fn clear_pending_requests(&self, error_code: i32) {
        if let Some(conn) = self.connection() {
            conn.clear_pending_requests(error_code);
        }
    }

    pub fn is_reconnecting(&self) -> bool {
        let guard = self.connection();
        guard.is_some() && guard.as_ref().unwrap().is_reconnecting()
    }

    fn host(&self) -> String {
        if let Some(conn) = self.connection() {
            return conn.host();
        }
        Default::default()
    }

    pub fn connected(&self) -> bool {
        let guard = self.connection();
        guard.is_some() && guard.as_ref().unwrap().connected()
    }

    pub fn connection_state(&self) -> ConnectionState {
        if let Some(conn) = self.connection() {
            return conn.connection_state();
        }
        ConnectionState::NEW
    }

    pub fn try_reconnect_if_needed(&self) -> bool {
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

    pub fn connection(&self) -> Option<Arc<ClientConnection>> {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return Some(guard.as_ref().unwrap().clone());
        }
        None
    }

    pub fn server_instance_id(&self) -> u64 {
        if let Some(conn) = self.connection() {
            return conn.server_instance_id();
        }
        0u64
    }

    pub fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) {
        if let Some(conn) = self.connection() {
            conn.set_on_server_restart(callback);
        }
    }

    fn check_server_instance(&self, new_id: u64) -> bool {
        if let Some(conn) = self.connection() {
            return conn.check_server_instance(new_id);
        }
        false
    }

    pub fn set_reconnect_policy(&self, policy: &ReconnectPolicy) {
        self.pending_reconnect_policy_field.set(*policy);
        if let Some(conn) = self.connection() {
            conn.set_reconnect_policy(policy);
        }
    }

    pub fn set_buffering_config(&self, config: &BufferingConfig) {
        if let Some(conn) = self.connection() {
            conn.set_buffering_config(config);
        }
    }

    pub fn set_keepalive(&self, config: &KeepaliveConfig) {
        self.pending_keepalive_config_field.set(*config);
        if let Some(conn) = self.connection() {
            conn.set_keepalive(config);
        }
    }

    pub fn keepalive_config(&self) -> KeepaliveConfig {
        if let Some(conn) = self.connection() {
            return conn.keepalive_config();
        }
        self.pending_keepalive_config_field.get()
    }

    pub fn set_heartbeat(&self, config: &HeartbeatConfig) {
        self.pending_heartbeat_config_field.set(*config);
        if let Some(conn) = self.connection() {
            conn.set_heartbeat_config(config);
        }
    }

    pub fn heartbeat_config(&self) -> HeartbeatConfig {
        if let Some(conn) = self.connection() {
            return conn.heartbeat_config();
        }
        self.pending_heartbeat_config_field.get()
    }

    pub fn set_circuit_breaker(&self, config: &CircuitBreakerConfig) {
        self.pending_circuit_breaker_config_field.set(*config);
        if let Some(conn) = self.connection() {
            conn.set_circuit_breaker_config(config);
        }
    }

    pub fn circuit_breaker_config(&self) -> CircuitBreakerConfig {
        if let Some(conn) = self.connection() {
            return conn.circuit_breaker_config();
        }
        self.pending_circuit_breaker_config_field.get()
    }

    pub fn circuit_breaker_state(&self) -> CircuitState {
        if let Some(conn) = self.connection() {
            return conn.circuit_breaker_state();
        }
        CircuitState::CLOSED
    }

    pub fn is_idle(&self, idle_ms: u64, current_time_ms: u64) -> bool {
        if let Some(conn) = self.connection() {
            return conn.is_idle(idle_ms, current_time_ms);
        }
        false
    }

    fn validate_connection(&self) -> bool {
        if let Some(conn) = self.connection() {
            return conn.validate_connection();
        }
        false
    }

    // Explicit Arc dereference preserves the borrowed counter reference in C++.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it returns the `metrics_field` Arc handle where `const ConnectionMetrics&` is declared (2 emitted lines in srpc.client.cppm).
    #[allow(clippy::explicit_auto_deref)]
    pub fn metrics(&self) -> &ConnectionMetrics { &*self.metrics_field }

    pub fn has_connection(&self) -> bool {
        let guard = self.connection();
        guard.is_some()
    }

    pub fn add_on_connected(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_connected(cb);
    }
    pub fn add_on_disconnected(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_disconnected(cb);
    }
    pub fn add_on_error(&self, cb: OnErrorCallbackFn) {
        self.callback_manager_field.add_on_error(cb);
    }
    pub fn add_on_reconnecting(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_reconnecting(cb);
    }
    pub fn add_on_reconnected(&self, cb: OnReconnectedCallbackFn) {
        self.callback_manager_field.add_on_reconnected(cb);
    }
    pub fn clear_connection_callbacks(&self) {
        self.callback_manager_field.clear_all();
    }
}

pub struct PoolState {
    cache: BTreeMap<String, Vec<Arc<Client>>>,
    lb_state: BTreeMap<String, LoadBalancerState>,
}

impl PoolState {
    fn new() -> PoolState {
        PoolState {
            cache: BTreeMap::<String, Vec<Arc<Client>>>::new(),
            lb_state: BTreeMap::<String, LoadBalancerState>::new(),
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
    state_: std::sync::Mutex<PoolState>,
    config_: std::sync::Mutex<PoolConfig>,
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
            (*self.poll_thread_worker_.as_ref().unwrap()).shutdown();
        }
    }
}

impl ClientPool {
    pub fn new(poll_thread_worker: Option<Arc<PollThread>>, config: PoolConfig) -> ClientPool {
        client_verify(config.min_connections > 0);
        client_verify(config.max_connections >= config.min_connections);
        let mut ptw: Option<Arc<PollThread>> = poll_thread_worker;
        if ptw.is_none() {
            // SAFETY: foreign named-module boundary; no caller precondition.
            ptw = Some(PollThread::create());
        }
        ClientPool {
            poll_thread_worker_: ptw,
            state_: std::sync::Mutex::<PoolState>::new(PoolState::new()),
            config_: std::sync::Mutex::<PoolConfig>::new(config),
        }
    }

    pub fn set_pool_config(&self, config: PoolConfig) {
        let mut guard = self.config_.lock().unwrap();
        (*guard) = config;
    }

    pub fn pool_config(&self) -> PoolConfig {
        let guard = self.config_.lock().unwrap();
        *guard
    }

    fn is_client_healthy(&self, client: &Arc<Client>) -> bool {
        clientpool_is_client_healthy_with(self.pool_config(), client)
    }

    pub fn get_healthy_client_count(&self, addr: &str) -> usize {
        clientpool_get_healthy_client_count(self, addr)
    }

    // clippy::for_kv_map -- measured: emits `.values()` in place of the tuple-destructuring for loop. See the Task-2 measurement block above.
    #[allow(clippy::for_kv_map)]
    pub fn total_client_count(&self) -> usize {
        let guard = self.state_.lock().unwrap();
        let mut count: usize = 0;
        for (_addr, clients) in guard.cache.iter() {
            count += clients.len();
        }
        count
    }

    pub fn address_count(&self) -> usize {
        let guard = self.state_.lock().unwrap();
        guard.cache.len()
    }

    pub fn remove_unhealthy_clients(&self, addr: &str) -> usize {
        clientpool_remove_unhealthy_clients(self, addr)
    }

    pub fn close_idle_clients(&self, addr: &str, current_time_ms: u64) -> usize {
        clientpool_close_idle_clients(self, addr, current_time_ms)
    }

    pub fn remove_all_unhealthy(&self) -> usize {
        clientpool_remove_all_unhealthy(self)
    }

    pub fn close_all_idle(&self, current_time_ms: u64) -> usize {
        clientpool_close_all_idle(self, current_time_ms)
    }

    pub fn get_client(&self, addr: &str) -> Option<Arc<Client>> {
        clientpool_get_client(self, addr)
    }
}

pub fn make_pending_queue(c: &RequestQueueConfig) -> RequestQueue {
    RequestQueue::with_config(*c)
}

pub fn clientconn_monotonic_ms_now() -> u64 { Time::now(true) / 1000 }

// clippy::unnecessary_cast -- measured: drops the emitted rusty::detail::ptr_cast<const int8_t*>. See the Task-2 measurement block above.
#[allow(clippy::unnecessary_cast)]
pub fn clientconn_reconnect(self_: &ClientConnection, mut on_complete: OnReconnectCompleteCallbackFn) -> i32 {
    let mut complete_callback = |result: i32| -> i32 {
        if let Some(callback) = on_complete.as_mut() {
            callback(result == 0);
        }
        result
    };
    // Waiting here can wait for this very call's on_reconnecting/error
    // callback to return. A second caller receives an explicit busy result.
    if self_.reconnect_.reconnecting_.load(std::sync::atomic::Ordering::Acquire) {
        return complete_callback(CLIENT_ERR_BUSY);
    }
    if self_.reconnect_address_.get().is_empty() || !self_.state_machine_.can_connect() {
        return complete_callback(CLIENT_ERR_INVALID_ARGUMENT);
    }
    let start_generation;
    {
        let lifecycle = self_.lifecycle_.lock().unwrap();
        if self_.reconnect_.reconnecting_.compare_exchange(false, true,
            std::sync::atomic::Ordering::AcqRel,
            std::sync::atomic::Ordering::Acquire).is_err() {
            drop(lifecycle);
            return complete_callback(CLIENT_ERR_BUSY);
        }
        start_generation = lifecycle.generation;
        self_.reconnect_.reconnect_abort_.store(false, std::sync::atomic::Ordering::Release);
    }
    let attempt_generation = Cell::new(start_generation);
    let mut finish = |success: bool, result: i32| -> i32 {
        let generation = attempt_generation.get();
        let mut result = result;
        {
            let lifecycle = self_.lifecycle_.lock().unwrap();
            if lifecycle.generation != generation || (success && !lifecycle.active) {
                result = CLIENT_ERR_CANCELED;
            }
            // No later part of this completion writes the latch. A user
            // notification may now start and own a completely new attempt.
            self_.reconnect_.reconnecting_.store(false, std::sync::atomic::Ordering::Release);
            if success && result == 0 {
                self_.metrics_.record_reconnect();
            }
        }
        if success && result == 0 {
            self_.invoke_connected_callback();
            if self_.binding_is_current(generation) {
                clientconn_replay_pending_for_binding(self_, generation);
            }
            if !self_.binding_is_current(generation) {
                result = CLIENT_ERR_CANCELED;
            }
        }
        self_.invoke_reconnected_callback(success && result == 0);
        if success && result == 0 && !self_.binding_is_current(generation) {
            result = CLIENT_ERR_CANCELED;
        }
        complete_callback(result)
    };
    self_.invoke_reconnecting_callback();
    let attempt_is_current = || -> bool {
        let lifecycle = self_.lifecycle_.lock().unwrap();
        lifecycle.generation == attempt_generation.get()
            && !self_.reconnect_.reconnect_abort_.load(std::sync::atomic::Ordering::Acquire)
    };
    let reconnect_once = || -> i32 {
        if !attempt_is_current() {
            return CLIENT_ERR_CANCELED;
        }
        let mut address = self_.reconnect_address_.get().as_bytes().to_vec();
        address.push(0);
        self_.connect_attempt(address.as_ptr() as *const i8, &attempt_generation)
    };
    let mut result = reconnect_once();
    if result == 0 {
        return finish(true, result);
    }
    let policy = self_.reconnect_policy_.get();
    let calc = crate::reconnect_policy::ReconnectCalculator::new(&policy);
    while result != CLIENT_ERR_CANCELED && calc.should_retry() {
        if !attempt_is_current() {
            return finish(false, CLIENT_ERR_CANCELED);
        }
        let delay = calc.next_delay_ms();
        if delay > 0 {
            Time::sleep((delay as u64) * 1000);
        }
        result = reconnect_once();
        if result == 0 {
            return finish(true, result);
        }
    }
    finish(false, result)
}

// clippy::borrowed_box -- the concrete Box spelling is load-bearing: through &T the pointer-like check fails and the calls lower to `.` instead of `->` (docs 7.50); measured. See the Task-2 measurement block above.
// clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber` to the handle `fc`, and turns three `fu.xid_` reads into `std::move((*fu).xid_)` / `&(*fu).xid_` -- a move out of a shared Arc's field (8 emitted lines in srpc.client.cppm).
#[allow(clippy::borrowed_box, clippy::explicit_auto_deref)]
pub fn clientconn_request_via_channel<F>(conn: &ClientConnection, rpc_id: i32,
                                     attr: &FutureAttr, mut write_fn: F) -> FutureResult
where F: FnMut(&mut BinaryWriteArchive) {
    let generation = conn.lifecycle_.lock().unwrap().generation;
    if !conn.allow_request_with_circuit_metrics() {
        return FutureResult::Err(CLIENT_ERR_BUSY);
    }
    conn.pending_queue_.expire_stale();
    if !conn.state_machine_.is_connected() {
        let buffering_cfg = conn.buffering_config_.get();
        if buffering_cfg.enabled && buffering_cfg.behavior == DisconnectBehavior::QUEUE {
            return clientconn_queue_request(conn, rpc_id, attr, write_fn);
        }
        conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
        return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
    }
    {
        let direct = conn.direct_channel();
        if let Some(channel) = direct {
            let proxy: &dyn ChannelConnectionBase = &**channel;
            if proxy.is_closed() {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        } else {
            let channel = conn.fiber_channel();
            let mut chan_dead = channel.is_none();
            if let Some(fc) = channel {
                let fiber: &FiberChannel = &**fc;
                chan_dead = fiber.is_closed();
            }
            if chan_dead {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        }
    }

    let fu = Future::create(conn.xid_counter_.next(1i64), attr.clone());
    // sconn_reply's archive shape: aggregate literals + the &mut alias
    // (bare reference args pass as lvalues where a by-value local would
    // be move-wrapped at its last use).
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::with_capacity(kRequestSinkInitialCapacity) };
    let mut ar_store = BinaryWriteArchive { sink_: client_sink_proxy(&mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(&v64::new((*fu).xid_), ar);
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(&rpc_id, ar);
    write_fn(ar);

    // Publish ownership and its in-flight count together before a channel can
    // reply inline or teardown can drain the pending map. User writing has
    // already finished, so callback reentry cannot retire an uncounted request.
    {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if !lifecycle.active || lifecycle.generation != generation || !conn.connected() {
            return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        let mut pending_guard = conn.pending_fu_.lock().unwrap();
        (*pending_guard).insert((*fu).xid_, fu.clone());
        conn.metrics_.record_request_sent();
        conn.on_request_dispatched(body_sink.bytes.len());
    }
    let ch_err = unsafe {
        clientconn_dispatch_frame_for_binding(conn, generation, body_sink.bytes.as_ptr(), body_sink.bytes.len())
    };
    if ch_err != ChannelError::None {
        {
            let mut pending_guard2 = conn.pending_fu_.lock().unwrap();
            if (*pending_guard2).remove(&(*fu).xid_).is_some() {
                conn.metrics_.record_request_dropped();
            }
        }
        conn.record_circuit_result(CLIENT_ERR_IO);
        return FutureResult::Err(CLIENT_ERR_IO);
    }

    FutureResult::Ok(fu)
}

/// Serialize once while disconnected. The queued bytes and future remain
/// owned until replay, expiry, overflow, or explicit connection teardown.
fn clientconn_queue_request<F>(conn: &ClientConnection, rpc_id: i32,
                              attr: &FutureAttr, mut write_fn: F) -> FutureResult
where F: FnMut(&mut BinaryWriteArchive) {
    let generation = conn.lifecycle_.lock().unwrap().generation;
    let future = Future::create(conn.xid_counter_.next(1i64), attr.clone());
    let xid = future.xid_;
    let mut body = BufferSink { bytes: Vec::<u8>::with_capacity(kRequestSinkInitialCapacity) };
    {
        let mut archive = BinaryWriteArchive { sink_: client_sink_proxy(&mut body) };
        crate::serializable::Serialize_::serialize(&v64::new(xid), &mut archive);
        crate::serializable::Serialize_::serialize(&rpc_id, &mut archive);
        write_fn(&mut archive);
    }
    let mut request = QueuedRequest::new();
    request.xid = xid;
    request.rpc_id = rpc_id;
    request.ttl_ms = conn.buffering_config_.get().default_ttl_ms;
    request.payload = body.bytes;
    let queued: Arc<Mutex<HashMap<i64, Arc<Future>>>> = conn.queued_fu_.clone();
    let weak_connection = conn.weak_self_.clone();
    let on_queue_error: QueuedRequestCallback = Some(Box::new(move |error: i32| {
        let completed: Option<Arc<Future>> = queued.lock().unwrap().remove(&xid);
        if let Some(future) = completed {
            if let Some(connection) = weak_connection.upgrade() {
                let owner: Arc<ClientConnection> = connection;
                (*owner).metrics().record_queue_drop();
            }
            future.error_code_.set(error);
            future.notify_ready(future.clone());
        }
    }));
    request.callback = on_queue_error;
    let admission = {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if lifecycle.generation != generation {
            return FutureResult::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        conn.queued_fu_.lock().unwrap().insert(xid, future.clone());
        conn.pending_queue_.enqueue_deferred(request)
    };
    if !admission.notify() {
        return FutureResult::Err(CLIENT_REQUEST_QUEUE_REJECTED_ERROR);
    }
    // Reconnect may finish while the caller serializes its request.
    if conn.connected() {
        conn.replay_pending_requests();
    }
    FutureResult::Ok(future)
}

fn clientconn_replay_pending_requests(conn: &ClientConnection) -> usize {
    let generation = conn.lifecycle_.lock().unwrap().generation;
    clientconn_replay_pending_for_binding(conn, generation)
}

fn clientconn_replay_pending_for_binding(conn: &ClientConnection, expected_generation: u64) -> usize {
    if conn.replaying_.compare_exchange(false, true,
        std::sync::atomic::Ordering::AcqRel,
        std::sync::atomic::Ordering::Acquire).is_err() {
        return 0;
    }
    let scope = ClientReplayScope { running: conn.replaying_.clone() };
    let mut replayed = 0usize;
    loop {
        let request = {
            let lifecycle = conn.lifecycle_.lock().unwrap();
            if !lifecycle.active || lifecycle.generation != expected_generation {
                break;
            }
            conn.pending_queue_.dequeue()
        };
        if request.is_none() {
            break;
        }
        let request = request.unwrap();
        if request.is_expired() {
            rq_invoke_callback_safely(request.callback, kRequestQueueExpiredError);
            continue;
        }
        if !conn.connected() {
            let admission = {
                let _lifecycle = conn.lifecycle_.lock().unwrap();
                if conn.queued_fu_.lock().unwrap().contains_key(&request.xid) {
                    Some(conn.pending_queue_.enqueue_deferred(request))
                } else {
                    None
                }
            };
            if let Some(admission) = admission {
                admission.notify();
            }
            break;
        }
        // Transfer ownership while holding both map locks. A concurrent close
        // always finds this future in one map, including after queue removal.
        let admitted: bool;
        let generation;
        {
            let lifecycle = conn.lifecycle_.lock().unwrap();
            generation = lifecycle.generation;
            let mut queued = conn.queued_fu_.lock().unwrap();
            let future = if lifecycle.active && lifecycle.generation == expected_generation && conn.connected() {
                queued.remove(&request.xid)
            } else {
                None
            };
            admitted = future.is_some();
            if let Some(future) = future {
                let mut pending = conn.pending_fu_.lock().unwrap();
                pending.insert(request.xid, future);
                conn.metrics_.record_request_sent();
                conn.on_request_dispatched(request.payload.len());
            }
        }
        if !admitted {
            continue;
        }
        // In-memory responses may complete synchronously inside send_frame.
        let error = unsafe {
            clientconn_dispatch_frame_for_binding(conn, generation, request.payload.as_ptr(), request.payload.len())
        };
        if error == ChannelError::None {
            replayed += 1;
        } else {
            conn.record_circuit_result(CLIENT_ERR_IO);
            conn.fail_pending_future(request.xid, CLIENT_ERR_IO);
        }
    }
    drop(scope);
    // A callback may reconnect while this replay still owns the running flag.
    // Hand off only after releasing it, then admit work using the current
    // binding's token. The old loop never sends through a replacement slot.
    let current_generation = {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if lifecycle.active { Some(lifecycle.generation) } else { None }
    };
    if let Some(current) = current_generation {
        if !conn.pending_queue_.empty() {
            replayed += clientconn_replay_pending_for_binding(conn, current);
        }
    }
    replayed
}

// clippy::borrowed_box -- the concrete Box spelling is load-bearing: through &T the pointer-like check fails and the calls lower to `.` instead of `->` (docs 7.50); measured. See the Task-2 measurement block above.
// clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber` to the handle `fc` instead of the channel (2 emitted lines in srpc.client.cppm).
#[allow(clippy::borrowed_box, clippy::explicit_auto_deref)]
pub fn clientconn_request_async<F>(conn: &ClientConnection, rpc_id: i32,
                               mut write_fn: F, on_reply: AsyncReplyCallback)
                               -> Result<(), i32>
where F: FnMut(&mut BinaryWriteArchive) {
    let generation = conn.lifecycle_.lock().unwrap().generation;
    if !conn.allow_request_with_circuit_metrics() {
        return Result::<(), i32>::Err(CLIENT_ERR_BUSY);
    }
    if !conn.state_machine_.is_connected() {
        conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
        return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
    }
    {
        let direct = conn.direct_channel();
        if let Some(channel) = direct {
            let proxy: &dyn ChannelConnectionBase = &**channel;
            if proxy.is_closed() {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        } else {
            let channel = conn.fiber_channel();
            let mut chan_dead = channel.is_none();
            if let Some(fc) = channel {
                let fiber: &FiberChannel = &**fc;
                chan_dead = fiber.is_closed();
            }
            if chan_dead {
                conn.record_circuit_result(CLIENT_ERR_NOT_CONNECTED);
                return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
            }
        }
    }

    let xid: i64 = conn.xid_counter_.next(1i64);
    let slot: usize = (xid as usize) % kAsyncSlotCount;

    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::with_capacity(kRequestSinkInitialCapacity) };
    let mut ar_store = BinaryWriteArchive { sink_: client_sink_proxy(&mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(&v64::new(xid), ar);
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(&rpc_id, ar);
    write_fn(ar);

    {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if !lifecycle.active || lifecycle.generation != generation || !conn.connected() {
            return Result::<(), i32>::Err(CLIENT_ERR_NOT_CONNECTED);
        }
        let mut guard = conn.pending_cb_slots_.lock().unwrap();
        if (*guard)[slot].is_some() {
            conn.record_circuit_result(CLIENT_ERR_BUSY);
            return Result::<(), i32>::Err(CLIENT_ERR_BUSY);
        }
        (*guard)[slot] = Some(on_reply);
        conn.metrics_.record_request_sent();
        conn.on_request_dispatched(body_sink.bytes.len());
    }
    let ch_err = unsafe {
        clientconn_dispatch_frame_for_binding(conn, generation, body_sink.bytes.as_ptr(), body_sink.bytes.len())
    };
    if ch_err != ChannelError::None {
        let rejected: Option<AsyncReplyCallback> = {
            let lifecycle = conn.lifecycle_.lock().unwrap();
            if lifecycle.generation == generation {
                conn.pending_cb_slots_.lock().unwrap()[slot].take()
            } else {
                None
            }
        };
        if rejected.is_some() {
            conn.metrics_.record_request_dropped();
        }
        // Captured destructors can reenter the client, just like invocations.
        // Keep callback destruction outside the callback-table mutex.
        drop(rejected);
        conn.record_circuit_result(CLIENT_ERR_IO);
        return Result::<(), i32>::Err(CLIENT_ERR_IO);
    }
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

// Copies the attempt's unread reply region into the coordinator
// future's buffer. Both distinct reply buffers are locked around a sub-slice
// body — spelled exactly as clientconn_decode_response_and_notify below
// already spells the same fill: `ptr::add` + `core::slice::from_raw_parts`
// inside `unsafe`, which is what retired the "span has no DSL form"
// excuse. Takes REFERENCES, not pointers: `&Arc<Future>` lowers to
// `const Arc<Future>&`, and the caller's `&attempt_fu` collapses to
// the handle itself.
// clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds the reply lock guards by value (`auto` in place of `const auto&&` through deref_call) and reads their fields through a raw `(*x)` (10 emitted lines in srpc.client.cppm).
#[allow(clippy::explicit_auto_deref)]
pub fn request_copy_reply(final_fu: &Arc<Future>, attempt_fu: &Arc<Future>) {
    let attempt_reply = (*attempt_fu).reply_.lock().unwrap();
    let reply_size: usize = (*attempt_reply).src.remaining();
    if reply_size > 0usize {
        let base: *const u8 = (*attempt_reply).body.as_ptr();
        let start: usize = (*attempt_reply).src.pos();
        let mut final_reply = (*final_fu).reply_.lock().unwrap();
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

// clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds the state and reply lock guards by value (`auto` in place of `const auto&&`) and turns every unwrap into a raw `(*x)` (22 emitted lines in srpc.client.cppm).
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
    let mut args_sink = BufferSink { bytes: Vec::<u8>::with_capacity(kRequestSinkInitialCapacity) };
    let mut ar: BinaryWriteArchive = make_write_archive(&raw mut args_sink);
    let ar_ref: &mut BinaryWriteArchive = &mut ar;
    write_fn(ar_ref);
    // Keep the replay payload as bytes (was a reinterpret_cast'd
    // std::string round-trip).
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
    drop(crate::threading::spawn_abort_on_panic(move || {
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
                    (*conn).metrics().record_request_timeout();
                } else if err != 0i32 {
                    (*conn).metrics().record_request_failed();
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

            (*conn).metrics().record_retry_attempt();
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
    }));

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
    let generation = conn.lifecycle_.lock().unwrap().generation;
    unsafe { clientconn_dispatch_frame_for_binding(conn, generation, body_bytes, body_size) }
}

// Arc<Box<FiberChannel>> needs both payload dereferences in the generated C++ call.
// clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber_ref` to the handle `channel` instead of the channel (2 emitted lines in srpc.client.cppm).
#[allow(clippy::explicit_auto_deref)]
unsafe fn clientconn_dispatch_frame_for_binding(conn: &ClientConnection, generation: u64,
                                              body_bytes: *const u8, body_size: usize) -> ChannelError {
    let direct;
    let fiber;
    {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if !lifecycle.active || lifecycle.generation != generation {
            return ChannelError::ConnectionReset;
        }
        direct = conn.direct_channel();
        fiber = conn.fiber_channel();
    }
    if let Some(channel) = direct {
        let proxy: &dyn ChannelConnectionBase = &**channel;
        return unsafe { proxy.send_frame(&ChannelFrame { payload: body_bytes, size: body_size }) };
    }
    if let Some(channel) = fiber {
        let fiber_ref: &FiberChannel = &**channel;
        return unsafe { fiber_ref.send_frame(&ChannelFrame { payload: body_bytes, size: body_size }) };
    }
    ChannelError::ConnectionReset
}

pub fn clientconn_enqueue_heartbeat_probe(conn: &ClientConnection) {
    // Build the heartbeat frame body and dispatch through the channel
    // proxy. Same archive shape as the server's sconn_reply: aggregate
    // struct literals + the &mut alias so serialize's Archive& binds.
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::with_capacity(kRequestSinkInitialCapacity) };
    let mut ar_store = BinaryWriteArchive { sink_: client_sink_proxy(&mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(
        &v64::new(conn.xid_counter_.next(1i64)),
        ar,
    );
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
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
pub fn clientconn_addr_to_string(addr: *const i8) -> String {
    if addr.is_null() {
        return String::new();
    }
    // Copy up to the NUL -- the same shape `base/logging.rs`'s `log_basename`
    // uses. Valid UTF-8 is preserved byte for byte, as the historical
    // `std::string(addr)` did; an invalid sequence becomes U+FFFD.
    //
    // `CStr::from_ptr(..).to_string_lossy().into_owned()` is not spellable
    // here: the checked map spells `CStr` `std::string`, so the associated
    // function emits the non-existent `std::string::from_ptr`, and every
    // `CStr` method behind it has the same problem.
    let mut bytes: Vec<u8> = Vec::new();
    let mut index: usize = 0;
    // SAFETY: all callers uphold the historical C-string input contract;
    // `index` is advanced only until the first NUL byte.
    while unsafe { *addr.add(index) } != 0i8 {
        // SAFETY: as above -- `index` is still before the terminator.
        bytes.push(unsafe { *addr.add(index) } as u8);
        index += 1;
    }
    String::from(String::from_utf8_lossy(bytes.as_slice()))
}

pub fn clientconn_connect_via_factory(conn: &ClientConnection, addr_i8: *const i8) -> i32 {
    let generation = conn.lifecycle_.lock().unwrap().generation;
    let result = clientconn_connect_factory_for_binding(conn, addr_i8, generation);
    if result != 0 {
        return result;
    }
    conn.invoke_connected_callback();
    if conn.binding_is_current(generation) { 0 } else { CLIENT_ERR_CANCELED }
}

fn clientconn_connect_factory_for_binding(conn: &ClientConnection, addr_i8: *const i8,
                                         generation: u64) -> i32 {
    let addr_str: String = clientconn_addr_to_string(addr_i8);
    let factory = conn.factory_.lock().unwrap().clone();
    if factory.is_none() {
        conn.invoke_error_callback(CLIENT_ERR_NOT_CONNECTED, &client_text("factory unbound"));
        return CLIENT_ERR_NOT_CONNECTED;
    }
    let factory = factory.unwrap();
    // Only the factory's own mutable-callable lock is held across connect.
    // The client slot and lifecycle locks are released before the callout.
    let mut result: ConnectResult = {
        let mut factory_guard = factory.lock().unwrap();
        let bound: &mut Box<dyn ChannelFactoryBase> = &mut factory_guard;
        bound.connect(&addr_str)
    };
    if result.error != ChannelError::None || result.connection.is_none() {
        let err_str = client_text_str("factory connect failed: ", channel_error_to_string(result.error), "");
        let mut rc = CLIENT_ERR_NOT_CONNECTED;
        if result.error == ChannelError::ConnectionRefused {
            rc = CLIENT_ERR_CONNECTION_REFUSED;
        } else if result.error == ChannelError::AddressInvalid {
            rc = CLIENT_ERR_INVALID_ARGUMENT;
        }
        {
            let lifecycle = conn.lifecycle_.lock().unwrap();
            if lifecycle.generation == generation {
                conn.state_machine_.state_field.set(ConnectionState::FAILED);
            }
        }
        conn.invoke_error_callback(rc, &err_str);
        return rc;
    }
    let conn_proxy = result.connection.take().unwrap();
    if !conn.bind_channel_direct(Some(conn_proxy), generation) {
        return CLIENT_ERR_CANCELED;
    }

    {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if lifecycle.generation != generation || !lifecycle.active {
            return CLIENT_ERR_CANCELED;
        }
        conn.reconnect_address_.set(addr_str);
        let now = clientconn_monotonic_ms_now();
        conn.metrics_.record_connect(now);
        conn.update_last_activity(now);
    }
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
pub fn clientconn_recv_job_entry(weak_self: WeakClientConnection, channel: Arc<Box<FiberChannel>>) {
    let conn_opt = weak_self.upgrade();
    if conn_opt.is_some() {
        let c = conn_opt.unwrap();
        clientconn_run_recv_loop_on_channel(&c, channel);
    }
}

// clippy::arc_with_non_send_sync -- no fix short of changing the payload type; the C++ Arc erases Rust auto traits. See the Task-2 measurement block above.
#[allow(clippy::arc_with_non_send_sync)]
pub fn clientconn_bind_channel_via_poll_thread(conn: &ClientConnection,
                                           channel: NullableChannelConnectionProxy) {
    if channel.is_none() {
        return;
    }
    let channel = channel.unwrap();
    let channel = conn.replace_fiber_channel(channel);
    conn.channel_mode_.set(true);

    let weak_self: WeakClientConnection = conn.weak_self_.clone();

    // Schedule the recv-loop fiber spawn onto the poll thread. The
    // poll thread's `trigger_job` calls `Fiber::create_run` from its
    // own reactor, so the resulting fiber's IntEvent waits and the
    // `on_frame` callback's signal both land on the same thread.
    let job_fn = move || {
        clientconn_recv_job_entry(weak_self.clone(), channel.clone());
    };
    let recv_job: Arc<OneTimeJob> =
        Arc::<OneTimeJob>::new(OneTimeJob::new(Box::new(job_fn)));
    // Erase the job type for the worker command queue.
    let recv_job_erased: Arc<dyn crate::misc::Job> = recv_job;
    let pt: &Arc<PollThread> = &conn.poll_thread_worker_;
    pt.add(recv_job_erased);
}

pub fn clientconn_run_recv_loop(conn: &ClientConnection) {
    if let Some(channel) = conn.fiber_channel() {
        clientconn_run_recv_loop_on_channel(conn, channel);
    }
}

// clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it binds `const FiberChannel& fiber` to the handle `channel` instead of the channel (2 emitted lines in srpc.client.cppm).
#[allow(clippy::explicit_auto_deref)]
pub fn clientconn_run_recv_loop_on_channel(conn: &ClientConnection, channel: Arc<Box<FiberChannel>>) {
    loop {
        let fiber: &FiberChannel = &**channel;
        let frame_opt: Option<OwnedFrame> = fiber.recv_frame();
        let generation = {
            let lifecycle = conn.lifecycle_.lock().unwrap();
            let current = conn.fiber_channel();
            if !lifecycle.active || current.is_none() {
                return;
            }
            if Arc::as_ptr(current.as_ref().unwrap()) != Arc::as_ptr(&channel) {
                return;
            }
            lifecycle.generation
        };
        if frame_opt.is_none() {
            conn.on_binding_closed(generation);
            return;
        }
        let frame = frame_opt.unwrap();
        clientconn_decode_response_for_binding(conn, generation, frame.bytes.as_ptr(), frame.bytes.len());
    }
}

// clippy::unnecessary_unwrap -- measured: emits an extra `decltype(auto)` binding and re-shapes the branch. See the Task-2 measurement block above.
// clippy::not_unsafe_ptr_arg_deref -- this became public with the module's
// surface; the raw-pointer contract is the historical C++ one and is
// documented at the deref itself. Marking the fn `unsafe` instead would
// wrap every call site in an `unsafe` block, which the emitter renders as an
// @unsafe comment block -- measured: changes emitted C++.
#[allow(clippy::unnecessary_unwrap, clippy::not_unsafe_ptr_arg_deref)]
pub fn clientconn_decode_response_and_notify(conn: &ClientConnection,
                                         bytes: *const u8, size: usize) {
    let generation = conn.lifecycle_.lock().unwrap().generation;
    clientconn_decode_response_for_binding(conn, generation, bytes, size);
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
fn clientconn_decode_response_for_binding(conn: &ClientConnection, generation: u64,
                                        bytes: *const u8, size: usize) {
    let mut src = BufferSource::new(bytes, size);
    let mut ar = BinaryReadArchive::new(client_source_proxy(&mut src));
    let mut xid = v64::new(0);
    let mut error = v32::new(0);
    let mut server_id = v64::new(0);
    crate::serializable::Deserialize_::deserialize(&mut xid, &mut ar);
    crate::serializable::Deserialize_::deserialize(&mut error, &mut ar);
    crate::serializable::Deserialize_::deserialize(&mut server_id, &mut ar);
    let header_size = src.pos();
    let payload_size = size - header_size;
    let callback: Option<AsyncReplyCallback>;
    let mut future: Option<Arc<Future>> = None;
    let mut restart: Option<Arc<Mutex<OnServerRestartCallbackFn>>> = None;
    let old_server_id;
    let new_server_id = server_id.get() as u64;
    {
        let lifecycle = conn.lifecycle_.lock().unwrap();
        if !lifecycle.active || lifecycle.generation != generation {
            return;
        }
        conn.on_response_received(size);
        old_server_id = conn.server_instance_id_.get();
        conn.server_instance_id_.set(new_server_id);
        if old_server_id != 0 && old_server_id != new_server_id {
            restart = Some(conn.on_server_restart_.lock().unwrap().clone());
        }
        conn.heartbeat_manager_.on_pong_received();
        let slot = (xid.get() as usize) % kAsyncSlotCount;
        callback = conn.pending_cb_slots_.lock().unwrap()[slot].take();
        if callback.is_none() {
            future = conn.pending_fu_.lock().unwrap().remove(&xid.get());
        }
        if let Some(future) = &future {
            client_verify(future.xid_ == xid.get());
            future.error_code_.set(error.get());
            if payload_size > 0 {
                let mut reply = future.reply_.lock().unwrap();
                reply_buffer_fill(&mut reply, unsafe {
                    core::slice::from_raw_parts(bytes.add(header_size), payload_size)
                });
            }
        }
        if callback.is_some() || future.is_some() {
            if error.get() == 0 {
                conn.metrics_.record_request_completed();
            } else {
                conn.metrics_.record_request_failed();
            }
            conn.record_circuit_result(error.get());
        }
    }
    // All mutable connection effects and completion ownership were selected
    // under lifecycle. A restart callback may now reconnect without an old
    // frame removing that replacement's future or updating its heartbeat.
    if let Some(restart) = restart {
        let mut handler = restart.lock().unwrap();
        if handler.is_some() {
            handler.as_mut().unwrap()(old_server_id, new_server_id);
        }
    }
    if let Some(mut callback) = callback {
        // Keep the typed take: C++ lowering otherwise copies the if-let binding.
        let mut on_reply: AsyncReplyCallback = std::mem::take(&mut callback);
        on_reply.as_mut().unwrap()(error.get(), unsafe { bytes.add(header_size) }, payload_size);
    }
    if let Some(future) = future {
        future.notify_ready(future.clone());
    }
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
pub fn clientpool_get_healthy_client_count(self_: &ClientPool, addr: &str) -> usize {
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
pub fn clientpool_remove_unhealthy_clients(self_: &ClientPool, addr: &str) -> usize {
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
pub fn clientpool_close_idle_clients(self_: &ClientPool, addr: &str, current_time_ms: u64) -> usize {
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

    let mut keys: Vec<String> = Vec::<String>::new();
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
    let mut empty_keys: Vec<String> = Vec::<String>::new();
    let mut k: usize = 0usize;
    while k < keys.len() {
        let addr: &String = &keys[k];
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
        let key: &String = &empty_keys[j];
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

    let mut keys: Vec<String> = Vec::<String>::new();
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
    let mut empty_keys: Vec<String> = Vec::<String>::new();
    let mut k: usize = 0usize;
    while k < keys.len() {
        let addr: &String = &keys[k];
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
        let key: &String = &empty_keys[j];
        guard.cache.remove(key);
        j += 1usize;
    }
    total_closed
}

// The owned NUL terminator keeps the C address valid for the synchronous
// connect call in Rust and generated C++ alike.
pub fn clientpool_connect_client(client: &Arc<Client>, addr: &str) -> i32 {
    let mut address_bytes = addr.as_bytes().to_vec();
    address_bytes.push(0u8);
    client.connect(address_bytes.as_ptr() as *const i8, true)
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
pub fn clientpool_get_client(self_: &ClientPool, addr: &str) -> Option<Arc<Client>> {
    let mut sp_cl: Option<Arc<Client>> = None;
    let cfg: PoolConfig = self_.pool_config();
    let num_connections: i32 = cfg.min_connections;

    let mut guard = self_.state_.lock().unwrap();

    // Get or create load balancer state for this address. select() takes
    // &LoadBalancerState (round-robin advances through a Cell), so the
    // shared get() probe is enough.
    let has_lb: bool = guard.lb_state.get(addr).is_some();
    if !has_lb {
        guard.lb_state.insert(addr.to_string(), LoadBalancerState::new());
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
            guard.cache.insert(addr.to_string(), parallel_clients);
        }
        // If not ok, parallel_clients cleans up via the Arc drops
    }
    sp_cl
}
