//! Canonical physical-lock planning and typed guard access.
//!
//! Logical transaction items may name the same physical lock.  This module
//! keeps lock-frame slots stable for typed [`LockUse`] tokens while acquiring
//! the deduplicated frames in full [`LockIdentity`] order.

use std::any::{Any, TypeId};
use std::marker::PhantomData;
use std::num::NonZeroUsize;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::sync::Arc;

#[cfg(test)]
use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};

use crate::error::{
    AcquireError, AdapterFault, AdapterFaultKind, AdapterPhase, CapacityError, PrepareError,
};
use crate::identity::{LockIdentity, OccCommitId, OccVersion, OwnerId, RuntimeId};

#[cfg(test)]
static NEXT_TEST_PLAN_NONCE: AtomicU64 = AtomicU64::new(1);

#[cfg(test)]
fn next_test_plan_nonce() -> u64 {
    NEXT_TEST_PLAN_NONCE
        .fetch_update(
            AtomicOrdering::Relaxed,
            AtomicOrdering::Relaxed,
            |current| current.checked_add(1).filter(|next| *next != 0),
        )
        .expect("test lock-plan nonce domain exhausted")
}

/// A physical lock target used by one or more logical transaction items.
///
/// The guard must be a `'static` value rather than a value borrowing from the
/// target. It may either own the state it needs for the locked phase or be a
/// detached token whose validity relies on the canonical target retained by
/// the core-owned lock frame.
pub trait TransactionLock: Send + Sync + 'static {
    /// The core-owned token proving that this lock was acquired. After
    /// `release` makes it inert, the core retains the value until every lock
    /// in the plan has been released and only then runs its destructor.
    type Guard: 'static;

    /// Attempts a bounded, nonblocking acquisition.
    ///
    /// The core retains the canonical target at a stable address until after
    /// `release` returns and the guard's destructor completes. A target may
    /// therefore return a detached `'static` token without cloning its
    /// [`Arc`]. If acquisition, release, or guard destruction panics, the core
    /// quarantines the frame and keeps its target alive. `identity` is the
    /// canonical identity selected by the lock plan; multiplexing targets may
    /// use it to locate a stable inline lock.
    fn try_acquire(
        &self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError>;

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
/// prepared state. Its runtime ID and persistent per-owner plan generation
/// prevent a retained token from aliasing a later plan, even after the owner
/// slot is detached and reused.
pub struct LockUse<L: TransactionLock> {
    runtime_id: RuntimeId,
    plan_nonce: u64,
    // Store the stable zero-based frame slot as slot + 1. Besides making an
    // impossible slot unrepresentable, this gives enums containing LockUse a
    // zero niche without dropping either plan-identity check.
    encoded_slot: NonZeroUsize,
    lock_type: PhantomData<fn() -> L>,
}

impl<L: TransactionLock> LockUse<L> {
    #[inline]
    fn encode_slot(slot: usize) -> Result<NonZeroUsize, CapacityError> {
        slot.checked_add(1)
            .and_then(NonZeroUsize::new)
            .ok_or(CapacityError::LockLimit)
    }

    #[inline]
    fn slot(&self) -> usize {
        self.encoded_slot.get() - 1
    }

    #[cfg(test)]
    pub(crate) const fn plan_nonce_for_test(&self) -> u64 {
        self.plan_nonce
    }
}

impl<L: TransactionLock> std::fmt::Debug for LockUse<L> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LockUse")
            .field("runtime_id", &self.runtime_id)
            .field("plan_nonce", &self.plan_nonce)
            .field("slot", &self.slot())
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

    /// Resolves a current-plan token to its exact retained target and guard.
    ///
    /// The returned target is the same allocation supplied by the matching
    /// [`LockRequest`], not a separately resolved or reconstructed value. Both
    /// borrows remain scoped to this validation context.
    pub fn target_and_guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
    ) -> Result<(&L, &L::Guard), AdapterFault> {
        self.plan.target_and_guard(use_, AdapterPhase::Validation)
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

    pub(crate) fn preflight_free_read_context(&self) -> PreflightFreeValidationContext<'_> {
        PreflightFreeValidationContext {
            occ_commit_id: self.occ_commit_id,
            scope: PhantomData,
            not_send_or_sync: PhantomData,
        }
    }
}

