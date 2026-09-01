//! Public error, abort-outcome, and failure-disposition taxonomy.

use std::fmt;

use crate::identity::{OccCommitId, OccVersion, OwnerId};

macro_rules! debug_display_error {
    ($($ty:ty),+ $(,)?) => {
        $(
            impl fmt::Display for $ty {
                fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    write!(f, "{self:?}")
                }
            }

            impl std::error::Error for $ty {}
        )+
    };
}

/// A normal retryable conflict.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Conflict {
    /// A planned physical lock was already held.
    LockBusy,
    /// A recorded read no longer validates.
    ReadValidation,
    /// A recorded semantic predicate no longer holds.
    PredicateValidation,
    /// Execution-time opacity revalidation failed.
    Opacity,
    /// A bounded adapter-internal acquisition could not complete.
    HiddenLockBusy,
}

/// A checked finite resource was exhausted.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CapacityError {
    RuntimeIdExhausted,
    ObjectIdExhausted,
    OwnerIdExhausted,
    WorkerLimit,
    ItemLimit,
    LockLimit,
    VersionExhausted,
    BufferLimit,
    KeyLimit,
}

/// An operation was attempted with an incompatible handle or lifecycle state.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum InvalidUse {
    WrongRuntime,
    WrongThread,
    WorkerBusy,
    TransactionFinished,
    TransactionDoomed,
    /// A direct unique group cannot start or append in the current item
    /// representation. The historical name also covers a repeated resource
    /// binding or a differently typed/ordinary active frame.
    UniqueBatchRequiresEmptyTransaction,
    DuplicateResourceClass,
    ResourceTypeMismatch,
    IllegalItemState,
    StaleLockUse,
    LockIdentityMismatch,
}

/// A requested feature is not implemented or negotiated.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Unsupported {
    IsolationMode,
    Capability(&'static str),
}

/// Adapter callback phase in which a semantic contract violation occurred.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AdapterPhase {
    ItemInit,
    Execute,
    Preflight,
    Acquire,
    ExecutionCheck,
    PredicateUpgrade,
    Validation,
    Install,
    Release,
    Finish,
}

/// Stable classification of an adapter contract fault.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AdapterFaultKind {
    InvariantViolation,
    TypeMismatch,
    LockIdentityMismatch,
    StaleLockUse,
    Panic,
    Other(&'static str),
}

/// A non-retryable adapter contract violation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct AdapterFault {
    phase: AdapterPhase,
    kind: AdapterFaultKind,
}

impl AdapterFault {
    pub const fn new(phase: AdapterPhase, kind: AdapterFaultKind) -> Self {
        Self { phase, kind }
    }

    pub const fn invariant(phase: AdapterPhase) -> Self {
        Self::new(phase, AdapterFaultKind::InvariantViolation)
    }

    pub const fn phase(&self) -> AdapterPhase {
        self.phase
    }

    pub const fn kind(&self) -> &AdapterFaultKind {
        &self.kind
    }
}

/// Phase used to classify a core failure or quarantine event.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FailurePhase {
    Runtime,
    Attach,
    Begin,
    Registration,
    Execution,
    Preflight,
    Acquire,
    PredicateUpgrade,
    UpperMetadata,
    CommitMetadata,
    Validation,
    PreinstallHook,
    Install,
    Release,
    Finish,
    WorkerReset,
}

/// A pre-irrevocable internal failure whose abort cleanup completed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct InternalError {
    phase: FailurePhase,
    reason: &'static str,
}

impl InternalError {
    pub const fn new(phase: FailurePhase, reason: &'static str) -> Self {
        Self { phase, reason }
    }

    pub const fn phase(&self) -> FailurePhase {
        self.phase
    }

    pub const fn reason(&self) -> &'static str {
        self.reason
    }
}

/// Why a runtime was poisoned even though the transaction outcome is known.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct PoisonInfo {
    phase: FailurePhase,
    reason: &'static str,
}

impl PoisonInfo {
    pub const fn new(phase: FailurePhase, reason: &'static str) -> Self {
        Self { phase, reason }
    }

    pub const fn phase(&self) -> FailurePhase {
        self.phase
    }

    pub const fn reason(&self) -> &'static str {
        self.reason
    }
}

