//! Record-local policy checks over the memory directory, using the real STO core.

use super::*;
use sto_core::{
    AbortReason, BeginError, CommitFailure, CommitHook, CommitHookError, CommitInfo, CommitOutcome,
    RuntimeConfig, RuntimeHealth, TxnCell, VersionState, WorkerContext,
};

fn table(runtime: &Arc<Runtime>, direct: bool, local: bool) -> Table {
    let config = TableConfig::default().with_record_local_versions(local);
    if direct {
        Table::new_memory_direct(runtime, config)
    } else {
        Table::new_memory(runtime, config)
    }
}

fn record<'a>(table: &'a Table, key: &[u8]) -> RecordAccess<'a> {
    let id = table.shared().lookup(None, key).unwrap().unwrap();
    table.shared().resolve_directory_access(id).unwrap()
}

fn version(table: &Table, key: &[u8]) -> OccVersion {
    record(table, key).record.version.observe().unwrap()
}

fn committed(result: Result<CommitOutcome, CommitFailure>) -> CommitInfo {
    match result.unwrap() {
        CommitOutcome::Committed(info) => info,
        other => panic!("unexpected outcome: {other:?}"),
    }
}

fn put(table: &Table, worker: &mut WorkerContext, key: &[u8], value: &[u8]) -> CommitInfo {
    let mut txn = worker.begin().unwrap();
    table
        .put_presence_inner(&mut txn, None, key, Value::from(value))
        .unwrap();
    committed(txn.commit())
}

fn raise_version(table: &Table, key: &[u8], generation: u64) {
    let record = record(table, key).record;
    let mut guard = record
        .version
        .try_acquire_detached(OwnerId::new(500).unwrap())
        .unwrap();
    guard
        .release_commit(&record.version, OccCommitId::new(generation).unwrap())
        .unwrap();
}

#[derive(Default)]
struct Hooks {
    reserve: usize,
    before: usize,
    after: usize,
}

impl CommitHook for Hooks {
    fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
        self.reserve += 1;
        Ok(())
    }

    fn pre_install(&mut self) -> Result<(), CommitHookError> {
        self.before += 1;
        Ok(())
    }

    fn post_install(&mut self) {
        self.after += 1;
    }
}

#[test]
fn local_scan_generation_combination_is_rejected_before_admission() {
    assert!(!TableConfig::default().record_local_versions());
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let budget = RegistryBudget::new(0);
    let invalid = TableConfig::default()
        .with_record_local_versions(true)
        .with_trusted_scan_value_generation(true);
    assert!(matches!(
        Table::admit_table(&runtime, invalid, budget.clone()),
        Err(RegistrationError::Unsupported(Unsupported::Capability(_)))
    ));
    assert_eq!(budget.used_bytes(), 0);
    // Object identities are monotonic. Rejection must not even consume one.
    assert_eq!(runtime.register_object().unwrap().object_id().get(), 1);
    assert_eq!(runtime.health(), RuntimeHealth::Healthy);
}

#[test]
fn local_writers_keep_hooks_and_ignore_read_only_ordered_bindings() {
    for direct in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, direct, true);
        let ordered = TxnCell::new(&runtime, 0_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        assert_eq!(ordered.get(&mut txn).unwrap(), 0);
        local
            .put_inner(&mut txn, None, b"row", Arc::from(&b"value"[..]))
            .unwrap();
        let mut hooks = Hooks::default();
        assert_eq!(
            committed(txn.commit_with_hook(&mut hooks)).occ_commit_id(),
            None
        );
        assert_eq!((hooks.reserve, hooks.before, hooks.after), (1, 1, 1));
        assert_eq!(version(&local, b"row").get(), 2);
        let mut txn = worker.begin().unwrap();
        ordered.set(&mut txn, 1).unwrap();
        assert_eq!(committed(txn.commit()).occ_commit_id().unwrap().get(), 2);
    }
}

