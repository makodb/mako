//! Core-owned typed transaction items and their private erased dispatch.

use std::{
    any::{Any, TypeId},
    marker::PhantomData,
    rc::Rc,
};

use crate::{
    adapter::{
        FinishDisposition, FinishItem, InstallItem, ObservationOrder, ObservationRef,
        PreflightItem, TransactionalResource,
    },
    error::{AccessError, CheckError, InvalidUse, PrepareError},
    identity::{ObjectId, ResourceClass},
    lock::{
        ExecutionCheckContext, FinishContext, InstallContext, PredicateContext, PreflightContext,
        ValidationContext,
    },
    runtime::RegisteredResource,
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
    Prepared(P),
    Installed(P),
}

/// The core-controlled, typed equivalent of one C++ `TItem`.
pub(crate) struct ItemBox<A: TransactionalResource> {
    // Field order is also the fail-safe automatic drop order. Normal cleanup
    // uses `teardown_after_finish` so each adapter-owned destructor is
    // individually contained and the resource handle remains alive last.
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

    fn observation_ref(&self) -> ObservationRef<'_, A> {
        self.observation.as_ref::<A>()
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
        drop(self.resource.take());
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
    pub(crate) fn new(item: &'entry mut ItemBox<A>) -> Self {
        Self {
            item,
            not_send_sync: PhantomData,
        }
    }

    pub fn local(&self) -> &A::Local {
        self.item
            .local
            .as_ref()
            .expect("active item retains its local state")
    }

    pub fn local_mut(&mut self) -> &mut A::Local {
        self.item
            .local
            .as_mut()
            .expect("active item retains its local state")
    }

    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.item.observation_ref()
    }

    pub fn intent(&self) -> Option<&A::Intent> {
        self.item.intent.as_ref()
    }

    pub fn intent_mut(&mut self) -> Option<&mut A::Intent> {
        self.item.intent.as_mut()
    }

    /// Records the first ordinary read observation for this logical item.
    pub fn record_read(&mut self, observation: A::Observation) -> Result<(), AccessError> {
        if !matches!(self.item.observation, ObservationState::Unobserved) {
            return Err(InvalidUse::IllegalItemState.into());
        }
        self.item.observation = ObservationState::Read(observation);
        Ok(())
    }

    /// Records the first optimistic predicate for this logical item.
    pub fn record_predicate(&mut self, predicate: A::Predicate) -> Result<(), AccessError> {
        if !matches!(self.item.observation, ObservationState::Unobserved) {
            return Err(InvalidUse::IllegalItemState.into());
        }
        self.item.observation = ObservationState::Predicate(predicate);
        Ok(())
    }

    /// Stores an adapter-composed deferred intent.
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
    fn object_id(&self) -> ObjectId;
    fn resource_class(&self) -> ResourceClass;
    fn adapter_type_id(&self) -> TypeId;
    fn key_type_id(&self) -> TypeId;
    fn key_eq(&self, key: &dyn Any) -> bool;
    fn as_any_mut(&mut self) -> &mut dyn Any;
    fn has_intent(&self) -> bool;

    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError>;
    #[allow(dead_code, reason = "reserved for the negotiated opaque profile")]
    fn revalidate_for_opacity(
        &self,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError>;
    fn upgrade_predicate(&mut self, cx: &PredicateContext<'_>) -> Result<(), CheckError>;
    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError>;
    fn install(&mut self, cx: &mut InstallContext<'_>);
    fn finish(&mut self, disposition: FinishDisposition, cx: &mut FinishContext<'_>);
    fn teardown_after_finish(&mut self);
}

impl<A: TransactionalResource> sealed::Sealed for ItemBox<A> {}

impl<A: TransactionalResource> ErasedItem for ItemBox<A> {
    fn object_id(&self) -> ObjectId {
        self.resource
            .as_ref()
            .expect("active item retains its resource")
            .object_id()
    }

    fn resource_class(&self) -> ResourceClass {
        self.resource
            .as_ref()
            .expect("active item retains its resource")
            .resource_class()
    }

    fn adapter_type_id(&self) -> TypeId {
        TypeId::of::<A>()
    }

    fn key_type_id(&self) -> TypeId {
        TypeId::of::<A::Key>()
    }

    fn key_eq(&self, key: &dyn Any) -> bool {
        key.downcast_ref::<A::Key>() == self.key.as_ref().map(|retained| retained as &A::Key)
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    fn has_intent(&self) -> bool {
        self.intent.is_some()
    }

    fn preflight(&mut self, cx: &mut PreflightContext<'_>) -> Result<(), PrepareError> {
        if !matches!(self.preparation, PreparationState::Unprepared) {
            return Err(crate::error::AdapterFault::invariant(
                crate::error::AdapterPhase::Preflight,
            )
            .into());
        }

        let Self {
            intent,
            preparation: _,
            observation,
            retained_predicate: _,
            local,
            key,
            resource,
        } = self;
        let view = PreflightItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        let prepared = resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter()
            .preflight(key.as_ref().expect("active item retains its key"), view, cx)?;
        self.preparation = PreparationState::Prepared(prepared);
        Ok(())
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

    fn validate(&self, cx: &ValidationContext<'_>) -> Result<(), CheckError> {
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
        let prepared = match preparation {
            PreparationState::Unprepared => None,
            PreparationState::Prepared(prepared) | PreparationState::Installed(prepared) => {
                Some(prepared)
            }
        };
        let view = FinishItem::new(
            local.as_mut().expect("active item retains its local state"),
            observation.as_ref::<A>(),
            intent,
        );
        resource
            .as_ref()
            .expect("active item retains its resource")
            .adapter()
            .finish(
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
    }
}
