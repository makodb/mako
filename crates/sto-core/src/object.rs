//! Safe, typed extension contract for transactional datatypes.
//!
//! [`TObject`] is the datatype-author interface, not a forwarding facade.
//! Its associated types hold observations, predicates, write intents, and
//! private scratch; the core owns the transaction items and phase transitions.
//! [`crate::TransProxy`] records execution-time reads, predicates, and writes.
//!
//! # Commit responsibilities
//!
//! Datatypes choose lock acquisition and publication timing; core retains
//! ownership and records partial progress:
//!
//! 1. **Lock acquisition.** [`TObject::lock`] finalizes private state and
//!    synchronously acquires physical locks through [`LockContext`], in item
//!    access order. [`PreflightItem`] permits local/intent mutation, but not
//!    intent removal. [`TObject::Prepared`] retains typed tokens and scratch.
//! 2. **Predicate check.** [`TObject::check_predicate`] uses immutable held
//!    guards from [`PredicateContext`] to produce a covering ordinary
//!    observation. Core records the upgrade; the callback does not mutate the
//!    item's predicate slot.
//! 3. **Final ordinary read check.** [`TObject::check`] certifies ordinary
//!    reads, including upgraded predicates, using [`ValidationContext`].
//!    This context exposes immutable held guards and the selected commit
//!    metadata, not mutation or release authority.
//! 4. **Irrevocable install.** After successful certification and any
//!    pre-install hook, core crosses the irreversible boundary and invokes
//!    [`TObject::install`] for staged writes. [`InstallItem`] borrows the
//!    retained intent; [`InstallContext`] permits mutable held-guard access.
//!    Installation must be infallible. A datatype may call
//!    [`InstallContext::try_publish_unlock`] after its mutation is complete.
//!    Core defers publication for shared locks and retained participants.
//! 5. **Final publication and unlock.** After installation and any post-install
//!    hook, core publishes and releases every remaining held lock.
//! 6. **Outcome cleanup.** Once locks are released, [`TObject::cleanup`]
//!    receives a definite [`CleanupDisposition`]. [`CleanupItem`] can take
//!    the remaining intent; [`CleanupContext`] exposes neither locks nor the
//!    transaction. Cleanup is exact-once, not a second install or an unlock.
//!
//! # Capabilities and failure boundaries
//!
//! [`PreflightFreeReadCapability`] and [`TerminalReadBatchCapability`] describe
//! explicit restricted read protocols, including their drop-only cleanup
//! exceptions. [`TObject::direct_commit_capability`] selects the stronger
//! concrete typed-batch contract in [`DirectCommitCapability`], including its
//! cleanup policy. These are opt-in proofs, not permission to bypass final
//! certification or change the transaction's irreversible boundary.
//! Lock tokens remain scoped to the
//! current core-owned plan and resolve through the appropriate phase context.
//!
//! Before irrevocability, failure can abort without publishing staged writes.
//! An install/post-install failure or failed commit release can instead leave
//! publication indeterminate: core quarantines the item state rather than
//! invoking definite-outcome cleanup. An abort whose lock release fails also
//! quarantines its items. After successful publication, cleanup or teardown
//! panics poison the runtime but cannot turn the committed outcome into abort.
//! Commit callbacks must not panic; containment is not an adapter recovery API.
//!
//! # Execution-time opacity seams
//!
//! [`OpacityToken`], [`ExecutionCheckContext`], [`TObject::revalidate_read`],
//! and [`TObject::revalidate_predicate`] describe reserved execution-time
//! revalidation seams. The current runtime rejects
//! [`crate::IsolationMode::Opaque`]; implementing these methods or returning an
//! ordered observation does not provide opacity. Final commit certification
//! remains distinct from execution-time revalidation.

use std::{fmt, hash::Hash, marker::PhantomData, rc::Rc};

use crate::{
    direct_commit::DirectCommitCapability,
    error::{CheckError, ItemInitError, PrepareError},
    identity::OccVersion,
};

