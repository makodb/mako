//! Runtime, worker, and transactional-object registration.

use std::{
    any::TypeId,
    collections::HashMap,
    marker::PhantomData,
    rc::Rc,
    sync::{
        atomic::{AtomicU64, AtomicU8, Ordering},
        Arc, Mutex, MutexGuard, Weak,
    },
    thread::{self, ThreadId},
};

use crate::{
    adapter::TransactionalResource,
    error::{
        AttachError, BeginError, CapacityError, FailurePhase, InvalidUse, PoisonInfo,
        RegistrationError, RuntimeError, Unsupported,
    },
    identity::{ObjectId, OccCommitId, OwnerId, ResourceClass, RuntimeId},
    transaction::TransactionScratch,
};

static NEXT_RUNTIME_ID: AtomicU64 = AtomicU64::new(1);

const HEALTHY: u8 = 0;
const POISONED: u8 = 1;
const INDETERMINATE: u8 = 2;
const EXHAUSTED: u8 = 3;

// A lock-plan nonce is runtime-scoped and packs a persistent per-owner
// generation above the complete OwnerId representation. Generation zero is
// reserved so every issued nonce is nonzero, including for owner zero.
const LOCK_PLAN_OWNER_BITS: u32 = u16::BITS;
const FIRST_LOCK_PLAN_GENERATION: u64 = 1;
const MAX_LOCK_PLAN_GENERATION: u64 = u64::MAX >> LOCK_PLAN_OWNER_BITS;

/// Isolation policy selected for a transaction.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum IsolationMode {
    /// Strict serializability for committed transactions, with final OCC
    /// validation but no promise that intermediate reads are opaque.
    Serializable,
    /// Execution-time revalidation against an ordered runtime clock.
    ///
    /// This capability is reserved by the public API but is not negotiated by
    /// the first native implementation slice.
    Opaque,
}

/// Bounded resources and default policy for one independent STO runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RuntimeConfig {
    default_isolation: IsolationMode,
    max_workers: usize,
    max_items_per_transaction: usize,
    max_locks_per_transaction: usize,
}

impl RuntimeConfig {
    /// Constructs the conservative native default configuration.
    pub const fn new() -> Self {
        Self {
            default_isolation: IsolationMode::Serializable,
            max_workers: 512,
            max_items_per_transaction: 4_096,
            max_locks_per_transaction: 4_096,
        }
    }

    /// Selects the default isolation policy used by [`crate::WorkerContext::begin`].
    pub const fn with_default_isolation(mut self, isolation: IsolationMode) -> Self {
        self.default_isolation = isolation;
        self
    }

    /// Sets the maximum number of simultaneously attached workers.
    pub const fn with_max_workers(mut self, max_workers: usize) -> Self {
        self.max_workers = max_workers;
        self
    }

    /// Sets the maximum number of logical items in one transaction.
    pub const fn with_max_items_per_transaction(mut self, max_items: usize) -> Self {
        self.max_items_per_transaction = max_items;
        self
    }

    /// Sets the maximum number of unique physical locks in one transaction.
    pub const fn with_max_locks_per_transaction(mut self, max_locks: usize) -> Self {
        self.max_locks_per_transaction = max_locks;
        self
    }

    pub const fn default_isolation(&self) -> IsolationMode {
        self.default_isolation
    }

    pub const fn max_workers(&self) -> usize {
        self.max_workers
    }

    pub const fn max_items_per_transaction(&self) -> usize {
        self.max_items_per_transaction
    }

    pub const fn max_locks_per_transaction(&self) -> usize {
        self.max_locks_per_transaction
    }
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self::new()
    }
}

/// Observable quarantine state of a runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RuntimeHealth {
    Healthy,
    Poisoned,
    Indeterminate,
    /// The checked OCC commit-ID domain is permanently exhausted.
    Exhausted,
}

#[derive(Debug)]
struct OwnerRegistry {
    slots: Vec<OwnerSlot>,
}

