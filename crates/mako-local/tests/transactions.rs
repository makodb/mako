#![cfg(have_mako)]

use std::sync::{mpsc, Arc, Barrier};
use std::time::Duration;

use mako_local::{
    advance_mako_timestamp_past, features, CommitDisposition, Error, LocalDb, MakoTimestamp,
    MAX_MAKO_TIMESTAMP, TRANSACTION_ITEM_BUDGET,
};

#[test]
fn native_features_include_point_transactions() {
    let features = features().unwrap();
    assert!(features.point_transactions());
    assert!(!features.scan_read_my_writes() || features.transactional_scans());
    assert!(!features.scan_read_my_writes() || features.read_my_writes());
}

#[test]
fn transactional_scans_have_symmetric_binary_bounds_and_staged_visibility() {
    if !features().unwrap().scan_read_my_writes() {
        return;
    }
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_transactional_scan", 20_018).unwrap();

    let mut seed = db.transaction().unwrap();
    for (key, value) in [
        (&b""[..], &b"empty"[..]),
        (&b"a"[..], &b"old-a"[..]),
        (&b"a\0"[..], &b"nul"[..]),
        (&b"b"[..], &b"old-b"[..]),
        (&b"c"[..], &b"old-c"[..]),
        (&b"\xff"[..], &b"high"[..]),
    ] {
        seed.put(&table, key, value).unwrap();
    }
    seed.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert!(!tx.put(&table, b"a", b"new-a").unwrap());
    assert!(tx.remove(&table, b"a\0").unwrap());
    assert!(tx.remove(&table, b"b").unwrap());
    assert!(tx.put(&table, b"b", b"reborn-b").unwrap());
    assert!(tx.insert(&table, b"ab", b"new-ab").unwrap());
    assert!(tx.insert(&table, b"az", b"new-az").unwrap());

    let forward: Vec<_> = tx
        .scan(&table, b"a", Some(b"c"))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(
        forward,
        vec![
            (b"a".to_vec(), b"new-a".to_vec()),
            (b"ab".to_vec(), b"new-ab".to_vec()),
            (b"az".to_vec(), b"new-az".to_vec()),
            (b"b".to_vec(), b"reborn-b".to_vec()),
        ]
    );
    let reverse: Vec<_> = tx
        .rscan(&table, b"a", Some(b"c"))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(reverse, forward.iter().cloned().rev().collect::<Vec<_>>());
    tx.commit().unwrap();

    let mut verify = db.transaction().unwrap();
    let all: Vec<_> = verify
        .scan(&table, b"", None)
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(all.first().unwrap().0, b"");
    assert_eq!(all.last().unwrap().0, b"\xff");
    assert!(!all.iter().any(|(key, _)| key == b"a\0"));
    verify.commit().unwrap();
}

#[test]
fn transactional_scan_chunks_resume_and_grow_the_arena_without_gaps() {
    if !features().unwrap().scan_read_my_writes() {
        return;
    }
    let db = LocalDb::open().unwrap();
    let table = db
        .open_table("rust_transactional_scan_chunks", 20_019)
        .unwrap();
    let large = vec![b'x'; 16 * 1024];

    for batch in 0..2 {
        let mut seed = db.transaction().unwrap();
        for index in batch * 64..(batch + 1) * 64 {
            let key = format!("k{index:03}");
            let value = if index == 64 {
                large.as_slice()
            } else {
                key.as_bytes()
            };
            seed.put(&table, key.as_bytes(), value).unwrap();
        }
        seed.commit().unwrap();
    }

    let mut forward_tx = db.transaction().unwrap();
    let forward: Vec<_> = forward_tx
        .scan(&table, b"k008", Some(b"k120"))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(forward.len(), 112);
    assert_eq!(forward.first().unwrap().0, b"k008");
    assert_eq!(forward.last().unwrap().0, b"k119");
    assert_eq!(forward.iter().filter(|(key, _)| key == b"k064").count(), 1);
    assert_eq!(
        forward.iter().find(|(key, _)| key == b"k064").unwrap().1,
        large
    );
    assert!(forward.windows(2).all(|pair| pair[0].0 < pair[1].0));
    forward_tx.commit().unwrap();

    let mut reverse_tx = db.transaction().unwrap();
    let reverse: Vec<_> = reverse_tx
        .rscan(&table, b"k008", Some(b"k120"))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(reverse, forward.into_iter().rev().collect::<Vec<_>>());
    reverse_tx.commit().unwrap();
}

