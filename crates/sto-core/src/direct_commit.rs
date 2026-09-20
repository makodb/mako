//! Core-owned direct commit planning for homogeneous typed item batches.
//!
//! This module exposes a narrow, opt-in callback surface while retaining the
//! lock state machine, failure recovery, and adapter-state teardown inside
//! `sto-core`. Adapters cannot implement the erased plan trait or replace the
//! protocol driver; they can only configure the concrete unique-lock plan
//! supplied here.

use std::{
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
    ptr::NonNull,
    rc::Rc,
    sync::Arc,
};

use crate::{
    adapter::{InstallItem, NoPredicate, ObservationRef, PreflightItem, TransactionalResource},
    error::{
        AcquireError, AdapterFault, AdapterFaultKind, AdapterPhase, CapacityError, CheckError,
        PrepareError,
    },
    identity::{LockIdentity, OccCommitId, OwnerId, RuntimeId},
    item::ItemData,
    lock::{AcquireContext, LockDisposition, LockRequest, ReleaseContext, TransactionLock},
};

/// Read-only item state supplied during direct final certification.
///
/// The view cannot mutate or remove any adapter-owned value. The direct plan
/// separately supplies the exact retained target and held guard for a writing
/// item, and supplies no lock for an ordinary read.
pub struct DirectValidationItem<'item, A: TransactionalResource> {
    local: &'item A::Local,
    observation: ObservationRef<'item, A>,
    intent: Option<&'item A::Intent>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<'item, A: TransactionalResource> DirectValidationItem<'item, A> {
    pub(crate) fn new(
        local: &'item A::Local,
        observation: ObservationRef<'item, A>,
        intent: Option<&'item A::Intent>,
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

    /// Borrows the final observation state.
    pub fn observation(&self) -> ObservationRef<'_, A> {
        self.observation
    }

    /// Borrows the staged intent, if this is a writing item.
    pub fn intent(&self) -> Option<&A::Intent> {
        self.intent
    }
}

/// The exact target and immutable held guard assigned to one direct item.
pub struct DirectLockRef<'lock, L: TransactionLock> {
    target: &'lock L,
    guard: &'lock L::Guard,
}

impl<'lock, L: TransactionLock> DirectLockRef<'lock, L> {
    /// Returns the target retained continuously from planning through release.
    pub fn target(&self) -> &L {
        self.target
    }

    /// Returns the held guard produced by that target.
    pub fn guard(&self) -> &L::Guard {
        self.guard
    }
}

/// The exact target and mutable held guard assigned to one direct write.
pub struct DirectLockMut<'lock, L: TransactionLock> {
    target: &'lock L,
    guard: &'lock mut L::Guard,
}

impl<'lock, L: TransactionLock> DirectLockMut<'lock, L> {
    /// Returns the target retained continuously from planning through release.
    pub fn target(&self) -> &L {
        self.target
    }

    /// Returns the mutable held guard produced by that target.
    pub fn guard_mut(&mut self) -> &mut L::Guard {
        self.guard
    }

    /// Consumes the view and returns its exact target and mutable guard.
    pub fn into_parts(self) -> (&'lock L, &'lock mut L::Guard) {
        (self.target, self.guard)
    }
}

/// Commit metadata available during direct final certification.
pub struct DirectValidationContext {
    owner: OwnerId,
    occ_commit_id: Option<OccCommitId>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl DirectValidationContext {
    /// Returns the owner of every held direct guard.
    pub fn owner(&self) -> OwnerId {
        self.owner
    }

    /// Returns the reserved OCC commit identity, when this transaction writes.
    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }
}

/// Commit metadata available after the direct lane becomes irreversible.
pub struct DirectInstallContext {
    owner: OwnerId,
    occ_commit_id: Option<OccCommitId>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl DirectInstallContext {
    /// Returns the owner of every held direct guard.
    pub fn owner(&self) -> OwnerId {
        self.owner
    }

    /// Returns the OCC commit identity reserved for this writing transaction.
    pub fn occ_commit_id(&self) -> Option<OccCommitId> {
        self.occ_commit_id
    }
}

/// Plans either no lock for a read or exactly one unique lock for a write.
pub type DirectPrepare<A, L> = for<'item> fn(
    &A,
    &<A as TransactionalResource>::Key,
    PreflightItem<'item, A>,
) -> Result<Option<LockRequest<L>>, PrepareError>;

/// Plans either no lock for a read or one identity whose target remains owned
/// by the live transaction item.
pub type BorrowedDirectPrepare<A> = for<'item> fn(
    &A,
    &<A as TransactionalResource>::Key,
    PreflightItem<'item, A>,
) -> Result<Option<LockIdentity>, PrepareError>;

/// Compact, adapter-owned name for one lock in an exact borrowed target.
///
/// The runtime remains explicit so core can reject a cross-runtime token
/// during planning. The target address supplies the lock namespace and the
/// concrete [`DirectTokenLock`] implementation supplies the lock class; both
/// remain stable through guard release.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BorrowedLockToken<T: Copy> {
    runtime_id: RuntimeId,
    token: T,
}

impl<T: Copy> BorrowedLockToken<T> {
    /// Binds an adapter token to the runtime that owns its exact target.
    pub const fn new(runtime_id: RuntimeId, token: T) -> Self {
        Self { runtime_id, token }
    }

    /// Returns the runtime domain supplied during direct preparation.
    pub const fn runtime_id(self) -> RuntimeId {
        self.runtime_id
    }

    /// Returns the compact adapter token.
    pub const fn token(self) -> T {
        self.token
    }
}

/// Plans either no lock for a read or one compact token for a write.
pub type BorrowedTokenPrepare<A, L> =
    for<'item> fn(
        &A,
        &<A as TransactionalResource>::Key,
        PreflightItem<'item, A>,
    )
        -> Result<Option<BorrowedLockToken<<L as DirectTokenLock>::Token>>, PrepareError>;

/// Certifies one item against its optional exact direct guard.
pub type DirectValidate<A, L> = for<'item, 'lock> fn(
    &A,
    &<A as TransactionalResource>::Key,
    DirectValidationItem<'item, A>,
    Option<DirectLockRef<'lock, L>>,
    &DirectValidationContext,
) -> Result<(), CheckError>;

/// Installs one writing item through its exact direct target and guard.
pub type DirectInstall<A, L> = for<'item, 'lock> fn(
    &A,
    &<A as TransactionalResource>::Key,
    InstallItem<'item, A>,
    DirectLockMut<'lock, L>,
    &mut DirectInstallContext,
);

/// Typed callbacks used by the core-owned one-lock-per-write commit plan.
///
/// `prepare` must return `Some` exactly when the item has an intent, and every
/// returned identity must be distinct across the transaction. Core verifies
/// both promises before acquisition. `validate` receives `None` for a read and
/// the exact retained target/guard for a write. `install` is called once per
/// write after the irreversible boundary. As with the ordinary STO adapter
/// contract, callbacks must not panic; core nevertheless contains unwinds and
/// preserves the definite/indeterminate outcome rules.
///
/// The associated [`DirectCommitCapability`] additionally promises that
/// [`TransactionalResource::finish`] is complete when called with no
/// `Prepared` value after this alternate protocol.
pub struct UniqueLockCommitCapability<A: TransactionalResource, L: TransactionLock> {
    prepare: DirectPrepare<A, L>,
    validate: DirectValidate<A, L>,
    install: DirectInstall<A, L>,
}

/// Supplies a stable lock target borrowed from a transaction item's adapter.
///
/// This trait is an explicit opt-in to
/// [`DirectCommitCapability::borrowed_unique_lock`] or, with a stronger
/// identity proof, [`DirectCommitCapability::borrowed_injective_lock`].
/// The method must return the same target address from direct preparation
/// through release. Core records and verifies that address before using a
/// guard with the target; it never dereferences the recorded raw address.
pub trait DirectBorrowedLockTarget<L: TransactionLock>: TransactionalResource {
    /// Reborrows the canonical target retained by this adapter.
    fn direct_borrowed_lock_target(&self) -> &L;
}

/// Acquires a compact token within one exact borrowed lock target.
///
/// This is a narrower representation of [`TransactionLock::try_acquire`] for
/// direct plans whose unsafe capability proof makes the target address part of
/// every physical identity. Implementations must retain the ordinary lock
/// contract and additionally reject a `runtime_id` that does not own `self`.
/// The associated token type and its constructors must make every value that
/// safe code can supply safe to inspect. Implementations must validate all
/// externally malleable token shape before any unsafe dereference and return an
/// error without retaining a lock for every definitely rejected token. If
/// provenance itself cannot be checked without a dereference, `Token` must be
/// an unforgeable type whose safe construction establishes that provenance.
///
/// # Safety
///
/// For every token accepted by [`Self::try_acquire_token`], the pair
/// `(self address, token)` must identify exactly one stable physical lock and
/// the returned guard must prove acquisition of that lock. Calling this safe
/// method with any `Token` obtainable through safe code must never cause
/// undefined behavior, including when the runtime or token shape is rejected.
/// The runtime identity and token interpretation must remain stable while the
/// target is retained by a transaction item. A violating implementation can
/// make a guard certify or publish through the wrong physical lock.
#[allow(
    unsafe_code,
    reason = "the trait makes the compact target/token identity and validation proof explicit"
)]
pub unsafe trait DirectTokenLock: TransactionLock {
    /// Compact, destructor-free token retained while a frame is planned.
    type Token: Copy + 'static;