#[derive(Debug)]
struct OwnerSlot {
    attached_thread: Option<ThreadId>,
    // This value survives detach/reattach so a stale LockUse can never alias
    // a later plan created by a worker that reuses the same OwnerId.
    next_lock_plan_generation: u64,
}

#[derive(Debug, Clone, Copy)]
struct RegisteredType {
    adapter: TypeId,
    key: TypeId,
}

#[derive(Debug)]
struct ObjectLease {
    runtime: Arc<Runtime>,
    object_id: ObjectId,
    classes: Mutex<HashMap<ResourceClass, RegisteredType>>,
}

impl Drop for ObjectLease {
    fn drop(&mut self) {
        let mut objects = recover_lock(&self.runtime.objects);
        objects.remove(&self.object_id);
    }
}

/// One independent version clock, owner domain, and object registry.
#[derive(Debug)]
pub struct Runtime {
    id: RuntimeId,
    config: RuntimeConfig,
    next_object_id: AtomicU64,
    next_commit_id: AtomicU64,
    owners: Mutex<OwnerRegistry>,
    objects: Mutex<HashMap<ObjectId, Weak<ObjectLease>>>,
    health: AtomicU8,
}

impl Runtime {
    /// Creates a native STO runtime.
    pub fn new(config: RuntimeConfig) -> Result<Arc<Self>, RuntimeError> {
        if config.default_isolation == IsolationMode::Opaque {
            return Err(Unsupported::IsolationMode.into());
        }
        if config.max_workers == 0 || config.max_workers > OwnerId::MAX_VALUE as usize + 1 {
            return Err(CapacityError::WorkerLimit.into());
        }
        if config.max_items_per_transaction == 0
            || config.max_items_per_transaction > u32::MAX as usize
        {
            return Err(CapacityError::ItemLimit.into());
        }
        if config.max_locks_per_transaction == 0 {
            return Err(CapacityError::LockLimit.into());
        }

        let raw_id = NEXT_RUNTIME_ID
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                current.checked_add(1).filter(|next| *next != 0)
            })
            .map_err(|_| CapacityError::RuntimeIdExhausted)?;
        let id = RuntimeId::new(raw_id).map_err(|_| CapacityError::RuntimeIdExhausted)?;

        let mut owner_slots = Vec::new();
        owner_slots
            .try_reserve_exact(config.max_workers)
            .map_err(|_| CapacityError::WorkerLimit)?;
        owner_slots.resize_with(config.max_workers, || OwnerSlot {
            attached_thread: None,
            next_lock_plan_generation: FIRST_LOCK_PLAN_GENERATION,
        });

        Ok(Arc::new(Self {
            id,
            config,
            next_object_id: AtomicU64::new(1),
            // Version 1 is the initial generation for native resources. The
            // first writing transaction therefore reserves commit ID 2.
            next_commit_id: AtomicU64::new(1),
            owners: Mutex::new(OwnerRegistry { slots: owner_slots }),
            objects: Mutex::new(HashMap::new()),
            health: AtomicU8::new(HEALTHY),
        }))
    }

    /// Returns this runtime's process-unique identity.
    pub const fn id(&self) -> RuntimeId {
        self.id
    }

    pub const fn config(&self) -> &RuntimeConfig {
        &self.config
    }

    /// Returns the current quarantine state.
    pub fn health(&self) -> RuntimeHealth {
        match self.health.load(Ordering::Acquire) {
            HEALTHY => RuntimeHealth::Healthy,
            POISONED => RuntimeHealth::Poisoned,
            INDETERMINATE => RuntimeHealth::Indeterminate,
            _ => RuntimeHealth::Exhausted,
        }
    }

    /// Attaches a thread-affine worker to this runtime.
    pub fn attach(self: &Arc<Self>) -> Result<WorkerContext, AttachError> {
        self.ensure_healthy(FailurePhase::Attach)?;
        let current_thread = thread::current().id();
        let mut owners = recover_lock(&self.owners);

        if owners
            .slots
            .iter()
            .filter_map(|slot| slot.attached_thread.as_ref())
            .any(|attached| *attached == current_thread)
        {
            return Err(InvalidUse::WorkerBusy.into());
        }

        let Some(slot) = owners
            .slots
            .iter()
            .position(|slot| slot.attached_thread.is_none())
        else {
            return Err(CapacityError::OwnerIdExhausted.into());
        };
        let owner = OwnerId::new(slot as u32).map_err(|_| CapacityError::OwnerIdExhausted)?;
        let owner_slot = &mut owners.slots[slot];
        owner_slot.attached_thread = Some(current_thread);
        let next_lock_plan_generation = owner_slot.next_lock_plan_generation;
        drop(owners);

        Ok(WorkerContext {
            runtime: Arc::clone(self),
            owner,
            thread: current_thread,
            transaction_active: false,
            transaction_scratch: Some(TransactionScratch::new(Arc::clone(self))),
            next_lock_plan_generation,
            not_send_sync: PhantomData,
        })
    }

    /// Creates a stable transactional-object lease.
    pub fn register_object(self: &Arc<Self>) -> Result<ObjectRegistration, RegistrationError> {
        self.ensure_healthy(FailurePhase::Registration)?;
        let raw_id = self
            .next_object_id
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                current.checked_add(1).filter(|next| *next != 0)
            })
            .map_err(|_| CapacityError::ObjectIdExhausted)?;
        let object_id = ObjectId::new(raw_id).map_err(|_| CapacityError::ObjectIdExhausted)?;

        let lease = Arc::new(ObjectLease {
            runtime: Arc::clone(self),
            object_id,
            classes: Mutex::new(HashMap::new()),
        });
        let mut objects = recover_lock(&self.objects);
        objects
            .try_reserve(1)
            .map_err(|_| CapacityError::ObjectIdExhausted)?;
        objects.insert(object_id, Arc::downgrade(&lease));
        drop(objects);

        Ok(ObjectRegistration { lease })
    }

    pub(crate) fn reserve_commit_id(&self) -> Result<OccCommitId, CapacityError> {
        // A writing commit needs only a unique, monotonically increasing
        // ticket.  `fetch_update` implements that with a contended CAS loop;
        // under many writers its retries serialize progress much more than
        // the single hardware xadd used by native STO.  Exhaustion is
        // terminal, so it is safe to consume one value past the representable
        // range and quarantine the runtime.  At most the already attached,
        // bounded worker set can reach this point after the transition.
        let previous = self.next_commit_id.fetch_add(1, Ordering::AcqRel);
        if previous >= OccCommitId::MAX_VALUE {
            let _ = self.health.compare_exchange(
                HEALTHY,
                EXHAUSTED,
                Ordering::AcqRel,
                Ordering::Acquire,
            );
            return Err(CapacityError::VersionExhausted);
        }
        OccCommitId::new(previous + 1).map_err(|_| CapacityError::VersionExhausted)
    }

    pub(crate) fn poison(&self) {
        let _ =
            self.health
                .compare_exchange(HEALTHY, POISONED, Ordering::AcqRel, Ordering::Acquire);
    }

    pub(crate) fn mark_indeterminate(&self) {
        self.health.store(INDETERMINATE, Ordering::Release);
    }

    pub(crate) fn ensure_healthy(&self, phase: FailurePhase) -> Result<(), PoisonInfo> {
        match self.health() {
            RuntimeHealth::Healthy => Ok(()),
            RuntimeHealth::Poisoned => Err(PoisonInfo::new(phase, "runtime is poisoned")),
            RuntimeHealth::Indeterminate => Err(PoisonInfo::new(
                phase,
                "runtime has indeterminate publication",
            )),
            RuntimeHealth::Exhausted => {
                Err(PoisonInfo::new(phase, "runtime commit clock is exhausted"))
            }
        }
    }

    #[cfg(test)]
    pub(crate) fn has_registered_object(&self, object_id: ObjectId) -> bool {
        recover_lock(&self.objects)
            .get(&object_id)
            .is_some_and(|lease| lease.strong_count() != 0)
    }
}

