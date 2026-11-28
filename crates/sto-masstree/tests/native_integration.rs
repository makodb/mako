#![cfg(mtree_native_integration)]
use masstree::{Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig};
use sto_core::{AbortReason, CommitOutcome, Conflict, Runtime, RuntimeConfig};
use sto_masstree::{ScanBound, ScanDirection, ScanRequest, Table, TableConfig, Value};
#[test]
fn native_distributed_history_replay_and_retained_participant() {
    use sto_core::clock::distributed::{
        DistributedTerm, DistributedTimeTerm, DistributedTimestamp,
    };
    use sto_masstree::ReadVisibility;

    let decision = |base, term| {
        DistributedTimeTerm::new(
            DistributedTimestamp::new(base).unwrap(),
            DistributedTerm::new(term).unwrap(),
        )
    };
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    for direct in [false, true] {
        let config = TableConfig::default()
            .with_record_local_versions(true)
            .with_distributed_history(true);
        let table = Table::new(
            &sto_runtime,
            &native_runtime,
            &native_worker,
            config.with_record_local_versions(direct),
        )
        .unwrap();
        let mut owner = sto_runtime.attach().unwrap();
        for (value, source) in [
            (Some(Value::from(&b"stable"[..])), decision(10, 0)),
            (None, decision(20, 0)),
        ] {
            let mut replay = owner.begin().unwrap();
            assert!(table
                .stage_replay(&mut replay, &native_worker, b"key", value, source)
                .unwrap());
            assert!(matches!(
                replay.commit().unwrap(),
                CommitOutcome::Committed(_)
            ));
        }
        let mut participant = owner.begin().unwrap().into_participant();
        participant
            .with_transaction(|txn| {
                table.put(txn, &native_worker, b"key", b"first")?;
                Ok(())
            })
            .unwrap();
        participant.lock_writes().unwrap();
        participant
            .with_transaction(|txn| {
                table.put(txn, &native_worker, b"key", b"final")?;
                Ok(())
            })
            .unwrap();
        participant.validate().unwrap();
        participant.install(decision(30, 0)).unwrap();
        std::thread::scope(|scope| {
            scope
                .spawn(|| {
                    let native_observer = native_runtime.attach().unwrap();
                    let mut observer = sto_runtime.attach().unwrap();
                    let mut blocked = observer.begin().unwrap();
                    assert!(table
                        .get_versioned(
                            &mut blocked,
                            &native_observer,
                            b"key",
                            ReadVisibility::latest(decision(1, 0).term())
                        )
                        .is_err());
                    blocked.abort();
                    native_observer.quiesce().unwrap();
                })
                .join()
                .unwrap();
        });
        assert!(matches!(
            participant.finish(false).unwrap(),
            CommitOutcome::Committed(_)
        ));

        let mut policy = ReadVisibility::latest(decision(1, 1).term());
        policy.stable_through[0] = Some(10);
        let mut scan = owner.begin().unwrap();
        let rows = table
            .scan_versioned(
                &mut scan,
                &native_worker,
                ScanRequest::new(ScanDirection::Forward, 1),
                policy,
            )
            .unwrap();
        assert_eq!(rows.rows.len(), 1);
        assert_eq!(rows.rows[0].value.as_ref(), b"stable");
        assert_eq!(rows.rows[0].metadata, Some(decision(10, 0)));
        assert_eq!(rows.dependency, Some(decision(10, 0).timestamp()));
        assert!(matches!(
            scan.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));

        policy.stable_through[0] = Some(20);
        let mut read = owner.begin().unwrap();
        let selected = table
            .get_versioned(&mut read, &native_worker, b"key", policy)
            .unwrap();
        assert_eq!(selected.value, None);
        assert_eq!(selected.metadata, Some(decision(20, 0)));
        assert!(matches!(
            read.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));

        let mut read = owner.begin().unwrap();
        let selected = table
            .get_versioned(
                &mut read,
                &native_worker,
                b"key",
                ReadVisibility::latest(decision(1, 0).term()),
            )
            .unwrap();
        assert_eq!(selected.value.unwrap().as_ref(), b"final");
        assert_eq!(selected.metadata, Some(decision(30, 0)));
        assert!(matches!(
            read.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));

        table.advance_history_retention_floor(decision(20, 0).timestamp());
        let mut replay = owner.begin().unwrap();
        assert!(table
            .stage_replay(
                &mut replay,
                &native_worker,
                b"key",
                Some(Value::from(&b"pruned"[..])),
                decision(40, 0)
            )
            .unwrap());
        assert!(matches!(
            replay.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
        let mut read = owner.begin().unwrap();
        let selected = table
            .get_versioned(&mut read, &native_worker, b"key", policy)
            .unwrap();
        assert_eq!(selected.value, None);
        assert_eq!(selected.metadata, Some(decision(20, 0)));
        assert!(matches!(
            read.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
        policy.stable_through[0] = Some(10);
        let mut discarded = owner.begin().unwrap();
        assert!(table
            .get_versioned(&mut discarded, &native_worker, b"key", policy)
            .is_err());
        discarded.abort();
    }
    native_worker.quiesce().unwrap();
}

#[test]
fn native_point_commit_read_and_abort_round_trip() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = Table::new(
        &sto_runtime,
        &native_runtime,
        &native_worker,
        TableConfig::default(),
    )
    .unwrap();
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

#[test]
fn private_direct_native_scan_handles_chunks_bounds_binary_keys_and_tombstones() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let config = TableConfig::new()
        .with_scan_chunk_records(1)
        .with_scan_max_key_arena_bytes(64)
        .with_max_scan_chunks(32);
    let table = Table::new(&sto_runtime, &native_runtime, &native_worker, config).unwrap();
    let mut sto_worker = sto_runtime.attach().unwrap();

    let entries: &[(&[u8], &[u8])] = &[
        (b"", b""),
        (b"\0", b"nul"),
        (b"a", b"A"),
        (b"a\0", b"A0"),
        (b"long-key", b"long"),
        (b"z", b"removed"),
        (b"\xff", b"outside"),
    ];
    let mut seed = sto_worker.begin().unwrap();
    for (key, value) in entries {
        table.put(&mut seed, &native_worker, key, value).unwrap();
    }
    assert!(matches!(
        seed.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut remove = sto_worker.begin().unwrap();
    assert_eq!(
        table
            .remove(&mut remove, &native_worker, b"z")
            .unwrap()
            .as_deref(),
        Some(&b"removed"[..])
    );
    assert!(matches!(
        remove.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let request = |direction| {
        ScanRequest::new(direction, 16)
            .with_lower(ScanBound::Included(b""))
            .with_upper(ScanBound::Excluded(b"\xff"))
    };
    let expected = [
        (b"".to_vec(), b"".to_vec()),
        (b"\0".to_vec(), b"nul".to_vec()),
        (b"a".to_vec(), b"A".to_vec()),
        (b"a\0".to_vec(), b"A0".to_vec()),
        (b"long-key".to_vec(), b"long".to_vec()),
    ];

    let mut forward = sto_worker.begin().unwrap();
    let forward_rows: Vec<_> = table
        .scan(
            &mut forward,
            &native_worker,
            request(ScanDirection::Forward),
        )
        .unwrap()
        .iter()
        .map(|row| (row.key().to_vec(), row.value().to_vec()))
        .collect();
    assert_eq!(forward_rows, expected);
    assert!(matches!(
        forward.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));

    let mut reverse = sto_worker.begin().unwrap();
    let reverse_rows: Vec<_> = table
        .scan(
            &mut reverse,
            &native_worker,
            request(ScanDirection::Reverse),
        )
        .unwrap()
        .iter()
        .map(|row| (row.key().to_vec(), row.value().to_vec()))
        .collect();
    assert_eq!(reverse_rows, expected.into_iter().rev().collect::<Vec<_>>());
    assert!(matches!(
        reverse.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    native_worker.quiesce().unwrap();
}

#[test]
fn native_transactional_scan_resumes_and_rejects_a_phantom() {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new()).unwrap();
    let native_worker = native_runtime.attach().unwrap();
    let sto_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let config = TableConfig::new()
        .with_scan_chunk_records(1)
        .with_scan_max_key_arena_bytes(64)
        .with_max_scan_chunks(32);
    let table = Table::new(&sto_runtime, &native_runtime, &native_worker, config).unwrap();
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
