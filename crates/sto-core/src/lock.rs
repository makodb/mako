//! Canonical physical-lock planning and typed guard access.
//!
//! Logical transaction items may name the same physical lock.  This module
//! keeps lock-frame slots stable for typed [`LockUse`] tokens while acquiring
//! the deduplicated frames in full [`LockIdentity`] order.

use std::any::{Any, TypeId};
use std::collections::BTreeMap;
use std::marker::PhantomData;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};
use std::sync::Arc;

use crate::error::{
    AcquireError, AdapterFault, AdapterFaultKind, AdapterPhase, CapacityError, PrepareError,
};
use crate::identity::{LockIdentity, OccCommitId, OccVersion, OwnerId, RuntimeId};

static NEXT_PLAN_NONCE: AtomicU64 = AtomicU64::new(1);

/// A physical lock target used by one or more logical transaction items.
///
/// The guard must own everything it needs for the whole locked phase. Borrowed
/// mutex guards are deliberately excluded by the `'static` bound.
pub trait TransactionLock: Send + Sync + 'static {
    /// The core-owned token proving that this lock was acquired. After
    /// `release` makes it inert, the core retains the value until every lock
    /// in the plan has been released and only then runs its destructor.
    type Guard: 'static;

    /// Attempts a bounded, nonblocking acquisition.
    fn try_acquire(&self, cx: &AcquireContext<'_>) -> Result<Self::Guard, AcquireError>;

    /// Releases and publishes (or aborts) an acquired guard.
    ///
    /// Implementations must make `guard` inert before returning and must not
    /// fail or panic. The transaction layer contains a contract-violating
    /// panic and quarantines this plan; this module intentionally does not
    /// catch it.
    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    );
}

/// A typed request emitted during adapter preflight.
pub struct LockRequest<L: TransactionLock> {
    identity: LockIdentity,
    target: Arc<L>,
}

impl<L: TransactionLock> LockRequest<L> {
    /// Creates a request for `target` under its canonical physical identity.
    pub fn new(identity: LockIdentity, target: Arc<L>) -> Self {
        Self { identity, target }
    }

    /// Returns the canonical identity carried by this request.
    pub fn identity(&self) -> &LockIdentity {
        &self.identity
    }
}

/// An unforgeable typed reference to one frame in the current lock plan.
///
/// This token is intentionally neither `Clone` nor `Copy`. An adapter stores
/// the exact value returned by [`PreflightContext::require_lock`] in its
/// prepared state.
pub struct LockUse<L: TransactionLock> {
    runtime_id: RuntimeId,
    plan_nonce: u64,
    slot: usize,
    lock_type: PhantomData<fn() -> L>,
}

impl<L: TransactionLock> std::fmt::Debug for LockUse<L> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LockUse")
            .field("runtime_id", &self.runtime_id)
            .field("plan_nonce", &self.plan_nonce)
            .field("slot", &self.slot)
            .finish_non_exhaustive()
    }
}

/// How a held lock must publish or discard its protected state.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LockDisposition {
    /// No installation occurred; restore the pre-transaction generation.
    Aborted,
    /// Installation completed and the new state must become visible.
    Committed {
        /// Core OCC identity, when the selected profile uses one.
        occ_commit_id: Option<OccCommitId>,
    },
    /// Installation may have started; never restore an old generation.
    Indeterminate {
        /// Reserved core OCC identity, if one had already been chosen.
        occ_commit_id: Option<OccCommitId>,
    },
}

impl LockDisposition {
    fn occ_commit_id(self) -> Option<OccCommitId> {
        match self {
            Self::Aborted => None,
            Self::Committed { occ_commit_id } | Self::Indeterminate { occ_commit_id } => {
                occ_commit_id
            }
        }
    }
}

/// Capabilities available while attempting one planned lock.
pub struct AcquireContext<'a> {
    owner: &'a OwnerId,
    not_send_or_sync: PhantomData<Rc<()>>,
}

/// Opacity metadata available while revalidating execution-time observations.
pub struct ExecutionCheckContext<'a> {
    checked_through: Option<OccVersion>,
    scope: PhantomData<&'a mut ()>,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl ExecutionCheckContext<'_> {
    /// Returns the ordered version bound through which prior observations must
    /// be revalidated. Serializable-only execution supplies `None`.
    pub fn checked_through(&self) -> Option<OccVersion> {
        self.checked_through
    }

    #[allow(dead_code, reason = "reserved for the negotiated opaque profile")]
    pub(crate) fn new(checked_through: Option<OccVersion>) -> Self {
        Self {
            checked_through,
            scope: PhantomData,
            not_send_or_sync: PhantomData,
        }
    }
}

