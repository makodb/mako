use std::{
    cell::Cell,
    hash::{Hash, Hasher},
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc, Barrier,
    },
    thread,
};

use sto_core::{
    AccessError, Active, CapacityError, CheckError, CommitOutcome, Conflict, Entry,
    ExecutionCheckContext, FinishContext, FinishDisposition, FinishItem, InstallContext,
    InstallItem, InvalidUse, ItemBatchControl, ItemBatchOutcome, ItemInitError, NoPredicate,
    ObservationOrder, ObservationRef, OpacityToken, PredicateContext, PreflightContext,
    PreflightItem, PrepareError, ResourceClass, Runtime, RuntimeConfig, RuntimeHealth, Transaction,
    TransactionalResource, TxnCell, UniqueItemKeyIndex, UniqueItemKeys, ValidationContext,
};

#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct CollisionKey(u64);

thread_local! {
    static COLLISION_HASH_CALLS: Cell<usize> = const { Cell::new(0) };
    static PANIC_ORD_ENABLED: Cell<bool> = const { Cell::new(false) };
}

impl Hash for CollisionKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        COLLISION_HASH_CALLS.with(|calls| calls.set(calls.get() + 1));
        assert_ne!(self.0, u64::MAX, "injected key-hash panic");
        0_u8.hash(state);
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct PanicOrdKey(u64);

impl PartialOrd for PanicOrdKey {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for PanicOrdKey {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        PANIC_ORD_ENABLED.with(|enabled| {
            assert!(
                !enabled.get() || (self.0 != 17 && other.0 != 17),
                "injected key-order panic"
            );
        });
        self.0.cmp(&other.0)
    }
}

impl Hash for PanicOrdKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.0.hash(state);
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
    init_capacity_key: Option<u64>,
}

struct FixtureIntent {
    value: u64,
    drops: Arc<AtomicUsize>,
    panic_on_drop: bool,
}

impl Drop for FixtureIntent {
    fn drop(&mut self) {
        self.drops.fetch_add(1, Ordering::Relaxed);
        assert!(!self.panic_on_drop, "injected intent-drop panic");
    }
}

impl TransactionalResource for FixtureAdapter {
    type Key = CollisionKey;
    type Local = ();
    type Observation = UnorderedObservation;
    type Predicate = NoPredicate;
    type Intent = FixtureIntent;
    type Prepared = ();

    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, ItemInitError> {
        self.initialized.fetch_add(1, Ordering::Relaxed);
        if self.init_capacity_key == Some(key.0) {
            return Err(CapacityError::ItemLimit.into());
        }
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
    fixture_resource_with_init_capacity(runtime, None)
}

fn fixture_resource_with_init_capacity(
    runtime: &Arc<Runtime>,
    init_capacity_key: Option<u64>,
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
                init_capacity_key,
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

    // A fresh generation must not mistake the pooled items from the preceding
    // transaction for live entries, even though every key has the same hash.
    let mut transaction = worker.begin().unwrap();
    for key in [CollisionKey(2), CollisionKey(3), CollisionKey(2)] {
        transaction
            .with_item(&resource, key, |_entry: &mut Entry<'_, FixtureAdapter>| {
                Ok(())
            })
            .unwrap();
    }
    assert_eq!(initialized.load(Ordering::Relaxed), 4);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 4);
}

#[test]
fn consecutive_exact_item_access_bypasses_rehashing() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, _) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    for key in [1, 1, 2, 2, 1, 1] {
        transaction
            .with_item(&resource, CollisionKey(key), |_| Ok(()))
            .unwrap();
    }

    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    COLLISION_HASH_CALLS.with(|calls| {
        assert_eq!(
            calls.get(),
            3,
            "only the first access and exact-cache misses should hash"
        );
    });
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn scalar_typed_append_error_aborts_the_activated_item() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    assert_eq!(
        transaction.with_item(&resource, CollisionKey(1), |_| {
            Err::<(), AccessError>(InvalidUse::IllegalItemState.into())
        }),
        Err(AccessError::InvalidUse(InvalidUse::IllegalItemState))
    );
    assert_eq!(initialized.load(Ordering::Relaxed), 1);
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));
    assert_eq!(finished.load(Ordering::Relaxed), 1);
}

#[test]
fn unique_item_keys_use_exact_equality_without_hashes() {
    let empty: [CollisionKey; 0] = [];
    let empty_proof = UniqueItemKeys::try_new(&empty).expect("an empty batch is unique");
    assert!(empty_proof.is_empty());
    assert_eq!(empty_proof.len(), 0);

    let distinct = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let proof = UniqueItemKeys::try_new(&distinct)
        .expect("different keys remain unique despite identical hashes");
    assert_eq!(proof.as_slice(), &distinct);

    let repeated = [CollisionKey(1), CollisionKey(2), CollisionKey(1)];
    assert!(UniqueItemKeys::try_new(&repeated).is_none());
}

#[test]
fn indexed_unique_item_keys_scale_exactly_and_reuse_scratch() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let distinct: Vec<_> = (0..1_024).rev().map(CollisionKey).collect();
    let mut order = Vec::with_capacity(distinct.len());
    let allocation = order.as_ptr();
    let retained_capacity = order.capacity();

    let proof = UniqueItemKeys::try_new_indexed(&distinct, &mut order)
        .expect("preallocated uniqueness scratch must not fail")
        .expect("every large-batch key is distinct");
    assert_eq!(proof.as_slice(), distinct.as_slice());
    assert_eq!(order, (0..distinct.len()).rev().collect::<Vec<_>>());
    assert_eq!(order.as_ptr(), allocation);
    assert_eq!(order.capacity(), retained_capacity);
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));

    let repeated = [
        CollisionKey(9),
        CollisionKey(3),
        CollisionKey(7),
        CollisionKey(3),
        CollisionKey(1),
    ];
    assert!(UniqueItemKeys::try_new_indexed(&repeated, &mut order)
        .expect("retained uniqueness scratch must not fail")
        .is_none());
    assert!(order
        .windows(2)
        .any(|adjacent| repeated[adjacent[0]] == repeated[adjacent[1]]));

    let distinct_again = [CollisionKey(6), CollisionKey(2), CollisionKey(4)];
    assert!(UniqueItemKeys::try_new_indexed(&distinct_again, &mut order)
        .expect("retained uniqueness scratch must not fail")
        .is_some());
    assert_eq!(order, [1, 2, 0]);
    assert_eq!(order.as_ptr(), allocation);
    assert_eq!(order.capacity(), retained_capacity);
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));
}

