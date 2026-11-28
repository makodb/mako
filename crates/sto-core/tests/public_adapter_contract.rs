use std::sync::{
    atomic::{AtomicBool, AtomicI64, Ordering},
    Arc,
};

use sto_core::{
    AccessError, AcquireContext, AcquireError, Active, AdapterFault, AdapterFaultKind,
    AdapterPhase, AtomicVersion, CheckError, CleanupContext, CleanupDisposition, CleanupItem,
    Conflict, DetachedVersionGuard, ExecutionCheckContext, InstallContext, InstallItem, LockClass,
    LockContext, LockDisposition, LockIdentity, LockNamespaceId, LockRequest, LockUse, NoPredicate,
    ObservationOrder, ObservationRef, OccVersion, OpacityToken, PredicateContext, PreflightItem,
    PrepareError, RegisteredResource, ReleaseContext, ResourceClass, Runtime, RuntimeConfig,
    TObject, TransProxy, Transaction, TransactionLock, ValidationContext,
};

const FIRST_RESOURCE_CLASS: u32 = 41;
const SECOND_RESOURCE_CLASS: u32 = 42;
const SHARED_LOCK_CLASS: u32 = 73;

struct SharedVersionLock {
    version: Arc<AtomicVersion>,
}

struct SharedVersionGuard {
    inner: DetachedVersionGuard,
}

impl TransactionLock for SharedVersionLock {
    type Guard = SharedVersionGuard;

    fn try_acquire(
        &self,
        _identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        self.version
            .try_acquire_detached(cx.owner())
            .map(|inner| SharedVersionGuard { inner })
    }

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) {
        assert_eq!(guard.inner.owner(), cx.owner());
        assert!(guard.inner.is_for(self.version.as_ref()));
        match disposition {
            LockDisposition::Aborted => guard.inner.release_abort(self.version.as_ref()).unwrap(),
            LockDisposition::Committed {
                occ_commit_id: Some(commit_id),
            } => {
                guard
                    .inner
                    .release_commit(self.version.as_ref(), commit_id)
                    .unwrap();
            }
            LockDisposition::Committed {
                occ_commit_id: None,
            } => panic!("writing contract fixture must receive a commit ID"),
            LockDisposition::Indeterminate { occ_commit_id } => {
                guard
                    .inner
                    .release_indeterminate(self.version.as_ref(), occ_commit_id)
                    .unwrap();
            }
        }
    }
}

#[derive(Clone, Copy)]
struct Observation {
    version: OccVersion,
}

impl OpacityToken for Observation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct Local {
    snapshot: Option<i64>,
    displaced: Option<i64>,
}

struct Prepared {
    lock_use: Option<LockUse<SharedVersionLock>>,
    final_value: Option<i64>,
}

struct ClassAdapter<const TAG: u32> {
    value: Arc<AtomicI64>,
    version: Arc<AtomicVersion>,
    lock: Arc<SharedVersionLock>,
    lock_identity: LockIdentity,
    published_early: AtomicBool,
}

impl<const TAG: u32> ClassAdapter<TAG> {
    fn get(&self, entry: &mut TransProxy<'_, Self>) -> Result<i64, AccessError> {
        if let Some(intent) = entry.intent() {
            return Ok(*intent);
        }
        if let Some(snapshot) = entry.local().snapshot {
            return Ok(snapshot);
        }

        let version = self
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let snapshot = self.value.load(Ordering::Acquire);
        if !self.version.validate(version) {
            return Err(Conflict::ReadValidation.into());
        }
        entry.add_read(Observation { version })?;
        entry.local_mut().snapshot = Some(snapshot);
        Ok(snapshot)
    }

