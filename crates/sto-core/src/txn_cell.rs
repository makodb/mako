//! A generic, single-item transactional cell.
//!
//! `TxnCell` is the correctness-first vertical-slice adapter for native STO.
//! Values are immutable `Arc<T>` snapshots published through `ArcSwap`; the
//! tracked [`AtomicVersion`] supplies OCC validation and exclusive commit
//! ownership. No borrowed payload guard escapes an operation.

use std::fmt;
use std::sync::Arc;

use arc_swap::ArcSwap;

use crate::adapter::{
    FinishDisposition, FinishItem, InstallItem, NoPredicate, ObservationOrder, OpacityToken,
    PreflightItem, TransactionalResource,
};
use crate::error::{
    AccessError, AcquireError, AdapterFault, AdapterFaultKind, AdapterPhase, CheckError, Conflict,
    ItemInitError, PrepareError, RegistrationError,
};
use crate::identity::{LockClass, LockIdentity, LockNamespaceId, OccVersion, ResourceClass};
use crate::lock::{
    AcquireContext, ExecutionCheckContext, FinishContext, InstallContext, LockDisposition,
    LockRequest, LockUse, PredicateContext, PreflightContext, ReleaseContext, TransactionLock,
    ValidationContext,
};
use crate::runtime::{RegisteredResource, Runtime};
use crate::version::{AtomicVersion, DetachedVersionGuard};
use crate::{Active, Entry, Transaction};

const CELL_RESOURCE_CLASS_VALUE: u32 = 1;
const CELL_LOCK_CLASS_VALUE: u32 = 1;
const CELL_LOCK_KEY: u64 = 0;

/// A canonical transaction lock backed by one native [`AtomicVersion`].
///
/// The same `Arc<VersionLock>` must be used for every request naming a given
/// physical version. `TxnCell` constructs and retains that canonical target.
#[derive(Debug)]
pub struct VersionLock {
    version: Arc<AtomicVersion>,
}

impl VersionLock {
    /// Wraps a native atomic version as a transaction-lock target.
    pub fn new(version: Arc<AtomicVersion>) -> Self {
        Self { version }
    }

    /// Returns the version protected by this lock target.
    pub fn version(&self) -> &Arc<AtomicVersion> {
        &self.version
    }
}

impl TransactionLock for VersionLock {
    type Guard = DetachedVersionGuard;

    fn try_acquire(
        &self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        <AtomicVersion as TransactionLock>::try_acquire(self.version.as_ref(), identity, cx)
    }

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) {
        <AtomicVersion as TransactionLock>::release(self.version.as_ref(), guard, disposition, cx);
    }
}

/// A generic transactionally read and written cell.
///
/// Each cell is one private STO object with one logical item and one physical
/// version lock. Cloning this handle preserves that identity and shares the
/// same value.
pub struct TxnCell<T: Clone + Send + Sync + 'static> {
    resource: RegisteredResource<CellAdapter<T>>,
}

impl<T: Clone + Send + Sync + 'static> TxnCell<T> {
    /// Registers a new cell in `runtime` with `initial` as its committed value.
    pub fn new(runtime: &Arc<Runtime>, initial: T) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a LockNamespaceId");
        let resource_class = ResourceClass::new(CELL_RESOURCE_CLASS_VALUE)
            .expect("the private TxnCell resource class is nonzero");
        let lock_class = LockClass::new(CELL_LOCK_CLASS_VALUE)
            .expect("the private TxnCell lock class is nonzero");

        let version = Arc::new(AtomicVersion::default());
        let lock = Arc::new(VersionLock::new(Arc::clone(&version)));
        let lock_identity =
            LockIdentity::new(object.runtime_id(), namespace, lock_class, CELL_LOCK_KEY);
        let adapter = CellAdapter {
            value: ArcSwap::from_pointee(initial),
            version,
            lock,
            lock_identity,
        };
        let resource = object.register_resource(resource_class, adapter)?;
        Ok(Self { resource })
    }

    /// Reads the transaction-local value, recording a point observation on
    /// the first shared read.
    pub fn get(&self, txn: &mut Transaction<'_, Active>) -> Result<T, AccessError> {
        let adapter = self.resource.adapter();
        txn.with_item(&self.resource, (), |entry| adapter.get(entry))
    }

    /// Replaces this transaction's staged value without publishing it.
    pub fn set(&self, txn: &mut Transaction<'_, Active>, value: T) -> Result<(), AccessError> {
        let intent = Arc::new(value);
        txn.with_item(&self.resource, (), move |entry| entry.stage(intent))
    }

    /// Stable STO object identity of this cell.
    pub fn object_id(&self) -> crate::identity::ObjectId {
        self.resource.object_id()
    }
}

