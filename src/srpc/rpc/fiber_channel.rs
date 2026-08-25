//! Fiber-blocking receive wrapper for the callback-driven channel facade.
//!
//! This is the valid-Rust owner of the legacy `srpc.fiber_channel` module.  A
//! `FiberChannel` must reach its final address before [`FiberChannel::bind_callbacks`]
//! is called: the installed callbacks retain a raw pointer to the wrapper.  The
//! pin marker makes the generated C++ type non-copyable and non-movable, as the
//! original class was, and `Drop` detaches all three callbacks before the
//! connection and waiter state are destroyed.
//!
//! Inbound callbacks and `recv_frame` run on the connection's reactor thread;
//! construction, binding, and destruction belong there too.  The inbound queue
//! and optional waiter retain their independent mutexes to preserve the legacy
//! reentrancy and arm/wake ordering.  The waiter mutex is always released before
//! `IntEvent::set` or `wait`, because either operation may transfer control back
//! into the reactor.  Only one fiber may call `recv_frame` at a time.

#![allow(
    non_camel_case_types,
    unsafe_code,
    unused_unsafe,
    clippy::borrowed_box,
    clippy::explicit_auto_deref,
    clippy::type_complexity
)]

use cpp::srpc::reactor as cpp_reactor;
use rusty as cpp;
use std::cell::Cell;
use std::collections::VecDeque;
use std::marker::PhantomPinned;
use std::sync::{Arc, Mutex};

use crate::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFrame,
    OnClosedCallback, OnErrorCallback, OnFrameCallback,
};

// The Mako consumer profile maps this private queue alias back to its current
// nominal STL carrier. Owned payloads intentionally remain rusty::Vec in C++,
// matching the legacy module's post-DSL public API.
type LegacyStdDeque<T> = VecDeque<T>;
type LegacyChannelConnectionBase = dyn ChannelConnectionBase;

/// Heap-owned copy of an inbound frame payload.
///
/// The C++ consumer keeps this as `rusty::Vec`, matching the current legacy
/// module. (The pre-DSL historical class used `std::vector`.)
#[repr(C)]
#[derive(Default)]
pub struct OwnedFrame {
    pub bytes: Vec<u8>,
}

/// Fiber-style adapter over one callback-driven channel connection.
///
/// Construction and destruction, callback binding, and `recv_frame` belong on
/// the reactor thread.  `send_frame`, `close`, and `is_closed` preserve the
/// channel facade's thread-safety contract.  Call `bind_callbacks` only after
/// the value is pinned at its final address.
#[repr(C)]
#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
pub struct FiberChannel {
    pub ch_: ChannelConnectionProxy,
    pub queue_: Mutex<LegacyStdDeque<OwnedFrame>>,
    pub pending_recv_event_: Mutex<Option<Arc<rusty::ReactorIntEvent>>>,
    pub closed_: Cell<bool>,
    _pin: PhantomPinned,
}

impl FiberChannel {
    /// Construct an unbound wrapper.  Bind only after placing it at a stable
    /// address (normally inside `Box`/`rusty::Box`).
    pub fn new(ch: ChannelConnectionProxy) -> FiberChannel {
        FiberChannel {
            ch_: ch,
            queue_: Mutex::new(Default::default()),
            pending_recv_event_: Mutex::new(None),
            closed_: Cell::new(false),
            _pin: PhantomPinned,
        }
    }

    /// Install the connection callbacks after this wrapper reaches its final
    /// address.  Rebinding replaces the previous callback set.
    pub fn bind_callbacks(&mut self) {
        let self_ptr: *mut FiberChannel = &raw mut *self;

        // Store the pinned address as an integer so the callback itself meets
        // the channel facade's Send+Sync capture contract. The reactor-thread
        // affinity documented on FiberChannel remains the safety invariant
        // governing the dereference.
        let frame_self: usize = self_ptr as usize;
        let frame_callback: Box<dyn Fn(&ChannelFrame) + Send + Sync> = Box::new(move |frame| {
            // SAFETY: `bind_callbacks` requires a pinned wrapper and Drop
            // detaches this callback before tearing down any member.
            unsafe { (*(frame_self as *mut FiberChannel)).on_inbound_frame(frame) };
        });
        let ch: &mut Box<LegacyChannelConnectionBase> = &mut self.ch_;
        ch.set_on_frame(OnFrameCallback::from_callable(
            frame_callback,
        ));

        let closed_self: usize = self_ptr as usize;
        let closed_callback: Box<dyn Fn(ChannelError) + Send + Sync> = Box::new(move |_reason| {
            // SAFETY: same pin-and-detach invariant as the frame callback.
            unsafe { (*(closed_self as *mut FiberChannel)).on_inbound_closed() };
        });
        ch.set_on_closed(OnClosedCallback::from_callable(
            closed_callback,
        ));

        // Fatal errors are followed by on_closed.  Non-fatal errors are
        // intentionally ignored at this layer, matching the original wrapper.
        let error_callback: Box<dyn Fn(ChannelError, &str) + Send + Sync> =
            Box::new(move |_error, _message| {});
        ch.set_on_error(OnErrorCallback::from_callable(
            error_callback,
        ));
    }

