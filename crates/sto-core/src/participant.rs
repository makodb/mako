//! A retained distributed participant using the ordinary transaction frame.

use super::*;
use mako_clock::distributed::DistributedTimeTerm;

/// Execution is mutable only before final certification.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParticipantPhase {
    Collecting,
    Validated,
    Installed,
    Finished,
}

/// A phase refusal preserves the distinction between abort and uncertain publication.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParticipantError {
    Aborted(AbortReason),
    Failure(CommitFailure),
    InvalidUse(InvalidUse),
}

impl std::fmt::Display for ParticipantError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for ParticipantError {}

/// Worker-affine ownership of an executing, certified, or installed transaction.
///
/// Incremental locking and ordinary operations share the same typed items.
/// INSTALL is irreversible; dropping or aborting an installed participant
/// completes committed publication rather than restoring old generations.
pub struct Participant<'worker> {
    driver: CommitDriver<'worker, 'static>,
    phase: ParticipantPhase,
    terminal: Option<Result<CommitOutcome, CommitFailure>>,
}

impl<'worker> Participant<'worker> {
    pub(super) fn new(worker: &'worker mut WorkerContext, frame: Box<TransactionFrame>) -> Self {
        Self {
            driver: CommitDriver::new(worker, frame, None),
            phase: ParticipantPhase::Collecting,
            terminal: None,
        }
    }

    pub fn phase(&self) -> ParticipantPhase {
        self.phase
    }

    pub fn is_empty(&self) -> bool {
        self.driver
            .frame
            .as_ref()
            .is_none_or(|frame| frame.item_count == 0)
    }

    pub fn has_writes(&self) -> bool {
        self.driver
            .frame
            .as_ref()
            .is_some_and(|frame| frame.commit_shape().has_writes)
    }