#[test]
fn hashed_unique_item_keys_resolve_collisions_and_reuse_generations() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let distinct: Vec<_> = (0..64).rev().map(CollisionKey).collect();
    let mut index = UniqueItemKeyIndex::with_capacity(distinct.len());
    let retained_capacity = index.capacity();

    let proof = UniqueItemKeys::try_new_hashed(&distinct, &mut index)
        .expect("preallocated hash proof must not fail")
        .expect("full equality keeps colliding distinct keys unique");
    assert_eq!(proof.as_slice(), distinct.as_slice());
    assert_eq!(index.capacity(), retained_capacity);
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), distinct.len()));

    let repeated = [
        CollisionKey(9),
        CollisionKey(3),
        CollisionKey(7),
        CollisionKey(3),
        CollisionKey(1),
    ];
    assert!(UniqueItemKeys::try_new_hashed(&repeated, &mut index)
        .expect("retained hash proof must not fail")
        .is_none());

    let panicking = [CollisionKey(1), CollisionKey(u64::MAX)];
    let panic = catch_unwind(AssertUnwindSafe(|| {
        let _ = UniqueItemKeys::try_new_hashed(&panicking, &mut index);
    }));
    assert!(panic.is_err());

    let capacity_before_failure = index.capacity();
    assert!(index.try_reserve_for_len(usize::MAX).is_err());
    assert_eq!(index.capacity(), capacity_before_failure);

    let distinct_again = [CollisionKey(6), CollisionKey(2), CollisionKey(4)];
    let proof = UniqueItemKeys::try_new_hashed(&distinct_again, &mut index)
        .expect("scratch remains reusable after proof and reserve failures")
        .expect("the retry keys are distinct");
    assert_eq!(proof.as_slice(), &distinct_again);
    assert_eq!(index.capacity(), retained_capacity);
}

#[test]
fn indexed_unique_item_key_order_unwind_leaves_scratch_reusable() {
    let keys: Vec<_> = (0..64).rev().map(PanicOrdKey).collect();
    let mut order = Vec::with_capacity(keys.len());
    let allocation = order.as_ptr();

    PANIC_ORD_ENABLED.with(|enabled| enabled.set(true));
    let panic = catch_unwind(AssertUnwindSafe(|| {
        let _ = UniqueItemKeys::try_new_indexed(&keys, &mut order);
    }));
    PANIC_ORD_ENABLED.with(|enabled| enabled.set(false));
    assert!(panic.is_err());

    let proof = UniqueItemKeys::try_new_indexed(&keys, &mut order)
        .expect("retained uniqueness scratch must not fail after an unwind")
        .expect("the retry keys remain distinct");
    assert_eq!(proof.as_slice(), keys.as_slice());
    assert_eq!(order.as_ptr(), allocation);
    assert!(order
        .windows(2)
        .all(|adjacent| keys[adjacent[0]] < keys[adjacent[1]]));
}

#[test]
fn distinct_resource_unique_groups_append_without_key_hashing() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let second_keys = [CollisionKey(1), CollisionKey(2)];

    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();
    transaction
        .with_item_session(&second, |session| {
            assert!(session.can_start_unique_item_batch());
            assert!(session.try_with_unique_item_batch(
                UniqueItemKeys::try_new(&second_keys).unwrap(),
                |_, entry| entry.record_read(UnorderedObservation),
            )?);
            assert!(!session.can_start_unique_item_batch());
            Ok(())
        })
        .unwrap();

    // Equal keys in distinct registered resources are distinct full item
    // identities. Neither uniqueness proof nor either direct append hashes.
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));
    assert_eq!(first_initialized.load(Ordering::Relaxed), 3);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));
    assert_eq!(first_finished.load(Ordering::Relaxed), 3);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
}

#[test]
fn distinct_binding_scalars_extend_the_unindexed_typed_suffix() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let (third, third_initialized, third_finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];

    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();
    transaction
        .with_item(&second, CollisionKey(10), |entry| {
            entry.record_read(UnorderedObservation)
        })
        .unwrap();
    transaction
        .with_item(&second, CollisionKey(10), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();
    transaction
        .with_item(&third, CollisionKey(20), |entry| {
            entry.record_read(UnorderedObservation)
        })
        .unwrap();

    COLLISION_HASH_CALLS.with(|calls| {
        assert_eq!(
            calls.get(),
            0,
            "distinct bindings and exact last-item reuse must remain unindexed"
        );
    });
    assert_eq!(first_initialized.load(Ordering::Relaxed), 3);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 1);
    assert_eq!(third_initialized.load(Ordering::Relaxed), 1);

    // A non-cached access to a binding already in the live suffix may alias an
    // earlier item. It materializes every live slot once, hashes the query, and
    // reuses the original observation rather than initializing a duplicate.
    transaction
        .with_item(&first, CollisionKey(2), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 6));
    assert_eq!(first_initialized.load(Ordering::Relaxed), 3);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 6));
    assert_eq!(first_finished.load(Ordering::Relaxed), 3);
    assert_eq!(second_finished.load(Ordering::Relaxed), 1);
    assert_eq!(third_finished.load(Ordering::Relaxed), 1);
}

