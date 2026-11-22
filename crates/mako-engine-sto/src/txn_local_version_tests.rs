//! Check the facade's table policy, not just the adapter's optional capability.

use super::*;
use sto_core::TxnCell;

fn finish_core(mut transaction: Transaction<'_>) -> sto_core::CommitInfo {
    let outcome = transaction.take_inner().unwrap().commit();
    transaction.attachment.slot.release();
    match outcome.unwrap() {
        CommitOutcome::Committed(info) => info,
        other => panic!("expected a committed core transaction, got {other:?}"),
    }
}

#[test]
fn facade_tables_use_local_versions_and_preserve_mixed_ordered_writes() {
    std::thread::spawn(|| {
        {
            let db = LocalDb::open().unwrap();
            let table = db.open_table(b"local-version-policy", 1).unwrap();
            let ordered = TxnCell::new(db.runtime_for_test(), 0_u64).unwrap();

            // More local generations than the ordered clock has issued.
            // An accidental loss of the facade's opt-in fails on the first
            // iteration instead of becoming an unnoticed scaling regression.
            for value in 0..32_u64 {
                let mut transaction = db.transaction().unwrap();
                transaction
                    .put(&table, b"key", &value.to_le_bytes())
                    .unwrap();
                let info = finish_core(transaction);
                assert_eq!(info.occ_commit_id(), None);
            }

            let mut mixed = db.transaction().unwrap();
            mixed.put(&table, b"key", &32_u64.to_le_bytes()).unwrap();
            ordered.set(mixed.inner_mut(), 99).unwrap();
            let info = finish_core(mixed);
            // Version 1 is the initial generation; this is the runtime's
            // first reserved ordered commit ID.
            assert_eq!(info.occ_commit_id().unwrap().get(), 2);

            let mut read = db.transaction().unwrap();
            assert_eq!(
                read.get(&table, b"key").unwrap(),
                Some(32_u64.to_le_bytes().to_vec())
            );
            assert_eq!(ordered.get(read.inner_mut()).unwrap(), 99);
            read.commit().unwrap();
            assert!(db.runtime_is_healthy());
        }
        // SAFETY: this fresh worker's healthy database, tables, core cells,
        // transactions, and all aliases have left scope. Only its idle
        // process-lifetime test attachments remain.
        unsafe { worker::drop_healthy_test_attachments() };
    })
    .join()
    .unwrap();
}
