//! Fiber receive wrapper for callback-driven channels.
//!
//! Callbacks own the synchronized frame queue and closed flag, so delivery may
//! come from another thread and an in-flight callback may outlive the wrapper.
//! Each recv_frame call owns its reactor event locally. The owner thread polls
//! the shared readiness predicate and resumes the waiting fiber. Only one
//! fiber may receive at a time. Drop detaches callbacks from the connection.

#![allow(
    non_camel_case_types,
    unsafe_code,
    unused_unsafe,
    clippy::borrowed_box,
    clippy::explicit_auto_deref,
    clippy::type_complexity
)]

use std::sync::atomic::{AtomicBool, Ordering};
use std::collections::VecDeque;
use std::marker::PhantomPinned;
use std::sync::{Arc, Mutex};

#[allow(unused_imports)]
use crate::reactor as _;
use crate::reactor::EventTestFn;

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
/// `recv_frame` and its event remain on the owning reactor thread. Callback
/// delivery can run on any transport thread: shared queue and closed state
/// are synchronized, and the owner observes readiness through its event.
#[repr(C)]
#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
pub struct FiberChannel {
    pub ch_: ChannelConnectionProxy,
    pub queue_: Arc<Mutex<LegacyStdDeque<OwnedFrame>>>,
    pub closed_: Arc<AtomicBool>,
    _pin: PhantomPinned,
}

impl FiberChannel {
    /// Construct an unbound receive wrapper.
    pub fn new(ch: ChannelConnectionProxy) -> FiberChannel {
        FiberChannel {
            ch_: ch,
            queue_: Arc::new(Mutex::new(Default::default())),
            closed_: Arc::new(AtomicBool::new(false)),
            _pin: PhantomPinned,
        }
    }

    /// Bind callbacks to shared receive state. An in-flight callback retains
    /// its state even when this wrapper is dropped or its callbacks are replaced.
    pub fn bind_callbacks(&mut self) {
        let queue: Arc<Mutex<LegacyStdDeque<OwnedFrame>>> = self.queue_.clone();
        let frame_callback: Box<dyn Fn(&ChannelFrame) + Send + Sync> = Box::new(move |frame| {
            let copy = fiberchannel_owned_copy(frame);
            queue.lock().unwrap().push_back(copy);
        });
        let ch: &mut Box<LegacyChannelConnectionBase> = &mut self.ch_;
        ch.set_on_frame(OnFrameCallback::from_callable(frame_callback));
        let closed: Arc<AtomicBool> = self.closed_.clone();
        let closed_callback: Box<dyn Fn(ChannelError) + Send + Sync> = Box::new(move |_reason| {
            closed.store(true, Ordering::Release);
        });
        ch.set_on_closed(OnClosedCallback::from_callable(closed_callback));

        // Fatal errors are followed by on_closed.  Non-fatal errors are
        // intentionally ignored at this layer, matching the original wrapper.
        let error_callback: Box<dyn Fn(ChannelError, &str) + Send + Sync> =
            Box::new(move |_error, _message| {});
        ch.set_on_error(OnErrorCallback::from_callable(
            error_callback,
        ));
    }

    fn try_pop(&self) -> Option<OwnedFrame> {
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
    pub fn recv_frame(&self) -> Option<OwnedFrame> {
        loop {
            if let Some(frame) = self.try_pop() {
                return Some(frame);
            }
            if self.closed_.load(Ordering::Acquire) {
                return None;
            }

            let event = self.arm_waiter();

            // Recheck after arming. A later delivery remains visible through
            // the event predicate when the owner next polls its waiting events.
            let mut should_wait: bool = true;
            {
                let guard = self.queue_.lock().unwrap();
                if !guard.is_empty() || self.closed_.load(Ordering::Acquire) {
                    should_wait = false;
                }
            }
            if should_wait {
                event.wait();
            }

        }
    }

    fn arm_waiter(&self) -> Arc<crate::reactor::IntEvent> {
        let event: Arc<crate::reactor::IntEvent> = crate::reactor::create_sp_int_event(1_i32);
        let queue: Arc<Mutex<LegacyStdDeque<OwnedFrame>>> = self.queue_.clone();
        let closed: Arc<AtomicBool> = self.closed_.clone();
        let predicate: EventTestFn = Some(Box::new(move |_value| {
            closed.load(Ordering::Acquire) || !queue.lock().unwrap().is_empty()
        }));
        // Only the owner reactor touches the event. Transport callbacks
        // publish queue/closed state through the mutex and atomic latch.
        *event.state_.test_.borrow_mut() = predicate;
        event
    }

    /// # Safety
    ///
    /// `frame` must satisfy the channel facade's raw payload validity
    /// contract for this synchronous call.
    pub unsafe fn send_frame(
        &self,
        frame: &ChannelFrame,
    ) -> ChannelError {
        let ch: &dyn ChannelConnectionBase = &*self.ch_;
        unsafe { ch.send_frame(frame) }
    }

    pub fn close(&self) {
        let ch: &dyn ChannelConnectionBase = &*self.ch_;
        ch.close();
    }

    /// The local callback latch may trail an explicit cross-thread proxy
    /// close, so preserve the current disjunction rather than consulting only
    /// one side.
    pub fn is_closed(&self) -> bool {
        if self.closed_.load(Ordering::Acquire) {
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