#[test]
fn unsupported_scan_profile_is_typed_and_leaves_transaction_active() {
    if features().unwrap().scan_read_my_writes() {
        return;
    }
    let db = LocalDb::open().unwrap();
    let table = db
        .open_table("rust_transactional_scan_unavailable", 20_020)
        .unwrap();
    let mut tx = db.transaction().unwrap();
    assert!(matches!(
        tx.scan(&table, b"", None),
        Err(Error::TransactionalScansUnavailable)
    ));
    tx.put(&table, b"point-path", b"still-active").unwrap();
    tx.commit().unwrap();
}

#[test]
fn empty_scans_still_validate_table_identity_and_leave_transaction_active() {
    if !features().unwrap().scan_read_my_writes() {
        return;
    }
    let left = LocalDb::open().unwrap();
    let right = LocalDb::open().unwrap();
    let left_table = left.open_table("rust_empty_scan_left", 20_022).unwrap();
    let wrong_table = right.open_table("rust_empty_scan_right", 20_023).unwrap();
    let mut transaction = left.transaction().unwrap();

    let forward = transaction
        .scan(&wrong_table, b"z", Some(b"a"))
        .unwrap()
        .collect::<Result<Vec<_>, _>>();
    assert_eq!(forward, Err(Error::WrongDatabaseOrTable));
    let reverse = transaction
        .rscan(&wrong_table, b"same", Some(b"same"))
        .unwrap()
        .collect::<Result<Vec<_>, _>>();
    assert_eq!(reverse, Err(Error::WrongDatabaseOrTable));

    transaction
        .put(&left_table, b"still-active", b"value")
        .unwrap();
    transaction.commit().unwrap();
}

#[test]
fn transactional_scan_predicate_conflicts_with_a_concurrent_insert() {
    if !features().unwrap().scan_read_my_writes() {
        return;
    }
    let db = Arc::new(LocalDb::open().unwrap());
    let _ = db
        .open_table("rust_transactional_scan_phantom", 20_021)
        .unwrap();
    let (scanned_tx, scanned_rx) = mpsc::sync_channel(1);
    let (release_tx, release_rx) = mpsc::sync_channel(1);

    let reader_db = Arc::clone(&db);
    let reader = std::thread::spawn(move || {
        let table = reader_db
            .open_table("rust_transactional_scan_phantom", 20_021)
            .unwrap();
        let mut transaction = reader_db.transaction().unwrap();
        let rows: Vec<_> = transaction
            .scan(&table, b"phantom/", Some(b"phantom0"))
            .unwrap()
            .collect::<Result<_, _>>()
            .unwrap();
        assert!(rows.is_empty());
        scanned_tx.send(()).unwrap();
        release_rx.recv_timeout(Duration::from_secs(10)).unwrap();
        transaction.commit()
    });

    scanned_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    let table = db
        .open_table("rust_transactional_scan_phantom", 20_021)
        .unwrap();
    let mut writer = db.transaction().unwrap();
    writer.put(&table, b"phantom/new", b"value").unwrap();
    writer.commit().unwrap();
    release_tx.send(()).unwrap();

    assert_eq!(reader.join().unwrap(), Err(Error::Conflict));
}

