#![deny(unsafe_code)]

//! Native Rust implementation of Software Transactional Objects.
//!
//! The crate coordinates typed logical items and canonical physical locks;
//! datatype adapters supply observations, conflict coverage, deferred intents,
//! and commit callbacks. This crate is native Rust and is not translated into
//! C++.
//!
//! The default `distributed` feature preserves the participant and distributed
//! decision API. Disable default features for a local-only dependency closure.
//! [`LocalCommit`] retains the ordinary frame and locks across an external
//! durable decision without importing distributed time. Its storage operation
//! is fallible and transaction-wide; item install callbacks publish metadata
//! only after that operation succeeds.

pub mod object;
pub mod direct_commit;
pub mod error;
pub mod hook;
pub mod identity;
pub mod legacy_tid;
pub mod lock;
pub mod runtime;
pub mod transaction;
pub mod txn_array;
pub mod txn_cell;
pub mod txn_counter;
pub mod version;

#[cfg(feature = "test-install-observer")]
pub mod test_install;

mod trans_item;
mod terminal_read;

#[cfg(doctest)]
mod capability_contracts;

pub use object::{
    CommitVersionRequirement, CleanupDisposition, CleanupItem, InstallItem, NoPredicate,
    ObservationOrder, ObservationRef, OpacityToken, PreflightFreeReadCapability,
    PreflightFreeReadCleanup, PreflightFreeReadValidate, PreflightItem, ResourceKey,
    TerminalReadBatchCapability, TerminalReadBatchValidate, TObject,
};
pub use direct_commit::{
    BorrowedDirectPrepare, BorrowedInjectiveLockCommitCapability, BorrowedLockToken,
    BorrowedTokenCommitOperations, BorrowedUniqueLockCommitCapability, DirectBorrowedLockTarget,
    DirectCommitCapability, DirectInstall, DirectInstallContext, DirectLockMut, DirectLockRef,
    DirectPrepare, DirectTokenLock, DirectValidate, DirectValidationContext, DirectValidationItem,
    UniqueLockCommitCapability,
};
pub use error::*;
pub use hook::{CommitHook, CommitHookError};
pub use identity::*;
pub use trans_item::TransProxy;
pub use lock::{
    AcquireContext, ErasedLockUse, ExecutionCheckContext, CleanupContext, InstallContext,
    LockDisposition, LockRequest, LockUse, PredicateContext, LockContext,
    PreflightFreeValidationContext, ReleaseContext, TransactionLock, ValidationContext,
};
pub use runtime::{
    IsolationMode, ObjectRegistration, RegisteredResource, Runtime, RuntimeConfig, RuntimeHealth,
    WorkerContext,
};
pub use terminal_read::TerminalReadProxy;
pub use transaction::{
    Active, ItemBatchControl, ItemBatchOutcome,
    ResolvedItemSession, TerminalReadOpen, TerminalReadReady, TerminalReadTransaction, Transaction,
    UniqueItemKeyIndex, UniqueItemKeys,
};
#[cfg(feature = "distributed")]
pub use transaction::{Participant, ParticipantError, ParticipantPhase};
pub use transaction::{LocalApplyError, LocalCommit, LocalCommitError, LocalCommitPhase, SealedItem};
pub use txn_array::{ArrayBoundsError, TxnArray};
pub use txn_cell::{TxnCell, VersionLock};
pub use txn_counter::TxnCounter;
pub use version::{AtomicVersion, DetachedVersionGuard, VersionGuard, VersionLocked, VersionState};
