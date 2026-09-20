//! Private whole-value snapshot adapter used by reference datatypes.

use std::sync::Arc;

use arc_swap::ArcSwap;
use sto_core::{
    AccessError, Active, AdapterFault, AdapterFaultKind, AdapterPhase, AtomicVersion, CheckError,
    Conflict, Entry, ExecutionCheckContext, FinishContext, FinishDisposition, FinishItem,
    InstallContext, InstallItem, LockClass, LockIdentity, LockNamespaceId, LockRequest, LockUse,
    NoPredicate, ObjectId, ObservationOrder, ObservationRef, OccVersion, OpacityToken,
    PredicateContext, PreflightContext, PreflightItem, PrepareError, RegisteredResource,
    RegistrationError, ResourceClass, Runtime, Transaction, TransactionalResource,
    ValidationContext, VersionLock,
};

const SNAPSHOT_RESOURCE_CLASS_VALUE: u32 = 1;
const SNAPSHOT_LOCK_CLASS_VALUE: u32 = 1;
const SNAPSHOT_LOCK_KEY: u64 = 0;

/// One registered, transactionally replaced immutable value.
pub(crate) struct Snapshot<T: Clone + Send + Sync + 'static> {
    resource: RegisteredResource<SnapshotAdapter<T>>,
}

impl<T: Clone + Send + Sync + 'static> Snapshot<T> {
    pub(crate) fn new(runtime: &Arc<Runtime>, initial: T) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a LockNamespaceId");
        let resource_class = ResourceClass::new(SNAPSHOT_RESOURCE_CLASS_VALUE)
            .expect("the private snapshot resource class is nonzero");
        let lock_class = LockClass::new(SNAPSHOT_LOCK_CLASS_VALUE)
            .expect("the private snapshot lock class is nonzero");

        let version = Arc::new(AtomicVersion::default());
        let lock = Arc::new(VersionLock::new(Arc::clone(&version)));
        let lock_identity = LockIdentity::new(
            object.runtime_id(),
            namespace,
            lock_class,
            SNAPSHOT_LOCK_KEY,
        );
        let adapter = SnapshotAdapter {
            value: ArcSwap::from_pointee(initial),
            version,
            lock,
            lock_identity,
        };
        let resource = object.register_resource(resource_class, adapter)?;
        Ok(Self { resource })
    }

    /// Runs `operation` against this transaction's current immutable snapshot.
    pub(crate) fn inspect<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        operation: impl FnOnce(&T) -> R,
    ) -> Result<R, AccessError> {
        let adapter = self.resource.adapter();
        txn.with_item(&self.resource, (), |entry| {
            let snapshot = adapter.snapshot(entry)?;
            Ok(operation(snapshot.as_ref()))
        })
    }

    /// Clones and mutates a transaction-local replacement snapshot.
    ///
    /// Both the value clone and its `Arc` allocation happen during execution.
    /// Installation only clones and swaps that prepared `Arc`.
    pub(crate) fn update<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        operation: impl FnOnce(&mut T) -> R,
    ) -> Result<R, AccessError> {
        let adapter = self.resource.adapter();
        txn.with_item(&self.resource, (), |entry| {
            let snapshot = adapter.snapshot(entry)?;
            let mut replacement = snapshot.as_ref().clone();
            let result = operation(&mut replacement);
            let replacement = Arc::new(replacement);
            entry.stage(replacement)?;
            Ok(result)
        })
    }

    /// Tries a transaction-local mutation whose rejected result stages no
    /// replacement.
    ///
    /// The operation and the drop of any unused captured input both run under
    /// the transaction failure boundary. This matters for checked collection
    /// operations whose user-defined value destructor can unwind.
    pub(crate) fn update_checked<R, E>(
        &self,
        txn: &mut Transaction<'_, Active>,
        operation: impl FnOnce(&mut T) -> Result<R, E>,
    ) -> Result<Result<R, E>, AccessError> {
        let adapter = self.resource.adapter();
        txn.with_item(&self.resource, (), |entry| {
            let snapshot = adapter.snapshot(entry)?;
            let mut replacement = snapshot.as_ref().clone();
            match operation(&mut replacement) {
                Ok(result) => {
                    entry.stage(Arc::new(replacement))?;
                    Ok(Ok(result))
                }
                Err(error) => Ok(Err(error)),
            }
        })
    }

    pub(crate) fn object_id(&self) -> ObjectId {
        self.resource.object_id()
    }
}

