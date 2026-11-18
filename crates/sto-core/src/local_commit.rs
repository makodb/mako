//! Worker-affine retained local commits with an explicit external decision.
//!
//! Native storage I/O belongs in `apply`, not in a commit hook or item install.
//! Item installation on this path publishes metadata only, after the complete
//! atomic backend batch has succeeded. No distributed timestamp is involved.

use super::*;
use crate::object::ObservationRef;

/// Immutable references into the authoritative core item. `visit_sealed`
/// guarantees final certification; `Transaction::visit_items` deliberately
/// provides the same shape during execution for read-your-writes scan merges.
pub struct SealedItem<'item, A: TObject> {
    pub key: &'item A::Key,
    pub local: &'item A::Local,
    pub observation: ObservationRef<'item, A>,
    pub intent: Option<&'item A::Intent>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LocalCommitPhase {
    Collecting,
    Locked,
    Certified,
    Submitted,
    Unknown,
    Committed,
    /// Backend application failed or unwound. Recovery must establish whether
    /// it applied; calling `apply` again is forbidden.
    ApplyFailed,
    Applied,
    Finished,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LocalCommitError {
    Aborted(AbortReason),
    Failure(CommitFailure),
    InvalidUse(InvalidUse),
    /// Storage was applied and the external decision is committed, but volatile
    /// publication or cleanup failed. This must never become an aborted reply.
    CommittedPublication(CommitFailure),
}

impl std::fmt::Display for LocalCommitError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for LocalCommitError {}

/// A typed I/O error is separate from core certification/publication failures.
#[derive(Debug)]
pub enum LocalApplyError<E> {
    Storage(E),
    Core(LocalCommitError),
}

impl<E: std::fmt::Display> std::fmt::Display for LocalApplyError<E> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Storage(error) => write!(f, "committed storage application failed: {error}"),
            Self::Core(error) => write!(f, "{error}"),
        }
    }
}
impl<E: std::error::Error + 'static> std::error::Error for LocalApplyError<E> {}

/// Owns the original frame and exclusively borrows its worker until resolved.
/// Like `Transaction`, this object is neither Send nor Sync. Unresolved drop
/// fences the runtime and quarantines the frame AND guards; it cannot run abort
/// release callbacks after an append may have become externally visible.
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<sto_core::LocalCommit<'static>>();
/// ```
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<sto_core::LocalCommit<'static>>();
/// ```
#[must_use = "a retained commit must be explicitly resolved"]
pub struct LocalCommit<'worker> {
    driver: CommitDriver<'worker, 'static>,
    phase: LocalCommitPhase,
    terminal: Option<Result<CommitOutcome, LocalCommitError>>,
}

impl<'worker> LocalCommit<'worker> {
    pub(super) fn new(worker: &'worker mut WorkerContext, frame: Box<TransactionFrame>) -> Self {
        Self {
            driver: CommitDriver::new(worker, frame, None),
            phase: LocalCommitPhase::Collecting,
            terminal: None,
        }
    }

    pub fn phase(&self) -> LocalCommitPhase {
        self.phase
    }

    pub fn has_writes(&self) -> bool {
        self.driver.frame.as_ref().is_some_and(|frame| frame.commit_shape().has_writes)
    }

    /// Acquire the ordinary canonical lock plan outside the certification gate.
    /// Execution cannot resume after conversion into this retained owner.
    pub fn lock_writes(&mut self) -> Result<(), LocalCommitError> {
        self.require_phase(LocalCommitPhase::Collecting)?;
        self.run_phase(|driver| {
            let health = driver.worker.runtime.ensure_healthy(FailurePhase::Preflight);
            if driver.frame().doomed || health.is_err() {
                return Err(driver.abort_commit(AbortReason::Doomed, health.err()));
            }
            // Unlike incremental participants, execution cannot resume here.
            // Preserve the homogeneous batch instead of allocating erased items.
            driver.lock_ordinary(false)
        })?;
        self.phase = LocalCommitPhase::Locked;
        Ok(())
    }

    /// Final predicate upgrade and read validation through the ordinary driver.
    /// The caller orders this and local proposal admission under its shard gate;
    /// that gate must not remain held while waiting for an external decision.
    pub fn certify(&mut self) -> Result<(), LocalCommitError> {
        self.require_phase(LocalCommitPhase::Locked)?;
        self.run_phase(|driver| {
            if let Err(info) = driver.worker.runtime.ensure_healthy(FailurePhase::Validation) {
                return Err(driver.abort_commit(AbortReason::Doomed, Some(info)));
            }
            driver.certify_ordinary()
        })?;
        self.phase = LocalCommitPhase::Certified;
        Ok(())
    }