pub use crate::lock::{
    CleanupContext, ExecutionCheckContext, InstallContext, LockContext, PredicateContext,
    PreflightFreeValidationContext, ValidationContext,
};

/// An owned, stable logical key for a transactional resource.
///
/// Implementations must keep `Eq`, `Ord`, and `Hash` mutually consistent. A
/// key's comparison and hashing behavior must not change while the key is in a
/// transaction.
pub trait ResourceKey: Clone + Eq + Ord + Hash + fmt::Debug + Send + Sync + 'static {}

impl<T> ResourceKey for T where T: Clone + Eq + Ord + Hash + fmt::Debug + Send + Sync + 'static {}

/// Describes observation ordering for the reserved opacity protocol.
///
/// The current runtime does not support [`crate::IsolationMode::Opaque`].
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

/// Commit metadata required by writes through one registered binding.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CommitVersionRequirement {
    /// Core must reserve an ordered runtime commit ID before certification.
    #[default]
    Ordered,
    /// The adapter publishes checked, record-local generations instead.
    ///
    /// Every observation of those generations must be [`ObservationOrder::Unordered`].
    /// The adapter must check generation capacity before installation becomes
    /// irreversible and must ignore an ordered ID supplied for another binding
    /// in a mixed transaction. This mode promises serializability, not opacity.
    /// All bindings that can mutate the same physical version must use the
    /// same policy; a record's policy cannot change during its lifetime.
    AdapterLocal,
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

/// Final-certification callback for an ordinary read that needs no preflight
/// state or physical lock of its own.
pub type PreflightFreeReadValidate<A> = for<'context> fn(
    &A,
    &<A as TObject>::Key,
    &<A as TObject>::Observation,
    &PreflightFreeValidationContext<'context>,
) -> Result<(), CheckError>;

/// Final-certification callback for a terminal read-batch item.
pub type TerminalReadBatchValidate<A> = for<'context> fn(
    &A,
    &<A as TObject>::Key,
    &<A as TObject>::Observation,
    &PreflightFreeValidationContext<'context>,
) -> Result<(), CheckError>;

/// Explicit adapter proof for the restricted terminal read-batch protocol.
///
/// A terminal batch owns only keys, ordinary-read observations, and one shared
/// registered-resource binding. Core never constructs
/// [`TObject::Local`], `Intent`, or `Prepared` state for this
/// protocol and never invokes `preflight`, `install`, or `cleanup`.
///
/// Constructing this capability is therefore a stronger correctness promise
/// than [`PreflightFreeReadCapability::new_drop_only`]. `validate` must certify
/// the same abstract observation as [`TObject::check`].
/// On both commit and abort, dropping the key and observation must be the
/// complete cleanup: it must not require shared-state mutation, an outcome,
/// transaction-local state, or a phase context. An operation that records such
/// an observation must be a read only; it must not publish or stage a mutation
/// or create cleanup obligations outside those two owned values.
///
/// The adapter must return the same capability for the complete registered
/// lifetime. Violating these promises can break transactional correctness,
/// although core still contains callback and destructor panics for memory
/// safety.
pub struct TerminalReadBatchCapability<A: TObject> {
    validate: TerminalReadBatchValidate<A>,
}

impl<A: TObject> TerminalReadBatchCapability<A> {
    /// Constructs a terminal capability with drop-only cleanup on every
    /// definite outcome.
    pub const fn new_drop_only(validate: TerminalReadBatchValidate<A>) -> Self {
        Self { validate }
    }

