//! The decisions of Mako's sharding storage host on the Rust engine that
//! Verus checks.
//!
//! Every function here is pure and total: it has no precondition, so the
//! unverified shell that calls it (`sto-tpcc-ffi`'s `shard_host.rs`) cannot
//! call it wrongly. The shell keeps what Verus does not take: the C types, raw
//! pointers, thread-locals, the registry lock, panic containment, the
//! fail-stop and every call into the engine.
//!
//! All code of this crate is inside `verus!`, and none of it is trusted:
//! `scripts/verify_shard_host_core.sh` runs the pinned Verus with
//! `--no-cheating`, refuses any item outside the macro, and requires Verus to
//! reject a list of deliberately wrong variants.
#![forbid(unsafe_code)]

pub mod order;
pub mod scan;
pub mod status;

pub use scan::{fixed_scan, FixedScan, scan_arguments, scan_flags_valid, ScanLower, ScanRefusal, ScanStart};
pub use status::{
    map_engine, Mapped, Stage, ABORTED, COMMITTED, ENGINE_COMMITTED_ERROR, ENGINE_EXHAUSTED,
    ENGINE_FATAL, ENGINE_OK, ENGINE_RETRY, ENGINE_UNKNOWN, SHARD_EXHAUSTED, SHARD_INVALID,
    SHARD_IO, SHARD_NOT_FOUND, SHARD_OK, SHARD_RETRY,
};