#[test]
fn higher_local_versions_mix_with_ordered_cells_and_same_type_tables() {
    for direct in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, direct, true);
        let ordered_table = table(&runtime, direct, false);
        let ordered_cell = TxnCell::new(&runtime, 0_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        for _ in 0..16 {
            assert_eq!(
                put(&local, &mut worker, b"row", b"local").occ_commit_id(),
                None
            );
        }
        assert_eq!(version(&local, b"row").get(), 17);
        let mut mixed = worker.begin().unwrap();
        local
            .put_presence_inner(&mut mixed, None, b"row", Value::from(&b"mixed"[..]))
            .unwrap();
        ordered_table
            .put_presence_inner(&mut mixed, None, b"row", Value::from(&b"ordered"[..]))
            .unwrap();
        let ordered_id = committed(mixed.commit()).occ_commit_id().unwrap();
        assert_eq!(ordered_id.get(), 2);
        assert_eq!(version(&local, b"row").get(), 18);
        assert_eq!(version(&ordered_table, b"row"), ordered_id.to_version());

        let mut mixed = worker.begin().unwrap();
        ordered_cell.set(&mut mixed, 1).unwrap();
        local
            .put_presence_inner(&mut mixed, None, b"row", Value::from(&b"cell"[..]))
            .unwrap();
        assert_eq!(committed(mixed.commit()).occ_commit_id().unwrap().get(), 3);
        assert_eq!(version(&local, b"row").get(), 19);
    }
}

#[test]
fn local_same_key_worker_handoff_rejects_stale_reads() {
    for direct in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, direct, true);
        let mut worker = runtime.attach().unwrap();
        put(&local, &mut worker, b"row", b"before");
        let mut stale = worker.begin().unwrap();
        assert_eq!(
            local
                .get_inner(&mut stale, None, b"row")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let local = local.clone();
            scope
                .spawn(move || {
                    let mut other = runtime.attach().unwrap();
                    assert_eq!(
                        put(&local, &mut other, b"row", b"winner").occ_commit_id(),
                        None
                    );
                })
                .join()
                .unwrap();
        });
        local
            .put_presence_inner(&mut stale, None, b"row", Value::from(&b"loser"[..]))
            .unwrap();
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(version(&local, b"row").get(), 3);
        assert_eq!(
            put(&local, &mut worker, b"row", b"handoff").occ_commit_id(),
            None
        );
        assert_eq!(version(&local, b"row").get(), 4);
    }
}

#[test]
fn local_exhaustion_aborts_all_writes_before_hooks_and_stops_later_transactions() {
    for (direct, materialized) in [(false, false), (true, false), (true, true)] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, direct, true);
        let ordered = TxnCell::new(&runtime, 0_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        put(&local, &mut worker, b"first", b"unchanged");
        put(&local, &mut worker, b"max", b"unchanged");
        raise_version(&local, b"max", OccVersion::MAX_VALUE - 1);
        put(&local, &mut worker, b"max", b"last");
        assert_eq!(version(&local, b"max").get(), OccVersion::MAX_VALUE);
        let before = version(&local, b"first");
        let mut txn = worker.begin().unwrap();
        if materialized {
            ordered.set(&mut txn, 1).unwrap();
        }
        local
            .put_presence_inner(
                &mut txn,
                None,
                b"first",
                Value::from(&b"must-not-install"[..]),
            )
            .unwrap();
        local
            .put_presence_inner(&mut txn, None, b"max", Value::from(&b"must-not-wrap"[..]))
            .unwrap();
        let mut hooks = Hooks::default();
        assert_eq!(
            txn.commit_with_hook(&mut hooks).unwrap(),
            CommitOutcome::Aborted(AbortReason::Capacity(CapacityError::VersionExhausted))
        );
        assert_eq!((hooks.reserve, hooks.before, hooks.after), (0, 0, 0));
        assert_eq!(runtime.health(), RuntimeHealth::Exhausted);
        assert_eq!(version(&local, b"first"), before);
        assert_eq!(version(&local, b"max").get(), OccVersion::MAX_VALUE);
        for (key, expected) in [
            (b"first".as_slice(), b"unchanged".as_slice()),
            (b"max".as_slice(), b"last".as_slice()),
        ] {
            let access = record(&local, key);
            match access.record.state.load(access.stable) {
                CommittedStateLoad::Complete { state, .. } => {
                    assert_eq!(state.value().map(Value::as_ref), Some(expected))
                }
                _ => panic!("exhaustion must not start value publication"),
            }
        }
        assert!(matches!(worker.begin(), Err(BeginError::Poisoned(_))));
        assert!(matches!(
            runtime.attach(),
            Err(sto_core::AttachError::Poisoned(_))
        ));
    }
}