    fn try_pop(&mut self) -> Option<OwnedFrame> {
        let mut guard = self.queue_.lock().unwrap();
        if guard.is_empty() {
            return None;
        }

        // Indexing plus `mem::take` is deliberate.  It remains valid Rust and
        // lowers against the mapped `std::deque`, whose `pop_front` returns
        // void rather than Rust VecDeque's removed element.
        let frame: OwnedFrame = core::mem::take(&mut guard[0]);
        guard.pop_front();
        Some(frame)
    }

    /// Suspend until one frame is available or the channel has closed.
    /// Queued frames are always drained before `None` is returned.
    pub fn recv_frame(&mut self) -> Option<OwnedFrame> {
        loop {
            if let Some(frame) = self.try_pop() {
                return Some(frame);
            }
            if self.closed_.get() {
                return None;
            }

            self.arm_waiter();

            // Close the empty-check/arm race before suspending.  A callback
            // that ran before arming made either the queue or closed latch
            // observable here; a callback after this check sees the waiter.
            let mut should_wait: bool = true;
            {
                let guard = self.queue_.lock().unwrap();
                if !guard.is_empty() || self.closed_.get() {
                    should_wait = false;
                }
            }
            if should_wait {
                self.wait_for_signal();
            }

            let mut event_guard = self.pending_recv_event_.lock().unwrap();
            *event_guard = None;
        }
    }

    fn on_inbound_frame(&mut self, frame: &ChannelFrame) {
        let copy: OwnedFrame = fiberchannel_owned_copy(frame);
        {
            let mut guard = self.queue_.lock().unwrap();
            guard.push_back(copy);
        }
        self.signal_pending_recv();
    }

    fn on_inbound_closed(&mut self) {
        self.closed_.set(true);
        self.signal_pending_recv();
    }

    fn signal_pending_recv(&mut self) {
        let held: Option<Arc<rusty::ReactorIntEvent>> = {
            let guard = self.pending_recv_event_.lock().unwrap();
            (*guard).clone()
        };
        if let Some(event) = held {
            // SAFETY: the indexed foreign method is `IntEvent::set(int32_t)
            // const`; the live Arc keeps its receiver valid for the call.
            unsafe {
                cpp_reactor::IntEvent::set(&*event, 1_i32);
            }
        }
    }

    fn arm_waiter(&mut self) {
        let event: Arc<rusty::ReactorIntEvent> = unsafe {
            // SAFETY: the reactor factory registers a fresh IntEvent owned by
            // the returned Arc.  This method runs on the reactor thread.
            cpp_reactor::create_sp_int_event(1_i32)
        };
        let mut guard = self.pending_recv_event_.lock().unwrap();
        *guard = Some(event);
    }

    fn wait_for_signal(&mut self) {
        let held: Option<Arc<rusty::ReactorIntEvent>> = {
            let guard = self.pending_recv_event_.lock().unwrap();
            (*guard).clone()
        };
        if let Some(event) = held {
            // SAFETY: `wait` is the reactor's fiber-suspending const method.
            // The cloned Arc keeps the event alive across the suspension.
            unsafe {
                cpp_reactor::IntEvent::wait(&*event);
            }
        }
    }

    /// # Safety
    ///
    /// `frame` must satisfy the channel facade's raw payload validity
    /// contract for this synchronous call.
    pub unsafe fn send_frame(
        &mut self,
        frame: &ChannelFrame,
    ) -> ChannelError {
        let ch: &mut Box<LegacyChannelConnectionBase> = &mut self.ch_;
        unsafe { ch.send_frame(frame) }
    }

    pub fn close(&mut self) {
        let ch: &mut Box<LegacyChannelConnectionBase> = &mut self.ch_;
        ch.close();
    }

    /// The local callback latch may trail an explicit cross-thread proxy
    /// close, so preserve the current disjunction rather than consulting only
    /// one side.
    pub fn is_closed(&self) -> bool {
        if self.closed_.get() {
            return true;
        }
        let ch: &Box<LegacyChannelConnectionBase> = &self.ch_;
        ch.is_closed()
    }

    pub fn channel_for_test(&mut self) -> &mut ChannelConnectionProxy {
        &mut self.ch_
    }
}

impl Drop for FiberChannel {
    #[cfg_attr(any(), cpp_noexcept)]
    fn drop(&mut self) {
        // Callback replacement happens before any other field is destroyed.
        // The explicit values preserve the channel facade's nullable wrapper
        // representation without synthesizing an Option around it.
        let ch: &mut Box<LegacyChannelConnectionBase> = &mut self.ch_;
        ch.set_on_frame(OnFrameCallback::default());
        ch.set_on_closed(OnClosedCallback::default());
        ch.set_on_error(OnErrorCallback::default());
    }
}

fn fiberchannel_owned_copy(frame: &ChannelFrame) -> OwnedFrame {
    let mut owned: OwnedFrame = Default::default();
    if frame.size > 0_usize && !frame.payload.is_null() {
        // SAFETY: ChannelFrame promises that payload addresses `size` bytes
        // for the duration of the callback.  Copying severs that lifetime.
        owned.bytes.resize(frame.size, 0_u8);
        unsafe {
            core::ptr::copy_nonoverlapping(frame.payload, owned.bytes.as_mut_ptr(), frame.size);
        }
    }
    owned
}