    fn validate(
        &self,
        observation: &Observation,
        prepared: &Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if guard.inner.owner() != cx.owner()
                || !guard.inner.is_for(&self.version)
                || guard.inner.before() != observation.version
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

impl<const TAG: u32> TObject for ClassAdapter<TAG> {
    type Key = ();
    type Local = Local;
    type Observation = Observation;
    type Predicate = NoPredicate;
    type Intent = i64;
    type Prepared = Prepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        Ok(Local {
            snapshot: None,
            displaced: None,
        })
    }

    fn lock(
        &self,
        _key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut LockContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        let lock_use = if item.intent().is_some() {
            Some(cx.try_lock(LockRequest::new(
                self.lock_identity.clone(),
                Arc::clone(&self.lock),
            ))?)
        } else {
            None
        };
        if let Some(use_) = lock_use.as_ref() {
            assert!(cx.guard(use_)?.inner.is_held(), "locking is synchronous");
        }
        Ok(Prepared {
            lock_use,
            final_value: item.intent().copied(),
        })
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
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        self.validate(observation, prepared, cx)
    }

    fn install(
        &self,
        _key: &Self::Key,
        mut item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let commit_id = cx
            .occ_commit_id()
            .expect("writing contract fixture must receive a commit ID");
        let guard = cx
            .guard_mut(
                prepared
                    .lock_use
                    .as_ref()
                    .expect("write must preflight the shared lock"),
            )
            .expect("prepared lock use must resolve");
        assert!(guard.inner.is_held());
        assert!(guard.inner.is_for(&self.version));
        assert!(commit_id.to_version() > guard.inner.before());
        assert!(item.local_mut().displaced.is_none());

        if let ObservationRef::Read(observation) | ObservationRef::UpgradedPredicate(observation) =
            item.observation()
        {
            assert_eq!(observation.version, guard.inner.before());
        }

        assert_eq!(prepared.final_value, Some(*item.intent()));
        let displaced = self
            .value
            .swap(prepared.final_value.unwrap(), Ordering::AcqRel);
        item.local_mut().displaced = Some(displaced);
        let published = cx
            .try_publish_unlock(prepared.lock_use.as_ref().unwrap())
            .unwrap();
        self.published_early.store(published, Ordering::Release);
        assert_eq!(self.version.observe().is_ok(), published);
    }

    fn cleanup(
        &self,
        _key: &Self::Key,
        mut item: CleanupItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: CleanupDisposition,
        _cx: &mut CleanupContext<'_>,
    ) {
        item.local_mut().snapshot = None;
        item.local_mut().displaced = None;
        let _ = item.take_remaining_intent();
    }
}

struct ClassHandle<const TAG: u32> {
    resource: RegisteredResource<ClassAdapter<TAG>>,
}

impl<const TAG: u32> ClassHandle<TAG> {
    fn get(&self, transaction: &mut Transaction<'_, Active>) -> Result<i64, AccessError> {
        let adapter = self.resource.adapter();
        transaction.with_item(&self.resource, (), |entry| adapter.get(entry))
    }

    fn set(
        &self,
        transaction: &mut Transaction<'_, Active>,
        value: i64,
    ) -> Result<(), AccessError> {
        transaction.with_item(&self.resource, (), |entry| entry.add_write(value))
    }

