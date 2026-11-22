//! Engine-neutral contract between the Mako transaction cache and a local
//! transaction engine.
//!
//! The cache (`mako-cache`) drives exactly one local engine per process. Two
//! engines implement this contract: the C ABI facade over C++ STO/MassTrans
//! (`mako-local`) and the native Rust STO engine (`mako-engine-sto`). Both
//! expose the same value types defined here, and each implements the traits
//! in [`engine`] so a shared conformance suite can hold them to one behavior.
//!
//! Everything in this crate is plain data or a trait; there is no unsafe block
//! and no engine logic. Numeric contract values (feature bits, limits,
//! enum discriminants, record framing) are literals here; a native facade must
//! assert equality with its linked ABI's constants at compile time.

#![deny(unsafe_code)]
// The contract declares `unsafe fn` signatures (the engines own the unsafe
// bodies); this crate itself contains no unsafe block.
#![allow(unsafe_code)]

mod commit;
pub mod engine;
mod error;
mod features;
mod limits;
mod modes;
mod test_hooks;
mod timestamp;

pub use commit::{
    record_format, CommitDisposition, CommitHolderReport, CommitRecordChecksum,
    CommitRecordPreflight, CommitRecordReport, CommitRecordTarget, CommitReport,
    FusedOnePutHolderAttempt,
};
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
pub use modes::CacheOrderMode;
pub use test_hooks::{TestCleanupBoundary, TestCommitObserver, TestCommitPhase, WorkerHealth};
pub use timestamp::MakoTimestamp;