#[test]
fn streaming_unique_batch_retains_only_the_stopped_prefix() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [
        CollisionKey(1),
        CollisionKey(2),
        CollisionKey(3),
        CollisionKey(4),
    ];

    let outcome = transaction
        .with_item_session(&resource, |session| {
            session.try_with_unique_item_batch_while(
                UniqueItemKeys::try_new(&keys).unwrap(),
                |index, entry| {
                    entry.record_read(UnorderedObservation)?;
                    Ok(if index == 1 {
                        ItemBatchControl::Stop
                    } else {
                        ItemBatchControl::Continue
                    })
                },
            )
        })
        .unwrap();

    assert_eq!(outcome, ItemBatchOutcome::Stopped { appended: 2 });
    assert_eq!(outcome.appended(), 2);
    assert!(outcome.stopped());
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 2);
}

#[test]
fn streaming_unique_batch_reports_capacity_after_its_successful_prefix() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let operated = AtomicUsize::new(0);

    let result = transaction.with_item_session(&resource, |session| {
        session
            .try_with_unique_item_batch_while(
                UniqueItemKeys::try_new(&keys).unwrap(),
                |_, entry| {
                    entry.record_read(UnorderedObservation)?;
                    operated.fetch_add(1, Ordering::Relaxed);
                    Ok(ItemBatchControl::Continue)
                },
            )
            .map(|_| ())
    });

    assert_eq!(
        result,
        Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
    );
    assert_eq!(operated.load(Ordering::Relaxed), 2);
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 2);
}

#[test]
fn unindexed_scalar_prefix_accepts_unique_suffix_and_materializes_on_lookup() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    transaction
        .with_item(&first, CollisionKey(10), |entry| {
            entry.record_read(UnorderedObservation)
        })
        .unwrap();
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 0));

    let suffix = [CollisionKey(20), CollisionKey(21)];
    transaction
        .with_item_session(&second, |session| {
            assert!(session.can_start_unique_item_batch());
            assert!(session.try_with_unique_item_batch(
                UniqueItemKeys::try_new(&suffix).unwrap(),
                |_, entry| entry.record_read(UnorderedObservation),
            )?);
            Ok(())
        })
        .unwrap();
    COLLISION_HASH_CALLS.with(|calls| {
        assert_eq!(
            calls.get(),
            0,
            "neither the fresh-binding scalar nor proven-unique suffix hashes"
        );
    });

    transaction
        .with_item(&second, CollisionKey(21), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();
    COLLISION_HASH_CALLS.with(|calls| {
        assert_eq!(
            calls.get(),
            4,
            "lookup hashes all three live items and the query exactly once"
        );
    });
    assert_eq!(first_initialized.load(Ordering::Relaxed), 1);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(first_finished.load(Ordering::Relaxed), 1);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
}

#[test]
fn indexed_typed_prefix_materializes_only_the_unique_suffix_into_ordinary_storage() {
    COLLISION_HASH_CALLS.with(|calls| calls.set(0));
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    transaction
        .with_item(&first, CollisionKey(10), |entry| {
            entry.record_read(UnorderedObservation)
        })
        .unwrap();
    let suffix = [CollisionKey(20), CollisionKey(21)];
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&suffix).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();

    assert_eq!(cell.get(&mut transaction).unwrap(), 41);
    COLLISION_HASH_CALLS.with(|calls| {
        assert_eq!(
            calls.get(),
            3,
            "ordinary materialization hashes the two-item suffix but not the indexed prefix"
        );
    });
    transaction
        .with_item(&second, CollisionKey(20), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();
    COLLISION_HASH_CALLS.with(|calls| assert_eq!(calls.get(), 4));
    assert_eq!(first_initialized.load(Ordering::Relaxed), 1);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(first_finished.load(Ordering::Relaxed), 1);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
}

#[test]
fn indexed_prefix_suffix_hash_unwind_aborts_every_item_and_worker_recovers() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let suffix = [CollisionKey(20), CollisionKey(u64::MAX)];

    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut transaction = worker.begin().unwrap();
        transaction
            .with_item(&first, CollisionKey(10), |entry| {
                entry.record_read(UnorderedObservation)
            })
            .unwrap();
        transaction
            .with_unique_item_batch(
                &second,
                UniqueItemKeys::try_new(&suffix).unwrap(),
                |_, entry| entry.record_read(UnorderedObservation),
            )
            .unwrap();

        // Typed-to-ordinary draining completes first. Hashing then extends the
        // existing exact prefix one suffix slot at a time; this panic leaves a
        // longer exact prefix, while Drop still aborts every materialized item.
        let _ = cell.get(&mut transaction);
    }));
    assert!(result.is_err());
    assert_eq!(first_initialized.load(Ordering::Relaxed), 1);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(first_finished.load(Ordering::Relaxed), 1);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);

    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&second, CollisionKey(30), |_| Ok(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(second_initialized.load(Ordering::Relaxed), 3);
    assert_eq!(second_finished.load(Ordering::Relaxed), 3);
}

#[test]
fn direct_unique_group_rejects_a_previously_used_resource_binding() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(1)];
    let repeated_binding_keys = [CollisionKey(2)];
    let cloned_binding = resource.clone();
    let calls = AtomicUsize::new(0);

    transaction
        .with_unique_item_batch(
            &resource,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();
    assert_eq!(
        transaction.with_unique_item_batch(
            &cloned_binding,
            UniqueItemKeys::try_new(&repeated_binding_keys).unwrap(),
            |_, _| {
                calls.fetch_add(1, Ordering::Relaxed);
                Ok(())
            },
        ),
        Err(AccessError::InvalidUse(
            InvalidUse::UniqueBatchRequiresEmptyTransaction
        ))
    );
    assert_eq!(calls.load(Ordering::Relaxed), 0);
    assert_eq!(initialized.load(Ordering::Relaxed), 1);
    assert!(transaction.is_doomed());
    transaction.abort();
    assert_eq!(finished.load(Ordering::Relaxed), 1);
}

