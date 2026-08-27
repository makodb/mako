//! A transactionally composable wrapping `i64` counter.
//!
//! `TxnCounter` is intentionally independent of [`crate::TxnCell`]'s payload
//! representation. The committed value is one [`AtomicI64`], while the shared
//! [`AtomicVersion`] supplies observation validation and canonical exclusive
//! commit ownership. Transaction-local intents are wrapping deltas rather than
//! replacement values.

use std::fmt;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;

use crate::adapter::{
    FinishDisposition, FinishItem, InstallItem, NoPredicate, ObservationOrder, ObservationRef,
    OpacityToken, PreflightItem, TransactionalResource,
};
use crate::error::{
    AccessError, AdapterFault, AdapterFaultKind, AdapterPhase, CheckError, Conflict, ItemInitError,
    PrepareError, RegistrationError,
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

const COUNTER_RESOURCE_CLASS_VALUE: u32 = 1;
const COUNTER_LOCK_CLASS_VALUE: u32 = 1;
const COUNTER_LOCK_KEY: u64 = 0;

/// A transactionally read and incremented signed counter.
///
/// Every addition has explicit two's-complement wrapping semantics. Repeated
/// increments in one transaction compose into one deferred delta, including
/// when their mathematical sum exceeds the `i64` range.
pub struct TxnCounter {
    resource: RegisteredResource<CounterAdapter>,
}

impl TxnCounter {
    /// Registers a new counter in `runtime` with `initial` committed value.
    pub fn new(runtime: &Arc<Runtime>, initial: i64) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a LockNamespaceId");
        let resource_class = ResourceClass::new(COUNTER_RESOURCE_CLASS_VALUE)
            .expect("the private TxnCounter resource class is nonzero");
        let lock_class = LockClass::new(COUNTER_LOCK_CLASS_VALUE)
            .expect("the private TxnCounter lock class is nonzero");

        let version = Arc::new(AtomicVersion::default());
        let lock = Arc::new(VersionLock::new(Arc::clone(&version)));
        let lock_identity =
            LockIdentity::new(object.runtime_id(), namespace, lock_class, COUNTER_LOCK_KEY);
        let adapter = CounterAdapter {
            value: AtomicI64::new(initial),
            version,
            lock,
            lock_identity,
        };
        let resource = object.register_resource(resource_class, adapter)?;
        Ok(Self { resource })
    }

    /// Reads the transaction-local value.
    ///
    /// The first shared read records one stable `(version, base)` snapshot.
    /// Any staged delta is overlaid, including an increment staged before this
    /// call.
    pub fn get(&self, txn: &mut Transaction<'_, Active>) -> Result<i64, AccessError> {
        let adapter = self.resource.adapter();
        txn.with_item(&self.resource, (), |entry| adapter.get(entry))
    }

    /// Stages a wrapping increment without requiring a shared read.
    pub fn increment(
        &self,
        txn: &mut Transaction<'_, Active>,
        delta: i64,
    ) -> Result<(), AccessError> {
        txn.with_item(&self.resource, (), move |entry| {
            let composed = entry.intent().copied().unwrap_or(0).wrapping_add(delta);
            entry.stage(composed)
        })
    }
}

impl Clone for TxnCounter {
    fn clone(&self) -> Self {
        Self {
            resource: self.resource.clone(),
        }
    }
}

impl fmt::Debug for TxnCounter {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TxnCounter")
            .field("runtime_id", &self.resource.runtime_id())
            .field("object_id", &self.resource.object_id())
            .finish_non_exhaustive()
    }
}

struct CounterAdapter {
    value: AtomicI64,
    version: Arc<AtomicVersion>,
    lock: Arc<VersionLock>,
    lock_identity: LockIdentity,
}

#[derive(Debug, Default)]
struct CounterLocal {
    first_base: Option<i64>,
    installed: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CounterObservation {
    version: OccVersion,
}

impl OpacityToken for CounterObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct CounterPrepared {
    lock_use: Option<LockUse<VersionLock>>,
}

impl CounterAdapter {
    fn get(&self, entry: &mut Entry<'_, Self>) -> Result<i64, AccessError> {
        let delta = entry.intent().copied().unwrap_or(0);
        if let Some(base) = entry.local().first_base {
            return Ok(base.wrapping_add(delta));
        }

        let observed = self
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let base = self.value.load(Ordering::Acquire);
        if !self.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }

        entry.record_read(CounterObservation { version: observed })?;
        entry.local_mut().first_base = Some(base);
        Ok(base.wrapping_add(delta))
    }

