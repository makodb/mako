//! Deterministic in-process implementation of the legacy channel facade.
//!
//! This is the valid-Rust owner of `srpc.inmemory_channel`.  The two channel
//! halves share one mutex-protected state object.  Frame, close, and accept
//! callbacks are snapshotted while locked and invoked after releasing the
//! guard, preserving the legacy synchronous ordering while allowing callbacks
//! to re-enter either half without deadlocking.

#![allow(unsafe_code)]
#![allow(clippy::arc_with_non_send_sync, clippy::question_mark)]

use rusty::cpp_inherit;
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use crate::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFactoryBase,
    ChannelFactoryProxy, ChannelFrame, ChannelListenerBase, ChannelListenerProxy, ConnectResult,
    OnAcceptCallback, OnClosedCallback, OnErrorCallback, OnFrameCallback,
};

// The consumer profile maps this private carrier to `std::string`, retaining
// the established C++ surface instead of exposing rusty-cpp's distinct
// `rusty::String` owner.
type LegacyStdString = String;

// Cross-module unit variants are currently rendered as constructor calls when
// they appear in unconstrained expression positions. ChannelError has a fixed
// i32 representation, so these private constructors use the same bit-preserving
// conversion in Rust and generated C++ without changing the exported surface.
fn channel_error_from_code(code: i32) -> ChannelError {
    // SAFETY: every caller below passes a declared ChannelError discriminant.
    unsafe { core::mem::transmute(code) }
}

fn channel_error_none() -> ChannelError {
    channel_error_from_code(0_i32)
}

fn channel_error_connection_reset() -> ChannelError {
    channel_error_from_code(3_i32)
}

fn channel_error_address_in_use() -> ChannelError {
    channel_error_from_code(5_i32)
}

fn channel_error_internal() -> ChannelError {
    channel_error_from_code(9_i32)
}

/// Registry mapping an in-memory bind address to its live listener.
#[repr(C)]
pub struct InMemorySwitchboard {
    pub listeners_: Mutex<HashMap<LegacyStdString, std::sync::Weak<InMemoryListener>>>,
}

impl InMemorySwitchboard {
    pub fn new() -> InMemorySwitchboard {
        InMemorySwitchboard {
            listeners_: Mutex::new(HashMap::new()),
        }
    }

    pub fn register_listener(
        &self,
        address: LegacyStdString,
        listener: std::sync::Weak<InMemoryListener>,
    ) -> bool {
        let mut guard = self.listeners_.lock().unwrap();
        if guard.contains_key(&address) {
            return false;
        }
        guard.insert(address, listener);
        true
    }

    pub fn unregister_listener(&self, address: &LegacyStdString) {
        let mut guard = self.listeners_.lock().unwrap();
        guard.remove(address);
    }

    pub fn find_listener(&self, address: &LegacyStdString) -> Option<Arc<InMemoryListener>> {
        let mut guard = self.listeners_.lock().unwrap();
        let upgraded: Option<Arc<InMemoryListener>> = match guard.get(address) {
            Some(listener) => listener.upgrade(),
            None => return None,
        };
        if upgraded.is_none() {
            guard.remove(address);
        }
        upgraded
    }
}

/// Mutable state shared by both halves of a connected pair.
#[repr(C)]
pub struct InMemoryConnectionStateInner {
    pub a_peer_address: LegacyStdString,
    pub a_on_frame: OnFrameCallback,
    pub a_on_closed: OnClosedCallback,
    pub a_on_error: OnErrorCallback,
    pub a_closed: bool,
    pub b_peer_address: LegacyStdString,
    pub b_on_frame: OnFrameCallback,
    pub b_on_closed: OnClosedCallback,
    pub b_on_error: OnErrorCallback,
    pub b_closed: bool,
    pub drop_next_sends_a: i32,
    pub drop_next_sends_b: i32,
    pub send_error_count_a: i32,
    pub send_error_count_b: i32,
    pub send_error_a: ChannelError,
    pub send_error_b: ChannelError,
}

fn empty_connection_inner() -> InMemoryConnectionStateInner {
    InMemoryConnectionStateInner {
        a_peer_address: "".to_string(),
        a_on_frame: OnFrameCallback::default(),
        a_on_closed: OnClosedCallback::default(),
        a_on_error: OnErrorCallback::default(),
        a_closed: false,
        b_peer_address: "".to_string(),
        b_on_frame: OnFrameCallback::default(),
        b_on_closed: OnClosedCallback::default(),
        b_on_error: OnErrorCallback::default(),
        b_closed: false,
        drop_next_sends_a: 0_i32,
        drop_next_sends_b: 0_i32,
        send_error_count_a: 0_i32,
        send_error_count_b: 0_i32,
        send_error_a: channel_error_none(),
        send_error_b: channel_error_none(),
    }
}