    /// Attempts acquisition after validating the runtime and compact token.
    fn try_acquire_token(
        &self,
        runtime_id: RuntimeId,
        token: Self::Token,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError>;
}

/// Typed callbacks for the one-distinct-lock-per-write plan whose targets are
/// already retained by each live transaction item.
///
/// This is the borrowed-target counterpart to [`UniqueLockCommitCapability`].
/// `prepare` emits only an identity. [`DirectBorrowedLockTarget`] reborrows the
/// canonical target from the same immutable adapter in every later phase,
/// avoiding an owned [`Arc`] in every lock frame. Core verifies target-address
/// stability and retains the complete item batch until all guards have been
/// released and destroyed.
pub struct BorrowedUniqueLockCommitCapability<A: TransactionalResource, L: TransactionLock> {
    prepare: BorrowedDirectPrepare<A>,
    validate: DirectValidate<A, L>,
    install: DirectInstall<A, L>,
}

/// Typed callbacks for the compact intrinsically-injective borrowed plan.
///
/// `prepare` must return one runtime-bound token exactly for each writing
/// item. Core retains that token in the frame until acquisition replaces it
/// with the exact guard, avoiding a parallel full-identity vector. Validation,
/// installation, release, and teardown retain the ordinary direct protocol.
pub struct BorrowedInjectiveLockCommitCapability<A: TransactionalResource, L: DirectTokenLock> {
    prepare: BorrowedTokenPrepare<A, L>,
    validate: DirectValidate<A, L>,
    install: DirectInstall<A, L>,
    write_acquisition_certifies: bool,
}

impl<A: TransactionalResource, L: DirectTokenLock> BorrowedInjectiveLockCommitCapability<A, L> {
    /// Creates the callback set consumed by core's sealed compact plan.
    pub const fn new(
        prepare: BorrowedTokenPrepare<A, L>,
        validate: DirectValidate<A, L>,
        install: DirectInstall<A, L>,
    ) -> Self {
        Self {
            prepare,
            validate,
            install,
            write_acquisition_certifies: false,
        }
    }

    /// Omits the final validation callback for writing items after their exact
    /// token locks have been acquired. Read-only items still run `validate`.
    ///
    /// This option is useful when token acquisition already compares the
    /// execution-time observation with the current protected state. It removes
    /// a second adapter callback and a repeated guard check from each write.
    ///
    /// # Safety
    ///
    /// For every writing item, `prepare` must validate all item-shape and token
    /// provenance conditions that do not depend on a held guard. The emitted
    /// token must bind the item's exact execution-time observation to its exact
    /// physical lock. [`DirectTokenLock::try_acquire_token`] may return a guard
    /// only after it atomically proves that observation is still current; an
    /// observation mismatch must abort without retaining a lock. The guard must
    /// then exclude every change to the observed state until release.
    ///
    /// After a successful acquisition, the omitted `validate` call for that
    /// write must be side-effect free and must return `Ok(())` for the exact
    /// retained guard. Every condition that could reject the write must already
    /// follow from preparation and acquisition.
    ///
    /// `install` must independently reject a guard for the wrong target, item,
    /// owner, or physical lock. It must not depend on side effects from the
    /// skipped validation callback. Violating any part of this contract can
    /// publish a write whose execution-time observation is stale.
    #[allow(
        unsafe_code,
        reason = "the builder records the adapter's write-at-acquisition certification proof"
    )]
    #[must_use]
    pub const unsafe fn with_write_acquisition_certification(mut self) -> Self {
        self.write_acquisition_certifies = true;
        self
    }

    #[inline]
    const fn write_acquisition_certifies(&self) -> bool {
        self.write_acquisition_certifies
    }
}

impl<A: TransactionalResource, L: TransactionLock> BorrowedUniqueLockCommitCapability<A, L> {
    /// Creates the typed callback set consumed by core's sealed borrowed plan.
    pub const fn new(
        prepare: BorrowedDirectPrepare<A>,
        validate: DirectValidate<A, L>,
        install: DirectInstall<A, L>,
    ) -> Self {
        Self {
            prepare,
            validate,
            install,
        }
    }
}

impl<A: TransactionalResource, L: TransactionLock> UniqueLockCommitCapability<A, L> {
    /// Creates the typed callback set consumed by core's sealed direct plan.
    pub const fn new(
        prepare: DirectPrepare<A, L>,
        validate: DirectValidate<A, L>,
        install: DirectInstall<A, L>,
    ) -> Self {
        Self {
            prepare,
            validate,
            install,
        }
    }
}

mod sealed {
    use super::*;

    pub(crate) trait Capability<A: TransactionalResource>: Sync {
        fn create_plan(
            &'static self,
            public: &'static DirectCommitCapability<A>,
        ) -> Box<dyn ErasedDirectCommitPlan<A>>;
    }
}

/// Stable adapter opt-in to a core-owned direct commit implementation.
///
/// The wrapper is intentionally non-extensible: adapters construct it only
/// from one of core's typed capability values, while the erased plan
/// implementation and its state transitions remain private to `sto-core`.
pub struct DirectCommitCapability<A: TransactionalResource> {
    implementation: &'static dyn sealed::Capability<A>,
    // `true` is installed only through the explicit builder below. Core never
    // infers drop-only cleanup from callback behavior or adapter state.
    drop_only_committed_finish: bool,
    // The safe constructors retain core's exact duplicate-identity proof. The
    // unsafe borrowed constructor may suppress it after the adapter proves an
    // injective full-item-identity -> physical-lock-identity mapping.
    check_duplicate_identities: bool,
}

