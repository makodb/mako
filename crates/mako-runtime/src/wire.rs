//! Shared upper-layer wire; the protocol crate has no native backend dependency.
pub use sto_core::protocol::wire::*;

const _: () = assert!(sto_core::protocol::wire::SCAN_REQUEST_CAPACITY == mako_sharding::full_scan_core::REQUEST_CAPACITY);
