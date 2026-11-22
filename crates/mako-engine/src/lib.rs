//! The local transaction engine the Mako cache is compiled against.
//!
//! `mako-cache` names every engine type through this crate. Which engine sits
//! behind those names is a cargo feature decided once per build:
//!
//! - `c-abi` (default): [`mako_local`], the safe facade over the C ABI to the
//!   C++ STO/MassTrans engine.
//! - `sto`: [`mako_engine_sto`], the native Rust STO engine over `sto-core`,
//!   `sto-masstree`, and `mako-clock`.
//!
//! Every backend implements the traits in [`mako_engine_api`], and every type
//! re-exported here carries the same name in every backend, so switching the
//! feature changes no cache source. Exactly one backend can be enabled: the
//! cache-order namespace and Mako clock are process-global.

#![forbid(unsafe_code)]

pub use mako_engine_api::*;

#[cfg(all(feature = "c-abi", not(feature = "sto")))]
mod backend {
    pub use mako_local::{
        advance_mako_timestamp_past, arm_test_cleanup_failure, clear_test_cleanup_failure,
        clear_test_commit_observer, features, install_test_commit_observer,
        quarantined_worker_count, worker_health, LocalDb, MakoLocalEngine as Engine, Scan, Table,
        Transaction, TrustedFusedOnePutHolderAttempt, TrustedNativeOrderedArenaControl,
        TrustedNativeOrderedHolderControl, TrustedNativeOrderedOnePutRecordOutcome,
        TrustedOnePutHolderPool, TrustedOnePutHolderView,
        TrustedPreselectedUncheckedOnePutHolderOutcome,
        TrustedPreselectedUncheckedOnePutRecordOutcome, TrustedSpscOnePutHolderControl,
        TrustedUncheckedOnePutRecordOutcome, UninitCommitRecord,
    };
}

#[cfg(all(feature = "sto", not(feature = "c-abi")))]
mod backend {
    pub use mako_engine_sto::{
        advance_mako_timestamp_past, arm_test_cleanup_failure, clear_test_cleanup_failure,
        clear_test_commit_observer, features, install_test_commit_observer,
        quarantined_worker_count, worker_health, LocalDb, MakoStoEngine as Engine, Scan, Table,
        Transaction, TrustedFusedOnePutHolderAttempt, TrustedNativeOrderedArenaControl,
        TrustedNativeOrderedHolderControl, TrustedNativeOrderedOnePutRecordOutcome,
        TrustedOnePutHolderPool, TrustedOnePutHolderView,
        TrustedPreselectedUncheckedOnePutHolderOutcome,
        TrustedPreselectedUncheckedOnePutRecordOutcome, TrustedSpscOnePutHolderControl,
        TrustedUncheckedOnePutRecordOutcome,
    };
}

#[cfg(all(feature = "c-abi", feature = "sto"))]
compile_error!(
    "mako-engine backends are mutually exclusive: build with --no-default-features --features sto"
);

#[cfg(not(any(feature = "c-abi", feature = "sto")))]
compile_error!("mako-engine requires exactly one engine backend feature");

pub use backend::*;