/// Diagnostic information for publication whose outcome cannot be classified.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct IndeterminateInfo {
    phase: FailurePhase,
    occ_commit_id: Option<OccCommitId>,
    reason: &'static str,
}

impl IndeterminateInfo {
    pub const fn new(
        phase: FailurePhase,
        occ_commit_id: Option<OccCommitId>,
        reason: &'static str,
    ) -> Self {
        Self {
            phase,
            occ_commit_id,
            reason,
        }
    }

    pub const fn phase(&self) -> FailurePhase {
        self.phase
    }

    pub const fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }

    pub const fn reason(&self) -> &'static str {
        self.reason
    }
}

/// Runtime-construction failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RuntimeError {
    Capacity(CapacityError),
    InvalidUse(InvalidUse),
    Unsupported(Unsupported),
    Poisoned(PoisonInfo),
    Internal(InternalError),
}

/// Worker-attachment failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AttachError {
    Capacity(CapacityError),
    InvalidUse(InvalidUse),
    Poisoned(PoisonInfo),
}

/// Transaction-begin failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BeginError {
    Capacity(CapacityError),
    InvalidUse(InvalidUse),
    Unsupported(Unsupported),
    Poisoned(PoisonInfo),
}

/// Object or resource registration failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RegistrationError {
    Capacity(CapacityError),
    InvalidUse(InvalidUse),
    Poisoned(PoisonInfo),
}

/// Failure of an adapter-author operation inside `Transaction::with_item`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AccessError {
    Conflict(Conflict),
    Capacity(CapacityError),
    InvalidUse(InvalidUse),
    Unsupported(Unsupported),
    Fault(AdapterFault),
    Poisoned(PoisonInfo),
    Internal(InternalError),
}

/// Failure to construct transaction-local state for a new item.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ItemInitError {
    Capacity(CapacityError),
    Fault(AdapterFault),
}

/// Fallible result of an item's preflight callback.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum PrepareError {
    Conflict(Conflict),
    Capacity(CapacityError),
    Fault(AdapterFault),
}

/// Fallible result of bounded physical-lock acquisition.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AcquireError {
    Conflict(Conflict),
    Fault(AdapterFault),
}

/// Fallible result of read, predicate, or opacity validation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CheckError {
    Conflict(Conflict),
    Fault(AdapterFault),
}

/// Definite reason why a transaction did not commit.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AbortReason {
    Explicit,
    Doomed,
    Conflict(Conflict),
    Capacity(CapacityError),
    InvalidUse(InvalidUse),
    Unsupported(Unsupported),
    HookRejected,
    Internal(InternalError),
}

/// Core OCC metadata returned for a committed transaction.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CommitInfo {
    occ_commit_id: Option<OccCommitId>,
}

impl CommitInfo {
    pub const fn new(occ_commit_id: Option<OccCommitId>) -> Self {
        Self { occ_commit_id }
    }

    pub const fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }
}

/// Result of explicit transaction abort after exact-once cleanup.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct AbortInfo {
    reason: AbortReason,
}

impl AbortInfo {
    pub const fn new(reason: AbortReason) -> Self {
        Self { reason }
    }

    pub const fn reason(&self) -> &AbortReason {
        &self.reason
    }
}

/// Normal, definite result of a commit attempt.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CommitOutcome {
    Committed(CommitInfo),
    Aborted(AbortReason),
}

/// Transaction outcome retained by a poisoned runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DefiniteOutcome {
    Committed(CommitInfo),
    Aborted(AbortReason),
}

/// Exceptional failure of a consuming commit operation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CommitFailure {
    Poisoned {
        outcome: DefiniteOutcome,
        info: PoisonInfo,
    },
    Indeterminate(IndeterminateInfo),
}

/// Native version-state failure. A release error leaves the version locked so
/// the caller can quarantine the owning guard rather than publish unsafely.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum VersionError {
    GenerationExhausted(OccVersion),
    CommitVersionNotNewer {
        current: OccVersion,
        proposed: OccCommitId,
    },
    GuardAlreadyReleased,
    GuardTargetMismatch,
    LostOwnership {
        expected_owner: OwnerId,
    },
}

