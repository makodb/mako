//! Native-free checks for opt-in local versions and binding-specific clocks.

use std::sync::{
    atomic::{AtomicU64, AtomicUsize, Ordering},
    Arc,
};

use sto_core::*;

struct Row {
    version: AtomicVersion,
    value: AtomicU64,
    local: bool,
}

impl TransactionLock for Row {
    type Guard = DetachedVersionGuard;

    fn try_acquire(
        &self,
        _: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        if self.local {
            self.version.try_acquire_local_detached(cx.owner())
        } else {
            self.version.try_acquire_detached(cx.owner())
        }
    }

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) {
        assert_eq!(guard.owner(), cx.owner());
        match disposition {
            LockDisposition::Aborted => guard.release_abort(&self.version).unwrap(),
            LockDisposition::Committed { occ_commit_id } => {
                if self.local {
                    guard.release_local_commit(&self.version).unwrap();
                } else {
                    guard
                        .release_commit(&self.version, occ_commit_id.unwrap())
                        .unwrap();
                }
            }
            LockDisposition::Indeterminate { occ_commit_id } => {
                guard
                    .release_indeterminate(
                        &self.version,
                        if self.local { None } else { occ_commit_id },
                    )
                    .unwrap();
            }
        }
    }
}

struct Observation {
    version: OccVersion,
    local: bool,
}

impl OpacityToken for Observation {
    fn observation_order(&self) -> ObservationOrder {
        if self.local {
            ObservationOrder::Unordered
        } else {
            ObservationOrder::Ordered(self.version)
        }
    }
}

struct Adapter {
    row: Arc<Row>,
    identity: LockIdentity,
    requirement_calls: Arc<AtomicUsize>,
}

impl TObject for Adapter {
    type Key = ();
    type Local = Option<u64>;
    type Observation = Observation;
    type Predicate = NoPredicate;
    type Intent = u64;
    type Prepared = Option<LockUse<Row>>;

    fn commit_version_requirement(&self) -> CommitVersionRequirement {
        self.requirement_calls.fetch_add(1, Ordering::Relaxed);
        if self.row.local {
            CommitVersionRequirement::AdapterLocal
        } else {
            CommitVersionRequirement::Ordered
        }
    }

    fn new_local(&self, _: &()) -> Result<Self::Local, ItemInitError> {
        Ok(None)
    }

    fn lock(
        &self,
        _: &(),
        item: PreflightItem<'_, Self>,
        cx: &mut LockContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        if item.intent().is_some() {
            Ok(Some(cx.try_lock(LockRequest::new(
                self.identity.clone(),
                Arc::clone(&self.row),
            ))?))
        } else {
            Ok(None)
        }
    }

    fn revalidate_read(
        &self,
        _: &(),
        observation: &Observation,
        _: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        if self.row.version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }

    fn revalidate_predicate(
        &self,
        _: &(),
        predicate: &NoPredicate,
        _: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        match *predicate {}
    }

    fn check_predicate(
        &self,
        _: &(),
        predicate: &NoPredicate,
        _: &Self::Prepared,
        _: &PredicateContext<'_>,
    ) -> Result<Observation, CheckError> {
        match *predicate {}
    }

    fn check(
        &self,
        _: &(),
        observation: &Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let valid = if let Some(lock) = prepared {
            let guard = cx.guard(lock)?;
            assert!(guard.is_for(&self.row.version));
            self.row
                .version
                .validate_own(observation.version, cx.owner())
        } else {
            self.row.version.validate(observation.version)
        };
        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }

    fn install(
        &self,
        _: &(),
        item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let commit_id = cx.occ_commit_id();
        let guard = cx.guard_mut(prepared.as_ref().unwrap()).unwrap();
        assert!(guard.is_held() && guard.is_for(&self.row.version));
        if self.row.local {
            assert!(guard.before().checked_next().is_some());
        } else {
            assert!(commit_id.unwrap().to_version() > guard.before());
        }
        self.row.value.store(*item.intent(), Ordering::Release);
    }

