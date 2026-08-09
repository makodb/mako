//! RPC layer — conversion slice S3 (leaf types and reliability policy)
//! and, later, the client/server endpoints.
//!
//! Ordered bottom-up: the pieces here depend only on [`crate::base`]
//! and [`crate::wire`], never on the transport or the fiber runtime.

pub mod callbacks;
pub mod channel;
pub mod circuit_breaker;
pub mod client;
pub mod completion_tracker;
pub mod connection_metrics;
pub mod connection_state;
pub mod errors;
pub mod heartbeat;
pub mod pollable_proxy;
pub mod reconnect_policy;
pub mod request_options;
pub mod server;
pub mod task;

pub use channel::ChannelError;

// REMOVED 2026-07-31: load_balancer — a hand-written module wired to NOTHING
// and duplicating the rrr counterpart consulted by rrr's client.
//
// They come back by EXTRACTION from those DSL blocks, not by being
// rewritten again. The oracle for the extracted versions is rrr's own
// gtests (rpc_circuit_breaker_test.cc and friends), not the tests that
// went with this code — those tested an implementation that was never
// reached.
