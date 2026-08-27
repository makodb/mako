use std::{
    hash::{Hash, Hasher},
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc, Barrier,
    },
    thread,
};

use sto_core::{
    AccessError, Active, CheckError, CommitOutcome, Conflict, Entry, ExecutionCheckContext,
    FinishContext, FinishDisposition, FinishItem, InstallContext, InstallItem, InvalidUse,
    ItemInitError, NoPredicate, ObservationOrder, OpacityToken, PredicateContext, PreflightContext,
    PreflightItem, PrepareError, ResourceClass, Runtime, RuntimeConfig, Transaction,
    TransactionalResource, TxnCell, ValidationContext,
};

#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct CollisionKey(u64);

impl Hash for CollisionKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        0_u8.hash(state);
    }
}

#[derive(Clone, Copy)]
struct UnorderedObservation;

impl OpacityToken for UnorderedObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Unordered
    }
}

struct FixtureAdapter {
    initialized: Arc<AtomicUsize>,
    finished: Arc<AtomicUsize>,
}

impl TransactionalResource for FixtureAdapter {
    type Key = CollisionKey;
    type Local = ();
    type Observation = UnorderedObservation;
    type Predicate = NoPredicate;
    type Intent = ();
    type Prepared = ();

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        self.initialized.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn preflight(
        &self,
        _key: &Self::Key,
        _item: PreflightItem<'_, Self>,
        _cx: &mut PreflightContext<'_>,
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
        _prepared: &mut Self::Prepared,
        _cx: &mut InstallContext<'_>,
    ) {
    }

    fn finish(
        &self,
        _key: &Self::Key,
        _item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        self.finished.fetch_add(1, Ordering::Relaxed);
    }
}

fn fixture_resource(
    runtime: &Arc<Runtime>,
) -> (
    sto_core::RegisteredResource<FixtureAdapter>,
    Arc<AtomicUsize>,
    Arc<AtomicUsize>,
) {
    let initialized = Arc::new(AtomicUsize::new(0));
    let finished = Arc::new(AtomicUsize::new(0));
    let object = runtime.register_object().unwrap();
    let resource = object
        .register_resource(
            ResourceClass::new(77).unwrap(),
            FixtureAdapter {
                initialized: Arc::clone(&initialized),
                finished: Arc::clone(&finished),
            },
        )
        .unwrap();
    (resource, initialized, finished)
}

#[test]
fn cell_commit_abort_drop_and_read_your_writes() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, String::from("old")).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), "old");
    cell.set(&mut transaction, String::from("new")).unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), "new");
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), "new");
    cell.set(&mut transaction, String::from("aborted")).unwrap();
    transaction.abort();

    {
        let mut transaction = worker.begin().unwrap();
        cell.set(&mut transaction, String::from("dropped")).unwrap();
    }

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), "new");
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn one_transaction_commits_across_two_independent_objects() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let first = TxnCell::new(&runtime, 1_u64).unwrap();
    let second = TxnCell::new(&runtime, 10_u64).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    first.set(&mut transaction, 2).unwrap();
    second.set(&mut transaction, 20).unwrap();
    let info = match transaction.commit().unwrap() {
        CommitOutcome::Committed(info) => info,
        CommitOutcome::Aborted(reason) => panic!("unexpected abort: {reason:?}"),
    };
    assert!(info.occ_commit_id().is_some());

    let mut transaction = worker.begin().unwrap();
    assert_eq!(first.get(&mut transaction).unwrap(), 2);
    assert_eq!(second.get(&mut transaction).unwrap(), 20);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn full_key_equality_survives_hash_collisions_and_reuses_items() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    for key in [CollisionKey(1), CollisionKey(2), CollisionKey(1)] {
        transaction
            .with_item(&resource, key, |_entry: &mut Entry<'_, FixtureAdapter>| {
                Ok(())
            })
            .unwrap();
    }
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 2);
}

#[test]
fn caught_access_error_permanently_dooms_the_transaction() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, _, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    let result: Result<(), AccessError> = transaction.with_item(
        &resource,
        CollisionKey(1),
        |_entry: &mut Entry<'_, FixtureAdapter>| Err(InvalidUse::IllegalItemState.into()),
    );
    assert_eq!(
        result,
        Err(AccessError::InvalidUse(InvalidUse::IllegalItemState))
    );
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 1);
}