    fn cleanup(
        &self,
        _: &(),
        _: CleanupItem<'_, Self>,
        _: Option<&mut Self::Prepared>,
        _: CleanupDisposition,
        _: &mut CleanupContext<'_>,
    ) {
    }
}

struct Cell {
    resource: RegisteredResource<Adapter>,
}

impl Cell {
    fn new(runtime: &Arc<Runtime>, local: bool, initial_version: u64) -> Self {
        let object = runtime.register_object().unwrap();
        let identity = LockIdentity::new(
            runtime.id(),
            LockNamespaceId::new(object.object_id().get()).unwrap(),
            LockClass::new(1).unwrap(),
            0_u64,
        );
        let row = Arc::new(Row {
            version: AtomicVersion::new(OccVersion::new(initial_version).unwrap()),
            value: AtomicU64::new(0),
            local,
        });
        let resource = object
            .register_resource(
                ResourceClass::new(1).unwrap(),
                Adapter {
                    row,
                    identity,
                    requirement_calls: Arc::new(AtomicUsize::new(0)),
                },
            )
            .unwrap();
        Self { resource }
    }

    fn set(&self, tx: &mut Transaction<'_, Active>, value: u64) {
        tx.with_item(&self.resource, (), |entry| entry.add_write(value))
            .unwrap();
    }

    fn get(&self, tx: &mut Transaction<'_, Active>) -> u64 {
        tx.with_item(&self.resource, (), |entry| {
            if let Some(value) = entry.intent() {
                return Ok(*value);
            }
            if let Some(value) = *entry.local() {
                return Ok(value);
            }
            let row = &self.resource.adapter().row;
            let version = row
                .version
                .observe()
                .map_err(|_| AccessError::from(Conflict::LockBusy))?;
            let value = row.value.load(Ordering::Acquire);
            if !row.version.validate(version) {
                return Err(Conflict::ReadValidation.into());
            }
            entry.add_read(Observation {
                version,
                local: row.local,
            })?;
            *entry.local_mut() = Some(value);
            Ok(value)
        })
        .unwrap()
    }

    fn version(&self) -> u64 {
        self.resource.adapter().row.version.observe().unwrap().get()
    }
    fn value(&self) -> u64 {
        self.resource.adapter().row.value.load(Ordering::Acquire)
    }
}

fn committed(outcome: Result<CommitOutcome, CommitFailure>) -> Option<OccCommitId> {
    match outcome.unwrap() {
        CommitOutcome::Committed(info) => info.occ_commit_id(),
        other => panic!("expected commit, got {other:?}"),
    }
}

#[test]
fn binding_write_union_survives_mixed_types_and_worker_pool_reuse() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let local = Cell::new(&runtime, true, 200);
    let ordered = Cell::new(&runtime, false, 1);
    let ordinary = TxnCell::new(&runtime, 0).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut tx = worker.begin().unwrap();
    local.set(&mut tx, 1);
    assert_eq!(local.get(&mut tx), 1);
    assert_eq!(committed(tx.commit()), None);
    assert_eq!(local.version(), 201);

    let mut tx = worker.begin().unwrap();
    local.set(&mut tx, 2);
    assert_eq!(ordered.get(&mut tx), 0);
    assert_eq!(committed(tx.commit()), None);

    // These two exact bindings share an adapter type, but not a clock policy.
    let mut tx = worker.begin().unwrap();
    local.set(&mut tx, 3);
    ordered.set(&mut tx, 4);
    assert_eq!(committed(tx.commit()).unwrap().get(), 2);
    assert_eq!(local.version(), 203);
    assert_eq!(ordered.version(), 2);

    // Adding TxnCell materializes the homogeneous batch into erased items.
    let mut tx = worker.begin().unwrap();
    local.set(&mut tx, 5);
    ordinary.set(&mut tx, 6).unwrap();
    assert_eq!(committed(tx.commit()).unwrap().get(), 3);
    assert_eq!(local.version(), 204);

    // A later local-only attempt must not inherit the pooled ordered bit.
    let mut tx = worker.begin().unwrap();
    local.set(&mut tx, 7);
    assert_eq!(committed(tx.commit()), None);
    assert_eq!(local.version(), 205);
    assert_eq!(local.value(), 7);
    assert_eq!(
        local
            .resource
            .adapter()
            .requirement_calls
            .load(Ordering::Relaxed),
        1
    );
    assert_eq!(
        ordered
            .resource
            .adapter()
            .requirement_calls
            .load(Ordering::Relaxed),
        1
    );
}

