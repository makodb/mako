//! Same-source native sharding actors and their erased independent-spec proofs.
//! Runtime threads/foreign engine and RPC callbacks are explicit native boundaries.
pub mod types;
pub mod bytes;
pub mod catalog;
pub mod directory;
pub mod directory_proofs;
pub mod leases;
pub mod migration;
pub mod participant;
pub mod storage;
pub mod transfer;
pub mod routing;
pub mod routing_codec;
pub mod warehouse;
pub mod gateway;
pub mod full_scan_core;
pub mod full_scan;

#[cfg(not(verus_keep_ghost))]
pub mod host;
#[cfg(not(verus_keep_ghost))]
pub mod wire;
#[cfg(not(verus_keep_ghost))]
pub mod runtime;
#[cfg(not(verus_keep_ghost))]
pub mod ffi;
#[cfg(not(verus_keep_ghost))]
pub mod gateway_ffi;

#[cfg(verus_keep_ghost)]
#[path = "../../tla/mako/src/sharding_partition.rs"]
pub mod sharding_partition;
#[cfg(verus_keep_ghost)]
#[path = "../../tla/mako/src/sharding_placement.rs"]
pub mod sharding_placement;
#[cfg(verus_keep_ghost)]
#[path = "../../tla/mako/src/sharding_mirror.rs"]
pub mod sharding_mirror;
#[cfg(verus_keep_ghost)]
pub mod ghost_log;
#[cfg(verus_keep_ghost)]
pub mod directory_partition;
#[cfg(verus_keep_ghost)]
pub mod leases_proofs;
#[cfg(verus_keep_ghost)]
pub mod migration_refinement;
#[cfg(verus_keep_ghost)]
pub mod migration_invariants;
#[cfg(verus_keep_ghost)]
pub mod participant_proofs;
#[cfg(verus_keep_ghost)]
pub mod storage_refinement;
#[cfg(verus_keep_ghost)]
pub mod transfer_proofs;
#[cfg(verus_keep_ghost)]
pub mod execution_refinement;
#[cfg(verus_keep_ghost)]
pub mod routing_proofs;
#[cfg(verus_keep_ghost)]
pub mod routing_codec_proofs;