    fn add(
        &self,
        transaction: &mut Transaction<'_, Active>,
        delta: i64,
    ) -> Result<(), AccessError> {
        let adapter = self.resource.adapter();
        transaction.with_item(&self.resource, (), |entry| {
            let current = adapter.get(entry)?;
            entry.add_write(current.wrapping_add(delta))
        })
    }
}

type FirstClass = ClassHandle<1>;
type SecondClass = ClassHandle<2>;

fn register_two_classes(
    runtime: &Arc<Runtime>,
    first: i64,
    second: i64,
) -> (FirstClass, SecondClass) {
    let object = runtime.register_object().unwrap();
    let namespace = LockNamespaceId::new(object.object_id().get()).unwrap();
    let version = Arc::new(AtomicVersion::default());
    let lock = Arc::new(SharedVersionLock {
        version: Arc::clone(&version),
    });
    let lock_identity = LockIdentity::new(
        object.runtime_id(),
        namespace,
        LockClass::new(SHARED_LOCK_CLASS).unwrap(),
        0_u64,
    );

    let first = object
        .register_resource(
            ResourceClass::new(FIRST_RESOURCE_CLASS).unwrap(),
            ClassAdapter::<1> {
                value: Arc::new(AtomicI64::new(first)),
                version: Arc::clone(&version),
                lock: Arc::clone(&lock),
                lock_identity: lock_identity.clone(),
                published_early: AtomicBool::new(false),
            },
        )
        .unwrap();
    let second = object
        .register_resource(
            ResourceClass::new(SECOND_RESOURCE_CLASS).unwrap(),
            ClassAdapter::<2> {
                value: Arc::new(AtomicI64::new(second)),
                version,
                lock,
                lock_identity,
                published_early: AtomicBool::new(false),
            },
        )
        .unwrap();

    assert_eq!(first.object_id(), second.object_id());
    assert_ne!(first.resource_class(), second.resource_class());
    (
        FirstClass { resource: first },
        SecondClass { resource: second },
    )
}

fn read_pair(
    worker: &mut sto_core::WorkerContext,
    first: &FirstClass,
    second: &SecondClass,
) -> (i64, i64) {
    let mut transaction = worker.begin().unwrap();
    let values = (
        first.get(&mut transaction).unwrap(),
        second.get(&mut transaction).unwrap(),
    );
    assert!(matches!(
        transaction.commit().unwrap(),
        sto_core::CommitOutcome::Committed(_)
    ));
    values
}

#[test]
fn downstream_adapter_uses_two_resource_classes_and_one_typed_physical_lock() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(1)).unwrap();
    let (first, second) = register_two_classes(&runtime, 10, 100);
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    first.add(&mut transaction, 5).unwrap();
    first.add(&mut transaction, -2).unwrap();
    second.set(&mut transaction, 200).unwrap();
    second.add(&mut transaction, 7).unwrap();
    assert_eq!(first.get(&mut transaction).unwrap(), 13);
    assert_eq!(second.get(&mut transaction).unwrap(), 207);
    assert!(matches!(
        transaction.commit().unwrap(),
        sto_core::CommitOutcome::Committed(_)
    ));

    assert_eq!(read_pair(&mut worker, &first, &second), (13, 207));

    let mut transaction = worker.begin().unwrap();
    first.set(&mut transaction, -1).unwrap();
    second.set(&mut transaction, -2).unwrap();
    transaction.abort();
    assert_eq!(read_pair(&mut worker, &first, &second), (13, 207));
}