#[derive(Default)]
struct HookCounts([usize; 3]);

impl CommitHook for HookCounts {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        self.0[0] += 1;
        Ok(())
    }
    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        self.0[1] += 1;
        Ok(())
    }
    fn post_install(&mut self) {
        self.0[2] += 1;
    }
}

#[test]
fn local_writes_without_commit_id_still_run_every_writing_hook() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let local = Cell::new(&runtime, true, 1);
    let mut worker = runtime.attach().unwrap();
    let mut tx = worker.begin().unwrap();
    local.set(&mut tx, 9);
    let mut hook = HookCounts::default();
    assert_eq!(committed(tx.commit_with_hook(&mut hook)), None);
    assert_eq!(hook.0, [1, 1, 1]);
    assert_eq!(local.value(), 9);
    assert_eq!(local.version(), 2);
}

#[test]
fn local_exhaustion_precedes_hooks_and_refuses_later_commit_starts() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let exhausted = Cell::new(&runtime, true, OccVersion::MAX_VALUE);
    let unaffected = Cell::new(&runtime, true, 1);
    let mut first = runtime.attach().unwrap();
    let ready = std::sync::Barrier::new(2);
    let failed = std::sync::Barrier::new(2);
    std::thread::scope(|scope| {
        let late = scope.spawn(|| {
            let mut second = runtime.attach().unwrap();
            let mut already_created = second.begin().unwrap();
            unaffected.set(&mut already_created, 10);
            ready.wait();
            failed.wait();
            assert!(matches!(
                already_created.commit(),
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Aborted(_),
                    ..
                })
            ));
        });
        ready.wait();
        let mut tx = first.begin().unwrap();
        exhausted.set(&mut tx, 11);
        let mut hook = HookCounts::default();
        assert_eq!(
            tx.commit_with_hook(&mut hook).unwrap(),
            CommitOutcome::Aborted(AbortReason::Capacity(CapacityError::VersionExhausted))
        );
        assert_eq!(runtime.health(), RuntimeHealth::Exhausted);
        assert_eq!(exhausted.version(), OccVersion::MAX_VALUE);
        assert_eq!(exhausted.value(), 0);
        assert_eq!(hook.0, [0, 0, 0]);
        failed.wait();
        late.join().unwrap();
    });
    assert_eq!(unaffected.value(), 0);
    assert_eq!(unaffected.version(), 1);
    assert!(matches!(first.begin(), Err(BeginError::Poisoned(_))));
    assert!(matches!(runtime.attach(), Err(AttachError::Poisoned(_))));
}