#[test]
fn point_reads_observe_own_put_insert_and_remove() {
    if !features().unwrap().read_my_writes() {
        return;
    }
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_point_ryw", 20_014).unwrap();

    let mut seed = db.transaction().unwrap();
    assert!(seed.put(&table, b"existing", b"old").unwrap());
    assert!(seed.put(&table, b"doomed", b"present").unwrap());
    seed.commit().unwrap();

    // Growing an existing value exercises MassTrans's relocation path. The
    // staged bytes must be copied from the transaction item, not reread from
    // the still-committed physical row.
    let large = vec![b'x'; 8 * 1024];
    let mut tx = db.transaction().unwrap();
    assert_eq!(
        tx.get(&table, b"existing").unwrap().as_deref(),
        Some(&b"old"[..])
    );
    assert_eq!(
        tx.get(&table, b"existing").unwrap().as_deref(),
        Some(&b"old"[..])
    );
    assert!(!tx.put(&table, b"existing", &large).unwrap());
    assert_eq!(tx.get(&table, b"existing").unwrap(), Some(large.clone()));

    assert!(tx.insert(&table, b"inserted", b"\0new\xff").unwrap());
    assert_eq!(
        tx.get(&table, b"inserted").unwrap().as_deref(),
        Some(&b"\0new\xff"[..])
    );

    assert!(tx.remove(&table, b"doomed").unwrap());
    assert_eq!(tx.get(&table, b"doomed").unwrap(), None);
    tx.commit().unwrap();

    let mut verify = db.transaction().unwrap();
    assert_eq!(
        verify.get(&table, b"existing").unwrap(),
        Some(large.clone())
    );
    assert_eq!(
        verify.get(&table, b"inserted").unwrap().as_deref(),
        Some(&b"\0new\xff"[..])
    );
    assert_eq!(verify.get(&table, b"doomed").unwrap(), None);
    verify.commit().unwrap();

    let mut abort = db.transaction().unwrap();
    assert!(!abort.put(&table, b"existing", b"aborted").unwrap());
    assert_eq!(
        abort.get(&table, b"existing").unwrap().as_deref(),
        Some(&b"aborted"[..])
    );
    abort.abort().unwrap();

    let mut after_abort = db.transaction().unwrap();
    assert_eq!(after_abort.get(&table, b"existing").unwrap(), Some(large));
    after_abort.commit().unwrap();
}

#[test]
fn explicit_close_consumes_the_facade_without_a_drop_retry() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_explicit_close", 20_009).unwrap();
    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"key", b"value").unwrap();
    tx.commit().unwrap();

    db.close().unwrap();
}

#[test]
fn detailed_commit_reports_visibility_before_cleanup() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_commit_report", 20_010).unwrap();
    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"key", b"value").unwrap();

    let report = tx.commit_report();
    assert_eq!(report.disposition, CommitDisposition::Committed);
    assert_eq!(report.cleanup, Ok(()));
}

#[test]
fn post_validation_hook_carries_mako_timestamp_and_can_reject_safely() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_commit_hook", 20_011).unwrap();
    assert_eq!(
        advance_mako_timestamp_past(MakoTimestamp::new(MAX_MAKO_TIMESTAMP).unwrap()),
        Err(Error::TimestampExhausted)
    );
    let recovered_max = MakoTimestamp::new(1_u32 << 24).unwrap();
    advance_mako_timestamp_past(recovered_max).unwrap();

    let mut seen = None;
    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"accepted", b"visible").unwrap();
    let report = tx.commit_report_with_hook(|timestamp| {
        seen = Some(timestamp);
        true
    });
    assert_eq!(report.disposition, CommitDisposition::Committed);
    assert_eq!(report.cleanup, Ok(()));
    assert!(seen.is_some_and(|timestamp| timestamp > recovered_max));

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"rejected", b"invisible").unwrap();
    let report = tx.commit_report_with_hook(|timestamp| {
        assert!(timestamp > recovered_max);
        false
    });
    assert_eq!(
        report.disposition,
        CommitDisposition::Aborted(Error::CommitHookRejected)
    );
    assert_eq!(report.cleanup, Ok(()));

    let mut tx = db.transaction().unwrap();
    assert_eq!(
        tx.get(&table, b"accepted").unwrap().as_deref(),
        Some(&b"visible"[..])
    );
    assert_eq!(tx.get(&table, b"rejected").unwrap(), None);
    tx.commit().unwrap();
}