/// One attached, same-thread transaction executor.
///
/// The worker cannot migrate to another thread:
///
/// To reuse item allocations without shared reference-count traffic, an idle
/// worker may retain one typed resource lease per slot in its peak transaction
/// item count. A lease is released when that slot is rebound or the worker is
/// dropped; retention is therefore bounded by the runtime's configured maximum
/// items per transaction.
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<sto_core::WorkerContext>();
/// ```
#[derive(Debug)]
pub struct WorkerContext {
    pub(crate) runtime: Arc<Runtime>,
    pub(crate) owner: OwnerId,
    pub(crate) thread: ThreadId,
    pub(crate) transaction_active: bool,
    pub(crate) transaction_scratch: Option<TransactionScratch>,
    next_lock_plan_generation: u64,
    not_send_sync: PhantomData<Rc<()>>,
}

impl WorkerContext {
    pub fn runtime(&self) -> &Arc<Runtime> {
        &self.runtime
    }

    pub const fn owner_id(&self) -> OwnerId {
        self.owner
    }

    pub(crate) fn begin_isolation(&mut self, isolation: IsolationMode) -> Result<(), BeginError> {
        // WorkerContext is structurally !Send + !Sync, so safe code cannot
        // invoke this on another thread. Keep the invariant check in debug
        // builds without paying for a thread-ID query on every transaction.
        debug_assert_eq!(thread::current().id(), self.thread);
        self.runtime.ensure_healthy(FailurePhase::Begin)?;
        if self.transaction_active {
            return Err(InvalidUse::WorkerBusy.into());
        }
        if isolation == IsolationMode::Opaque {
            return Err(Unsupported::IsolationMode.into());
        }
        self.transaction_active = true;
        Ok(())
    }