impl<T: Clone + Send + Sync + 'static> Clone for TxnCell<T> {
    fn clone(&self) -> Self {
        Self {
            resource: self.resource.clone(),
        }
    }
}

impl<T: Clone + Send + Sync + 'static> fmt::Debug for TxnCell<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TxnCell")
            .field("runtime_id", &self.resource.runtime_id())
            .field("object_id", &self.resource.object_id())
            .finish_non_exhaustive()
    }
}

struct CellAdapter<T: Clone + Send + Sync + 'static> {
    value: ArcSwap<T>,
    version: Arc<AtomicVersion>,
    lock: Arc<VersionLock>,
    lock_identity: LockIdentity,
}

#[derive(Debug)]
struct CellLocal<T> {
    first_snapshot: Option<Arc<T>>,
    displaced: Option<Arc<T>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CellObservation {
    version: OccVersion,
}

impl OpacityToken for CellObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct CellPrepared {
    lock_use: Option<LockUse<VersionLock>>,
}

impl<T: Clone + Send + Sync + 'static> CellAdapter<T> {
    fn get(&self, entry: &mut Entry<'_, Self>) -> Result<T, AccessError> {
        if let Some(intent) = entry.intent() {
            return Ok(intent.as_ref().clone());
        }
        if let Some(snapshot) = entry.local().first_snapshot.as_ref() {
            return Ok(snapshot.as_ref().clone());
        }

        let observed = self
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let snapshot = self.value.load_full();
        if !self.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }

