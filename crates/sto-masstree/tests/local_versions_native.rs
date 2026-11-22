#![cfg(mtree_native_integration)]

use masstree::{Runtime as NativeRuntime, RuntimeConfig as NativeConfig, Worker};
use std::sync::Arc;
use sto_core::{AbortReason, CommitInfo, CommitOutcome, Conflict, Runtime, RuntimeConfig, TxnCell};
use sto_masstree::{
    PointMutation, PointReadBatch, ScanDirection, ScanRequest, Table, TableConfig,
    TerminalReadVisitOutcome, Value,
};

fn table(
    runtime: &Arc<Runtime>,
    native: &NativeRuntime,
    worker: &Worker,
    direct: bool,
    local: bool,
) -> Table {
    let config = TableConfig::default()
        .with_bounded_atomic_values(true)
        .with_record_local_versions(local);
    if direct {
        Table::new_direct(runtime, native, worker, config).unwrap()
    } else {
        Table::new(runtime, native.create_tree(worker).unwrap(), config).unwrap()
    }
}

fn committed(outcome: CommitOutcome) -> CommitInfo {
    match outcome {
        CommitOutcome::Committed(info) => info,
        other => panic!("unexpected outcome: {other:?}"),
    }
}

#[cfg(feature = "fixed-u64")]
#[test]
fn native_fixed_u64_rejects_record_local_policy_before_directory_creation() {
    let native = NativeRuntime::new(NativeConfig::new()).unwrap();
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    // A deliberately foreign worker would fail native tree creation. The
    // unsupported-policy error must win before that operation is attempted.
    let foreign = NativeRuntime::new(NativeConfig::new()).unwrap();
    let worker = foreign.attach().unwrap();
    let result = sto_masstree::FixedU64Table::new(
        &runtime,
        &native,
        &worker,
        TableConfig::default().with_record_local_versions(true),
    );
    assert!(matches!(
        result,
        Err(sto_masstree::FixedU64CreateError::Registration(
            sto_core::RegistrationError::Unsupported(_)
        ))
    ));
    assert_eq!(runtime.register_object().unwrap().object_id().get(), 1);
    worker.quiesce().unwrap();
}

#[test]
fn native_local_versions_mix_with_ordered_tables_and_cells() {
    for direct in [false, true] {
        let native = NativeRuntime::new(NativeConfig::new()).unwrap();
        let native_worker = native.attach().unwrap();
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, &native, &native_worker, direct, true);
        let ordered = table(&runtime, &native, &native_worker, direct, false);
        let cell = TxnCell::new(&runtime, 0_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        for value in 0..16_u8 {
            let mut txn = worker.begin().unwrap();
            local
                .put(&mut txn, &native_worker, b"same-row", &[value; 128])
                .unwrap();
            assert_eq!(committed(txn.commit().unwrap()).occ_commit_id(), None);
        }
        let mut txn = worker.begin().unwrap();
        local
            .put(&mut txn, &native_worker, b"same-row", b"mixed-table")
            .unwrap();
        ordered
            .put(&mut txn, &native_worker, b"same-row", b"ordered")
            .unwrap();
        assert_eq!(
            committed(txn.commit().unwrap())
                .occ_commit_id()
                .unwrap()
                .get(),
            2
        );
        let mut txn = worker.begin().unwrap();
        cell.set(&mut txn, 7).unwrap();
        local
            .put(&mut txn, &native_worker, b"same-row", b"mixed-cell")
            .unwrap();
        assert_eq!(
            committed(txn.commit().unwrap())
                .occ_commit_id()
                .unwrap()
                .get(),
            3
        );
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            local
                .get(&mut verify, &native_worker, b"same-row")
                .unwrap()
                .as_deref(),
            Some(&b"mixed-cell"[..])
        );
        assert_eq!(
            ordered
                .get(&mut verify, &native_worker, b"same-row")
                .unwrap()
                .as_deref(),
            Some(&b"ordered"[..])
        );
        assert_eq!(cell.get(&mut verify).unwrap(), 7);
        assert_eq!(committed(verify.commit().unwrap()).occ_commit_id(), None);
        native_worker.quiesce().unwrap();
    }
}