/// Final-certification metadata for an ordinary read that owns no lock use.
///
/// Unlike [`ValidationContext`], this deliberately has no `guard` or `owner`
/// capability. An adapter advertising a prepared-free read therefore cannot
/// accidentally depend on a physical lock supplied by another transaction
/// item: the same callback is valid both in a heterogeneous locked commit and
/// in the whole-transaction lock-free read lane.
pub struct PreflightFreeValidationContext<'a> {
    occ_commit_id: Option<OccCommitId>,
    scope: PhantomData<&'a mut ()>,
    not_send_or_sync: PhantomData<Rc<()>>,
}

impl PreflightFreeValidationContext<'_> {
    /// Returns the core OCC identity selected before final validation.
    ///
    /// A wholly read-only transaction does not reserve an identity and
    /// therefore returns `None`.
    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }

    pub(crate) fn without_locks<'a>(
        occ_commit_id: Option<OccCommitId>,
        _scope: &'a mut (),
    ) -> PreflightFreeValidationContext<'a> {
        PreflightFreeValidationContext {
            occ_commit_id,
            scope: PhantomData,
            not_send_or_sync: PhantomData,
        }
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

    /// Resolves a current-plan token to its exact retained target and mutable
    /// guard.
    ///
    /// The target and guard occupy disjoint fields in the core-owned lock
    /// frame, so callers can inspect the canonical target while updating its
    /// held guard without cloning or resolving the target again.
    pub fn target_and_guard_mut<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
    ) -> Result<(&L, &mut L::Guard), AdapterFault> {
        self.plan.target_and_guard_mut(use_, AdapterPhase::Install)
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
    /// Adapter-owned guard and target state was torn down successfully. The
    /// core-owned allocation may be rebound for a later transaction on the
    /// same worker.
    Pooled,
}

trait ErasedLockFrame: Any {
    fn target_type_id(&self) -> TypeId;
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn acquire(
        &mut self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<(), AcquireError>;
    fn release(
        &mut self,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault>;
    fn teardown_adapter_state(&mut self) -> Result<(), ()>;
    fn is_reusable(&self) -> bool;
}

struct LockFrame<L: TransactionLock> {
    // The guard is explicitly torn down before the target, with a separate
    // unwind boundary for each adapter-owned value.
    guard: Option<L::Guard>,
    target: Option<Arc<L>>,
    state: FrameState,
}

impl<L: TransactionLock> LockFrame<L> {
    fn new(target: Arc<L>) -> Self {
        Self {
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

    fn target_and_guard(&self, phase: AdapterPhase) -> Result<(&L, &L::Guard), AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::new(phase, AdapterFaultKind::StaleLockUse));
        }
        let target = self
            .target
            .as_deref()
            .ok_or_else(|| AdapterFault::invariant(phase))?;
        let guard = self
            .guard
            .as_ref()
            .ok_or_else(|| AdapterFault::invariant(phase))?;
        Ok((target, guard))
    }

    fn target_and_guard_mut(
        &mut self,
        phase: AdapterPhase,
    ) -> Result<(&L, &mut L::Guard), AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::new(phase, AdapterFaultKind::StaleLockUse));
        }
        let target = self
            .target
            .as_deref()
            .ok_or_else(|| AdapterFault::invariant(phase))?;
        let guard = self
            .guard
            .as_mut()
            .ok_or_else(|| AdapterFault::invariant(phase))?;
        Ok((target, guard))
    }

    fn rebind(&mut self, target: Arc<L>) -> Result<(), ()> {
        if self.state != FrameState::Pooled || self.guard.is_some() || self.target.is_some() {
            return Err(());
        }
        self.target = Some(target);
        self.state = FrameState::Planned;
        Ok(())
    }
}

