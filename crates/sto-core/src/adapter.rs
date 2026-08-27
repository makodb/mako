//! Safe, typed extension contract for transactional datatypes.
//!
//! A resource implementation supplies datatype-specific observations,
//! predicates, intents, and commit preparation. The core owns item state and
//! exposes only the capabilities appropriate to each protocol phase.

use std::{fmt, hash::Hash, marker::PhantomData, rc::Rc};

use crate::{
    error::{CheckError, ItemInitError, PrepareError},
    identity::OccVersion,
};

pub use crate::lock::{
    ExecutionCheckContext, FinishContext, InstallContext, PredicateContext, PreflightContext,
    ValidationContext,
};

/// An owned, stable logical key for a transactional resource.
///
/// Implementations must keep `Eq`, `Ord`, and `Hash` mutually consistent. A
/// key's comparison and hashing behavior must not change while the key is in a
/// transaction.
pub trait ResourceKey: Clone + Eq + Ord + Hash + fmt::Debug + Send + Sync + 'static {}

impl<T> ResourceKey for T where T: Clone + Eq + Ord + Hash + fmt::Debug + Send + Sync + 'static {}

/// Relates an observation to the runtime's opacity clock.
pub trait OpacityToken: 'static {
    /// Returns the ordering information carried by this observation.
    fn observation_order(&self) -> ObservationOrder;
}

/// Whether an observation can be compared with the runtime OCC clock.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ObservationOrder {
    /// The observation is covered through this ordered OCC version.
    Ordered(OccVersion),
    /// The observation requires conservative full revalidation.
    Unordered,
}

/// An uninhabited predicate type for resources that never record predicates.
///
/// The predicate callbacks remain explicit trait requirements. Their
/// implementations can exhaustively match the borrowed `NoPredicate` value.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum NoPredicate {}

impl OpacityToken for NoPredicate {
    fn observation_order(&self) -> ObservationOrder {
        match *self {}
    }
}

/// The safe Rust counterpart of an STO `TObject` implementation.
///
/// The associated types replace the untyped read, predicate, write, and stash
/// slots in the C++ item. Implementations are trusted for transactional
/// correctness, but the core does not rely on them for memory safety.
pub trait TransactionalResource: Send + Sync + Sized + 'static {
    /// Stable logical key within this resource class.
    type Key: ResourceKey;
    /// Datatype-private, transaction-local operation state.
    type Local: 'static;
    /// Token covering an observed abstract result.
    type Observation: OpacityToken;
    /// Token describing a predicate that must remain true.
    type Predicate: OpacityToken;
    /// Deferred abstract mutation composed during execution.
    type Intent: 'static;
    /// Commit scratch and typed lock uses produced during preflight.
    type Prepared: 'static;

    /// Creates owned transaction-local state for a newly encountered key.
    ///
    /// This callback must not perform an unrecorded shared read.
    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, ItemInitError>;

    /// Finalizes the item and plans every physical lock it will need.
    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError>;

    /// Revalidates a prior read during opaque execution.
    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError>;

    /// Revalidates a predicate without changing the item's predicate state.
    fn revalidate_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError>;

    /// Converts a satisfied predicate into an ordinary covering observation.
    fn upgrade_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        prepared: &Self::Prepared,
        cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError>;

    /// Validates a read in the final certification pass.
    fn validate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError>;

    /// Installs a staged intent after the transaction becomes irrevocable.
    ///
    /// This callback is infallible and must not panic. The item view only
    /// borrows the intent, so unwinding cannot make the core lose it.
    fn install(
        &self,
        key: &Self::Key,
        item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    );

    /// Performs exact-once, post-unlock cleanup for a definite outcome.
    ///
    /// This callback is infallible and must not panic.
    fn finish(
        &self,
        key: &Self::Key,
        item: FinishItem<'_, Self>,
        prepared: Option<&mut Self::Prepared>,
        disposition: FinishDisposition,
        cx: &mut FinishContext<'_>,
    );
}

