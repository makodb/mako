//! Fast harness for the canonical runtime gateway policy and regression cases.
//! Compiles no runtime services, RPC or engines. Full runtime tests use the
//! same cases through its compiled library.
#[path = "../../mako-runtime/src/gateway_protocol.rs"]
pub mod client;
#[path = "../../mako-runtime/tests/support/gateway_contract.rs"]
mod cases;