#[test]
fn resolved_session_falls_back_after_same_resource_unique_group() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(1)];

    transaction
        .with_unique_item_batch(
            &resource,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();

    transaction
        .with_item_session(&resource, |session| {
            assert!(!session.can_start_unique_item_batch());
            let second_keys = [CollisionKey(2)];
            let calls = AtomicUsize::new(0);
            assert!(!session.try_with_unique_item_batch(
                UniqueItemKeys::try_new(&second_keys).unwrap(),
                |_, _| {
                    calls.fetch_add(1, Ordering::Relaxed);
                    Ok(())
                },
            )?);
            assert_eq!(calls.load(Ordering::Relaxed), 0);

            // Scalar fallback builds the exact typed index, initializes the
            // new identity, and can still find the earlier batched identity.
            session.with_resolved_item(
                || Ok(Some((CollisionKey(2), ()))),
                || panic!("a resolved key must not invoke create"),
                |entry, ()| entry.record_read(UnorderedObservation),
            )?;
            session.with_resolved_item(
                || Ok(Some((CollisionKey(1), ()))),
                || panic!("a resolved key must not invoke create"),
                |entry, ()| {
                    assert!(matches!(entry.observation(), ObservationRef::Read(_)));
                    Ok(())
                },
            )
        })
        .unwrap();

    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 2);
}

#[test]
fn distinct_resource_unique_groups_check_total_capacity_before_initialization() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(3)).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(1), CollisionKey(2)];
    let second_keys = [CollisionKey(3), CollisionKey(4)];
    let second_calls = AtomicUsize::new(0);

    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert_eq!(
        transaction.with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&second_keys).unwrap(),
            |_, _| {
                second_calls.fetch_add(1, Ordering::Relaxed);
                Ok(())
            },
        ),
        Err(AccessError::Capacity(CapacityError::ItemLimit))
    );
    assert_eq!(second_calls.load(Ordering::Relaxed), 0);
    assert_eq!(first_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 0);
    assert!(transaction.is_doomed());
    transaction.abort();
    assert_eq!(first_finished.load(Ordering::Relaxed), 2);
    assert_eq!(second_finished.load(Ordering::Relaxed), 0);
}

#[test]
fn second_resource_group_init_failure_aborts_prefix_and_pool_recovers() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) =
        fixture_resource_with_init_capacity(&runtime, Some(22));
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(10), CollisionKey(11)];
    let failing_second_keys = [CollisionKey(20), CollisionKey(22), CollisionKey(21)];
    let second_calls = AtomicUsize::new(0);
    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert_eq!(
        transaction.with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&failing_second_keys).unwrap(),
            |_, _| {
                second_calls.fetch_add(1, Ordering::Relaxed);
                Ok(())
            },
        ),
        Err(AccessError::Capacity(CapacityError::ItemLimit))
    );
    assert_eq!(second_calls.load(Ordering::Relaxed), 1);
    assert!(transaction.is_doomed());
    transaction.abort();
    assert_eq!(first_finished.load(Ordering::Relaxed), 2);
    assert_eq!(second_finished.load(Ordering::Relaxed), 1);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);

    // Reverse the resource-group order. This rebinds every previously active
    // pooled slot, including the boundary after the partially initialized
    // second group, and then extends the old high-water mark again.
    let mut transaction = worker.begin().unwrap();
    let fresh_second_keys = [CollisionKey(40), CollisionKey(41)];
    let fresh_first_keys = [CollisionKey(30), CollisionKey(31)];
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&fresh_second_keys).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&fresh_first_keys).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    assert_eq!(first_initialized.load(Ordering::Relaxed), 4);
    assert_eq!(first_finished.load(Ordering::Relaxed), 4);
    // `new_local` is observable even for the one rejected key.
    assert_eq!(second_initialized.load(Ordering::Relaxed), 4);
    assert_eq!(second_finished.load(Ordering::Relaxed), 3);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn second_resource_group_unwind_aborts_all_active_items_and_worker_recovers() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let first_keys = [CollisionKey(1), CollisionKey(2)];
    let second_keys = [CollisionKey(3), CollisionKey(4)];

    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut transaction = worker.begin().unwrap();
        transaction
            .with_unique_item_batch(
                &first,
                UniqueItemKeys::try_new(&first_keys).unwrap(),
                |_, _| Ok(()),
            )
            .unwrap();
        transaction
            .with_unique_item_batch(
                &second,
                UniqueItemKeys::try_new(&second_keys).unwrap(),
                |index, _| {
                    assert_ne!(index, 1, "injected second-group operation panic");
                    Ok(())
                },
            )
            .unwrap();
    }));
    assert!(result.is_err());
    assert_eq!(first_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(first_finished.load(Ordering::Relaxed), 2);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);

    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&second, CollisionKey(5), |_| Ok(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(second_initialized.load(Ordering::Relaxed), 3);
    assert_eq!(second_finished.load(Ordering::Relaxed), 3);
}