#[test]
fn local_versions_preserve_final_read_validation() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let local = Cell::new(&runtime, true, 1);
    let other = Cell::new(&runtime, true, 1);
    let mut first = runtime.attach().unwrap();
    let mut stale = first.begin().unwrap();
    assert_eq!(local.get(&mut stale), 0);
    other.set(&mut stale, 8);
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                let mut second = runtime.attach().unwrap();
                let mut writer = second.begin().unwrap();
                local.set(&mut writer, 7);
                assert_eq!(committed(writer.commit()), None);
            })
            .join()
            .unwrap();
    });
    assert_eq!(
        stale.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    assert_eq!(other.value(), 0);
    assert_eq!(other.version(), 1);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn local_acquisition_checks_capacity_without_changing_exhausted_words() {
    let owner = OwnerId::new(1).unwrap();
    let penultimate = OccVersion::new(OccVersion::MAX_VALUE - 1).unwrap();
    for observed in [false, true] {
        let version = AtomicVersion::new(penultimate);
        let mut guard = if observed {
            version
                .try_acquire_local_detached_observed(penultimate, owner)
                .unwrap()
        } else {
            version.try_acquire_local_detached(owner).unwrap()
        };
        guard.release_abort(&version).unwrap();
        assert_eq!(version.observe().unwrap(), penultimate);
        let mut guard = if observed {
            version
                .try_acquire_local_detached_observed(penultimate, owner)
                .unwrap()
        } else {
            version.try_acquire_local_detached(owner).unwrap()
        };
        assert_eq!(
            guard.release_local_commit(&version).unwrap().get(),
            OccVersion::MAX_VALUE
        );
        let maximum = version.observe().unwrap();
        let error = if observed {
            version
                .try_acquire_local_detached_observed(maximum, owner)
                .unwrap_err()
        } else {
            version.try_acquire_local_detached(owner).unwrap_err()
        };
        assert_eq!(
            error,
            AcquireError::Capacity(CapacityError::VersionExhausted)
        );
        assert_eq!(version.observe().unwrap(), maximum);
    }
}

#[test]
fn local_observed_acquisition_does_not_turn_stale_or_busy_words_into_exhaustion() {
    let owner = OwnerId::new(1).unwrap();
    let maximum = OccVersion::new(OccVersion::MAX_VALUE).unwrap();
    let fresh = AtomicVersion::default();
    assert_eq!(
        fresh
            .try_acquire_local_detached_observed(maximum, owner)
            .unwrap_err(),
        AcquireError::Conflict(Conflict::ReadValidation)
    );
    assert_eq!(fresh.observe().unwrap(), OccVersion::INITIAL);
    let max_word = AtomicVersion::new(maximum);
    let mut held = max_word.try_acquire_detached(owner).unwrap();
    assert_eq!(
        max_word
            .try_acquire_local_detached_observed(maximum, OwnerId::new(2).unwrap())
            .unwrap_err(),
        AcquireError::Conflict(Conflict::LockBusy)
    );
    assert_eq!(
        held.release_local_commit(&max_word),
        Err(VersionError::GenerationExhausted(maximum))
    );
    assert!(held.is_held());
    held.release_abort(&max_word).unwrap();
    assert_eq!(max_word.observe().unwrap(), maximum);
}

#[test]
#[allow(
    unsafe_code,
    reason = "stack targets remain unmoved and exclusively held through every checked release"
)]
fn local_stable_release_rejects_wrong_targets_and_repeated_release() {
    let version = AtomicVersion::default();
    let wrong = AtomicVersion::default();
    let mut guard = version
        .try_acquire_local_detached(OwnerId::new(1).unwrap())
        .unwrap();
    // SAFETY: the wrong target is rejected before release; the held target is
    // alive, unmoved, and has not been released or reacquired.
    unsafe {
        assert_eq!(
            guard.release_local_commit_stable_target(&wrong),
            Err(VersionError::GuardTargetMismatch)
        );
        assert!(guard.is_held());
        assert_eq!(wrong.observe().unwrap(), OccVersion::INITIAL);
        assert_eq!(
            guard
                .release_local_commit_stable_target(&version)
                .unwrap()
                .get(),
            2
        );
        assert_eq!(
            guard.release_local_commit_stable_target(&version),
            Err(VersionError::GuardAlreadyReleased)
        );
    }
    assert_eq!(version.observe().unwrap().get(), 2);
}
