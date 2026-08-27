//! Worker-affine transaction execution and the native STO commit protocol.

use std::{
    any::TypeId,
    collections::HashMap,
    hash::{Hash, Hasher},
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
    sync::Arc,
};

use crate::{
    adapter::{FinishDisposition, TransactionalResource},
    error::{
        AbortInfo, AbortReason, AccessError, AcquireError, AdapterFault, AdapterPhase, BeginError,
        CheckError, CommitFailure, CommitInfo, CommitOutcome, DefiniteOutcome, FailurePhase,
        IndeterminateInfo, InternalError, InvalidUse, ItemInitError, PoisonInfo, PrepareError,
    },
    hook::{CommitHook, CommitHookError},
    item::{Entry, ErasedItem, ItemBox},
    lock::{FinishContext, LockDisposition, LockPlan},
    runtime::{IsolationMode, RegisteredResource, Runtime, WorkerContext},
};

/// Marker for the only public, executable transaction state in v1.
#[derive(Debug)]
pub struct Active;

struct TransactionFrame {
    runtime: Arc<Runtime>,
    isolation: IsolationMode,
    items: Vec<Option<Box<dyn ErasedItem>>>,
    by_hash: HashMap<u64, Vec<usize>>,
    doomed: bool,
}

impl TransactionFrame {
    fn new(runtime: Arc<Runtime>, isolation: IsolationMode) -> Self {
        Self {
            runtime,
            isolation,
            items: Vec::new(),
            by_hash: HashMap::new(),
            doomed: false,
        }
    }

    fn has_writes(&self) -> bool {
        self.items.iter().flatten().any(|item| item.has_intent())
    }
}

/// One active transaction borrowing exactly one attached worker.
///
/// Completion consumes this value. Dropping it performs a definite abort.
/// Active transaction state cannot cross a thread boundary:
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<sto_core::Transaction<'static>>();
/// ```
pub struct Transaction<'worker, State = Active> {
    worker: Option<&'worker mut WorkerContext>,
    frame: Option<TransactionFrame>,
    state: PhantomData<fn() -> State>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<State> std::fmt::Debug for Transaction<'_, State> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Transaction")
            .field("active", &self.frame.is_some())
            .field(
                "items",
                &self.frame.as_ref().map_or(0, |frame| frame.items.len()),
            )
            .field(
                "doomed",
                &self.frame.as_ref().is_some_and(|frame| frame.doomed),
            )
            .finish_non_exhaustive()
    }
}

impl WorkerContext {
    /// Begins a transaction with the runtime's configured default isolation.
    pub fn begin(&mut self) -> Result<Transaction<'_, Active>, BeginError> {
        let isolation = self.runtime.config().default_isolation();
        self.begin_with(isolation)
    }

    /// Begins a transaction with an explicitly selected isolation profile.
    pub fn begin_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<Transaction<'_, Active>, BeginError> {
        self.begin_isolation(isolation)?;
        let runtime = Arc::clone(&self.runtime);
        Ok(Transaction {
            worker: Some(self),
            frame: Some(TransactionFrame::new(runtime, isolation)),
            state: PhantomData,
            not_send_sync: PhantomData,
        })
    }
}

impl<'worker> Transaction<'worker, Active> {
    /// Returns this transaction's selected isolation profile.
    pub fn isolation(&self) -> IsolationMode {
        self.frame
            .as_ref()
            .map(|frame| frame.isolation)
            .unwrap_or(IsolationMode::Serializable)
    }

    /// Returns whether a prior failed or unwound access made commit impossible.
    pub fn is_doomed(&self) -> bool {
        self.frame.as_ref().is_none_or(|frame| frame.doomed)
    }