impl<A: TransactionalResource> DirectCommitCapability<A> {
    /// Selects core's one-distinct-lock-per-writing-item implementation.
    pub const fn unique_lock<L: TransactionLock>(
        implementation: &'static UniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: TransactionalResource<Predicate = NoPredicate>,
    {
        Self {
            implementation,
            drop_only_committed_finish: false,
            check_duplicate_identities: true,
        }
    }

    /// Selects core's borrowed-target one-distinct-lock-per-write plan.
    ///
    /// Unlike [`Self::unique_lock`], this protocol stores no per-write target
    /// [`Arc`]. The adapter itself, retained by every live transaction item,
    /// must own the exact target through guard release.
    pub const fn borrowed_unique_lock<L: TransactionLock>(
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L> + TransactionalResource<Predicate = NoPredicate>,
    {
        Self {
            implementation,
            drop_only_committed_finish: false,
            check_duplicate_identities: true,
        }
    }

    /// Selects the borrowed-target plan without duplicate-identity hashing.
    ///
    /// This is the intrinsically-unique counterpart to
    /// [`Self::borrowed_unique_lock`]. Core continues to verify that
    /// `prepare` returns an identity exactly for writing items, that every
    /// identity belongs to the committing runtime, and that the configured
    /// lock limit is respected. It omits only the transaction-wide exact set
    /// used to reject two equal [`LockIdentity`] values.
    ///
    /// # Safety
    ///
    /// For every homogeneous direct batch that can select this capability,
    /// the callback set must map distinct full transaction-item identities to
    /// distinct physical [`LockIdentity`] values whenever `prepare` returns
    /// `Some`. A full item identity consists of the exact registered resource
    /// binding and its logical key. The proof must cover distinct bindings
    /// whose adapters return this same static capability, not merely distinct
    /// keys within one binding, and must remain true for every callback-visible
    /// item state. Core already deduplicates equal full item identities before
    /// commit; it does not otherwise verify this stronger injectivity promise.
    /// Violating it may acquire and associate the same non-reentrant physical
    /// lock more than once and invalidate the direct protocol's guard and
    /// publication guarantees.
    #[allow(
        unsafe_code,
        reason = "the unsafe constructor makes the adapter's unchecked injectivity proof explicit"
    )]
    pub const unsafe fn borrowed_injective_lock<L: TransactionLock>(
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L> + TransactionalResource<Predicate = NoPredicate>,
    {
        Self {
            implementation,
            drop_only_committed_finish: false,
            check_duplicate_identities: false,
        }
    }

    /// Selects the compact borrowed-target plan for an injective token map.
    ///
    /// Unlike [`Self::borrowed_injective_lock`], this plan never materializes
    /// a general [`LockIdentity`]. It retains one small, runtime-bound token in
    /// the planned frame and replaces that token with the acquired guard. Core
    /// still checks intent shape, runtime identity, lock capacity, exact target
    /// address, guard state, callback ordering, and teardown state.
    ///
    /// # Safety
    ///
    /// For every homogeneous direct batch that can select this capability,
    /// distinct full transaction-item identities with intents must map to
    /// distinct `(exact borrowed target address, token)` pairs. Every emitted
    /// token must be valid for that target under the unsafe [`DirectTokenLock`]
    /// contract, and the callback must emit `Some` exactly for writing items.
    /// The proof must cover distinct registered bindings sharing this static
    /// capability and every callback-visible item state. Core deduplicates full
    /// logical item identities but deliberately performs no second token-set
    /// scan. Violating the proof may acquire one non-reentrant physical lock
    /// more than once or associate a guard with the wrong item.
    #[allow(
        unsafe_code,
        reason = "the unsafe constructor records the adapter's compact-token injectivity proof"
    )]
    pub const unsafe fn borrowed_injective_token_lock<L: DirectTokenLock>(
        implementation: &'static BorrowedInjectiveLockCommitCapability<A, L>,
    ) -> Self
    where
        A: DirectBorrowedLockTarget<L> + TransactionalResource<Predicate = NoPredicate>,
    {
        Self {
            implementation,
            drop_only_committed_finish: false,
            check_duplicate_identities: false,
        }
    }

    /// Explicitly makes committed direct-item cleanup core-owned and drop-only.
    ///
    /// The ordinary direct capability still invokes
    /// [`TransactionalResource::finish`] after every definite outcome. This
    /// stronger opt-in suppresses that callback only after direct validation,
    /// installation, and committed lock release have all completed. At that
    /// point, dropping each item's remaining intent, observation, predicate,
    /// local state, and key in reverse item order must be the complete
    /// committed cleanup. In particular, cleanup must not require another
    /// shared-state mutation, consumption through [`crate::adapter::FinishItem`],
    /// or access to [`crate::adapter::FinishContext`].
    ///
    /// Aborted direct attempts continue to invoke `finish` exactly once for
    /// every live item. Indeterminate attempts remain quarantined and run no
    /// item cleanup, exactly as for the ordinary direct capability. Core
    /// contains destructor panics under its normal post-publication failure
    /// boundary.
    #[must_use]
    pub const fn with_drop_only_committed_finish(mut self) -> Self {
        self.drop_only_committed_finish = true;
        self
    }

    #[inline]
    pub(crate) const fn has_drop_only_committed_finish(&self) -> bool {
        self.drop_only_committed_finish
    }

    #[inline]
    const fn checks_duplicate_identities(&self) -> bool {
        self.check_duplicate_identities
    }

    pub(crate) fn create_plan(&'static self) -> Box<dyn ErasedDirectCommitPlan<A>> {
        self.implementation.create_plan(self)
    }
}

pub(crate) trait ErasedDirectCommitPlan<A: TransactionalResource> {
    fn capability(&self) -> &'static DirectCommitCapability<A>;
    fn prepare(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &mut [Option<A::Intent>],
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError>;
    fn acquire_all(&mut self, items: &[ItemData<A>], owner: OwnerId) -> Result<(), AcquireError>;
    fn validate(
        &self,
        items: &[ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) -> Result<(), CheckError>;
    fn install(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    );
    fn requires_release(&self) -> bool;
    fn release_all(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault>;
    fn recover_after_callback_panic(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault>;
    fn teardown_adapter_state(&mut self) -> Result<(), ()>;
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum PlanState {
    Pooled,
    Planning,
    Acquiring,
    Held,
    Releasing,
    Released,
    Quarantined,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FrameState {
    Planned,
    Held,
    Released,
    Pooled,
}

#[derive(Clone, Copy, Default)]
struct DirectIdentityIndexEntry {
    identity_hash: u64,
    generation: u32,
    frame_slot: u32,
}

/// Pooled exact set used once a direct plan is large enough that the compact
/// prefilter would repeatedly rescan prior full identities.
///
/// Generation tags make reset O(1). The table is reserved and materialized
/// before any adapter callback, so probing after a target enters a frame is
/// allocation-free and cannot run an adapter-owned destructor. Full identity
/// equality remains the correctness boundary for hash collisions.
#[derive(Default)]
struct DirectIdentityIndex {
    entries: Vec<DirectIdentityIndexEntry>,
    generation: u32,
}

impl DirectIdentityIndex {
    // Below this size the single-word filter has less setup and cache traffic.
    const MIN_INDEXED_IDENTITIES: usize = 9;
    const MIN_CAPACITY: usize = 32;

    fn try_begin(&mut self, maximum_planned: usize) -> Result<bool, CapacityError> {
        if maximum_planned < Self::MIN_INDEXED_IDENTITIES {
            return Ok(false);
        }
        let minimum_capacity = maximum_planned
            .checked_mul(2)
            .ok_or(CapacityError::LockLimit)?;
        let table_len = minimum_capacity
            .max(Self::MIN_CAPACITY)
            .checked_next_power_of_two()
            .ok_or(CapacityError::LockLimit)?;
        if self.entries.len() < table_len {
            self.entries
                .try_reserve_exact(table_len.saturating_sub(self.entries.len()))
                .map_err(|_| CapacityError::LockLimit)?;
            self.entries
                .resize(table_len, DirectIdentityIndexEntry::default());
        }

        self.generation = self.generation.wrapping_add(1);
        if self.generation == 0 {
            // Reachable only after 2^32 indexed attempts on one pooled plan.
            // Clearing primitive tags occurs before the first adapter callback.
            for entry in &mut self.entries {
                entry.generation = 0;
            }
            self.generation = 1;
        }
        Ok(true)
    }

    #[inline]
    fn contains_or_insert(
        &mut self,
        identity_hash: u64,
        frame_slot: usize,
        identities: &[LockIdentity],
    ) -> bool {
        debug_assert!(self.entries.len().is_power_of_two());
        debug_assert!(frame_slot < identities.len());
        debug_assert!(frame_slot <= u32::MAX as usize);
        let mask = self.entries.len() - 1;
        let mut bucket = identity_hash as usize & mask;
        loop {
            let entry = &mut self.entries[bucket];
            if entry.generation != self.generation {
                *entry = DirectIdentityIndexEntry {
                    identity_hash,
                    generation: self.generation,
                    frame_slot: frame_slot as u32,
                };
                return false;
            }
            if entry.identity_hash == identity_hash
                && identities[entry.frame_slot as usize] == identities[frame_slot]
            {
                return true;
            }
            bucket = (bucket + 1) & mask;
        }
    }
}

struct DirectLockFrame<L: TransactionLock> {
    guard: Option<L::Guard>,
    target: Option<Arc<L>>,
    item_slot: usize,
    state: FrameState,
}

impl<L: TransactionLock> DirectLockFrame<L> {
    fn new(item_slot: usize, target: Arc<L>) -> Self {
        Self {
            guard: None,
            target: Some(target),
            item_slot,
            state: FrameState::Planned,
        }
    }

    fn rebind(&mut self, item_slot: usize, target: Arc<L>) -> Result<(), AdapterFault> {
        if self.state != FrameState::Pooled || self.guard.is_some() || self.target.is_some() {
            // A core-state invariant failed before this adapter-owned value
            // could enter a safely tear-downable frame. Leak it rather than
            // run an uncontained destructor on this impossible path.
            std::mem::forget(target);
            return Err(AdapterFault::invariant(AdapterPhase::Preflight));
        }
        self.item_slot = item_slot;
        self.target = Some(target);
        self.state = FrameState::Planned;
        Ok(())
    }

    fn is_reusable(&self) -> bool {
        self.state == FrameState::Pooled && self.guard.is_none() && self.target.is_none()
    }

    fn acquire(
        &mut self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<(), AcquireError> {
        if self.state != FrameState::Planned || self.guard.is_some() {
            return Err(AdapterFault::invariant(AdapterPhase::Acquire).into());
        }
        let target = self
            .target
            .as_ref()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Acquire))?;
        self.guard = Some(target.try_acquire(identity, cx)?);
        self.state = FrameState::Held;
        Ok(())
    }

    fn lock_ref(&self) -> Result<DirectLockRef<'_, L>, AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::invariant(AdapterPhase::Validation));
        }
        Ok(DirectLockRef {
            target: self
                .target
                .as_deref()
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Validation))?,
            guard: self
                .guard
                .as_ref()
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Validation))?,
        })
    }

    fn lock_mut(&mut self) -> Result<DirectLockMut<'_, L>, AdapterFault> {
        if self.state != FrameState::Held {
            return Err(AdapterFault::invariant(AdapterPhase::Install));
        }
        Ok(DirectLockMut {
            target: self
                .target
                .as_deref()
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Install))?,
            guard: self
                .guard
                .as_mut()
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Install))?,
        })
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
        self.state = FrameState::Released;
        Ok(())
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if self.state == FrameState::Held {
            return Err(());
        }
        drop(self.guard.take());
        drop(self.target.take());
        self.state = FrameState::Pooled;
        Ok(())
    }
}