    pub(crate) fn validate(
        &self,
        adapter: &A,
        key: &A::Key,
        observation: &A::Observation,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError> {
        (self.validate)(adapter, key, observation, cx)
    }
}

/// Post-publication cleanup callback for a committed ordinary read that never
/// produced [`TObject::Prepared`] state.
pub type PreflightFreeReadCleanup<A> = for<'item, 'context> fn(
    &A,
    &<A as TObject>::Key,
    CleanupItem<'item, A>,
    &mut CleanupContext<'context>,
);

/// Explicit adapter proof that some ordinary reads need no preflight state.
///
/// This capability removes the adapter's per-item [`TObject::lock`]
/// callback and `Prepared` value. It never removes final certification. In a
/// heterogeneous transaction, the core validates the read at the same cut as
/// every locked item. If every live item carries this capability, the core can
/// omit the empty physical lock plan and certify the read-only transaction
/// directly.
///
/// The core selects this capability only for an ordinary read with no staged
/// intent or predicate. `validate` must certify the same abstract observation
/// as [`TObject::check`] would, but without relying on a
/// `Prepared` value or any physical lock. Its restricted
/// [`PreflightFreeValidationContext`] intentionally cannot resolve lock
/// guards. [`Self::new`] supplies an adapter callback that cleans up a committed
/// item without `Prepared` state. It runs exactly once after the transaction's
/// successful certification (and, for a mixed transaction, after all locks
/// have been released). [`Self::new_drop_only`] instead explicitly promises
/// that committed items need only core-owned teardown, so no adapter cleanup
/// callback runs for that committed prepared-free read. Validation and any
/// required cleanup callback are commit callbacks and therefore must not panic.
///
/// An adapter that advertises this capability must return the same capability
/// for its entire registered lifetime. Violating these requirements can break
/// transactional correctness, although the core still contains callback
/// panics and faults for memory safety.
pub struct PreflightFreeReadCapability<A: TObject> {
    validate: PreflightFreeReadValidate<A>,
    // `None` is an explicit adapter-selected policy, installed only through
    // `new_drop_only`; it is never inferred from callback behavior.
    finish_committed: Option<PreflightFreeReadCleanup<A>>,
}

impl<A: TObject> PreflightFreeReadCapability<A> {
    /// Constructs an explicit prepared-free ordinary-read capability.
    pub const fn new(
        validate: PreflightFreeReadValidate<A>,
        finish_committed: PreflightFreeReadCleanup<A>,
    ) -> Self {
        Self {
            validate,
            finish_committed: Some(finish_committed),
        }
    }

    /// Constructs a prepared-free capability whose committed cleanup is
    /// core-owned teardown only.
    ///
    /// This is an explicit adapter correctness promise: after successful
    /// final certification, dropping the item's intent, observation,
    /// predicate, local state, and key must be the complete committed cleanup.
    /// In particular, committed cleanup must not need to mutate shared state,
    /// consume transaction-local state through [`CleanupItem`], or observe a
    /// [`CleanupContext`]. The core still invokes
    /// [`TObject::cleanup`] exactly once for an aborted item and
    /// contains both abort-cleanup and teardown panics. The registered resource
    /// remains alive while its adapter-owned item state is dropped.
    pub const fn new_drop_only(validate: PreflightFreeReadValidate<A>) -> Self {
        Self {
            validate,
            finish_committed: None,
        }
    }

    pub(crate) fn validate(
        &self,
        adapter: &A,
        key: &A::Key,
        observation: &A::Observation,
        cx: &PreflightFreeValidationContext<'_>,
    ) -> Result<(), CheckError> {
        (self.validate)(adapter, key, observation, cx)
    }

    #[inline]
    pub(crate) const fn finish_committed_callback(&self) -> Option<PreflightFreeReadCleanup<A>> {
        self.finish_committed
    }
}

/// The safe Rust counterpart of an STO `TObject` implementation.
///
/// The associated types replace the untyped read, predicate, write, and stash
/// slots in the C++ item. Implementations are trusted for transactional
/// correctness, but the core does not rely on them for memory safety.
///
/// See the [module's commit responsibilities](crate::object)
/// for the phase sequence: acquire typed locks, check predicates and reads,
/// install with datatype-selected publication, then clean up after release.
/// Phase-specific item views keep intents core-owned until cleanup, including
/// when installation unwinds.
pub trait TObject: Send + Sync + Sized + 'static {
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
    /// Commit scratch and typed lock uses produced during datatype locking.
    type Prepared: 'static;