#[test]
fn unwinding_post_validation_hook_is_a_definite_abort() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_panicking_commit_hook", 20_012).unwrap();

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"panic", b"invisible").unwrap();
    let report = tx.commit_report_with_hook(|_| panic!("contained hook panic"));
    assert_eq!(
        report.disposition,
        CommitDisposition::Aborted(Error::CommitHookRejected)
    );
    assert_eq!(report.cleanup, Ok(()));

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"panic").unwrap(), None);
    tx.put(&table, b"worker", b"reusable").unwrap();
    tx.commit().unwrap();
}

#[test]
fn disjoint_post_validation_hooks_can_overlap() {
    const ENTRY_TIMEOUT: Duration = Duration::from_secs(5);
    const RELEASE_TIMEOUT: Duration = Duration::from_secs(10);

    let db = Arc::new(LocalDb::open().unwrap());
    let _ = db.open_table("rust_disjoint_commit_hooks", 20_013).unwrap();

    // The bounded entry channel cannot allocate or wait for capacity in either
    // hook. Each hook intentionally waits on its own release channel: the test
    // sends neither release until it has observed both entries (or timed out),
    // so two entries prove that native commit has no global serialization gate.
    let (entered_tx, entered_rx) = mpsc::sync_channel(2);
    let (release_0_tx, release_0_rx) = mpsc::sync_channel(1);
    let (release_1_tx, release_1_rx) = mpsc::sync_channel(1);
    let mut workers = Vec::new();

    for (worker_id, release_rx) in [release_0_rx, release_1_rx].into_iter().enumerate() {
        let db = Arc::clone(&db);
        let entered_tx = entered_tx.clone();
        workers.push(std::thread::spawn(move || {
            let table = db.open_table("rust_disjoint_commit_hooks", 20_013).unwrap();
            let mut tx = db.transaction().unwrap();
            tx.put(
                &table,
                if worker_id == 0 { b"left" } else { b"right" },
                b"visible",
            )
            .unwrap();
            let report = tx.commit_report_with_hook(move |_| {
                entered_tx.send(worker_id).is_ok()
                    && release_rx.recv_timeout(RELEASE_TIMEOUT).is_ok()
            });
            (worker_id, report)
        }));
    }
    drop(entered_tx);

    let first_entry = entered_rx.recv_timeout(ENTRY_TIMEOUT);
    let second_entry = entered_rx.recv_timeout(ENTRY_TIMEOUT);

    // Always release and join before asserting. If a global gate regresses,
    // this lets the first commit finish and the second hook eventually exit
    // instead of leaving a test worker hung behind the failed assertion.
    let _ = release_0_tx.send(());
    let _ = release_1_tx.send(());
    let outcomes: Vec<_> = workers.into_iter().map(|worker| worker.join()).collect();

    let first_entry = first_entry.expect("first commit hook did not run before the timeout");
    let second_entry =
        second_entry.expect("second disjoint commit was globally serialized behind the first hook");
    assert_ne!(first_entry, second_entry);

    for outcome in outcomes {
        let (_, report) = outcome.expect("commit worker panicked");
        assert_eq!(report.disposition, CommitDisposition::Committed);
        assert_eq!(report.cleanup, Ok(()));
    }
}

#[test]
fn multi_key_multi_table_commit_round_trips() {
    let db = LocalDb::open().unwrap();
    let accounts = db.open_table("rust_accounts", 20_001).unwrap();
    let audit = db.open_table("rust_audit", 20_002).unwrap();

    let mut tx = db.transaction().unwrap();
    assert!(tx.put(&accounts, b"alice", b"10").unwrap());
    assert!(tx.put(&accounts, b"bob", b"20").unwrap());
    assert!(tx.put(&audit, b"entry", b"created").unwrap());
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(
        tx.get(&accounts, b"alice").unwrap().as_deref(),
        Some(&b"10"[..])
    );
    assert_eq!(
        tx.get(&accounts, b"bob").unwrap().as_deref(),
        Some(&b"20"[..])
    );
    assert_eq!(
        tx.get(&audit, b"entry").unwrap().as_deref(),
        Some(&b"created"[..])
    );
    tx.commit().unwrap();
}

