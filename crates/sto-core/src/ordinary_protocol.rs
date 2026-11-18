//! Shared local and retained-participant phases. Direct lanes specialize the
//! same irreversible boundary and hook protocol.

use super::*;
#[cfg(feature = "distributed")]
use mako_clock::distributed::DistributedTimeTerm;

pub(super) type DriverStep = Result<(), Result<CommitOutcome, CommitFailure>>;

pub(super) fn contain_driver_unwind(
    driver: &mut CommitDriver<'_, '_>,
) -> Result<CommitOutcome, CommitFailure> {
    match catch_unwind(AssertUnwindSafe(|| driver.contain_unexpected_unwind())) {
        Ok(result) => result,
        Err(_) => driver.emergency_failure(),
    }
}

impl CommitDriver<'_, '_> {
    pub(super) fn lock_ordinary(&mut self, incremental: bool) -> DriverStep {
        self.phase = FailurePhase::Preflight;
        // Local run already checked admission before selecting its lane.
        // Retained participants must recheck on every separately invoked phase.
        if incremental {
            if self.frame().doomed {
                let poison = self.worker.runtime.ensure_healthy(FailurePhase::Execution).err();
                return Err(self.abort_commit(AbortReason::Doomed, poison));
            }
            if let Err(info) = self.frame().runtime.ensure_healthy(self.phase) {
                return Err(self.abort_commit(AbortReason::Doomed, Some(info)));
            }
        }
        // Execution can continue after this pass. Materialize before preparing
        // so changing adapter types cannot discard retained Prepared values.
        if incremental {
            let result = materialize_unique_batch(self.frame.as_mut().expect("retained frame"));
            if let Err(error) = result {
                let poison = match error {
                    AccessError::Fault(fault) => Some(self.poison(
                        adapter_failure_phase(fault), "participant item materialization fault",
                    )),
                    AccessError::Poisoned(info) => Some(info),
                    AccessError::Internal(info) => Some(self.poison(info.phase(), info.reason())),
                    _ => None,
                };
                return Err(self.abort_commit(access_abort_reason(error), poison));
            }
        }
        if self.locks.is_none() {
            let runtime_id = self.frame().runtime.id();
            let max_locks = self.frame().runtime.config().max_locks_per_transaction();
            let nonce = match self.worker.reserve_lock_plan_nonce() {
                Ok(nonce) => nonce,
                Err(error) => return Err(self.abort_commit(error.into(), None)),
            };
            let storage = std::mem::take(&mut self.frame.as_mut().expect("retained frame").lock_storage);
            let plan = match LockPlan::with_storage(runtime_id, max_locks, nonce, storage) {
                Ok(plan) => plan,
                Err(error) => return Err(self.abort_commit(error.into(), None)),
            };
            self.locks = Some(plan);
        }
        let owner = self.worker.owner;
        let result = catch_unwind(AssertUnwindSafe(|| {
            let (frame, locks) = self.parts_mut();
            let mut cx = locks.lock_context(owner, incremental)?;
            if frame.batch_active {
                frame.unique_batch.as_mut().expect("active batch").preflight(&mut cx)?;
            } else {
                for item in &mut frame.items[..frame.item_count] {
                    item.as_mut().expect("active item").preflight(&mut cx)?;
                }
            }
            locks.finish_locking()
        }));
        match result {
            Ok(Ok(())) => Ok(()),
            Ok(Err(error)) => Err(self.handle_prepare_error(error)),
            Err(_) if self.locks.as_ref().is_some_and(LockPlan::has_uncertain_callback) => {
                Err(self.handle_acquire_panic())
            }
            Err(_) => {
                let info = self.poison(FailurePhase::Preflight, "datatype lock callback panicked");
                Err(self.abort_commit(
                    AbortReason::Internal(InternalError::new(FailurePhase::Preflight, "datatype lock callback panicked")),
                    Some(info),
                ))
            }
        }
    }