impl<L: TransactionLock> ErasedLockFrame for LockFrame<L> {
    fn target_type_id(&self) -> TypeId {
        TypeId::of::<L>()
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn acquire(
        &mut self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<(), AcquireError> {
        if self.state != FrameState::Planned || self.guard.is_some() {
            return Err(AcquireError::Fault(AdapterFault::invariant(
                AdapterPhase::Acquire,
            )));
        }

        let target = self
            .target
            .as_ref()
            .ok_or_else(|| AcquireError::Fault(AdapterFault::invariant(AdapterPhase::Acquire)))?;
        let guard = target.try_acquire(identity, cx)?;
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
        self.state = FrameState::Pooled;
        Ok(())
    }

    fn is_reusable(&self) -> bool {
        self.state == FrameState::Pooled && self.guard.is_none() && self.target.is_none()
    }
}

/// Worker-affine allocations retained between definitely completed lock
/// plans. Every frame in this storage has had its adapter-owned guard and
/// target torn down under unwind containment before it enters the pool.
#[derive(Default)]
pub(crate) struct LockPlanStorage {
    frames: Vec<Box<dyn ErasedLockFrame>>,
    identities: Vec<LockIdentity>,
    acquisition_order: Vec<usize>,
    acquired: Vec<usize>,
}

/// Core-owned heterogeneous physical-lock plan.
///
/// Frame slots never move. `acquisition_order` contains sorted slot indices,
/// so sorting cannot invalidate any typed `LockUse<L>` retained by an adapter.
pub(crate) struct LockPlan {
    runtime_id: RuntimeId,
    // Reserved by the worker before any adapter preflight callback. RuntimeId
    // namespaces the persistent per-owner generation packed into this value.
    nonce: u64,
    max_locks: usize,
    frames: Vec<Box<dyn ErasedLockFrame>>,
    // Canonical identities are core-owned and slot-aligned with `frames`.
    // Keeping active identities contiguous makes deduplication and ordering
    // independent of heterogeneous frame vtables and pointer chasing.
    identities: Vec<LockIdentity>,
    active_frames: usize,
    acquisition_order: Vec<usize>,
    acquired: Vec<usize>,
    owner: Option<OwnerId>,
    state: PlanState,
    callback_in_progress: Option<usize>,
    quarantined_callback: Option<usize>,
}

impl LockPlan {
    #[cfg(test)]
    pub(crate) fn new(runtime_id: RuntimeId, max_locks: usize) -> Result<Self, CapacityError> {
        Self::with_storage(
            runtime_id,
            max_locks,
            next_test_plan_nonce(),
            LockPlanStorage::default(),
        )
    }

    pub(crate) fn with_storage(
        runtime_id: RuntimeId,
        max_locks: usize,
        nonce: u64,
        mut storage: LockPlanStorage,
    ) -> Result<Self, CapacityError> {
        if max_locks == 0 || nonce == 0 {
            return Err(CapacityError::LockLimit);
        }
        debug_assert!(storage.frames.iter().all(|frame| frame.is_reusable()));
        storage.identities.clear();
        storage.acquisition_order.clear();
        storage.acquired.clear();
        Ok(Self {
            runtime_id,
            nonce,
            max_locks,
            frames: storage.frames,
            identities: storage.identities,
            active_frames: 0,
            acquisition_order: storage.acquisition_order,
            acquired: storage.acquired,
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
        self.active_frames
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

        self.acquisition_order
            .sort_unstable_by(|left, right| self.identities[*left].cmp(&self.identities[*right]));
        self.state = PlanState::Acquiring;
        self.owner = Some(owner);

        let acquire_context = AcquireContext {
            owner: &owner,
            not_send_or_sync: PhantomData,
        };

        for order_index in 0..self.acquisition_order.len() {
            let slot = self.acquisition_order[order_index];
            self.callback_in_progress = Some(slot);
            let result = self.frames[slot].acquire(&self.identities[slot], &acquire_context);
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

        if let Some(slot) = self.identities[..self.active_frames]
            .iter()
            .position(|identity| identity == &request.identity)
        {
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
            return self.lock_use(slot);
        }

        if self.active_frames >= self.max_locks {
            return Err(PrepareError::Capacity(CapacityError::LockLimit));
        }

        self.acquisition_order
            .try_reserve(1)
            .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;
        self.acquired
            .try_reserve(1)
            .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;
        self.identities
            .try_reserve(1)
            .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;

        let slot = self.active_frames;
        let LockRequest { identity, target } = request;
        if slot < self.frames.len() {
            if self.frames[slot].target_type_id() != TypeId::of::<L>() {
                if let Some(offset) = self.frames[slot + 1..]
                    .iter()
                    .position(|frame| frame.target_type_id() == TypeId::of::<L>())
                {
                    self.frames.swap(slot, slot + 1 + offset);
                }
            }

            if self.frames[slot].target_type_id() == TypeId::of::<L>() {
                self.frames[slot]
                    .as_any_mut()
                    .downcast_mut::<LockFrame<L>>()
                    .ok_or_else(|| {
                        PrepareError::Fault(AdapterFault::new(
                            AdapterPhase::Preflight,
                            AdapterFaultKind::TypeMismatch,
                        ))
                    })?
                    .rebind(target)
                    .map_err(|()| {
                        PrepareError::Fault(AdapterFault::invariant(AdapterPhase::Preflight))
                    })?;
            } else {
                // The spare frame has no adapter-owned state. Replacing a
                // differently typed core allocation is safe; stable workloads
                // retain and reuse their typed boxes after the first attempt.
                self.frames[slot] = Box::new(LockFrame::new(target));
            }
        } else {
            self.frames
                .try_reserve(1)
                .map_err(|_| PrepareError::Capacity(CapacityError::LockLimit))?;
            self.frames.push(Box::new(LockFrame::new(target)));
        }
        self.identities.push(identity);
        self.active_frames += 1;
        self.acquisition_order.push(slot);
        self.lock_use(slot)
    }

    fn lock_use<L: TransactionLock>(&mut self, slot: usize) -> Result<LockUse<L>, PrepareError> {
        let encoded_slot = LockUse::<L>::encode_slot(slot).map_err(PrepareError::Capacity)?;
        Ok(LockUse {
            runtime_id: self.runtime_id,
            plan_nonce: self.nonce,
            encoded_slot,
            lock_type: PhantomData,
        })
    }

    fn guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
        phase: AdapterPhase,
    ) -> Result<&L::Guard, AdapterFault> {
        self.validate_use(use_, phase)?;
        self.frames[use_.slot()]
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
        self.frames[use_.slot()]
            .as_any_mut()
            .downcast_mut::<LockFrame<L>>()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::TypeMismatch))?
            .guard_mut(phase)
    }

    fn target_and_guard<L: TransactionLock>(
        &self,
        use_: &LockUse<L>,
        phase: AdapterPhase,
    ) -> Result<(&L, &L::Guard), AdapterFault> {
        self.validate_use(use_, phase)?;
        self.frames[use_.slot()]
            .as_any()
            .downcast_ref::<LockFrame<L>>()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::TypeMismatch))?
            .target_and_guard(phase)
    }

