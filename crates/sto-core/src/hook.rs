//! Optional upper-layer coordination at the local commit boundary.

use crate::error::CapacityError;

/// A definite, pre-install rejection from an upper-layer commit hook.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum CommitHookError {
    /// The upper layer deliberately rejected the local commit.
    Rejected,
    /// Finite upper-layer metadata or bookkeeping was exhausted.
    Capacity(CapacityError),
}

impl std::fmt::Display for CommitHookError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for CommitHookError {}

impl From<CapacityError> for CommitHookError {
    fn from(value: CapacityError) -> Self {
        Self::Capacity(value)
    }
}

/// Optional upper-layer work performed within a writing transaction's commit.
///
/// The hook is borrowed only for the consuming [`crate::Transaction::commit_with_hook`]
/// call and can retain typed metadata in its own fields. `sto-core` never
/// interprets that metadata.
///
/// Both callbacks must be bounded, nonblocking, non-reentrant, and leave no
/// externally visible effect when they return an error or panic. They must use
/// preallocated bookkeeping: the core invokes them after transaction locks have
/// been acquired. A panic is contained, poisons the runtime, and still reports
/// a definite aborted outcome because of this stronger hook contract.
pub trait CommitHook {
    /// Reserves upper-layer metadata after the complete write lock set is held
    /// and before predicate upgrade and final read validation.
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError>;

    /// Accepts or rejects the commit after final validation and immediately
    /// before the core crosses its irreversible boundary.
    fn pre_install(&mut self) -> Result<(), CommitHookError>;
}