impl AcquireContext<'_> {
    /// Returns the opaque owner assigned to the current worker.
    pub fn owner(&self) -> OwnerId {
        *self.owner
    }
}

/// Capabilities available while constructing the physical lock plan.
pub struct PreflightContext<'a> {
    plan: &'a mut LockPlan,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl PreflightContext<'_> {
    /// Adds or reuses a canonical physical lock request.
    pub fn require_lock<L: TransactionLock>(
        &mut self,
        request: LockRequest<L>,
    ) -> Result<LockUse<L>, PrepareError> {
        self.plan.require_lock(request)
    }
}

/// Immutable held-lock capabilities used during predicate upgrade.
pub struct PredicateContext<'a> {
    plan: &'a LockPlan,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl PredicateContext<'_> {
    /// Resolves a current-plan token to its typed, held guard.
    pub fn guard<L: TransactionLock>(&self, use_: &LockUse<L>) -> Result<&L::Guard, AdapterFault> {
        self.plan.guard(use_, AdapterPhase::PredicateUpgrade)
    }

    /// Returns the owner of all guards in this plan.
    pub fn owner(&self) -> OwnerId {
        self.plan
            .owner
            .expect("predicate context is constructed only for a held plan")
    }
}

/// Immutable held-lock and commit-metadata capabilities used by validation.
pub struct ValidationContext<'a> {
    plan: &'a LockPlan,
    occ_commit_id: Option<OccCommitId>,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl ValidationContext<'_> {
    /// Resolves a current-plan token to its typed, held guard.
    pub fn guard<L: TransactionLock>(&self, use_: &LockUse<L>) -> Result<&L::Guard, AdapterFault> {
        self.plan.guard(use_, AdapterPhase::Validation)
    }

    /// Returns the owner of all guards in this plan.
    pub fn owner(&self) -> OwnerId {
        self.plan
            .owner
            .expect("validation context is constructed only for a held plan")
    }

    /// Returns the core OCC identity selected before final validation.
    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }
}

/// Mutable held-lock capabilities used after the irreversible boundary.
pub struct InstallContext<'a> {
    plan: &'a mut LockPlan,
    occ_commit_id: Option<OccCommitId>,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl InstallContext<'_> {
    /// Resolves a current-plan token to its typed, mutable held guard.
    ///
    /// A stale or mismatched token is reported explicitly. The caller is
    /// already past the irreversible boundary and must classify such a fault
    /// as indeterminate; it must not turn this result into an ordinary abort.
    pub fn guard_mut<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
    ) -> Result<&mut L::Guard, AdapterFault> {
        self.plan.guard_mut(use_, AdapterPhase::Install)
    }

    /// Returns the core OCC identity selected for this transaction.
    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }
}

/// Metadata available while publishing or aborting one physical lock.
pub struct ReleaseContext<'a> {
    owner: &'a OwnerId,
    occ_commit_id: Option<OccCommitId>,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl ReleaseContext<'_> {
    /// Returns the owner releasing this guard.
    pub fn owner(&self) -> OwnerId {
        *self.owner
    }

    /// Returns the core OCC identity associated with publication, if any.
    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }
}

