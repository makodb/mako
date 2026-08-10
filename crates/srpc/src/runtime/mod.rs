//! The event loop and its primitives — conversion slice S2 onward.
//!
//! Ordered so each piece stands on the one below: epoll registration,
//! then the poll thread, then connections and endpoints.

pub mod epoll;
pub mod fiber;
pub mod poll_thread;
pub mod tcp;