#[cfg(feature = "distributed")]
#[test]
fn participant_refreshes_preparation_and_retains_aliased_guards_until_finish() {
    use sto_core::clock::distributed::{DistributedTerm, DistributedTimeTerm, DistributedTimestamp};
    use sto_core::{CommitOutcome, ParticipantPhase};

    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(1)).unwrap();
    let (first, second) = register_two_classes(&runtime, 10, 100);
    let mut worker = runtime.attach().unwrap();
    let mut participant = worker.begin().unwrap().into_participant();
    participant
        .with_transaction(|txn| first.set(txn, 11))
        .unwrap();
    participant.lock_writes().unwrap();
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                let mut competing = runtime.attach().unwrap();
                let mut reader = competing.begin().unwrap();
                assert_eq!(first.get(&mut reader), Err(Conflict::LockBusy.into()));
                reader.abort();
            })
            .join()
            .unwrap();
    });

    participant
        .with_transaction(|txn| {
            first.set(txn, 12)?;
            second.set(txn, 101)?;
            assert_eq!(first.get(txn)?, 12);
            Ok(())
        })
        .unwrap();
    participant.lock_writes().unwrap();
    participant.validate().unwrap();
    assert_eq!(participant.phase(), ParticipantPhase::Validated);
    assert!(participant
        .with_transaction(|txn| first.set(txn, 99))
        .is_err());
    participant
        .install(DistributedTimeTerm::new(
            DistributedTimestamp::new(73).unwrap(),
            DistributedTerm::new(4).unwrap(),
        ))
        .unwrap();
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                let mut competing = runtime.attach().unwrap();
                let mut reader = competing.begin().unwrap();
                assert_eq!(second.get(&mut reader), Err(Conflict::LockBusy.into()));
                reader.abort();
            })
            .join()
            .unwrap();
    });
    // Even an abort request after installation completes committed cleanup.
    assert!(matches!(
        participant.abort().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(read_pair(&mut worker, &first, &second), (12, 101));
}

#[test]
fn local_commit_visits_authoritative_final_intents_and_publishes_once() {
    use sto_core::{CommitOutcome, LocalCommitPhase};

    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(1)).unwrap();
    let (first, second) = register_two_classes(&runtime, 10, 100);
    let (unrelated, _) = register_two_classes(&runtime, -1, -2);
    let mut worker = runtime.attach().unwrap();
    let mut txn = worker.begin().unwrap();
    assert_eq!(first.get(&mut txn).unwrap(), 10);
    first.set(&mut txn, 11).unwrap();
    first.set(&mut txn, 12).unwrap();

    // The first lookup uses homogeneous batch storage; no adapter-owned set.
    let mut active = Vec::new();
    txn.visit_items(&first.resource, |item| {
        active.push((*item.key, *item.intent.unwrap(), item.local.snapshot));
        assert!(matches!(item.observation, ObservationRef::Read(_)));
    })
    .unwrap();
    assert_eq!(active, vec![((), 12, Some(10))]);
    second.set(&mut txn, 101).unwrap(); // Materializes the heterogeneous frame.
    let mut count = 0;
    txn.visit_items(&first.resource, |_| count += 1).unwrap();
    assert_eq!(count, 1);
    txn.visit_items(&unrelated.resource, |_| panic!("another binding leaked"))
        .unwrap();

    let mut local = txn.into_local_commit();
    assert!(local.visit_sealed(&first.resource, |_| {}).is_err());
    assert!(local.certify().is_err()); // Locks and certification are distinct.
    local.lock_writes().unwrap();
    assert!(first.resource.adapter().version.observe().is_err());
    local.certify().unwrap();
    let mut sealed = Vec::new();
    local
        .visit_sealed(&first.resource.clone(), |item| {
            sealed.push(*item.intent.unwrap());
            assert_eq!(item.local.snapshot, Some(10));
            assert!(matches!(item.observation, ObservationRef::Read(_)));
        })
        .unwrap();
    local
        .visit_sealed(&second.resource, |item| sealed.push(*item.intent.unwrap()))
        .unwrap();
    local
        .visit_sealed(&unrelated.resource, |_| panic!("another binding leaked"))
        .unwrap();
    assert_eq!(sealed, vec![12, 101]);
    local.mark_submitted().unwrap();
    local.mark_committed().unwrap();
    assert!(local.reject().is_err());
    let outcome = local
        .apply(|| {
            // No install callback is permitted before the complete batch succeeds.
            assert_eq!(first.resource.adapter().value.load(Ordering::Acquire), 10);
            assert_eq!(second.resource.adapter().value.load(Ordering::Acquire), 100);
            assert!(first.resource.adapter().version.observe().is_err());
            Ok::<(), std::io::Error>(())
        })
        .unwrap();
    assert!(matches!(outcome, CommitOutcome::Committed(_)));
    assert_eq!(local.phase(), LocalCommitPhase::Applied);
    let version = first.resource.adapter().version.observe().unwrap();
    assert_eq!(local.complete_applied().unwrap(), outcome);
    assert_eq!(first.resource.adapter().version.observe().unwrap(), version);
    drop(local);
    assert_eq!(read_pair(&mut worker, &first, &second), (12, 101));
}

