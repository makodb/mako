//! Core-owned typed transaction items and their private erased dispatch.

use std::{
    any::{Any, TypeId},
    marker::PhantomData,
    num::NonZeroUsize,
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
};

use crate::{
    adapter::{
        FinishDisposition, FinishItem, InstallItem, ObservationOrder, ObservationRef,
        PreflightFreeReadCapability, PreflightItem, TransactionalResource,
    },
    direct_commit::{DirectCommitCapability, DirectValidationItem, ErasedDirectCommitPlan},
    error::{
        AccessError, AcquireError, AdapterFault, AdapterPhase, CapacityError, CheckError,
        InvalidUse, PrepareError,
    },
    identity::{ObjectId, OccCommitId, OwnerId, ResourceClass, RuntimeId},
    lock::{
        ExecutionCheckContext, FinishContext, InstallContext, LockDisposition, PredicateContext,
        PreflightContext, PreflightFreeValidationContext, ValidationContext,
    },
    runtime::RegisteredResource,
    transaction::item_hash,
};

pub(crate) enum ObservationState<O, P> {
    Unobserved,
    Read(O),
    Predicate(P),
    UpgradedPredicate(O),
}

impl<O, P> ObservationState<O, P> {
    fn as_ref<A>(&self) -> ObservationRef<'_, A>
    where
        A: TransactionalResource<Observation = O, Predicate = P>,
    {
        match self {
            Self::Unobserved => ObservationRef::Unobserved,
            Self::Read(observation) => ObservationRef::Read(observation),
            Self::Predicate(predicate) => ObservationRef::Predicate(predicate),
            Self::UpgradedPredicate(observation) => ObservationRef::UpgradedPredicate(observation),
        }
    }
}

pub(crate) enum PreparationState<P> {
    Unprepared,
    PreflightFreeRead,
    Prepared(P),
    Installed(P),
}

/// Execution-hot state used by homogeneous typed batches.
///
/// This mirrors the execution fields of [`ItemBox`], but deliberately omits
/// the comparatively large intent and commit preparation. The batch keeps
/// both in parallel vectors so point-operation walks touch only the compact
/// identity, observation, and local-state stride.
pub(crate) struct ItemData<A: TransactionalResource> {
    observation: ObservationState<A::Observation, A::Predicate>,
    // Kept separate from `observation` so the two adapter-owned values can be
    // removed and dropped under distinct unwind boundaries.
    retained_predicate: Option<A::Predicate>,
    local: Option<A::Local>,
    key: Option<A::Key>,
    resource: Option<RegisteredResource<A>>,
}

/// The core-controlled, typed equivalent of one C++ `TItem` in the ordinary
/// heterogeneous representation.
pub(crate) struct ItemBox<A: TransactionalResource> {
    // Field order is also the fail-safe automatic drop order. Normal cleanup
    // uses `teardown_after_finish` so each adapter-owned destructor is
    // individually contained and the resource handle remains alive last. A
    // successfully finished pooled box retains that handle so repeated use of
    // the same binding does not mutate its shared Arc reference counts.
    intent: Option<A::Intent>,
    preparation: PreparationState<A::Prepared>,
    observation: ObservationState<A::Observation, A::Predicate>,
    // Kept separate from `observation` so the two adapter-owned values can be
    // removed and dropped under distinct unwind boundaries.
    retained_predicate: Option<A::Predicate>,
    local: Option<A::Local>,
    key: Option<A::Key>,
    resource: Option<RegisteredResource<A>>,
}

impl<A: TransactionalResource> ItemData<A> {
    #[inline]
    pub(crate) fn new(resource: RegisteredResource<A>, key: A::Key, local: A::Local) -> Self {
        Self {
            observation: ObservationState::Unobserved,
            retained_predicate: None,
            local: Some(local),
            key: Some(key),
            resource: Some(resource),
        }
    }

    #[inline(always)]
    pub(crate) fn reinitialize(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        local: A::Local,
    ) {
        debug_assert!(matches!(self.observation, ObservationState::Unobserved));
        debug_assert!(self.retained_predicate.is_none());
        debug_assert!(self.local.is_none());
        debug_assert!(self.key.is_none());
        debug_assert!(
            self.resource
                .as_ref()
                .is_none_or(|retained| retained.is_same_binding(resource)),
            "a different retained binding must be disposed under containment first"
        );
        if self.resource.is_none() {
            self.resource = Some(resource.clone());
        }
        self.local = Some(local);
        self.key = Some(key);
    }

    /// Reactivates an empty pooled item whose retained binding was already
    /// proven equal by the caller.
    ///
    /// Keeping this path separate from [`Self::reinitialize`] lets the common
    /// homogeneous-pool case avoid testing the resource option a second time.
    /// A different binding must still take the contained disposal path before
    /// calling `reinitialize`.
    #[inline(always)]
    pub(crate) fn reinitialize_same_binding(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        local: A::Local,
    ) {
        debug_assert!(matches!(self.observation, ObservationState::Unobserved));
        debug_assert!(self.retained_predicate.is_none());
        debug_assert!(self.local.is_none());
        debug_assert!(self.key.is_none());
        debug_assert!(self.retains_binding(resource));
        self.local = Some(local);
        self.key = Some(key);
    }

    #[inline]
    pub(crate) fn retains_binding(&self, resource: &RegisteredResource<A>) -> bool {
        self.resource
            .as_ref()
            .is_some_and(|retained| retained.is_same_binding(resource))
    }

    #[inline]
    pub(crate) fn matches_typed_identity(
        &self,
        resource: &RegisteredResource<A>,
        key: &A::Key,
    ) -> bool {
        self.retains_binding(resource) && self.key.as_ref() == Some(key)
    }

    #[inline]
    pub(crate) fn typed_identity_hash(&self) -> u64 {
        item_hash(
            self.resource
                .as_ref()
                .expect("active item retains its resource"),
            self.key.as_ref().expect("active item retains its key"),
        )
    }

    pub(crate) fn dispose_retained_resource(&mut self) {
        drop(self.resource.take());
    }

    pub(crate) fn direct_adapter_key_parts(&self) -> (&A, &A::Key) {
        let adapter = self
            .resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter();
        let key = self.key.as_ref().expect("active item retains its key");
        (adapter, key)
    }