#[test]
fn native_local_fixed_batches_terminal_reads_and_scan_read_your_writes() {
    for direct in [false, true] {
        let native = NativeRuntime::new(NativeConfig::new()).unwrap();
        let native_worker = native.attach().unwrap();
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, &native, &native_worker, direct, true);
        let mut worker = runtime.attach().unwrap();
        let keys = [[1_u8; 8], [2_u8; 8]];
        let mut txn = worker.begin().unwrap();
        for key in keys {
            local
                .put(&mut txn, &native_worker, &key, &[3; 128])
                .unwrap();
        }
        assert_eq!(committed(txn.commit().unwrap()).occ_commit_id(), None);
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut txn = worker.begin().unwrap();
        {
            let mut session = local.point_session(&mut txn, &native_worker);
            let old = session
                .modify_fixed(&keys, &mut batch, |index, current| {
                    assert_eq!(current.map(Value::as_ref), Some(&[3; 128][..]));
                    PointMutation::Put(Value::from(vec![index as u8 + 4; 161].as_slice()))
                })
                .unwrap();
            assert_eq!(old.len(), 2);
            session.close().unwrap();
        }
        assert_eq!(committed(txn.commit().unwrap()).occ_commit_id(), None);
        {
            let txn = worker.begin_terminal_read_batch().unwrap();
            let mut visits = 0;
            let outcome = local
                .visit_fixed_terminal(txn, &native_worker, &keys, &mut batch, |index, value| {
                    assert_eq!(value.unwrap().as_ref(), vec![index as u8 + 4; 161]);
                    visits += 1;
                })
                .unwrap();
            match outcome {
                TerminalReadVisitOutcome::Ready {
                    transaction,
                    visited,
                } => {
                    assert_eq!(visited, 2);
                    assert_eq!(
                        committed(transaction.commit().unwrap()).occ_commit_id(),
                        None
                    );
                }
                TerminalReadVisitOutcome::RetryOrdinary => panic!("all native keys were seeded"),
            }
            assert_eq!(visits, 2);
        }
        let mut txn = worker.begin().unwrap();
        local
            .put(&mut txn, &native_worker, &keys[0], b"staged")
            .unwrap();
        local.remove(&mut txn, &native_worker, &keys[1]).unwrap();
        for direction in [ScanDirection::Forward, ScanDirection::Reverse] {
            let rows = local
                .scan(&mut txn, &native_worker, ScanRequest::new(direction, 8))
                .unwrap();
            assert_eq!(rows.len(), 1);
            assert_eq!(rows[0].key(), &keys[0]);
            assert_eq!(rows[0].value(), b"staged");
        }
        txn.abort();
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            local
                .get(&mut verify, &native_worker, &keys[1])
                .unwrap()
                .unwrap()
                .as_ref(),
            &[5; 161]
        );
        committed(verify.commit().unwrap());
        native_worker.quiesce().unwrap();
    }
}

#[test]
fn native_local_cross_worker_same_key_and_phantom_conflicts() {
    for direct in [false, true] {
        let native = NativeRuntime::new(NativeConfig::new()).unwrap();
        let native_worker = native.attach().unwrap();
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = table(&runtime, &native, &native_worker, direct, true);
        let mut worker = runtime.attach().unwrap();
        let mut seed = worker.begin().unwrap();
        local
            .put(&mut seed, &native_worker, b"a", b"before")
            .unwrap();
        committed(seed.commit().unwrap());
        for scan in [false, true] {
            let mut stale = worker.begin().unwrap();
            if scan {
                local
                    .scan(
                        &mut stale,
                        &native_worker,
                        ScanRequest::new(ScanDirection::Forward, 8),
                    )
                    .unwrap();
            } else {
                assert_eq!(
                    local
                        .get(&mut stale, &native_worker, b"a")
                        .unwrap()
                        .as_deref(),
                    Some(&b"before"[..])
                );
            }
            std::thread::scope(|scope| {
                let native = native.clone();
                let runtime = Arc::clone(&runtime);
                let local = local.clone();
                scope
                    .spawn(move || {
                        let native_worker = native.attach().unwrap();
                        let mut worker = runtime.attach().unwrap();
                        let mut txn = worker.begin().unwrap();
                        local
                            .put(
                                &mut txn,
                                &native_worker,
                                if scan { b"phantom" } else { b"a" },
                                b"winner",
                            )
                            .unwrap();
                        assert_eq!(committed(txn.commit().unwrap()).occ_commit_id(), None);
                        native_worker.quiesce().unwrap();
                    })
                    .join()
                    .unwrap();
            });
            assert_eq!(
                stale.commit().unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
            );
        }
        native_worker.quiesce().unwrap();
    }
}
