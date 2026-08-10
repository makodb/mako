//! RPC layer — conversion slice S3 (leaf types and reliability policy)
//! and, later, the client/server endpoints.
//!
//! Ordered bottom-up: the pieces here depend only on [`crate::base`]
//! and [`crate::wire`], never on the transport or the fiber runtime.

pub mod client;
pub mod errors;
pub mod server;
pub mod task;

// REMOVED 2026-07-31: circuit_breaker, connection_metrics,
// connection_state, reconnect, request_options, heartbeat,
// load_balancer — 2,840 LOC, hand-written, wired to NOTHING, and
// duplicating rrr counterparts that are already 80-94% inline-Rust DSL
// and are consulted on every request in rrr's client path.
//
// They come back by EXTRACTION from those DSL blocks, not by being
// rewritten again. The oracle for the extracted versions is rrr's own
// gtests (rpc_circuit_breaker_test.cc and friends), not the tests that
// went with this code — those tested an implementation that was never
// reached.
