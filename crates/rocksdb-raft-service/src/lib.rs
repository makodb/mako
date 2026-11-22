//! Native SRPC transactions over local-only Rust STO, direct RocksDB and Raft.
//! Transaction handles remain pinned to bounded owner lanes; no borrowed STO
//! frame is sent across a thread or stored in a self-referential container.
pub mod client;
pub mod config;
pub mod ffi;
mod rpc;
pub mod server;
#[cfg(feature = "test-hooks")]
mod test_hooks;
pub mod wire;