#[test]
fn indexed_prefix_unique_suffix_operation_error_aborts_every_activated_item() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let second_keys = [CollisionKey(2), CollisionKey(3)];

    transaction
        .with_item(&first, CollisionKey(1), |_| Ok(()))
        .unwrap();
    assert_eq!(
        transaction.with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&second_keys).unwrap(),
            |index, _| {
                if index == 1 {
                    return Err(InvalidUse::IllegalItemState.into());
                }
                Ok(())
            },
        ),
        Err(AccessError::InvalidUse(InvalidUse::IllegalItemState))
    );
    assert_eq!(first_initialized.load(Ordering::Relaxed), 1);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(first_finished.load(Ordering::Relaxed), 1);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn multiple_unique_groups_survive_scalar_and_mixed_adapter_materialization() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let intent_drops = Arc::new(AtomicUsize::new(0));
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let first_keys = [CollisionKey(1), CollisionKey(2)];
    let second_keys = [CollisionKey(1), CollisionKey(3)];

    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&first_keys).unwrap(),
            |index, entry| {
                entry.record_read(UnorderedObservation)?;
                if index == 1 {
                    entry.stage(FixtureIntent {
                        value: 0xfeed,
                        drops: Arc::clone(&intent_drops),
                        panic_on_drop: false,
                    })?;
                }
                Ok(())
            },
        )
        .unwrap();
    transaction
        .with_unique_item_batch(
            &second,
            UniqueItemKeys::try_new(&second_keys).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();

    // A scalar access first builds the typed index. Equal key 1 must resolve
    // against the second resource rather than alias the first resource.
    transaction
        .with_item(&second, CollisionKey(1), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();
    transaction
        .with_item(&first, CollisionKey(2), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            assert_eq!(entry.intent().map(|intent| intent.value), Some(0xfeed));
            Ok(())
        })
        .unwrap();
    transaction
        .with_item(&first, CollisionKey(4), |entry| {
            entry.record_read(UnorderedObservation)
        })
        .unwrap();

    // A different adapter drains the complete multi-resource typed prefix.
    // Ordinary exact lookup must still find both old groups afterwards.
    assert_eq!(cell.get(&mut transaction).unwrap(), 41);
    transaction
        .with_item(&second, CollisionKey(3), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();

    assert_eq!(first_initialized.load(Ordering::Relaxed), 3);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 2);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(first_finished.load(Ordering::Relaxed), 3);
    assert_eq!(second_finished.load(Ordering::Relaxed), 2);
    assert_eq!(intent_drops.load(Ordering::Relaxed), 1);
}

#[test]
fn unique_batch_lazily_materializes_an_exact_index_for_later_access() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let intent_drops = Arc::new(AtomicUsize::new(0));
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();

    transaction
        .with_unique_item_batch(&resource, unique, |index, entry| {
            assert!(matches!(entry.observation(), ObservationRef::Unobserved));
            entry.record_read(UnorderedObservation)?;
            if index == 1 {
                // The optimized append lane deliberately retains the complete
                // Entry surface rather than imposing read-only semantics.
                entry.stage(FixtureIntent {
                    value: 0xfeed_beef,
                    drops: Arc::clone(&intent_drops),
                    panic_on_drop: false,
                })?;
            }
            Ok(())
        })
        .unwrap();
    assert_eq!(initialized.load(Ordering::Relaxed), 3);

    // A different adapter type forces the typed prefix into the ordinary
    // representation and builds its exact index before adding the cell item.
    assert_eq!(cell.get(&mut transaction).unwrap(), 41);

    // The materialized index must find key two rather than initialize an alias.
    transaction
        .with_item(&resource, CollisionKey(2), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            assert_eq!(entry.intent().map(|intent| intent.value), Some(0xfeed_beef));
            Ok(())
        })
        .unwrap();
    // Later insertion and reuse use the now-complete ordinary index.
    transaction
        .with_item(&resource, CollisionKey(4), |_| Ok(()))
        .unwrap();
    transaction
        .with_item(&resource, CollisionKey(1), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();
    assert_eq!(initialized.load(Ordering::Relaxed), 4);

    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 4);
    assert_eq!(intent_drops.load(Ordering::Relaxed), 1);
}

#[test]
fn typed_batch_intent_drop_panic_is_contained_before_pool_reuse() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, _, finished) = fixture_resource(&runtime);
    let drops = Arc::new(AtomicUsize::new(0));
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1)];
    transaction
        .with_unique_item_batch(
            &resource,
            UniqueItemKeys::try_new(&keys).unwrap(),
            |_, entry| {
                entry.stage(FixtureIntent {
                    value: 7,
                    drops: Arc::clone(&drops),
                    panic_on_drop: true,
                })
            },
        )
        .unwrap();

    transaction.abort();
    assert_eq!(drops.load(Ordering::Relaxed), 1);
    assert_eq!(finished.load(Ordering::Relaxed), 1);
    assert_eq!(runtime.health(), RuntimeHealth::Poisoned);
    drop(worker);
    assert_eq!(drops.load(Ordering::Relaxed), 1);
}