impl<T: Clone + Send + Sync + 'static> Clone for Snapshot<T> {
    fn clone(&self) -> Self {
        Self {
            resource: self.resource.clone(),
        }
    }
}

struct SnapshotAdapter<T: Clone + Send + Sync + 'static> {
    value: ArcSwap<T>,
    version: Arc<AtomicVersion>,
    lock: Arc<VersionLock>,
    lock_identity: LockIdentity,
}

struct SnapshotLocal<T> {
    first_snapshot: Option<Arc<T>>,
    displaced: Option<Arc<T>>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct SnapshotObservation {
    version: OccVersion,
}

impl OpacityToken for SnapshotObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct SnapshotPrepared {
    lock_use: Option<LockUse<VersionLock>>,
}

impl<T: Clone + Send + Sync + 'static> SnapshotAdapter<T> {
    fn snapshot(&self, entry: &mut Entry<'_, Self>) -> Result<Arc<T>, AccessError> {
        if let Some(intent) = entry.intent() {
            return Ok(Arc::clone(intent));
        }
        if let Some(snapshot) = entry.local().first_snapshot.as_ref() {
            return Ok(Arc::clone(snapshot));
        }

        let observed = self
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let snapshot = self.value.load_full();
        if !self.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }

        entry.record_read(SnapshotObservation { version: observed })?;
        entry.local_mut().first_snapshot = Some(Arc::clone(&snapshot));
        Ok(snapshot)
    }

    fn validate_observation(
        &self,
        observation: &SnapshotObservation,
        prepared: &SnapshotPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if !guard.is_for(&self.version) || guard.owner() != cx.owner() {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            self.version.validate_own(observation.version, cx.owner())
        } else {
            self.version.validate(observation.version)
        };

        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

impl<T: Clone + Send + Sync + 'static> TransactionalResource for SnapshotAdapter<T> {
    type Key = ();
    type Local = SnapshotLocal<T>;
    type Observation = SnapshotObservation;
    type Predicate = NoPredicate;
    type Intent = Arc<T>;
    type Prepared = SnapshotPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        Ok(SnapshotLocal {
            first_snapshot: None,
            displaced: None,
        })
    }

    fn preflight(
        &self,
        _key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        let lock_use = if item.intent().is_some() {
            Some(cx.require_lock(LockRequest::new(
                self.lock_identity.clone(),
                Arc::clone(&self.lock),
            ))?)
        } else {
            None
        };
        Ok(SnapshotPrepared { lock_use })
    }

    fn revalidate_read(
        &self,
        _key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        if self.version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
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
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        self.validate_observation(observation, prepared, cx)
    }

    fn install(
        &self,
        _key: &Self::Key,
        mut item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let lock_use = prepared
            .lock_use
            .as_ref()
            .expect("snapshot invariant: write has no planned version lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("snapshot invariant: write has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("snapshot invariant: {error}"));
        if !guard.is_held() || !guard.is_for(&self.version) {
            panic!("snapshot invariant: install received the wrong version guard");
        }
        if commit_id.to_version() <= guard.before() {
            panic!("snapshot invariant: OCC commit ID does not advance the value");
        }
        if item.local_mut().displaced.is_some() {
            panic!("snapshot invariant: item installed more than once");
        }

        match item.observation() {
            ObservationRef::Read(observation) | ObservationRef::UpgradedPredicate(observation) => {
                if observation.version != guard.before() {
                    panic!("snapshot invariant: read observation differs from lock guard");
                }
            }
            ObservationRef::Unobserved => {
                panic!("snapshot invariant: replacement has no covering observation");
            }
            ObservationRef::Predicate(predicate) => match *predicate {},
        }

        let replacement = Arc::clone(item.intent());
        let displaced = self.value.swap(replacement);
        item.local_mut().displaced = Some(displaced);
    }

    fn finish(
        &self,
        _key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        item.local_mut().first_snapshot = None;
        item.local_mut().displaced = None;
        let _ = item.take_remaining_intent();
    }
}
