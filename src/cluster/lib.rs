//! Same-source native sharding actors and their erased independent-spec proofs.
//! The default engine-host feature includes native runtime/foreign callbacks.
//! Without it, only immutable routing/catalog and their byte codecs are exported.
pub mod types;
#[cfg(verus_keep_ghost)]
#[path = "../../crates/shard-host-core/src/lib.rs"]
pub mod rust_storage_core;
#[cfg(verus_keep_ghost)]
pub mod rust_storage_bridge;
pub mod bytes;
pub mod catalog;
pub mod directory;
pub mod directory_proofs;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod leases;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod migration;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod participant;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod epoch_admission;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod storage;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod transfer;
pub mod routing;
pub mod routing_codec;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod coordinator_publication;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod warehouse;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod gateway;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod full_scan_core;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod full_scan;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod durable_kv;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod durable_codec;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod durable_coordinator;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod durable_coordinator_codec;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod durable_engine;
#[cfg(any(feature = "engine-host", verus_keep_ghost))]
pub mod durable_participant;

#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod rocks_metadata;
#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod metadata;
#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod metadata_coordinator;

#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod host;
#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod wire;
#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod runtime;
#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
pub mod ffi;
#[cfg(all(feature = "engine-host", not(verus_keep_ghost)))]
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
pub mod coordinator_publication_proofs;
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

#[cfg(all(test, feature = "engine-host", not(verus_keep_ghost)))]
#[path = "durable_recovery_tests.rs"]
mod durable_recovery_tests;

#[cfg(all(test, feature = "engine-host", not(verus_keep_ghost)))]
mod metadata_tests;
