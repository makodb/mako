//! Core-owned typed transaction items and their private erased dispatch.

use std::{
    any::{Any, TypeId},
    marker::PhantomData,
    rc::Rc,
};

use crate::{
    adapter::{
        FinishDisposition, FinishItem, InstallItem, ObservationOrder, ObservationRef,
        PreflightFreeReadCapability, PreflightItem, TransactionalResource,
    },
    error::{AccessError, CapacityError, CheckError, InvalidUse, PrepareError},
    identity::{ObjectId, ResourceClass},
    lock::{
        ExecutionCheckContext, FinishContext, InstallContext, PredicateContext, PreflightContext,
        PreflightFreeValidationContext, ValidationContext,
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

/// The core-controlled, typed equivalent of one C++ `TItem`.
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

    #[inline]
    fn observation_ref(&self) -> ObservationRef<'_, A> {
        self.observation.as_ref::<A>()
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

/// One homogeneous, contiguous unique-item batch behind a single core vtable.
///
/// A live batch is the transaction's complete item sequence. If ordinary
/// access follows, [`Self::drain_active_into`] materializes that sequence into
/// the ordinary erased-item representation before lookup continues.
pub(crate) trait ErasedItemBatch: Any {
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn active_len(&self) -> usize;
    fn commit_shape(&self) -> (bool, bool);
    fn is_preflight_free_read_only(&self) -> bool;
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

/// Worker-pooled typed storage used only by the exact unique-batch lane.
pub(crate) struct TypedItemBatch<A: TransactionalResource> {
    items: Vec<ItemBox<A>>,
    active_len: usize,
    // These facts are recorded while each newly appended item is already hot.
    // A live direct batch cannot be accessed again without first being
    // materialized into ordinary items, and adapter preflight cannot add or
    // remove intents or predicates, so the cached shape remains exact for the
    // lifetime of the direct batch.
    has_writes: bool,
    has_predicates: bool,
    all_preflight_free_reads: bool,
}

impl<A: TransactionalResource> TypedItemBatch<A> {
    pub(crate) const fn new() -> Self {
        Self {
            items: Vec::new(),
            active_len: 0,
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
        self.items.len()
    }

    #[inline]
    pub(crate) fn try_reserve_for_len(&mut self, needed: usize) -> Result<(), CapacityError> {
        if needed <= self.items.len() {
            return Ok(());
        }
        self.items
            .try_reserve_exact(needed - self.items.len())
            .map_err(|_| CapacityError::ItemLimit)
    }

    #[inline]
    pub(crate) fn pooled_item_mut(&mut self, slot: usize) -> Option<&mut ItemBox<A>> {
        debug_assert_eq!(slot, self.active_len);
        self.items.get_mut(slot)
    }

    #[inline]
    pub(crate) fn activate_reinitialized(&mut self) {
        debug_assert!(self.active_len < self.items.len());
        self.active_len += 1;
    }

    #[inline]
    pub(crate) fn push_active(&mut self, item: ItemBox<A>) {
        debug_assert_eq!(self.active_len, self.items.len());
        self.items.push(item);
        self.active_len += 1;
    }

    #[inline]
    pub(crate) fn active_item_mut(&mut self, slot: usize) -> &mut ItemBox<A> {
        debug_assert!(slot < self.active_len);
        &mut self.items[slot]
    }

    #[inline]
    pub(crate) fn note_active_item_shape(&mut self, slot: usize) {
        debug_assert!(slot < self.active_len);
        let item = &self.items[slot];
        self.has_writes |= item.intent.is_some();
        self.has_predicates |= matches!(item.observation, ObservationState::Predicate(_));
        self.all_preflight_free_reads &=
            <ItemBox<A> as ErasedItem>::is_preflight_free_read_candidate(item);
    }

    #[inline]
    fn reset_shape(&mut self) {
        self.has_writes = false;
        self.has_predicates = false;
        self.all_preflight_free_reads = true;
    }
}

impl<A: TransactionalResource> ErasedItemBatch for TypedItemBatch<A> {
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
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

    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError> {
        for item in &mut self.items[..self.active_len] {
            <ItemBox<A> as ErasedItem>::preflight(item, cx)?;
        }
        Ok(())
    }

    fn upgrade_predicates(&mut self, cx: &PredicateContext<'_>) -> Result<(), CheckError> {
        for item in &mut self.items[..self.active_len] {
            <ItemBox<A> as ErasedItem>::upgrade_predicate(item, cx)?;
        }
        Ok(())
    }

    fn validate_preflight_free_reads(
        &mut self,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError> {
        for item in &mut self.items[..self.active_len] {
            <ItemBox<A> as ErasedItem>::validate_preflight_free_read(item, cx)?;
        }
        Ok(())
    }

    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError> {
        for item in &self.items[..self.active_len] {
            <ItemBox<A> as ErasedItem>::validate(item, cx)?;
        }
        Ok(())
    }

    fn install(&mut self, cx: &mut InstallContext<'_>) {
        for item in &mut self.items[..self.active_len] {
            if item.intent.is_some() {
                <ItemBox<A> as ErasedItem>::install(item, cx);
            }
        }
    }

    fn finish_and_teardown(
        &mut self,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
        stage: &mut BatchFinishStage,
    ) {
        for item in self.items[..self.active_len].iter_mut().rev() {
            *stage = BatchFinishStage::Callback;
            <ItemBox<A> as ErasedItem>::finish(item, disposition, cx);
            *stage = BatchFinishStage::Teardown;
            ItemBox::teardown_after_finish(item);
        }
        self.active_len = 0;
        self.reset_shape();
    }

    fn drain_active_into(&mut self, destination: &mut [Option<Box<dyn ErasedItem>>]) {
        let active_len = self.active_len;
        debug_assert!(destination.len() >= active_len);
        for (item_slot, item) in self.items.drain(..active_len).enumerate() {
            // The destination's retained resource was disposed under its own
            // unwind boundary before this infallible replacement phase. Its
            // prior item box therefore contains no adapter-owned live state.
            drop(destination[item_slot].replace(Box::new(item)));
        }
        self.active_len = 0;
        self.reset_shape();
    }

    fn dispose_retained_resources(&mut self) {
        debug_assert_eq!(self.active_len, 0);
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
    item: &'entry mut ItemBox<A>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'entry, A: TransactionalResource> Entry<'entry, A> {
    #[inline]
    pub(crate) fn new(item: &'entry mut ItemBox<A>) -> Self {
        Self {
            item,
            not_send_sync: PhantomData,
        }
    }

    #[inline]
    pub fn local(&self) -> &A::Local {
        self.item
            .local
            .as_ref()
            .expect("active item retains its local state")
    }

    #[inline]
    pub fn local_mut(&mut self) -> &mut A::Local {
        self.item
            .local
            .as_mut()
            .expect("active item retains its local state")
    }

    #[inline]
    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.item.observation_ref()
    }

    #[inline]
    pub fn intent(&self) -> Option<&A::Intent> {
        self.item.intent.as_ref()
    }

    #[inline]
    pub fn intent_mut(&mut self) -> Option<&mut A::Intent> {
        self.item.intent.as_mut()
    }

    /// Records the first ordinary read observation for this logical item.
    #[inline]
    pub fn record_read(&mut self, observation: A::Observation) -> Result<(), AccessError> {
        if !matches!(self.item.observation, ObservationState::Unobserved) {
            return Err(InvalidUse::IllegalItemState.into());
        }
        self.item.observation = ObservationState::Read(observation);
        Ok(())
    }

    /// Records the first optimistic predicate for this logical item.
    #[inline]
    pub fn record_predicate(&mut self, predicate: A::Predicate) -> Result<(), AccessError> {
        if !matches!(self.item.observation, ObservationState::Unobserved) {
            return Err(InvalidUse::IllegalItemState.into());
        }
        self.item.observation = ObservationState::Predicate(predicate);
        Ok(())
    }

    /// Stores an adapter-composed deferred intent.
    #[inline]
    pub fn stage(&mut self, intent: A::Intent) -> Result<(), AccessError> {
        self.item.intent = Some(intent);
        Ok(())
    }
}

mod sealed {
    pub trait Sealed {}
}

/// Private object-safe vtable used only for heterogeneous core dispatch.
pub(crate) trait ErasedItem: sealed::Sealed {
    fn identity_hash(&self) -> u64;
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

    #[test]
    fn observation_state_exposes_each_legal_borrow_shape() {
        struct Dummy;
        struct Token;
        impl crate::adapter::OpacityToken for Token {
            fn observation_order(&self) -> ObservationOrder {
                ObservationOrder::Unordered
            }
        }
        impl TransactionalResource for Dummy {
            type Key = u64;
            type Local = ();
            type Observation = Token;
            type Predicate = Token;
            type Intent = ();
            type Prepared = ();

            fn new_local(&self, _: &u64) -> Result<(), crate::error::ItemInitError> {
                Ok(())
            }
            fn preflight(
                &self,
                _: &u64,
                _: PreflightItem<'_, Self>,
                _: &mut PreflightContext<'_>,
            ) -> Result<(), PrepareError> {
                Ok(())
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
                _: &Token,
                _: &ExecutionCheckContext<'_>,
            ) -> Result<ObservationOrder, CheckError> {
                Ok(ObservationOrder::Unordered)
            }
            fn upgrade_predicate(
                &self,
                _: &u64,
                _: &Token,
                _: &(),
                _: &PredicateContext<'_>,
            ) -> Result<Token, CheckError> {
                Ok(Token)
            }
            fn validate_read(
                &self,
                _: &u64,
                _: &Token,
                _: &(),
                _: &ValidationContext<'_>,
            ) -> Result<(), CheckError> {
                Ok(())
            }
            fn install(
                &self,
                _: &u64,
                _: InstallItem<'_, Self>,
                _: &mut (),
                _: &mut InstallContext<'_>,
            ) {
            }
            fn finish(
                &self,
                _: &u64,
                _: FinishItem<'_, Self>,
                _: Option<&mut ()>,
                _: FinishDisposition,
                _: &mut FinishContext<'_>,
            ) {
            }
        }

        let state: ObservationState<Token, Token> = ObservationState::Read(Token);
        assert!(matches!(state.as_ref::<Dummy>(), ObservationRef::Read(_)));

        #[allow(dead_code)]
        enum LegacyPreparationState<P> {
            Unprepared,
            Prepared(P),
            Installed(P),
        }
        #[allow(dead_code)]
        struct LegacyItemBox {
            intent: Option<()>,
            preparation: LegacyPreparationState<()>,
            observation: ObservationState<Token, Token>,
            retained_predicate: Option<Token>,
            local: Option<()>,
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
}
