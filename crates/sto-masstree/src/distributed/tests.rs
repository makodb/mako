use super::*;
use sto_core::{CommitOutcome, RuntimeConfig, WorkerContext};

fn decision(base: u32, term: u32) -> DistributedTimeTerm {
    DistributedTimeTerm::new(DistributedTimestamp::new(base).unwrap(), DistributedTerm::new(term).unwrap())
}

fn table(direct: bool) -> (Arc<Runtime>, Table) {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let config = TableConfig::default().with_record_local_versions(true).with_distributed_history(true);
    let table = if direct { Table::new_memory_direct(&runtime, config) } else { Table::new_memory(&runtime, config) };
    (runtime, table)
}

fn commit(txn: Transaction<'_, Active>) {
    assert!(matches!(txn.commit(), Ok(CommitOutcome::Committed(_))));
}

fn replay(table: &Table, worker: &mut WorkerContext, key: &[u8], value: Option<&[u8]>, source: DistributedTimeTerm) {
    let mut txn = worker.begin().unwrap();
    assert!(table.stage_replay_inner(&mut txn, None, key, value.map(Value::from), source).unwrap());
    commit(txn);
}

fn read(table: &Table, worker: &mut WorkerContext, key: &[u8], policy: ReadVisibility) -> VersionedRead {
    let mut txn = worker.begin().unwrap();
    let result = table.get_versioned_inner(&mut txn, None, key, policy).unwrap();
    commit(txn);
    result
}

#[test]
fn bootstrap_keeps_inline_storage_and_has_no_fabricated_dependency() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        table.put_presence_inner(&mut txn, None, b"key", Value::from(&b"bootstrap"[..])).unwrap();
        commit(txn);
        let id = table.shared().lookup(None, b"key").unwrap().unwrap();
        let access = table.shared().resolve_directory_access(id).unwrap();
        assert!(access.record.state.shared.load_full().is_none());
        assert_eq!(read(&table, &mut worker, b"key", ReadVisibility::latest(decision(1, 0).term())), VersionedRead {
            value: Some(Value::from(&b"bootstrap"[..])), metadata: None,
        });
        replay(&table, &mut worker, b"key", Some(b"new"), decision(20, 0));
        let mut policy = ReadVisibility::latest(decision(1, 1).term());
        policy.stable_through[0] = Some(0);
        assert_eq!(read(&table, &mut worker, b"key", policy), VersionedRead {
            value: Some(Value::from(&b"bootstrap"[..])), metadata: None,
        });
    }
}

#[test]
fn selected_history_payload_and_metadata_agree_across_tombstones() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        replay(&table, &mut worker, b"key", Some(b"stable"), decision(10, 0));
        replay(&table, &mut worker, b"key", None, decision(20, 0));
        replay(&table, &mut worker, b"key", Some(b"speculative"), decision(30, 0));
        let mut policy = ReadVisibility::latest(decision(1, 1).term());
        policy.stable_through[0] = Some(10);
        let selected = read(&table, &mut worker, b"key", policy);
        assert_eq!(selected.value.unwrap().as_ref(), b"stable");
        assert_eq!(selected.metadata, Some(decision(10, 0)));
        policy.stable_through[0] = Some(20);
        assert_eq!(read(&table, &mut worker, b"key", policy), VersionedRead { value: None, metadata: Some(decision(20, 0)) });
        policy.stable_through[0] = Some(0);
        assert_eq!(read(&table, &mut worker, b"key", policy), VersionedRead { value: None, metadata: None });
        // The current term reads the newest value despite its stability bound.
        policy.current_term = decision(1, 0).term();
        assert_eq!(read(&table, &mut worker, b"key", policy).metadata, Some(decision(30, 0)));
    }
}