    /// Looks up or creates one typed logical item, then scopes adapter access
    /// to `operation`.
    pub fn with_item<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TransactionalResource,
    {
        let Some(frame) = self.frame.as_mut() else {
            return Err(InvalidUse::TransactionFinished.into());
        };
        if frame.doomed {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        // Arm before every fallible runtime, allocation, initialization, and
        // adapter-operation step. An unwind naturally leaves this set.
        frame.doomed = true;
        let result = with_item_inner(frame, resource, key, operation);
        if result.is_ok() {
            frame.doomed = false;
        } else if matches!(
            result,
            Err(AccessError::Fault(_) | AccessError::Poisoned(_) | AccessError::Internal(_))
        ) {
            frame.runtime.poison();
        }
        result
    }

    /// Attempts the full preflight/lock/validate/install/publication protocol.
    pub fn commit(mut self) -> Result<CommitOutcome, CommitFailure> {
        self.commit_with_optional_hook(None)
    }

    /// Commits with an upper-layer metadata and pre-install coordination hook.
    ///
    /// The hook is skipped for a read-only transaction. For a writing
    /// transaction its reservation callback runs after all planned locks are
    /// acquired, and its pre-install callback runs after final validation but
    /// before installation can begin.
    pub fn commit_with_hook<H: CommitHook>(
        mut self,
        hook: &mut H,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.commit_with_optional_hook(Some(hook))
    }

    fn commit_with_optional_hook(
        &mut self,
        hook: Option<&mut dyn CommitHook>,
    ) -> Result<CommitOutcome, CommitFailure> {
        let worker = self
            .worker
            .take()
            .expect("active transaction always retains its worker");
        let frame = self
            .frame
            .take()
            .expect("active transaction always retains its frame");
        let mut driver = CommitDriver::new(worker, frame, hook);
        match catch_unwind(AssertUnwindSafe(|| driver.run())) {
            Ok(result) => result,
            Err(_) => match catch_unwind(AssertUnwindSafe(|| driver.contain_unexpected_unwind())) {
                Ok(result) => result,
                Err(_) => driver.emergency_failure(),
            },
        }
    }

    /// Explicitly aborts and finishes every inserted item exactly once.
    pub fn abort(mut self) -> AbortInfo {
        let worker = self
            .worker
            .take()
            .expect("active transaction always retains its worker");
        let frame = self
            .frame
            .take()
            .expect("active transaction always retains its frame");
        abort_without_locks(worker, frame, AbortReason::Explicit)
    }
}

impl<State> Drop for Transaction<'_, State> {
    fn drop(&mut self) {
        let (Some(worker), Some(frame)) = (self.worker.take(), self.frame.take()) else {
            return;
        };
        let _ = abort_without_locks(worker, frame, AbortReason::Explicit);
    }
}

fn with_item_inner<A, R>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    key: A::Key,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    if resource.runtime_id() != frame.runtime.id() {
        return Err(InvalidUse::WrongRuntime.into());
    }
    resource.validate_binding()?;

    let identity_hash = item_hash(resource, &key);
    let existing = frame.by_hash.get(&identity_hash).and_then(|candidates| {
        candidates.iter().copied().find(|slot| {
            let Some(item) = frame.items.get(*slot).and_then(Option::as_ref) else {
                return false;
            };
            item.object_id() == resource.object_id()
                && item.resource_class() == resource.resource_class()
                && item.adapter_type_id() == TypeId::of::<A>()
                && item.key_type_id() == TypeId::of::<A::Key>()
                && item.key_eq(&key)
        })
    });

    let slot = if let Some(slot) = existing {
        slot
    } else {
        if frame.items.len() >= frame.runtime.config().max_items_per_transaction() {
            return Err(crate::error::CapacityError::ItemLimit.into());
        }
        frame
            .items
            .try_reserve(1)
            .map_err(|_| crate::error::CapacityError::ItemLimit)?;
        frame
            .by_hash
            .try_reserve(1)
            .map_err(|_| crate::error::CapacityError::ItemLimit)?;

        let local = match catch_unwind(AssertUnwindSafe(|| resource.adapter().new_local(&key))) {
            Ok(Ok(local)) => local,
            Ok(Err(error)) => return Err(item_init_access_error(error)),
            Err(_) => {
                frame.runtime.poison();
                return Err(AdapterFault::new(
                    AdapterPhase::ItemInit,
                    crate::error::AdapterFaultKind::Panic,
                )
                .into());
            }
        };

        let slot = frame.items.len();
        frame.items.push(Some(Box::new(ItemBox::new(
            resource.clone(),
            key.clone(),
            local,
        ))));
        let candidates = frame.by_hash.entry(identity_hash).or_default();
        candidates
            .try_reserve(1)
            .map_err(|_| crate::error::CapacityError::ItemLimit)?;
        candidates.push(slot);
        slot
    };

    let item = frame
        .items
        .get_mut(slot)
        .and_then(Option::as_mut)
        .ok_or_else(|| {
            AccessError::Internal(InternalError::new(
                FailurePhase::Execution,
                "item index references an empty slot",
            ))
        })?;
    if item.adapter_type_id() != TypeId::of::<A>() || item.key_type_id() != TypeId::of::<A::Key>() {
        frame.runtime.poison();
        return Err(InvalidUse::ResourceTypeMismatch.into());
    }
    let typed = item
        .as_any_mut()
        .downcast_mut::<ItemBox<A>>()
        .ok_or_else(|| {
            AccessError::Fault(AdapterFault::new(
                AdapterPhase::Execute,
                crate::error::AdapterFaultKind::TypeMismatch,
            ))
        })?;
    operation(&mut Entry::new(typed))
}