#[test]
fn typed_batch_intent_sidecar_reuses_three_one_three_high_water() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let intent_drops = Arc::new(AtomicUsize::new(0));
    let mut worker = runtime.attach().unwrap();
    let first = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let second = [CollisionKey(4)];
    let third = [CollisionKey(5), CollisionKey(6), CollisionKey(7)];
    let attempts: [&[CollisionKey]; 3] = [&first, &second, &third];

    for (attempt, keys) in attempts.into_iter().enumerate() {
        let mut transaction = worker.begin().unwrap();
        transaction
            .with_unique_item_batch(
                &resource,
                UniqueItemKeys::try_new(keys).unwrap(),
                |index, entry| {
                    entry.stage(FixtureIntent {
                        value: ((attempt as u64) << 32) | index as u64,
                        drops: Arc::clone(&intent_drops),
                        panic_on_drop: false,
                    })
                },
            )
            .unwrap();

        if attempt == 1 {
            transaction.abort();
        } else {
            assert!(matches!(
                transaction.commit().unwrap(),
                CommitOutcome::Committed(_)
            ));
        }
        assert_eq!(intent_drops.load(Ordering::Relaxed), [3, 4, 7][attempt]);
    }

    assert_eq!(initialized.load(Ordering::Relaxed), 7);
    assert_eq!(finished.load(Ordering::Relaxed), 7);
    assert_eq!(intent_drops.load(Ordering::Relaxed), 7);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn same_binding_typed_pool_recovers_after_item_init_capacity_failure() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource_with_init_capacity(&runtime, Some(2));
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    let first = [CollisionKey(1)];
    transaction
        .with_unique_item_batch(
            &resource,
            UniqueItemKeys::try_new(&first).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Slot zero now retains this exact resource binding. A recoverable failure
    // in `new_local` must not activate a half-initialized item or poison core.
    let mut transaction = worker.begin().unwrap();
    let failing = [CollisionKey(2)];
    assert_eq!(
        transaction.with_unique_item_batch(
            &resource,
            UniqueItemKeys::try_new(&failing).unwrap(),
            |_, _| Ok(()),
        ),
        Err(AccessError::Capacity(CapacityError::ItemLimit))
    );
    assert!(transaction.is_doomed());
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
    transaction.abort();

    // The same worker, typed slot, and retained binding remain reusable.
    let mut transaction = worker.begin().unwrap();
    let fresh = [CollisionKey(3)];
    transaction
        .with_unique_item_batch(
            &resource,
            UniqueItemKeys::try_new(&fresh).unwrap(),
            |_, _| Ok(()),
        )
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    assert_eq!(initialized.load(Ordering::Relaxed), 3);
    assert_eq!(finished.load(Ordering::Relaxed), 2);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn unique_batch_materializes_before_mixed_resource_and_adapter_access() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2)];

    transaction
        .with_unique_item_batch(
            &first,
            UniqueItemKeys::try_new(&keys).unwrap(),
            |_, entry| entry.record_read(UnorderedObservation),
        )
        .unwrap();

    // A same-key, same-adapter-type item from another registered resource is
    // distinct, and a different adapter type may follow it in the same
    // ordinary frame after the one-time typed-batch materialization.
    transaction
        .with_item(&second, CollisionKey(1), |entry| {
            entry.record_read(UnorderedObservation)
        })
        .unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 41);
    transaction
        .with_item(&first, CollisionKey(2), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            Ok(())
        })
        .unwrap();

    assert_eq!(first_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 1);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(first_finished.load(Ordering::Relaxed), 2);
    assert_eq!(second_finished.load(Ordering::Relaxed), 1);
}

#[test]
fn unique_batch_hash_unwind_after_materialization_aborts_complete_prefix() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();
    let keys = [CollisionKey(u64::MAX), CollisionKey(2)];

    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut transaction = worker.begin().unwrap();
        transaction
            .with_unique_item_batch(
                &resource,
                UniqueItemKeys::try_new(&keys).unwrap(),
                |_, entry| entry.record_read(UnorderedObservation),
            )
            .unwrap();

        // Materialization completes before hashing the batched identities.
        // The injected hash unwind must therefore leave one coherent ordinary
        // prefix for Transaction::drop to abort in reverse.
        let _ = cell.get(&mut transaction);
    }));
    assert!(result.is_err());
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert_eq!(finished.load(Ordering::Relaxed), 2);

    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&resource, CollisionKey(4), |_| Ok(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(initialized.load(Ordering::Relaxed), 3);
    assert_eq!(finished.load(Ordering::Relaxed), 3);
}

#[test]
fn shorter_typed_batch_after_long_disposed_prefix_materializes_coherently() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();

    // Seed a four-item ordinary pool by forcing the first homogeneous prefix
    // through a different adapter type before commit.
    let mut transaction = worker.begin().unwrap();
    for key in 1..=4 {
        transaction
            .with_item(&resource, CollisionKey(key), |_| Ok(()))
            .unwrap();
    }
    assert_eq!(cell.get(&mut transaction).unwrap(), 41);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // A long homogeneous transaction disposes the complete ordinary prefix.
    let mut transaction = worker.begin().unwrap();
    for key in 10..=13 {
        transaction
            .with_item(&resource, CollisionKey(key), |_| Ok(()))
            .unwrap();
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Reusing only one typed slot leaves the disposed-prefix watermark longer
    // than this transaction's live prefix.
    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&resource, CollisionKey(20), |_| Ok(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Materializing another one-item prefix must not rescan or index the
    // already-disposed ordinary tail, and ordinary lookup must still reuse the
    // newly materialized item exactly.
    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&resource, CollisionKey(30), |_| Ok(()))
        .unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 41);
    transaction
        .with_item(&resource, CollisionKey(30), |_| Ok(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    assert_eq!(initialized.load(Ordering::Relaxed), 10);
    assert_eq!(finished.load(Ordering::Relaxed), 10);
}

#[test]
fn unique_batch_requires_an_empty_transaction_and_dooms_misuse() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&resource, CollisionKey(1), |_| Ok(()))
        .unwrap();
    let keys = [CollisionKey(2)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();
    let called = AtomicUsize::new(0);

    assert_eq!(
        transaction.with_unique_item_batch(&resource, unique, |_, _| {
            called.fetch_add(1, Ordering::Relaxed);
            Ok(())
        }),
        Err(AccessError::InvalidUse(
            InvalidUse::UniqueBatchRequiresEmptyTransaction
        ))
    );
    assert_eq!(called.load(Ordering::Relaxed), 0);
    assert_eq!(initialized.load(Ordering::Relaxed), 1);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 1);
}

#[test]
fn unique_batch_checks_whole_capacity_before_initializing_an_item() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();

    assert_eq!(
        transaction.with_unique_item_batch(&resource, unique, |_, _| Ok(())),
        Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
    );
    assert_eq!(initialized.load(Ordering::Relaxed), 0);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 0);
}

