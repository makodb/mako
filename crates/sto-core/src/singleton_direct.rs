//! One prepared writing item, with all guard ownership retained by its plan.

use super::*;
use crate::{trans_item::DirectSingletonItem, transaction::DirectCommitBody};

pub(super) struct SingletonBody<
    'plan,
    A: TObject,
    L: DirectTokenLock,
    O: BorrowedTokenCommitOperations<A, L>,
> {
    implementation: &'static BorrowedInjectiveLockCommitCapability<A, L, O>,
    item: DirectSingletonItem<'plan, A>,
    frame: &'plan mut BorrowedTokenLockFrame<L>,
    active_frames: &'plan mut usize,
    acquired_len: &'plan mut usize,
    runtime_id: &'plan mut Option<RuntimeId>,
    owner: &'plan mut Option<OwnerId>,
    state: &'plan mut PlanState,
}

impl<
        'plan,
        A: DirectBorrowedLockTarget<L>,
        L: DirectTokenLock,
        O: BorrowedTokenCommitOperations<A, L>,
    > SingletonBody<'plan, A, L, O>
{
    pub(super) fn new(
        plan: &'plan mut BorrowedInjectiveLockCommitPlan<A, L, O>,
        item: &'plan mut TransItemData<A>,
        intent: &'plan Option<A::Intent>,
    ) -> Self {
        debug_assert_eq!(plan.active_frames, 1);
        debug_assert_eq!(plan.acquired_len, 0);
        debug_assert_eq!(plan.frames[0].item_slot, 0);
        Self {
            implementation: plan.implementation,
            item: item.direct_singleton_parts(intent),
            frame: &mut plan.frames[0],
            active_frames: &mut plan.active_frames,
            acquired_len: &mut plan.acquired_len,
            runtime_id: &mut plan.runtime_id,
            owner: &mut plan.owner,
            state: &mut plan.state,
        }
    }
}

impl<
        A: DirectBorrowedLockTarget<L>,
        L: DirectTokenLock,
        O: BorrowedTokenCommitOperations<A, L>,
    > DirectCommitBody for SingletonBody<'_, A, L, O>
{
    fn acquire(&mut self, owner: OwnerId) -> Result<(), AcquireError> {
        if *self.state != PlanState::Planning {
            return Err(AdapterFault::invariant(AdapterPhase::Acquire).into());
        }
        let runtime_id = self
            .runtime_id
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Acquire))?;
        *self.state = PlanState::Acquiring;
        *self.owner = Some(owner);
        debug_assert_eq!(*self.acquired_len, 0);
        let cx = AcquireContext::new(&owner);
        // Reborrow the target in the same phase as the general plan. Its
        // callback can panic or violate address stability; frame.acquire
        // retains the existing runtime/token/target validation.
        let target = self.item.adapter.direct_borrowed_lock_target();
        if let Err(error) = self.frame.acquire(runtime_id, target, &cx) {
            *self.state = PlanState::Releasing;
            // A singleton has no earlier acquired prefix to release. The
            // failing frame restored Planned, exactly as in acquire_all.
            debug_assert_eq!(*self.acquired_len, 0);
            *self.state = PlanState::Released;
            *self.owner = None;
            return Err(error);
        }
        *self.acquired_len = 1;
        *self.state = PlanState::Held;
        Ok(())
    }

    fn validate(&self, occ_commit_id: Option<OccCommitId>) -> Result<(), CheckError> {
        if *self.state != PlanState::Held || self.owner.is_none() {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        let owner = self.owner.expect("held direct token plan has an owner");
        if self.implementation.write_acquisition_certifies() {
            if self.item.intent.is_none() {
                return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
            }
            return Ok(());
        }
        let cx = DirectValidationContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        let item = DirectValidationItem::new(
            self.item.local,
            self.item.observation,
            self.item.intent.as_ref(),
        );
        let target = self.item.adapter.direct_borrowed_lock_target();
        let lock = self.frame.lock_ref(target).map_err(CheckError::Fault)?;
        self.implementation.operations.validate(
            self.item.adapter,
            self.item.key,
            item,
            Some(lock),
            &cx,
        )
    }

    fn install(&mut self, occ_commit_id: Option<OccCommitId>) {
        assert_eq!(
            *self.state,
            PlanState::Held,
            "direct token install requires held locks"
        );
        let owner = self.owner.expect("held direct token plan has an owner");
        let intent = self
            .item
            .intent
            .as_ref()
            .expect("a direct token frame belongs to a writing item");
        let item = InstallItem::new(self.item.local, self.item.observation, intent);
        let target = self.item.adapter.direct_borrowed_lock_target();
        let lock = self
            .frame
            .lock_mut(target)
            .expect("a borrowed direct token frame remains held by its exact target");
        let mut cx = DirectInstallContext {
            owner,
            occ_commit_id,
            distributed_time_term: None,
            not_send_sync: PhantomData,
        };
        self.implementation.operations.install(
            self.item.adapter,
            self.item.key,
            item,
            lock,
            &mut cx,
        );
        #[cfg(feature = "test-install-observer")]
        crate::test_install::write_installed();
    }

    fn release(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault> {
        if *self.state != PlanState::Held {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        *self.state = PlanState::Releasing;
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        // This is also the existing callback-in-progress marker. A panic in
        // target selection or release must leave uncertain frame zero for
        // the unchanged driver recovery/quarantine path.
        debug_assert_eq!(*self.acquired_len, 1);
        *self.acquired_len = 0;
        let target = self.item.adapter.direct_borrowed_lock_target();
        self.frame.release(target, disposition, &cx)?;
        *self.owner = None;
        if std::mem::needs_drop::<L::Guard>() {
            *self.state = PlanState::Released;
        } else {
            debug_assert!(self.frame.is_reusable());
            *self.active_frames = 0;
            *self.runtime_id = None;
            *self.state = PlanState::Pooled;
        }
        Ok(())
    }
}