    /// Inspect exact resource bindings, never a secondary read/write set.
    /// Read-only items are included with `intent = None`.
    /// Item references may outlive the visitor call, but not this immutable
    /// owner borrow. Callers can sort borrowed intents and encode once without
    /// copying payloads; no lifecycle transition is possible until they release
    /// the borrow.
    pub fn visit_sealed<'item, A: TObject>(
        &'item self,
        resource: &RegisteredResource<A>,
        visitor: impl FnMut(SealedItem<'item, A>),
    ) -> Result<(), LocalCommitError> {
        if !matches!(self.phase, LocalCommitPhase::Certified | LocalCommitPhase::Submitted
            | LocalCommitPhase::Unknown | LocalCommitPhase::Committed | LocalCommitPhase::ApplyFailed)
        {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        self.driver.frame().visit_items(resource, visitor);
        Ok(())
    }

    /// MUST precede every externally visible proposal/append call, including
    /// calls that may unwind or lose their reply before reporting acceptance.
    pub fn mark_submitted(&mut self) -> Result<(), LocalCommitError> {
        self.require_phase(LocalCommitPhase::Certified)?;
        self.driver.boundary = CommitBoundary::Irrevocable;
        self.phase = LocalCommitPhase::Submitted;
        Ok(())
    }

    pub fn mark_unknown(&mut self) -> Result<(), LocalCommitError> {
        if !matches!(self.phase, LocalCommitPhase::Submitted | LocalCommitPhase::Unknown) {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        self.phase = LocalCommitPhase::Unknown;
        Ok(())
    }

    pub fn mark_committed(&mut self) -> Result<(), LocalCommitError> {
        if !matches!(self.phase, LocalCommitPhase::Submitted | LocalCommitPhase::Unknown
            | LocalCommitPhase::Committed)
        {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        self.phase = LocalCommitPhase::Committed;
        Ok(())
    }

    /// Explicit authoritative rejection/discard, not timeout, disconnect, or
    /// leader loss. A committed decision can never be rejected.
    pub fn reject(&mut self) -> Result<CommitOutcome, LocalCommitError> {
        if !matches!(self.phase, LocalCommitPhase::Collecting | LocalCommitPhase::Locked
            | LocalCommitPhase::Certified | LocalCommitPhase::Submitted | LocalCommitPhase::Unknown)
        {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        self.driver.boundary = CommitBoundary::Reversible;
        let result = catch_unwind(AssertUnwindSafe(|| {
            self.driver.abort_commit(AbortReason::Explicit, None)
        })).unwrap_or_else(|_| contain_driver_unwind(&mut self.driver));
        self.record_terminal(result.map_err(LocalCommitError::Failure), LocalCommitPhase::Finished)
    }

    /// Abort only before possible submission. Afterward use `reject` solely
    /// with a definite external rejection, or retain the attempt for resolution.
    pub fn abort(&mut self) -> Result<CommitOutcome, LocalCommitError> {
        if !matches!(self.phase, LocalCommitPhase::Collecting | LocalCommitPhase::Locked
            | LocalCommitPhase::Certified)
        {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        self.reject()
    }

    /// Record an independently reported storage failure and fence new work.
    /// The original frame and locks remain retained for authoritative recovery.
    pub fn mark_apply_failed(&mut self) -> Result<(), LocalCommitError> {
        self.require_phase(LocalCommitPhase::Committed)?;
        self.phase = LocalCommitPhase::ApplyFailed;
        self.driver.worker.runtime.mark_indeterminate();
        Ok(())
    }

    /// Run ONE fallible whole-transaction atomic storage operation outside all
    /// bounded core callbacks. An error may mean an ambiguous native write:
    /// retain the guards, fence admission, and require recovery, never retry.
    pub fn apply<E>(
        &mut self,
        whole_batch: impl FnOnce() -> Result<(), E>,
    ) -> Result<CommitOutcome, LocalApplyError<E>> {
        self.require_phase(LocalCommitPhase::Committed).map_err(LocalApplyError::Core)?;
        // Also block retries if the callback unwinds and its caller catches it.
        self.phase = LocalCommitPhase::ApplyFailed;
        match catch_unwind(AssertUnwindSafe(whole_batch)) {
            Ok(Ok(())) => self.complete_applied().map_err(LocalApplyError::Core),
            Ok(Err(error)) => {
                self.driver.worker.runtime.mark_indeterminate();
                Err(LocalApplyError::Storage(error))
            }
            Err(payload) => {
                self.driver.worker.runtime.mark_indeterminate();
                std::panic::resume_unwind(payload)
            }
        }
    }

    /// Attest successful complete atomic backend application (including a
    /// recovery-confirmed ambiguous write), then publish volatile metadata.
    /// Adapter `TObject::install` MUST perform metadata publication only here.
    /// All physical guards remain held until every item has been published.
    /// Repeated completion returns the recorded result without invoking callbacks.
    pub fn complete_applied(&mut self) -> Result<CommitOutcome, LocalCommitError> {
        if self.phase == LocalCommitPhase::Applied {
            return self.terminal.expect("applied result retained");
        }
        if !matches!(self.phase, LocalCommitPhase::Committed | LocalCommitPhase::ApplyFailed) {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        let result = catch_unwind(AssertUnwindSafe(|| {
            match self.driver.install_ordinary(
                #[cfg(feature = "distributed")]
                None,
                true,
            ) {
                Ok(()) => self.driver.finish_ordinary(),
                Err(result) => result,
            }
        })).unwrap_or_else(|_| contain_driver_unwind(&mut self.driver));
        self.record_terminal(result.map_err(LocalCommitError::CommittedPublication), LocalCommitPhase::Applied)
    }

    /// Finish a certified read-only attempt after the caller's fresh read
    /// barrier. No write log or synthetic external decision is necessary.
    pub fn complete_read_only(&mut self) -> Result<CommitOutcome, LocalCommitError> {
        self.require_phase(LocalCommitPhase::Certified)?;
        if self.has_writes() {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        self.driver.boundary = CommitBoundary::Irrevocable;
        let result = catch_unwind(AssertUnwindSafe(|| self.driver.finish_ordinary()))
            .unwrap_or_else(|_| contain_driver_unwind(&mut self.driver));
        self.record_terminal(result.map_err(LocalCommitError::Failure), LocalCommitPhase::Applied)
    }

    fn require_phase(&self, phase: LocalCommitPhase) -> Result<(), LocalCommitError> {
        if self.phase != phase {
            return Err(LocalCommitError::InvalidUse(InvalidUse::IllegalItemState));
        }
        Ok(())
    }

    fn record_terminal(
        &mut self,
        result: Result<CommitOutcome, LocalCommitError>,
        phase: LocalCommitPhase,
    ) -> Result<CommitOutcome, LocalCommitError> {
        self.phase = phase;
        self.terminal = Some(result);
        result
    }

    fn run_phase(
        &mut self,
        operation: impl FnOnce(&mut CommitDriver<'worker, 'static>) -> DriverStep,
    ) -> Result<(), LocalCommitError> {
        let result = catch_unwind(AssertUnwindSafe(|| operation(&mut self.driver)))
            .unwrap_or_else(|_| Err(contain_driver_unwind(&mut self.driver)));
        match result {
            Ok(()) => Ok(()),
            Err(terminal) => {
                let error = match terminal {
                    Ok(CommitOutcome::Aborted(reason)) => LocalCommitError::Aborted(reason),
                    Err(error) => LocalCommitError::Failure(error),
                    Ok(CommitOutcome::Committed(_)) => unreachable!("preparation cannot commit"),
                };
                let _ = self.record_terminal(Err(error), LocalCommitPhase::Finished);
                Err(error)
            }
        }
    }
}

impl Drop for LocalCommit<'_> {
    fn drop(&mut self) {
        if self.terminal.is_some() {
            return;
        }
        if matches!(self.phase, LocalCommitPhase::Collecting | LocalCommitPhase::Locked
            | LocalCommitPhase::Certified)
        {
            let _ = self.abort();
        } else {
            // Unlike ordinary indeterminate callback recovery, an unresolved
            // external decision must retain the actual reservations. Forgetting
            // both objects prevents adapter destructors from restoring versions.
            self.driver.worker.runtime.mark_indeterminate();
            if let Some(locks) = self.driver.locks.take() {
                std::mem::forget(locks);
            }
            self.driver.quarantine_items();
            self.driver.complete_worker();
        }
    }
}