#[test]
fn unique_batch_error_aborts_only_the_initialized_prefix() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();

    assert_eq!(
        transaction.with_unique_item_batch(&resource, unique, |index, _| {
            if index == 1 {
                Err(InvalidUse::IllegalItemState.into())
            } else {
                Ok(())
            }
        }),
        Err(AccessError::InvalidUse(InvalidUse::IllegalItemState))
    );
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 2);
}

#[test]
fn unique_batch_unwind_runs_drop_abort_and_leaves_the_worker_reusable() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];

    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut transaction = worker.begin().unwrap();
        let unique = UniqueItemKeys::try_new(&keys).unwrap();
        let _ = transaction.with_unique_item_batch(&resource, unique, |index, _| {
            assert_ne!(index, 1, "injected unique-batch operation panic");
            Ok(())
        });
    }));
    assert!(result.is_err());
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert_eq!(finished.load(Ordering::Relaxed), 2);

    let mut transaction = worker.begin().unwrap();
    transaction
        .with_item(&resource, CollisionKey(4), |_| Ok(()))
        .unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(initialized.load(Ordering::Relaxed), 3);
    assert_eq!(finished.load(Ordering::Relaxed), 3);
}

#[test]
fn unique_batch_replaces_a_different_typed_pooled_slot() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 17_u64).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 17);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(9)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();
    assert!(transaction
        .with_unique_item_batch(&resource, unique, |_, entry| {
            entry.record_read(UnorderedObservation)
        })
        .is_ok());
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(initialized.load(Ordering::Relaxed), 1);
    assert_eq!(finished.load(Ordering::Relaxed), 1);
}

#[test]
fn pooled_slots_rebind_across_mixed_types_resources_and_colliding_keys() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (first, first_initialized, first_finished) = fixture_resource(&runtime);
    let (second, second_initialized, second_finished) = fixture_resource(&runtime);
    let cell = TxnCell::new(&runtime, 41_u64).unwrap();
    let mut worker = runtime.attach().unwrap();

    // Establish two same-type pooled slots carrying the first binding.
    let mut transaction = worker.begin().unwrap();
    for key in [CollisionKey(1), CollisionKey(2), CollisionKey(1)] {
        transaction.with_item(&first, key, |_| Ok(())).unwrap();
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Slot zero changes concrete adapter type, while slot one keeps its
    // concrete type but changes resource binding. Repeated access must still
    // find the one live item rather than initialize a hash-colliding alias.
    let mut transaction = worker.begin().unwrap();
    assert_eq!(cell.get(&mut transaction).unwrap(), 41);
    for key in [CollisionKey(3), CollisionKey(3)] {
        transaction.with_item(&second, key, |_| Ok(())).unwrap();
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    // Reverse the mixed-type replacement and exercise two distinct keys that
    // deliberately share the same identity hash in the rebound resource.
    let mut transaction = worker.begin().unwrap();
    for key in [CollisionKey(4), CollisionKey(5), CollisionKey(4)] {
        transaction.with_item(&second, key, |_| Ok(())).unwrap();
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    assert_eq!(first_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(first_finished.load(Ordering::Relaxed), 2);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 3);
    assert_eq!(second_finished.load(Ordering::Relaxed), 3);
}

#[test]
fn resolved_item_session_reuses_items_and_resolver_contexts() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let creates = AtomicUsize::new(0);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    let contexts = transaction
        .with_item_session(&resource, |session| {
            assert!(session.can_start_unique_item_batch());
            let created_context = session.with_resolved_item(
                || Ok(None),
                || {
                    creates.fetch_add(1, Ordering::Relaxed);
                    Ok((CollisionKey(1), 11_u8))
                },
                |entry, context| {
                    assert!(matches!(entry.observation(), ObservationRef::Unobserved));
                    entry.record_read(UnorderedObservation)?;
                    Ok(context)
                },
            )?;
            assert!(!session.can_start_unique_item_batch());
            let reused_context = session.with_resolved_item(
                || Ok(Some((CollisionKey(1), 22_u8))),
                || panic!("an existing identity must not invoke create"),
                |entry, context| {
                    assert!(matches!(entry.observation(), ObservationRef::Read(_)));
                    Ok(context)
                },
            )?;
            let second_context = session.with_resolved_item(
                || Ok(Some((CollisionKey(2), 33_u8))),
                || panic!("a resolved identity must not invoke create"),
                |_entry, context| Ok(context),
            )?;
            let at_capacity_context = session.with_resolved_item(
                || Ok(Some((CollisionKey(1), 44_u8))),
                || panic!("an existing identity must not invoke create"),
                |_entry, context| Ok(context),
            )?;
            Ok((
                created_context,
                reused_context,
                second_context,
                at_capacity_context,
            ))
        })
        .unwrap();

    assert_eq!(contexts, (11, 22, 33, 44));
    assert_eq!(creates.load(Ordering::Relaxed), 1);
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 2);
}

#[test]
fn resolved_session_unique_batch_can_fall_back_without_dooming() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();

    transaction
        .with_item_session(&resource, |session| {
            assert!(session.can_start_unique_item_batch());
            assert!(session.try_with_unique_item_batch(unique, |_, entry| {
                entry.record_read(UnorderedObservation)
            })?);
            assert!(!session.can_start_unique_item_batch());

            let ineligible_keys = [CollisionKey(4)];
            let ineligible = UniqueItemKeys::try_new(&ineligible_keys).unwrap();
            let calls = AtomicUsize::new(0);
            assert!(!session.try_with_unique_item_batch(ineligible, |_, _| {
                calls.fetch_add(1, Ordering::Relaxed);
                Ok(())
            })?);
            assert_eq!(calls.load(Ordering::Relaxed), 0);

            // Ineligibility is not a failure. The scalar session path now
            // builds the exact typed index and reuses a batched item.
            session.with_resolved_item(
                || Ok(Some((CollisionKey(2), ()))),
                || panic!("a resolved identity must not invoke create"),
                |entry, ()| {
                    assert!(matches!(entry.observation(), ObservationRef::Read(_)));
                    Ok(())
                },
            )
        })
        .unwrap();

    assert!(!transaction.is_doomed());
    assert_eq!(initialized.load(Ordering::Relaxed), 3);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(finished.load(Ordering::Relaxed), 3);
}

