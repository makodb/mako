#![deny(unsafe_code)]

//! Native Rust implementation of Software Transactional Objects.
//!
//! The crate coordinates typed logical items and canonical physical locks;
//! datatype adapters supply observations, conflict coverage, deferred intents,
//! and commit callbacks. This crate is native Rust and is not translated into
//! C++.

pub mod adapter;
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

mod item;
mod terminal_read;

pub use adapter::{
    FinishDisposition, FinishItem, InstallItem, NoPredicate, ObservationOrder, ObservationRef,
    OpacityToken, PreflightFreeReadCapability, PreflightFreeReadFinish, PreflightFreeReadValidate,
    PreflightItem, ResourceKey, TerminalReadBatchCapability, TerminalReadBatchValidate,
    TransactionalResource,
};
pub use error::*;
pub use hook::{CommitHook, CommitHookError};
pub use identity::*;
pub use item::Entry;
pub use lock::{
    AcquireContext, ExecutionCheckContext, FinishContext, InstallContext, LockDisposition,
    LockRequest, LockUse, PredicateContext, PreflightContext, PreflightFreeValidationContext,
    ReleaseContext, TransactionLock, ValidationContext,
};
pub use runtime::{
    IsolationMode, ObjectRegistration, RegisteredResource, Runtime, RuntimeConfig, RuntimeHealth,
    WorkerContext,
};
pub use terminal_read::TerminalReadEntry;
pub use transaction::{
    Active, ResolvedItemSession, TerminalReadOpen, TerminalReadReady, TerminalReadTransaction,
    Transaction, UniqueItemKeys,
};
pub use txn_array::{ArrayBoundsError, TxnArray};
pub use txn_cell::{TxnCell, VersionLock};
pub use txn_counter::TxnCounter;
pub use version::{AtomicVersion, DetachedVersionGuard, VersionGuard, VersionLocked, VersionState};
