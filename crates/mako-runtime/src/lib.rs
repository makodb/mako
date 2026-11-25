//! Native application ownership, shard sessions and SRPC transport.
//! Distributed commit decisions use mako-protocol; native conflict protection
//! uses STO. Routing and migration remain in mako-sharding. This crate owns
//! their process and network lifetimes.
pub mod admission;
#[cfg(feature = "native-storage")]
pub mod benchmark;
#[cfg(feature = "native-storage")]
pub mod client;
#[cfg(feature = "native-storage")]
pub mod configuration;
#[cfg(feature = "durable-storage")]
pub mod durable;
pub mod epoch;
#[cfg(feature = "native-storage")]
pub mod gateway;
pub mod gateway_client;
#[cfg(feature = "replication")]
pub mod log;
#[cfg(feature = "native-storage")]
pub mod migration;
#[cfg(feature = "native-storage")]
pub mod node;
#[cfg(feature = "native-storage")]
mod prepared;
#[cfg(feature = "native-storage")]
pub mod process;
#[cfg(feature = "native-storage")]
pub mod receiver;
#[cfg(feature = "native-storage")]
pub mod redis;
pub mod redo;
pub mod rpc;
#[cfg(feature = "native-storage")]
mod scan;
#[cfg(feature = "native-storage")]
pub mod storage;
#[cfg(feature = "native-storage")]
pub mod tpcc;
#[cfg(feature = "native-storage")]
pub mod transactions;
pub mod wire;
