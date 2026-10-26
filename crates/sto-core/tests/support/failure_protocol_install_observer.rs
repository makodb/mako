//! The observer must run between real installs, not at the all-installed hook.

use super::*;

static OWNED_LOCKS: sto_core::UniqueLockCommitCapability<DirectInjectAdapter, InjectLock> =
    sto_core::UniqueLockCommitCapability::new(
        |adapter, key, item| {
            Ok(prepare_inject_direct(adapter, key, item)?.map(|identity| {
                LockRequest::new(
                    identity,
                    Arc::new(InjectLock {
                        key: *key,
                        harness: Arc::clone(&adapter.harness),
                    }),
                )
            }))
        },
        |adapter, key, item, lock, _| {
            assert_eq!(item.intent().is_some(), lock.is_some());
            if let Some(lock) = lock {
                assert_eq!(lock.guard().key, *key);
            }
            adapter.harness.record(Event::Validation(*key));
            Ok(())
        },
        |adapter, key, _, mut lock, _| {
            assert_eq!(lock.guard_mut().key, *key);
            adapter.harness.record(Event::Install(*key));
        },
    );

pub(super) static OWNED_DIRECT_COMMIT: DirectCommitCapability<DirectInjectAdapter> =
    DirectCommitCapability::unique_lock(&OWNED_LOCKS);

fn stop_after_first_install(installed: usize) {
    assert_eq!(installed, 1, "the observer must run after each install");
    panic!("injected failure between the first and second installs");
}

fn assert_partial_install(
    result: Result<CommitOutcome, CommitFailure>,
    runtime: &Runtime,
    harness: &Harness,
) {
    let Err(CommitFailure::Indeterminate(info)) = result else {
        panic!("expected a partial-install failure, got {result:?}");
    };
    assert_eq!(info.phase(), FailurePhase::Install);
    assert!(info.occ_commit_id().is_some());
    let events = harness.events();
    assert_eq!(callback_keys(&events, Callback::Install), [1]);
    assert!(release_events(&events)
        .iter()
        .all(|(_, disposition)| matches!(disposition, LockDisposition::Indeterminate { .. })));
    assert!(finish_events(&events).is_empty());
    assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
}

#[test]
fn typed_batch_stops_after_one_of_two_real_installs() {
    let fixture = fixture(Vec::new());
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &fixture.resource, 1);
    stage_write(&mut transaction, &fixture.resource, 2);
    let result =
        sto_core::test_install::with_observer(stop_after_first_install, || transaction.commit());
    assert_partial_install(result, &fixture.runtime, &fixture.harness);

    // A contained observer panic must not leave a callback armed on this OS
    // thread. A different, healthy runtime can still commit both writes.
    let healthy = super::fixture(Vec::new());
    let mut worker = healthy.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &healthy.resource, 1);
    stage_write(&mut transaction, &healthy.resource, 2);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(
        callback_keys(&healthy.harness.events(), Callback::Install),
        [1, 2]
    );
}

#[test]
fn materialized_batch_stops_after_one_of_two_real_installs() {
    let fixture = fixture(Vec::new());
    let array = TxnArray::new(&fixture.runtime, [11_u64]).unwrap();
    let mut worker = fixture.runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_write(&mut transaction, &fixture.resource, 1);
    stage_write(&mut transaction, &fixture.resource, 2);
    // A second resource type converts the typed batch to erased items.
    assert_eq!(array.get(&mut transaction, 0).unwrap(), Ok(11));
    let result =
        sto_core::test_install::with_observer(stop_after_first_install, || transaction.commit());
    assert_partial_install(result, &fixture.runtime, &fixture.harness);
}

#[test]
fn borrowed_direct_batches_stop_after_one_of_two_real_installs() {
    for identity_mode in [DirectIdentityMode::Checked, DirectIdentityMode::Injective] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let (resource, harness) =
            register_direct_resource_with_modes(&runtime, Vec::new(), false, false, identity_mode);
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        stage_direct_write(&mut transaction, &resource, 1);
        stage_direct_write(&mut transaction, &resource, 2);
        let result = sto_core::test_install::with_observer(stop_after_first_install, || {
            transaction.commit()
        });
        assert_partial_install(result, &runtime, &harness);
    }
}

#[test]
fn owned_direct_batch_stops_after_one_of_two_real_installs() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let (resource, harness) = register_direct_resource_with_modes(
        &runtime,
        Vec::new(),
        false,
        false,
        DirectIdentityMode::Owned,
    );
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    stage_direct_write(&mut transaction, &resource, 1);
    stage_direct_write(&mut transaction, &resource, 2);
    let result =
        sto_core::test_install::with_observer(stop_after_first_install, || transaction.commit());
    assert_partial_install(result, &runtime, &harness);
}