#[repr(C)]
pub struct InMemoryConnectionState {
    pub inner: Mutex<InMemoryConnectionStateInner>,
}

fn make_connection_state(
    a_address: LegacyStdString,
    b_address: LegacyStdString,
) -> Arc<InMemoryConnectionState> {
    let mut inner: InMemoryConnectionStateInner = empty_connection_inner();
    inner.a_peer_address = a_address;
    inner.b_peer_address = b_address;
    Arc::new(InMemoryConnectionState {
        inner: Mutex::new(inner),
    })
}

/// One side of a paired in-memory connection.
#[repr(C)]
pub struct InMemoryChannel {
    pub state_: Arc<InMemoryConnectionState>,
    pub is_a_side_: bool,
}

impl InMemoryChannel {
    pub fn new(state: Arc<InMemoryConnectionState>, is_a_side: bool) -> InMemoryChannel {
        InMemoryChannel {
            state_: state,
            is_a_side_: is_a_side,
        }
    }

    /// # Safety
    ///
    /// `frame` must satisfy the raw payload validity contract on
    /// `ChannelConnectionBase::send_frame` for this synchronous call.
    pub unsafe fn send_frame(&self, frame: &ChannelFrame) -> ChannelError {
        unsafe { inmemory_channel_send_frame(self, frame) }
    }

    pub fn flush(&self) {}

    /// Close this half once and synchronously notify only the peer.
    pub fn close(&self) {
        let mut peer_on_closed: OnClosedCallback = OnClosedCallback::default();
        let mut fire_peer_closed: bool = false;
        {
            let mut guard = self.state_.inner.lock().unwrap();
            if self.is_a_side_ {
                if guard.a_closed {
                    return;
                }
                guard.a_closed = true;
                if !guard.b_closed {
                    peer_on_closed = guard.b_on_closed.clone();
                    fire_peer_closed = true;
                }
            } else {
                if guard.b_closed {
                    return;
                }
                guard.b_closed = true;
                if !guard.a_closed {
                    peer_on_closed = guard.a_on_closed.clone();
                    fire_peer_closed = true;
                }
            }
        }
        if fire_peer_closed && peer_on_closed.has_value() {
            (peer_on_closed.callable())(ChannelError::None);
        }
    }

    pub fn is_closed(&self) -> bool {
        let guard = self.state_.inner.lock().unwrap();
        guard.a_closed || guard.b_closed
    }

    pub fn peer_address(&self) -> LegacyStdString {
        let guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ {
            guard.b_peer_address.clone()
        } else {
            guard.a_peer_address.clone()
        }
    }

    pub fn set_on_frame(&self, callback: OnFrameCallback) {
        let mut guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ {
            guard.a_on_frame = callback;
        } else {
            guard.b_on_frame = callback;
        }
    }

    pub fn set_on_closed(&self, callback: OnClosedCallback) {
        let mut guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ {
            guard.a_on_closed = callback;
        } else {
            guard.b_on_closed = callback;
        }
    }

    pub fn set_on_error(&self, callback: OnErrorCallback) {
        let mut guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ {
            guard.a_on_error = callback;
        } else {
            guard.b_on_error = callback;
        }
    }
}