#[test]
fn resolved_session_unique_batch_latches_its_first_error() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();

    let result: Result<(), AccessError> = transaction.with_item_session(&resource, |session| {
        assert!(session.can_start_unique_item_batch());
        assert_eq!(
            session.try_with_unique_item_batch(unique, |_, _| Ok(())),
            Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
        );
        assert!(!session.can_start_unique_item_batch());
        assert_eq!(
            session.with_resolved_item(
                || Ok(Some((CollisionKey(1), ()))),
                || panic!("a doomed session must not invoke create"),
                |_, ()| Ok(())
            ),
            Err(AccessError::InvalidUse(InvalidUse::TransactionDoomed))
        );
        // Catching both inner errors cannot clear the session's first error.
        Ok(())
    });
    assert_eq!(
        result,
        Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
    );
    assert_eq!(initialized.load(Ordering::Relaxed), 0);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 0);
}

#[test]
fn resolved_item_session_latches_errors_and_checks_capacity_before_create() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(1)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let creates = AtomicUsize::new(0);
    let later_lookups = AtomicUsize::new(0);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    let result: Result<(), AccessError> = transaction.with_item_session(&resource, |session| {
        session.with_resolved_item(
            || Ok(Some((CollisionKey(1), ()))),
            || panic!("a resolved identity must not invoke create"),
            |_entry, ()| Ok(()),
        )?;
        let capacity = session.with_resolved_item(
            || Ok(None),
            || {
                creates.fetch_add(1, Ordering::Relaxed);
                Ok((CollisionKey(2), ()))
            },
            |_entry, ()| Ok(()),
        );
        assert_eq!(
            capacity,
            Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
        );
        let after_failure: Result<(), AccessError> = session.with_resolved_item(
            || {
                later_lookups.fetch_add(1, Ordering::Relaxed);
                Ok(Some((CollisionKey(1), ())))
            },
            || panic!("a doomed session must not invoke create"),
            |_entry, ()| Ok(()),
        );
        assert_eq!(
            after_failure,
            Err(AccessError::InvalidUse(InvalidUse::TransactionDoomed))
        );
        // Deliberately catch both errors: the outer boundary must still report
        // the original failure and leave the transaction doomed.
        Ok(())
    });

    assert_eq!(
        result,
        Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
    );
    assert_eq!(creates.load(Ordering::Relaxed), 0);
    assert_eq!(later_lookups.load(Ordering::Relaxed), 0);
    assert_eq!(initialized.load(Ordering::Relaxed), 1);
    assert!(transaction.is_doomed());
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 1);
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
fn validated_binding_cache_distinguishes_runtime_class_and_adapter_type() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let foreign_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let object = runtime.register_object().unwrap();

    let first_initialized = Arc::new(AtomicUsize::new(0));
    let first_finished = Arc::new(AtomicUsize::new(0));
    let first = object
        .register_resource(
            ResourceClass::new(77).unwrap(),
            FixtureAdapter {
                initialized: Arc::clone(&first_initialized),
                finished: Arc::clone(&first_finished),
                init_capacity_key: None,
            },
        )
        .unwrap();
    let second_initialized = Arc::new(AtomicUsize::new(0));
    let second_finished = Arc::new(AtomicUsize::new(0));
    let second = object
        .register_resource(
            ResourceClass::new(78).unwrap(),
            FixtureAdapter {
                initialized: Arc::clone(&second_initialized),
                finished: Arc::clone(&second_finished),
                init_capacity_key: None,
            },
        )
        .unwrap();
    assert_eq!(first.object_id(), second.object_id());
    assert_ne!(first.resource_class(), second.resource_class());

    let different_type = TxnCell::new(&runtime, 9_u64).unwrap();
    let (foreign, foreign_initialized, _) = fixture_resource(&foreign_runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    for key in [CollisionKey(1), CollisionKey(2)] {
        transaction.with_item(&first, key, |_| Ok(())).unwrap();
    }
    transaction
        .with_item(&second, CollisionKey(1), |_| Ok(()))
        .unwrap();
    assert_eq!(different_type.get(&mut transaction).unwrap(), 9);
    transaction
        .with_item(&first, CollisionKey(1), |_| Ok(()))
        .unwrap();

    assert_eq!(
        transaction.with_item(&foreign, CollisionKey(1), |_| Ok(())),
        Err(AccessError::InvalidUse(InvalidUse::WrongRuntime))
    );
    assert_eq!(foreign_initialized.load(Ordering::Relaxed), 0);
    assert_eq!(first_initialized.load(Ordering::Relaxed), 2);
    assert_eq!(second_initialized.load(Ordering::Relaxed), 1);
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(first_finished.load(Ordering::Relaxed), 2);
    assert_eq!(second_finished.load(Ordering::Relaxed), 1);
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
    let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();

    transaction
        .with_item(&resource, CollisionKey(1), |_entry| Ok(()))
        .unwrap();
    transaction
        .with_item(&resource, CollisionKey(2), |_entry| Ok(()))
        .unwrap();
    // Reusing an existing identity remains legal at the configured limit.
    transaction
        .with_item(&resource, CollisionKey(1), |_entry| Ok(()))
        .unwrap();
    assert_eq!(
        transaction.with_item(&resource, CollisionKey(3), |_entry| Ok(())),
        Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
    );
    assert_eq!(initialized.load(Ordering::Relaxed), 2);
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(sto_core::AbortReason::Doomed)
    );
    assert_eq!(finished.load(Ordering::Relaxed), 2);
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