#[test]
fn every_point_observation_uses_the_fixed_table_version_domain() {
    assert_eq!(std::mem::size_of::<RecordObservation>(), 16);
    for local in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(
            &runtime,
            TableConfig::default()
                .with_bounded_atomic_values(true)
                .with_record_local_versions(local),
        );
        let mut worker = runtime.attach().unwrap();
        for length in [None, Some(0), Some(8), Some(128), Some(161)] {
            let key = format!("length-{length:?}");
            if let Some(length) = length {
                put(&table, &mut worker, key.as_bytes(), &vec![3; length]);
            }
            for accessor in 0..4 {
                let mut txn = worker.begin().unwrap();
                match accessor {
                    0 => {
                        table.get_inner(&mut txn, None, key.as_bytes()).unwrap();
                    }
                    1 => {
                        table
                            .copy_get_resolving_inner(&mut txn, None, key.as_bytes(), &mut [0; 192])
                            .unwrap();
                    }
                    2 => {
                        table
                            .visit_get_inner(&mut txn, None, key.as_bytes(), |_| ())
                            .unwrap();
                    }
                    _ => {
                        table
                            .put_presence_inner(
                                &mut txn,
                                None,
                                key.as_bytes(),
                                Value::from(&b"staged"[..]),
                            )
                            .unwrap();
                    }
                }
                let id = table
                    .shared()
                    .lookup(None, key.as_bytes())
                    .unwrap()
                    .unwrap();
                txn.with_item(&table.record_resource, TableKey::Record(id), |entry| {
                    let observation = match entry.observation() {
                        ObservationRef::Read(observation) => observation,
                        _ => panic!("point access must retain a read observation"),
                    };
                    assert_eq!(
                        observation.observation_order(),
                        if local {
                            ObservationOrder::Unordered
                        } else {
                            ObservationOrder::Ordered(version(&table, key.as_bytes()))
                        }
                    );
                    Ok(())
                })
                .unwrap();
                txn.abort();
            }
        }
    }
}

#[test]
fn local_scans_and_read_your_writes_keep_row_and_directory_validation() {
    for direct in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, direct, true);
        let mut worker = runtime.attach().unwrap();
        put(&local, &mut worker, b"a", b"old-a");
        put(&local, &mut worker, b"b", b"old-b");
        let before = version(&local, b"a");
        let mut txn = worker.begin().unwrap();
        local
            .put_inner(&mut txn, None, b"a", Arc::from(&b"new-a"[..]))
            .unwrap();
        local.remove_inner(&mut txn, None, b"b").unwrap();
        assert_eq!(
            local.get_inner(&mut txn, None, b"a").unwrap().as_deref(),
            Some(&b"new-a"[..])
        );
        assert_eq!(local.get_inner(&mut txn, None, b"b").unwrap(), None);
        for direction in [ScanDirection::Forward, ScanDirection::Reverse] {
            let rows = local
                .scan_inner(&mut txn, None, ScanRequest::new(direction, 8))
                .unwrap();
            assert_eq!(rows.len(), 1);
            assert_eq!(rows[0].key(), b"a");
            assert_eq!(rows[0].value(), b"new-a");
        }
        txn.abort();
        assert_eq!(version(&local, b"a"), before);

        let mut stale = worker.begin().unwrap();
        assert_eq!(
            local
                .scan_inner(
                    &mut stale,
                    None,
                    ScanRequest::new(ScanDirection::Forward, 8)
                )
                .unwrap()
                .len(),
            2
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let local = local.clone();
            scope
                .spawn(move || {
                    let mut other = runtime.attach().unwrap();
                    put(&local, &mut other, b"a", b"winner");
                    put(&local, &mut other, b"c", b"phantom");
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }
}

#[test]
fn indeterminate_local_release_ignores_mixed_ordered_id() {
    struct PanicAfterInstall;
    impl CommitHook for PanicAfterInstall {
        fn reserve_upper_metadata(&mut self) -> Result<(), CommitHookError> {
            Ok(())
        }
        fn pre_install(&mut self) -> Result<(), CommitHookError> {
            Ok(())
        }
        fn post_install(&mut self) {
            panic!("exact local-version quarantine test");
        }
    }
    for direct in [false, true] {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, direct, true);
        let ordered = TxnCell::new(&runtime, 0_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        put(&local, &mut worker, b"row", b"before");
        for value in 1..8 {
            let mut txn = worker.begin().unwrap();
            ordered.set(&mut txn, value).unwrap();
            committed(txn.commit());
        }
        let mut txn = worker.begin().unwrap();
        local
            .put_presence_inner(&mut txn, None, b"row", Value::from(&b"after"[..]))
            .unwrap();
        ordered.set(&mut txn, 9).unwrap();
        assert!(matches!(
            txn.commit_with_hook(&mut PanicAfterInstall),
            Err(CommitFailure::Indeterminate(_))
        ));
        assert_eq!(runtime.health(), RuntimeHealth::Indeterminate);
        assert_eq!(
            record(&local, b"row").record.version.state(),
            VersionState::Unlocked(OccVersion::new(3).unwrap())
        );
    }
}