/// Deliver a copied frame synchronously after dropping the state lock.
///
/// # Safety
///
/// `frame` must satisfy the raw payload validity contract on
/// `ChannelConnectionBase::send_frame` for this synchronous call.
pub unsafe fn inmemory_channel_send_frame(
    channel: &InMemoryChannel,
    frame: &ChannelFrame,
) -> ChannelError {
    let peer_on_frame: OnFrameCallback;
    let self_already_closed: bool;
    let peer_already_closed: bool;
    let mut drop_this_send: bool = false;
    let mut inject_error: bool = false;
    let mut injected_error: ChannelError = channel_error_none();
    {
        let mut guard = channel.state_.inner.lock().unwrap();
        if channel.is_a_side_ {
            self_already_closed = guard.a_closed;
            peer_already_closed = guard.b_closed;
            peer_on_frame = guard.b_on_frame.clone();
            if guard.drop_next_sends_a > 0_i32 {
                drop_this_send = true;
                guard.drop_next_sends_a -= 1_i32;
            } else if guard.send_error_count_a > 0_i32 {
                inject_error = true;
                injected_error = guard.send_error_a;
                guard.send_error_count_a -= 1_i32;
            }
        } else {
            self_already_closed = guard.b_closed;
            peer_already_closed = guard.a_closed;
            peer_on_frame = guard.a_on_frame.clone();
            if guard.drop_next_sends_b > 0_i32 {
                drop_this_send = true;
                guard.drop_next_sends_b -= 1_i32;
            } else if guard.send_error_count_b > 0_i32 {
                inject_error = true;
                injected_error = guard.send_error_b;
                guard.send_error_count_b -= 1_i32;
            }
        }
    }

    if self_already_closed || peer_already_closed {
        return channel_error_connection_reset();
    }
    if drop_this_send {
        return channel_error_none();
    }
    if inject_error {
        return injected_error;
    }

    let mut bytes: Vec<u8> = Vec::new();
    if frame.size > 0_usize && !frame.payload.is_null() {
        let payload: &[u8] = unsafe { core::slice::from_raw_parts(frame.payload, frame.size) };
        bytes.extend_from_slice(payload);
    }
    let delivered: ChannelFrame = ChannelFrame {
        payload: bytes.as_ptr(),
        size: bytes.len(),
    };
    if peer_on_frame.has_value() {
        (peer_on_frame.callable())(&delivered);
    }
    channel_error_none()
}

pub fn inmemory_channel_inject_drop_next_sends(channel: &InMemoryChannel, count: i32) {
    let mut guard = channel.state_.inner.lock().unwrap();
    if channel.is_a_side_ {
        guard.drop_next_sends_a = count;
    } else {
        guard.drop_next_sends_b = count;
    }
}

pub fn inmemory_channel_inject_send_error(
    channel: &InMemoryChannel,
    error: ChannelError,
    count: i32,
) {
    let mut guard = channel.state_.inner.lock().unwrap();
    if channel.is_a_side_ {
        guard.send_error_a = error;
        guard.send_error_count_a = count;
    } else {
        guard.send_error_b = error;
        guard.send_error_count_b = count;
    }
}

pub fn inmemory_channel_clear_fault_injection(channel: &InMemoryChannel) {
    let mut guard = channel.state_.inner.lock().unwrap();
    if channel.is_a_side_ {
        guard.drop_next_sends_a = 0_i32;
        guard.send_error_count_a = 0_i32;
        guard.send_error_a = channel_error_none();
    } else {
        guard.drop_next_sends_b = 0_i32;
        guard.send_error_count_b = 0_i32;
        guard.send_error_b = channel_error_none();
    }
}

/// Arc-retaining adapter for the channel facade's owning proxy.
#[repr(C)]
pub struct InMemoryChannelShim {
    pub conn_: Arc<InMemoryChannel>,
}

#[cpp_inherit]
impl ChannelConnectionBase for InMemoryChannelShim {
    unsafe fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        unsafe { self.conn_.send_frame(frame) }
    }

    fn flush(&mut self) {
        self.conn_.flush();
    }

    fn close(&mut self) {
        self.conn_.close();
    }

    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }

    fn peer_address(&self) -> LegacyStdString {
        self.conn_.peer_address()
    }

    fn set_on_frame(&mut self, callback: OnFrameCallback) {
        self.conn_.set_on_frame(callback);
    }

    fn set_on_closed(&mut self, callback: OnClosedCallback) {
        self.conn_.set_on_closed(callback);
    }

    fn set_on_error(&mut self, callback: OnErrorCallback) {
        self.conn_.set_on_error(callback);
    }
}

pub fn make_inmemory_channel_proxy(connection: Arc<InMemoryChannel>) -> ChannelConnectionProxy {
    Box::new(InMemoryChannelShim { conn_: connection })
}

/// Mutex-owned mutable listener state.
#[repr(C)]
pub struct InMemoryListenerInnerState {
    pub local_address: LegacyStdString,
    pub closed: bool,
    pub on_accept: OnAcceptCallback,
    pub on_error: OnErrorCallback,
}

fn empty_listener_inner() -> InMemoryListenerInnerState {
    InMemoryListenerInnerState {
        local_address: "".to_string(),
        closed: false,
        on_accept: OnAcceptCallback::default(),
        on_error: OnErrorCallback::default(),
    }
}

/// Accept-side endpoint registered in an [`InMemorySwitchboard`].
#[repr(C)]
pub struct InMemoryListener {
    pub switchboard_: Arc<InMemorySwitchboard>,
    pub self_weak_: Option<std::sync::Weak<InMemoryListener>>,
    pub inner_: Mutex<InMemoryListenerInnerState>,
}