/// Post-unlock cleanup capability.
///
/// V1 intentionally exposes no lock or transaction access during cleanup.
pub struct FinishContext<'a> {
    scope: PhantomData<&'a mut ()>,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl FinishContext<'_> {
    pub(crate) fn new() -> Self {
        Self {
            scope: PhantomData,
            not_send_or_sync: PhantomData,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum PlanState {
    Planning,
    Acquiring,
    Held,
    Releasing,
    Released,
    /// One callback frame is uncertain and the whole plan must be retained.
    Quarantined,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FrameState {
    Planned,
    Held,
    Released,
}

trait ErasedLockFrame: Any {
    fn identity(&self) -> &LockIdentity;
    fn target_type_id(&self) -> TypeId;
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn acquire(&mut self, cx: &AcquireContext<'_>) -> Result<(), AcquireError>;
    fn release(
        &mut self,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault>;
    fn teardown_adapter_state(&mut self) -> Result<(), ()>;
}

struct LockFrame<L: TransactionLock> {
    identity: LockIdentity,
    // The guard is explicitly torn down before the target, with a separate
    // unwind boundary for each adapter-owned value.
    guard: Option<L::Guard>,
    target: Option<Arc<L>>,
    state: FrameState,
}

impl<L: TransactionLock> LockFrame<L> {
    fn new(identity: LockIdentity, target: Arc<L>) -> Self {
        Self {
            identity,
            guard: None,
            target: Some(target),
            state: FrameState::Planned,
        }
    }

    fn guard(&self, phase: AdapterPhase) -> Result<&L::Guard, AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::new(phase, AdapterFaultKind::StaleLockUse));
        }
        self.guard
            .as_ref()
            .ok_or_else(|| AdapterFault::invariant(phase))
    }

    fn guard_mut(&mut self, phase: AdapterPhase) -> Result<&mut L::Guard, AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::new(phase, AdapterFaultKind::StaleLockUse));
        }
        self.guard
            .as_mut()
            .ok_or_else(|| AdapterFault::invariant(phase))
    }
}

impl<L: TransactionLock> ErasedLockFrame for LockFrame<L> {
    fn identity(&self) -> &LockIdentity {
        &self.identity
    }

    fn target_type_id(&self) -> TypeId {
        TypeId::of::<L>()
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn acquire(&mut self, cx: &AcquireContext<'_>) -> Result<(), AcquireError> {
        if self.state != FrameState::Planned || self.guard.is_some() {
            return Err(AcquireError::Fault(AdapterFault::invariant(
                AdapterPhase::Acquire,
            )));
        }

        let guard = self
            .target
            .as_ref()
            .ok_or_else(|| AcquireError::Fault(AdapterFault::invariant(AdapterPhase::Acquire)))?
            .try_acquire(cx)?;
        self.guard = Some(guard);
        self.state = FrameState::Held;
        Ok(())
    }

    fn release(
        &mut self,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }

        let guard = self
            .guard
            .as_mut()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        self.target
            .as_ref()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?
            .release(guard, disposition, cx);

        // Mark release but retain the now-inert adapter-owned guard. The plan
        // drops all guards only after every physical lock has been released,
        // so arbitrary associated-type destructors never run under an earlier
        // lock in the global order.
        self.state = FrameState::Released;
        Ok(())
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if self.state == FrameState::Held {
            return Err(());
        }
        if catch_unwind(AssertUnwindSafe(|| drop(self.guard.take()))).is_err() {
            return Err(());
        }
        if catch_unwind(AssertUnwindSafe(|| drop(self.target.take()))).is_err() {
            return Err(());
        }
        Ok(())
    }
}

/// Core-owned heterogeneous physical-lock plan.
///
/// Frame slots never move. `acquisition_order` contains sorted slot indices,
/// so sorting cannot invalidate any typed `LockUse<L>` retained by an adapter.
pub(crate) struct LockPlan {
    runtime_id: RuntimeId,
    nonce: u64,
    max_locks: usize,
    frames: Vec<Box<dyn ErasedLockFrame>>,
    by_identity: BTreeMap<LockIdentity, usize>,
    acquisition_order: Vec<usize>,
    acquired: Vec<usize>,
    owner: Option<OwnerId>,
    state: PlanState,
    callback_in_progress: Option<usize>,
    quarantined_callback: Option<usize>,
}

impl LockPlan {
    pub(crate) fn new(runtime_id: RuntimeId, max_locks: usize) -> Result<Self, CapacityError> {
        if max_locks == 0 {
            return Err(CapacityError::LockLimit);
        }
        let nonce = NEXT_PLAN_NONCE
            .fetch_update(
                AtomicOrdering::Relaxed,
                AtomicOrdering::Relaxed,
                |current| current.checked_add(1).filter(|next| *next != 0),
            )
            .map_err(|_| CapacityError::LockLimit)?;
        Ok(Self {
            runtime_id,
            nonce,
            max_locks,
            frames: Vec::new(),
            by_identity: BTreeMap::new(),
            acquisition_order: Vec::new(),
            acquired: Vec::new(),
            owner: None,
            state: PlanState::Planning,
            callback_in_progress: None,
            quarantined_callback: None,
        })
    }

