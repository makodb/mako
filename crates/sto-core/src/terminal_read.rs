//! Restricted storage and dispatch for one transaction-terminal read batch.

use std::{any::Any, marker::PhantomData, rc::Rc};

use crate::{
    adapter::{TerminalReadBatchCapability, TransactionalResource},
    error::{AccessError, AdapterFault, AdapterPhase, CapacityError, CheckError, InvalidUse},
    lock::PreflightFreeValidationContext,
    runtime::RegisteredResource,
};

/// Private object-safe dispatch for a homogeneous terminal read batch.
pub(crate) trait ErasedTerminalReadBatch: Any {
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn active_len(&self) -> usize;
    fn is_complete(&self) -> bool;
    fn validate(&self, cx: &PreflightFreeValidationContext<'_>) -> Result<(), CheckError>;
    fn teardown_drop_only_reverse(&mut self);
    fn dispose_retained_resource(&mut self);
}

/// Worker-pooled terminal storage with no per-item local or commit state.
///
/// The parallel vectors deliberately permit one pending key while an operation
/// callback is active. This keeps each completed item to exactly one `Key` and
/// one `Observation`, while a callback error or unwind still leaves its key in
/// core custody for contained abort cleanup.
pub(crate) struct TypedTerminalReadBatch<A: TransactionalResource> {
    // Automatic drop order keeps the registered binding alive until both
    // adapter-owned vectors have been destroyed.
    observations: Vec<A::Observation>,
    keys: Vec<A::Key>,
    capability: Option<&'static TerminalReadBatchCapability<A>>,
    resource: Option<RegisteredResource<A>>,
}

impl<A: TransactionalResource> TypedTerminalReadBatch<A> {
    pub(crate) const fn new() -> Self {
        Self {
            observations: Vec::new(),
            keys: Vec::new(),
            capability: None,
            resource: None,
        }
    }

    #[inline]
    pub(crate) fn active_len(&self) -> usize {
        self.keys.len()
    }

    #[inline]
    pub(crate) fn is_complete(&self) -> bool {
        self.keys.len() == self.observations.len()
    }

    pub(crate) fn try_reserve_for_len(&mut self, needed: usize) -> Result<(), CapacityError> {
        debug_assert!(self.keys.is_empty());
        debug_assert!(self.observations.is_empty());
        self.keys
            .try_reserve_exact(needed)
            .map_err(|_| CapacityError::ItemLimit)?;
        self.observations
            .try_reserve_exact(needed)
            .map_err(|_| CapacityError::ItemLimit)
    }

    #[inline]
    pub(crate) fn retains_binding(&self, resource: &RegisteredResource<A>) -> bool {
        self.resource
            .as_ref()
            .is_some_and(|retained| retained.is_same_binding(resource))
    }

    #[inline]
    pub(crate) fn has_retained_binding(&self) -> bool {
        self.resource.is_some()
    }

    #[inline]
    pub(crate) fn retains_capability(
        &self,
        capability: &'static TerminalReadBatchCapability<A>,
    ) -> bool {
        self.capability
            .is_some_and(|retained| std::ptr::eq(retained, capability))
    }

    pub(crate) fn retain_binding(
        &mut self,
        resource: &RegisteredResource<A>,
        capability: &'static TerminalReadBatchCapability<A>,
    ) {
        debug_assert!(self.keys.is_empty());
        debug_assert!(self.observations.is_empty());
        debug_assert!(self.resource.is_none());
        debug_assert!(self.capability.is_none());
        self.capability = Some(capability);
        self.resource = Some(resource.clone());
    }

    pub(crate) fn dispose_retained_resource(&mut self) {
        debug_assert!(self.keys.is_empty());
        debug_assert!(self.observations.is_empty());
        self.capability = None;
        drop(self.resource.take());
    }

    #[inline]
    pub(crate) fn push_pending_key(&mut self, key: A::Key) {
        debug_assert!(self.is_complete());
        debug_assert!(self.keys.len() < self.keys.capacity());
        self.keys.push(key);
    }

    #[inline]
    pub(crate) fn pending_entry(&mut self) -> TerminalReadEntry<'_, A> {
        debug_assert_eq!(self.keys.len(), self.observations.len() + 1);
        let slot = self.observations.len();
        let key = &self.keys[slot];
        TerminalReadEntry {
            key,
            observations: &mut self.observations,
            slot,
            not_send_sync: PhantomData,
        }
    }

    fn validate_inner(&self, cx: &PreflightFreeValidationContext<'_>) -> Result<(), CheckError> {
        if !self.is_complete() {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        let resource = self
            .resource
            .as_ref()
            .expect("an active terminal batch retains its resource");
        let capability = self
            .capability
            .expect("an active terminal batch retains its capability");
        let adapter = resource.adapter();
        for (key, observation) in self.keys.iter().zip(&self.observations) {
            capability.validate(adapter, key, observation, cx)?;
        }
        Ok(())
    }

    fn teardown_inner(&mut self) {
        debug_assert!(self.keys.len() >= self.observations.len());
        debug_assert!(self.keys.len() <= self.observations.len() + 1);

        // An operation that returned or unwound before `record_read` leaves one
        // pending key. Remove it before the completed prefix.
        while self.keys.len() > self.observations.len() {
            drop(self.keys.pop().expect("a pending terminal key exists"));
        }
        while let Some(observation) = self.observations.pop() {
            // Pop only the value currently being destroyed. If its destructor
            // unwinds, the matching key and every earlier item remain retained
            // in the quarantined frame rather than running a second destructor.
            drop(observation);
            drop(
                self.keys
                    .pop()
                    .expect("a completed observation retains its key"),
            );
        }
        debug_assert!(self.keys.is_empty());
    }
}

impl<A: TransactionalResource> ErasedTerminalReadBatch for TypedTerminalReadBatch<A> {
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn active_len(&self) -> usize {
        self.active_len()
    }

    fn is_complete(&self) -> bool {
        self.is_complete()
    }

    fn validate(&self, cx: &PreflightFreeValidationContext<'_>) -> Result<(), CheckError> {
        self.validate_inner(cx)
    }

    fn teardown_drop_only_reverse(&mut self) {
        self.teardown_inner();
    }

    fn dispose_retained_resource(&mut self) {
        self.dispose_retained_resource();
    }
}

/// The only operation-time surface for one terminal read item.
///
/// It intentionally exposes no local state, predicates, intents, or commit
/// preparation. The callback must record exactly one ordinary observation.
pub struct TerminalReadEntry<'entry, A: TransactionalResource> {
    key: &'entry A::Key,
    observations: &'entry mut Vec<A::Observation>,
    slot: usize,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<A: TransactionalResource> TerminalReadEntry<'_, A> {
    /// Borrows the exact key retained by core for this item.
    #[inline]
    pub fn key(&self) -> &A::Key {
        self.key
    }

    /// Records this item's one ordinary-read observation.
    #[inline]
    pub fn record_read(&mut self, observation: A::Observation) -> Result<(), AccessError> {
        if self.observations.len() != self.slot {
            return Err(InvalidUse::IllegalItemState.into());
        }
        debug_assert!(self.observations.len() < self.observations.capacity());
        self.observations.push(observation);
        Ok(())
    }

    #[inline]
    pub(crate) fn has_read(&self) -> bool {
        self.observations.len() == self.slot + 1
    }
}