impl InMemoryListener {
    pub fn new(switchboard: Arc<InMemorySwitchboard>) -> InMemoryListener {
        InMemoryListener {
            switchboard_: switchboard,
            self_weak_: None,
            inner_: Mutex::new(empty_listener_inner()),
        }
    }

    pub fn listen(&self, address: &str) -> ChannelError {
        inmemory_listener_listen_with_weak(self, address, self.self_weak_.clone())
    }

    pub fn close(&self) {
        let mut guard = self.inner_.lock().unwrap();
        if guard.closed {
            return;
        }
        guard.closed = true;
        let address_to_unregister: LegacyStdString = guard.local_address.clone();
        drop(guard);
        if !address_to_unregister.is_empty() {
            // Keep the cross-object lock acquisition after dropping the
            // listener guard. The direct map operation also avoids taking a
            // reference to a mapped local through an Arc-dispatched method.
            let mut switchboard_guard = self.switchboard_.listeners_.lock().unwrap();
            switchboard_guard.remove(&address_to_unregister);
        }
    }

    pub fn is_closed(&self) -> bool {
        let guard = self.inner_.lock().unwrap();
        guard.closed
    }

    pub fn local_address(&self) -> LegacyStdString {
        let guard = self.inner_.lock().unwrap();
        guard.local_address.clone()
    }

    pub fn set_on_accept(&self, callback: OnAcceptCallback) {
        let mut guard = self.inner_.lock().unwrap();
        guard.on_accept = callback;
    }

    pub fn set_on_error(&self, callback: OnErrorCallback) {
        let mut guard = self.inner_.lock().unwrap();
        guard.on_error = callback;
    }

    pub fn set_self_weak(&mut self, weak: std::sync::Weak<InMemoryListener>) {
        self.self_weak_ = Some(weak);
    }
}

// The owning shim can safely derive a Weak from its Arc without creating a
// self-cycle during construction. Keeping this helper private preserves the
// exported listener surface and the historical lock/register ordering.
fn inmemory_listener_listen_with_weak(
    listener: &InMemoryListener,
    address: &str,
    self_weak: Option<std::sync::Weak<InMemoryListener>>,
) -> ChannelError {
    let weak: std::sync::Weak<InMemoryListener>;
    {
        let mut guard = listener.inner_.lock().unwrap();
        if guard.closed {
            return channel_error_internal();
        }
        if !guard.local_address.is_empty() {
            if guard.local_address == address {
                return channel_error_none();
            }
            return channel_error_address_in_use();
        }
        if self_weak.is_none() {
            return channel_error_internal();
        }
        weak = self_weak.unwrap();
        guard.local_address = address.to_string();
    }

    if !listener
        .switchboard_
        .register_listener(address.to_string(), weak)
    {
        let mut guard = listener.inner_.lock().unwrap();
        guard.local_address.clear();
        return channel_error_address_in_use();
    }
    channel_error_none()
}

pub fn inmemory_listener_accept_for_connect(
    listener: &InMemoryListener,
    client_address: &LegacyStdString,
) -> Option<Arc<InMemoryChannel>> {
    let callback: OnAcceptCallback;
    let server_address: LegacyStdString;
    {
        let guard = listener.inner_.lock().unwrap();
        if guard.closed || guard.local_address.is_empty() {
            return None;
        }
        callback = guard.on_accept.clone();
        server_address = guard.local_address.clone();
    }
    if !callback.has_value() {
        return None;
    }

    let state: Arc<InMemoryConnectionState> =
        make_connection_state(client_address.clone(), server_address);
    let client_side: Arc<InMemoryChannel> = Arc::new(InMemoryChannel::new(state.clone(), true));
    let server_side: Arc<InMemoryChannel> = Arc::new(InMemoryChannel::new(state.clone(), false));

    (callback.callable())(make_inmemory_channel_proxy(server_side));
    Some(client_side)
}

/// Arc-retaining adapter for the listener facade's owning proxy.
#[repr(C)]
pub struct InMemoryListenerShim {
    pub listener_: Arc<InMemoryListener>,
}

#[cpp_inherit]
#[allow(unsafe_code)]
unsafe impl ChannelListenerBase for InMemoryListenerShim {
    fn listen(&mut self, address: &str) -> ChannelError {
        let self_weak: std::sync::Weak<InMemoryListener> =
            rusty::sync::downgrade(self.listener_.clone());
        inmemory_listener_listen_with_weak(&self.listener_, address, Some(self_weak))
    }

