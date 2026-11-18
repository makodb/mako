//! The engine error vocabulary shared by every local engine.
//!
//! Every variant names a contract outcome, not an implementation detail. The
//! `Abi*` variants describe a linkage or negotiation failure between a Rust
//! engine facade and the native library it drives; a pure-Rust engine never
//! produces them but callers may still match on them.

use std::fmt;

/// Failure reported by a local transaction engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Optimistic validation or write locking rejected the transaction.
    Conflict,
    /// The current worker was not attached to the engine runtime.
    NotAttached,
    /// A thread-affine transaction was used from another OS thread.
    WrongThread,
    /// This OS thread already has one active transaction.
    TransactionAlreadyActive,
    /// An operation targeted a transaction that has already ended.
    TransactionFinished,
    /// A table from a different local database was supplied.
    WrongDatabaseOrTable,
    /// The engine rejected an invalid pointer, length, or argument.
    InvalidArgument,
    /// The engine's process-lifetime worker slots are exhausted.
    ThreadLimit,
    /// A database or worker resource is still in use.
    Busy,
    /// An engine allocation, or a fallible allocation needed by a facade, failed.
    OutOfMemory,
    /// Engine state failed or violated an invariant. On commit, visibility may
    /// consequently be uncertain.
    Internal,
    /// Transaction cleanup could not be proved complete, so this worker is retired.
    WorkerPoisoned,
    /// The engine cannot compose two writes to one key (no read-your-writes).
    DuplicateWrite,
    /// The transaction exceeded an item or write-set limit and was aborted.
    TransactionTooLarge,
    /// The engine's OCC version space is exhausted; the runtime cannot admit
    /// more transactions. This is terminal, not a retryable transaction-size
    /// limit, and is separate from the upper-layer Mako timestamp.
    VersionExhausted,
    /// A key or value exceeded a representation limit.
    ValueTooLarge,
    /// The post-validation durability hook rejected the transaction before install.
    CommitHookRejected,
    /// Mako's process-wide representable timestamp space is exhausted.
    TimestampExhausted,
    /// A test-only or optional engine capability was not compiled in.
    FeatureUnavailable,
    /// A transactional scan needs the scan/read-your-writes engine feature.
    TransactionalScansUnavailable,
    /// A raw chunk scan's caller-owned arena could not hold its first result.
    ///
    /// Safe scan iterators grow their arena and retry internally, so ordinary
    /// safe callers should not observe this status.
    BufferTooSmall,
    /// Rust declarations and the linked native ABI have different versions.
    AbiMismatch {
        /// Version compiled into the Rust declarations.
        expected: u32,
        /// Version reported by the linked native library.
        found: u32,
    },
    /// An ABI structure has a different native and Rust layout.
    AbiLayoutMismatch {
        /// Structure whose layout did not match.
        structure: &'static str,
        /// Size compiled into the Rust facade.
        expected: usize,
        /// Size reported by the linked native library.
        found: usize,
    },
    /// The generated Rust status catalog and linked native catalog disagree.
    AbiStatusCatalogMismatch {
        /// Status whose canonical identity or diagnostic did not match.
        status: i32,
    },
    /// The linked implementation is not the expected engine family.
    AbiEngineMismatch,
    /// The linked native build returned a fingerprint with the wrong byte length.
    AbiBuildFingerprintSizeMismatch {
        /// Number of SHA-256 bytes required by the facade.
        expected: usize,
        /// Number of bytes reported by the native library.
        found: usize,
    },
    /// Current source/configuration and the linked native archive have different identities.
    AbiBuildFingerprintMismatch,
    /// The native library returned a status unknown to the facade.
    UnknownStatus(i32),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Conflict => write!(f, "local transaction conflict"),
            Self::NotAttached => write!(f, "worker thread is not attached to the engine"),
            Self::WrongThread => write!(f, "transaction used from a different OS thread"),
            Self::TransactionAlreadyActive => {
                write!(f, "this worker already has an active transaction")
            }
            Self::TransactionFinished => write!(f, "transaction has already finished"),
            Self::WrongDatabaseOrTable => write!(f, "table belongs to another database"),
            Self::InvalidArgument => write!(f, "invalid argument reached the engine"),
            Self::ThreadLimit => write!(
                f,
                "the engine's process-lifetime thread slots are exhausted; use a fixed worker pool"
            ),
            Self::Busy => write!(f, "engine resource is busy"),
            Self::OutOfMemory => write!(f, "engine or facade allocation failed"),
            Self::Internal => write!(f, "the engine reported an internal failure"),
            Self::WorkerPoisoned => write!(
                f,
                "transaction cleanup is uncertain and this worker is quarantined"
            ),
            Self::DuplicateWrite => write!(
                f,
                "the engine requires read-your-writes support to mutate one key twice"
            ),
            Self::TransactionTooLarge => {
                write!(f, "local transaction exceeded an engine size limit and was aborted")
            }
            Self::VersionExhausted => {
                write!(f, "transaction engine version space exhausted; retire this runtime")
            }
            Self::ValueTooLarge => write!(f, "key or value exceeded an engine size limit"),
            Self::CommitHookRejected => {
                write!(f, "post-validation commit hook rejected the transaction")
            }
            Self::TimestampExhausted => write!(f, "Mako hybrid logical clock exhausted"),
            Self::FeatureUnavailable => write!(f, "the requested engine feature is unavailable"),
            Self::TransactionalScansUnavailable => write!(
                f,
                "the engine does not support transactional scans with read-your-writes"
            ),
            Self::BufferTooSmall => write!(f, "transactional scan buffer is too small"),
            Self::AbiMismatch { expected, found } => write!(
                f,
                "engine ABI mismatch: Rust expects {expected}, linked native reports {found}"
            ),
            Self::AbiLayoutMismatch {
                structure,
                expected,
                found,
            } => write!(
                f,
                "engine ABI layout mismatch for {structure}: Rust expects {expected} bytes, linked native reports {found}"
            ),
            Self::AbiStatusCatalogMismatch { status } => {
                write!(f, "engine ABI status catalog mismatch at status {status}")
            }
            Self::AbiEngineMismatch => {
                write!(f, "linked native engine identity does not match the expected engine")
            }
            Self::AbiBuildFingerprintSizeMismatch { expected, found } => write!(
                f,
                "native build fingerprint has {found} bytes; expected {expected}"
            ),
            Self::AbiBuildFingerprintMismatch => write!(
                f,
                "native archive does not match current source, configuration, and toolchain"
            ),
            Self::UnknownStatus(status) => write!(f, "engine returned unknown status {status}"),
        }
    }
}

impl std::error::Error for Error {}

/// Result type shared by every engine facade.
pub type Result<T> = std::result::Result<T, Error>;