#[test]
fn scan_selects_before_filtering_and_tracks_tombstone_dependencies() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        replay(&table, &mut worker, b"a", Some(b"old-a"), decision(10, 0));
        replay(&table, &mut worker, b"b", Some(b"old-b"), decision(11, 0));
        replay(&table, &mut worker, b"a", None, decision(20, 0));
        let mut policy = ReadVisibility::latest(decision(1, 1).term());
        policy.stable_through[0] = Some(15);
        let mut txn = worker.begin().unwrap();
        let result = table.scan_versioned_inner(&mut txn, None, ScanRequest::new(ScanDirection::Forward, 1), policy).unwrap();
        assert_eq!(result.rows.len(), 1);
        assert_eq!(result.rows[0].key.as_ref(), b"a");
        assert_eq!(result.rows[0].value.as_ref(), b"old-a");
        assert_eq!(result.rows[0].metadata, Some(decision(10, 0)));
        assert_eq!(result.dependency, Some(decision(10, 0).timestamp()));
        commit(txn);
        policy.stable_through[0] = Some(20);
        let mut txn = worker.begin().unwrap();
        let result = table.scan_versioned_inner(&mut txn, None, ScanRequest::new(ScanDirection::Forward, 1), policy).unwrap();
        assert_eq!(result.rows[0].key.as_ref(), b"b");
        assert_eq!(result.dependency, Some(decision(20, 0).timestamp()));
        commit(txn);
        let mut txn = worker.begin().unwrap();
        let result = table.scan_versioned_inner(&mut txn, None, ScanRequest::new(ScanDirection::Reverse, 1), policy).unwrap();
        assert_eq!(result.rows[0].key.as_ref(), b"b");
        assert_eq!(result.dependency, Some(decision(11, 0).timestamp()));
        commit(txn);
    }
}

#[test]
fn replay_compares_source_metadata_and_abort_preserves_all_versions() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        replay(&table, &mut worker, b"key", Some(b"original"), decision(20, 1));
        let mut txn = worker.begin().unwrap();
        assert!(!table.stage_replay_inner(&mut txn, None, b"key", Some(Value::from(&b"duplicate"[..])), decision(20, 1)).unwrap());
        assert!(!table.stage_replay_inner(&mut txn, None, b"key", None, decision(19, 0)).unwrap());
        commit(txn);
        let mut txn = worker.begin().unwrap();
        assert!(table.stage_replay_inner(&mut txn, None, b"key", None, decision(21, 1)).unwrap());
        assert!(table.stage_replay_inner(&mut txn, None, b"key", Some(Value::from(&b"staged"[..])), decision(22, 1)).unwrap());
        assert!(!table.stage_replay_inner(&mut txn, None, b"key", None, decision(21, 1)).unwrap());
        assert_eq!(table.get_versioned_inner(&mut txn, None, b"key", ReadVisibility::latest(decision(1, 1).term())).unwrap().metadata, Some(decision(22, 1)));
        txn.abort();
        let selected = read(&table, &mut worker, b"key", ReadVisibility::latest(decision(1, 1).term()));
        assert_eq!(selected.metadata, Some(decision(20, 1)));
        assert_eq!(selected.value.unwrap().as_ref(), b"original");
        // Legacy replay is an OR, not lexicographic ordering.
        replay(&table, &mut worker, b"key", Some(b"new-term"), decision(5, 2));
        replay(&table, &mut worker, b"key", None, decision(25, 1));
        assert_eq!(read(&table, &mut worker, b"key", ReadVisibility::latest(decision(1, 1).term())).metadata, Some(decision(25, 1)));
    }
}

#[test]
fn stale_replay_comparison_and_unsafe_failure_reads_abort() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut first = runtime.attach().unwrap();
        let mut second = runtime.attach().unwrap();
        replay(&table, &mut first, b"key", Some(b"first"), decision(10, 0));
        let mut stale = first.begin().unwrap();
        assert!(table.stage_replay_inner(&mut stale, None, b"key", Some(Value::from(&b"stale"[..])), decision(20, 0)).unwrap());
        replay(&table, &mut second, b"key", Some(b"winner"), decision(30, 0));
        assert!(matches!(stale.commit(), Ok(CommitOutcome::Aborted(_))));
        let mut policy = ReadVisibility::latest(decision(1, 0).term());
        policy.failed_shard_through = Some(0);
        let mut txn = first.begin().unwrap();
        assert!(table.get_versioned_inner(&mut txn, None, b"key", policy).is_err());
        txn.abort();
        assert_eq!(read(&table, &mut first, b"key", ReadVisibility::latest(decision(1, 0).term())).metadata, Some(decision(30, 0)));
    }
}

#[test]
fn replay_last_valid_base_does_not_allocate_a_replacement_timestamp() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        let last = DistributedTimeTerm::new(DistributedTimestamp::MAX, DistributedTerm::new(9).unwrap());
        replay(&table, &mut worker, b"last", Some(b"last"), last);
        assert_eq!(read(&table, &mut worker, b"last", ReadVisibility::latest(last.term())).metadata, Some(last));
    }
}

