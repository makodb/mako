#![cfg(mtree_native_integration)]
use masstree::{Runtime as NativeRuntime, RuntimeConfig as NativeConfig};
use sto_core::{CommitInfo, CommitOutcome, Runtime, RuntimeConfig, TxnCell};
use sto_masstree::{Table, TableConfig};
fn committed(outcome: CommitOutcome) -> CommitInfo {
    match outcome {
        CommitOutcome::Committed(info) => info,
        _ => panic!("unexpected abort"),
    }
}
#[test]
fn native_local_versions_mix_with_ordered_tables_and_cells() {
    {
        let native = NativeRuntime::new(NativeConfig::new()).unwrap();
        let native_worker = native.attach().unwrap();
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = Table::new(
            &runtime,
            &native,
            &native_worker,
            TableConfig::new().with_record_local_versions(true),
        )
        .unwrap();
        let ordered = Table::new(&runtime, &native, &native_worker, TableConfig::new()).unwrap();
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
