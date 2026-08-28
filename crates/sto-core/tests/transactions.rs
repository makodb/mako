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
    ItemInitError, NoPredicate, ObservationOrder, ObservationRef, OpacityToken, PredicateContext,
    PreflightContext, PreflightItem, PrepareError, ResourceClass, Runtime, RuntimeConfig,
    Transaction, TransactionalResource, TxnCell, UniqueItemKeys, ValidationContext,
};

#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct CollisionKey(u64);

impl Hash for CollisionKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        assert_ne!(self.0, u64::MAX, "injected key-hash panic");
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
fn unique_batch_lazily_materializes_an_exact_index_for_later_access() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, initialized, finished) = fixture_resource(&runtime);
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let keys = [CollisionKey(1), CollisionKey(2), CollisionKey(3)];
    let unique = UniqueItemKeys::try_new(&keys).unwrap();

    transaction
        .with_unique_item_batch(&resource, unique, |index, entry| {
            assert!(matches!(entry.observation(), ObservationRef::Unobserved));
            entry.record_read(UnorderedObservation)?;
            if index == 1 {
                // The optimized append lane deliberately retains the complete
                // Entry surface rather than imposing read-only semantics.
                entry.stage(())?;
            }
            Ok(())
        })
        .unwrap();
    assert_eq!(initialized.load(Ordering::Relaxed), 3);

    // The first ordinary access materializes all three colliding identities.
    // It must find key two rather than initialize an alias.
    transaction
        .with_item(&resource, CollisionKey(2), |entry| {
            assert!(matches!(entry.observation(), ObservationRef::Read(_)));
            assert!(entry.intent().is_some());
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
        let _ = transaction.with_item(&resource, CollisionKey(3), |_| Ok(()));
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
            assert!(session.try_with_unique_item_batch(unique, |_, entry| {
                entry.record_read(UnorderedObservation)
            })?);

            let ineligible_keys = [CollisionKey(4)];
            let ineligible = UniqueItemKeys::try_new(&ineligible_keys).unwrap();
            let calls = AtomicUsize::new(0);
            assert!(!session.try_with_unique_item_batch(ineligible, |_, _| {
                calls.fetch_add(1, Ordering::Relaxed);
                Ok(())
            })?);
            assert_eq!(calls.load(Ordering::Relaxed), 0);

            // Ineligibility is not a failure. The ordinary session path now
            // materializes the exact index and reuses a batched item.
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
        assert_eq!(
            session.try_with_unique_item_batch(unique, |_, _| Ok(())),
            Err(AccessError::Capacity(sto_core::CapacityError::ItemLimit))
        );
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
