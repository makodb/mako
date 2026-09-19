//! Shared direct-commit phases for concrete and erased batch dispatch.

use super::{CommitBoundary, ErasedItemBatch};
use crate::{
    adapter::TransactionalResource, direct_commit::ErasedDirectCommitPlan, item::ItemData,
    AcquireError, AdapterFault, CapacityError, CheckError, CommitHook, CommitHookError,
    FailurePhase, LockDisposition, OccCommitId, OwnerId, PrepareError, Runtime,
};
use std::{
    any::Any,
    panic::{catch_unwind, AssertUnwindSafe},
};

/// These are disjoint borrows of the existing commit owner's state. Neither
/// this context nor a callback owns the frame, plan, items, or write guards.
/// A failed phase returns to that same owner for ordinary abort/quarantine.
pub(crate) struct DirectCommitContext<'state, 'hook> {
    pub(super) runtime: &'state Runtime,
    pub(super) owner: OwnerId,
    pub(super) phase: &'state mut FailurePhase,
    pub(super) boundary: &'state mut CommitBoundary,
    pub(super) commit_id: &'state mut Option<OccCommitId>,
    pub(super) hook: &'state mut Option<&'hook mut dyn CommitHook>,
}

pub(crate) enum DirectCommitStepFailure {
    Prepare(PrepareError),
    Acquire(AcquireError),
    CommitMetadata(CapacityError),
    Validation(CheckError),
    Hook(CommitHookError),
    // Keep the caught payload alive until the owner has computed its cleanup
    // outcome. Its destructor may itself panic, just as it could when the
    // original driver's named catch result left scope after cleanup.
    Panicked(FailurePhase, Box<dyn Any + Send>),
    Release,
    ReleasedPlanTeardown,
}

/// The already-prepared part of the protocol. The ordinary batch delegates
/// through its retained plan; a singleton enters that concrete plan once.
/// Neither implementation owns or removes any guard or adapter state.
pub(crate) trait DirectCommitBody {
    fn acquire(&mut self, owner: OwnerId) -> Result<(), AcquireError>;
    fn validate(&self, commit_id: Option<OccCommitId>) -> Result<(), CheckError>;
    fn install(&mut self, commit_id: Option<OccCommitId>);
    fn release(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault>;
}

impl<B: ErasedItemBatch + ?Sized> DirectCommitBody for B {
    fn acquire(&mut self, owner: OwnerId) -> Result<(), AcquireError> {
        self.direct_acquire_all(owner)
    }

    fn validate(&self, commit_id: Option<OccCommitId>) -> Result<(), CheckError> {
        self.direct_validate(commit_id)
    }

    fn install(&mut self, commit_id: Option<OccCommitId>) {
        self.direct_install(commit_id);
    }