fn item_hash<A: TransactionalResource>(resource: &RegisteredResource<A>, key: &A::Key) -> u64 {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    resource.object_id().hash(&mut hasher);
    resource.resource_class().hash(&mut hasher);
    TypeId::of::<A>().hash(&mut hasher);
    TypeId::of::<A::Key>().hash(&mut hasher);
    key.hash(&mut hasher);
    hasher.finish()
}

fn item_init_access_error(error: ItemInitError) -> AccessError {
    match error {
        ItemInitError::Capacity(capacity) => capacity.into(),
        ItemInitError::Fault(fault) => fault.into(),
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CommitBoundary {
    Reversible,
    Irrevocable,
    Published,
}

struct CommitDriver<'worker, 'hook> {
    worker: &'worker mut WorkerContext,
    runtime: Arc<Runtime>,
    frame: Option<TransactionFrame>,
    locks: Option<LockPlan>,
    commit_id: Option<crate::identity::OccCommitId>,
    boundary: CommitBoundary,
    phase: FailurePhase,
    completed: bool,
    hook: Option<&'hook mut dyn CommitHook>,
}

impl<'worker, 'hook> CommitDriver<'worker, 'hook> {
    fn new(
        worker: &'worker mut WorkerContext,
        frame: TransactionFrame,
        hook: Option<&'hook mut dyn CommitHook>,
    ) -> Self {
        let runtime = Arc::clone(&frame.runtime);
        Self {
            worker,
            runtime,
            frame: Some(frame),
            locks: None,
            commit_id: None,
            boundary: CommitBoundary::Reversible,
            phase: FailurePhase::Preflight,
            completed: false,
            hook,
        }
    }

    fn run(&mut self) -> Result<CommitOutcome, CommitFailure> {
        self.phase = FailurePhase::Preflight;
        if self.frame().doomed {
            let poison = self.runtime.ensure_healthy(FailurePhase::Execution).err();
            return self.abort_commit(AbortReason::Doomed, poison);
        }
        if let Err(info) = self.frame().runtime.ensure_healthy(FailurePhase::Preflight) {
            return self.abort_commit(AbortReason::Doomed, Some(info));
        }

        let lock_plan = match LockPlan::new(
            self.frame().runtime.id(),
            self.frame().runtime.config().max_locks_per_transaction(),
        ) {
            Ok(plan) => plan,
            Err(error) => return self.abort_commit(error.into(), None),
        };
        self.locks = Some(lock_plan);

        for slot in 0..self.frame().items.len() {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let item = frame.items[slot]
                    .as_mut()
                    .expect("commit owns every live item slot");
                let mut cx = locks.preflight_context()?;
                item.preflight(&mut cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_prepare_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Preflight,
                            "preflight callback panicked",
                        )),
                        Some(self.poison(FailurePhase::Preflight, "preflight callback panicked")),
                    );
                }
            }
        }

        self.phase = FailurePhase::Acquire;
        let owner = self.worker.owner;
        let acquire = catch_unwind(AssertUnwindSafe(|| self.locks_mut().acquire_all(owner)));
        match acquire {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return self.handle_acquire_error(error),
            Err(_) => return self.handle_acquire_panic(),
        }

        if self.frame().has_writes() {
            self.phase = FailurePhase::UpperMetadata;
            let reservation = catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::reserve_upper_metadata)
            }));
            match reservation {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.abort_for_hook_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::UpperMetadata,
                            "upper metadata reservation panicked",
                        )),
                        Some(self.poison(
                            FailurePhase::UpperMetadata,
                            "upper metadata reservation panicked",
                        )),
                    )
                }
            }
        }

        self.phase = FailurePhase::PredicateUpgrade;
        for slot in 0..self.frame().items.len() {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let cx = locks.predicate_context().map_err(CheckError::from)?;
                frame.items[slot]
                    .as_mut()
                    .expect("commit owns every live item slot")
                    .upgrade_predicate(&cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => {
                    return self.handle_check_error(error, FailurePhase::PredicateUpgrade)
                }
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::PredicateUpgrade,
                            "predicate callback panicked",
                        )),
                        Some(self.poison(
                            FailurePhase::PredicateUpgrade,
                            "predicate callback panicked",
                        )),
                    )
                }
            }
        }

        if self.frame().has_writes() {
            self.phase = FailurePhase::CommitMetadata;
            self.commit_id = match self.frame().runtime.reserve_commit_id() {
                Ok(commit_id) => Some(commit_id),
                Err(error) => return self.abort_commit(error.into(), None),
            };
        }

        self.phase = FailurePhase::Validation;
        for slot in 0..self.frame().items.len() {
            let commit_id = self.commit_id;
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let cx = locks
                    .validation_context(commit_id)
                    .map_err(CheckError::from)?;
                frame.items[slot]
                    .as_ref()
                    .expect("commit owns every live item slot")
                    .validate(&cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_check_error(error, FailurePhase::Validation),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Validation,
                            "validation callback panicked",
                        )),
                        Some(self.poison(FailurePhase::Validation, "validation callback panicked")),
                    )
                }
            }
        }

        if self.frame().has_writes() {
            self.phase = FailurePhase::PreinstallHook;
            let acceptance = catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::pre_install)
            }));
            match acceptance {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.abort_for_hook_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::PreinstallHook,
                            "pre-install hook panicked",
                        )),
                        Some(
                            self.poison(FailurePhase::PreinstallHook, "pre-install hook panicked"),
                        ),
                    )
                }
            }
        }

        self.boundary = CommitBoundary::Irrevocable;
        self.phase = FailurePhase::Install;
        for slot in 0..self.frame().items.len() {
            if !self.frame().items[slot]
                .as_ref()
                .expect("commit owns every live item slot")
                .has_intent()
            {
                continue;
            }
            let commit_id = self.commit_id;
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let mut cx = locks
                    .install_context(commit_id)
                    .expect("held plan must construct an install context");
                frame.items[slot]
                    .as_mut()
                    .expect("commit owns every live item slot")
                    .install(&mut cx);
            }));
            if result.is_err() {
                return self.indeterminate(FailurePhase::Install, "install callback panicked");
            }
        }

        self.phase = FailurePhase::Release;
        let disposition = LockDisposition::Committed {
            occ_commit_id: self.commit_id,
        };
        let released = catch_unwind(AssertUnwindSafe(|| {
            self.locks_mut().release_all(disposition)
        }));
        match released {
            Ok(Ok(())) => {
                self.boundary = CommitBoundary::Published;
                if self.drop_released_lock_plan().is_err() {
                    return self.finish_committed_with_poison(PoisonInfo::new(
                        FailurePhase::Release,
                        "released lock-plan destruction panicked",
                    ));
                }
            }
            Ok(Err(_)) | Err(_) => {
                return self.indeterminate_after_release_failure(
                    FailurePhase::Release,
                    "lock release failed or panicked",
                )
            }
        }

        self.phase = FailurePhase::Finish;
        let commit_info = CommitInfo::new(self.commit_id);
        if let Err(info) = self.finish_items(FinishDisposition::Committed, FailurePhase::Finish) {
            self.complete_worker();
            return Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(commit_info),
                info,
            });
        }

        self.frame.take();
        self.complete_worker();
        Ok(CommitOutcome::Committed(commit_info))
    }

    fn handle_prepare_error(
        &mut self,
        error: PrepareError,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            PrepareError::Conflict(conflict) => self.abort_commit(conflict.into(), None),
            PrepareError::Capacity(capacity) => self.abort_commit(capacity.into(), None),
            PrepareError::Fault(fault) => self.abort_commit(
                AbortReason::Internal(InternalError::new(
                    FailurePhase::Preflight,
                    "adapter fault during preflight",
                )),
                Some(self.poison(
                    adapter_failure_phase(fault),
                    "adapter fault during preflight",
                )),
            ),
        }
    }

    fn abort_for_hook_error(
        &mut self,
        error: CommitHookError,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            CommitHookError::Rejected => self.abort_commit(AbortReason::HookRejected, None),
            CommitHookError::Capacity(capacity) => self.abort_commit(capacity.into(), None),
        }
    }

    fn handle_acquire_error(
        &mut self,
        error: AcquireError,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            AcquireError::Conflict(conflict) => self.abort_commit(conflict.into(), None),
            AcquireError::Fault(fault) => self.abort_commit(
                AbortReason::Internal(InternalError::new(
                    FailurePhase::Acquire,
                    "adapter fault during lock acquisition",
                )),
                Some(self.poison(
                    adapter_failure_phase(fault),
                    "adapter fault during acquisition",
                )),
            ),
        }
    }

    fn handle_acquire_panic(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = FailurePhase::Acquire;
        let reason = AbortReason::Internal(InternalError::new(
            phase,
            "lock acquisition callback panicked",
        ));
        let poison = self.poison(phase, "lock acquisition callback panicked");

        if let Some(mut locks) = self.locks.take() {
            let _ = catch_unwind(AssertUnwindSafe(|| {
                locks.recover_after_callback_panic(LockDisposition::Aborted)
            }));
            // The in-progress callback frame is uncertain even when recovery
            // released every other definitely acquired guard. Retain the
            // complete plan and all item state: `finish` is a post-unlock
            // callback and therefore cannot run under that uncertainty.
            std::mem::forget(locks);
        }
        self.quarantine_items();
        self.complete_worker();
        Err(CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Aborted(reason),
            info: poison,
        })
    }

    fn handle_check_error(
        &mut self,
        error: CheckError,
        phase: FailurePhase,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            CheckError::Conflict(conflict) => self.abort_commit(conflict.into(), None),
            CheckError::Fault(fault) => self.abort_commit(
                AbortReason::Internal(InternalError::new(phase, "adapter validation fault")),
                Some(self.poison(adapter_failure_phase(fault), "adapter validation fault")),
            ),
        }
    }

    fn abort_commit(
        &mut self,
        reason: AbortReason,
        mut poison: Option<PoisonInfo>,
    ) -> Result<CommitOutcome, CommitFailure> {
        if self.boundary != CommitBoundary::Reversible {
            return self.indeterminate(
                FailurePhase::Install,
                "abort requested after irreversible boundary",
            );
        }

        if let Some(mut locks) = self.locks.take() {
            if locks.requires_release() {
                let released = catch_unwind(AssertUnwindSafe(|| {
                    locks.release_all(LockDisposition::Aborted)
                }));
                if !matches!(released, Ok(Ok(()))) {
                    let _ = catch_unwind(AssertUnwindSafe(|| {
                        locks.recover_after_callback_panic(LockDisposition::Aborted)
                    }));
                    std::mem::forget(locks);
                    poison = Some(self.poison(
                        FailurePhase::Release,
                        "abort lock release failed or panicked",
                    ));
                    self.quarantine_items();
                    self.complete_worker();
                    return Err(CommitFailure::Poisoned {
                        outcome: DefiniteOutcome::Aborted(reason),
                        info: poison.expect("abort release failure poisons runtime"),
                    });
                }
            }
            if teardown_lock_plan(locks).is_err() {
                poison = Some(self.poison(
                    FailurePhase::Release,
                    "aborted lock-plan destruction panicked",
                ));
            }
        }

        if let Err(finish_poison) =
            self.finish_items(FinishDisposition::Aborted, FailurePhase::Finish)
        {
            poison = Some(finish_poison);
        }
        self.frame.take();
        self.complete_worker();

        match poison {
            Some(info) => Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(reason),
                info,
            }),
            None => Ok(CommitOutcome::Aborted(reason)),
        }
    }

    fn indeterminate(
        &mut self,
        phase: FailurePhase,
        reason: &'static str,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.runtime.mark_indeterminate();
        if let Some(mut locks) = self.locks.take() {
            let disposition = LockDisposition::Indeterminate {
                occ_commit_id: self.commit_id,
            };
            let released = catch_unwind(AssertUnwindSafe(|| locks.release_all(disposition)));
            match released {
                Ok(Ok(())) => {
                    let _ = teardown_lock_plan(locks);
                }
                Ok(Err(_)) | Err(_) => {
                    let _ = catch_unwind(AssertUnwindSafe(|| {
                        locks.recover_after_callback_panic(disposition)
                    }));
                    std::mem::forget(locks);
                }
            }
        }
        self.quarantine_items();
        self.complete_worker();
        Err(CommitFailure::Indeterminate(IndeterminateInfo::new(
            phase,
            self.commit_id,
            reason,
        )))
    }

    fn indeterminate_after_release_failure(
        &mut self,
        phase: FailurePhase,
        reason: &'static str,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.runtime.mark_indeterminate();
        if let Some(mut locks) = self.locks.take() {
            let disposition = LockDisposition::Indeterminate {
                occ_commit_id: self.commit_id,
            };
            let _ = catch_unwind(AssertUnwindSafe(|| {
                locks.recover_after_callback_panic(disposition)
            }));
            std::mem::forget(locks);
        }
        self.quarantine_items();
        self.complete_worker();
        Err(CommitFailure::Indeterminate(IndeterminateInfo::new(
            phase,
            self.commit_id,
            reason,
        )))
    }

    fn drop_released_lock_plan(&mut self) -> Result<(), ()> {
        let Some(locks) = self.locks.take() else {
            return Ok(());
        };
        teardown_lock_plan(locks)
    }

    fn finish_committed_with_poison(
        &mut self,
        mut info: PoisonInfo,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.runtime.poison();
        self.phase = FailurePhase::Finish;
        if let Err(finish_poison) =
            self.finish_items(FinishDisposition::Committed, FailurePhase::Finish)
        {
            info = finish_poison;
        }
        self.frame.take();
        self.complete_worker();
        Err(CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Committed(CommitInfo::new(self.commit_id)),
            info,
        })
    }

    fn contain_unexpected_unwind(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = self.phase;
        match self.boundary {
            CommitBoundary::Reversible => {
                let reason = AbortReason::Internal(InternalError::new(
                    phase,
                    "unexpected commit-driver unwind",
                ));
                let info = self.poison(phase, "unexpected commit-driver unwind");
                self.abort_commit(reason, Some(info))
            }
            CommitBoundary::Irrevocable => {
                self.indeterminate(phase, "unexpected unwind after irreversible boundary")
            }
            CommitBoundary::Published => self.finish_committed_with_poison(
                self.poison(phase, "unexpected unwind after complete publication"),
            ),
        }
    }

    fn emergency_failure(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = self.phase;
        if let Some(locks) = self.locks.take() {
            std::mem::forget(locks);
        }
        self.quarantine_items();
        self.complete_worker();

        match self.boundary {
            CommitBoundary::Reversible => {
                self.runtime.poison();
                let reason = AbortReason::Internal(InternalError::new(
                    phase,
                    "panic containment itself failed before installation",
                ));
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Aborted(reason),
                    info: PoisonInfo::new(phase, "panic containment itself failed"),
                })
            }
            CommitBoundary::Irrevocable => {
                self.runtime.mark_indeterminate();
                Err(CommitFailure::Indeterminate(IndeterminateInfo::new(
                    phase,
                    self.commit_id,
                    "panic containment itself failed after installation began",
                )))
            }
            CommitBoundary::Published => {
                self.runtime.poison();
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Committed(CommitInfo::new(self.commit_id)),
                    info: PoisonInfo::new(
                        phase,
                        "panic containment itself failed after publication",
                    ),
                })
            }
        }
    }

    fn finish_items(
        &mut self,
        disposition: FinishDisposition,
        phase: FailurePhase,
    ) -> Result<(), PoisonInfo> {
        let Some(frame) = self.frame.as_mut() else {
            return Ok(());
        };
        for slot in (0..frame.items.len()).rev() {
            let callback = catch_unwind(AssertUnwindSafe(|| {
                let item = frame.items[slot]
                    .as_mut()
                    .expect("finish owns every remaining item slot");
                let mut cx = FinishContext::new();
                item.finish(disposition, &mut cx);
            }));
            if callback.is_err() {
                frame.runtime.poison();
                let retained = self.frame.take().expect("frame exists during finish");
                std::mem::forget(retained);
                return Err(PoisonInfo::new(phase, "finish callback panicked"));
            }

            let teardown = catch_unwind(AssertUnwindSafe(|| {
                frame.items[slot]
                    .as_mut()
                    .expect("finished item remains owned during teardown")
                    .teardown_after_finish();
            }));
            if teardown.is_err() {
                frame.runtime.poison();
                let retained = self.frame.take().expect("frame exists during teardown");
                std::mem::forget(retained);
                return Err(PoisonInfo::new(phase, "adapter-owned state drop panicked"));
            }

            let item = frame.items[slot]
                .take()
                .expect("successful finish retains its item until drop");
            if catch_unwind(AssertUnwindSafe(|| drop(item))).is_err() {
                frame.runtime.poison();
                let retained = self.frame.take().expect("frame exists during finish");
                std::mem::forget(retained);
                return Err(PoisonInfo::new(phase, "adapter-owned state drop panicked"));
            }
        }
        Ok(())
    }

    fn frame(&self) -> &TransactionFrame {
        self.frame.as_ref().expect("commit driver owns its frame")
    }

    fn locks_mut(&mut self) -> &mut LockPlan {
        self.locks
            .as_mut()
            .expect("commit driver owns its lock plan")
    }

    fn parts_mut(&mut self) -> (&mut TransactionFrame, &mut LockPlan) {
        (
            self.frame.as_mut().expect("commit driver owns its frame"),
            self.locks
                .as_mut()
                .expect("commit driver owns its lock plan"),
        )
    }

    fn poison(&self, phase: FailurePhase, reason: &'static str) -> PoisonInfo {
        self.runtime.poison();
        PoisonInfo::new(phase, reason)
    }

    fn quarantine_items(&mut self) {
        if let Some(frame) = self.frame.take() {
            std::mem::forget(frame);
        }
    }

    fn complete_worker(&mut self) {
        if !self.completed {
            self.worker.finish_transaction();
            self.completed = true;
        }
    }
}