    pub(crate) fn preflight_context(&mut self) -> Result<PreflightContext<'_>, PrepareError> {
        if self.state != PlanState::Planning {
            return Err(PrepareError::Fault(AdapterFault::invariant(
                AdapterPhase::Preflight,
            )));
        }
        Ok(PreflightContext {
            plan: self,
            not_send_or_sync: PhantomData,
        })
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.frames.len()
    }

    #[cfg(test)]
    pub(crate) fn has_acquired(&self) -> bool {
        !self.acquired.is_empty()
    }

    pub(crate) fn requires_release(&self) -> bool {
        self.state == PlanState::Held
    }

    pub(crate) fn acquire_all(&mut self, owner: OwnerId) -> Result<(), AcquireError> {
        if self.state != PlanState::Planning || self.callback_in_progress.is_some() {
            return Err(AcquireError::Fault(AdapterFault::invariant(
                AdapterPhase::Acquire,
            )));
        }

        self.acquisition_order.sort_unstable_by(|left, right| {
            self.frames[*left]
                .identity()
                .cmp(self.frames[*right].identity())
        });
        self.state = PlanState::Acquiring;
        self.owner = Some(owner);

        let acquire_context = AcquireContext {
            owner: &owner,
            not_send_or_sync: PhantomData,
        };

        for order_index in 0..self.acquisition_order.len() {
            let slot = self.acquisition_order[order_index];
            self.callback_in_progress = Some(slot);
            let result = self.frames[slot].acquire(&acquire_context);
            self.callback_in_progress = None;

            if let Err(error) = result {
                self.state = PlanState::Releasing;
                let release_context = ReleaseContext {
                    owner: &owner,
                    occ_commit_id: None,
                    not_send_or_sync: PhantomData,
                };
                self.release_acquired(LockDisposition::Aborted, &release_context)
                    .map_err(AcquireError::Fault)?;
                self.state = PlanState::Released;
                self.owner = None;
                return Err(error);
            }

            // Capacity for this push was reserved when the frame was planned,
            // before any lock could be held.
            self.acquired.push(slot);
        }

        self.state = PlanState::Held;
        Ok(())
    }

    pub(crate) fn predicate_context(&self) -> Result<PredicateContext<'_>, AdapterFault> {
        self.ensure_held(AdapterPhase::PredicateUpgrade)?;
        Ok(PredicateContext {
            plan: self,
            not_send_or_sync: PhantomData,
        })
    }

    pub(crate) fn validation_context(
        &self,
        occ_commit_id: Option<OccCommitId>,
    ) -> Result<ValidationContext<'_>, AdapterFault> {
        self.ensure_held(AdapterPhase::Validation)?;
        Ok(ValidationContext {
            plan: self,
            occ_commit_id,
            not_send_or_sync: PhantomData,
        })
    }

    pub(crate) fn install_context(
        &mut self,
        occ_commit_id: Option<OccCommitId>,
    ) -> Result<InstallContext<'_>, AdapterFault> {
        self.ensure_held(AdapterPhase::Install)?;
        Ok(InstallContext {
            plan: self,
            occ_commit_id,
            not_send_or_sync: PhantomData,
        })
    }

    pub(crate) fn release_all(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault> {
        if self.state != PlanState::Held || self.callback_in_progress.is_some() {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        self.state = PlanState::Releasing;
        let release_context = ReleaseContext {
            owner: &owner,
            occ_commit_id: disposition.occ_commit_id(),
            not_send_or_sync: PhantomData,
        };
        self.release_acquired(disposition, &release_context)?;
        self.state = PlanState::Released;
        self.owner = None;
        Ok(())
    }

    /// Recovers every definitely acquired *other* frame after a lock callback
    /// panics.
    ///
    /// `acquire_all` adds a slot to `acquired` only after its callback returns,
    /// and `release_acquired` removes a slot before invoking its release
    /// callback. Therefore `callback_in_progress` identifies the one uncertain
    /// frame while `acquired` contains exactly the frames that can still be
    /// released safely. The uncertain callback is never invoked again.
    ///
    /// Even when this method returns `Ok(())`, the plan remains terminally
    /// quarantined and its owner must retain/forget it rather than run normal
    /// Drop. A panic from one of the recovery releases propagates with
    /// `callback_in_progress` set to that new uncertain frame; the transaction
    /// layer must then retain the whole plan without another recovery attempt.
    pub(crate) fn recover_after_callback_panic(
        &mut self,
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault> {
        if !matches!(self.state, PlanState::Acquiring | PlanState::Releasing)
            || self.quarantined_callback.is_some()
        {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let uncertain = self
            .callback_in_progress
            .take()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;

        self.quarantined_callback = Some(uncertain);
        self.state = PlanState::Quarantined;
        let release_context = ReleaseContext {
            owner: &owner,
            occ_commit_id: disposition.occ_commit_id(),
            not_send_or_sync: PhantomData,
        };
        self.release_acquired(disposition, &release_context)
    }

    #[cfg(test)]
    pub(crate) fn is_quarantined(&self) -> bool {
        self.state == PlanState::Quarantined
    }

    fn require_lock<L: TransactionLock>(
        &mut self,
        request: LockRequest<L>,
    ) -> Result<LockUse<L>, PrepareError> {
        if self.state != PlanState::Planning {
            return Err(PrepareError::Fault(AdapterFault::invariant(
                AdapterPhase::Preflight,
            )));
        }
        if request.identity.runtime_id() != self.runtime_id {
            return Err(PrepareError::Fault(AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::LockIdentityMismatch,
            )));
        }

        if let Some(&slot) = self.by_identity.get(&request.identity) {
            let frame = self.frames.get(slot).ok_or_else(|| {
                PrepareError::Fault(AdapterFault::invariant(AdapterPhase::Preflight))
            })?;
            if frame.target_type_id() != TypeId::of::<L>() {
                return Err(PrepareError::Fault(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::TypeMismatch,
                )));
            }
            let typed = frame
                .as_any()
                .downcast_ref::<LockFrame<L>>()
                .ok_or_else(|| {
                    PrepareError::Fault(AdapterFault::new(
                        AdapterPhase::Preflight,
                        AdapterFaultKind::TypeMismatch,
                    ))
                })?;
            let Some(target) = typed.target.as_ref() else {
                return Err(PrepareError::Fault(AdapterFault::invariant(
                    AdapterPhase::Preflight,
                )));
            };
            if !Arc::ptr_eq(target, &request.target) {
                return Err(PrepareError::Fault(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )));
            }
            return Ok(self.lock_use(slot));
        }

        if self.frames.len() >= self.max_locks {
            return Err(PrepareError::Capacity(CapacityError::LockLimit));
        }

        self.frames
            .try_reserve(1)
            .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;
        self.acquisition_order
            .try_reserve(1)
            .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;
        self.acquired
            .try_reserve(1)
            .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;

        let slot = self.frames.len();
        let index_identity = request.identity.clone();
        self.frames
            .push(Box::new(LockFrame::new(request.identity, request.target)));
        self.acquisition_order.push(slot);
        if self.by_identity.insert(index_identity, slot).is_some() {
            return Err(PrepareError::Fault(AdapterFault::invariant(
                AdapterPhase::Preflight,
            )));
        }
        Ok(self.lock_use(slot))
    }

    fn lock_use<L: TransactionLock>(&self, slot: usize) -> LockUse<L> {
        LockUse {
            runtime_id: self.runtime_id,
            plan_nonce: self.nonce,
            slot,
            lock_type: PhantomData,
        }
    }

    fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
        phase: AdapterPhase,
    ) -> Result<&L::Guard, AdapterFault> {
        self.validate_use(use_, phase)?;
        self.frames[use_.slot]
            .as_any()
            .downcast_ref::<LockFrame<L>>()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::TypeMismatch))?
            .guard(phase)
    }

    fn guard_mut<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
        phase: AdapterPhase,
    ) -> Result<&mut L::Guard, AdapterFault> {
        self.validate_use(use_, phase)?;
        self.frames[use_.slot]
            .as_any_mut()
            .downcast_mut::<LockFrame<L>>()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::TypeMismatch))?
            .guard_mut(phase)
    }

    fn validate_use<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
        phase: AdapterPhase,
    ) -> Result<(), AdapterFault> {
        if use_.runtime_id != self.runtime_id || use_.plan_nonce != self.nonce {
            return Err(AdapterFault::new(phase, AdapterFaultKind::StaleLockUse));
        }
        let frame = self
            .frames
            .get(use_.slot)
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::StaleLockUse))?;
        if frame.target_type_id() != TypeId::of::<L>() {
            return Err(AdapterFault::new(phase, AdapterFaultKind::TypeMismatch));
        }
        Ok(())
    }

    fn ensure_held(&self, phase: AdapterPhase) -> Result<(), AdapterFault> {
        if self.state != PlanState::Held
            || self.callback_in_progress.is_some()
            || self.owner.is_none()
        {
            return Err(AdapterFault::invariant(phase));
        }
        Ok(())
    }

    fn release_acquired(
        &mut self,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        while let Some(slot) = self.acquired.pop() {
            self.callback_in_progress = Some(slot);
            self.frames[slot].release(disposition, cx)?;
            self.callback_in_progress = None;
        }
        Ok(())
    }

    /// Drops adapter-owned guards and lock targets one value at a time.
    ///
    /// The caller must retain/forget the plan if this reports a destructor
    /// panic; untouched frames and the remainder of the failing frame then
    /// stay reachable without a second unwind.
    pub(crate) fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if !matches!(self.state, PlanState::Planning | PlanState::Released) {
            return Err(());
        }
        for frame in self.frames.iter_mut().rev() {
            frame.teardown_adapter_state()?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::panic::{catch_unwind, AssertUnwindSafe};
    use std::sync::{Arc, Mutex};

    use super::*;
    use crate::identity::{LockClass, LockKey, LockNamespaceId};

    #[derive(Clone, Debug, Eq, PartialEq)]
    enum Event {
        Acquire(u64),
        Release(u64, LockDisposition),
    }

    #[derive(Debug)]
    struct TestGuard {
        id: u64,
    }

    struct TestLock {
        id: u64,
        fail: bool,
        panic_acquire: bool,
        panic_release: bool,
        events: Arc<Mutex<Vec<Event>>>,
    }

    impl TransactionLock for TestLock {
        type Guard = TestGuard;

        fn try_acquire(&self, _cx: &AcquireContext<'_>) -> Result<Self::Guard, AcquireError> {
            self.events.lock().unwrap().push(Event::Acquire(self.id));
            if self.panic_acquire {
                panic!("injected acquisition panic for lock {}", self.id);
            }
            if self.fail {
                return Err(AcquireError::Fault(AdapterFault::new(
                    AdapterPhase::Acquire,
                    AdapterFaultKind::Other("injected acquisition failure"),
                )));
            }
            Ok(TestGuard { id: self.id })
        }

        fn release(
            &self,
            guard: &mut Self::Guard,
            disposition: LockDisposition,
            _cx: &ReleaseContext<'_>,
        ) {
            assert_eq!(guard.id, self.id);
            self.events
                .lock()
                .unwrap()
                .push(Event::Release(self.id, disposition));
            if self.panic_release {
                panic!("injected release panic for lock {}", self.id);
            }
        }
    }

    struct OtherLock;

    impl TransactionLock for OtherLock {
        type Guard = ();

        fn try_acquire(&self, _cx: &AcquireContext<'_>) -> Result<Self::Guard, AcquireError> {
            Ok(())
        }

        fn release(
            &self,
            _guard: &mut Self::Guard,
            _disposition: LockDisposition,
            _cx: &ReleaseContext<'_>,
        ) {
        }
    }

    fn runtime_id() -> RuntimeId {
        RuntimeId::new(7).unwrap()
    }

    fn owner_id() -> OwnerId {
        OwnerId::new(3).unwrap()
    }

    fn identity(key: u64) -> LockIdentity {
        LockIdentity::new(
            runtime_id(),
            LockNamespaceId::new(11).unwrap(),
            LockClass::new(2).unwrap(),
            LockKey::from(key),
        )
    }

    fn request(
        id: u64,
        key: u64,
        fail: bool,
        events: &Arc<Mutex<Vec<Event>>>,
    ) -> (Arc<TestLock>, LockRequest<TestLock>) {
        let target = Arc::new(TestLock {
            id,
            fail,
            panic_acquire: false,
            panic_release: false,
            events: Arc::clone(events),
        });
        let request = LockRequest::new(identity(key), Arc::clone(&target));
        (target, request)
    }

    fn panic_request(
        id: u64,
        key: u64,
        panic_acquire: bool,
        panic_release: bool,
        events: &Arc<Mutex<Vec<Event>>>,
    ) -> LockRequest<TestLock> {
        let target = Arc::new(TestLock {
            id,
            fail: false,
            panic_acquire,
            panic_release,
            events: Arc::clone(events),
        });
        LockRequest::new(identity(key), target)
    }

    #[test]
    fn deduplicates_only_the_same_typed_arc_target() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (target, first_request) = request(1, 10, false, &events);
        let first = plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        let second = plan
            .preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(identity(10), Arc::clone(&target)))
            .unwrap();

        assert_eq!(plan.len(), 1);
        plan.acquire_all(owner_id()).unwrap();
        let context = plan.predicate_context().unwrap();
        assert_eq!(context.guard(&first).unwrap().id, 1);
        assert_eq!(context.guard(&second).unwrap().id, 1);
        plan.release_all(LockDisposition::Aborted).unwrap();
        assert_eq!(
            *events.lock().unwrap(),
            vec![
                Event::Acquire(1),
                Event::Release(1, LockDisposition::Aborted)
            ]
        );
    }

    #[test]
    fn rejects_same_identity_with_another_target_or_type() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut target_plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (_, first_request) = request(1, 10, false, &events);
        target_plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        let (_, other_request) = request(2, 10, false, &events);
        let target_error = target_plan
            .preflight_context()
            .unwrap()
            .require_lock(other_request)
            .unwrap_err();
        assert!(matches!(
            target_error,
            PrepareError::Fault(fault)
                if *fault.kind() == AdapterFaultKind::LockIdentityMismatch
        ));

        let mut type_plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (_, first_request) = request(1, 10, false, &events);
        type_plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        let type_error = type_plan
            .preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(identity(10), Arc::new(OtherLock)))
            .unwrap_err();
        assert!(matches!(
            type_error,
            PrepareError::Fault(fault) if *fault.kind() == AdapterFaultKind::TypeMismatch
        ));
    }

    #[test]
    fn sorting_does_not_change_typed_slots_and_release_is_reverse() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        let mut uses = Vec::new();
        for (id, key) in [(30, 30), (10, 10), (20, 20)] {
            let (_, lock_request) = request(id, key, false, &events);
            uses.push(
                plan.preflight_context()
                    .unwrap()
                    .require_lock(lock_request)
                    .unwrap(),
            );
        }

        plan.acquire_all(owner_id()).unwrap();
        assert_eq!(
            *events.lock().unwrap(),
            vec![Event::Acquire(10), Event::Acquire(20), Event::Acquire(30)]
        );
        {
            let mut context = plan.install_context(None).unwrap();
            assert_eq!(context.guard_mut(&uses[0]).unwrap().id, 30);
            assert_eq!(context.guard_mut(&uses[1]).unwrap().id, 10);
            assert_eq!(context.guard_mut(&uses[2]).unwrap().id, 20);
        }

        plan.release_all(LockDisposition::Committed {
            occ_commit_id: None,
        })
        .unwrap();
        assert_eq!(
            *events.lock().unwrap(),
            vec![
                Event::Acquire(10),
                Event::Acquire(20),
                Event::Acquire(30),
                Event::Release(
                    30,
                    LockDisposition::Committed {
                        occ_commit_id: None
                    }
                ),
                Event::Release(
                    20,
                    LockDisposition::Committed {
                        occ_commit_id: None
                    }
                ),
                Event::Release(
                    10,
                    LockDisposition::Committed {
                        occ_commit_id: None
                    }
                ),
            ]
        );
        assert!(plan.release_all(LockDisposition::Aborted).is_err());
    }

    #[test]
    fn rejects_stale_and_wrongly_typed_uses_without_unchecked_casts() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut first_plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (_, first_request) = request(1, 10, false, &events);
        let stale = first_plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();

        let mut second_plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (_, second_request) = request(2, 20, false, &events);
        let current = second_plan
            .preflight_context()
            .unwrap()
            .require_lock(second_request)
            .unwrap();
        second_plan.acquire_all(owner_id()).unwrap();
        {
            let context = second_plan.validation_context(None).unwrap();
            let stale_error = context.guard(&stale).unwrap_err();
            assert_eq!(*stale_error.kind(), AdapterFaultKind::StaleLockUse);

            let wrong_type = LockUse::<OtherLock> {
                runtime_id: current.runtime_id,
                plan_nonce: current.plan_nonce,
                slot: current.slot,
                lock_type: PhantomData,
            };
            let type_error = context.guard(&wrong_type).unwrap_err();
            assert_eq!(*type_error.kind(), AdapterFaultKind::TypeMismatch);
        }
        second_plan.release_all(LockDisposition::Aborted).unwrap();
    }

    #[test]
    fn nth_acquisition_failure_releases_prior_locks_once_in_reverse() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        for (id, fail) in [(1, false), (2, false), (3, true), (4, false)] {
            let (_, lock_request) = request(id, id, fail, &events);
            plan.preflight_context()
                .unwrap()
                .require_lock(lock_request)
                .unwrap();
        }

        assert!(plan.acquire_all(owner_id()).is_err());
        assert!(!plan.has_acquired());
        assert_eq!(
            *events.lock().unwrap(),
            vec![
                Event::Acquire(1),
                Event::Acquire(2),
                Event::Acquire(3),
                Event::Release(2, LockDisposition::Aborted),
                Event::Release(1, LockDisposition::Aborted),
            ]
        );
    }

    #[test]
    fn rejects_a_lock_identity_from_another_runtime() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        let target = Arc::new(TestLock {
            id: 1,
            fail: false,
            panic_acquire: false,
            panic_release: false,
            events,
        });
        let other_runtime_identity = LockIdentity::new(
            RuntimeId::new(8).unwrap(),
            LockNamespaceId::new(11).unwrap(),
            LockClass::new(2).unwrap(),
            LockKey::from(10_u64),
        );
        let error = plan
            .preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(other_runtime_identity, target))
            .unwrap_err();
        assert!(matches!(
            error,
            PrepareError::Fault(fault)
                if *fault.kind() == AdapterFaultKind::LockIdentityMismatch
        ));
    }

    #[test]
    fn enforces_the_unique_lock_limit_without_rejecting_deduplication() {
        assert!(matches!(
            LockPlan::new(runtime_id(), 0),
            Err(CapacityError::LockLimit)
        ));

        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 1).unwrap();
        let (target, first_request) = request(1, 10, false, &events);
        plan.preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        plan.preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(identity(10), Arc::clone(&target)))
            .unwrap();

        let (_, second_request) = request(2, 20, false, &events);
        assert!(matches!(
            plan.preflight_context()
                .unwrap()
                .require_lock(second_request),
            Err(PrepareError::Capacity(CapacityError::LockLimit))
        ));
        assert_eq!(plan.len(), 1);
    }

    #[test]
    fn acquisition_panic_quarantines_uncertain_frame_and_abort_releases_prior_guards() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        for request in [
            panic_request(1, 1, false, false, &events),
            panic_request(2, 2, false, false, &events),
            panic_request(3, 3, true, false, &events),
            panic_request(4, 4, false, false, &events),
        ] {
            plan.preflight_context()
                .unwrap()
                .require_lock(request)
                .unwrap();
        }

        assert!(catch_unwind(AssertUnwindSafe(|| plan.acquire_all(owner_id()))).is_err());
        plan.recover_after_callback_panic(LockDisposition::Aborted)
            .unwrap();
        assert!(plan.is_quarantined());
        assert!(!plan.has_acquired());
        assert_eq!(
            *events.lock().unwrap(),
            vec![
                Event::Acquire(1),
                Event::Acquire(2),
                Event::Acquire(3),
                Event::Release(2, LockDisposition::Aborted),
                Event::Release(1, LockDisposition::Aborted),
            ]
        );
        assert!(plan.release_all(LockDisposition::Aborted).is_err());
    }

    #[test]
    fn release_panic_is_not_retried_and_remaining_guards_publish_indeterminate() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        for request in [
            panic_request(1, 1, false, false, &events),
            panic_request(2, 2, false, true, &events),
            panic_request(3, 3, false, false, &events),
        ] {
            plan.preflight_context()
                .unwrap()
                .require_lock(request)
                .unwrap();
        }
        plan.acquire_all(owner_id()).unwrap();

        let commit_id = OccCommitId::new(9).unwrap();
        let committed = LockDisposition::Committed {
            occ_commit_id: Some(commit_id),
        };
        assert!(catch_unwind(AssertUnwindSafe(|| plan.release_all(committed))).is_err());

        let indeterminate = LockDisposition::Indeterminate {
            occ_commit_id: Some(commit_id),
        };
        plan.recover_after_callback_panic(indeterminate).unwrap();
        assert!(plan.is_quarantined());
        assert!(!plan.has_acquired());
        assert_eq!(
            *events.lock().unwrap(),
            vec![
                Event::Acquire(1),
                Event::Acquire(2),
                Event::Acquire(3),
                Event::Release(3, committed),
                // The uncertain frame is attempted exactly once.
                Event::Release(2, committed),
                Event::Release(1, indeterminate),
            ]
        );
        assert!(plan.release_all(indeterminate).is_err());
    }
}
