#![cfg(mtree_native_integration)]

use masstree::{Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig};
#[cfg(feature = "fixed-u64")]
use sto_core::InvalidUse;
use sto_core::{
    AbortReason, AccessError, CapacityError, CommitOutcome, Conflict, Runtime, RuntimeConfig,
};
#[cfg(feature = "fixed-u64")]
use sto_masstree::{FixedU64Batch, FixedU64Mutation, FixedU64Table, TerminalReadVisitOutcome};
use sto_masstree::{
    InsertOutcome, PointMutation, PointReadBatch, RegistryLayout, ScanBound, ScanDirection,
    ScanRequest, Table, TableConfig, Value,
};

#[test]
fn native_point_commit_read_and_abort_round_trip() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = Table::new(&sto_runtime, tree, TableConfig::default()).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();
    let key = b"native\0point";

    let mut write = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .put(&mut write, &native_worker, key, b"committed")
            .unwrap(),
        None
    );
    assert!(matches!(
        write.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut read = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .get(&mut read, &native_worker, key)
            .unwrap()
            .as_deref(),
        Some(&b"committed"[..])
    );
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut aborted = sto_worker.begin().unwrap();
    table
        .put(&mut aborted, &native_worker, key, b"aborted")
        .unwrap();
    assert_eq!(aborted.abort().reason(), &AbortReason::Explicit);

    let mut verify = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .get(&mut verify, &native_worker, key)
            .unwrap()
            .as_deref(),
        Some(&b"committed"[..])
    );
    assert!(matches!(
        verify.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    native_worker.quiesce().unwrap();
}

#[cfg(feature = "fixed-u64")]
#[test]
fn fixed_u64_public_native_loader_mutation_and_terminal_read_round_trip() {
    const RECORD_LIMIT: u64 = 16;
    const REGISTRY_BUDGET_BYTES: usize = 2 * 1024 * 1024;

    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = FixedU64Table::new(
        &sto_runtime,
        &native_runtime,
        &native_worker,
        TableConfig::new()
            .with_max_retained_records(RECORD_LIMIT)
            .with_max_retained_key_bytes(8 * RECORD_LIMIT)
            .with_max_consumed_record_ids(RECORD_LIMIT)
            .with_registry_layout(RegistryLayout::EagerContiguous {
                max_bytes: REGISTRY_BUDGET_BYTES,
            }),
    )
    .unwrap();
    let key_a = 7_u64.to_be_bytes();
    let key_b = 9_u64.to_be_bytes();
    table.insert_initial(&native_worker, &key_a, 10).unwrap();
    table.insert_initial(&native_worker, &key_b, 20).unwrap();
    table.insert_initial(&native_worker, &key_a, 10).unwrap();
    assert_eq!(table.usage().retained_records(), 2);
    table.finish_initial_load().unwrap();
    assert_eq!(
        table
            .insert_initial(&native_worker, &11_u64.to_be_bytes(), 30)
            .unwrap_err(),
        AccessError::InvalidUse(InvalidUse::IllegalItemState)
    );

    let mut sto_worker = sto_runtime.attach().unwrap();
    let keys = [key_a, key_b];
    let mut batch = FixedU64Batch::with_capacity(keys.len());
    let retained_capacity = batch.capacity();
    let mut transaction = sto_worker.begin().unwrap();
    let mut observed = Vec::new();
    assert_eq!(
        table
            .modify_fixed(
                &mut transaction,
                &native_worker,
                &keys,
                &mut batch,
                |index, value| {
                    observed.push(value);
                    if index == 0 {
                        FixedU64Mutation::Put(value + 1)
                    } else {
                        FixedU64Mutation::Keep
                    }
                },
            )
            .unwrap(),
        Some(keys.len())
    );
    assert_eq!(observed, [10, 20]);
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    {
        let transaction = sto_worker.begin_terminal_read_batch().unwrap();
        let mut observed = Vec::new();
        let outcome = table
            .visit_fixed_terminal(
                transaction,
                &native_worker,
                &keys,
                &mut batch,
                |index, value| observed.push((index, value)),
            )
            .unwrap();
        let (transaction, visited) = match outcome {
            TerminalReadVisitOutcome::Ready {
                transaction,
                visited,
            } => (transaction, visited),
            TerminalReadVisitOutcome::RetryOrdinary => {
                panic!("preloaded fixed-u64 keys must remain present");
            }
        };
        assert_eq!(visited, keys.len());
        assert!(matches!(
            transaction.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
        assert_eq!(observed, [(0, 11), (1, 20)]);
    }
    assert_eq!(batch.capacity(), retained_capacity);

    let transaction = sto_worker.begin_terminal_read_batch().unwrap();
    let outcome = table
        .visit_fixed_terminal(
            transaction,
            &native_worker,
            &[99_u64.to_be_bytes()],
            &mut batch,
            |_, _| panic!("a terminal miss must invoke no visitor"),
        )
        .unwrap();
    assert!(matches!(outcome, TerminalReadVisitOutcome::RetryOrdinary));
    native_worker.quiesce().unwrap();
}

#[test]
fn eager_contiguous_registry_native_read_write_round_trip() {
    const RECORD_LIMIT: u64 = 32;
    // Deliberately leave ample headroom over the bounded arena's current
    // accounting so this public integration test does not encode its private
    // slot or sidecar representation.
    const REGISTRY_BUDGET_BYTES: usize = 2 * 1024 * 1024;

    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let config = TableConfig::new()
        .with_max_retained_records(RECORD_LIMIT)
        .with_max_retained_key_bytes(4 * 1024)
        .with_max_consumed_record_ids(RECORD_LIMIT)
        .with_registry_layout(RegistryLayout::EagerContiguous {
            max_bytes: REGISTRY_BUDGET_BYTES,
        });
    let table = Table::new(&sto_runtime, tree, config).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();
    let key = b"eager\0native";

    let mut write = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .put(&mut write, &native_worker, key, b"contiguous")
            .unwrap(),
        None
    );
    assert!(matches!(
        write.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let usage = table.usage();
    assert_eq!(usage.retained_records(), 1);
    assert_eq!(usage.consumed_record_ids(), 1);

    let mut read = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .get(&mut read, &native_worker, key)
            .unwrap()
            .as_deref(),
        Some(&b"contiguous"[..])
    );
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    native_worker.quiesce().unwrap();
}

#[test]
fn point_session_closes_on_miss_and_scan_and_reopens_after_each_boundary() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = Table::new(&sto_runtime, tree, TableConfig::default()).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();

    let mut seed = sto_worker.begin().unwrap();
    table
        .put(&mut seed, &native_worker, b"session/a", b"A")
        .unwrap();
    table
        .put(&mut seed, &native_worker, b"session/b", b"B")
        .unwrap();
    assert!(matches!(
        seed.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut transaction = sto_worker.begin().unwrap();
    let mut points = table.point_session(&mut transaction, &native_worker);
    assert_eq!(
        points.get(b"session/a").unwrap().as_deref(),
        Some(&b"A"[..])
    );
    assert_eq!(
        points.put(b"session/a", b"A2").unwrap().as_deref(),
        Some(&b"A"[..])
    );
    assert_eq!(
        points.get(b"session/a").unwrap().as_deref(),
        Some(&b"A2"[..])
    );

    // This miss must end the active native read scope before get_or_insert.
    assert_eq!(
        points.insert(b"session/new", b"N").unwrap(),
        InsertOutcome::Inserted
    );
    // The next hit opens another scope, which remove and scan both reuse/end.
    assert_eq!(
        points.remove(b"session/b").unwrap().as_deref(),
        Some(&b"B"[..])
    );
    assert_eq!(
        points.get(b"session/a").unwrap().as_deref(),
        Some(&b"A2"[..])
    );
    let rows = points
        .scan(
            ScanRequest::new(ScanDirection::Forward, 8)
                .with_lower(ScanBound::Included(b"session/"))
                .with_upper(ScanBound::Excluded(b"session0")),
        )
        .unwrap();
    let rows: Vec<_> = rows
        .iter()
        .map(|row| (row.key().to_vec(), row.value().to_vec()))
        .collect();
    assert_eq!(
        rows,
        [
            (b"session/a".to_vec(), b"A2".to_vec()),
            (b"session/new".to_vec(), b"N".to_vec()),
        ]
    );
    // A point lookup after scan proves that the lazy scope can reopen.
    assert_eq!(
        points.get(b"session/new").unwrap().as_deref(),
        Some(&b"N"[..])
    );
    points.close().unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut aborted = sto_worker.begin().unwrap();
    {
        let mut points = table.point_session(&mut aborted, &native_worker);
        assert!(points.get(b"session/a").unwrap().is_some());
        // Implicit Drop must end the native scope on every early-return path.
    }
    assert_eq!(aborted.abort().reason(), &AbortReason::Explicit);
    native_worker.quiesce().unwrap();
}

#[test]
fn fixed_point_batch_handles_binary_hits_misses_and_reuses_storage() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = Table::new(&sto_runtime, tree, TableConfig::default()).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();
    let hit_a = [0x00, 0x11, 0x00, 0xff, 0x41, 0x00, 0x7f, 0x80];
    let miss = [0x00, 0x22, 0x00, 0xff, 0x42, 0x00, 0x7f, 0x80];
    let hit_b = [0xff, 0x00, 0x33, 0x00, 0x43, 0x00, 0x7f, 0x80];

    let mut seed = sto_worker.begin().unwrap();
    table.put(&mut seed, &native_worker, &hit_a, b"A").unwrap();
    table.put(&mut seed, &native_worker, &hit_b, b"B").unwrap();
    assert!(matches!(
        seed.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let keys = [hit_a, miss, miss, hit_b];
    let mut batch = PointReadBatch::with_capacity(keys.len());
    let retained_capacity = batch.capacity();
    let mut transaction = sto_worker.begin().unwrap();
    let mut points = table.point_session(&mut transaction, &native_worker);
    let snapshots = points
        .modify_fixed(&keys, &mut batch, |index, current| {
            if index == 0 {
                assert_eq!(current.map(Value::as_ref), Some(&b"A"[..]));
                PointMutation::Put(Value::from(&b"A2"[..]))
            } else {
                PointMutation::Keep
            }
        })
        .unwrap();
    assert_eq!(snapshots[0].as_deref(), Some(&b"A"[..]));
    assert_eq!(snapshots[1], None);
    assert_eq!(snapshots[2], None);
    assert_eq!(snapshots[3].as_deref(), Some(&b"B"[..]));
    points.close().unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(batch.capacity(), retained_capacity);

    let mut verify = sto_worker.begin().unwrap();
    let mut points = table.point_session(&mut verify, &native_worker);
    let snapshots = points.get_fixed(&[hit_a, hit_b], &mut batch).unwrap();
    assert_eq!(snapshots[0].as_deref(), Some(&b"A2"[..]));
    assert_eq!(snapshots[1].as_deref(), Some(&b"B"[..]));
    assert!(points
        .get_fixed(&[] as &[[u8; 8]], &mut batch)
        .unwrap()
        .is_empty());
    points.close().unwrap();
    assert!(matches!(
        verify.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(batch.capacity(), retained_capacity);

    let repeated = [hit_a, hit_a, hit_b];
    let mut transaction = sto_worker.begin().unwrap();
    let mut points = table.point_session(&mut transaction, &native_worker);
    let mut observed = Vec::new();
    let visited = points
        .modify_fixed_visit(&repeated, &mut batch, |index, current| {
            observed.push((index, current.map(|value| value.as_ref().to_vec())));
            if index == 0 {
                PointMutation::Put(Value::from(&b"A3"[..]))
            } else {
                PointMutation::Keep
            }
        })
        .unwrap();
    assert_eq!(visited, repeated.len());
    assert_eq!(
        observed,
        vec![
            (0, Some(b"A2".to_vec())),
            (1, Some(b"A3".to_vec())),
            (2, Some(b"B".to_vec())),
        ]
    );
    assert!(batch.results().is_empty());
    points.close().unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let keys = [hit_b, hit_a];
    let mut transaction = sto_worker.begin().unwrap();
    let mut points = table.point_session(&mut transaction, &native_worker);
    let mut observed = Vec::new();
    let visited = points
        .visit_fixed(&keys, &mut batch, |index, current| {
            observed.push((index, current.map(|value| value.as_ref().to_vec())));
        })
        .unwrap();
    assert_eq!(visited, keys.len());
    assert_eq!(
        observed,
        vec![(0, Some(b"B".to_vec())), (1, Some(b"A3".to_vec()))]
    );
    assert!(batch.results().is_empty());
    points.close().unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    assert_eq!(batch.capacity(), retained_capacity);
    native_worker.quiesce().unwrap();
}

#[test]
fn fixed_point_batch_capacity_error_dooms_and_clears_results() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
    let table = Table::new(&sto_runtime, tree, TableConfig::default()).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();
    let keys = [[0x00; 8], [0x80; 8], [0xff; 8]];

    for (index, key) in keys.iter().enumerate() {
        let value = [index as u8];
        let mut seed = sto_worker.begin().unwrap();
        table.put(&mut seed, &native_worker, key, &value).unwrap();
        assert!(matches!(
            seed.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
    }

    let mut batch = PointReadBatch::with_capacity(keys.len());
    let mut transaction = sto_worker.begin().unwrap();
    {
        let mut points = table.point_session(&mut transaction, &native_worker);
        assert_eq!(
            points.get_fixed(&keys, &mut batch).unwrap_err(),
            AccessError::Capacity(CapacityError::ItemLimit)
        );
        assert!(batch.is_empty());
        points.close().unwrap();
    }
    assert!(transaction.is_doomed());
    transaction.abort();
    native_worker.quiesce().unwrap();
}

#[test]
fn native_transactional_scan_resumes_and_rejects_a_phantom() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let tree = native_runtime.create_tree(&native_worker).unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let config = TableConfig::new()
        .with_scan_chunk_records(1)
        .with_scan_initial_key_arena_bytes(1)
        .with_scan_max_key_arena_bytes(64)
        .with_max_scan_chunks(32);
    let table = Table::new(&sto_runtime, tree, config).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();

    let mut seed = sto_worker.begin().unwrap();
    table
        .put(&mut seed, &native_worker, b"scan/a", b"A")
        .unwrap();
    table
        .put(&mut seed, &native_worker, b"scan/long", b"L")
        .unwrap();
    assert!(matches!(
        seed.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut scanner = sto_worker.begin().unwrap();
    let rows = table
        .scan(
            &mut scanner,
            &native_worker,
            ScanRequest::new(ScanDirection::Forward, 8)
                .with_lower(ScanBound::Included(b"scan/"))
                .with_upper(ScanBound::Excluded(b"scan0")),
        )
        .unwrap();
    let keys: Vec<_> = rows.iter().map(|row| row.key().to_vec()).collect();
    assert_eq!(keys, [b"scan/a".to_vec(), b"scan/long".to_vec()]);

    std::thread::scope(|scope| {
        let native_runtime = native_runtime.clone();
        let sto_runtime = sto_runtime.clone();
        let table = table.clone();
        scope
            .spawn(move || {
                let native_worker = native_runtime.attach().unwrap();
                let mut sto_worker = sto_runtime.attach().unwrap();
                let mut writer = sto_worker.begin().unwrap();
                table
                    .put(&mut writer, &native_worker, b"scan/new", b"N")
                    .unwrap();
                assert!(matches!(
                    writer.commit().unwrap(),
                    CommitOutcome::Committed(_)
                ));
                native_worker.quiesce().unwrap();
            })
            .join()
            .unwrap();
    });

    assert_eq!(
        scanner.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    native_worker.quiesce().unwrap();
}