#[test]
fn drop_aborts_and_distinguishes_missing_from_empty() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_drop", 20_003).unwrap();

    {
        let mut tx = db.transaction().unwrap();
        tx.put(&table, b"rolled-back", b"value").unwrap();
        // Drop is the rollback.
    }

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"rolled-back").unwrap(), None);
    tx.put(&table, b"empty", b"").unwrap();
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"missing").unwrap(), None);
    assert_eq!(tx.get(&table, b"empty").unwrap(), Some(Vec::new()));
    tx.commit().unwrap();
}

#[test]
fn binary_values_and_distinct_staged_buffers_survive_commit() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_binary", 20_004).unwrap();
    let long = vec![b'z'; 4096];

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"k\0y", b"old-binary").unwrap();
    tx.put(&table, b"short", b"old-short").unwrap();
    tx.put(&table, b"long", b"old-long").unwrap();
    tx.put(&table, b"third", b"old-third").unwrap();
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"k\0y", b"v\0x\xff").unwrap();
    tx.put(&table, b"short", b"first").unwrap();
    tx.put(&table, b"long", &long).unwrap();
    tx.put(&table, b"third", b"different").unwrap();
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(
        tx.get(&table, b"k\0y").unwrap().as_deref(),
        Some(&b"v\0x\xff"[..])
    );
    assert_eq!(
        tx.get(&table, b"short").unwrap().as_deref(),
        Some(&b"first"[..])
    );
    assert_eq!(
        tx.get(&table, b"long").unwrap().as_deref(),
        Some(long.as_slice())
    );
    assert_eq!(
        tx.get(&table, b"third").unwrap().as_deref(),
        Some(&b"different"[..])
    );
    tx.commit().unwrap();
}

#[test]
fn insert_remove_and_nested_begin_have_typed_results() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_verbs", 20_005).unwrap();

    let mut tx = db.transaction().unwrap();
    assert!(matches!(
        db.transaction(),
        Err(Error::TransactionAlreadyActive)
    ));
    assert!(tx.insert(&table, b"key", b"one").unwrap());
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert!(!tx.insert(&table, b"key", b"ignored").unwrap());
    assert!(tx.remove(&table, b"key").unwrap());
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"key").unwrap(), None);
    tx.commit().unwrap();
}

#[test]
fn every_same_key_mutation_pair_composes_in_transaction_order() {
    if !features().unwrap().read_my_writes() {
        return;
    }

    #[derive(Clone, Copy, Debug)]
    enum Mutation {
        Put,
        Insert,
        Remove,
    }

    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_same_key_pairs", 20_007).unwrap();
    let mutations = [Mutation::Put, Mutation::Insert, Mutation::Remove];
    let mut sequence = 0usize;

    for initially_present in [false, true] {
        for first in mutations {
            for second in mutations {
                let key = format!("same-key-pair-{sequence}").into_bytes();
                sequence += 1;

                if initially_present {
                    let mut seed = db.transaction().unwrap();
                    assert!(seed.put(&table, &key, b"seed").unwrap());
                    seed.commit().unwrap();
                }

                let mut expected = initially_present.then(|| b"seed".to_vec());
                let mut tx = db.transaction().unwrap();
                for (mutation, value) in [(first, &b"first"[..]), (second, &b"second"[..])] {
                    let was_present = expected.is_some();
                    let changed = match mutation {
                        Mutation::Put => {
                            let changed = tx.put(&table, &key, value).unwrap();
                            expected = Some(value.to_vec());
                            changed
                        }
                        Mutation::Insert => {
                            let changed = tx.insert(&table, &key, value).unwrap();
                            if !was_present {
                                expected = Some(value.to_vec());
                            }
                            changed
                        }
                        Mutation::Remove => {
                            let changed = tx.remove(&table, &key).unwrap();
                            expected = None;
                            changed
                        }
                    };
                    let expected_changed = match mutation {
                        Mutation::Put | Mutation::Insert => !was_present,
                        Mutation::Remove => was_present,
                    };
                    assert_eq!(
                        changed, expected_changed,
                        "initially_present={initially_present}, first={first:?}, second={second:?}, operation={mutation:?}"
                    );
                    assert_eq!(
                        tx.get(&table, &key).unwrap(),
                        expected,
                        "initially_present={initially_present}, first={first:?}, second={second:?}, operation={mutation:?}"
                    );
                }
                tx.commit().unwrap();

                let mut verify = db.transaction().unwrap();
                assert_eq!(verify.get(&table, &key).unwrap(), expected);
                verify.commit().unwrap();
            }
        }
    }
}