    pub(crate) fn finish_transaction(&mut self) {
        self.transaction_active = false;
    }

    pub(crate) fn recycle_transaction_scratch(&mut self, scratch: TransactionScratch) {
        debug_assert!(self.transaction_active);
        debug_assert!(self.transaction_scratch.is_none());
        self.transaction_scratch = Some(scratch);
    }

    /// Reserves a runtime-scoped identity for a lock plan.
    ///
    /// The generation is consumed before adapter preflight begins. An aborted
    /// or panicking attempt therefore leaves a gap instead of permitting a
    /// retained `LockUse` to alias a later plan.
    pub(crate) fn reserve_lock_plan_nonce(&mut self) -> Result<u64, CapacityError> {
        let generation = self.next_lock_plan_generation;
        if !(FIRST_LOCK_PLAN_GENERATION..=MAX_LOCK_PLAN_GENERATION).contains(&generation) {
            return Err(CapacityError::LockLimit);
        }

        // MAX_LOCK_PLAN_GENERATION + 1 is an in-range exhausted sentinel. It
        // persists with the owner slot and is never packed into a nonce.
        self.next_lock_plan_generation = generation + 1;
        Ok((generation << LOCK_PLAN_OWNER_BITS) | u64::from(self.owner.get()))
    }
}

impl Drop for WorkerContext {
    fn drop(&mut self) {
        if self.transaction_active {
            self.runtime.poison();
        }
        if let Some(scratch) = self.transaction_scratch.take() {
            if let Err(quarantined) = scratch.dispose_retained_resources() {
                // A retained adapter destructor unwound. Keep the entire
                // scratch allocation quarantined so no later pooled destructor
                // runs during this unwind, and make the failure observable to
                // every other worker through runtime health.
                self.runtime.poison();
                std::mem::forget(quarantined);
            }
        }
        let mut owners = recover_lock(&self.runtime.owners);
        let slot = self.owner.get() as usize;
        match owners.slots.get_mut(slot) {
            Some(owner_slot) if owner_slot.attached_thread.as_ref() == Some(&self.thread) => {
                // Persist before making the OwnerId available for reuse.
                owner_slot.next_lock_plan_generation = self.next_lock_plan_generation;
                owner_slot.attached_thread = None;
            }
            _ => self.runtime.poison(),
        }
    }
}