#[test]
fn local_definite_rejection_restores_versions_even_after_unknown() {
    use sto_core::{CommitOutcome, RuntimeHealth};

    for submitted in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
        let (first, second) = register_two_classes(&runtime, 10, 100);
        let before = first.resource.adapter().version.observe().unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        first.set(&mut txn, 11).unwrap();
        let mut local = txn.into_local_commit();
        local.lock_writes().unwrap();
        local.certify().unwrap();
        if submitted {
            local.mark_submitted().unwrap();
            local.mark_unknown().unwrap();
            assert!(local.abort().is_err());
        }
        assert!(matches!(local.reject().unwrap(), CommitOutcome::Aborted(_)));
        drop(local);
        assert_eq!(runtime.health(), RuntimeHealth::Healthy);
        assert_eq!(first.resource.adapter().version.observe().unwrap(), before);
        assert_eq!(read_pair(&mut worker, &first, &second), (10, 100));
    }
}

#[test]
fn local_committed_apply_error_retains_locks_and_cannot_retry_or_abort() {
    use sto_core::{LocalApplyError, LocalCommitPhase, RuntimeHealth};

    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let (first, _) = register_two_classes(&runtime, 10, 100);
    let mut worker = runtime.attach().unwrap();
    let mut txn = worker.begin().unwrap();
    first.set(&mut txn, 11).unwrap();
    let mut local = txn.into_local_commit();
    local.lock_writes().unwrap();
    local.certify().unwrap();
    local.mark_submitted().unwrap();
    local.mark_unknown().unwrap();
    local.mark_committed().unwrap();
    assert!(matches!(
        local.apply(|| Err::<(), _>("ambiguous write")),
        Err(LocalApplyError::Storage("ambiguous write"))
    ));
    assert_eq!(local.phase(), LocalCommitPhase::ApplyFailed);
    assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
    assert!(first.resource.adapter().version.observe().is_err());
    assert_eq!(first.resource.adapter().value.load(Ordering::Acquire), 10);
    assert!(local.abort().is_err());
    assert!(local.reject().is_err());
    assert!(local
        .apply::<()>(|| panic!("must not blindly repeat ambiguous write"))
        .is_err());
    // Represents a recovery-confirmed atomic batch, not a native retry.
    local.complete_applied().unwrap();
    assert_eq!(first.resource.adapter().value.load(Ordering::Acquire), 11);
    assert!(first.resource.adapter().version.observe().is_ok());
    drop(local);
    assert!(worker.begin().is_err()); // Recovery must recreate the fenced runtime.
}

#[test]
fn local_submitted_unknown_and_committed_drop_never_restore_aborted_versions() {
    use sto_core::RuntimeHealth;

    for decision in 0..3 {
        let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
        let (first, _) = register_two_classes(&runtime, 10, 100);
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        first.set(&mut txn, 11).unwrap();
        let mut local = txn.into_local_commit();
        local.lock_writes().unwrap();
        local.certify().unwrap();
        local.mark_submitted().unwrap();
        if decision == 1 {
            local.mark_unknown().unwrap();
        }
        if decision == 2 {
            local.mark_committed().unwrap();
        }
        drop(local);
        assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
        assert!(first.resource.adapter().version.observe().is_err());
        assert_eq!(first.resource.adapter().value.load(Ordering::Acquire), 10);
        assert!(worker.begin().is_err());
    }
}