debug_display_error!(
    Conflict,
    CapacityError,
    InvalidUse,
    Unsupported,
    AdapterFault,
    InternalError,
    RuntimeError,
    AttachError,
    BeginError,
    RegistrationError,
    AccessError,
    ItemInitError,
    PrepareError,
    AcquireError,
    CheckError,
    AbortReason,
    CommitFailure,
    VersionError,
);

macro_rules! from_variant {
    ($source:ty => $target:ty, $variant:ident) => {
        impl From<$source> for $target {
            fn from(value: $source) -> Self {
                Self::$variant(value)
            }
        }
    };
}

from_variant!(CapacityError => RuntimeError, Capacity);
from_variant!(InvalidUse => RuntimeError, InvalidUse);
from_variant!(Unsupported => RuntimeError, Unsupported);
from_variant!(PoisonInfo => RuntimeError, Poisoned);
from_variant!(InternalError => RuntimeError, Internal);

from_variant!(CapacityError => AttachError, Capacity);
from_variant!(InvalidUse => AttachError, InvalidUse);
from_variant!(PoisonInfo => AttachError, Poisoned);

from_variant!(CapacityError => BeginError, Capacity);
from_variant!(InvalidUse => BeginError, InvalidUse);
from_variant!(Unsupported => BeginError, Unsupported);
from_variant!(PoisonInfo => BeginError, Poisoned);

from_variant!(CapacityError => RegistrationError, Capacity);
from_variant!(InvalidUse => RegistrationError, InvalidUse);
from_variant!(PoisonInfo => RegistrationError, Poisoned);

from_variant!(Conflict => AccessError, Conflict);
from_variant!(CapacityError => AccessError, Capacity);
from_variant!(InvalidUse => AccessError, InvalidUse);
from_variant!(Unsupported => AccessError, Unsupported);
from_variant!(AdapterFault => AccessError, Fault);
from_variant!(PoisonInfo => AccessError, Poisoned);
from_variant!(InternalError => AccessError, Internal);

from_variant!(CapacityError => ItemInitError, Capacity);
from_variant!(AdapterFault => ItemInitError, Fault);

from_variant!(Conflict => PrepareError, Conflict);
from_variant!(CapacityError => PrepareError, Capacity);
from_variant!(AdapterFault => PrepareError, Fault);

from_variant!(Conflict => AcquireError, Conflict);
from_variant!(AdapterFault => AcquireError, Fault);

from_variant!(Conflict => CheckError, Conflict);
from_variant!(AdapterFault => CheckError, Fault);

from_variant!(Conflict => AbortReason, Conflict);
from_variant!(CapacityError => AbortReason, Capacity);
from_variant!(InvalidUse => AbortReason, InvalidUse);
from_variant!(Unsupported => AbortReason, Unsupported);
from_variant!(InternalError => AbortReason, Internal);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn callback_conversions_preserve_fault_and_capacity_classes() {
        let fault = AdapterFault::new(
            AdapterPhase::Preflight,
            AdapterFaultKind::LockIdentityMismatch,
        );
        assert_eq!(PrepareError::from(fault), PrepareError::Fault(fault));
        assert_eq!(AcquireError::from(fault), AcquireError::Fault(fault));
        assert_eq!(CheckError::from(fault), CheckError::Fault(fault));

        let capacity = CapacityError::LockLimit;
        assert_eq!(
            PrepareError::from(capacity),
            PrepareError::Capacity(capacity)
        );
        assert_eq!(AccessError::from(capacity), AccessError::Capacity(capacity));
    }

    #[test]
    fn poisoned_failure_always_carries_a_definite_outcome() {
        let abort = AbortReason::Conflict(Conflict::ReadValidation);
        let failure = CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Aborted(abort),
            info: PoisonInfo::new(FailurePhase::Finish, "cleanup callback panicked"),
        };
        assert!(matches!(
            failure,
            CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation)),
                ..
            }
        ));
    }

    #[test]
    fn indeterminate_failure_is_a_separate_non_retryable_shape() {
        let commit_id = OccCommitId::new(11).unwrap();
        let failure = CommitFailure::Indeterminate(IndeterminateInfo::new(
            FailurePhase::Install,
            Some(commit_id),
            "install callback panicked",
        ));
        assert!(matches!(failure, CommitFailure::Indeterminate(_)));
    }
}
