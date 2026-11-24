//! The Mako cache's local engine, implemented over the native Rust STO.
//!
//! This crate implements the [`mako_engine_api`] contract using `sto-core`
//! transactions over `sto-masstree` tables, Mako timestamps from `mako-clock`,
//! and cache commit records encoded in Rust. `mako-engine` re-exports this
//! implementation for cache callers.
//!
//! # Protocol parity
//!
//! The commit terminals implement the validation-gate protocol using `sto-core`'s [`sto_core::CommitHook`] seams:
//!
//! - `reserve_upper_metadata` (write set locked) enters the general
//!   certification gate for general-profile commits;
//! - `pre_install` (final validation succeeded, locks held) mints the Mako
//!   timestamp, takes the dense cache sequence or the caller's lane sequence,
//!   binds the record sink, leaves the gate, and serializes the record before
//!   any write is installed;
//! - `post_install` publishes nothing; queue publication is either done by the
//!   terminal after the transaction is finished (holder READY, SPSC ACK) or
//!   by the cache.
//!
//! The restricted profile (exactly one update of an existing key, observed
//! only through its own write lock) never takes the gate and allocates its
//! `(timestamp, sequence)` pair after validation, exactly like the C++
//! `accept_ordered` path.
//!
//! # Worker model
//!
//! Workers are process-lifetime, as in the C++ engine: a thread's
//! `sto-core` worker context and Masstree worker are attached on first use
//! and never released, so `MAX_WORKERS` bounds the threads that may ever use
//! one database. A cleanup or commit failure that `sto-core` cannot prove
//! complete quarantines the calling worker; because `sto-core` also poisons
//! its runtime, every later worker of that database reports the same
//! quarantine. This is the conservative reading of the open decision in the
//! cutover plan.
//!
//! Database health also includes the process-wide native Masstree runtime.
//! Its poison can be visible before a point operation propagates it into a
//! database's core runtime. The diagnostic only observes these states; it
//! does not attach a worker or change transaction admission and cleanup.

#![warn(missing_docs)]

mod db;
mod engine;
mod holder;
mod outcome;
mod prefetch;
mod record;
mod touched;
mod txn;
mod worker;

pub use mako_engine_api::{
    record_format, CacheOrderMode, CommitDisposition, CommitHolderReport, CommitRecordChecksum,
    CommitRecordPreflight, CommitRecordReport, CommitRecordTarget, CommitReport, Error, Features,
    FusedOnePutHolderAttempt, MakoTimestamp, Result, TestCleanupBoundary, TestCommitObserver,
    TestCommitPhase, WorkerHealth, MAX_KEY_BYTES, MAX_TABLE_NAME_BYTES, MAX_VALUE_BYTES,
    MAX_WORKERS, TRANSACTION_ITEM_BUDGET,
};

pub use db::{LocalDb, Table};
pub use engine::{
    advance_mako_timestamp_past, arm_test_cleanup_failure, clear_test_cleanup_failure,
    clear_test_commit_observer, features, install_test_commit_observer, quarantined_worker_count,
    worker_health, MakoStoEngine, MakoStoEngine as Engine,
};
pub use holder::{
    TrustedNativeOrderedArenaControl, TrustedNativeOrderedHolderControl, TrustedOnePutHolderPool,
    TrustedOnePutHolderView, TrustedSpscOnePutHolderControl,
};
pub use outcome::{
    TrustedNativeOrderedOnePutRecordOutcome, TrustedPreselectedUncheckedOnePutHolderOutcome,
    TrustedPreselectedUncheckedOnePutRecordOutcome, TrustedUncheckedOnePutRecordOutcome,
};
pub use txn::{Scan, Transaction};

/// Ownership result of one attempt at the fused one-put SPSC terminal.
pub type TrustedFusedOnePutHolderAttempt =
    FusedOnePutHolderAttempt<TrustedPreselectedUncheckedOnePutHolderOutcome>;