        entry.record_read(CellObservation { version: observed })?;
        entry.local_mut().first_snapshot = Some(Arc::clone(&snapshot));
        Ok(snapshot.as_ref().clone())
    }

    fn validate_observation(
        &self,
        observation: &CellObservation,
        prepared: &CellPrepared,
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

impl<T: Clone + Send + Sync + 'static> TransactionalResource for CellAdapter<T> {
    type Key = ();
    type Local = CellLocal<T>;
    type Observation = CellObservation;
    type Predicate = NoPredicate;
    type Intent = Arc<T>;
    type Prepared = CellPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        Ok(CellLocal {
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
        Ok(CellPrepared { lock_use })
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
            .expect("sto-core TxnCell invariant: write has no planned version lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-core TxnCell invariant: write has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-core TxnCell invariant: {error}"));
        if !guard.is_held() || !guard.is_for(&self.version) {
            panic!("sto-core TxnCell invariant: install received the wrong version guard");
        }
        if commit_id.to_version() <= guard.before() {
            panic!("sto-core TxnCell invariant: OCC commit ID does not advance the cell");
        }
        if item.local_mut().displaced.is_some() {
            panic!("sto-core TxnCell invariant: item installed more than once");
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::adapter::ObservationRef;
    use crate::error::{AbortReason, CommitOutcome};
    use crate::runtime::RuntimeConfig;

    fn assert_send_sync<T: Send + Sync>() {}

    #[test]
    fn cell_handles_are_send_sync_and_clones_preserve_identity() {
        assert_send_sync::<TxnCell<String>>();
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, String::from("initial")).unwrap();
        let clone = cell.clone();

        assert_eq!(cell.object_id(), clone.object_id());
        assert!(cell.resource.is_same_binding(&clone.resource));
        assert!(std::ptr::eq(
            cell.resource.adapter(),
            clone.resource.adapter()
        ));
        assert_eq!(
            cell.resource.adapter().value.load_full().as_ref(),
            "initial"
        );
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn registered_resource_handle_is_one_arc_wide() {
        assert_eq!(
            std::mem::size_of::<RegisteredResource<CellAdapter<u64>>>(),
            8
        );
        assert_eq!(
            std::mem::size_of::<Option<RegisteredResource<CellAdapter<u64>>>>(),
            8
        );
        assert_eq!(std::mem::size_of::<TxnCell<u64>>(), 8);
    }

    #[test]
    fn resource_clones_retain_the_object_lease_until_the_last_handle() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, 1_u64).unwrap();
        let object_id = cell.object_id();
        let clone = cell.clone();

        assert!(runtime.has_registered_object(object_id));
        drop(cell);
        assert!(runtime.has_registered_object(object_id));
        drop(clone);
        assert!(!runtime.has_registered_object(object_id));
    }

    #[test]
    fn distinct_cells_receive_distinct_objects_and_lock_namespaces() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let first = TxnCell::new(&runtime, 1_u64).unwrap();
        let second = TxnCell::new(&runtime, 2_u64).unwrap();

        assert_ne!(first.object_id(), second.object_id());
        assert_ne!(
            first.resource.adapter().lock_identity,
            second.resource.adapter().lock_identity
        );
    }

    #[test]
    fn snapshot_validation_detects_publication() {
        let value = ArcSwap::from_pointee(1_u64);
        let version = Arc::new(AtomicVersion::default());
        let observed = version.observe().unwrap();
        assert_eq!(*value.load_full(), 1);
        assert!(version.validate(observed));
    }

    #[test]
    fn observation_is_ordered_by_the_native_occ_generation() {
        let observation = CellObservation {
            version: OccVersion::new(17).unwrap(),
        };
        assert_eq!(
            observation.observation_order(),
            ObservationOrder::Ordered(OccVersion::new(17).unwrap())
        );
    }

    #[test]
    fn observation_ref_shape_remains_point_read_only() {
        fn classify<T: Clone + Send + Sync + 'static>(
            observation: ObservationRef<'_, CellAdapter<T>>,
        ) -> bool {
            matches!(observation, ObservationRef::Read(_))
        }
        let _ = classify::<u64>;
    }

    #[test]
    fn commit_publishes_and_get_observes_read_your_writes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, String::from("initial")).unwrap();
        let mut worker = runtime.attach().unwrap();

        let mut write = worker.begin().unwrap();
        assert_eq!(cell.get(&mut write).unwrap(), "initial");
        cell.set(&mut write, String::from("replacement")).unwrap();
        assert_eq!(cell.get(&mut write).unwrap(), "replacement");
        let CommitOutcome::Committed(info) = write.commit().unwrap() else {
            panic!("uncontended write must commit");
        };
        assert_eq!(info.occ_commit_id().unwrap().get(), 2);

        let mut read = worker.begin().unwrap();
        assert_eq!(cell.get(&mut read).unwrap(), "replacement");
        let CommitOutcome::Committed(info) = read.commit().unwrap() else {
            panic!("valid read-only transaction must commit");
        };
        assert_eq!(info.occ_commit_id(), None);
    }

    #[test]
    fn explicit_abort_never_changes_the_shared_value() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, 1_u64).unwrap();
        let mut worker = runtime.attach().unwrap();

        let mut write = worker.begin().unwrap();
        cell.set(&mut write, 2).unwrap();
        assert_eq!(cell.get(&mut write).unwrap(), 2);
        assert_eq!(write.abort().reason(), &AbortReason::Explicit);

        let mut read = worker.begin().unwrap();
        assert_eq!(cell.get(&mut read).unwrap(), 1);
        assert!(matches!(
            read.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
    }

    #[test]
    fn a_stale_reader_aborts_after_another_worker_publishes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let cell = TxnCell::new(&runtime, 1_u64).unwrap();
        let mut reader_worker = runtime.attach().unwrap();
        let mut stale = reader_worker.begin().unwrap();
        assert_eq!(cell.get(&mut stale).unwrap(), 1);

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let cell = cell.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut write = writer_worker.begin().unwrap();
                    cell.set(&mut write, 2).unwrap();
                    assert!(matches!(
                        write.commit().unwrap(),
                        CommitOutcome::Committed(_)
                    ));
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut current = reader_worker.begin().unwrap();
        assert_eq!(cell.get(&mut current).unwrap(), 2);
        assert!(matches!(
            current.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
    }
}
