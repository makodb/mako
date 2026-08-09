//! Owning compatibility facade for objects driven by the C++ poll thread.
//!
//! The two traits are deliberately distinct.  [`PollableBase`] preserves the
//! legacy virtual interface: cheap observations are shared, while operations
//! performed by the poll loop require mutable access to the owning proxy.
//! [`PollableSharedTarget`] is the nominal contract of the object stored in an
//! [`Arc`].  Its methods all take shared access because the concrete object is
//! responsible for its own synchronization.

use std::sync::Arc;

/// A thread-safe object whose poll hooks can run through shared ownership.
///
/// This nominal Rust bound replaces the legacy C++ adapter's unchecked duck
/// typing.  The C++ consumer erases ordinary Rust trait bounds on templates,
/// retaining the established structural call surface for existing C++ types.
#[doc(hidden)]
pub trait PollableSharedTarget: Send + Sync {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&self) -> usize;
    fn handle_read(&self) -> bool;
    fn handle_write(&self) -> i32;
    fn handle_error(&self);
    fn close(&self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}

/// Type-erased interface stored by the legacy poll worker.
///
/// Receiver mutability intentionally matches `rrr::PollableBase`: changing
/// the mutable methods to `&self` would silently change their C++ virtual
/// signatures and break existing derived shims.
pub trait PollableBase: Send + Sync {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&mut self) -> usize;
    fn handle_read(&mut self) -> bool;
    fn handle_write(&mut self) -> i32;
    fn handle_error(&mut self);
    fn close(&mut self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}

/// Owning, type-erased handle consumed by the poll worker.
pub type PollableProxy = Box<dyn PollableBase>;

/// One-field adapter retaining shared ownership of a concrete pollable.
///
/// The disabled attribute is valid Rust.  Before this owner is enabled in the
/// C++ consumer manifest, rusty-cpp must recognize this hidden spelling as an
/// instruction to emit direct `PollableBase` inheritance rather than a
/// separate adapter object.
pub struct PollableArcShim<T: PollableSharedTarget> {
    pub poll_: Arc<T>,
}

#[cfg_attr(any(), cpp_inherit)]
impl<T: PollableSharedTarget> PollableBase for PollableArcShim<T> {
    fn fd(&self) -> i32 {
        let poll: &T = &*self.poll_;
        poll.fd()
    }

    fn poll_mode(&self) -> i32 {
        let poll: &T = &*self.poll_;
        poll.poll_mode()
    }

    fn content_size(&mut self) -> usize {
        let poll: &T = &*self.poll_;
        poll.content_size()
    }

    fn handle_read(&mut self) -> bool {
        let poll: &T = &*self.poll_;
        poll.handle_read()
    }

    fn handle_write(&mut self) -> i32 {
        let poll: &T = &*self.poll_;
        poll.handle_write()
    }

    fn handle_error(&mut self) {
        let poll: &T = &*self.poll_;
        poll.handle_error()
    }

    fn close(&mut self) {
        let poll: &T = &*self.poll_;
        poll.close()
    }

    fn check_pending_write_update(&self) -> bool {
        let poll: &T = &*self.poll_;
        poll.check_pending_write_update()
    }

    fn is_closed(&self) -> bool {
        let poll: &T = &*self.poll_;
        poll.is_closed()
    }
}

/// Retain `poll` in a one-field adapter and erase its concrete type.
pub fn make_pollable_proxy_from_typed_arc<T>(poll: Arc<T>) -> PollableProxy
where
    T: PollableSharedTarget + 'static,
{
    Box::new(PollableArcShim { poll_: poll })
}