    fn validate_observation(
        &self,
        observation: &CounterObservation,
        prepared: &CounterPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if !guard.is_for(&self.version)
                || guard.owner() != cx.owner()
                || guard.before() != observation.version
            {
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

impl TransactionalResource for CounterAdapter {
    type Key = ();
    type Local = CounterLocal;
    type Observation = CounterObservation;
    type Predicate = NoPredicate;
    type Intent = i64;
    type Prepared = CounterPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        Ok(CounterLocal::default())
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
        Ok(CounterPrepared { lock_use })
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
            .expect("sto-core TxnCounter invariant: write has no planned version lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-core TxnCounter invariant: write has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-core TxnCounter invariant: {error}"));
        if !guard.is_held() || !guard.is_for(&self.version) {
            panic!("sto-core TxnCounter invariant: install received the wrong version guard");
        }
        if commit_id.to_version() <= guard.before() {
            panic!("sto-core TxnCounter invariant: OCC commit ID does not advance the counter");
        }
        if item.local_mut().installed {
            panic!("sto-core TxnCounter invariant: item installed more than once");
        }

        match item.observation() {
            ObservationRef::Unobserved => {}
            ObservationRef::Read(observation) | ObservationRef::UpgradedPredicate(observation) => {
                if observation.version != guard.before() {
                    panic!(
                        "sto-core TxnCounter invariant: read observation differs from lock guard"
                    );
                }
            }
            ObservationRef::Predicate(predicate) => match *predicate {},
        }

        let delta = *item.intent();
        let expected_base = item.local_mut().first_base;
        let previous = self.value.fetch_add(delta, Ordering::Relaxed);
        item.local_mut().installed = true;
        if expected_base.is_some_and(|expected| expected != previous) {
            panic!("sto-core TxnCounter invariant: installed over an unvalidated base");
        }
    }

    fn finish(
        &self,
        _key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        item.local_mut().first_base = None;
        item.local_mut().installed = false;
        let _ = item.take_remaining_intent();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::{AbortReason, CommitOutcome};
    use crate::runtime::RuntimeConfig;
    use crate::TxnCell;

    fn assert_committed(outcome: CommitOutcome) {
        assert!(matches!(outcome, CommitOutcome::Committed(_)));
    }

    #[test]
    fn increment_before_get_overlays_the_staged_delta_and_abort_is_invisible() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let counter = TxnCounter::new(&runtime, 10).unwrap();

        let mut transaction = worker.begin().unwrap();
        counter.increment(&mut transaction, 5).unwrap();
        assert_eq!(counter.get(&mut transaction).unwrap(), 15);
        assert_eq!(*transaction.abort().reason(), AbortReason::Explicit);

        let mut transaction = worker.begin().unwrap();
        assert_eq!(counter.get(&mut transaction).unwrap(), 10);
        assert_committed(transaction.commit().unwrap());
    }

    #[test]
    fn repeated_increments_compose_with_explicit_wrapping_semantics() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let counter = TxnCounter::new(&runtime, i64::MAX).unwrap();

        let mut transaction = worker.begin().unwrap();
        counter.increment(&mut transaction, 1).unwrap();
        counter.increment(&mut transaction, i64::MAX).unwrap();
        counter.increment(&mut transaction, 2).unwrap();
        let expected = i64::MAX
            .wrapping_add(1)
            .wrapping_add(i64::MAX)
            .wrapping_add(2);
        assert_eq!(counter.get(&mut transaction).unwrap(), expected);
        assert_committed(transaction.commit().unwrap());

        let mut transaction = worker.begin().unwrap();
        assert_eq!(counter.get(&mut transaction).unwrap(), expected);
        assert_committed(transaction.commit().unwrap());
    }

    #[test]
    fn blind_increment_keeps_the_item_unobserved_until_a_get() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let counter = TxnCounter::new(&runtime, 1).unwrap();
        let mut transaction = worker.begin().unwrap();

        counter.increment(&mut transaction, 2).unwrap();
        let unobserved = transaction
            .with_item(&counter.resource, (), |entry| {
                Ok(matches!(entry.observation(), ObservationRef::Unobserved))
            })
            .unwrap();
        assert!(unobserved);
        assert_committed(transaction.commit().unwrap());
    }

    #[test]
    fn counter_and_cell_commit_through_two_erased_adapter_types() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let counter = TxnCounter::new(&runtime, 7).unwrap();
        let cell = TxnCell::new(&runtime, String::from("before")).unwrap();

        let mut transaction = worker.begin().unwrap();
        counter.increment(&mut transaction, -3).unwrap();
        cell.set(&mut transaction, String::from("after")).unwrap();
        assert_committed(transaction.commit().unwrap());

        let mut transaction = worker.begin().unwrap();
        assert_eq!(counter.get(&mut transaction).unwrap(), 4);
        assert_eq!(cell.get(&mut transaction).unwrap(), "after");
        assert_committed(transaction.commit().unwrap());
    }
}
