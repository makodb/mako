//! Verified storage-operation control flow and cursor decisions.
//!
//! Public entry points are total. The engine trait's internal preconditions
//! are proved at their call sites. Native implementations must satisfy the
//! explicit E1-E7 contracts in engine.rs and walk.rs. The shell retains C
//! types, raw pointers, thread registration, panic containment, and fail-stop.
//!
//! All executable implementations here are checked inside `verus!`:
//! `scripts/verify_shard_host_core.sh` runs the pinned Verus with
//! `--no-cheating`, refuses any item outside the macro, and requires Verus to
//! reject a list of deliberately wrong variants.
#![forbid(unsafe_code)]

pub mod order;
pub mod engine;
pub mod verbs;
pub mod walk;
pub mod scan;
pub mod status;

pub use scan::{fixed_scan, FixedScan, scan_arguments, scan_flags_valid, ScanLower, ScanRefusal, ScanStart};
pub use status::{
    map_engine, Mapped, Stage, ABORTED, COMMITTED, ENGINE_COMMITTED_ERROR, ENGINE_EXHAUSTED,
    ENGINE_FATAL, ENGINE_OK, ENGINE_RETRY, ENGINE_UNKNOWN, SHARD_EXHAUSTED, SHARD_INVALID,
    SHARD_IO, SHARD_NOT_FOUND, SHARD_OK, SHARD_RETRY,
};