struct UniqueLockCommitPlan<A: TransactionalResource, L: TransactionLock> {
    public_capability: &'static DirectCommitCapability<A>,
    implementation: &'static UniqueLockCommitCapability<A, L>,
    frames: Vec<DirectLockFrame<L>>,
    identities: Vec<LockIdentity>,
    active_frames: usize,
    acquired_len: usize,
    owner: Option<OwnerId>,
    state: PlanState,
    unique_identity_filter: u64,
    identity_index: DirectIdentityIndex,
    use_identity_index: bool,
    callback_in_progress: Option<usize>,
    quarantined_callback: Option<usize>,
}

impl<A: TransactionalResource, L: TransactionLock> UniqueLockCommitPlan<A, L> {
    fn new(
        public_capability: &'static DirectCommitCapability<A>,
        implementation: &'static UniqueLockCommitCapability<A, L>,
    ) -> Self {
        Self {
            public_capability,
            implementation,
            frames: Vec::new(),
            identities: Vec::new(),
            active_frames: 0,
            acquired_len: 0,
            owner: None,
            state: PlanState::Pooled,
            unique_identity_filter: 0,
            identity_index: DirectIdentityIndex::default(),
            use_identity_index: false,
            callback_in_progress: None,
            quarantined_callback: None,
        }
    }

    fn release_acquired(
        &mut self,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        while self.acquired_len != 0 {
            self.acquired_len -= 1;
            let frame_slot = self.acquired_len;
            self.callback_in_progress = Some(frame_slot);
            self.frames[frame_slot].release(disposition, cx)?;
            self.callback_in_progress = None;
        }
        Ok(())
    }
}

impl<A: TransactionalResource, L: TransactionLock> sealed::Capability<A>
    for UniqueLockCommitCapability<A, L>
{
    fn create_plan(
        &'static self,
        public: &'static DirectCommitCapability<A>,
    ) -> Box<dyn ErasedDirectCommitPlan<A>> {
        Box::new(UniqueLockCommitPlan::new(public, self))
    }
}

