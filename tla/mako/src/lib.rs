//! MakoV2 scalar/single-Raft model and corrected live-process range handoff.
//! TLA-style transition relations and unbounded Verus safety proofs.
#![allow(non_snake_case)]
#![allow(unused_imports)]
#![allow(dead_code)]

pub mod timestamp;
pub mod types;
pub mod normal;
pub mod recovery;
pub mod behavior;
pub mod log_invariants;
pub mod log_lemmas;
pub mod proofs_replication;
pub mod invariants;
pub mod proofs_occ;
pub mod history;
pub mod proofs_stable;
pub mod proofs_history;
pub mod proofs_witness;
pub mod proofs_main;

// Source-derived sharding algorithms and checked as-built counterexamples.
pub mod sharding_partition;
pub mod sharding_publication;
pub mod sharding_drain;
pub mod sharding_reads;
pub mod sharding_mirror;

// Corrected handoff at an explicit successful-transaction engine boundary.
pub mod sharding_placement;
pub mod sharding_transactions;
pub mod sharding_witness;
pub mod sharding_return_witness;
pub mod sharding_abort_witness;

// Sharding protocol extensions under explicit transaction/Raft interfaces.
// These modules do not enable the production durability/recovery adapters.
pub mod sharding_recovery;
pub mod sharding_progress;
pub mod sharding_retention;
pub mod sharding_bytes;