    /// Borrows the real transaction for one operation without exposing ownership
    /// of the retained worker or permitting an operation borrow to escape.
    pub fn with_transaction<R>(
        &mut self,
        operation: impl for<'op> FnOnce(&mut Transaction<'op, Active>) -> Result<R, AccessError>,
    ) -> Result<R, AccessError> {
        if self.phase != ParticipantPhase::Collecting {
            return Err(InvalidUse::TransactionFinished.into());
        }
        let mut transaction = Transaction {
            worker: Some(&mut *self.driver.worker),
            frame: Some(TransactionFrameRef::Borrowed(
                self.driver.frame.as_deref_mut().expect("collecting frame"),
            )),
            state: PhantomData,
            not_send_sync: PhantomData,
        };
        // The operation owns only a frame borrow. A caller that replaces this
        // view can neither commit nor drop the retained participant frame.
        let result = catch_unwind(AssertUnwindSafe(|| operation(&mut transaction)));
        let intact = matches!(
            transaction.frame.as_ref(),
            Some(TransactionFrameRef::Borrowed(_))
        );
        if intact {
            let _ = transaction.frame.take();
            let _ = transaction.worker.take();
        }
        drop(transaction);
        if !intact {
            self.driver.frame.as_mut().expect("collecting frame").doomed = true;
        }
        match result {
            Ok(result) => {
                if result.is_err() {
                    self.driver.frame.as_mut().expect("collecting frame").doomed = true;
                }
                if intact {
                    result
                } else {
                    Err(InvalidUse::IllegalItemState.into())
                }
            }
            Err(payload) => {
                self.driver.frame.as_mut().expect("collecting frame").doomed = true;
                std::panic::resume_unwind(payload)
            }
        }
    }

    /// Acquires newly staged writes now, retaining all earlier acquisitions.
    /// Existing item preparation is refreshed from its latest intent.
    pub fn lock_writes(&mut self) -> Result<(), ParticipantError> {
        self.require_phase(ParticipantPhase::Collecting)?;
        self.run_phase(|driver| driver.lock_ordinary(true))
    }

    /// Acquires remaining writes and seals execution with final certification.
    /// Required local OCC metadata is reserved before INSTALL can be admitted.
    pub fn validate(&mut self) -> Result<(), ParticipantError> {
        self.require_phase(ParticipantPhase::Collecting)?;
        self.run_phase(|driver| {
            driver.lock_ordinary(true)?;
            driver.certify_ordinary()
        })?;
        self.phase = ParticipantPhase::Validated;
        Ok(())
    }

    /// Adopts the external decision, without allocating a distributed timestamp.
    /// Physical locks remain retained until finish, including early-publishing
    /// datatypes that would release their exclusive lock in a local commit.
    pub fn install(&mut self, decision: DistributedTimeTerm) -> Result<(), ParticipantError> {
        self.require_phase(ParticipantPhase::Validated)?;
        self.run_phase(|driver| driver.install_ordinary(Some(decision), true))?;
        self.phase = ParticipantPhase::Installed;
        Ok(())
    }

    /// Finishes a decision. After INSTALL, `committed = false` cannot roll back.
    /// A read-only validated participant can finish committed without INSTALL.
    pub fn finish(mut self, committed: bool) -> Result<CommitOutcome, CommitFailure> {
        self.finish_inner(committed)
    }

    pub fn abort(self) -> Result<CommitOutcome, CommitFailure> {
        self.finish(false)
    }

    fn require_phase(&self, phase: ParticipantPhase) -> Result<(), ParticipantError> {
        if let Some(result) = self.terminal {
            return Err(match result {
                Ok(CommitOutcome::Aborted(reason)) => ParticipantError::Aborted(reason),
                Err(error) => ParticipantError::Failure(error),
                Ok(CommitOutcome::Committed(_)) => {
                    ParticipantError::InvalidUse(InvalidUse::TransactionFinished)
                }
            });
        }
        if self.phase != phase {
            return Err(ParticipantError::InvalidUse(
                InvalidUse::TransactionFinished,
            ));
        }
        Ok(())
    }

    fn run_phase(
        &mut self,
        operation: impl FnOnce(&mut CommitDriver<'worker, 'static>) -> DriverStep,
    ) -> Result<(), ParticipantError> {
        let result = match catch_unwind(AssertUnwindSafe(|| operation(&mut self.driver))) {
            Ok(result) => result,
            Err(_) => Err(contain_driver_unwind(&mut self.driver)),
        };
        match result {
            Ok(()) => Ok(()),
            Err(terminal) => {
                self.phase = ParticipantPhase::Finished;
                self.terminal = Some(terminal);
                Err(match terminal {
                    Ok(CommitOutcome::Aborted(reason)) => ParticipantError::Aborted(reason),
                    Err(error) => ParticipantError::Failure(error),
                    Ok(CommitOutcome::Committed(_)) => unreachable!("phase failure cannot commit"),
                })
            }
        }
    }

    fn finish_inner(&mut self, committed: bool) -> Result<CommitOutcome, CommitFailure> {
        if let Some(result) = self.terminal {
            return result;
        }
        let result = catch_unwind(AssertUnwindSafe(|| {
            if self.phase == ParticipantPhase::Installed
                || (committed && self.phase == ParticipantPhase::Validated && !self.has_writes())
            {
                self.driver.finish_ordinary()
            } else {
                let reason = if committed {
                    AbortReason::InvalidUse(InvalidUse::IllegalItemState)
                } else {
                    AbortReason::Explicit
                };
                self.driver.abort_commit(reason, None)
            }
        }));
        let result = result.unwrap_or_else(|_| contain_driver_unwind(&mut self.driver));
        self.phase = ParticipantPhase::Finished;
        self.terminal = Some(result);
        result
    }
}

impl Drop for Participant<'_> {
    fn drop(&mut self) {
        if self.terminal.is_none() {
            let _ = self.finish_inner(false);
        }
    }
}