#[test]
fn same_key_growth_delete_reinsert_chain_commits_and_aborts() {
    if !features().unwrap().read_my_writes() {
        return;
    }

    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_same_key_chain", 20_015).unwrap();
    let medium = vec![b'm'; 4 * 1024];
    let large = vec![b'l'; 32 * 1024];

    let mut seed = db.transaction().unwrap();
    assert!(seed.put(&table, b"key", b"seed").unwrap());
    seed.commit().unwrap();

    let mut abort = db.transaction().unwrap();
    assert!(!abort.put(&table, b"key", b"short").unwrap());
    assert!(!abort.put(&table, b"key", &medium).unwrap());
    assert_eq!(abort.get(&table, b"key").unwrap(), Some(medium));
    assert!(!abort.put(&table, b"key", &large).unwrap());
    assert_eq!(abort.get(&table, b"key").unwrap(), Some(large.clone()));
    assert!(abort.remove(&table, b"key").unwrap());
    assert_eq!(abort.get(&table, b"key").unwrap(), None);
    assert!(abort.insert(&table, b"key", b"final\0\xff").unwrap());
    assert_eq!(
        abort.get(&table, b"key").unwrap().as_deref(),
        Some(&b"final\0\xff"[..])
    );
    abort.abort().unwrap();

    let mut after_abort = db.transaction().unwrap();
    assert_eq!(
        after_abort.get(&table, b"key").unwrap().as_deref(),
        Some(&b"seed"[..])
    );
    after_abort.commit().unwrap();

    let mut commit = db.transaction().unwrap();
    assert!(commit.remove(&table, b"key").unwrap());
    assert!(commit.put(&table, b"key", b"recreated").unwrap());
    assert!(!commit.insert(&table, b"key", b"ignored").unwrap());
    assert!(!commit.put(&table, b"key", &large).unwrap());
    assert_eq!(commit.get(&table, b"key").unwrap(), Some(large.clone()));
    commit.commit().unwrap();

    let mut verify = db.transaction().unwrap();
    assert_eq!(verify.get(&table, b"key").unwrap(), Some(large));
    verify.commit().unwrap();
}

#[test]
fn repeated_same_key_writes_remain_charged_to_the_item_budget() {
    if !features().unwrap().read_my_writes() {
        return;
    }

    const KEY_LEN: usize = 1;
    const WRITE_CHARGE: usize = 4 + KEY_LEN.div_ceil(8);
    const ACCEPTED: usize = TRANSACTION_ITEM_BUDGET / WRITE_CHARGE;

    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_same_key_budget", 20_016).unwrap();
    let mut tx = db.transaction().unwrap();
    for i in 0..ACCEPTED {
        let value = [b'a' + (i % 26) as u8];
        assert_eq!(tx.put(&table, b"b", &value).unwrap(), i == 0);
    }
    assert_eq!(
        tx.put(&table, b"b", b"over-budget"),
        Err(Error::TransactionTooLarge)
    );
    assert_eq!(
        tx.put(&table, b"b", b"after-terminal"),
        Err(Error::TransactionFinished)
    );
    drop(tx);

    let mut verify = db.transaction().unwrap();
    assert_eq!(verify.get(&table, b"b").unwrap(), None);
    verify.commit().unwrap();
}

