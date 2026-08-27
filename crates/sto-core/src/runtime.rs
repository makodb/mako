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
};

static NEXT_RUNTIME_ID: AtomicU64 = AtomicU64::new(1);

const HEALTHY: u8 = 0;
const POISONED: u8 = 1;
const INDETERMINATE: u8 = 2;
const EXHAUSTED: u8 = 3;

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
    attached_threads: Vec<Option<ThreadId>>,
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
        if config.max_items_per_transaction == 0 {
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

        let mut attached_threads = Vec::new();
        attached_threads
            .try_reserve_exact(config.max_workers)
            .map_err(|_| CapacityError::WorkerLimit)?;
        attached_threads.resize(config.max_workers, None);

        Ok(Arc::new(Self {
            id,
            config,
            next_object_id: AtomicU64::new(1),
            // Version 1 is the initial generation for native resources. The
            // first writing transaction therefore reserves commit ID 2.
            next_commit_id: AtomicU64::new(1),
            owners: Mutex::new(OwnerRegistry { attached_threads }),
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
            .attached_threads
            .iter()
            .flatten()
            .any(|attached| *attached == current_thread)
        {
            return Err(InvalidUse::WorkerBusy.into());
        }

        let Some(slot) = owners.attached_threads.iter().position(Option::is_none) else {
            return Err(CapacityError::OwnerIdExhausted.into());
        };
        let owner = OwnerId::new(slot as u32).map_err(|_| CapacityError::OwnerIdExhausted)?;
        owners.attached_threads[slot] = Some(current_thread);
        drop(owners);

        Ok(WorkerContext {
            runtime: Arc::clone(self),
            owner,
            thread: current_thread,
            transaction_active: false,
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
        let previous =
            match self
                .next_commit_id
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                    if current >= OccCommitId::MAX_VALUE {
                        None
                    } else {
                        Some(current + 1)
                    }
                }) {
                Ok(previous) => previous,
                Err(_) => {
                    let _ = self.health.compare_exchange(
                        HEALTHY,
                        EXHAUSTED,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    );
                    return Err(CapacityError::VersionExhausted);
                }
            };
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
}

/// One attached, same-thread transaction executor.
///
/// The worker cannot migrate to another thread:
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
    not_send_sync: PhantomData<Rc<()>>,
}

impl WorkerContext {
    pub fn runtime(&self) -> &Arc<Runtime> {
        &self.runtime
    }

    pub const fn owner_id(&self) -> OwnerId {
        self.owner
    }

    pub(crate) fn check_thread(&self) -> Result<(), InvalidUse> {
        if thread::current().id() == self.thread {
            Ok(())
        } else {
            Err(InvalidUse::WrongThread)
        }
    }

    pub(crate) fn begin_isolation(&mut self, isolation: IsolationMode) -> Result<(), BeginError> {
        self.check_thread()?;
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
}

impl Drop for WorkerContext {
    fn drop(&mut self) {
        if self.transaction_active {
            self.runtime.poison();
        }
        let mut owners = recover_lock(&self.runtime.owners);
        let slot = self.owner.get() as usize;
        if owners.attached_threads.get(slot) == Some(&Some(self.thread)) {
            owners.attached_threads[slot] = None;
        } else {
            self.runtime.poison();
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
        classes.insert(
            class,
            RegisteredType {
                adapter: TypeId::of::<A>(),
                key: TypeId::of::<A::Key>(),
            },
        );
        drop(classes);

        Ok(RegisteredResource {
            lease: Arc::clone(&self.lease),
            class,
            adapter: Arc::new(adapter),
        })
    }
}

/// Cloneable typed handle for one `(ObjectId, ResourceClass)` binding.
pub struct RegisteredResource<A: TransactionalResource> {
    lease: Arc<ObjectLease>,
    class: ResourceClass,
    adapter: Arc<A>,
}

impl<A: TransactionalResource> Clone for RegisteredResource<A> {
    fn clone(&self) -> Self {
        Self {
            lease: Arc::clone(&self.lease),
            class: self.class,
            adapter: Arc::clone(&self.adapter),
        }
    }
}

impl<A: TransactionalResource> std::fmt::Debug for RegisteredResource<A> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("RegisteredResource")
            .field("runtime_id", &self.runtime_id())
            .field("object_id", &self.object_id())
            .field("class", &self.class)
            .field("adapter_type", &TypeId::of::<A>())
            .finish_non_exhaustive()
    }
}

impl<A: TransactionalResource> RegisteredResource<A> {
    pub fn adapter(&self) -> &A {
        &self.adapter
    }

    pub fn runtime_id(&self) -> RuntimeId {
        self.lease.runtime.id()
    }

    pub fn object_id(&self) -> ObjectId {
        self.lease.object_id
    }

    pub const fn resource_class(&self) -> ResourceClass {
        self.class
    }

    pub(crate) fn validate_binding(&self) -> Result<(), InvalidUse> {
        let classes = recover_lock(&self.lease.classes);
        let Some(binding) = classes.get(&self.class) else {
            return Err(InvalidUse::ResourceTypeMismatch);
        };
        if binding.adapter != TypeId::of::<A>() || binding.key != TypeId::of::<A::Key>() {
            Err(InvalidUse::ResourceTypeMismatch)
        } else {
            Ok(())
        }
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
