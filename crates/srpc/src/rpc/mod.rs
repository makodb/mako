//! RPC layer — conversion slice S3 (leaf types and reliability policy)
//! and, later, the client/server endpoints.
//!
//! Ordered bottom-up: the pieces here depend only on [`crate::base`]
//! and [`crate::wire`], never on the transport or the fiber runtime.

pub mod circuit_breaker;
pub mod connection_state;
pub mod errors;
pub mod heartbeat;
pub mod load_balancer;
pub mod reconnect;
pub mod request_options;