    fn target_and_guard_mut<L: TransactionLock>(
        &mut self,
        use_: &LockUse<L>,
        phase: AdapterPhase,
    ) -> Result<(&L, &mut L::Guard), AdapterFault> {
        self.validate_use(use_, phase)?;
        self.frames[use_.slot()]
            .as_any_mut()
            .downcast_mut::<LockFrame<L>>()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::TypeMismatch))?
            .target_and_guard_mut(phase)
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
            .get(use_.slot())
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
        for frame in self.frames[..self.active_frames].iter_mut().rev() {
            frame.teardown_adapter_state()?;
        }
        Ok(())
    }

    /// Returns the core-owned allocation pool after all adapter state was
    /// safely destroyed. Plans with uncertain callbacks or held locks are
    /// rejected and must be quarantined by their owner.
    pub(crate) fn into_reusable_storage(mut self) -> Result<LockPlanStorage, ()> {
        if !matches!(self.state, PlanState::Planning | PlanState::Released)
            || self.callback_in_progress.is_some()
            || self.quarantined_callback.is_some()
            || self.owner.is_some()
            || !self.acquired.is_empty()
            || self.identities.len() != self.active_frames
            || !self.frames.iter().all(|frame| frame.is_reusable())
        {
            // The rejected plan may contain an uncertain adapter frame. Make
            // quarantine intrinsic to this consuming operation so no caller
            // can accidentally run its destructor after a failed recycle.
            std::mem::forget(self);
            return Err(());
        }

        self.identities.clear();
        self.acquisition_order.clear();
        self.acquired.clear();
        Ok(LockPlanStorage {
            frames: std::mem::take(&mut self.frames),
            identities: std::mem::take(&mut self.identities),
            acquisition_order: std::mem::take(&mut self.acquisition_order),
            acquired: std::mem::take(&mut self.acquired),
        })
    }
}

#[cfg(test)]
mod tests {
    use std::panic::{catch_unwind, AssertUnwindSafe};
    use std::sync::{Arc, Mutex};

    use super::*;
    use crate::identity::{LockClass, LockKey, LockNamespaceId};
    use crate::version::AtomicVersion;

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