#[test]
fn local_read_only_completion_uses_ordinary_validation_without_submission() {
    use sto_core::CommitOutcome;

    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let (first, _) = register_two_classes(&runtime, 10, 100);
    let mut worker = runtime.attach().unwrap();
    let mut txn = worker.begin().unwrap();
    assert_eq!(first.get(&mut txn).unwrap(), 10);
    let mut local = txn.into_local_commit();
    local.lock_writes().unwrap();
    local.certify().unwrap();
    assert!(!local.has_writes());
    assert!(matches!(
        local.complete_read_only().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn local_completion_disables_single_item_install_time_unlock() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let (first, _) = register_two_classes(&runtime, 10, 100);
    let mut worker = runtime.attach().unwrap();
    let mut ordinary = worker.begin().unwrap();
    first.set(&mut ordinary, 11).unwrap();
    ordinary.commit().unwrap();
    assert!(first
        .resource
        .adapter()
        .published_early
        .load(Ordering::Acquire));

    let mut txn = worker.begin().unwrap();
    first.set(&mut txn, 12).unwrap();
    let mut local = txn.into_local_commit();
    local.lock_writes().unwrap();
    local.certify().unwrap();
    local.mark_submitted().unwrap();
    local.mark_committed().unwrap();
    local.complete_applied().unwrap();
    assert!(!first
        .resource
        .adapter()
        .published_early
        .load(Ordering::Acquire));
    assert!(first.resource.adapter().version.observe().is_ok());
}

#[test]
fn local_certification_rechecks_reads_after_lock_acquisition() {
    use sto_core::{LocalCommitError, LocalCommitPhase};

    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let (observed, _) = register_two_classes(&runtime, 10, 100);
    let (written, _) = register_two_classes(&runtime, 20, 200);
    let mut worker = runtime.attach().unwrap();
    let mut txn = worker.begin().unwrap();
    assert_eq!(observed.get(&mut txn).unwrap(), 10);
    written.set(&mut txn, 21).unwrap();
    let mut local = txn.into_local_commit();
    local.lock_writes().unwrap();
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                let mut other = runtime.attach().unwrap();
                let mut concurrent = other.begin().unwrap();
                observed.set(&mut concurrent, 11).unwrap();
                concurrent.commit().unwrap();
            })
            .join()
            .unwrap();
    });
    assert!(matches!(
        local.certify(),
        Err(LocalCommitError::Aborted(sto_core::AbortReason::Conflict(
            Conflict::ReadValidation
        )))
    ));
    assert_eq!(local.phase(), LocalCommitPhase::Finished);
    assert_eq!(written.resource.adapter().value.load(Ordering::Acquire), 20);
    assert!(written.resource.adapter().version.observe().is_ok());
}

#[test]
fn local_sealed_read_only_item_is_retained_without_fabricated_intent() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let (first, _) = register_two_classes(&runtime, 10, 100);
    let mut worker = runtime.attach().unwrap();
    let mut txn = worker.begin().unwrap();
    first.get(&mut txn).unwrap();
    let mut local = txn.into_local_commit();
    local.lock_writes().unwrap();
    local.certify().unwrap();
    let mut visits = 0;
    local
        .visit_sealed(&first.resource, |item| {
            visits += 1;
            assert!(item.intent.is_none());
            assert_eq!(item.local.snapshot, Some(10));
            assert!(matches!(item.observation, ObservationRef::Read(_)));
        })
        .unwrap();
    assert_eq!(visits, 1);
    local.complete_read_only().unwrap();
}

#[test]
fn local_sealed_views_can_be_collected_without_copying_core_owned_intents() {
    for heterogeneous in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
        let (first, second) = register_two_classes(&runtime, 10, 100);
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        first.set(&mut txn, 12).unwrap();
        if heterogeneous {
            second.set(&mut txn, 101).unwrap();
        }
        let mut local = txn.into_local_commit();
        local.lock_writes().unwrap();
        local.certify().unwrap();
        {
            let mut borrowed = Vec::new();
            // The temporary binding handle need not outlive the references:
            // every reference is borrowed from the retained owner itself.
            local
                .visit_sealed(&first.resource.clone(), |item| borrowed.push(item))
                .unwrap();
            assert_eq!(borrowed.len(), 1);
            assert_eq!(borrowed[0].intent, Some(&12));
            local
                .visit_sealed(&first.resource, |item| {
                    assert!(std::ptr::eq(item.key, borrowed[0].key));
                    assert!(std::ptr::eq(item.local, borrowed[0].local));
                    assert!(std::ptr::eq(
                        item.intent.unwrap(),
                        borrowed[0].intent.unwrap()
                    ));
                })
                .unwrap();
        }
        local.abort().unwrap();
    }
}
