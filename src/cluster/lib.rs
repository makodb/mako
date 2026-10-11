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
pub mod coordinator_publication;
pub mod warehouse;
pub mod gateway;
pub mod full_scan_core;
pub mod full_scan;
pub mod durable_kv;
pub mod durable_codec;
pub mod durable_coordinator;
pub mod durable_coordinator_codec;
pub mod durable_engine;
pub mod durable_participant;

#[cfg(not(verus_keep_ghost))]
pub mod rocks_metadata;
#[cfg(not(verus_keep_ghost))]
pub mod metadata;
#[cfg(not(verus_keep_ghost))]
pub mod metadata_coordinator;

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
pub mod snapshot_admission_composition;
#[cfg(verus_keep_ghost)]
pub mod routing_codec_proofs;
#[cfg(verus_keep_ghost)]
pub mod full_scan_proofs;
#[cfg(verus_keep_ghost)]
#[path = "../../tla/mako/src/sharding_progress.rs"]
pub mod sharding_progress;
#[cfg(verus_keep_ghost)]
pub mod transfer_progress;
#[cfg(verus_keep_ghost)]
#[path = "../../tla/mako/src/sharding_bytes.rs"]
pub mod sharding_bytes;
#[cfg(verus_keep_ghost)]
#[path = "../../tla/mako/src/sharding_recovery.rs"]
pub mod sharding_recovery;
#[cfg(verus_keep_ghost)]
pub mod durable_coordinator_proofs;
#[cfg(verus_keep_ghost)]
pub mod durable_recovery_refinement;
#[cfg(verus_keep_ghost)]
pub mod durable_metadata_refinement;
#[cfg(verus_keep_ghost)]
pub mod durable_engine_refinement;

#[cfg(all(test, not(verus_keep_ghost)))]
#[path = "durable_recovery_tests.rs"]
mod durable_recovery_tests;

#[cfg(all(test, not(verus_keep_ghost)))]
mod metadata_tests;