/// Lease used to bind one or more typed resource classes to an object ID.
#[derive(Debug)]
pub struct ObjectRegistration {
    lease: Arc<ObjectLease>,
}

impl ObjectRegistration {
    pub fn object_id(&self) -> ObjectId {
        self.lease.object_id
    }

    pub fn runtime_id(&self) -> RuntimeId {
        self.lease.runtime.id()
    }

    /// Registers one adapter type for one resource class.
    pub fn register_resource<A: TransactionalResource>(
        &self,
        class: ResourceClass,
        adapter: A,
    ) -> Result<RegisteredResource<A>, RegistrationError> {
        self.lease
            .runtime
            .ensure_healthy(FailurePhase::Registration)?;
        let mut classes = recover_lock(&self.lease.classes);
        if classes.contains_key(&class) {
            return Err(InvalidUse::DuplicateResourceClass.into());
        }
        classes
            .try_reserve(1)
            .map_err(|_| CapacityError::ObjectIdExhausted)?;
        let registered_type = RegisteredType {
            adapter: TypeId::of::<A>(),
            key: TypeId::of::<A::Key>(),
        };
        classes.insert(class, registered_type);
        drop(classes);

        Ok(RegisteredResource {
            binding: Arc::new(ResourceBinding {
                // Drop adapter-owned state while this immutable binding still
                // retains the transactional object's lease.
                adapter,
                lease: Arc::clone(&self.lease),
                class,
                registered_type,
            }),
        })
    }
}

/// Immutable allocation shared by every clone of one typed resource handle.
///
/// Keeping identity, type proof, adapter, and object lease together makes the
/// public capability one `Arc` wide without weakening registration identity.
struct ResourceBinding<A: TransactionalResource> {
    adapter: A,
    lease: Arc<ObjectLease>,
    class: ResourceClass,
    registered_type: RegisteredType,
}

/// Cloneable typed handle for one `(ObjectId, ResourceClass)` binding.
#[repr(transparent)]
pub struct RegisteredResource<A: TransactionalResource> {
    binding: Arc<ResourceBinding<A>>,
}

impl<A: TransactionalResource> Clone for RegisteredResource<A> {
    fn clone(&self) -> Self {
        Self {
            binding: Arc::clone(&self.binding),
        }
    }
}

impl<A: TransactionalResource> std::fmt::Debug for RegisteredResource<A> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("RegisteredResource")
            .field("runtime_id", &self.runtime_id())
            .field("object_id", &self.object_id())
            .field("class", &self.resource_class())
            .field("adapter_type", &TypeId::of::<A>())
            .finish_non_exhaustive()
    }
}

impl<A: TransactionalResource> RegisteredResource<A> {
    #[inline]
    pub fn adapter(&self) -> &A {
        &self.binding.adapter
    }

    #[inline]
    pub fn runtime_id(&self) -> RuntimeId {
        self.binding.lease.runtime.id()
    }

    #[inline]
    pub fn object_id(&self) -> ObjectId {
        self.binding.lease.object_id
    }

    #[inline]
    pub fn resource_class(&self) -> ResourceClass {
        self.binding.class
    }

    #[inline]
    pub(crate) fn is_same_binding(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.binding, &other.binding)
    }

    #[inline(always)]
    pub(crate) fn binding_identity(&self) -> std::num::NonZeroUsize {
        // Arc allocations are nonnull and stable until the last strong handle
        // is dropped. The erased address is compared only; core never
        // dereferences it or reconstructs an Arc from it.
        std::num::NonZeroUsize::new(Arc::as_ptr(&self.binding).addr())
            .expect("an Arc allocation has a nonzero address")
    }

    #[inline]
    pub(crate) fn validate_for_runtime(&self, runtime_id: RuntimeId) -> Result<(), InvalidUse> {
        let binding = self.binding.as_ref();
        if binding.lease.runtime.id() != runtime_id {
            return Err(InvalidUse::WrongRuntime);
        }
        if binding.registered_type.adapter != TypeId::of::<A>()
            || binding.registered_type.key != TypeId::of::<A::Key>()
        {
            return Err(InvalidUse::ResourceTypeMismatch);
        }
        Ok(())
    }
}