    pub(super) fn certify_ordinary(&mut self) -> DriverStep {
        let shape = self.frame().commit_shape();
        if shape.has_writes && self.hook.is_some() {
            self.phase = FailurePhase::UpperMetadata;
            match catch_unwind(AssertUnwindSafe(|| {
                self.hook.as_deref_mut().map_or(Ok(()), CommitHook::reserve_upper_metadata)
            })) {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return Err(self.abort_for_hook_error(error)),
                Err(_) => return Err(self.contain_unexpected_unwind()),
            }
        }
        if shape.has_predicates {
            self.phase = FailurePhase::PredicateUpgrade;
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let cx = locks.predicate_context().map_err(CheckError::from)?;
                if frame.batch_active {
                    frame.unique_batch.as_mut().expect("active batch").upgrade_predicates(&cx)
                } else {
                    for item in &mut frame.items[..frame.item_count] {
                        item.as_mut().expect("active item").upgrade_predicate(&cx)?;
                    }
                    Ok(())
                }
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return Err(self.handle_check_error(error, self.phase)),
                Err(_) => return Err(self.contain_unexpected_unwind()),
            }
        }
        if shape.requires_ordered_commit_id {
            self.phase = FailurePhase::CommitMetadata;
            self.commit_id = match self.frame().runtime.reserve_commit_id() {
                Ok(id) => Some(id),
                Err(error) => return Err(self.abort_commit(error.into(), None)),
            };
        }
        self.phase = FailurePhase::Validation;
        let commit_id = self.commit_id;
        let result = catch_unwind(AssertUnwindSafe(|| {
            let (frame, locks) = self.parts_mut();
            let cx = locks.validation_context(commit_id).map_err(CheckError::from)?;
            if frame.batch_active {
                frame.unique_batch.as_ref().expect("active batch").validate(&cx)
            } else {
                for item in &frame.items[..frame.item_count] {
                    item.as_ref().expect("active item").validate(&cx)?;
                }
                Ok(())
            }
        }));
        match result {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return Err(self.handle_check_error(error, self.phase)),
            Err(_) => return Err(self.contain_unexpected_unwind()),
        }
        if shape.has_writes && self.hook.is_some() {
            self.phase = FailurePhase::PreinstallHook;
            match catch_unwind(AssertUnwindSafe(|| {
                self.hook.as_deref_mut().map_or(Ok(()), CommitHook::pre_install)
            })) {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return Err(self.abort_for_hook_error(error)),
                Err(_) => return Err(self.contain_unexpected_unwind()),
            }
        }
        self.certified_shape = Some(shape);
        Ok(())
    }

    pub(super) fn install_ordinary(
        &mut self,
        #[cfg(feature = "distributed")]
        decision: Option<DistributedTimeTerm>,
        retain_locks: bool,
    ) -> DriverStep {
        // No fallible metadata allocation or abort is permitted beyond here.
        self.boundary = CommitBoundary::Irrevocable;
        if self.certified_shape.expect("installation follows certification").has_writes {
            self.phase = FailurePhase::Install;
            let commit_id = self.commit_id;
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let mut cx = locks.install_context_with_decision(
                    commit_id,
                    #[cfg(feature = "distributed")]
                    decision,
                    retain_locks,
                )
                    .expect("certified plan retains held guards");
                if frame.batch_active {
                    frame.unique_batch.as_mut().expect("active batch").install(&mut cx);
                } else {
                    for item in &mut frame.items[..frame.item_count] {
                        let item = item.as_mut().expect("active item");
                        if item.has_intent() {
                            item.install(&mut cx);
                        }
                    }
                }
            }));
            if result.is_err() {
                return Err(self.indeterminate(FailurePhase::Install, "install callback panicked"));
            }
            if self.hook.is_some() {
                self.phase = FailurePhase::PostInstallHook;
                if catch_unwind(AssertUnwindSafe(|| {
                    if let Some(hook) = self.hook.as_deref_mut() {
                        hook.post_install();
                    }
                })).is_err() {
                    return Err(self.indeterminate(FailurePhase::PostInstallHook, "post-install hook panicked"));
                }
            }
        }
        Ok(())
    }

    pub(super) fn finish_ordinary(&mut self) -> Result<CommitOutcome, CommitFailure> {
        self.phase = FailurePhase::Release;
        let disposition = LockDisposition::Committed { occ_commit_id: self.commit_id };
        match catch_unwind(AssertUnwindSafe(|| self.locks_mut().release_all(disposition))) {
            Ok(Ok(())) => {
                self.boundary = CommitBoundary::Published;
                if self.drop_released_lock_plan().is_err() {
                    return self.finish_committed_with_poison(PoisonInfo::new(
                        FailurePhase::Release, "released lock-plan destruction panicked",
                    ));
                }
            }
            Ok(Err(_)) | Err(_) => return self.indeterminate_after_release_failure(
                FailurePhase::Release, "lock release failed or panicked",
            ),
        }
        self.phase = FailurePhase::Finish;
        let commit_info = CommitInfo::new(self.commit_id);
        if let Err(info) = self.finish_items(CleanupDisposition::Committed, FailurePhase::Finish) {
            self.complete_worker();
            return Err(CommitFailure::Poisoned { outcome: DefiniteOutcome::Committed(commit_info), info });
        }
        self.recycle_frame();
        self.complete_worker();
        Ok(CommitOutcome::Committed(commit_info))
    }
}

fn access_abort_reason(error: AccessError) -> AbortReason {
    match error {
        AccessError::Conflict(value) => AbortReason::Conflict(value),
        AccessError::Capacity(value) => AbortReason::Capacity(value),
        AccessError::InvalidUse(value) => AbortReason::InvalidUse(value),
        AccessError::Unsupported(value) => AbortReason::Unsupported(value),
        AccessError::Internal(value) => AbortReason::Internal(value),
        AccessError::Fault(_) | AccessError::Poisoned(_) => AbortReason::Doomed,
    }
}