fn teardown_lock_plan(mut locks: LockPlan) -> Result<(), ()> {
    let teardown = catch_unwind(AssertUnwindSafe(|| locks.teardown_adapter_state()));
    if !matches!(teardown, Ok(Ok(()))) {
        std::mem::forget(locks);
        return Err(());
    }
    catch_unwind(AssertUnwindSafe(|| drop(locks))).map_err(|_| ())
}

impl Drop for CommitDriver<'_, '_> {
    fn drop(&mut self) {
        if self.completed {
            return;
        }
        let fallback = catch_unwind(AssertUnwindSafe(|| {
            let _ = self.contain_unexpected_unwind();
        }));
        if fallback.is_err() || !self.completed {
            let _ = self.emergency_failure();
        }
    }
}

fn abort_without_locks(
    worker: &mut WorkerContext,
    mut frame: TransactionFrame,
    reason: AbortReason,
) -> AbortInfo {
    let cleanup = catch_unwind(AssertUnwindSafe(|| {
        for slot in (0..frame.items.len()).rev() {
            let Some(item) = frame.items[slot].as_mut() else {
                continue;
            };
            let mut cx = FinishContext::new();
            item.finish(FinishDisposition::Aborted, &mut cx);
            item.teardown_after_finish();
            let item = frame.items[slot]
                .take()
                .expect("finished item remains owned");
            drop(item);
        }
    }));
    if cleanup.is_err() {
        frame.runtime.poison();
        std::mem::forget(frame);
    }
    worker.finish_transaction();
    AbortInfo::new(reason)
}

fn adapter_failure_phase(fault: AdapterFault) -> FailurePhase {
    match fault.phase() {
        AdapterPhase::ItemInit => FailurePhase::Execution,
        AdapterPhase::Execute => FailurePhase::Execution,
        AdapterPhase::Preflight => FailurePhase::Preflight,
        AdapterPhase::Acquire => FailurePhase::Acquire,
        AdapterPhase::ExecutionCheck => FailurePhase::Execution,
        AdapterPhase::PredicateUpgrade => FailurePhase::PredicateUpgrade,
        AdapterPhase::Validation => FailurePhase::Validation,
        AdapterPhase::Install => FailurePhase::Install,
        AdapterPhase::Release => FailurePhase::Release,
        AdapterPhase::Finish => FailurePhase::Finish,
    }
}
