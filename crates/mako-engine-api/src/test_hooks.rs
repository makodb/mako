//! Test-only observation points and health queries shared by engines.

use crate::timestamp::MakoTimestamp;

/// A test-only synchronous observation point in local commit.
///
/// Available at runtime only when the engine advertises
/// [`Features::TEST_COMMIT_OBSERVER`](crate::Features::TEST_COMMIT_OBSERVER).
/// The discriminants are part of the engine contract.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TestCommitPhase {
    /// The complete write set is locked; no Mako timestamp exists yet.
    WritesetLocked = 1,
    /// The checked nonzero Mako timestamp has been assigned after validation.
    MakoTimestampAllocated = 2,
    /// Every local validation has succeeded; no Mako timestamp exists yet.
    LocalValidationComplete = 3,
    /// The preinstall hook accepted, before the first write installation.
    PreinstallAccepted = 4,
    /// The first write was installed and at least one more write remains.
    FirstWriteInstalled = 5,
    /// Every local write was installed, before commit cleanup and return.
    AllWritesInstalled = 6,
}

impl TestCommitPhase {
    /// Decode an engine-reported phase code.
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            1 => Some(Self::WritesetLocked),
            2 => Some(Self::MakoTimestampAllocated),
            3 => Some(Self::LocalValidationComplete),
            4 => Some(Self::PreinstallAccepted),
            5 => Some(Self::FirstWriteInstalled),
            6 => Some(Self::AllWritesInstalled),
            _ => None,
        }
    }
}

/// Test-only plain function pointer invoked synchronously at commit seams.
///
/// The timestamp is absent at [`TestCommitPhase::WritesetLocked`] and
/// [`TestCommitPhase::LocalValidationComplete`], then present at allocation
/// and every later seam.
#[doc(hidden)]
pub type TestCommitObserver = fn(TestCommitPhase, Option<MakoTimestamp>);

/// Cleanup boundary at which a test may arm a one-shot failure.
///
/// The discriminants are part of the engine contract.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TestCleanupBoundary {
    /// Cleanup after transaction startup fails.
    Begin = 1,
    /// Cleanup after a point or scan operation fails.
    Operation = 2,
    /// Cleanup after commit fails.
    Commit = 3,
    /// Explicit or drop-driven abort cleanup fails.
    Abort = 4,
    /// Final facade destruction fails.
    Destroy = 5,
}

/// Health of the current OS worker as seen by the engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WorkerHealth {
    /// This worker has not been attached to the engine runtime.
    NotAttached,
    /// The worker is attached and remains safe for transactions.
    Healthy,
    /// Cleanup is uncertain and the worker must be retired permanently.
    Poisoned,
}