    pub(crate) fn direct_preflight_parts<'item>(
        &'item mut self,
        intent: &'item mut Option<A::Intent>,
    ) -> (&'item A, &'item A::Key, PreflightItem<'item, A>) {
        let Self {
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let adapter = resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter();
        let key = key.as_ref().expect("active item retains its key");
        let item = PreflightItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        (adapter, key, item)
    }

    pub(crate) fn direct_validation_parts<'item>(
        &'item self,
        intent: &'item Option<A::Intent>,
    ) -> (&'item A, &'item A::Key, DirectValidationItem<'item, A>) {
        let adapter = self
            .resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter();
        let item = DirectValidationItem::new(
            self.local
                .as_ref()
                .expect("active item retains its local state"),
            self.observation.as_ref::<A>(),
            intent.as_ref(),
        );
        (
            adapter,
            self.key.as_ref().expect("active item retains its key"),
            item,
        )
    }

    pub(crate) fn direct_install_parts<'item>(
        &'item mut self,
        intent: &'item Option<A::Intent>,
    ) -> Option<(&'item A, &'item A::Key, InstallItem<'item, A>)> {
        let Self {
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let intent = intent.as_ref()?;
        let adapter = resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter();
        let item = InstallItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        Some((
            adapter,
            key.as_ref().expect("active item retains its key"),
            item,
        ))
    }

    #[inline(always)]
    fn is_preflight_free_read_candidate_typed(
        &self,
        intent: &Option<A::Intent>,
        preparation: &PreparationState<A::Prepared>,
    ) -> bool {
        matches!(preparation, PreparationState::Unprepared)
            && intent.is_none()
            && matches!(self.observation, ObservationState::Read(_))
            && self.retained_predicate.is_none()
            && self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .preflight_free_read_capability()
                .is_some()
    }

    #[inline(always)]
    fn select_preflight_free_read(
        &self,
        intent: &Option<A::Intent>,
        preparation: &mut PreparationState<A::Prepared>,
    ) -> Result<bool, PrepareError> {
        if !matches!(preparation, PreparationState::Unprepared) {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Preflight,
            )
            .into());
        }

        if intent.is_none()
            && matches!(self.observation, ObservationState::Read(_))
            && self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .preflight_free_read_capability()
                .is_some()
        {
            *preparation = PreparationState::PreflightFreeRead;
            return Ok(true);
        }

        Ok(false)
    }

    #[inline(always)]
    fn preflight_full_typed(
        &mut self,
        intent: &mut Option<A::Intent>,
        preparation: &mut PreparationState<A::Prepared>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<(), PrepareError> {
        let Self {
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let adapter = resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter();
        let view = PreflightItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        let prepared =
            adapter.preflight(key.as_ref().expect("active item retains its key"), view, cx)?;
        *preparation = PreparationState::Prepared(prepared);
        Ok(())
    }

    // Keep the full adapter preflight path out of the tiny prepared-free
    // selector so read candidates do not pay the stack frame needed by a
    // resource's potentially large write-planning callback.
    #[inline(never)]
    fn preflight_full(
        &mut self,
        intent: &mut Option<A::Intent>,
        preparation: &mut PreparationState<A::Prepared>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<(), PrepareError> {
        self.preflight_full_typed(intent, preparation, cx)
    }

    /// Statically dispatched predicate upgrade used by a homogeneous typed batch.
    #[inline(always)]
    fn upgrade_predicate_typed(
        &mut self,
        preparation: &PreparationState<A::Prepared>,
        cx: &PredicateContext<'_>,
    ) -> Result<(), CheckError> {
        let ObservationState::Predicate(predicate) = &self.observation else {
            return Ok(());
        };
        let PreparationState::Prepared(prepared) = preparation else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::PredicateUpgrade,
            )
            .into());
        };
        let observation = self
            .resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter()
            .upgrade_predicate(
                self.key.as_ref().expect("active item retains its key"),
                predicate,
                prepared,
                cx,
            )?;
        let prior = std::mem::replace(&mut self.observation, ObservationState::Unobserved);
        let ObservationState::Predicate(predicate) = prior else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::PredicateUpgrade,
            )
            .into());
        };
        self.retained_predicate = Some(predicate);
        self.observation = ObservationState::UpgradedPredicate(observation);
        Ok(())
    }

    /// Statically dispatched prepared-free validation for a homogeneous batch.
    #[inline(always)]
    fn validate_preflight_free_read_typed(
        &mut self,
        intent: &Option<A::Intent>,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let Self {
            observation,
            retained_predicate,
            local: _,
            key,
            resource,
        } = self;
        if intent.is_some() || retained_predicate.is_some() {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into());
        }
        let ObservationState::Read(observation) = observation else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into());
        };
        let resource = resource.as_ref().expect("active item retains its resource");
        let adapter = resource.adapter();
        let capability = adapter.preflight_free_read_capability().ok_or_else(|| {
            crate::error::AdapterFault::invariant(crate::error::AdapterPhase::Validation)
        })?;
        capability.validate(
            adapter,
            key.as_ref().expect("active item retains its key"),
            observation,
            cx,
        )
    }

    /// Statically dispatched validation used by a homogeneous typed batch.
    #[inline(always)]
    fn validate_typed(
        &self,
        preparation: &PreparationState<A::Prepared>,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        if matches!(preparation, PreparationState::PreflightFreeRead) {
            let ObservationState::Read(observation) = &self.observation else {
                return Err(crate::error::AdapterFault::invariant(
                    crate::error::AdapterPhase::Validation,
                )
                .into());
            };
            let resource = self
                .resource
                .as_ref()
                .expect("active item retains its resource");
            let adapter = resource.adapter();
            let capability = adapter.preflight_free_read_capability().ok_or_else(|| {
                crate::error::AdapterFault::invariant(crate::error::AdapterPhase::Validation)
            })?;
            let restricted = cx.preflight_free_read_context();
            return capability.validate(
                adapter,
                self.key.as_ref().expect("active item retains its key"),
                observation,
                &restricted,
            );
        }

        let PreparationState::Prepared(prepared) = preparation else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into());
        };
        match &self.observation {
            ObservationState::Unobserved => Ok(()),
            ObservationState::Read(observation)
            | ObservationState::UpgradedPredicate(observation) => self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .validate_read(
                    self.key.as_ref().expect("active item retains its key"),
                    observation,
                    prepared,
                    cx,
                ),
            ObservationState::Predicate(_) => Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into()),
        }
    }

    /// Statically dispatched installation used by a homogeneous typed batch.
    #[inline(always)]
    fn install_typed(
        &mut self,
        intent: &Option<A::Intent>,
        preparation: &mut PreparationState<A::Prepared>,
        cx: &mut InstallContext<'_>,
    ) {
        let Self {
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let Some(intent_ref) = intent.as_ref() else {
            return;
        };
        let PreparationState::Prepared(prepared) = preparation else {
            panic!("sto-core invariant: install called without prepared state");
        };
        let view = InstallItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent_ref,
        );
        resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter()
            .install(
                key.as_ref().expect("active item retains its key"),
                view,
                prepared,
                cx,
            );

        let old = std::mem::replace(preparation, PreparationState::Unprepared);
        let PreparationState::Prepared(prepared) = old else {
            unreachable!("prepared state was checked before install");
        };
        *preparation = PreparationState::Installed(prepared);
    }

    /// Statically dispatched finish used by a homogeneous typed batch.
    #[inline(always)]
    fn finish_typed(
        &mut self,
        intent: &mut Option<A::Intent>,
        preparation: &mut PreparationState<A::Prepared>,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
    ) {
        let Self {
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let resource = resource.as_ref().expect("active item retains its resource");
        let adapter = resource.adapter();
        if matches!(preparation, PreparationState::PreflightFreeRead)
            && disposition == FinishDisposition::Committed
        {
            let capability: &PreflightFreeReadCapability<A> = adapter
                .preflight_free_read_capability()
                .expect("preflight-free read capability disappeared before committed finish");
            if let Some(finish_committed) = capability.finish_committed_callback() {
                let view = FinishItem::new(
                    local.as_mut().expect("active item retains its local state"),
                    observation.as_ref::<A>(),
                    intent,
                );
                finish_committed(
                    adapter,
                    key.as_ref().expect("active item retains its key"),
                    view,
                    cx,
                );
            }
            return;
        }

        let prepared = match preparation {
            PreparationState::Unprepared | PreparationState::PreflightFreeRead => None,
            PreparationState::Prepared(prepared) | PreparationState::Installed(prepared) => {
                Some(prepared)
            }
        };
        let view = FinishItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        adapter.finish(
            key.as_ref().expect("active item retains its key"),
            view,
            prepared,
            disposition,
            cx,
        );
    }

    fn teardown_after_finish(
        &mut self,
        intent: &mut Option<A::Intent>,
        preparation: &mut PreparationState<A::Prepared>,
    ) {
        drop(intent.take());
        drop(std::mem::replace(preparation, PreparationState::Unprepared));
        drop(std::mem::replace(
            &mut self.observation,
            ObservationState::Unobserved,
        ));
        drop(self.retained_predicate.take());
        drop(self.local.take());
        drop(self.key.take());
        // Keep the immutable typed binding in this worker-local box. This
        // intentionally extends the object/adapter lease until the box is
        // rebound to another resource or the WorkerContext is dropped. The
        // number of retained leases is bounded by the worker's peak item
        // count, itself bounded by RuntimeConfig.
    }
}

impl<A: TransactionalResource> ItemBox<A> {
    #[inline]
    pub(crate) fn new(resource: RegisteredResource<A>, key: A::Key, local: A::Local) -> Self {
        Self {
            intent: None,
            preparation: PreparationState::Unprepared,
            observation: ObservationState::Unobserved,
            retained_predicate: None,
            local: Some(local),
            key: Some(key),
            resource: Some(resource),
        }
    }

    #[inline]
    fn from_data(data: ItemData<A>, intent: Option<A::Intent>) -> Self {
        let ItemData {
            observation,
            retained_predicate,
            local,
            key,
            resource,
        } = data;
        Self {
            intent,
            preparation: PreparationState::Unprepared,
            observation,
            retained_predicate,
            local,
            key,
            resource,
        }
    }