        fn try_acquire(
            &self,
            _identity: &LockIdentity,
            _cx: &AcquireContext<'_>,
        ) -> Result<Self::Guard, AcquireError> {
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

        fn try_acquire(
            &self,
            _identity: &LockIdentity,
            _cx: &AcquireContext<'_>,
        ) -> Result<Self::Guard, AcquireError> {
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

    struct IdentityRecordingLock {
        seen: Arc<Mutex<Vec<LockIdentity>>>,
    }

    impl TransactionLock for IdentityRecordingLock {
        type Guard = ();

        fn try_acquire(
            &self,
            identity: &LockIdentity,
            _cx: &AcquireContext<'_>,
        ) -> Result<Self::Guard, AcquireError> {
            self.seen.lock().unwrap().push(identity.clone());
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

    #[test]
    fn prepared_free_validation_context_carries_no_lock_plan_pointer() {
        assert_eq!(
            std::mem::size_of::<PreflightFreeValidationContext<'static>>(),
            std::mem::size_of::<Option<OccCommitId>>()
        );
        assert!(
            std::mem::size_of::<PreflightFreeValidationContext<'static>>()
                < std::mem::size_of::<ValidationContext<'static>>()
        );
    }

    #[test]
    fn atomic_version_detached_guard_uses_the_frame_retained_target() {
        let target = Arc::new(AtomicVersion::default());
        let weak_target = Arc::downgrade(&target);
        let mut plan = LockPlan::new(runtime_id(), 2).unwrap();
        let lock_use = plan
            .preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(identity(10), Arc::clone(&target)))
            .unwrap();
        drop(target);
        assert_eq!(weak_target.strong_count(), 1);

        plan.acquire_all(owner_id()).unwrap();
        assert_eq!(
            weak_target.strong_count(),
            1,
            "the detached guard must not clone the retained target"
        );
        let retained_target = weak_target
            .upgrade()
            .expect("the lock frame must retain its canonical target");
        {
            let context = plan.validation_context(None).unwrap();
            let guard = context.guard(&lock_use).unwrap();
            assert!(guard.is_for(retained_target.as_ref()));
            assert_eq!(guard.owner(), owner_id());
            assert!(retained_target.validate_own(OccVersion::INITIAL, owner_id()));
        }
        plan.release_all(LockDisposition::Aborted).unwrap();
        assert_eq!(retained_target.observe().unwrap(), OccVersion::INITIAL);
        drop(retained_target);
        assert_eq!(weak_target.strong_count(), 1);

        plan.teardown_adapter_state().unwrap();
        assert!(weak_target.upgrade().is_none());
    }

    #[test]
    fn contexts_return_the_exact_retained_target_with_the_guard() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let (target, request) = request(17, 10, false, &events);
        let mut plan = LockPlan::new(runtime_id(), 1).unwrap();
        let lock_use = plan
            .preflight_context()
            .unwrap()
            .require_lock(request)
            .unwrap();

        plan.acquire_all(owner_id()).unwrap();
        {
            let context = plan.validation_context(None).unwrap();
            let (retained_target, guard) = context.target_and_guard(&lock_use).unwrap();
            assert!(std::ptr::eq(retained_target, target.as_ref()));
            assert_eq!(guard.id, 17);
            assert!(std::ptr::eq(guard, context.guard(&lock_use).unwrap()));
        }
        {
            let mut context = plan.install_context(None).unwrap();
            let (retained_target, guard) = context.target_and_guard_mut(&lock_use).unwrap();
            assert!(std::ptr::eq(retained_target, target.as_ref()));
            guard.id = 71;
            assert_eq!(guard.id, 71);
            // Restore the fixture invariant checked by TestLock::release.
            guard.id = target.id;
        }
        plan.release_all(LockDisposition::Aborted).unwrap();
    }

    #[test]
    fn target_and_guard_access_rejects_stale_typed_and_wrong_plan_uses() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut other_plan = LockPlan::new(runtime_id(), 1).unwrap();
        let (_, other_request) = request(1, 10, false, &events);
        let wrong_plan = other_plan
            .preflight_context()
            .unwrap()
            .require_lock(other_request)
            .unwrap();

        let mut plan = LockPlan::new(runtime_id(), 1).unwrap();
        let (_, current_request) = request(2, 20, false, &events);
        let current = plan
            .preflight_context()
            .unwrap()
            .require_lock(current_request)
            .unwrap();
        let stale_slot = LockUse::<TestLock> {
            runtime_id: current.runtime_id,
            plan_nonce: current.plan_nonce,
            encoded_slot: LockUse::<TestLock>::encode_slot(1).unwrap(),
            lock_type: PhantomData,
        };
        let wrong_type = LockUse::<OtherLock> {
            runtime_id: current.runtime_id,
            plan_nonce: current.plan_nonce,
            encoded_slot: current.encoded_slot,
            lock_type: PhantomData,
        };

        plan.acquire_all(owner_id()).unwrap();
        {
            let context = plan.validation_context(None).unwrap();
            assert_eq!(
                *context.target_and_guard(&stale_slot).err().unwrap().kind(),
                AdapterFaultKind::StaleLockUse
            );
            assert_eq!(
                *context.target_and_guard(&wrong_type).err().unwrap().kind(),
                AdapterFaultKind::TypeMismatch
            );
            assert_eq!(
                *context.target_and_guard(&wrong_plan).err().unwrap().kind(),
                AdapterFaultKind::StaleLockUse
            );
        }
        {
            let mut context = plan.install_context(None).unwrap();
            assert_eq!(
                *context
                    .target_and_guard_mut(&stale_slot)
                    .err()
                    .unwrap()
                    .kind(),
                AdapterFaultKind::StaleLockUse
            );
            assert_eq!(
                *context
                    .target_and_guard_mut(&wrong_type)
                    .err()
                    .unwrap()
                    .kind(),
                AdapterFaultKind::TypeMismatch
            );
            assert_eq!(
                *context
                    .target_and_guard_mut(&wrong_plan)
                    .err()
                    .unwrap()
                    .kind(),
                AdapterFaultKind::StaleLockUse
            );
        }
        plan.release_all(LockDisposition::Aborted).unwrap();
    }

    #[test]
    fn direct_atomic_version_lock_publishes_the_commit_generation() {
        let target = Arc::new(AtomicVersion::default());
        let mut plan = LockPlan::new(runtime_id(), 1).unwrap();
        plan.preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(identity(10), Arc::clone(&target)))
            .unwrap();

        plan.acquire_all(owner_id()).unwrap();
        let commit_id = OccCommitId::new(2).unwrap();
        plan.release_all(LockDisposition::Committed {
            occ_commit_id: Some(commit_id),
        })
        .unwrap();
        assert_eq!(target.observe().unwrap(), commit_id.to_version());
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

    fn reusable_storage(mut plan: LockPlan) -> LockPlanStorage {
        plan.teardown_adapter_state().unwrap();
        match plan.into_reusable_storage() {
            Ok(storage) => storage,
            Err(()) => panic!("definitely released plan must be reusable"),
        }
    }

    fn frame_address(plan: &LockPlan, slot: usize) -> *const () {
        (&*plan.frames[slot] as *const dyn ErasedLockFrame).cast::<()>()
    }

    #[test]
    fn canonical_identities_are_contiguous_and_not_stored_in_typed_frames() {
        #[allow(dead_code)]
        struct InlineIdentityFrame<L: TransactionLock> {
            identity: LockIdentity,
            guard: Option<L::Guard>,
            target: Option<Arc<L>>,
            state: FrameState,
        }

        assert!(
            std::mem::size_of::<LockFrame<TestLock>>()
                < std::mem::size_of::<InlineIdentityFrame<TestLock>>()
        );

        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 8).unwrap();
        for (id, key) in [(1, 30), (2, 10), (3, 20)] {
            let (_, lock_request) = request(id, key, false, &events);
            plan.preflight_context()
                .unwrap()
                .require_lock(lock_request)
                .unwrap();
        }

        assert_eq!(
            plan.identities,
            vec![identity(30), identity(10), identity(20)]
        );
        assert_eq!(plan.identities.len(), plan.active_frames);
    }

