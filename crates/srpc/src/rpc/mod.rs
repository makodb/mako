//! RPC layer — conversion slice S3 (leaf types and reliability policy)
//! and, later, the client/server endpoints.
//!
//! Ordered bottom-up: the pieces here depend only on [`crate::base`]
//! and [`crate::wire`], never on the transport or the fiber runtime.

pub mod client;
pub mod connection_metrics;
pub mod errors;
pub mod server;
pub mod task;

// REMOVED 2026-07-31: circuit_breaker, connection_state, reconnect,
// request_options, heartbeat, load_balancer — hand-written modules wired
// to NOTHING and duplicating rrr counterparts consulted by rrr's client.
//
// They come back by EXTRACTION from those DSL blocks, not by being
// rewritten again. The oracle for the extracted versions is rrr's own
// gtests (rpc_circuit_breaker_test.cc and friends), not the tests that
// went with this code — those tested an implementation that was never
// reached.