    fn release(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault> {
        self.direct_release_all(disposition)
    }
}

struct ConcreteDirectPlan<'plan, A: TransactionalResource, P> {
    plan: &'plan mut P,
    items: &'plan mut [ItemData<A>],
    intents: &'plan mut [Option<A::Intent>],
    active_len: usize,
}

impl<A: TransactionalResource, P: ErasedDirectCommitPlan<A>> DirectCommitBody
    for ConcreteDirectPlan<'_, A, P>
{
    fn acquire(&mut self, owner: OwnerId) -> Result<(), AcquireError> {
        self.plan.acquire_all(&self.items[..self.active_len], owner)
    }

    fn validate(&self, commit_id: Option<OccCommitId>) -> Result<(), CheckError> {
        self.plan.validate(
            &self.items[..self.active_len],
            &self.intents[..self.active_len],
            commit_id,
        )
    }

    fn install(&mut self, commit_id: Option<OccCommitId>) {
        self.plan.install(
            &mut self.items[..self.active_len],
            &self.intents[..self.active_len],
            commit_id,
        );
    }

    fn release(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault> {
        self.plan
            .release_all(&self.items[..self.active_len], disposition)
    }
}

impl DirectCommitContext<'_, '_> {
    /// The ordinary direct entry retains erased batch dispatch. Singleton
    /// batches use this same preflight and prepared body through the concrete
    /// plan entry. Callback containment and boundary writes are shared.
    pub(crate) fn run<B: ErasedItemBatch + ?Sized>(
        &mut self,
        batch: &mut B,
    ) -> Result<(), DirectCommitStepFailure> {
        let requires_ordered_commit_id = self.preflight(batch)?;
        self.run_body(batch, requires_ordered_commit_id)?;
        batch
            .teardown_direct_plan()
            .map_err(|()| DirectCommitStepFailure::ReleasedPlanTeardown)
    }

    pub(crate) fn preflight<B: ErasedItemBatch + ?Sized>(
        &mut self,
        batch: &mut B,
    ) -> Result<bool, DirectCommitStepFailure> {
        let (has_writes, has_predicates, requires_ordered_commit_id) = batch.commit_shape();
        debug_assert!(has_writes);
        debug_assert!(!has_predicates);

        *self.phase = FailurePhase::Preflight;
        let runtime_id = self.runtime.id();
        let max_locks = self.runtime.config().max_locks_per_transaction();
        match catch_unwind(AssertUnwindSafe(|| {
            batch.direct_preflight(runtime_id, max_locks)
        })) {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return Err(DirectCommitStepFailure::Prepare(error)),
            Err(payload) => return Err(DirectCommitStepFailure::Panicked(*self.phase, payload)),
        }
        Ok(requires_ordered_commit_id)
    }

    /// All borrows refer to disjoint fields of the same retained batch. Slice
    /// bounds are checked in their original phases, not during this handoff.
    pub(crate) fn run_concrete_plan<A: TransactionalResource, P: ErasedDirectCommitPlan<A>>(
        &mut self,
        plan: &mut P,
        items: &mut [ItemData<A>],
        intents: &mut [Option<A::Intent>],
        active_len: usize,
        requires_ordered_commit_id: bool,
    ) -> Result<(), DirectCommitStepFailure> {
        self.run_body(
            &mut ConcreteDirectPlan {
                plan,
                items,
                intents,
                active_len,
            },
            requires_ordered_commit_id,
        )
    }

    /// A view may only borrow the already-prepared owner's fields. Creating
    /// those borrows is contained at the first prepared phase; it cannot
    /// acquire a guard or move any adapter-owned value. Creation and body run
    /// under one unwind: both belong to the same retained plan, and the
    /// recorded phase still names the step that panicked.
    pub(crate) fn run_created_body<B: DirectCommitBody>(
        &mut self,
        create: impl FnOnce() -> B,
        requires_ordered_commit_id: bool,
    ) -> Result<(), DirectCommitStepFailure> {
        *self.phase = FailurePhase::Acquire;
        match catch_unwind(AssertUnwindSafe(|| {
            let mut body = create();
            self.run_body_phases(&mut body, requires_ordered_commit_id)
        })) {
            Ok(result) => result,
            Err(payload) => Err(DirectCommitStepFailure::Panicked(*self.phase, payload)),
        }
    }

    fn run_body<B: DirectCommitBody + ?Sized>(
        &mut self,
        batch: &mut B,
        requires_ordered_commit_id: bool,
    ) -> Result<(), DirectCommitStepFailure> {
        // One unwind frame covers the acquire-to-release sequence. `*self.phase`
        // is written before every step, so a caught panic still reports the
        // phase it happened in and the owner's boundary/cleanup decision is
        // unchanged; only the number of landing pads shrinks. Hook callbacks
        // keep individual containment below because upper layers re-enter them.
        match catch_unwind(AssertUnwindSafe(|| {
            self.run_body_phases(batch, requires_ordered_commit_id)
        })) {
            Ok(result) => result,
            Err(payload) => Err(DirectCommitStepFailure::Panicked(*self.phase, payload)),
        }
    }

    fn run_body_phases<B: DirectCommitBody + ?Sized>(
        &mut self,
        batch: &mut B,
        requires_ordered_commit_id: bool,
    ) -> Result<(), DirectCommitStepFailure> {
        *self.phase = FailurePhase::Acquire;
        match batch.acquire(self.owner) {
            Ok(()) => {}
            Err(error) => return Err(DirectCommitStepFailure::Acquire(error)),
        }

        if self.hook.is_some() {
            *self.phase = FailurePhase::UpperMetadata;
            match catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::reserve_upper_metadata)
            })) {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return Err(DirectCommitStepFailure::Hook(error)),
                Err(payload) => {
                    return Err(DirectCommitStepFailure::Panicked(*self.phase, payload))
                }
            }
        }

        *self.phase = FailurePhase::CommitMetadata;
        if requires_ordered_commit_id {
            *self.commit_id = Some(
                self.runtime
                    .reserve_commit_id()
                    .map_err(DirectCommitStepFailure::CommitMetadata)?,
            );
        }

        *self.phase = FailurePhase::Validation;
        match batch.validate(*self.commit_id) {
            Ok(()) => {}
            Err(error) => return Err(DirectCommitStepFailure::Validation(error)),
        }

        if self.hook.is_some() {
            *self.phase = FailurePhase::PreinstallHook;
            match catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::pre_install)
            })) {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return Err(DirectCommitStepFailure::Hook(error)),
                Err(payload) => {
                    return Err(DirectCommitStepFailure::Panicked(*self.phase, payload))
                }
            }
        }

        *self.boundary = CommitBoundary::Irrevocable;
        *self.phase = FailurePhase::Install;
        batch.install(*self.commit_id);

        if self.hook.is_some() {
            *self.phase = FailurePhase::PostInstallHook;
            if let Err(payload) = catch_unwind(AssertUnwindSafe(|| {
                if let Some(hook) = self.hook.as_deref_mut() {
                    hook.post_install();
                }
            })) {
                return Err(DirectCommitStepFailure::Panicked(*self.phase, payload));
            }
        }

        *self.phase = FailurePhase::Release;
        let disposition = LockDisposition::Committed {
            occ_commit_id: *self.commit_id,
        };
        match batch.release(disposition) {
            Ok(()) => {
                *self.boundary = CommitBoundary::Published;
            }
            Err(_) => return Err(DirectCommitStepFailure::Release),
        }
        Ok(())
    }
}

#[cfg(test)]
#[path = "direct_protocol_tests.rs"]
mod tests;