#[test]
fn participant_repeated_locked_writes_publish_one_decision_and_hold_until_finish() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut owner = runtime.attach().unwrap();
        let mut competitor = runtime.attach().unwrap();
        replay(&table, &mut owner, b"a", Some(b"before"), decision(10, 0));
        let mut participant = owner.begin().unwrap().into_participant();
        participant.with_transaction(|txn| {
            table.put_presence_inner(txn, None, b"a", Value::from(&b"first"[..]))?;
            Ok(())
        }).unwrap();
        participant.lock_writes().unwrap();
        participant.with_transaction(|txn| {
            table.put_presence_inner(txn, None, b"a", Value::from(&b"final"[..]))?;
            table.put_presence_inner(txn, None, b"b", Value::from(&b"insert"[..]))?;
            let selected = table.get_versioned_inner(txn, None, b"a", ReadVisibility::latest(decision(1, 0).term()))?;
            assert_eq!(selected.value.unwrap().as_ref(), b"final");
            assert_eq!(selected.metadata, None);
            Ok(())
        }).unwrap();
        participant.lock_writes().unwrap();
        participant.validate().unwrap();
        participant.install(decision(50, 0)).unwrap();
        let mut blocked = competitor.begin().unwrap();
        assert!(table.get_versioned_inner(&mut blocked, None, b"a", ReadVisibility::latest(decision(1, 0).term())).is_err());
        blocked.abort();
        let mut blocked = competitor.begin().unwrap();
        assert!(table.put_presence_inner(&mut blocked, None, b"b", Value::from(&b"racer"[..])).is_err());
        blocked.abort();
        // Abort after INSTALL is committed cleanup, never rollback.
        assert!(matches!(participant.finish(false), Ok(CommitOutcome::Committed(_))));
        for (key, expected) in [(b"a".as_slice(), b"final".as_slice()), (b"b".as_slice(), b"insert".as_slice())] {
            let selected = read(&table, &mut owner, key, ReadVisibility::latest(decision(1, 0).term()));
            assert_eq!(selected.value.unwrap().as_ref(), expected);
            assert_eq!(selected.metadata, Some(decision(50, 0)));
        }
    }
}

#[test]
fn partial_participant_lock_failure_preserves_payloads_and_history() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut owner = runtime.attach().unwrap();
        let mut competitor = runtime.attach().unwrap();
        replay(&table, &mut owner, b"a", Some(b"old-a"), decision(10, 0));
        replay(&table, &mut owner, b"b", Some(b"old-b"), decision(11, 0));
        let mut participant = owner.begin().unwrap().into_participant();
        participant.with_transaction(|txn| {
            table.put_presence_inner(txn, None, b"a", Value::from(&b"never-a"[..]))?;
            Ok(())
        }).unwrap();
        participant.lock_writes().unwrap();
        participant.with_transaction(|txn| {
            table.put_presence_inner(txn, None, b"b", Value::from(&b"never-b"[..]))?;
            Ok(())
        }).unwrap();
        let mut blocker = competitor.begin().unwrap().into_participant();
        blocker.with_transaction(|txn| {
            table.put_presence_inner(txn, None, b"b", Value::from(&b"also-never"[..]))?;
            Ok(())
        }).unwrap();
        blocker.lock_writes().unwrap();
        assert!(participant.lock_writes().is_err());
        assert!(matches!(participant.finish(false), Ok(CommitOutcome::Aborted(_))));
        assert!(matches!(blocker.finish(false), Ok(CommitOutcome::Aborted(_))));
        for (key, expected, source) in [
            (b"a".as_slice(), b"old-a".as_slice(), decision(10, 0)),
            (b"b".as_slice(), b"old-b".as_slice(), decision(11, 0)),
        ] {
            let selected = read(&table, &mut owner, key, ReadVisibility::latest(source.term()));
            assert_eq!(selected.value.unwrap().as_ref(), expected);
            assert_eq!(selected.metadata, Some(source));
        }
    }
}

fn history(table: &Table, key: &[u8]) -> Arc<SharedValue> {
    let id = table.shared().lookup(None, key).unwrap().unwrap();
    table.shared().resolve_directory_access(id).unwrap().record.state.shared.load_full().unwrap()
}

