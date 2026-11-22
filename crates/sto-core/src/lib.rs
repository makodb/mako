#![deny(unsafe_code)]

//! Native Rust implementation of Software Transactional Objects.
//!
//! The crate coordinates typed logical items and canonical physical locks;
//! datatype adapters supply observations, conflict coverage, deferred intents,
//! and commit callbacks. This crate is native Rust and is not translated into
//! C++.

pub mod direct_commit;
pub mod error;
pub mod hook;
pub mod identity;
pub mod legacy_tid;
pub mod lock;
pub mod object;
pub mod runtime;
pub mod transaction;
pub mod txn_array;
pub mod txn_cell;
pub mod txn_counter;
pub mod version;

#[cfg(feature = "test-install-observer")]
pub mod test_install;

mod terminal_read;
mod trans_item;

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
pub use lock::{
    AcquireContext, CleanupContext, ErasedLockUse, ExecutionCheckContext, InstallContext,
    LockContext, LockDisposition, LockRequest, LockUse, PredicateContext,
    PreflightFreeValidationContext, ReleaseContext, TransactionLock, ValidationContext,
};
pub use object::{
    CleanupDisposition, CleanupItem, CommitVersionRequirement, InstallItem, NoPredicate,
    ObservationOrder, ObservationRef, OpacityToken, PreflightFreeReadCapability,
    PreflightFreeReadCleanup, PreflightFreeReadValidate, PreflightItem, ResourceKey, TObject,
    TerminalReadBatchCapability, TerminalReadBatchValidate,
};
pub use runtime::{
    IsolationMode, ObjectRegistration, RegisteredResource, Runtime, RuntimeConfig, RuntimeHealth,
    WorkerContext,
};
pub use terminal_read::TerminalReadProxy;
pub use trans_item::TransProxy;
pub use transaction::{
    Active, ItemBatchControl, ItemBatchOutcome, Participant, ParticipantError, ParticipantPhase,
    ResolvedItemSession, TerminalReadOpen, TerminalReadReady, TerminalReadTransaction, Transaction,
    UniqueItemKeyIndex, UniqueItemKeys,
};
pub use txn_array::{ArrayBoundsError, TxnArray};
pub use txn_cell::{TxnCell, VersionLock};
pub use txn_counter::TxnCounter;
pub use version::{AtomicVersion, DetachedVersionGuard, VersionGuard, VersionLocked, VersionState};
