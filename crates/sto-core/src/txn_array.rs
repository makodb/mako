//! A fixed-length transactional array with independently versioned slots.
//!
//! Each owned `usize` index is both one logical STO key and one canonical
//! physical-lock key. Slot identities never depend on vector storage or other
//! transient addresses. Values are immutable [`Arc`] snapshots published
//! through [`ArcSwap`]; a per-slot [`AtomicVersion`] supplies validation and
//! exclusive commit ownership.

use std::fmt;
use std::sync::Arc;

use arc_swap::ArcSwap;

use crate::adapter::{
    FinishDisposition, FinishItem, InstallItem, NoPredicate, ObservationOrder, ObservationRef,
    OpacityToken, PreflightItem, TransactionalResource,
};
use crate::error::{
    AccessError, AdapterFault, AdapterFaultKind, AdapterPhase, CapacityError, CheckError, Conflict,
    ItemInitError, PrepareError, RegistrationError,
};
use crate::identity::{LockClass, LockIdentity, LockNamespaceId, OccVersion, ResourceClass};
use crate::lock::{
    ExecutionCheckContext, FinishContext, InstallContext, LockRequest, LockUse, PredicateContext,
    PreflightContext, ValidationContext,
};
use crate::runtime::{RegisteredResource, Runtime};
use crate::txn_cell::VersionLock;
use crate::version::AtomicVersion;
use crate::{Active, Entry, Transaction};

const ARRAY_RESOURCE_CLASS_VALUE: u32 = 1;
const ARRAY_LOCK_CLASS_VALUE: u32 = 1;

/// A checked abstract array-bounds outcome.
///
/// Bounds failures are returned inside the transaction-access result. They do
/// not insert an STO item or doom an otherwise healthy transaction.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ArrayBoundsError {
    index: usize,
    len: usize,
}

impl ArrayBoundsError {
    const fn new(index: usize, len: usize) -> Self {
        Self { index, len }
    }

    /// Returns the rejected array index.
    pub const fn index(&self) -> usize {
        self.index
    }

    /// Returns the array length checked by the operation.
    pub const fn array_len(&self) -> usize {
        self.len
    }
}

impl fmt::Display for ArrayBoundsError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "array index {} is out of bounds for length {}",
            self.index, self.len
        )
    }
}

impl std::error::Error for ArrayBoundsError {}

/// A fixed-length, transactionally read and written array.
///
/// Distinct indices have distinct logical items, versions, and physical locks,
/// so transactions touching disjoint slots do not conflict in this adapter.
pub struct TxnArray<T: Clone + Send + Sync + 'static> {
    resource: RegisteredResource<ArrayAdapter<T>>,
}

impl<T: Clone + Send + Sync + 'static> TxnArray<T> {
    /// Registers a new fixed-length array containing `values`.
    pub fn new(
        runtime: &Arc<Runtime>,
        values: impl IntoIterator<Item = T>,
    ) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a LockNamespaceId");
        let resource_class = ResourceClass::new(ARRAY_RESOURCE_CLASS_VALUE)
            .expect("the private TxnArray resource class is nonzero");
        let lock_class = LockClass::new(ARRAY_LOCK_CLASS_VALUE)
            .expect("the private TxnArray lock class is nonzero");

        let mut slots = Vec::new();
        for value in values {
            let index = slots.len();
            let lock_key = u64::try_from(index).map_err(|_| CapacityError::KeyLimit)?;
            slots
                .try_reserve(1)
                .map_err(|_| CapacityError::BufferLimit)?;

            let version = Arc::new(AtomicVersion::default());
            let lock = Arc::new(VersionLock::new(Arc::clone(&version)));
            let lock_identity =
                LockIdentity::new(object.runtime_id(), namespace, lock_class, lock_key);
            slots.push(ArraySlot {
                value: ArcSwap::from_pointee(value),
                version,
                lock,
                lock_identity,
            });
        }

        let adapter = ArrayAdapter {
            slots: slots.into_boxed_slice(),
        };
        let resource = object.register_resource(resource_class, adapter)?;
        Ok(Self { resource })
    }

    /// Returns the fixed number of array slots.
    pub fn len(&self) -> usize {
        self.resource.adapter().slots.len()
    }

    /// Returns whether this array contains no slots.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Reads one transaction-local slot value.
    ///
    /// The inner result represents the abstract bounds outcome. The outer
    /// result is reserved for transaction/runtime failure.
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        index: usize,
    ) -> Result<Result<T, ArrayBoundsError>, AccessError> {
        if index >= self.len() {
            return Ok(Err(ArrayBoundsError::new(index, self.len())));
        }
        let adapter = self.resource.adapter();
        txn.with_item(&self.resource, index, |entry| adapter.get(index, entry))
            .map(Ok)
    }

    /// Replaces one transaction-local slot value.
    ///
    /// The inner result represents the abstract bounds outcome. The outer
    /// result is reserved for transaction/runtime failure.
    pub fn set(
        &self,
        txn: &mut Transaction<'_, Active>,
        index: usize,
        value: T,
    ) -> Result<Result<(), ArrayBoundsError>, AccessError> {
        if index >= self.len() {
            return Ok(Err(ArrayBoundsError::new(index, self.len())));
        }
        let intent = Arc::new(value);
        txn.with_item(&self.resource, index, move |entry| entry.stage(intent))
            .map(Ok)
    }
}