    /// Selects the version contract for this exact registered binding.
    ///
    /// Core captures this value at registration. It is not inferred from the
    /// adapter type or from a direct-commit capability. The adapter must retain
    /// the selected policy for the binding's complete lifetime, including its
    /// generic fallback, lock release, and indeterminate cleanup paths. A
    /// transaction reserves an ordered ID if any writing binding needs one;
    /// read-only bindings do not contribute to that requirement.
    fn commit_version_requirement(&self) -> CommitVersionRequirement {
        CommitVersionRequirement::Ordered
    }

    /// Creates owned transaction-local state for a newly encountered key.
    ///
    /// This callback must not perform an unrecorded shared read.
    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, ItemInitError>;

    /// Optionally advertises prepared-free final certification for ordinary
    /// reads with no staged intent or predicate.
    ///
    /// The default retains the full preflight protocol. Returning `Some` is an
    /// explicit, stable adapter contract; see [`PreflightFreeReadCapability`].
    fn preflight_free_read_capability(&self) -> Option<&'static PreflightFreeReadCapability<Self>> {
        None
    }

    /// Optionally advertises the stronger, terminal read-batch protocol.
    ///
    /// The default keeps this restricted representation unavailable. Returning
    /// `Some` is an explicit, stable adapter contract; see
    /// [`TerminalReadBatchCapability`].
    fn terminal_read_batch_capability(&self) -> Option<&'static TerminalReadBatchCapability<Self>> {
        None
    }

    /// Optionally selects core's concrete direct commit plan for an eligible
    /// homogeneous typed batch.
    ///
    /// Core checks the capability once when each distinct exact registered
    /// resource binding first becomes live and requires the same static value
    /// across all such bindings. The result must therefore remain stable for
    /// the lifetime of a binding. Returning `Some` is suitable for role- or
    /// backend-specific opt-in; a scan/directory binding can return `None` and
    /// transparently retain the ordinary lock plan. See
    /// [`DirectCommitCapability`] for the stronger alternate-protocol
    /// requirements.
    fn direct_commit_capability(&self) -> Option<&'static DirectCommitCapability<Self>> {
        None
    }

    /// Prepares the current intent and synchronously acquires its physical locks.
    ///
    /// Retain [`LockContext::try_lock`] tokens in [`Self::Prepared`] for later
    /// phase contexts. No shared write may be installed here. A retained
    /// participant may invoke this callback again after more operations; core
    /// reuses held targets, and the returned preparation replaces the previous
    /// value. Derive scratch from the current intent, not an earlier snapshot.
    fn lock(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut LockContext<'_>,
    ) -> Result<Self::Prepared, PrepareError>;

    /// Reserved execution-time read revalidation seam.
    ///
    /// The current runtime rejects opaque isolation; this is not the final
    /// commit check performed by [`Self::check`].
    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError>;

    /// Reserved execution-time predicate revalidation seam.
    ///
    /// This does not change the item's predicate state. The current runtime
    /// rejects opaque isolation; commit-time conversion uses
    /// [`Self::check_predicate`] instead.
    fn revalidate_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError>;

    /// Converts a satisfied predicate into an ordinary covering observation.
    ///
    /// Core invokes this after lock acquisition, stores the returned
    /// observation, and includes it in the final [`Self::check`] pass.
    /// [`PredicateContext`] provides immutable held-guard access only.
    fn check_predicate(
        &self,
        key: &Self::Key,
        predicate: &Self::Predicate,
        prepared: &Self::Prepared,
        cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError>;

    /// Validates an ordinary read in the final certification pass.
    ///
    /// This includes observations returned by [`Self::check_predicate`].
    /// [`ValidationContext`] provides immutable held guards and commit
    /// metadata; a failed check still precedes the irreversible boundary.
    fn check(
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
    /// [`InstallContext`] grants mutable held-guard access and optional
    /// publication via `try_publish_unlock`. Aliased physical locks remain held
    /// until final unlock so no unfinished item becomes visible. A panic here
    /// is an indeterminate publication failure, never a retryable abort.
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
    /// Core calls it only after successful lock release; `prepared` can be
    /// absent when an item aborted before completing preflight. Advertised
    /// read/direct capabilities define their own committed cleanup policies.
    /// [`CleanupItem`] allows taking the remaining intent, while
    /// [`CleanupContext`] supplies no lock or transaction access. Quarantined
    /// items do not receive this callback. A cleanup panic cannot undo an
    /// already definite outcome.
    fn cleanup(
        &self,
        key: &Self::Key,
        item: CleanupItem<'_, Self>,
        prepared: Option<&mut Self::Prepared>,
        disposition: CleanupDisposition,
        cx: &mut CleanupContext<'_>,
    );
}

/// A typed borrow of an item's current observation state.
pub enum ObservationRef<'a, A: TObject> {
    /// The item has not recorded a read or predicate.
    Unobserved,
    /// The item contains an ordinary read observation.
    Read(&'a A::Observation),
    /// The item contains an optimistic predicate.
    Predicate(&'a A::Predicate),
    /// A commit-time predicate upgrade produced an ordinary observation.
    UpgradedPredicate(&'a A::Observation),
}

impl<'a, A: TObject> Clone for ObservationRef<'a, A> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<A: TObject> Copy for ObservationRef<'_, A> {}

/// Phase-restricted item access while the adapter plans commit work.
pub struct PreflightItem<'a, A: TObject> {
    local: &'a mut A::Local,
    observation: ObservationRef<'a, A>,
    intent: &'a mut Option<A::Intent>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'a, A: TObject> PreflightItem<'a, A> {
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
pub struct InstallItem<'a, A: TObject> {
    local: &'a mut A::Local,
    observation: ObservationRef<'a, A>,
    intent: &'a A::Intent,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'a, A: TObject> InstallItem<'a, A> {
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
pub struct CleanupItem<'a, A: TObject> {
    local: &'a mut A::Local,
    observation: ObservationRef<'a, A>,
    intent: &'a mut Option<A::Intent>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'a, A: TObject> CleanupItem<'a, A> {
    /// Constructs a cleanup view from a core-owned item.
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

    /// Borrows the retained intent, if any; install only borrowed it.
    pub fn remaining_intent(&self) -> Option<&A::Intent> {
        self.intent.as_ref()
    }

    /// Takes the retained intent, if any, for post-unlock cleanup.
    pub fn take_remaining_intent(&mut self) -> Option<A::Intent> {
        self.intent.take()
    }
}

/// Definite transaction outcome supplied to post-unlock cleanup.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CleanupDisposition {
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

    impl TObject for Resource {
        type Key = u64;
        type Local = u64;
        type Observation = Observation;
        type Predicate = NoPredicate;
        type Intent = u64;
        type Prepared = ();

        fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
            Ok(0)
        }

        fn lock(
            &self,
            _key: &Self::Key,
            _item: PreflightItem<'_, Self>,
            _cx: &mut LockContext<'_>,
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

        fn check_predicate(
            &self,
            _key: &Self::Key,
            predicate: &Self::Predicate,
            _prepared: &Self::Prepared,
            _cx: &PredicateContext<'_>,
        ) -> Result<Self::Observation, CheckError> {
            match *predicate {}
        }

        fn check(
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

        fn cleanup(
            &self,
            _key: &Self::Key,
            _item: CleanupItem<'_, Self>,
            _prepared: Option<&mut Self::Prepared>,
            _disposition: CleanupDisposition,
            _cx: &mut CleanupContext<'_>,
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
            let mut item = CleanupItem::<Resource>::new(
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
