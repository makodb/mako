//! Native application ownership, shard sessions and SRPC transport.
//! Transaction algorithms remain in STO; routing and migration remain in
//! mako-sharding. This crate owns their process and network lifetimes.
pub mod admission;
#[cfg(feature = "native-storage")]
pub mod client;
pub mod epoch;
#[cfg(feature = "native-storage")]
pub mod receiver;
pub mod rpc;
#[cfg(feature = "native-storage")]
pub mod storage;
pub mod wire;