    #[inline(always)]
    pub(crate) fn reinitialize(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        local: A::Local,
    ) {
        debug_assert!(self.intent.is_none());
        debug_assert!(matches!(self.preparation, PreparationState::Unprepared));
        debug_assert!(matches!(self.observation, ObservationState::Unobserved));
        debug_assert!(self.retained_predicate.is_none());
        debug_assert!(self.local.is_none());
        debug_assert!(self.key.is_none());
        debug_assert!(
            self.resource
                .as_ref()
                .is_none_or(|retained| retained.is_same_binding(resource)),
            "a different retained binding must be disposed under containment first"
        );
        if self.resource.is_none() {
            self.resource = Some(resource.clone());
        }
        self.local = Some(local);
        self.key = Some(key);
    }

    #[inline]
    pub(crate) fn retains_binding(&self, resource: &RegisteredResource<A>) -> bool {
        self.resource
            .as_ref()
            .is_some_and(|retained| retained.is_same_binding(resource))
    }

    pub(crate) fn dispose_retained_resource(&mut self) {
        drop(self.resource.take());
    }

    // Keep the full adapter preflight path out of the tiny prepared-free
    // selector. In a mixed transaction every ordinary read reaches
    // `ErasedItem::preflight`, and those reads should not pay the stack frame
    // needed by a resource's potentially large write-planning callback.
    #[inline(never)]
    fn preflight_full(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError> {
        let Self {
            intent,
            preparation: _,
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let adapter = resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter();
        let view = PreflightItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        let prepared =
            adapter.preflight(key.as_ref().expect("active item retains its key"), view, cx)?;
        self.preparation = PreparationState::Prepared(prepared);
        Ok(())
    }

    fn teardown_after_finish(&mut self) {
        drop(self.intent.take());
        drop(std::mem::replace(
            &mut self.preparation,
            PreparationState::Unprepared,
        ));
        drop(std::mem::replace(
            &mut self.observation,
            ObservationState::Unobserved,
        ));
        drop(self.retained_predicate.take());
        drop(self.local.take());
        drop(self.key.take());
        // Keep the immutable typed binding in this worker-local box. This
        // intentionally extends the object/adapter lease until the box is
        // rebound to another resource or the WorkerContext is dropped. The
        // number of retained leases is bounded by the worker's peak item
        // count, itself bounded by RuntimeConfig.
    }
}

/// Which half of exact-once item cleanup was running if a typed batch unwinds.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum BatchFinishStage {
    Callback,
    Teardown,
}

/// One homogeneous, contiguous item batch behind a single core vtable.
///
/// A live batch is the transaction's complete item sequence while every access
/// uses the same adapter type. An access through another adapter type uses
/// [`Self::drain_active_into`] to materialize that sequence into the ordinary
/// erased-item representation before lookup continues.
pub(crate) trait ErasedItemBatch: Any {
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn adapter_type_id(&self) -> TypeId;
    fn active_len(&self) -> usize;
    fn commit_shape(&self) -> (bool, bool);
    fn is_preflight_free_read_only(&self) -> bool;
    fn select_direct_commit(&mut self) -> bool;
    fn direct_preflight(
        &mut self,
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError>;
    fn direct_acquire_all(&mut self, owner: OwnerId) -> Result<(), AcquireError>;
    fn direct_validate(&self, occ_commit_id: Option<OccCommitId>) -> Result<(), CheckError>;
    fn direct_install(&mut self, occ_commit_id: Option<OccCommitId>);
    fn direct_requires_release(&self) -> bool;
    fn direct_release_all(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault>;
    fn direct_recover_after_callback_panic(
        &mut self,
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault>;
    fn teardown_direct_plan(&mut self) -> Result<(), ()>;
    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError>;
    fn upgrade_predicates(&mut self, cx: &PredicateContext<'_>) -> Result<(), CheckError>;
    fn validate_preflight_free_reads(
        &mut self,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError>;
    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError>;
    fn install(&mut self, cx: &mut InstallContext<'_>);
    fn finish_and_teardown(
        &mut self,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
        stage: &mut BatchFinishStage,
    );
    fn drain_active_into(&mut self, destination: &mut [Option<Box<dyn ErasedItem>>]);
    fn dispose_retained_resources(&mut self);
}

/// Worker-pooled typed storage shared by exact unique batches and homogeneous
/// scalar transactions.
pub(crate) struct TypedItemBatch<A: TransactionalResource> {
    // Intent is execution-hot only for the subset of accesses that stage a
    // write. Keeping the dense optional values parallel to `items` removes
    // their full maximum footprint from every point-operation item stride.
    // Empty slots remain materialized at the worker's high-water mark, just
    // like `items`, so reactivation does not rewrite a potentially large
    // `None` value. Declaring this sidecar first also makes fail-safe field
    // drop destroy intents while every retained item resource is still alive.
    intents: Vec<Option<A::Intent>>,
    // Full commit preparation is phase-disjoint from execution. Keeping it in
    // a parallel allocation makes the execution-hot item stride smaller. The
    // vector remains empty for the prepared-free read-only lane.
    preparations: Vec<PreparationState<A::Prepared>>,
    // The alternate plan remains concrete behind this one batch-level vtable
    // and retains only empty reusable frames between transactions.
    direct_plan: Option<Box<dyn ErasedDirectCommitPlan<A>>>,
    selected_direct_capability: Option<&'static DirectCommitCapability<A>>,
    // Folded once for each distinct exact registered-resource binding while
    // items are activated. `None` is either the empty state or an ineligible
    // state, distinguished by `direct_capability_ineligible`.
    direct_capability_summary: Option<&'static DirectCommitCapability<A>>,
    direct_capability_ineligible: bool,
    // Direct preparation deliberately leaves the generic per-item sidecar
    // empty. This exact batch-level marker selects finish-without-Prepared.
    direct_prepared: bool,
    items: Vec<ItemData<A>>,
    active_len: usize,
    // One advisory bit per exact live binding. A clear bit proves absence;
    // a set bit is only a possible match and must never replace the exact Arc
    // identity comparison. Keeping this summary alongside the batch lets a
    // scalar access to a new binding extend the unindexed suffix without
    // scanning every earlier item.
    active_binding_filter: u64,
    // These facts are recorded while each appended or revisited item is already
    // hot. Writes and predicates are monotone during execution. Read-only lane
    // eligibility is deliberately conservative once any access disproves it,
    // and adapter preflight cannot later add or remove execution-time state.
    has_writes: bool,
    has_predicates: bool,
    all_preflight_free_reads: bool,
}

impl<A: TransactionalResource> TypedItemBatch<A> {
    pub(crate) const fn new() -> Self {
        Self {
            intents: Vec::new(),
            preparations: Vec::new(),
            direct_plan: None,
            selected_direct_capability: None,
            direct_capability_summary: None,
            direct_capability_ineligible: false,
            direct_prepared: false,
            items: Vec::new(),
            active_len: 0,
            active_binding_filter: 0,
            has_writes: false,
            has_predicates: false,
            all_preflight_free_reads: true,
        }
    }

    #[inline]
    pub(crate) fn active_len(&self) -> usize {
        self.active_len
    }

    #[inline]
    pub(crate) fn pooled_len(&self) -> usize {
        debug_assert_eq!(self.intents.len(), self.items.len());
        self.items.len()
    }

    #[inline]
    pub(crate) fn try_reserve_for_len(&mut self, needed: usize) -> Result<(), CapacityError> {
        debug_assert_eq!(self.intents.len(), self.items.len());
        if needed > self.items.len() {
            self.items
                .try_reserve_exact(needed - self.items.len())
                .map_err(|_| CapacityError::ItemLimit)?;
        }
        self.intents
            .try_reserve_exact(needed.saturating_sub(self.intents.len()))
            .map_err(|_| CapacityError::ItemLimit)?;
        Ok(())
    }

    #[inline]
    pub(crate) fn pooled_item_mut(&mut self, slot: usize) -> Option<&mut ItemData<A>> {
        debug_assert_eq!(slot, self.active_len);
        self.items.get_mut(slot)
    }

    #[inline]
    pub(crate) fn activate_reinitialized<const NEW_BINDING: bool>(
        &mut self,
        direct_capability: Option<&'static DirectCommitCapability<A>>,
    ) {
        debug_assert!(self.active_len < self.items.len());
        debug_assert_eq!(self.intents.len(), self.items.len());
        debug_assert!(self.intents[self.active_len].is_none());
        self.activate_current_item::<NEW_BINDING>(direct_capability);
    }

    #[inline]
    pub(crate) fn push_active<const NEW_BINDING: bool>(
        &mut self,
        item: ItemData<A>,
        direct_capability: Option<&'static DirectCommitCapability<A>>,
    ) {
        debug_assert_eq!(self.active_len, self.items.len());
        debug_assert_eq!(self.intents.len(), self.items.len());
        self.intents.push(None);
        self.items.push(item);
        self.activate_current_item::<NEW_BINDING>(direct_capability);
    }

    #[inline(always)]
    fn activate_current_item<const NEW_BINDING: bool>(
        &mut self,
        direct_capability: Option<&'static DirectCommitCapability<A>>,
    ) {
        if NEW_BINDING {
            let resource = self.items[self.active_len]
                .resource
                .as_ref()
                .expect("an activated typed item retains its resource");
            debug_assert!(
                !self.items[..self.active_len]
                    .iter()
                    .rev()
                    .any(|item| item.retains_binding(resource)),
                "new-binding activation must introduce an exact binding"
            );
            let binding_bit = active_binding_filter_bit(resource);
            self.fold_direct_capability(direct_capability);
            self.active_binding_filter |= binding_bit;
        } else {
            #[cfg(debug_assertions)]
            {
                let resource = self.items[self.active_len]
                    .resource
                    .as_ref()
                    .expect("an activated typed item retains its resource");
                debug_assert!(
                    self.items[..self.active_len]
                        .iter()
                        .rev()
                        .any(|item| item.retains_binding(resource)),
                    "existing-binding activation must reuse an exact binding"
                );
                debug_assert!(direct_capability.is_none());
            }
        }
        self.active_len += 1;
    }

    #[inline(always)]
    fn fold_direct_capability(&mut self, capability: Option<&'static DirectCommitCapability<A>>) {
        if self.direct_capability_ineligible {
            return;
        }
        let Some(capability) = capability else {
            self.direct_capability_summary = None;
            self.direct_capability_ineligible = true;
            return;
        };
        match self.direct_capability_summary {
            None => self.direct_capability_summary = Some(capability),
            Some(current) if std::ptr::eq(current, capability) => {}
            Some(_) => {
                self.direct_capability_summary = None;
                self.direct_capability_ineligible = true;
            }
        }
    }

    #[inline]
    pub(crate) fn active_item_parts_mut(
        &mut self,
        slot: usize,
    ) -> (&mut ItemData<A>, &mut Option<A::Intent>) {
        debug_assert!(slot < self.active_len);
        (&mut self.items[slot], &mut self.intents[slot])
    }

    #[inline]
    pub(crate) fn active_item(&self, slot: usize) -> &ItemData<A> {
        debug_assert!(slot < self.active_len);
        &self.items[slot]
    }

    /// Returns whether the live prefix already retains this exact registered
    /// resource binding.
    ///
    /// A later proven-unique group may skip all key hashing only when its
    /// binding is distinct from every earlier group. Comparing retained `Arc`
    /// allocation identities is exact while the live items keep those
    /// allocations alive, recognizes cloned handles, and requires no hashing.
    #[inline]
    pub(crate) fn contains_active_binding(&self, resource: &RegisteredResource<A>) -> bool {
        if self.binding_definitely_absent(resource) {
            return false;
        }
        self.items[..self.active_len]
            .iter()
            .rev()
            .any(|item| item.retains_binding(resource))
    }

    /// Returns true only when the one-word summary proves that this exact
    /// binding cannot occur in the live prefix. Filter collisions return false
    /// and retain the ordinary exact-lookup path.
    #[inline(always)]
    pub(crate) fn binding_definitely_absent(&self, resource: &RegisteredResource<A>) -> bool {
        self.active_binding_filter & active_binding_filter_bit(resource) == 0
    }

    #[inline]
    pub(crate) fn note_active_item_shape(&mut self, slot: usize) {
        debug_assert!(slot < self.active_len);
        let item = &self.items[slot];
        let intent = &self.intents[slot];
        if intent.is_some() {
            self.has_writes = true;
        }
        self.has_predicates |= matches!(item.observation, ObservationState::Predicate(_));
        if self.all_preflight_free_reads {
            self.all_preflight_free_reads =
                item.is_preflight_free_read_candidate_typed(intent, &PreparationState::Unprepared);
        }
    }

    #[inline]
    fn reset_shape(&mut self) {
        self.active_binding_filter = 0;
        self.direct_capability_summary = None;
        self.direct_capability_ineligible = false;
        self.has_writes = false;
        self.has_predicates = false;
        self.all_preflight_free_reads = true;
    }

    #[inline(always)]
    fn exact_direct_capability(&self) -> Option<&'static DirectCommitCapability<A>> {
        debug_assert_eq!(self.intents.len(), self.items.len());
        if self.active_len == 0
            || !self.has_writes
            || self.has_predicates
            || !self.preparations.is_empty()
            || self.direct_prepared
        {
            return None;
        }
        if self.direct_capability_ineligible {
            return None;
        }
        self.direct_capability_summary
    }
}

/// Maps one stable, nonzero binding allocation address to an advisory bit.
/// Multiplication spreads allocator alignment bits before selecting the high
/// six bits. Collisions are expected and are always resolved by exact binding
/// equality when a positive result matters.
#[inline(always)]
fn active_binding_filter_bit<A: TransactionalResource>(resource: &RegisteredResource<A>) -> u64 {
    let address = resource.binding_identity().get() as u64;
    let bit = address.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> 58;
    1_u64 << bit
}

impl<A: TransactionalResource> ErasedItemBatch for TypedItemBatch<A> {
    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    #[inline]
    fn adapter_type_id(&self) -> TypeId {
        TypeId::of::<A>()
    }

    fn active_len(&self) -> usize {
        self.active_len
    }

    fn commit_shape(&self) -> (bool, bool) {
        (self.has_writes, self.has_predicates)
    }

    fn is_preflight_free_read_only(&self) -> bool {
        self.all_preflight_free_reads
    }

    #[inline]
    fn select_direct_commit(&mut self) -> bool {
        self.selected_direct_capability = self.exact_direct_capability();
        self.selected_direct_capability.is_some()
    }

    fn direct_preflight(
        &mut self,
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError> {
        let capability = self
            .selected_direct_capability
            .ok_or_else(|| AdapterFault::invariant(crate::error::AdapterPhase::Preflight))?;
        let replace = self
            .direct_plan
            .as_ref()
            .is_some_and(|plan| !std::ptr::eq(plan.capability(), capability));
        if replace {
            drop(self.direct_plan.take());
        }
        if self.direct_plan.is_none() {
            self.direct_plan = Some(capability.create_plan());
        }
        self.direct_prepared = true;
        self.direct_plan
            .as_mut()
            .expect("direct plan was installed")
            .prepare(
                &mut self.items[..self.active_len],
                &mut self.intents[..self.active_len],
                runtime_id,
                max_locks,
            )
    }

    fn direct_acquire_all(&mut self, owner: OwnerId) -> Result<(), AcquireError> {
        let Self {
            direct_plan,
            items,
            active_len,
            ..
        } = self;
        direct_plan
            .as_mut()
            .ok_or_else(|| AcquireError::Fault(AdapterFault::invariant(AdapterPhase::Acquire)))?
            .acquire_all(&items[..*active_len], owner)
    }

    fn direct_validate(&self, occ_commit_id: Option<OccCommitId>) -> Result<(), CheckError> {
        self.direct_plan
            .as_ref()
            .ok_or_else(|| CheckError::Fault(AdapterFault::invariant(AdapterPhase::Validation)))?
            .validate(
                &self.items[..self.active_len],
                &self.intents[..self.active_len],
                occ_commit_id,
            )
    }

    fn direct_install(&mut self, occ_commit_id: Option<OccCommitId>) {
        self.direct_plan
            .as_mut()
            .expect("direct install owns its plan")
            .install(
                &mut self.items[..self.active_len],
                &self.intents[..self.active_len],
                occ_commit_id,
            );
    }

    fn direct_requires_release(&self) -> bool {
        self.direct_plan
            .as_ref()
            .is_some_and(|plan| plan.requires_release())
    }

    fn direct_release_all(&mut self, disposition: LockDisposition) -> Result<(), AdapterFault> {
        let Self {
            direct_plan,
            items,
            active_len,
            ..
        } = self;
        direct_plan
            .as_mut()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?
            .release_all(&items[..*active_len], disposition)
    }

    fn direct_recover_after_callback_panic(
        &mut self,
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault> {
        let Self {
            direct_plan,
            items,
            active_len,
            ..
        } = self;
        direct_plan
            .as_mut()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?
            .recover_after_callback_panic(&items[..*active_len], disposition)
    }

    fn teardown_direct_plan(&mut self) -> Result<(), ()> {
        let Some(plan) = self.direct_plan.as_mut() else {
            return Err(());
        };
        let teardown = catch_unwind(AssertUnwindSafe(|| plan.teardown_adapter_state()));
        if !matches!(teardown, Ok(Ok(()))) {
            let plan = self
                .direct_plan
                .take()
                .expect("a failed direct teardown retains its quarantined plan");
            std::mem::forget(plan);
            return Err(());
        }
        Ok(())
    }

    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError> {
        debug_assert!(self.preparations.is_empty());
        assert_eq!(
            self.intents.len(),
            self.items.len(),
            "typed items and intent sidecar diverged before preflight"
        );
        if self.preparations.capacity() < self.active_len {
            self.preparations
                .try_reserve_exact(self.active_len)
                .map_err(|_| CapacityError::ItemLimit)?;
        }
        self.preparations
            .resize_with(self.active_len, || PreparationState::Unprepared);

        for ((item, intent), preparation) in self.items[..self.active_len]
            .iter_mut()
            .zip(&mut self.intents[..self.active_len])
            .zip(&mut self.preparations)
        {
            if !item.select_preflight_free_read(intent, preparation)? {
                item.preflight_full(intent, preparation, cx)?;
            }
        }
        Ok(())
    }

    fn upgrade_predicates(&mut self, cx: &PredicateContext<'_>) -> Result<(), CheckError> {
        debug_assert_eq!(self.preparations.len(), self.active_len);
        debug_assert_eq!(self.intents.len(), self.items.len());
        for (item, preparation) in self.items[..self.active_len]
            .iter_mut()
            .zip(&self.preparations)
        {
            item.upgrade_predicate_typed(preparation, cx)?;
        }
        Ok(())
    }

    fn validate_preflight_free_reads(
        &mut self,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError> {
        debug_assert!(self.preparations.is_empty());
        assert_eq!(
            self.intents.len(),
            self.items.len(),
            "typed items and intent sidecar diverged before prepared-free validation"
        );
        for (item, intent) in self.items[..self.active_len]
            .iter_mut()
            .zip(&self.intents[..self.active_len])
        {
            item.validate_preflight_free_read_typed(intent, cx)?;
        }
        Ok(())
    }

    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError> {
        debug_assert_eq!(self.preparations.len(), self.active_len);
        debug_assert_eq!(self.intents.len(), self.items.len());
        for (item, preparation) in self.items[..self.active_len].iter().zip(&self.preparations) {
            item.validate_typed(preparation, cx)?;
        }
        Ok(())
    }

    fn install(&mut self, cx: &mut InstallContext<'_>) {
        debug_assert_eq!(self.preparations.len(), self.active_len);
        assert_eq!(
            self.intents.len(),
            self.items.len(),
            "typed items and intent sidecar diverged before install"
        );
        for ((item, intent), preparation) in self.items[..self.active_len]
            .iter_mut()
            .zip(&self.intents[..self.active_len])
            .zip(&mut self.preparations)
        {
            if intent.is_some() {
                item.install_typed(intent, preparation, cx);
            }
        }
    }

    fn finish_and_teardown(
        &mut self,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
        stage: &mut BatchFinishStage,
    ) {
        assert_eq!(
            self.intents.len(),
            self.items.len(),
            "typed items and intent sidecar diverged before finish"
        );
        if self.direct_prepared {
            debug_assert!(self.preparations.is_empty());
            let capability = self
                .selected_direct_capability
                .expect("a direct-prepared batch retains its selected capability");
            let drop_only_committed = disposition == FinishDisposition::Committed
                && capability.has_drop_only_committed_finish();
            if drop_only_committed {
                for item_slot in (0..self.active_len).rev() {
                    let item = &mut self.items[item_slot];
                    let intent = &mut self.intents[item_slot];
                    let mut preparation = PreparationState::Unprepared;
                    *stage = BatchFinishStage::Teardown;
                    item.teardown_after_finish(intent, &mut preparation);
                }
            } else {
                for item_slot in (0..self.active_len).rev() {
                    let item = &mut self.items[item_slot];
                    let intent = &mut self.intents[item_slot];
                    let mut preparation = PreparationState::Unprepared;
                    *stage = BatchFinishStage::Callback;
                    item.finish_typed(intent, &mut preparation, disposition, cx);
                    *stage = BatchFinishStage::Teardown;
                    item.teardown_after_finish(intent, &mut preparation);
                }
            }
            self.direct_prepared = false;
            self.selected_direct_capability = None;
        } else if self.preparations.len() == self.active_len {
            for item_slot in (0..self.active_len).rev() {
                let item = &mut self.items[item_slot];
                let intent = &mut self.intents[item_slot];
                let preparation = &mut self.preparations[item_slot];
                *stage = BatchFinishStage::Callback;
                item.finish_typed(intent, preparation, disposition, cx);
                *stage = BatchFinishStage::Teardown;
                item.teardown_after_finish(intent, preparation);
            }
            self.preparations.clear();
        } else {
            // The preparation vector is absent only before full preflight or
            // in the all-preflight-free read lane. A committed outcome can
            // reach this branch only after that lane's direct validation.
            debug_assert!(self.preparations.is_empty());
            debug_assert!(
                disposition == FinishDisposition::Aborted || self.all_preflight_free_reads
            );
            for item_slot in (0..self.active_len).rev() {
                let item = &mut self.items[item_slot];
                let intent = &mut self.intents[item_slot];
                let mut preparation = if disposition == FinishDisposition::Committed {
                    PreparationState::PreflightFreeRead
                } else {
                    PreparationState::Unprepared
                };
                *stage = BatchFinishStage::Callback;
                item.finish_typed(intent, &mut preparation, disposition, cx);
                *stage = BatchFinishStage::Teardown;
                item.teardown_after_finish(intent, &mut preparation);
            }
        }
        debug_assert!(self.intents[..self.active_len].iter().all(Option::is_none));
        self.active_len = 0;
        self.reset_shape();
    }

    fn drain_active_into(&mut self, destination: &mut [Option<Box<dyn ErasedItem>>]) {
        let active_len = self.active_len;
        debug_assert!(destination.len() >= active_len);
        debug_assert!(self.preparations.is_empty());
        debug_assert!(!self.direct_prepared);
        debug_assert!(self.selected_direct_capability.is_none());
        assert_eq!(
            self.intents.len(),
            self.items.len(),
            "typed items and intent sidecar diverged before materialization"
        );
        let items = self.items.drain(..active_len);
        let intents = self.intents.drain(..active_len);
        for (item_slot, (data, intent)) in items.zip(intents).enumerate() {
            // The destination's retained resource was disposed under its own
            // unwind boundary before this infallible replacement phase. Its
            // prior item box therefore contains no adapter-owned live state.
            drop(destination[item_slot].replace(Box::new(ItemBox::from_data(data, intent))));
        }
        debug_assert_eq!(self.intents.len(), self.items.len());
        debug_assert!(self.intents.iter().all(Option::is_none));
        self.active_len = 0;
        self.reset_shape();
    }

    fn dispose_retained_resources(&mut self) {
        debug_assert_eq!(self.active_len, 0);
        debug_assert!(self.preparations.is_empty());
        debug_assert!(!self.direct_prepared);
        debug_assert!(self.selected_direct_capability.is_none());
        debug_assert_eq!(self.intents.len(), self.items.len());
        debug_assert!(self.intents.iter().all(Option::is_none));
        for item in &mut self.items {
            item.dispose_retained_resource();
        }
    }
}

/// Scoped operation-time access to one typed transaction item.
///
/// An entry is created only by `Transaction::with_item`, cannot escape its
/// closure, and is structurally neither `Send` nor `Sync`.
pub struct Entry<'entry, A: TransactionalResource> {
    intent: &'entry mut Option<A::Intent>,
    observation: &'entry mut ObservationState<A::Observation, A::Predicate>,
    local: &'entry mut Option<A::Local>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'entry, A: TransactionalResource> Entry<'entry, A> {
    #[inline]
    pub(crate) fn new(item: &'entry mut ItemBox<A>) -> Self {
        Self {
            intent: &mut item.intent,
            observation: &mut item.observation,
            local: &mut item.local,
            not_send_sync: PhantomData,
        }
    }

    #[inline]
    pub(crate) fn new_batch(
        item: &'entry mut ItemData<A>,
        intent: &'entry mut Option<A::Intent>,
    ) -> Self {
        Self {
            intent,
            observation: &mut item.observation,
            local: &mut item.local,
            not_send_sync: PhantomData,
        }
    }

    #[inline]
    pub fn local(&self) -> &A::Local {
        self.local
            .as_ref()
            .expect("active item retains its local state")
    }

    #[inline]
    pub fn local_mut(&mut self) -> &mut A::Local {
        self.local
            .as_mut()
            .expect("active item retains its local state")
    }

    #[inline]
    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.observation.as_ref::<A>()
    }

    #[inline]
    pub fn intent(&self) -> Option<&A::Intent> {
        self.intent.as_ref()
    }

    #[inline]
    pub fn intent_mut(&mut self) -> Option<&mut A::Intent> {
        self.intent.as_mut()
    }

    /// Records the first ordinary read observation for this logical item.
    #[inline]
    pub fn record_read(&mut self, observation: A::Observation) -> Result<(), AccessError> {
        if !matches!(self.observation, ObservationState::Unobserved) {
            return Err(InvalidUse::IllegalItemState.into());
        }
        *self.observation = ObservationState::Read(observation);
        Ok(())
    }

    /// Records the first optimistic predicate for this logical item.
    #[inline]
    pub fn record_predicate(&mut self, predicate: A::Predicate) -> Result<(), AccessError> {
        if !matches!(self.observation, ObservationState::Unobserved) {
            return Err(InvalidUse::IllegalItemState.into());
        }
        *self.observation = ObservationState::Predicate(predicate);
        Ok(())
    }

    /// Stores an adapter-composed deferred intent.
    #[inline]
    pub fn stage(&mut self, intent: A::Intent) -> Result<(), AccessError> {
        *self.intent = Some(intent);
        Ok(())
    }
}

mod sealed {
    pub trait Sealed {}
}

/// Private object-safe vtable used only for heterogeneous core dispatch.
pub(crate) trait ErasedItem: sealed::Sealed {
    fn identity_hash(&self) -> u64;
    fn retains_binding_identity(&self, binding_identity: NonZeroUsize) -> bool;
    fn matches_identity(
        &self,
        object_id: ObjectId,
        resource_class: ResourceClass,
        adapter_type_id: TypeId,
        key_type_id: TypeId,
        key: &dyn Any,
    ) -> bool;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn has_intent(&self) -> bool;
    fn has_predicate(&self) -> bool;
    fn is_preflight_free_read_candidate(&self) -> bool;

    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError>;
    #[allow(dead_code, reason = "reserved for the negotiated opaque profile")]
    fn revalidate_for_opacity(
        &self,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError>;
    fn upgrade_predicate(&mut self, cx: &PredicateContext<'_>) -> Result<(), CheckError>;
    fn validate_preflight_free_read(
        &mut self,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError>;
    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError>;
    fn install(&mut self, cx: &mut InstallContext<'_>);
    fn finish(&mut self, disposition: FinishDisposition, cx: &mut FinishContext<'_>);
    fn teardown_after_finish(&mut self);
    fn dispose_retained_resource(&mut self);
}

impl<A: TransactionalResource> sealed::Sealed for ItemBox<A> {}

impl<A: TransactionalResource> ErasedItem for ItemBox<A> {
    #[inline]
    fn identity_hash(&self) -> u64 {
        item_hash(
            self.resource
                .as_ref()
                .expect("active item retains its resource"),
            self.key.as_ref().expect("active item retains its key"),
        )
    }

    #[inline]
    fn retains_binding_identity(&self, binding_identity: NonZeroUsize) -> bool {
        self.resource
            .as_ref()
            .is_some_and(|resource| resource.binding_identity() == binding_identity)
    }

    fn matches_identity(
        &self,
        object_id: ObjectId,
        resource_class: ResourceClass,
        adapter_type_id: TypeId,
        key_type_id: TypeId,
        key: &dyn Any,
    ) -> bool {
        self.resource.as_ref().is_some_and(|resource| {
            resource.object_id() == object_id && resource.resource_class() == resource_class
        }) && adapter_type_id == TypeId::of::<A>()
            && key_type_id == TypeId::of::<A::Key>()
            && key.downcast_ref::<A::Key>() == self.key.as_ref()
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn has_intent(&self) -> bool {
        self.intent.is_some()
    }

    fn has_predicate(&self) -> bool {
        matches!(self.observation, ObservationState::Predicate(_))
    }

    #[inline]
    fn is_preflight_free_read_candidate(&self) -> bool {
        matches!(self.preparation, PreparationState::Unprepared)
            && self.intent.is_none()
            && matches!(self.observation, ObservationState::Read(_))
            && self.retained_predicate.is_none()
            && self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .preflight_free_read_capability()
                .is_some()
    }

    #[inline(always)]
    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError> {
        if !matches!(self.preparation, PreparationState::Unprepared) {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Preflight,
            )
            .into());
        }

        if self.intent.is_none()
            && matches!(self.observation, ObservationState::Read(_))
            && self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .preflight_free_read_capability()
                .is_some()
        {
            self.preparation = PreparationState::PreflightFreeRead;
            return Ok(());
        }

        self.preflight_full(cx)
    }

    fn revalidate_for_opacity(
        &self,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        match &self.observation {
            ObservationState::Unobserved => Ok(ObservationOrder::Unordered),
            ObservationState::Read(observation)
            | ObservationState::UpgradedPredicate(observation) => {
                self.resource
                    .as_ref()
                    .expect("active item retains its resource")
                    .adapter()
                    .revalidate_read(
                        self.key.as_ref().expect("active item retains its key"),
                        observation,
                        cx,
                    )?;
                Ok(crate::adapter::OpacityToken::observation_order(observation))
            }
            ObservationState::Predicate(predicate) => self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .revalidate_predicate(
                    self.key.as_ref().expect("active item retains its key"),
                    predicate,
                    cx,
                ),
        }
    }

    fn upgrade_predicate(&mut self, cx: &PredicateContext<'_>) -> Result<(), CheckError> {
        let ObservationState::Predicate(predicate) = &self.observation else {
            return Ok(());
        };
        let PreparationState::Prepared(prepared) = &self.preparation else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::PredicateUpgrade,
            )
            .into());
        };
        let observation = self
            .resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter()
            .upgrade_predicate(
                self.key.as_ref().expect("active item retains its key"),
                predicate,
                prepared,
                cx,
            )?;
        let prior = std::mem::replace(&mut self.observation, ObservationState::Unobserved);
        let ObservationState::Predicate(predicate) = prior else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::PredicateUpgrade,
            )
            .into());
        };
        self.retained_predicate = Some(predicate);
        self.observation = ObservationState::UpgradedPredicate(observation);
        Ok(())
    }

    fn validate_preflight_free_read(
        &mut self,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let Self {
            intent,
            preparation,
            observation,
            retained_predicate,
            local: _,
            key,
            resource,
        } = self;
        if !matches!(preparation, PreparationState::Unprepared)
            || intent.is_some()
            || retained_predicate.is_some()
        {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into());
        }
        let ObservationState::Read(observation) = observation else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into());
        };
        let resource = resource.as_ref().expect("active item retains its resource");
        let adapter = resource.adapter();
        let capability = adapter.preflight_free_read_capability().ok_or_else(|| {
            crate::error::AdapterFault::invariant(crate::error::AdapterPhase::Validation)
        })?;

        // Mark the selection before entering adapter code. If the callback
        // faults or unwinds, the reversible abort still routes this item
        // through ordinary prepared-free abort cleanup. If it returns, a
        // committed finish uses the capability's matching callback.
        *preparation = PreparationState::PreflightFreeRead;
        capability.validate(
            adapter,
            key.as_ref().expect("active item retains its key"),
            observation,
            cx,
        )
    }

    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError> {
        if matches!(self.preparation, PreparationState::PreflightFreeRead) {
            let ObservationState::Read(observation) = &self.observation else {
                return Err(crate::error::AdapterFault::invariant(
                    crate::error::AdapterPhase::Validation,
                )
                .into());
            };
            let resource = self
                .resource
                .as_ref()
                .expect("active item retains its resource");
            let adapter = resource.adapter();
            let capability = adapter.preflight_free_read_capability().ok_or_else(|| {
                crate::error::AdapterFault::invariant(crate::error::AdapterPhase::Validation)
            })?;
            let restricted = cx.preflight_free_read_context();
            return capability.validate(
                adapter,
                self.key.as_ref().expect("active item retains its key"),
                observation,
                &restricted,
            );
        }

        let PreparationState::Prepared(prepared) = &self.preparation else {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into());
        };
        match &self.observation {
            ObservationState::Unobserved => Ok(()),
            ObservationState::Read(observation)
            | ObservationState::UpgradedPredicate(observation) => self
                .resource
                .as_ref()
                .expect("active item retains its resource")
                .adapter()
                .validate_read(
                    self.key.as_ref().expect("active item retains its key"),
                    observation,
                    prepared,
                    cx,
                ),
            ObservationState::Predicate(_) => Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Validation,
            )
            .into()),
        }
    }

    fn install(&mut self, cx: &mut InstallContext<'_>) {
        let Self {
            intent,
            preparation,
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let Some(intent_ref) = intent.as_ref() else {
            return;
        };
        let PreparationState::Prepared(prepared) = preparation else {
            panic!("sto-core invariant: install called without prepared state");
        };
        let view = InstallItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent_ref,
        );
        resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter()
            .install(
                key.as_ref().expect("active item retains its key"),
                view,
                prepared,
                cx,
            );

        let old = std::mem::replace(preparation, PreparationState::Unprepared);
        let PreparationState::Prepared(prepared) = old else {
            unreachable!("prepared state was checked before install");
        };
        *preparation = PreparationState::Installed(prepared);
    }

    fn finish(&mut self, disposition: FinishDisposition, cx: &mut FinishContext<'_>) {
        let Self {
            intent,
            preparation,
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let resource = resource.as_ref().expect("active item retains its resource");
        let adapter = resource.adapter();
        if matches!(preparation, PreparationState::PreflightFreeRead)
            && disposition == FinishDisposition::Committed
        {
            let capability: &PreflightFreeReadCapability<A> = adapter
                .preflight_free_read_capability()
                .expect("preflight-free read capability disappeared before committed finish");
            if let Some(finish_committed) = capability.finish_committed_callback() {
                let view = FinishItem::new(
                    local.as_mut().expect("active item retains its local state"),
                    observation.as_ref::<A>(),
                    intent,
                );
                finish_committed(
                    adapter,
                    key.as_ref().expect("active item retains its key"),
                    view,
                    cx,
                );
            }
            return;
        }

        let prepared = match preparation {
            PreparationState::Unprepared | PreparationState::PreflightFreeRead => None,
            PreparationState::Prepared(prepared) | PreparationState::Installed(prepared) => {
                Some(prepared)
            }
        };
        let view = FinishItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        adapter.finish(
            key.as_ref().expect("active item retains its key"),
            view,
            prepared,
            disposition,
            cx,
        );
    }

    fn teardown_after_finish(&mut self) {
        ItemBox::teardown_after_finish(self);
    }

    fn dispose_retained_resource(&mut self) {
        ItemBox::dispose_retained_resource(self);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::num::NonZeroUsize;

    struct Dummy;
    struct Token {
        _words: [usize; 2],
    }

    #[derive(Clone, Copy)]
    enum LayoutRole {
        Record,
        #[allow(dead_code)]
        Directory,
    }

    struct LayoutIntent {
        _words: [usize; 4],
        _niche: NonZeroUsize,
    }

    struct LayoutPrepared {
        _words: [usize; 3],
    }

    impl crate::adapter::OpacityToken for Token {
        fn observation_order(&self) -> ObservationOrder {
            ObservationOrder::Unordered
        }
    }

    impl TransactionalResource for Dummy {
        type Key = u64;
        type Local = LayoutRole;
        type Observation = Token;
        type Predicate = crate::adapter::NoPredicate;
        type Intent = LayoutIntent;
        type Prepared = LayoutPrepared;

        fn new_local(&self, _: &u64) -> Result<LayoutRole, crate::error::ItemInitError> {
            Ok(LayoutRole::Record)
        }
        fn preflight(
            &self,
            _: &u64,
            _: PreflightItem<'_, Self>,
            _: &mut PreflightContext<'_>,
        ) -> Result<LayoutPrepared, PrepareError> {
            Ok(LayoutPrepared { _words: [0; 3] })
        }
        fn revalidate_read(
            &self,
            _: &u64,
            _: &Token,
            _: &ExecutionCheckContext<'_>,
        ) -> Result<(), CheckError> {
            Ok(())
        }
        fn revalidate_predicate(
            &self,
            _: &u64,
            predicate: &crate::adapter::NoPredicate,
            _: &ExecutionCheckContext<'_>,
        ) -> Result<ObservationOrder, CheckError> {
            match *predicate {}
        }
        fn upgrade_predicate(
            &self,
            _: &u64,
            predicate: &crate::adapter::NoPredicate,
            _: &LayoutPrepared,
            _: &PredicateContext<'_>,
        ) -> Result<Token, CheckError> {
            match *predicate {}
        }
        fn validate_read(
            &self,
            _: &u64,
            _: &Token,
            _: &LayoutPrepared,
            _: &ValidationContext<'_>,
        ) -> Result<(), CheckError> {
            Ok(())
        }
        fn install(
            &self,
            _: &u64,
            _: InstallItem<'_, Self>,
            _: &mut LayoutPrepared,
            _: &mut InstallContext<'_>,
        ) {
        }
        fn finish(
            &self,
            _: &u64,
            _: FinishItem<'_, Self>,
            _: Option<&mut LayoutPrepared>,
            _: FinishDisposition,
            _: &mut FinishContext<'_>,
        ) {
        }
    }

    #[test]
    fn observation_state_exposes_each_legal_borrow_shape() {
        let state: ObservationState<Token, crate::adapter::NoPredicate> =
            ObservationState::Read(Token { _words: [0; 2] });
        assert!(matches!(state.as_ref::<Dummy>(), ObservationRef::Read(_)));

        #[allow(dead_code)]
        enum LegacyPreparationState<P> {
            Unprepared,
            Prepared(P),
            Installed(P),
        }
        #[allow(dead_code)]
        struct LegacyItemBox {
            intent: Option<LayoutIntent>,
            preparation: LegacyPreparationState<LayoutPrepared>,
            observation: ObservationState<Token, crate::adapter::NoPredicate>,
            retained_predicate: Option<crate::adapter::NoPredicate>,
            local: Option<LayoutRole>,
            key: Option<u64>,
            resource: Option<RegisteredResource<Dummy>>,
        }

        assert_eq!(
            std::mem::size_of::<PreparationState<()>>(),
            std::mem::size_of::<LegacyPreparationState<()>>()
        );
        assert_eq!(
            std::mem::size_of::<PreparationState<[usize; 4]>>(),
            std::mem::size_of::<LegacyPreparationState<[usize; 4]>>()
        );
        assert_eq!(
            std::mem::size_of::<ItemBox<Dummy>>(),
            std::mem::size_of::<LegacyItemBox>()
        );
        assert_eq!(
            std::mem::align_of::<ItemBox<Dummy>>(),
            std::mem::align_of::<LegacyItemBox>()
        );
    }

    #[test]
    fn typed_batch_keeps_intent_and_preparation_out_of_the_execution_stride() {
        // These associated-type footprints mirror TableAdapter on 64-bit
        // targets: key 8, local 1, observation 16, intent 40, prepared 24,
        // and resource handle 8 bytes. Pin the 56-byte hot TPCC item stride;
        // the 40-byte optional intent and 32-byte preparation live in cold,
        // phase-specific parallel streams while ItemBox remains unchanged.
        if cfg!(target_pointer_width = "64") {
            assert_eq!(std::mem::size_of::<LayoutIntent>(), 40);
            assert_eq!(std::mem::size_of::<Option<LayoutIntent>>(), 40);
            assert_eq!(std::mem::size_of::<LayoutPrepared>(), 24);
            assert_eq!(std::mem::size_of::<PreparationState<LayoutPrepared>>(), 32);
            assert_eq!(std::mem::size_of::<ItemData<Dummy>>(), 56);
            assert_eq!(std::mem::size_of::<ItemBox<Dummy>>(), 128);
        }
        assert!(std::mem::size_of::<ItemData<Dummy>>() < std::mem::size_of::<ItemBox<Dummy>>());

        let runtime =
            crate::runtime::Runtime::new(crate::runtime::RuntimeConfig::default()).unwrap();
        let resource = runtime
            .register_object()
            .unwrap()
            .register_resource(ResourceClass::new(1).unwrap(), Dummy)
            .unwrap();
        let mut batch = TypedItemBatch::<Dummy>::new();

        assert!(batch.intents.is_empty());
        assert_eq!(batch.intents.capacity(), 0);
        assert!(batch.preparations.is_empty());
        assert_eq!(batch.preparations.capacity(), 0);
        batch.push_active::<true>(
            ItemData::new(resource.clone(), 17, LayoutRole::Record),
            None,
        );
        assert_eq!(batch.intents.len(), 1);
        assert!(batch.intents[0].is_none());
        {
            let (item, intent) = batch.active_item_parts_mut(0);
            let mut entry = Entry::new_batch(item, intent);
            entry.record_read(Token { _words: [0; 2] }).unwrap();
            entry
                .stage(LayoutIntent {
                    _words: [0; 4],
                    _niche: NonZeroUsize::new(1).unwrap(),
                })
                .unwrap();
        }
        assert!(batch.intents[0].is_some());
        assert!(batch.preparations.is_empty());
        assert_eq!(batch.preparations.capacity(), 0);

        let mut destination: Vec<Option<Box<dyn ErasedItem>>> = vec![None];
        batch.drain_active_into(&mut destination);
        let item = destination[0]
            .as_deref_mut()
            .unwrap()
            .as_any_mut()
            .downcast_mut::<ItemBox<Dummy>>()
            .unwrap();

        assert_eq!(item.key.as_ref(), Some(&17));
        assert!(item.retains_binding(&resource));
        assert!(item.intent.is_some());
        assert!(matches!(item.observation, ObservationState::Read(_)));
        assert!(matches!(item.preparation, PreparationState::Unprepared));
        assert_eq!(batch.active_len(), 0);
        assert_eq!(batch.active_binding_filter, 0);
        assert!(batch.intents.is_empty());
        assert!(batch.preparations.is_empty());
    }

    #[test]
    fn typed_batch_intent_sidecar_stays_empty_at_high_water_before_reuse() {
        let runtime =
            crate::runtime::Runtime::new(crate::runtime::RuntimeConfig::default()).unwrap();
        let resource = runtime
            .register_object()
            .unwrap()
            .register_resource(ResourceClass::new(1).unwrap(), Dummy)
            .unwrap();
        let mut batch = TypedItemBatch::<Dummy>::new();
        batch.try_reserve_for_len(2).unwrap();
        batch.push_active::<true>(ItemData::new(resource.clone(), 1, LayoutRole::Record), None);
        let binding_filter = batch.active_binding_filter;
        batch.push_active::<false>(ItemData::new(resource.clone(), 2, LayoutRole::Record), None);
        assert_eq!(batch.active_binding_filter, binding_filter);
        for slot in 0..2 {
            let (item, intent) = batch.active_item_parts_mut(slot);
            Entry::new_batch(item, intent)
                .stage(LayoutIntent {
                    _words: [slot; 4],
                    _niche: NonZeroUsize::new(slot + 1).unwrap(),
                })
                .unwrap();
            batch.note_active_item_shape(slot);
        }

        let retained_capacity = batch.intents.capacity();
        let retained_allocation = batch.intents.as_ptr();
        let mut finish = FinishContext::new();
        let mut stage = BatchFinishStage::Callback;
        batch.finish_and_teardown(FinishDisposition::Aborted, &mut finish, &mut stage);
        assert_eq!(batch.active_len(), 0);
        assert_eq!(batch.intents.len(), 2);
        assert!(batch.intents.iter().all(Option::is_none));
        assert_eq!(batch.intents.capacity(), retained_capacity);
        assert_eq!(batch.intents.as_ptr(), retained_allocation);
        assert_eq!(batch.pooled_len(), 2);

        let pooled = batch.pooled_item_mut(0).unwrap();
        pooled.reinitialize(&resource, 3, LayoutRole::Record);
        batch.activate_reinitialized::<true>(None);
        assert_eq!(batch.intents.len(), 2);
        assert!(batch.intents[0].is_none());
        assert_eq!(batch.intents.as_ptr(), retained_allocation);
        {
            let (item, intent) = batch.active_item_parts_mut(0);
            let entry = Entry::new_batch(item, intent);
            assert!(entry.intent().is_none());
        }

        // Materializing a smaller active prefix moves its aligned intent slot
        // while preserving the inactive high-water tail for later reuse.
        let mut destination: Vec<Option<Box<dyn ErasedItem>>> = vec![None];
        batch.drain_active_into(&mut destination);
        assert_eq!(batch.active_len(), 0);
        assert_eq!(batch.active_binding_filter, 0);
        assert_eq!(batch.pooled_len(), 1);
        assert_eq!(batch.intents.len(), 1);
        assert!(batch.intents[0].is_none());
        let materialized = destination[0]
            .as_deref_mut()
            .unwrap()
            .as_any_mut()
            .downcast_mut::<ItemBox<Dummy>>()
            .unwrap();
        assert_eq!(materialized.key.as_ref(), Some(&3));
        assert!(materialized.intent.is_none());
    }

    #[test]
    fn active_binding_filter_resolves_collisions_exactly_and_resets() {
        let runtime =
            crate::runtime::Runtime::new(crate::runtime::RuntimeConfig::default()).unwrap();
        let object = runtime.register_object().unwrap();
        let resources: Vec<_> = (1_u32..=65)
            .map(|class| {
                object
                    .register_resource(ResourceClass::new(class).unwrap(), Dummy)
                    .unwrap()
            })
            .collect();

        // Sixty-five exact bindings must contain a collision in a 64-bit
        // summary, independently of allocator layout.
        let mut first_for_bit = [None; 64];
        let mut collision = None;
        for (index, resource) in resources.iter().enumerate() {
            let bit = active_binding_filter_bit(resource).trailing_zeros() as usize;
            if let Some(first) = first_for_bit[bit] {
                collision = Some((first, index));
                break;
            }
            first_for_bit[bit] = Some(index);
        }
        let (first_index, second_index) = collision.expect("65 bindings collide in 64 bits");
        let first = &resources[first_index];
        let second = &resources[second_index];
        assert_eq!(
            active_binding_filter_bit(first),
            active_binding_filter_bit(second)
        );

        let mut batch = TypedItemBatch::<Dummy>::new();
        batch.push_active::<true>(ItemData::new(first.clone(), 1, LayoutRole::Record), None);
        assert!(batch.contains_active_binding(first));
        assert!(!batch.binding_definitely_absent(second));
        assert!(
            !batch.contains_active_binding(second),
            "a positive filter bit must still use exact binding equality"
        );

        batch.push_active::<true>(ItemData::new(second.clone(), 2, LayoutRole::Record), None);
        assert!(batch.contains_active_binding(first));
        assert!(batch.contains_active_binding(second));

        let mut finish = FinishContext::new();
        let mut stage = BatchFinishStage::Callback;
        batch.finish_and_teardown(FinishDisposition::Aborted, &mut finish, &mut stage);
        assert_eq!(batch.active_len(), 0);
        assert_eq!(batch.active_binding_filter, 0);
        assert!(batch.binding_definitely_absent(first));
        assert!(batch.binding_definitely_absent(second));
    }
}