/// A typed borrow of an item's current observation state.
pub enum ObservationRef<'a, A: TransactionalResource> {
    /// The item has not recorded a read or predicate.
    Unobserved,
    /// The item contains an ordinary read observation.
    Read(&'a A::Observation),
    /// The item contains an optimistic predicate.
    Predicate(&'a A::Predicate),
    /// A commit-time predicate upgrade produced an ordinary observation.
    UpgradedPredicate(&'a A::Observation),
}

impl<'a, A: TransactionalResource> Clone for ObservationRef<'a, A> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<A: TransactionalResource> Copy for ObservationRef<'_, A> {}

/// Phase-restricted item access while the adapter plans commit work.
pub struct PreflightItem<'a, A: TransactionalResource> {
    local: &'a mut A::Local,
    observation: ObservationRef<'a, A>,
    intent: &'a mut Option<A::Intent>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'a, A: TransactionalResource> PreflightItem<'a, A> {
    /// Constructs a preflight view from a core-owned item.
    pub(crate) fn new(
        local: &'a mut A::Local,
        observation: ObservationRef<'a, A>,
        intent: &'a mut Option<A::Intent>,
    ) -> Self {
        Self {
            local,
            observation,
            intent,
            not_send_sync: PhantomData,
        }
    }

    /// Borrows datatype-private transaction-local state.
    pub fn local(&self) -> &A::Local {
        self.local
    }

    /// Mutably borrows datatype-private transaction-local state.
    pub fn local_mut(&mut self) -> &mut A::Local {
        self.local
    }

    /// Borrows the current observation state.
    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.observation
    }

    /// Borrows the staged intent, if any.
    pub fn intent(&self) -> Option<&A::Intent> {
        self.intent.as_ref()
    }

    /// Mutably borrows the staged intent, if any, without allowing removal.
    pub fn intent_mut(&mut self) -> Option<&mut A::Intent> {
        self.intent.as_mut()
    }
}

/// Phase-restricted item access after the irreversible commit boundary.
///
/// The intent cannot be moved out through this view, so it remains reachable
/// by the core if installation unwinds.
pub struct InstallItem<'a, A: TransactionalResource> {
    local: &'a mut A::Local,
    observation: ObservationRef<'a, A>,
    intent: &'a A::Intent,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'a, A: TransactionalResource> InstallItem<'a, A> {
    /// Constructs an install view from a core-owned item with an intent.
    pub(crate) fn new(
        local: &'a mut A::Local,
        observation: ObservationRef<'a, A>,
        intent: &'a A::Intent,
    ) -> Self {
        Self {
            local,
            observation,
            intent,
            not_send_sync: PhantomData,
        }
    }

    /// Mutably borrows datatype-private transaction-local state.
    pub fn local_mut(&mut self) -> &mut A::Local {
        self.local
    }

    /// Borrows the current observation state.
    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.observation
    }

    /// Borrows the intent being installed without permitting it to be moved.
    pub fn intent(&self) -> &A::Intent {
        self.intent
    }
}

/// Phase-restricted item access for post-unlock cleanup.
pub struct FinishItem<'a, A: TransactionalResource> {
    local: &'a mut A::Local,
    observation: ObservationRef<'a, A>,
    intent: &'a mut Option<A::Intent>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'a, A: TransactionalResource> FinishItem<'a, A> {
    /// Constructs a finish view from a core-owned item.
    pub(crate) fn new(
        local: &'a mut A::Local,
        observation: ObservationRef<'a, A>,
        intent: &'a mut Option<A::Intent>,
    ) -> Self {
        Self {
            local,
            observation,
            intent,
            not_send_sync: PhantomData,
        }
    }

    /// Mutably borrows datatype-private transaction-local state.
    pub fn local_mut(&mut self) -> &mut A::Local {
        self.local
    }

    /// Borrows the final observation state.
    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.observation
    }

    /// Borrows an intent that install did not consume.
    pub fn remaining_intent(&self) -> Option<&A::Intent> {
        self.intent.as_ref()
    }

    /// Takes an intent that install did not consume.
    pub fn take_remaining_intent(&mut self) -> Option<A::Intent> {
        self.intent.take()
    }
}

