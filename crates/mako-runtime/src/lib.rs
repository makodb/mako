//! Native application ownership, shard sessions and SRPC transport.
//! Transaction algorithms remain in STO; routing and migration remain in
//! mako-sharding. This crate owns their process and network lifetimes.
pub mod admission;
#[cfg(feature = "native-storage")]
pub mod client;
#[cfg(feature = "durable-storage")]
pub mod durable;
pub mod epoch;
#[cfg(feature = "native-storage")]
pub mod gateway;
pub mod gateway_client;
#[cfg(feature = "native-storage")]
pub mod migration;
#[cfg(feature = "native-storage")]
pub mod node;
#[cfg(feature = "native-storage")]
pub mod receiver;
pub mod redo;
#[cfg(feature = "replication")]
pub mod replication;
pub mod rpc;
#[cfg(feature = "native-storage")]
mod scan;
#[cfg(feature = "native-storage")]
pub mod storage;
pub mod wire;