impl<A: TransactionalResource, L: TransactionLock> ErasedDirectCommitPlan<A>
    for UniqueLockCommitPlan<A, L>
{
    fn capability(&self) -> &'static DirectCommitCapability<A> {
        self.public_capability
    }

    fn prepare(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &mut [Option<A::Intent>],
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError> {
        if items.len() != intents.len() {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        if self.state != PlanState::Pooled
            || self.active_frames != 0
            || self.acquired_len != 0
            || self.owner.is_some()
            || self.callback_in_progress.is_some()
            || self.quarantined_callback.is_some()
        {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        self.identities.clear();
        self.unique_identity_filter = 0;
        self.state = PlanState::Planning;

        // Reserve before invoking an adapter. Afterwards every returned Arc
        // moves immediately into an existing or allocation-free new frame,
        // before max/runtime/uniqueness validation can reject the request.
        // This ensures adapter-owned target destruction always occurs through
        // the contained plan teardown path.
        let maximum_planned = items.len().min(max_locks.saturating_add(1));
        self.identities
            .try_reserve_exact(maximum_planned)
            .map_err(|_| CapacityError::LockLimit)?;
        if self.frames.capacity() < maximum_planned {
            self.frames
                .try_reserve_exact(maximum_planned.saturating_sub(self.frames.len()))
                .map_err(|_| CapacityError::LockLimit)?;
        }
        if !self.frames.iter().all(DirectLockFrame::is_reusable) {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        self.use_identity_index = self.identity_index.try_begin(maximum_planned)?;

        for (item_slot, (item, intent)) in items.iter_mut().zip(intents).enumerate() {
            let (adapter, key, item_view) = item.direct_preflight_parts(intent);
            let has_intent = item_view.intent().is_some();
            let request = (self.implementation.prepare)(adapter, key, item_view)?;
            let Some(request) = request else {
                if has_intent {
                    return Err(AdapterFault::new(
                        AdapterPhase::Preflight,
                        AdapterFaultKind::LockIdentityMismatch,
                    )
                    .into());
                }
                continue;
            };
            let (identity, target) = request.into_parts();
            let frame_slot = self.active_frames;
            if frame_slot == self.frames.len() {
                debug_assert!(self.frames.capacity() > self.frames.len());
                self.frames.push(DirectLockFrame::new(item_slot, target));
            } else {
                self.frames[frame_slot]
                    .rebind(item_slot, target)
                    .map_err(PrepareError::Fault)?;
            }
            self.identities.push(identity);
            self.active_frames += 1;

            if !has_intent {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            if self.active_frames > max_locks {
                return Err(CapacityError::LockLimit.into());
            }
            let identity = &self.identities[frame_slot];
            if identity.runtime_id() != runtime_id {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            let duplicate = if self.use_identity_index {
                let identity_hash = identity.planning_hash();
                self.identity_index
                    .contains_or_insert(identity_hash, frame_slot, &self.identities)
            } else {
                let filter_bit = identity.planning_filter_bit();
                let duplicate = self.unique_identity_filter & filter_bit != 0
                    && self.identities[..frame_slot]
                        .iter()
                        .any(|current| current == identity);
                self.unique_identity_filter |= filter_bit;
                duplicate
            };
            if duplicate {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
        }

        if self.active_frames == 0 {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        Ok(())
    }

    fn acquire_all(&mut self, _items: &[ItemData<A>], owner: OwnerId) -> Result<(), AcquireError> {
        if self.state != PlanState::Planning || self.callback_in_progress.is_some() {
            return Err(AdapterFault::invariant(AdapterPhase::Acquire).into());
        }
        self.state = PlanState::Acquiring;
        self.owner = Some(owner);
        let cx = AcquireContext::new(&owner);

        for frame_slot in 0..self.active_frames {
            self.callback_in_progress = Some(frame_slot);
            let result = self.frames[frame_slot].acquire(&self.identities[frame_slot], &cx);
            self.callback_in_progress = None;
            if let Err(error) = result {
                self.state = PlanState::Releasing;
                let release = ReleaseContext::new(&owner, None);
                self.release_acquired(LockDisposition::Aborted, &release)
                    .map_err(AcquireError::Fault)?;
                self.state = PlanState::Released;
                self.owner = None;
                return Err(error);
            }
            debug_assert_eq!(self.acquired_len, frame_slot);
            self.acquired_len += 1;
        }
        self.state = PlanState::Held;
        Ok(())
    }

    fn validate(
        &self,
        items: &[ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) -> Result<(), CheckError> {
        if self.state != PlanState::Held || self.owner.is_none() || items.len() != intents.len() {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        let owner = self.owner.expect("held direct plan has an owner");
        let cx = DirectValidationContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        let mut frame_slot = 0;
        for (item_slot, (item, intent)) in items.iter().zip(intents).enumerate() {
            let lock = if frame_slot < self.active_frames
                && self.frames[frame_slot].item_slot == item_slot
            {
                let lock = self.frames[frame_slot]
                    .lock_ref()
                    .map_err(CheckError::Fault)?;
                frame_slot += 1;
                Some(lock)
            } else {
                None
            };
            let (adapter, key, item_view) = item.direct_validation_parts(intent);
            (self.implementation.validate)(adapter, key, item_view, lock, &cx)?;
        }
        if frame_slot != self.active_frames {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        Ok(())
    }

    fn install(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) {
        assert_eq!(
            self.state,
            PlanState::Held,
            "direct install requires held locks"
        );
        assert_eq!(items.len(), intents.len(), "direct item sidecars diverged");
        let owner = self.owner.expect("held direct plan has an owner");
        let mut cx = DirectInstallContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        for frame_slot in 0..self.active_frames {
            let item_slot = self.frames[frame_slot].item_slot;
            let (adapter, key, item) = items[item_slot]
                .direct_install_parts(&intents[item_slot])
                .expect("a direct lock frame belongs to a writing item");
            let lock = self.frames[frame_slot]
                .lock_mut()
                .expect("a direct install frame remains held");
            (self.implementation.install)(adapter, key, item, lock, &mut cx);
        }
    }

    fn requires_release(&self) -> bool {
        self.state == PlanState::Held
    }

    fn release_all(
        &mut self,
        _items: &[ItemData<A>],
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault> {
        if self.state != PlanState::Held || self.callback_in_progress.is_some() {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        self.state = PlanState::Releasing;
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        self.release_acquired(disposition, &cx)?;
        self.state = PlanState::Released;
        self.owner = None;
        Ok(())
    }

    fn recover_after_callback_panic(
        &mut self,
        _items: &[ItemData<A>],
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
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        self.release_acquired(disposition, &cx)
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if !matches!(self.state, PlanState::Planning | PlanState::Released)
            || self.owner.is_some()
            || self.acquired_len != 0
            || self.callback_in_progress.is_some()
            || self.quarantined_callback.is_some()
        {
            return Err(());
        }
        let teardown = catch_unwind(AssertUnwindSafe(|| {
            for frame in self.frames[..self.active_frames].iter_mut().rev() {
                frame.teardown_adapter_state()?;
            }
            Ok::<(), ()>(())
        }));
        if !matches!(teardown, Ok(Ok(()))) {
            return Err(());
        }
        self.identities.clear();
        self.active_frames = 0;
        self.unique_identity_filter = 0;
        self.use_identity_index = false;
        self.state = PlanState::Pooled;
        Ok(())
    }
}

struct BorrowedDirectLockFrame<L: TransactionLock> {
    guard: Option<L::Guard>,
    target_address: Option<NonNull<L>>,
    item_slot: usize,
    state: FrameState,
}

impl<L: TransactionLock> BorrowedDirectLockFrame<L> {
    fn new(item_slot: usize, target: &L) -> Self {
        Self {
            guard: None,
            target_address: Some(NonNull::from(target)),
            item_slot,
            state: FrameState::Planned,
        }
    }

    fn rebind(&mut self, item_slot: usize, target: &L) -> Result<(), AdapterFault> {
        if self.state != FrameState::Pooled || self.guard.is_some() || self.target_address.is_some()
        {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight));
        }
        self.item_slot = item_slot;
        self.target_address = Some(NonNull::from(target));
        self.state = FrameState::Planned;
        Ok(())
    }

    fn is_reusable(&self) -> bool {
        self.state == FrameState::Pooled && self.guard.is_none() && self.target_address.is_none()
    }

    fn matches_target(&self, target: &L) -> bool {
        self.target_address
            .is_some_and(|address| std::ptr::eq(address.as_ptr(), target))
    }

    fn acquire(
        &mut self,
        identity: &LockIdentity,
        target: &L,
        cx: &AcquireContext<'_>,
    ) -> Result<(), AcquireError> {
        if self.state != FrameState::Planned || self.guard.is_some() || !self.matches_target(target)
        {
            return Err(AdapterFault::new(
                AdapterPhase::Acquire,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        self.guard = Some(target.try_acquire(identity, cx)?);
        self.state = FrameState::Held;
        Ok(())
    }

    fn lock_ref<'lock>(
        &'lock self,
        target: &'lock L,
    ) -> Result<DirectLockRef<'lock, L>, AdapterFault> {
        if self.state != FrameState::Held || !self.matches_target(target) {
            return Err(AdapterFault::new(
                AdapterPhase::Validation,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        Ok(DirectLockRef {
            target,
            guard: self
                .guard
                .as_ref()
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Validation))?,
        })
    }

    fn lock_mut<'lock>(
        &'lock mut self,
        target: &'lock L,
    ) -> Result<DirectLockMut<'lock, L>, AdapterFault> {
        if self.state != FrameState::Held || !self.matches_target(target) {
            return Err(AdapterFault::new(
                AdapterPhase::Install,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        Ok(DirectLockMut {
            target,
            guard: self
                .guard
                .as_mut()
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Install))?,
        })
    }

    fn release(
        &mut self,
        target: &L,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        if self.state != FrameState::Held || !self.matches_target(target) {
            return Err(AdapterFault::new(
                AdapterPhase::Release,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        let guard = self
            .guard
            .as_mut()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        target.release(guard, disposition, cx);
        self.state = FrameState::Released;
        Ok(())
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if self.state == FrameState::Held {
            return Err(());
        }
        drop(self.guard.take());
        self.target_address = None;
        self.state = FrameState::Pooled;
        Ok(())
    }
}

struct BorrowedUniqueLockCommitPlan<A: TransactionalResource, L: TransactionLock> {
    public_capability: &'static DirectCommitCapability<A>,
    implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    frames: Vec<BorrowedDirectLockFrame<L>>,
    identities: Vec<LockIdentity>,
    active_frames: usize,
    acquired_len: usize,
    owner: Option<OwnerId>,
    state: PlanState,
    unique_identity_filter: u64,
    identity_index: DirectIdentityIndex,
    use_identity_index: bool,
    callback_in_progress: Option<usize>,
    quarantined_callback: Option<usize>,
}

impl<A: DirectBorrowedLockTarget<L>, L: TransactionLock> BorrowedUniqueLockCommitPlan<A, L> {
    fn new(
        public_capability: &'static DirectCommitCapability<A>,
        implementation: &'static BorrowedUniqueLockCommitCapability<A, L>,
    ) -> Self {
        Self {
            public_capability,
            implementation,
            frames: Vec::new(),
            identities: Vec::new(),
            active_frames: 0,
            acquired_len: 0,
            owner: None,
            state: PlanState::Pooled,
            unique_identity_filter: 0,
            identity_index: DirectIdentityIndex::default(),
            use_identity_index: false,
            callback_in_progress: None,
            quarantined_callback: None,
        }
    }

    fn release_acquired(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        while self.acquired_len != 0 {
            self.acquired_len -= 1;
            let frame_slot = self.acquired_len;
            self.callback_in_progress = Some(frame_slot);
            let item_slot = self.frames[frame_slot].item_slot;
            let item = items
                .get(item_slot)
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
            let (adapter, _) = item.direct_adapter_key_parts();
            let target = adapter.direct_borrowed_lock_target();
            self.frames[frame_slot].release(target, disposition, cx)?;
            self.callback_in_progress = None;
        }
        Ok(())
    }
}

impl<A: DirectBorrowedLockTarget<L>, L: TransactionLock> sealed::Capability<A>
    for BorrowedUniqueLockCommitCapability<A, L>
{
    fn create_plan(
        &'static self,
        public: &'static DirectCommitCapability<A>,
    ) -> Box<dyn ErasedDirectCommitPlan<A>> {
        Box::new(BorrowedUniqueLockCommitPlan::new(public, self))
    }
}

impl<A: DirectBorrowedLockTarget<L>, L: TransactionLock> ErasedDirectCommitPlan<A>
    for BorrowedUniqueLockCommitPlan<A, L>
{
    fn capability(&self) -> &'static DirectCommitCapability<A> {
        self.public_capability
    }

    fn prepare(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &mut [Option<A::Intent>],
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError> {
        if items.len() != intents.len() {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        if self.state != PlanState::Pooled
            || self.active_frames != 0
            || self.acquired_len != 0
            || self.owner.is_some()
            || self.callback_in_progress.is_some()
            || self.quarantined_callback.is_some()
        {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        self.identities.clear();
        self.unique_identity_filter = 0;
        self.state = PlanState::Planning;

        let maximum_planned = items.len().min(max_locks.saturating_add(1));
        self.identities
            .try_reserve_exact(maximum_planned)
            .map_err(|_| CapacityError::LockLimit)?;
        if self.frames.capacity() < maximum_planned {
            self.frames
                .try_reserve_exact(maximum_planned.saturating_sub(self.frames.len()))
                .map_err(|_| CapacityError::LockLimit)?;
        }
        if !self.frames.iter().all(BorrowedDirectLockFrame::is_reusable) {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        let check_duplicate_identities = self.public_capability.checks_duplicate_identities();
        self.use_identity_index =
            check_duplicate_identities && self.identity_index.try_begin(maximum_planned)?;

        for (item_slot, (item, intent)) in items.iter_mut().zip(intents).enumerate() {
            let (adapter, key, item_view) = item.direct_preflight_parts(intent);
            let has_intent = item_view.intent().is_some();
            let identity = (self.implementation.prepare)(adapter, key, item_view)?;
            let Some(identity) = identity else {
                if has_intent {
                    return Err(AdapterFault::new(
                        AdapterPhase::Preflight,
                        AdapterFaultKind::LockIdentityMismatch,
                    )
                    .into());
                }
                continue;
            };
            let target = adapter.direct_borrowed_lock_target();
            let frame_slot = self.active_frames;
            if frame_slot == self.frames.len() {
                debug_assert!(self.frames.capacity() > self.frames.len());
                self.frames
                    .push(BorrowedDirectLockFrame::new(item_slot, target));
            } else {
                self.frames[frame_slot]
                    .rebind(item_slot, target)
                    .map_err(PrepareError::Fault)?;
            }
            self.identities.push(identity);
            self.active_frames += 1;

            if !has_intent {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            if self.active_frames > max_locks {
                return Err(CapacityError::LockLimit.into());
            }
            let identity = &self.identities[frame_slot];
            if identity.runtime_id() != runtime_id {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            if check_duplicate_identities {
                let duplicate = if self.use_identity_index {
                    let identity_hash = identity.planning_hash();
                    self.identity_index.contains_or_insert(
                        identity_hash,
                        frame_slot,
                        &self.identities,
                    )
                } else {
                    let filter_bit = identity.planning_filter_bit();
                    let duplicate = self.unique_identity_filter & filter_bit != 0
                        && self.identities[..frame_slot]
                            .iter()
                            .any(|current| current == identity);
                    self.unique_identity_filter |= filter_bit;
                    duplicate
                };
                if duplicate {
                    return Err(AdapterFault::new(
                        AdapterPhase::Preflight,
                        AdapterFaultKind::LockIdentityMismatch,
                    )
                    .into());
                }
            }
        }

        if self.active_frames == 0 {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        Ok(())
    }

    fn acquire_all(&mut self, items: &[ItemData<A>], owner: OwnerId) -> Result<(), AcquireError> {
        if self.state != PlanState::Planning || self.callback_in_progress.is_some() {
            return Err(AdapterFault::invariant(AdapterPhase::Acquire).into());
        }
        self.state = PlanState::Acquiring;
        self.owner = Some(owner);
        let cx = AcquireContext::new(&owner);

        for frame_slot in 0..self.active_frames {
            self.callback_in_progress = Some(frame_slot);
            let item_slot = self.frames[frame_slot].item_slot;
            let result = match items.get(item_slot) {
                Some(item) => {
                    let (adapter, _) = item.direct_adapter_key_parts();
                    let target = adapter.direct_borrowed_lock_target();
                    self.frames[frame_slot].acquire(&self.identities[frame_slot], target, &cx)
                }
                None => Err(AdapterFault::invariant(AdapterPhase::Acquire).into()),
            };
            self.callback_in_progress = None;
            if let Err(error) = result {
                self.state = PlanState::Releasing;
                let release = ReleaseContext::new(&owner, None);
                self.release_acquired(items, LockDisposition::Aborted, &release)
                    .map_err(AcquireError::Fault)?;
                self.state = PlanState::Released;
                self.owner = None;
                return Err(error);
            }
            debug_assert_eq!(self.acquired_len, frame_slot);
            self.acquired_len += 1;
        }
        self.state = PlanState::Held;
        Ok(())
    }

    fn validate(
        &self,
        items: &[ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) -> Result<(), CheckError> {
        if self.state != PlanState::Held || self.owner.is_none() || items.len() != intents.len() {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        let owner = self.owner.expect("held direct plan has an owner");
        let cx = DirectValidationContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        let mut frame_slot = 0;
        for (item_slot, (item, intent)) in items.iter().zip(intents).enumerate() {
            let (adapter, key, item_view) = item.direct_validation_parts(intent);
            let lock = if frame_slot < self.active_frames
                && self.frames[frame_slot].item_slot == item_slot
            {
                let target = adapter.direct_borrowed_lock_target();
                let lock = self.frames[frame_slot]
                    .lock_ref(target)
                    .map_err(CheckError::Fault)?;
                frame_slot += 1;
                Some(lock)
            } else {
                None
            };
            (self.implementation.validate)(adapter, key, item_view, lock, &cx)?;
        }
        if frame_slot != self.active_frames {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        Ok(())
    }

    fn install(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) {
        assert_eq!(
            self.state,
            PlanState::Held,
            "direct install requires held locks"
        );
        assert_eq!(items.len(), intents.len(), "direct item sidecars diverged");
        let owner = self.owner.expect("held direct plan has an owner");
        let mut cx = DirectInstallContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        for frame_slot in 0..self.active_frames {
            let item_slot = self.frames[frame_slot].item_slot;
            let (adapter, key, item) = items[item_slot]
                .direct_install_parts(&intents[item_slot])
                .expect("a direct lock frame belongs to a writing item");
            let target = adapter.direct_borrowed_lock_target();
            let lock = self.frames[frame_slot]
                .lock_mut(target)
                .expect("a borrowed direct install frame remains held by its exact target");
            (self.implementation.install)(adapter, key, item, lock, &mut cx);
        }
    }

    fn requires_release(&self) -> bool {
        self.state == PlanState::Held
    }

    fn release_all(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault> {
        if self.state != PlanState::Held || self.callback_in_progress.is_some() {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        self.state = PlanState::Releasing;
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        self.release_acquired(items, disposition, &cx)?;
        self.state = PlanState::Released;
        self.owner = None;
        Ok(())
    }

    fn recover_after_callback_panic(
        &mut self,
        items: &[ItemData<A>],
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
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        self.release_acquired(items, disposition, &cx)
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if !matches!(self.state, PlanState::Planning | PlanState::Released)
            || self.owner.is_some()
            || self.acquired_len != 0
            || self.callback_in_progress.is_some()
            || self.quarantined_callback.is_some()
        {
            return Err(());
        }
        let teardown = catch_unwind(AssertUnwindSafe(|| {
            for frame in self.frames[..self.active_frames].iter_mut().rev() {
                frame.teardown_adapter_state()?;
            }
            Ok::<(), ()>(())
        }));
        if !matches!(teardown, Ok(Ok(()))) {
            return Err(());
        }
        self.identities.clear();
        self.active_frames = 0;
        self.unique_identity_filter = 0;
        self.use_identity_index = false;
        self.state = PlanState::Pooled;
        Ok(())
    }
}

enum BorrowedTokenFrameState<T: Copy, G> {
    Pooled,
    Planned(BorrowedLockToken<T>),
    Acquiring,
    Held(G),
    Released(G),
}

struct BorrowedTokenLockFrame<L: DirectTokenLock> {
    state: BorrowedTokenFrameState<L::Token, L::Guard>,
    target_address: Option<NonNull<L>>,
    item_slot: usize,
}

impl<L: DirectTokenLock> BorrowedTokenLockFrame<L> {
    fn new(item_slot: usize, target: &L, token: BorrowedLockToken<L::Token>) -> Self {
        Self {
            state: BorrowedTokenFrameState::Planned(token),
            target_address: Some(NonNull::from(target)),
            item_slot,
        }
    }

    fn rebind(
        &mut self,
        item_slot: usize,
        target: &L,
        token: BorrowedLockToken<L::Token>,
    ) -> Result<(), AdapterFault> {
        if !self.is_reusable() {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight));
        }
        self.item_slot = item_slot;
        self.target_address = Some(NonNull::from(target));
        self.state = BorrowedTokenFrameState::Planned(token);
        Ok(())
    }

    fn is_reusable(&self) -> bool {
        matches!(self.state, BorrowedTokenFrameState::Pooled) && self.target_address.is_none()
    }

    fn matches_target(&self, target: &L) -> bool {
        self.target_address
            .is_some_and(|address| std::ptr::eq(address.as_ptr(), target))
    }

    fn acquire(
        &mut self,
        runtime_id: RuntimeId,
        target: &L,
        cx: &AcquireContext<'_>,
    ) -> Result<(), AcquireError> {
        let token = match self.state {
            BorrowedTokenFrameState::Planned(token)
                if self.matches_target(target) && token.runtime_id() == runtime_id =>
            {
                token
            }
            _ => {
                return Err(AdapterFault::new(
                    AdapterPhase::Acquire,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
        };

        // If the target violates its no-panic contract after acquisition, the
        // explicit uncertain state prevents teardown from treating this frame
        // as reusable. The plan-level callback marker quarantines it and keeps
        // the exact target and item batch alive.
        self.state = BorrowedTokenFrameState::Acquiring;
        match target.try_acquire_token(runtime_id, token.token(), cx) {
            Ok(guard) => {
                self.state = BorrowedTokenFrameState::Held(guard);
                Ok(())
            }
            Err(error) => {
                self.state = BorrowedTokenFrameState::Planned(token);
                Err(error)
            }
        }
    }

    fn lock_ref<'lock>(
        &'lock self,
        target: &'lock L,
    ) -> Result<DirectLockRef<'lock, L>, AdapterFault> {
        if !self.matches_target(target) {
            return Err(AdapterFault::new(
                AdapterPhase::Validation,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        let BorrowedTokenFrameState::Held(guard) = &self.state else {
            return Err(AdapterFault::invariant(AdapterPhase::Validation));
        };
        Ok(DirectLockRef { target, guard })
    }

    fn lock_mut<'lock>(
        &'lock mut self,
        target: &'lock L,
    ) -> Result<DirectLockMut<'lock, L>, AdapterFault> {
        if !self.matches_target(target) {
            return Err(AdapterFault::new(
                AdapterPhase::Install,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        let BorrowedTokenFrameState::Held(guard) = &mut self.state else {
            return Err(AdapterFault::invariant(AdapterPhase::Install));
        };
        Ok(DirectLockMut { target, guard })
    }

    fn release(
        &mut self,
        target: &L,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        if !self.matches_target(target) {
            return Err(AdapterFault::new(
                AdapterPhase::Release,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        let BorrowedTokenFrameState::Held(guard) = &mut self.state else {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        };

        // Keep the guard resident in the frame while adapter code runs. A
        // release panic therefore leaves the uncertain guard and target
        // quarantined rather than dropping either during unwind.
        target.release(guard, disposition, cx);
        let BorrowedTokenFrameState::Held(guard) =
            std::mem::replace(&mut self.state, BorrowedTokenFrameState::Acquiring)
        else {
            unreachable!("the direct token guard state cannot change during release")
        };
        if std::mem::needs_drop::<L::Guard>() {
            // A potentially adapter-owned destructor remains deferred until
            // every release has completed and the commit boundary is known to
            // be published. Teardown contains a destructor panic there.
            self.state = BorrowedTokenFrameState::Released(guard);
        } else {
            // A destructor-free guard has no observable drop operation and
            // cannot panic. Recycle its scalar frame now, while the target is
            // already proven inert by the successful release callback. Fully
            // released batches can consequently skip a second frame walk.
            drop(guard);
            self.target_address = None;
            self.state = BorrowedTokenFrameState::Pooled;
        }
        Ok(())
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if matches!(self.state, BorrowedTokenFrameState::Pooled) {
            return self.target_address.is_none().then_some(()).ok_or(());
        }
        if matches!(
            self.state,
            BorrowedTokenFrameState::Held(_) | BorrowedTokenFrameState::Acquiring
        ) {
            return Err(());
        }
        let prior = std::mem::replace(&mut self.state, BorrowedTokenFrameState::Acquiring);
        match prior {
            BorrowedTokenFrameState::Planned(_) => {}
            BorrowedTokenFrameState::Released(guard) => drop(guard),
            BorrowedTokenFrameState::Pooled
            | BorrowedTokenFrameState::Acquiring
            | BorrowedTokenFrameState::Held(_) => return Err(()),
        }
        self.target_address = None;
        self.state = BorrowedTokenFrameState::Pooled;
        Ok(())
    }
}

struct BorrowedInjectiveLockCommitPlan<A: TransactionalResource, L: DirectTokenLock> {
    public_capability: &'static DirectCommitCapability<A>,
    implementation: &'static BorrowedInjectiveLockCommitCapability<A, L>,
    frames: Vec<BorrowedTokenLockFrame<L>>,
    active_frames: usize,
    acquired_len: usize,
    runtime_id: Option<RuntimeId>,
    owner: Option<OwnerId>,
    state: PlanState,
    quarantined_callback: Option<usize>,
}

impl<A: DirectBorrowedLockTarget<L>, L: DirectTokenLock> BorrowedInjectiveLockCommitPlan<A, L> {
    fn new(
        public_capability: &'static DirectCommitCapability<A>,
        implementation: &'static BorrowedInjectiveLockCommitCapability<A, L>,
    ) -> Self {
        Self {
            public_capability,
            implementation,
            frames: Vec::new(),
            active_frames: 0,
            acquired_len: 0,
            runtime_id: None,
            owner: None,
            state: PlanState::Pooled,
            quarantined_callback: None,
        }
    }

    fn release_acquired(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) -> Result<(), AdapterFault> {
        while self.acquired_len != 0 {
            self.acquired_len -= 1;
            let frame_slot = self.acquired_len;
            let item_slot = self.frames[frame_slot].item_slot;
            let item = items
                .get(item_slot)
                .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
            let (adapter, _) = item.direct_adapter_key_parts();
            let target = adapter.direct_borrowed_lock_target();
            self.frames[frame_slot].release(target, disposition, cx)?;
        }
        Ok(())
    }

    fn plan_writing_item(
        &mut self,
        item_slot: usize,
        item: &mut ItemData<A>,
        intent: &mut Option<A::Intent>,
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError> {
        let (adapter, key, item_view) = item.direct_preflight_parts(intent);
        if item_view.intent().is_none() {
            return Err(AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        let token = (self.implementation.prepare)(adapter, key, item_view)?.ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::LockIdentityMismatch,
            )
        })?;

        let target = adapter.direct_borrowed_lock_target();
        let frame_slot = self.active_frames;
        if frame_slot == self.frames.len() {
            debug_assert!(self.frames.capacity() > self.frames.len());
            self.frames
                .push(BorrowedTokenLockFrame::new(item_slot, target, token));
        } else {
            self.frames[frame_slot]
                .rebind(item_slot, target, token)
                .map_err(PrepareError::Fault)?;
        }
        self.active_frames += 1;

        if token.runtime_id() != runtime_id {
            return Err(AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        if self.active_frames > max_locks {
            return Err(CapacityError::LockLimit.into());
        }
        Ok(())
    }
}

impl<A: DirectBorrowedLockTarget<L>, L: DirectTokenLock> sealed::Capability<A>
    for BorrowedInjectiveLockCommitCapability<A, L>
{
    fn create_plan(
        &'static self,
        public: &'static DirectCommitCapability<A>,
    ) -> Box<dyn ErasedDirectCommitPlan<A>> {
        Box::new(BorrowedInjectiveLockCommitPlan::new(public, self))
    }
}

impl<A: DirectBorrowedLockTarget<L>, L: DirectTokenLock> ErasedDirectCommitPlan<A>
    for BorrowedInjectiveLockCommitPlan<A, L>
{
    fn capability(&self) -> &'static DirectCommitCapability<A> {
        self.public_capability
    }

    fn prepare(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &mut [Option<A::Intent>],
        runtime_id: RuntimeId,
        max_locks: usize,
    ) -> Result<(), PrepareError> {
        if items.len() != intents.len() {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        if self.state != PlanState::Pooled
            || self.active_frames != 0
            || self.acquired_len != 0
            || self.runtime_id.is_some()
            || self.owner.is_some()
            || self.quarantined_callback.is_some()
        {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        self.state = PlanState::Planning;
        self.runtime_id = Some(runtime_id);

        let maximum_planned = items.len().min(max_locks.saturating_add(1));
        if self.frames.capacity() < maximum_planned {
            self.frames
                .try_reserve_exact(maximum_planned.saturating_sub(self.frames.len()))
                .map_err(|_| CapacityError::LockLimit)?;
        }
        // Every frame selected below is checked again by `rebind`; unused
        // high-water frames cannot affect this attempt. `Pooled` is installed
        // only after complete teardown, so retain the whole-vector assertion
        // for diagnostics without scanning every 64-byte frame in release.
        debug_assert!(self.frames.iter().all(BorrowedTokenLockFrame::is_reusable));

        for (item_slot, (item, intent)) in items.iter_mut().zip(intents).enumerate() {
            if intent.is_some() {
                self.plan_writing_item(item_slot, item, intent, runtime_id, max_locks)?;
                continue;
            }
            let (adapter, key, item_view) = item.direct_preflight_parts(intent);
            if (self.implementation.prepare)(adapter, key, item_view)?.is_some() {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
        }

        if self.active_frames == 0 {
            return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
        }
        Ok(())
    }

    fn acquire_all(&mut self, items: &[ItemData<A>], owner: OwnerId) -> Result<(), AcquireError> {
        if self.state != PlanState::Planning {
            return Err(AdapterFault::invariant(AdapterPhase::Acquire).into());
        }
        let runtime_id = self
            .runtime_id
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Acquire))?;
        self.state = PlanState::Acquiring;
        self.owner = Some(owner);
        let cx = AcquireContext::new(&owner);
        for frame_slot in 0..self.active_frames {
            debug_assert_eq!(self.acquired_len, frame_slot);
            let item_slot = self.frames[frame_slot].item_slot;
            let result = match items.get(item_slot) {
                Some(item) => {
                    let (adapter, _) = item.direct_adapter_key_parts();
                    let target = adapter.direct_borrowed_lock_target();
                    self.frames[frame_slot].acquire(runtime_id, target, &cx)
                }
                None => Err(AdapterFault::invariant(AdapterPhase::Acquire).into()),
            };
            if let Err(error) = result {
                self.state = PlanState::Releasing;
                let release = ReleaseContext::new(&owner, None);
                self.release_acquired(items, LockDisposition::Aborted, &release)
                    .map_err(AcquireError::Fault)?;
                self.state = PlanState::Released;
                self.owner = None;
                return Err(error);
            }
            self.acquired_len += 1;
        }
        self.state = PlanState::Held;
        Ok(())
    }

    fn validate(
        &self,
        items: &[ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) -> Result<(), CheckError> {
        if self.state != PlanState::Held || self.owner.is_none() || items.len() != intents.len() {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        let owner = self.owner.expect("held direct token plan has an owner");
        let cx = DirectValidationContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        let mut frame_slot = 0;
        for (item_slot, (item, intent)) in items.iter().zip(intents).enumerate() {
            let has_frame =
                frame_slot < self.active_frames && self.frames[frame_slot].item_slot == item_slot;
            if has_frame && self.implementation.write_acquisition_certifies() {
                if intent.is_none() {
                    return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
                }
                frame_slot += 1;
                continue;
            }
            let (adapter, key, item_view) = item.direct_validation_parts(intent);
            let lock = if has_frame {
                let target = adapter.direct_borrowed_lock_target();
                let lock = self.frames[frame_slot]
                    .lock_ref(target)
                    .map_err(CheckError::Fault)?;
                frame_slot += 1;
                Some(lock)
            } else {
                None
            };
            (self.implementation.validate)(adapter, key, item_view, lock, &cx)?;
        }
        if frame_slot != self.active_frames {
            return Err(AdapterFault::invariant(AdapterPhase::Validation).into());
        }
        Ok(())
    }

    fn install(
        &mut self,
        items: &mut [ItemData<A>],
        intents: &[Option<A::Intent>],
        occ_commit_id: Option<OccCommitId>,
    ) {
        assert_eq!(
            self.state,
            PlanState::Held,
            "direct token install requires held locks"
        );
        assert_eq!(items.len(), intents.len(), "direct item sidecars diverged");
        let owner = self.owner.expect("held direct token plan has an owner");
        let mut cx = DirectInstallContext {
            owner,
            occ_commit_id,
            not_send_sync: PhantomData,
        };
        for frame_slot in 0..self.active_frames {
            let item_slot = self.frames[frame_slot].item_slot;
            let (adapter, key, item) = items[item_slot]
                .direct_install_parts(&intents[item_slot])
                .expect("a direct token frame belongs to a writing item");
            let target = adapter.direct_borrowed_lock_target();
            let lock = self.frames[frame_slot]
                .lock_mut(target)
                .expect("a borrowed direct token frame remains held by its exact target");
            (self.implementation.install)(adapter, key, item, lock, &mut cx);
        }
    }

    fn requires_release(&self) -> bool {
        self.state == PlanState::Held
    }

    fn release_all(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault> {
        if self.state != PlanState::Held {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        self.state = PlanState::Releasing;
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        self.release_acquired(items, disposition, &cx)?;
        self.owner = None;
        if std::mem::needs_drop::<L::Guard>() {
            self.state = PlanState::Released;
        } else {
            // `release_acquired` covered the entire held prefix. Each
            // destructor-free frame is already empty and reusable, so retain
            // no attempt-local metadata for the later contained teardown.
            debug_assert!(self.frames[..self.active_frames]
                .iter()
                .all(BorrowedTokenLockFrame::is_reusable));
            self.active_frames = 0;
            self.runtime_id = None;
            self.state = PlanState::Pooled;
        }
        Ok(())
    }

    fn recover_after_callback_panic(
        &mut self,
        items: &[ItemData<A>],
        disposition: LockDisposition,
    ) -> Result<(), AdapterFault> {
        if !matches!(self.state, PlanState::Acquiring | PlanState::Releasing)
            || self.quarantined_callback.is_some()
        {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        // Acquisition increments `acquired_len` only after a callback
        // returns; release decrements it immediately before the callback.
        // It therefore names the uncertain frame in either state without a
        // hot-path Some/None store around every callback.
        let uncertain = self.acquired_len;
        if uncertain >= self.active_frames {
            return Err(AdapterFault::invariant(AdapterPhase::Release));
        }
        let owner = self
            .owner
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Release))?;
        self.quarantined_callback = Some(uncertain);
        self.state = PlanState::Quarantined;
        let cx = ReleaseContext::new(&owner, disposition.occ_commit_id());
        self.release_acquired(items, disposition, &cx)
    }

    fn teardown_adapter_state(&mut self) -> Result<(), ()> {
        if self.state == PlanState::Pooled {
            debug_assert_eq!(self.active_frames, 0);
            debug_assert_eq!(self.acquired_len, 0);
            debug_assert!(self.runtime_id.is_none());
            debug_assert!(self.owner.is_none());
            debug_assert!(self.quarantined_callback.is_none());
            return Ok(());
        }
        if !matches!(self.state, PlanState::Planning | PlanState::Released)
            || self.owner.is_some()
            || self.acquired_len != 0
            || self.runtime_id.is_none()
            || self.quarantined_callback.is_some()
        {
            return Err(());
        }
        let teardown = catch_unwind(AssertUnwindSafe(|| {
            for frame in self.frames[..self.active_frames].iter_mut().rev() {
                frame.teardown_adapter_state()?;
            }
            Ok::<(), ()>(())
        }));
        if !matches!(teardown, Ok(Ok(()))) {
            return Err(());
        }
        self.active_frames = 0;
        self.runtime_id = None;
        self.state = PlanState::Pooled;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::{LockClass, LockNamespaceId};

    struct DestructorFreeTokenLock;

    struct DestructorFreeGuard(u64);

    impl TransactionLock for DestructorFreeTokenLock {
        type Guard = DestructorFreeGuard;

        fn try_acquire(
            &self,
            _identity: &LockIdentity,
            _cx: &AcquireContext<'_>,
        ) -> Result<Self::Guard, AcquireError> {
            Ok(DestructorFreeGuard(0))
        }

        fn release(
            &self,
            guard: &mut Self::Guard,
            _disposition: LockDisposition,
            _cx: &ReleaseContext<'_>,
        ) {
            guard.0 = 0;
        }
    }

    // SAFETY: the test target has one scalar token namespace, validates no
    // external addresses, and returns a guard for exactly that scalar.
    #[allow(
        unsafe_code,
        reason = "the fixture exercises the compact direct-token frame"
    )]
    unsafe impl DirectTokenLock for DestructorFreeTokenLock {
        type Token = u64;

        fn try_acquire_token(
            &self,
            _runtime_id: RuntimeId,
            token: Self::Token,
            _cx: &AcquireContext<'_>,
        ) -> Result<Self::Guard, AcquireError> {
            Ok(DestructorFreeGuard(token))
        }
    }

    fn identity(key: u64) -> LockIdentity {
        LockIdentity::new(
            RuntimeId::new(1).unwrap(),
            LockNamespaceId::new(2).unwrap(),
            LockClass::new(3).unwrap(),
            key,
        )
    }

    #[test]
    fn exact_identity_index_resolves_hash_collisions_with_full_identity() {
        assert_eq!(std::mem::size_of::<DirectIdentityIndexEntry>(), 16);
        assert_eq!(std::mem::align_of::<DirectIdentityIndexEntry>(), 8);
        let mut index = DirectIdentityIndex::default();
        assert!(index.try_begin(16).unwrap());
        let identities = vec![identity(11), identity(12), identity(11)];

        assert!(!index.contains_or_insert(7, 0, &identities));
        assert!(!index.contains_or_insert(7, 1, &identities));
        assert!(index.contains_or_insert(7, 2, &identities));
    }

    #[test]
    fn exact_identity_index_reuses_storage_with_generation_reset() {
        let mut index = DirectIdentityIndex::default();
        assert!(index.try_begin(9).unwrap());
        let identities: Vec<_> = (0..9).map(identity).collect();
        for slot in 0..identities.len() {
            assert!(!index.contains_or_insert(5, slot, &identities));
        }
        let retained_capacity = index.entries.capacity();

        assert!(index.try_begin(9).unwrap());
        assert_eq!(index.entries.capacity(), retained_capacity);
        assert!(!index.contains_or_insert(5, 0, &identities));
    }

    #[test]
    fn small_identity_sets_keep_the_single_word_filter_lane() {
        let mut index = DirectIdentityIndex::default();
        assert!(!index.try_begin(8).unwrap());
        assert!(index.entries.is_empty());
    }

    #[test]
    fn borrowed_u64_lock_token_is_exactly_two_words() {
        assert_eq!(std::mem::size_of::<BorrowedLockToken<u64>>(), 16);
        assert_eq!(std::mem::align_of::<BorrowedLockToken<u64>>(), 8);
    }

    #[test]
    fn destructor_free_token_guard_recycles_during_successful_release() {
        assert!(!std::mem::needs_drop::<DestructorFreeGuard>());
        let runtime_id = RuntimeId::new(1).unwrap();
        let owner = OwnerId::new(0).unwrap();
        let target = DestructorFreeTokenLock;
        let token = BorrowedLockToken::new(runtime_id, 41);
        let mut frame = BorrowedTokenLockFrame::new(7, &target, token);

        frame
            .acquire(runtime_id, &target, &AcquireContext::new(&owner))
            .unwrap();
        frame
            .release(
                &target,
                LockDisposition::Aborted,
                &ReleaseContext::new(&owner, None),
            )
            .unwrap();

        assert!(frame.is_reusable());
        assert!(frame.teardown_adapter_state().is_ok());
    }
}