#[test]
fn wrong_runtime_access_dooms_without_aliasing_the_resource() {
    let first_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let second_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let foreign = TxnCell::new(&second_runtime, 7_u64).unwrap();
    let mut worker = first_runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    assert_eq!(
        foreign.get(&mut transaction),
        Err(AccessError::InvalidUse(InvalidUse::WrongRuntime))
    );
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
}

#[test]
fn body_panic_runs_drop_abort_before_unwinding() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1_u64).unwrap();
    let mut worker = runtime.attach().unwrap();

    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut transaction = worker.begin().unwrap();
        cell.set(&mut transaction, 99).unwrap();
        panic!("injected body panic");
    }));
    assert!(result.is_err());

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 1);
    transaction.commit().unwrap();
}

#[test]
fn two_read_modify_write_transactions_cannot_both_commit() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = Arc::new(TxnCell::new(&runtime, 0_i64).unwrap());
    let barrier = Arc::new(Barrier::new(2));

    let joins: Vec<_> = (0..2)
        .map(|_| {
            let runtime = Arc::clone(&runtime);
            let cell = Arc::clone(&cell);
            let barrier = Arc::clone(&barrier);
            thread::spawn(move || {
                let mut worker = runtime.attach().unwrap();
                let mut transaction = worker.begin().unwrap();
                let value = cell.get(&mut transaction).unwrap();
                cell.set(&mut transaction, value + 1).unwrap();
                barrier.wait();
                transaction.commit().unwrap()
            })
        })
        .collect();

    let outcomes: Vec<_> = joins.into_iter().map(|join| join.join().unwrap()).collect();
    assert_eq!(
        outcomes
            .iter()
            .filter(|outcome| matches!(outcome, CommitOutcome::Committed(_)))
            .count(),
        1
    );
    assert_eq!(
        outcomes
            .iter()
            .filter(|outcome| {
                matches!(
                    outcome,
                    CommitOutcome::Aborted(sto_core::AbortReason::Conflict(
                        Conflict::LockBusy | Conflict::ReadValidation
                    ))
                )
            })
            .count(),
        1
    );

    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 1);
    transaction.commit().unwrap();
}

#[test]
fn configured_item_limit_dooms_access_before_aliasing_an_item() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(1)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    transaction
        .with_item(&resource, CollisionKey(1), |_entry| Ok(()))
        .unwrap();
    assert_eq!(
        transaction.with_item(&resource, CollisionKey(2), |_entry| Ok(())),
        Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
    );
    assert_eq!(initialized.load(Ordering::Relaxed), 1);
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 1);
}

#[test]
fn configured_lock_limit_is_a_definite_preinstall_abort() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(1)).unwrap();
    let array = TxnArrayForTest::new(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    array.set_two(&mut transaction);

    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Capacity(
            sto_core::CapacityError::LockLimit
        ))
    );
    array.assert_unchanged(&mut worker);
}

struct TxnArrayForTest {
    array: sto_core::TxnArray<u64>,
}

impl TxnArrayForTest {
    fn new(runtime: &Arc<Runtime>) -> Self {
        Self {
            array: sto_core::TxnArray::new(runtime, [1, 2]).unwrap(),
        }
    }

    fn set_two(&self, transaction: &mut Transaction<'_, Active>) {
        self.array.set(transaction, 0, 10).unwrap().unwrap();
        self.array.set(transaction, 1, 20).unwrap().unwrap();
    }

    fn assert_unchanged(&self, worker: &mut sto_core::WorkerContext) {
        let mut transaction = worker.begin().unwrap();
        assert_eq!(self.array.get(&mut transaction, 0).unwrap().unwrap(), 1);
        assert_eq!(self.array.get(&mut transaction, 1).unwrap().unwrap(), 2);
        transaction.commit().unwrap();
    }
}

// The HRTB transaction surface accepts this ordinary adapter helper while an
// Entry remains scoped to one call; this function itself is an out-of-crate
// compile fixture for the public trait shape.
fn _external_adapter_operation(
    transaction: &mut Transaction<'_, Active>,
    resource: &sto_core::RegisteredResource<FixtureAdapter>,
) -> Result<(), AccessError> {
    transaction.with_item(resource, CollisionKey(9), |entry| {
        entry.record_read(UnorderedObservation)
    })
}
