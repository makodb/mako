//! The engine opts into the adapter's 160-byte atomic value tier. Exercise
//! transitions in both directions through point reads, scans, and records.

use std::num::NonZeroU64;
use std::ptr::NonNull;

use mako_engine_sto::{CommitDisposition, CommitRecordChecksum, CommitRecordTarget, LocalDb};

#[test]
fn cache_values_cross_atomic_and_shared_storage_boundaries() {
    let db = LocalDb::open().unwrap();
    db.bind_trusted_table("values", 1).unwrap();
    let table = db.open_table("values", 1).unwrap();
    let lengths = [0, 8, 128, 160, 161, 1024];
    for (round, length) in lengths
        .into_iter()
        .chain(lengths.into_iter().rev())
        .enumerate()
    {
        let expected = vec![(round + 1) as u8; length];
        let (_, mut tx) = db.trusted_bound_transaction().unwrap();
        tx.put(&table, b"value", &expected).unwrap();
        assert_eq!(tx.get(&table, b"value").unwrap(), Some(expected.clone()));
        let rows = tx
            .scan(&table, b"value", Some(b"valuf"))
            .unwrap()
            .collect::<Result<Vec<_>, _>>()
            .unwrap();
        assert_eq!(rows, vec![(b"value".to_vec(), expected.clone())]);
        let plan = tx
            .commit_record_preflight_with_checksum(4096, CommitRecordChecksum::Crc32c)
            .unwrap();
        let mut record = vec![0; plan.exact_record_bytes()];
        // SAFETY: the unique local sequence and record buffer remain live
        // throughout the synchronous commit; no cache owns this database.
        let report = unsafe {
            tx.commit_report_with_record_target(|_, plan| {
                Some(CommitRecordTarget::from_raw_parts(
                    NonZeroU64::new(round as u64 + 1).unwrap(),
                    NonNull::new(record.as_mut_ptr()).unwrap(),
                    plan.exact_record_bytes(),
                ))
            })
        };
        assert_eq!(report.commit.disposition, CommitDisposition::Committed);
        assert_eq!(report.commit.cleanup, Ok(()));
        assert!(report.record_written && report.completion_contract_valid);
        let mut check = db.transaction().unwrap();
        assert_eq!(check.get(&table, b"value").unwrap(), Some(expected.clone()));
        assert_eq!(
            check
                .rscan(&table, b"value", Some(b"valuf"))
                .unwrap()
                .collect::<Result<Vec<_>, _>>()
                .unwrap(),
            vec![(b"value".to_vec(), expected)]
        );
        check.commit().unwrap();
    }
    for length in [160, 161, 8] {
        let mut tx = db.transaction().unwrap();
        assert!(tx.remove(&table, b"value").unwrap());
        assert_eq!(tx.get(&table, b"value").unwrap(), None);
        tx.commit().unwrap();
        let mut tx = db.transaction().unwrap();
        let expected = vec![0x37; length];
        assert!(tx.insert(&table, b"value", &expected).unwrap());
        assert_eq!(tx.get(&table, b"value").unwrap(), Some(expected));
        tx.commit().unwrap();
    }

    // Every earlier borrowed byte version must stay immutable while a later
    // operation overwrites or deletes its canonical log entry.
    let other = db.open_table("other-values", 2).unwrap();
    let mut tx = db.transaction().unwrap();
    tx.put(&table, b"value", &[0x11; 128]).unwrap();
    let first_snapshot = tx.get(&table, b"value").unwrap().unwrap();
    tx.put(&other, b"value", &[0x22; 160]).unwrap();
    tx.put(&table, b"value", &[0x33; 161]).unwrap();
    tx.put(&table, b"value", &[0x44; 128]).unwrap();
    assert!(tx.remove(&table, b"value").unwrap());
    assert!(tx.insert(&table, b"value", &[0x55; 160]).unwrap());
    tx.put(&other, b"value", &[0x66; 128]).unwrap();
    assert_eq!(first_snapshot, [0x11; 128]);
    assert_eq!(tx.get(&table, b"value").unwrap().unwrap(), [0x55; 160]);
    assert_eq!(tx.get(&other, b"value").unwrap().unwrap(), [0x66; 128]);
    tx.commit().unwrap();

    let mut aborted = db.transaction().unwrap();
    aborted.put(&table, b"value", &[0x77; 128]).unwrap();
    aborted.put(&table, b"value", &[0x88; 160]).unwrap();
    drop(aborted);
    let mut check = db.transaction().unwrap();
    assert_eq!(check.get(&table, b"value").unwrap().unwrap(), [0x55; 160]);
    check.commit().unwrap();
}