#[test]
fn legacy_no_ryw_builds_still_report_duplicate_write() {
    if features().unwrap().read_my_writes() {
        return;
    }

    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_legacy_duplicate", 20_017).unwrap();

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"key", b"first").unwrap();
    assert_eq!(
        tx.put(&table, b"key", b"second"),
        Err(Error::DuplicateWrite)
    );
    tx.commit().unwrap();

    let mut tx = db.transaction().unwrap();
    assert_eq!(
        tx.get(&table, b"key").unwrap().as_deref(),
        Some(&b"first"[..])
    );
    tx.commit().unwrap();
}

#[test]
fn unwind_abort_and_operation_conflict_leave_worker_reusable() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_cleanup", 20_008).unwrap();

    let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut tx = db.transaction().unwrap();
        tx.put(&table, b"panic", b"invisible").unwrap();
        panic!("exercise Transaction::drop during unwind");
    }));
    assert!(unwind.is_err());

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"panic").unwrap(), None);
    tx.abort().unwrap();

    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"explicit-abort", b"invisible").unwrap();
    tx.abort().unwrap();

    if !features().unwrap().read_my_writes() {
        let mut tx = db.transaction().unwrap();
        tx.put(&table, b"own-write", b"value").unwrap();
        assert_eq!(tx.get(&table, b"own-write"), Err(Error::Conflict));
        assert_eq!(
            tx.put(&table, b"after-terminal", b"must-not-run"),
            Err(Error::TransactionFinished)
        );
        assert_eq!(tx.commit(), Err(Error::TransactionFinished));
    }

    let mut tx = db.transaction().unwrap();
    assert_eq!(tx.get(&table, b"explicit-abort").unwrap(), None);
    assert_eq!(tx.get(&table, b"own-write").unwrap(), None);
    tx.put(&table, b"after-conflict", b"works").unwrap();
    tx.commit().unwrap();
}

#[test]
fn deterministic_occ_conflict_aborts_exactly_one() {
    let db = Arc::new(LocalDb::open().unwrap());
    let table = db.open_table("rust_conflict", 20_006).unwrap();
    let mut seed = db.transaction().unwrap();
    seed.put(&table, b"contended", b"0").unwrap();
    seed.commit().unwrap();

    let ready = Arc::new(Barrier::new(2));
    let mut workers = Vec::new();
    for (worker_id, value) in [b"1".as_slice(), b"2".as_slice()].into_iter().enumerate() {
        let db = Arc::clone(&db);
        let ready = Arc::clone(&ready);
        workers.push(std::thread::spawn(move || {
            // Reopen returns the same borrowed native table handle on this DB.
            let table = db.open_table("rust_conflict", 20_006).unwrap();
            let mut tx = db.transaction().unwrap();
            assert_eq!(
                tx.get(&table, b"contended").unwrap().as_deref(),
                Some(&b"0"[..])
            );
            tx.put(&table, b"contended", value).unwrap();
            tx.put(
                &table,
                if worker_id == 0 { b"side-0" } else { b"side-1" },
                if worker_id == 0 { b"left" } else { b"right" },
            )
            .unwrap();
            ready.wait();
            (worker_id, tx.commit())
        }));
    }

    let outcomes: Vec<_> = workers
        .into_iter()
        .map(|worker| worker.join().unwrap())
        .collect();
    assert_eq!(
        outcomes.iter().filter(|(_, result)| result.is_ok()).count(),
        1
    );
    assert_eq!(
        outcomes
            .iter()
            .filter(|(_, result)| matches!(result, Err(Error::Conflict)))
            .count(),
        1
    );

    let winner = outcomes
        .iter()
        .find_map(|(worker_id, result)| result.is_ok().then_some(*worker_id))
        .unwrap();
    let mut verify = db.transaction().unwrap();
    assert_eq!(
        verify.get(&table, b"contended").unwrap().as_deref(),
        Some(if winner == 0 { &b"1"[..] } else { &b"2"[..] })
    );
    assert_eq!(
        verify.get(&table, b"side-0").unwrap().is_some(),
        winner == 0
    );
    assert_eq!(
        verify.get(&table, b"side-1").unwrap().is_some(),
        winner == 1
    );
    verify.commit().unwrap();
}