impl<T: Clone + Send + Sync + 'static> Clone for TxnArray<T> {
    fn clone(&self) -> Self {
        Self {
            resource: self.resource.clone(),
        }
    }
}

impl<T: Clone + Send + Sync + 'static> fmt::Debug for TxnArray<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TxnArray")
            .field("runtime_id", &self.resource.runtime_id())
            .field("object_id", &self.resource.object_id())
            .field("len", &self.len())
            .finish_non_exhaustive()
    }
}

struct ArraySlot<T> {
    value: ArcSwap<T>,
    version: Arc<AtomicVersion>,
    lock: Arc<VersionLock>,
    lock_identity: LockIdentity,
}

struct ArrayAdapter<T> {
    slots: Box<[ArraySlot<T>]>,
}

#[derive(Debug)]
struct ArrayLocal<T> {
    first_snapshot: Option<Arc<T>>,
    displaced: Option<Arc<T>>,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
struct ArrayObservation {
    version: OccVersion,
}

impl OpacityToken for ArrayObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct ArrayPrepared {
    lock_use: Option<LockUse<VersionLock>>,
}

impl<T: Clone + Send + Sync + 'static> ArrayAdapter<T> {
    fn get(&self, index: usize, entry: &mut Entry<'_, Self>) -> Result<T, AccessError> {
        if let Some(intent) = entry.intent() {
            return Ok(intent.as_ref().clone());
        }
        if let Some(snapshot) = entry.local().first_snapshot.as_ref() {
            return Ok(snapshot.as_ref().clone());
        }

        let slot = self.slot_for_access(index)?;
        let observed = slot
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let snapshot = slot.value.load_full();
        if !slot.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }

        entry.record_read(ArrayObservation { version: observed })?;
        entry.local_mut().first_snapshot = Some(Arc::clone(&snapshot));
        Ok(snapshot.as_ref().clone())
    }

    fn slot_for_access(&self, index: usize) -> Result<&ArraySlot<T>, AccessError> {
        self.slots.get(index).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Execute,
                AdapterFaultKind::Other("TxnArray item key is out of bounds"),
            )
            .into()
        })
    }

    fn validate_observation(
        &self,
        index: usize,
        observation: &ArrayObservation,
        prepared: &ArrayPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let slot = self.slots.get(index).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Validation,
                AdapterFaultKind::Other("TxnArray item key is out of bounds"),
            )
        })?;
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if !guard.is_for(&slot.version) || guard.owner() != cx.owner() {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            slot.version.validate_own(observation.version, cx.owner())
        } else {
            slot.version.validate(observation.version)
        };

        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

