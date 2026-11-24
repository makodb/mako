//! Engine-neutral contract between the Mako transaction cache and a local
//! transaction engine.
//!
//! The cache (`mako-cache`) drives the Rust engine (`mako-engine-sto`) through
//! the value types defined here. The default `distributed` feature exposes
//! the cache/engine facade; local-only users retain neutral types.
//!
//! Everything in this crate is plain data or a trait; there is no unsafe block
//! and no engine logic. Numeric contract values (feature bits, limits,
//! enum discriminants, record framing) are literals here; a native facade must
//! assert equality with its linked ABI's constants at compile time.

#![deny(unsafe_code)]
// The contract declares `unsafe fn` signatures (the engines own the unsafe
// bodies); this crate itself contains no unsafe block.
#![allow(unsafe_code)]

#[cfg(feature = "distributed")]
mod commit;
#[cfg(feature = "distributed")]
pub mod engine;
mod error;
mod features;
mod limits;
#[cfg(feature = "distributed")]
mod modes;
#[cfg(feature = "distributed")]
mod test_hooks;
mod timestamp;

#[cfg(feature = "distributed")]
pub use commit::{
    record_format, CommitDisposition, CommitHolderReport, CommitRecordChecksum,
    CommitRecordPreflight, CommitRecordReport, CommitRecordTarget, CommitReport,
    FusedOnePutHolderAttempt,
};
#[cfg(feature = "distributed")]
pub use engine::{
    EngineDb, EngineTransaction, HolderPool, HolderView, LocalEngine, NativeOrderedArenaControl,
    NativeOrderedHolderControl, NativeOrderedRecordOutcome, PreselectedHolderOutcome,
    PreselectedRecordOutcome, SpscHolderControl,
};
pub use error::{Error, Result};
pub use features::Features;
pub use limits::{
    MAX_KEY_BYTES, MAX_TABLE_NAME_BYTES, MAX_VALUE_BYTES, MAX_WORKERS, TRANSACTION_ITEM_BUDGET,
};
#[cfg(feature = "distributed")]
pub use modes::CacheOrderMode;
#[cfg(feature = "distributed")]
pub use test_hooks::{TestCleanupBoundary, TestCommitObserver, TestCommitPhase, WorkerHealth};
pub use timestamp::MakoTimestamp;