    #[test]
    fn pooled_frame_acquires_with_the_current_sidecar_identity() {
        let seen = Arc::new(Mutex::new(Vec::new()));
        let mut first_plan = LockPlan::new(runtime_id(), 1).unwrap();
        first_plan
            .preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(
                identity(10),
                Arc::new(IdentityRecordingLock {
                    seen: Arc::clone(&seen),
                }),
            ))
            .unwrap();
        let pooled_address = frame_address(&first_plan, 0);
        first_plan.acquire_all(owner_id()).unwrap();
        first_plan.release_all(LockDisposition::Aborted).unwrap();
        let storage = reusable_storage(first_plan);

        let mut second_plan =
            LockPlan::with_storage(runtime_id(), 1, next_test_plan_nonce(), storage).unwrap();
        second_plan
            .preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(
                identity(77),
                Arc::new(IdentityRecordingLock {
                    seen: Arc::clone(&seen),
                }),
            ))
            .unwrap();
        assert_eq!(frame_address(&second_plan, 0), pooled_address);
        second_plan.acquire_all(owner_id()).unwrap();
        second_plan.release_all(LockDisposition::Aborted).unwrap();

        assert_eq!(*seen.lock().unwrap(), vec![identity(10), identity(77)]);
        drop(reusable_storage(second_plan));
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
                encoded_slot: current.encoded_slot,
                lock_type: PhantomData,
            };
            let type_error = context.guard(&wrong_type).unwrap_err();
            assert_eq!(*type_error.kind(), AdapterFaultKind::TypeMismatch);

            let wrong_runtime = LockUse::<TestLock> {
                runtime_id: RuntimeId::new(8).unwrap(),
                plan_nonce: current.plan_nonce,
                encoded_slot: current.encoded_slot,
                lock_type: PhantomData,
            };
            assert_eq!(
                *context.guard(&wrong_runtime).unwrap_err().kind(),
                AdapterFaultKind::StaleLockUse
            );

            let out_of_bounds = LockUse::<TestLock> {
                runtime_id: current.runtime_id,
                plan_nonce: current.plan_nonce,
                encoded_slot: NonZeroUsize::new(usize::MAX).unwrap(),
                lock_type: PhantomData,
            };
            assert_eq!(
                *context.guard(&out_of_bounds).unwrap_err().kind(),
                AdapterFaultKind::StaleLockUse
            );
        }
        second_plan.release_all(LockDisposition::Aborted).unwrap();
    }

    #[test]
    fn lock_use_slot_encoding_checks_capacity_and_supplies_an_enum_niche() {
        assert_eq!(LockUse::<TestLock>::encode_slot(0).unwrap().get(), 1);
        assert_eq!(
            LockUse::<TestLock>::encode_slot(usize::MAX - 1)
                .unwrap()
                .get(),
            usize::MAX
        );
        assert_eq!(
            LockUse::<TestLock>::encode_slot(usize::MAX),
            Err(CapacityError::LockLimit)
        );

        let last = LockUse::<TestLock> {
            runtime_id: runtime_id(),
            plan_nonce: 1,
            encoded_slot: LockUse::<TestLock>::encode_slot(usize::MAX - 1).unwrap(),
            lock_type: PhantomData,
        };
        assert_eq!(last.slot(), usize::MAX - 1);

        #[allow(dead_code)]
        enum PreparedShape {
            ReadOnly,
            Write { lock_use: LockUse<TestLock> },
        }

        #[cfg(target_pointer_width = "64")]
        {
            assert_eq!(std::mem::size_of::<LockUse<TestLock>>(), 24);
            assert_eq!(std::mem::size_of::<Option<LockUse<TestLock>>>(), 24);
            assert_eq!(std::mem::size_of::<PreparedShape>(), 24);
        }
    }

    #[test]
    fn definitely_released_plan_reuses_vectors_and_typed_frame_boxes() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut first_plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (_, first_request) = request(1, 10, false, &events);
        let stale = first_plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        let (_, second_request) = request(2, 20, false, &events);
        first_plan
            .preflight_context()
            .unwrap()
            .require_lock(second_request)
            .unwrap();

        let frame_addresses = [frame_address(&first_plan, 0), frame_address(&first_plan, 1)];
        let capacities = (
            first_plan.frames.capacity(),
            first_plan.identities.capacity(),
            first_plan.acquisition_order.capacity(),
            first_plan.acquired.capacity(),
        );
        first_plan.acquire_all(owner_id()).unwrap();
        first_plan.release_all(LockDisposition::Aborted).unwrap();
        let storage = reusable_storage(first_plan);

        assert_eq!(storage.frames.capacity(), capacities.0);
        assert_eq!(storage.identities.capacity(), capacities.1);
        assert_eq!(storage.acquisition_order.capacity(), capacities.2);
        assert_eq!(storage.acquired.capacity(), capacities.3);

        let mut second_plan =
            LockPlan::with_storage(runtime_id(), 8, next_test_plan_nonce(), storage).unwrap();
        let (_, first_request) = request(3, 30, false, &events);
        let current = second_plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        let (_, second_request) = request(4, 40, false, &events);
        second_plan
            .preflight_context()
            .unwrap()
            .require_lock(second_request)
            .unwrap();

        assert_eq!(frame_address(&second_plan, 0), frame_addresses[0]);
        assert_eq!(frame_address(&second_plan, 1), frame_addresses[1]);
        assert_eq!(second_plan.frames.capacity(), capacities.0);
        assert_eq!(second_plan.identities.capacity(), capacities.1);
        assert_eq!(second_plan.acquisition_order.capacity(), capacities.2);
        assert_eq!(second_plan.acquired.capacity(), capacities.3);

        second_plan.acquire_all(owner_id()).unwrap();
        let context = second_plan.validation_context(None).unwrap();
        assert_eq!(context.guard(&current).unwrap().id, 3);
        assert_eq!(
            *context.guard(&stale).unwrap_err().kind(),
            AdapterFaultKind::StaleLockUse
        );
        second_plan.release_all(LockDisposition::Aborted).unwrap();
        drop(reusable_storage(second_plan));
    }

    #[test]
    fn linear_identity_scan_distinguishes_nearby_byte_keys() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut plan = LockPlan::new(runtime_id(), 16).unwrap();
        let mut targets = Vec::new();

        for suffix in 0_u8..10 {
            let target = Arc::new(TestLock {
                id: u64::from(suffix),
                fail: false,
                panic_acquire: false,
                panic_release: false,
                events: Arc::clone(&events),
            });
            let key = LockKey::from_bytes(vec![0xaa, 0xbb, 0xcc, suffix]);
            let lock_identity = LockIdentity::new(
                runtime_id(),
                LockNamespaceId::new(11).unwrap(),
                LockClass::new(2).unwrap(),
                key,
            );
            plan.preflight_context()
                .unwrap()
                .require_lock(LockRequest::new(lock_identity, Arc::clone(&target)))
                .unwrap();
            targets.push(target);
        }

        let duplicate_identity = LockIdentity::new(
            runtime_id(),
            LockNamespaceId::new(11).unwrap(),
            LockClass::new(2).unwrap(),
            LockKey::from_bytes(vec![0xaa, 0xbb, 0xcc, 4]),
        );
        plan.preflight_context()
            .unwrap()
            .require_lock(LockRequest::new(
                duplicate_identity,
                Arc::clone(&targets[4]),
            ))
            .unwrap();
        assert_eq!(plan.len(), 10);

        plan.acquire_all(owner_id()).unwrap();
        plan.release_all(LockDisposition::Aborted).unwrap();
        drop(reusable_storage(plan));
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
    fn definite_acquisition_abort_returns_frames_to_the_pool() {
        let events = Arc::new(Mutex::new(Vec::new()));
        let mut first_plan = LockPlan::new(runtime_id(), 8).unwrap();
        let (_, first_request) = request(1, 1, false, &events);
        first_plan
            .preflight_context()
            .unwrap()
            .require_lock(first_request)
            .unwrap();
        let (_, failing_request) = request(2, 2, true, &events);
        first_plan
            .preflight_context()
            .unwrap()
            .require_lock(failing_request)
            .unwrap();
        let frame_addresses = [frame_address(&first_plan, 0), frame_address(&first_plan, 1)];

        assert!(first_plan.acquire_all(owner_id()).is_err());
        assert!(!first_plan.has_acquired());
        let storage = reusable_storage(first_plan);

        let mut second_plan =
            LockPlan::with_storage(runtime_id(), 8, next_test_plan_nonce(), storage).unwrap();
        for (id, key) in [(3, 3), (4, 4)] {
            let (_, lock_request) = request(id, key, false, &events);
            second_plan
                .preflight_context()
                .unwrap()
                .require_lock(lock_request)
                .unwrap();
        }
        assert_eq!(frame_address(&second_plan, 0), frame_addresses[0]);
        assert_eq!(frame_address(&second_plan, 1), frame_addresses[1]);
        second_plan.acquire_all(owner_id()).unwrap();
        second_plan.release_all(LockDisposition::Aborted).unwrap();
        drop(reusable_storage(second_plan));
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
        assert!(matches!(
            LockPlan::with_storage(runtime_id(), 1, 0, LockPlanStorage::default()),
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