impl<T: Clone + Send + Sync + 'static> TransactionalResource for ArrayAdapter<T> {
    type Key = usize;
    type Local = ArrayLocal<T>;
    type Observation = ArrayObservation;
    type Predicate = NoPredicate;
    type Intent = Arc<T>;
    type Prepared = ArrayPrepared;

    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        if *key >= self.slots.len() {
            return Err(AdapterFault::new(
                AdapterPhase::ItemInit,
                AdapterFaultKind::Other("TxnArray item key is out of bounds"),
            )
            .into());
        }
        Ok(ArrayLocal {
            first_snapshot: None,
            displaced: None,
        })
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        let slot = self.slots.get(*key).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::Other("TxnArray item key is out of bounds"),
            )
        })?;
        let lock_use = if item.intent().is_some() {
            Some(cx.require_lock(LockRequest::new(
                slot.lock_identity.clone(),
                Arc::clone(&slot.lock),
            ))?)
        } else {
            None
        };
        Ok(ArrayPrepared { lock_use })
    }

    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        let slot = self.slots.get(*key).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::ExecutionCheck,
                AdapterFaultKind::Other("TxnArray item key is out of bounds"),
            )
        })?;
        if slot.version.validate(observation.version) {
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
        key: &Self::Key,
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        self.validate_observation(*key, observation, prepared, cx)
    }

    fn install(
        &self,
        key: &Self::Key,
        mut item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let slot = self
            .slots
            .get(*key)
            .expect("sto-core TxnArray invariant: item key is out of bounds during install");
        let lock_use = prepared
            .lock_use
            .as_ref()
            .expect("sto-core TxnArray invariant: write has no planned version lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-core TxnArray invariant: write has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-core TxnArray invariant: {error}"));
        if !guard.is_held() || !guard.is_for(&slot.version) {
            panic!("sto-core TxnArray invariant: install received the wrong version guard");
        }
        if commit_id.to_version() <= guard.before() {
            panic!("sto-core TxnArray invariant: OCC commit ID does not advance the slot");
        }
        if item.local_mut().displaced.is_some() {
            panic!("sto-core TxnArray invariant: item installed more than once");
        }

        match item.observation() {
            ObservationRef::Unobserved => {}
            ObservationRef::Read(observation) | ObservationRef::UpgradedPredicate(observation) => {
                if observation.version != guard.before() {
                    panic!("sto-core TxnArray invariant: read observation differs from lock guard");
                }
            }
            ObservationRef::Predicate(predicate) => match *predicate {},
        }

        let replacement = Arc::clone(item.intent());
        let displaced = slot.value.swap(replacement);
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
    use std::sync::Barrier;
    use std::thread;

    use super::*;
    use crate::error::{AbortReason, CommitOutcome};
    use crate::runtime::RuntimeConfig;

    fn assert_committed(outcome: CommitOutcome) {
        assert!(matches!(outcome, CommitOutcome::Committed(_)));
    }

    fn get_value<T: Clone + Send + Sync + 'static>(
        array: &TxnArray<T>,
        transaction: &mut Transaction<'_, Active>,
        index: usize,
    ) -> T {
        array
            .get(transaction, index)
            .expect("transaction access")
            .expect("in-bounds index")
    }

    fn set_value<T: Clone + Send + Sync + 'static>(
        array: &TxnArray<T>,
        transaction: &mut Transaction<'_, Active>,
        index: usize,
        value: T,
    ) {
        array
            .set(transaction, index, value)
            .expect("transaction access")
            .expect("in-bounds index");
    }

    #[test]
    fn multi_index_commit_and_repeated_item_access_are_transaction_local() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let array = TxnArray::new(&runtime, [10, 20, 30]).unwrap();
        assert_eq!(array.len(), 3);
        assert!(!array.is_empty());

        let mut transaction = worker.begin().unwrap();
        assert_eq!(get_value(&array, &mut transaction, 2), 30);
        set_value(&array, &mut transaction, 2, 31);
        set_value(&array, &mut transaction, 0, 11);
        set_value(&array, &mut transaction, 2, 32);
        assert_eq!(get_value(&array, &mut transaction, 0), 11);
        assert_eq!(get_value(&array, &mut transaction, 1), 20);
        assert_eq!(get_value(&array, &mut transaction, 2), 32);
        assert_committed(transaction.commit().unwrap());

        let mut transaction = worker.begin().unwrap();
        assert_eq!(get_value(&array, &mut transaction, 0), 11);
        assert_eq!(get_value(&array, &mut transaction, 1), 20);
        assert_eq!(get_value(&array, &mut transaction, 2), 32);
        assert_committed(transaction.commit().unwrap());
    }

    #[test]
    fn bounds_are_inner_outcomes_and_do_not_doom_the_transaction() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let array = TxnArray::new(&runtime, [1, 2]).unwrap();
        let mut transaction = worker.begin().unwrap();

        let get_error = array.get(&mut transaction, 2).unwrap().unwrap_err();
        assert_eq!(get_error.index(), 2);
        assert_eq!(get_error.array_len(), 2);
        let set_error = array
            .set(&mut transaction, usize::MAX, 9)
            .unwrap()
            .unwrap_err();
        assert_eq!(set_error.index(), usize::MAX);
        assert_eq!(set_error.array_len(), 2);
        assert!(!transaction.is_doomed());
        assert_committed(transaction.commit().unwrap());

        let empty = TxnArray::<i64>::new(&runtime, []).unwrap();
        assert!(empty.is_empty());
    }

    #[test]
    fn abort_discards_every_staged_slot_replacement() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let array = TxnArray::new(&runtime, [String::from("a"), String::from("b")]).unwrap();

        let mut transaction = worker.begin().unwrap();
        set_value(&array, &mut transaction, 0, String::from("x"));
        set_value(&array, &mut transaction, 1, String::from("y"));
        assert_eq!(*transaction.abort().reason(), AbortReason::Explicit);

        let mut transaction = worker.begin().unwrap();
        assert_eq!(get_value(&array, &mut transaction, 0), "a");
        assert_eq!(get_value(&array, &mut transaction, 1), "b");
        assert_committed(transaction.commit().unwrap());
    }

    #[test]
    fn stale_reader_aborts_after_a_concurrent_slot_publication() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut reader_worker = runtime.attach().unwrap();
        let array = TxnArray::new(&runtime, [5_i64]).unwrap();
        let mut reader = reader_worker.begin().unwrap();
        assert_eq!(get_value(&array, &mut reader, 0), 5);

        let writer_runtime = Arc::clone(&runtime);
        let writer_array = array.clone();
        thread::spawn(move || {
            let mut worker = writer_runtime.attach().unwrap();
            let mut transaction = worker.begin().unwrap();
            set_value(&writer_array, &mut transaction, 0, 6);
            assert_committed(transaction.commit().unwrap());
        })
        .join()
        .unwrap();

        assert!(matches!(
            reader.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        ));
    }

    #[test]
    fn stale_read_then_write_is_a_retryable_conflict_not_a_runtime_fault() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut stale_worker = runtime.attach().unwrap();
        let array = TxnArray::new(&runtime, [5_i64]).unwrap();
        let mut stale = stale_worker.begin().unwrap();
        assert_eq!(get_value(&array, &mut stale, 0), 5);
        set_value(&array, &mut stale, 0, 7);

        let writer_runtime = Arc::clone(&runtime);
        let writer_array = array.clone();
        thread::spawn(move || {
            let mut worker = writer_runtime.attach().unwrap();
            let mut transaction = worker.begin().unwrap();
            set_value(&writer_array, &mut transaction, 0, 6);
            assert_committed(transaction.commit().unwrap());
        })
        .join()
        .unwrap();

        assert!(matches!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        ));
        assert_eq!(runtime.health(), crate::RuntimeHealth::Healthy);
    }

    #[test]
    fn concurrent_disjoint_indices_commit_without_a_coarse_array_lock() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let array = TxnArray::new(&runtime, [0_i64, 0_i64]).unwrap();
        let barrier = Arc::new(Barrier::new(2));
        let mut joins = Vec::new();

        for (index, value) in [(0, 10), (1, 20)] {
            let runtime = Arc::clone(&runtime);
            let array = array.clone();
            let barrier = Arc::clone(&barrier);
            joins.push(thread::spawn(move || {
                let mut worker = runtime.attach().unwrap();
                let mut transaction = worker.begin().unwrap();
                set_value(&array, &mut transaction, index, value);
                barrier.wait();
                transaction.commit().unwrap()
            }));
        }

        for join in joins {
            assert_committed(join.join().unwrap());
        }

        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        assert_eq!(get_value(&array, &mut transaction, 0), 10);
        assert_eq!(get_value(&array, &mut transaction, 1), 20);
        assert_committed(transaction.commit().unwrap());
    }
}