    fn close(&mut self) {
        self.listener_.close();
    }

    fn is_closed(&self) -> bool {
        self.listener_.is_closed()
    }

    fn local_address(&self) -> LegacyStdString {
        self.listener_.local_address()
    }

    fn set_on_accept(&mut self, callback: OnAcceptCallback) {
        self.listener_.set_on_accept(callback);
    }

    fn set_on_error(&mut self, callback: OnErrorCallback) {
        self.listener_.set_on_error(callback);
    }
}

pub fn make_inmemory_listener_proxy(listener: Arc<InMemoryListener>) -> ChannelListenerProxy {
    Box::new(InMemoryListenerShim {
        listener_: listener,
    })
}

/// Factory sharing one address registry across all of its listeners.
#[repr(C)]
pub struct InMemoryFactory {
    pub switchboard_: Arc<InMemorySwitchboard>,
}

impl InMemoryFactory {
    pub fn new(switchboard: Arc<InMemorySwitchboard>) -> InMemoryFactory {
        InMemoryFactory {
            switchboard_: switchboard,
        }
    }

    pub fn backend_name(&self) -> LegacyStdString {
        "inmemory".to_string()
    }

    pub fn connect(&self, address: &str) -> ConnectResult {
        inmemory_factory_connect(self, address)
    }

    pub fn make_listener(&self) -> Option<ChannelListenerProxy> {
        inmemory_factory_make_listener(self)
    }
}

pub fn inmemory_factory_connect(factory: &InMemoryFactory, address: &str) -> ConnectResult {
    let address_string: LegacyStdString = address.to_string();
    let listener_option: Option<Arc<InMemoryListener>> =
        factory.switchboard_.find_listener(&address_string);
    if listener_option.is_none() {
        return ConnectResult {
            connection: None,
            error: ChannelError::ConnectionRefused,
        };
    }
    let listener: Arc<InMemoryListener> = listener_option.unwrap();

    static CLIENT_COUNTER: AtomicU64 = AtomicU64::new(0_u64);
    let client_id: u64 = CLIENT_COUNTER.fetch_add(1_u64, Ordering::Relaxed);
    let mut client_address: LegacyStdString = "inmemory://client-".to_string();
    client_address += &client_id.to_string();
    let client_side_option: Option<Arc<InMemoryChannel>> =
        inmemory_listener_accept_for_connect(&listener, &client_address);
    if client_side_option.is_none() {
        return ConnectResult {
            connection: None,
            error: ChannelError::ConnectionRefused,
        };
    }
    let client_side: Arc<InMemoryChannel> = client_side_option.unwrap();

    ConnectResult {
        connection: Some(make_inmemory_channel_proxy(client_side)),
        error: ChannelError::None,
    }
}

pub fn inmemory_factory_make_listener(factory: &InMemoryFactory) -> Option<ChannelListenerProxy> {
    let listener: Arc<InMemoryListener> =
        Arc::new(InMemoryListener::new(factory.switchboard_.clone()));
    Some(make_inmemory_listener_proxy(listener))
}

/// Arc-retaining adapter for the factory facade's owning proxy.
#[repr(C)]
pub struct InMemoryFactoryShim {
    pub factory_: Arc<InMemoryFactory>,
}

#[cpp_inherit]
impl ChannelFactoryBase for InMemoryFactoryShim {
    fn connect(&mut self, address: &str) -> ConnectResult {
        self.factory_.connect(address)
    }

    fn make_listener(&mut self) -> Option<ChannelListenerProxy> {
        self.factory_.make_listener()
    }

    fn backend_name(&self) -> LegacyStdString {
        self.factory_.backend_name()
    }
}

pub fn make_inmemory_factory_proxy(factory: Arc<InMemoryFactory>) -> ChannelFactoryProxy {
    Box::new(InMemoryFactoryShim { factory_: factory })
}

/// Build a raw pair for fault-injection tests without a listener/factory.
pub fn make_channel_pair_for_testing(
    a_address: LegacyStdString,
    b_address: LegacyStdString,
) -> (Arc<InMemoryChannel>, Arc<InMemoryChannel>) {
    let state: Arc<InMemoryConnectionState> = make_connection_state(a_address, b_address);
    let a_side: Arc<InMemoryChannel> = Arc::new(InMemoryChannel::new(state.clone(), true));
    let b_side: Arc<InMemoryChannel> = Arc::new(InMemoryChannel::new(state.clone(), false));
    (a_side, b_side)
}