fn recover_lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    use crate::{
        AbortReason, AcquireContext, AcquireError, CheckError, CommitFailure, CommitOutcome,
        Conflict, ExecutionCheckContext, FinishContext, FinishDisposition, FinishItem,
        InstallContext, InstallItem, ItemInitError, LockClass, LockDisposition, LockIdentity,
        LockNamespaceId, LockRequest, LockUse, NoPredicate, ObservationOrder, OpacityToken,
        PredicateContext, PreflightContext, PreflightItem, PrepareError, ReleaseContext,
        ResourceClass, TransactionLock, TransactionalResource, ValidationContext,
    };

    const PREFLIGHT_SUCCEEDS: u8 = 0;
    const PREFLIGHT_CONFLICTS: u8 = 1;
    const PREFLIGHT_PANICS: u8 = 2;

    #[derive(Debug)]
    struct NonceTestLock;

    impl TransactionLock for NonceTestLock {
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

    #[derive(Clone, Copy)]
    struct NonceObservation;

    impl OpacityToken for NonceObservation {
        fn observation_order(&self) -> ObservationOrder {
            ObservationOrder::Unordered
        }
    }

    struct NonceAdapter {
        runtime_id: RuntimeId,
        behavior: Arc<AtomicU8>,
        preflight_calls: Arc<AtomicUsize>,
        observed_nonces: Arc<Mutex<Vec<u64>>>,
    }

    impl TransactionalResource for NonceAdapter {
        type Key = u64;
        type Local = ();
        type Observation = NonceObservation;
        type Predicate = NoPredicate;
        type Intent = ();
        type Prepared = LockUse<NonceTestLock>;

        fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
            Ok(())
        }

        fn preflight(
            &self,
            key: &Self::Key,
            _item: PreflightItem<'_, Self>,
            cx: &mut PreflightContext<'_>,
        ) -> Result<Self::Prepared, PrepareError> {
            self.preflight_calls.fetch_add(1, Ordering::Relaxed);
            let lock_use = cx.require_lock(LockRequest::new(
                LockIdentity::new(
                    self.runtime_id,
                    LockNamespaceId::new(1).unwrap(),
                    LockClass::new(1).unwrap(),
                    *key,
                ),
                Arc::new(NonceTestLock),
            ))?;
            recover_lock(&self.observed_nonces).push(lock_use.plan_nonce_for_test());

            match self.behavior.load(Ordering::Relaxed) {
                PREFLIGHT_SUCCEEDS => Ok(lock_use),
                PREFLIGHT_CONFLICTS => Err(Conflict::ReadValidation.into()),
                PREFLIGHT_PANICS => panic!("injected preflight panic after LockUse creation"),
                other => panic!("unknown test preflight behavior {other}"),
            }
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
            prepared: &mut Self::Prepared,
            cx: &mut InstallContext<'_>,
        ) {
            cx.guard_mut(prepared)
                .expect("the current transaction's LockUse must resolve");
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

    struct NonceFixture {
        resource: RegisteredResource<NonceAdapter>,
        behavior: Arc<AtomicU8>,
        preflight_calls: Arc<AtomicUsize>,
        observed_nonces: Arc<Mutex<Vec<u64>>>,
    }

    impl NonceFixture {
        fn new(runtime: &Arc<Runtime>) -> Self {
            let behavior = Arc::new(AtomicU8::new(PREFLIGHT_SUCCEEDS));
            let preflight_calls = Arc::new(AtomicUsize::new(0));
            let observed_nonces = Arc::new(Mutex::new(Vec::new()));
            let object = runtime.register_object().unwrap();
            let resource = object
                .register_resource(
                    ResourceClass::new(1).unwrap(),
                    NonceAdapter {
                        runtime_id: runtime.id(),
                        behavior: Arc::clone(&behavior),
                        preflight_calls: Arc::clone(&preflight_calls),
                        observed_nonces: Arc::clone(&observed_nonces),
                    },
                )
                .unwrap();
            Self {
                resource,
                behavior,
                preflight_calls,
                observed_nonces,
            }
        }

        fn set_behavior(&self, behavior: u8) {
            self.behavior.store(behavior, Ordering::Relaxed);
        }

        fn nonces(&self) -> Vec<u64> {
            recover_lock(&self.observed_nonces).clone()
        }
    }

    fn commit_nonce_transaction(
        worker: &mut WorkerContext,
        fixture: &NonceFixture,
    ) -> Result<CommitOutcome, CommitFailure> {
        let mut transaction = worker.begin().unwrap();
        transaction
            .with_item(&fixture.resource, 7, |entry| entry.stage(()))
            .unwrap();
        transaction.commit()
    }

    fn packed_plan_nonce(generation: u64, owner: OwnerId) -> u64 {
        (generation << LOCK_PLAN_OWNER_BITS) | u64::from(owner.get())
    }

    #[test]
    fn runtime_ids_and_object_ids_are_nonzero_and_distinct() {
        let first = Runtime::new(RuntimeConfig::default()).unwrap();
        let second = Runtime::new(RuntimeConfig::default()).unwrap();
        assert_ne!(first.id(), second.id());

        let a = first.register_object().unwrap();
        let b = first.register_object().unwrap();
        assert_ne!(a.object_id(), b.object_id());
        assert_eq!(a.runtime_id(), first.id());
    }

    #[test]
    fn worker_capacity_is_checked_and_released() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let worker = runtime.attach().unwrap();
        assert!(matches!(
            runtime.attach(),
            Err(AttachError::InvalidUse(InvalidUse::WorkerBusy))
        ));
        drop(worker);
        assert!(runtime.attach().is_ok());
    }

    #[test]
    fn successful_transactions_receive_distinct_lock_plan_nonces() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let fixture = NonceFixture::new(&runtime);
        let mut worker = runtime.attach().unwrap();
        let owner = worker.owner_id();

        assert!(matches!(
            commit_nonce_transaction(&mut worker, &fixture).unwrap(),
            CommitOutcome::Committed(_)
        ));
        assert!(matches!(
            commit_nonce_transaction(&mut worker, &fixture).unwrap(),
            CommitOutcome::Committed(_)
        ));

        assert_eq!(
            fixture.nonces(),
            vec![packed_plan_nonce(1, owner), packed_plan_nonce(2, owner)]
        );
        assert_eq!(worker.next_lock_plan_generation, 3);
    }

    #[test]
    fn preflight_abort_consumes_a_generation_before_the_next_transaction() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let fixture = NonceFixture::new(&runtime);
        let mut worker = runtime.attach().unwrap();
        let owner = worker.owner_id();

        fixture.set_behavior(PREFLIGHT_CONFLICTS);
        assert_eq!(
            commit_nonce_transaction(&mut worker, &fixture).unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(worker.next_lock_plan_generation, 2);

        fixture.set_behavior(PREFLIGHT_SUCCEEDS);
        assert!(matches!(
            commit_nonce_transaction(&mut worker, &fixture).unwrap(),
            CommitOutcome::Committed(_)
        ));
        assert_eq!(
            fixture.nonces(),
            vec![packed_plan_nonce(1, owner), packed_plan_nonce(2, owner)]
        );
        assert_eq!(worker.next_lock_plan_generation, 3);
    }

    #[test]
    fn preflight_panic_after_lock_use_creation_consumes_its_generation() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let fixture = NonceFixture::new(&runtime);
        let mut worker = runtime.attach().unwrap();
        let owner = worker.owner_id();
        fixture.set_behavior(PREFLIGHT_PANICS);

        assert!(matches!(
            commit_nonce_transaction(&mut worker, &fixture),
            Err(CommitFailure::Poisoned { .. })
        ));
        assert_eq!(fixture.nonces(), vec![packed_plan_nonce(1, owner)]);
        assert_eq!(worker.next_lock_plan_generation, 2);
        assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    }

    #[test]
    fn owner_detach_and_reuse_preserves_the_next_plan_generation() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let fixture = NonceFixture::new(&runtime);
        let mut first_worker = runtime.attach().unwrap();
        let owner = first_worker.owner_id();

        assert!(matches!(
            commit_nonce_transaction(&mut first_worker, &fixture).unwrap(),
            CommitOutcome::Committed(_)
        ));
        drop(first_worker);

        let mut reused_worker = runtime.attach().unwrap();
        assert_eq!(reused_worker.owner_id(), owner);
        assert_eq!(reused_worker.next_lock_plan_generation, 2);
        assert!(matches!(
            commit_nonce_transaction(&mut reused_worker, &fixture).unwrap(),
            CommitOutcome::Committed(_)
        ));
        assert_eq!(
            fixture.nonces(),
            vec![packed_plan_nonce(1, owner), packed_plan_nonce(2, owner)]
        );
    }

    #[test]
    fn exhausted_plan_generation_never_wraps_and_fails_before_preflight() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let fixture = NonceFixture::new(&runtime);
        let mut worker = runtime.attach().unwrap();
        let owner = worker.owner_id();
        worker.next_lock_plan_generation = MAX_LOCK_PLAN_GENERATION;

        assert!(matches!(
            commit_nonce_transaction(&mut worker, &fixture).unwrap(),
            CommitOutcome::Committed(_)
        ));
        assert_eq!(
            fixture.nonces(),
            vec![packed_plan_nonce(MAX_LOCK_PLAN_GENERATION, owner)]
        );
        assert_eq!(fixture.preflight_calls.load(Ordering::Relaxed), 1);
        assert_eq!(
            worker.next_lock_plan_generation,
            MAX_LOCK_PLAN_GENERATION + 1
        );

        assert_eq!(
            commit_nonce_transaction(&mut worker, &fixture).unwrap(),
            CommitOutcome::Aborted(AbortReason::Capacity(CapacityError::LockLimit))
        );
        assert_eq!(fixture.preflight_calls.load(Ordering::Relaxed), 1);
        assert_eq!(fixture.nonces().len(), 1);
        assert_eq!(
            worker.next_lock_plan_generation,
            MAX_LOCK_PLAN_GENERATION + 1
        );
        assert_eq!(runtime.health(), RuntimeHealth::Healthy);

        drop(worker);
        let mut reused_worker = runtime.attach().unwrap();
        assert_eq!(
            reused_worker.next_lock_plan_generation,
            MAX_LOCK_PLAN_GENERATION + 1
        );
        assert_eq!(
            commit_nonce_transaction(&mut reused_worker, &fixture).unwrap(),
            CommitOutcome::Aborted(AbortReason::Capacity(CapacityError::LockLimit))
        );
        assert_eq!(fixture.preflight_calls.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn opaque_runtime_is_rejected_instead_of_downgraded() {
        let result =
            Runtime::new(RuntimeConfig::new().with_default_isolation(IsolationMode::Opaque));
        assert_eq!(
            result.unwrap_err(),
            RuntimeError::Unsupported(Unsupported::IsolationMode)
        );
    }

    #[test]
    fn commit_clock_exhaustion_is_terminal_and_refuses_new_transactions() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        runtime
            .next_commit_id
            .store(OccCommitId::MAX_VALUE, Ordering::Release);

        assert_eq!(
            runtime.reserve_commit_id(),
            Err(CapacityError::VersionExhausted)
        );
        assert_eq!(runtime.health(), RuntimeHealth::Exhausted);
        assert!(matches!(worker.begin(), Err(BeginError::Poisoned(_))));
        assert!(matches!(runtime.attach(), Err(AttachError::Poisoned(_))));
    }
}
