//! Native application ownership, shard sessions and SRPC transport.
//! Distributed STO supplies commit decisions, coordination and OCC protection.
//! Routing and migration remain in mako-sharding. This crate owns
//! their process and network lifetimes.
pub mod admission;
#[cfg(feature = "logging")]
mod application;
#[cfg(feature = "runtime-storage")]
pub mod client;
#[cfg(feature = "runtime-storage")]
pub mod configuration;
pub mod epoch;
#[cfg(feature = "runtime-storage")]
pub mod gateway;
pub mod gateway_client;
pub mod gateway_protocol;
#[cfg(feature = "logging")]
pub mod log;
#[cfg(feature = "runtime-storage")]
pub mod migration;
#[cfg(feature = "runtime-storage")]
mod migration_metadata;
#[cfg(feature = "runtime-storage")]
mod migration_ranges;
#[cfg(feature = "runtime-storage")]
mod migration_storage;
#[cfg(feature = "runtime-storage")]
pub mod node;
#[cfg(feature = "runtime-storage")]
mod prepared;
#[cfg(feature = "runtime-storage")]
pub mod process;
#[cfg(feature = "runtime-storage")]
pub mod receiver;
#[cfg(feature = "runtime-storage")]
pub mod redis;
pub mod redo;
pub mod rpc;
#[cfg(feature = "runtime-storage")]
mod scan;
#[cfg(feature = "runtime-storage")]
pub mod storage;
#[cfg(feature = "runtime-storage")]
pub mod transactions;
pub mod wire;