fn history_decisions(head: &Arc<SharedValue>) -> Vec<Option<DistributedTimeTerm>> {
    let mut result = Vec::new();
    let mut cursor = Some(head.as_ref());
    while let Some(SharedValue::Distributed(version)) = cursor {
        result.push(version.metadata);
        cursor = version.previous.as_deref();
    }
    result
}

#[test]
fn retention_floor_cuts_strictly_older_tail_without_mutating_pinned_snapshots() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        replay(&table, &mut worker, b"key", Some(b"ten"), decision(10, 0));
        replay(&table, &mut worker, b"key", None, decision(20, 0));
        replay(&table, &mut worker, b"key", Some(b"thirty"), decision(30, 0));
        let pinned = history(&table, b"key");
        let mut txn = worker.begin().unwrap();
        let owned_payload = table.get_inner(&mut txn, None, b"key").unwrap().unwrap();
        commit(txn);
        assert_eq!(history_decisions(&pinned), [Some(decision(30, 0)), Some(decision(20, 0)), Some(decision(10, 0)), None]);

        table.advance_history_retention_floor(decision(20, 0).timestamp());
        let mut aborted = worker.begin().unwrap();
        table.stage_replay_inner(&mut aborted, None, b"key", Some(Value::from(&b"aborted"[..])), decision(40, 0)).unwrap();
        aborted.abort();
        assert!(Arc::ptr_eq(&pinned, &history(&table, b"key")));
        replay(&table, &mut worker, b"key", Some(b"forty"), decision(40, 0));
        let pruned = history(&table, b"key");
        assert_eq!(history_decisions(&pruned), [Some(decision(40, 0)), Some(decision(30, 0)), Some(decision(20, 0))]);
        assert_eq!(history_decisions(&pinned), [Some(decision(30, 0)), Some(decision(20, 0)), Some(decision(10, 0)), None]);
        assert_eq!(owned_payload.as_ref(), b"thirty");
        let mut visibility = ReadVisibility::latest(decision(1, 1).term());
        visibility.stable_through[0] = Some(20);
        assert_eq!(read(&table, &mut worker, b"key", visibility), VersionedRead {
            value: None, metadata: Some(decision(20, 0)),
        });
        visibility.stable_through[0] = Some(19);
        let mut too_old = worker.begin().unwrap();
        assert!(table.get_versioned_inner(&mut too_old, None, b"key", visibility).is_err());
        too_old.abort();

        // An old authority snapshot cannot resurrect discarded history.
        table.advance_history_retention_floor(decision(10, 0).timestamp());
        replay(&table, &mut worker, b"key", Some(b"fifty"), decision(50, 0));
        let reused = history(&table, b"key");
        let SharedValue::Distributed(version) = reused.as_ref() else { panic!("history missing") };
        assert!(Arc::ptr_eq(version.previous.as_ref().unwrap(), &pruned));
        table.advance_history_retention_floor(decision(35, 0).timestamp());
        replay(&table, &mut worker, b"key", Some(b"sixty"), decision(60, 0));
        assert_eq!(history_decisions(&history(&table, b"key")), [Some(decision(60, 0)), Some(decision(50, 0)), Some(decision(40, 0))]);
    }
}

#[test]
fn retention_bounds_advancing_history_and_cuts_nonmonotone_replay_at_first_old_node() {
    for direct in [false, true] {
        let (runtime, table) = table(direct);
        let mut worker = runtime.attach().unwrap();
        for base in 1..=128 {
            if base > 2 {
                table.advance_history_retention_floor(decision(base - 2, 0).timestamp());
            }
            replay(&table, &mut worker, b"bounded", Some(b"value"), decision(base, 0));
            assert!(history_decisions(&history(&table, b"bounded")).len() <= 3);
        }
        let source = decision(200, 0);
        replay(&table, &mut worker, b"nonmonotone", Some(b"high"), source);
        table.advance_history_retention_floor(decision(150, 0).timestamp());
        // A higher term can replay a lower base. The incoming head is always
        // retained, even below the floor; only its previous chain is pruned.
        replay(&table, &mut worker, b"nonmonotone", Some(b"low"), decision(100, 1));
        assert_eq!(history_decisions(&history(&table, b"nonmonotone")), [Some(decision(100, 1)), Some(source)]);
        replay(&table, &mut worker, b"nonmonotone", Some(b"next"), decision(160, 1));
        assert_eq!(history_decisions(&history(&table, b"nonmonotone")), [Some(decision(160, 1))]);
    }
}