/// Definite transaction outcome supplied to post-unlock cleanup.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FinishDisposition {
    /// The transaction committed and published its staged writes.
    Committed,
    /// The transaction definitely aborted without publishing staged writes.
    Aborted,
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Observation;

    impl OpacityToken for Observation {
        fn observation_order(&self) -> ObservationOrder {
            ObservationOrder::Unordered
        }
    }

    struct Resource;

    impl TransactionalResource for Resource {
        type Key = u64;
        type Local = u64;
        type Observation = Observation;
        type Predicate = NoPredicate;
        type Intent = u64;
        type Prepared = ();

        fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
            Ok(0)
        }

        fn preflight(
            &self,
            _key: &Self::Key,
            _item: PreflightItem<'_, Self>,
            _cx: &mut PreflightContext<'_>,
        ) -> Result<Self::Prepared, PrepareError> {
            Ok(())
        }

        fn revalidate_read(
            &self,
            _key: &Self::Key,
            _observation: &Self::Observation,
            _cx: &ExecutionCheckContext<'_>,
        ) -> Result<(), CheckError> {
            Ok(())
        }

        fn revalidate_predicate(
            &self,
            _key: &Self::Key,
            predicate: &Self::Predicate,
            _cx: &ExecutionCheckContext<'_>,
        ) -> Result<ObservationOrder, CheckError> {
            match *predicate {}
        }

        fn upgrade_predicate(
            &self,
            _key: &Self::Key,
            predicate: &Self::Predicate,
            _prepared: &Self::Prepared,
            _cx: &PredicateContext<'_>,
        ) -> Result<Self::Observation, CheckError> {
            match *predicate {}
        }

        fn validate_read(
            &self,
            _key: &Self::Key,
            _observation: &Self::Observation,
            _prepared: &Self::Prepared,
            _cx: &ValidationContext<'_>,
        ) -> Result<(), CheckError> {
            Ok(())
        }

        fn install(
            &self,
            _key: &Self::Key,
            _item: InstallItem<'_, Self>,
            _prepared: &mut Self::Prepared,
            _cx: &mut InstallContext<'_>,
        ) {
        }

        fn finish(
            &self,
            _key: &Self::Key,
            _item: FinishItem<'_, Self>,
            _prepared: Option<&mut Self::Prepared>,
            _disposition: FinishDisposition,
            _cx: &mut FinishContext<'_>,
        ) {
        }
    }

    #[test]
    fn phase_views_expose_only_borrows_until_finish() {
        let mut local = 3;
        let observation = Observation;
        let mut intent = Some(7);

        {
            let mut item = PreflightItem::<Resource>::new(
                &mut local,
                ObservationRef::Read(&observation),
                &mut intent,
            );
            *item.local_mut() += 1;
            *item.intent_mut().expect("intent") += 1;
            assert!(matches!(item.observation(), ObservationRef::Read(_)));
        }

        {
            let mut item = InstallItem::<Resource>::new(
                &mut local,
                ObservationRef::Read(&observation),
                intent.as_ref().expect("intent"),
            );
            *item.local_mut() += 1;
            assert_eq!(*item.intent(), 8);
        }
        assert_eq!(intent, Some(8));

        {
            let mut item = FinishItem::<Resource>::new(
                &mut local,
                ObservationRef::Read(&observation),
                &mut intent,
            );
            assert_eq!(item.take_remaining_intent(), Some(8));
            assert!(item.remaining_intent().is_none());
        }
        assert_eq!(local, 5);
        assert_eq!(intent, None);
    }

    #[test]
    fn no_predicate_satisfies_the_token_bound() {
        fn assert_token<T: OpacityToken>() {}
        assert_token::<NoPredicate>();
    }
}
