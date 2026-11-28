use sto_core::protocol::{
    epoch::EpochDomain,
    range_lock::{RangeLocks, Request},
    wire::ErrorCode,
};
use std::sync::Arc;

fn request(generation: u64, lo: &[u8], hi: Option<&[u8]>) -> Request {
    Request {
        table: 7,
        lower: lo.to_vec(),
        upper: hi.map(Vec::from),
        generation,
        identity: generation.to_le_bytes().to_vec(),
    }
}

#[test]
fn frozen_range_waits_for_old_work_and_blocks_new_points_absences_and_scans() {
    let epochs = Arc::new(EpochDomain::default());
    let locks = RangeLocks::new(epochs.clone());
    let prepared = epochs.enter();
    let guard = locks.acquire(request(1, b"b", Some(b"f"))).unwrap();
    let fresh = epochs.enter();
    assert!(!guard.drained().unwrap());
    assert_eq!(guard.enter().err().unwrap().code, ErrorCode::Busy);
    // Earlier work can finish under its captured admission, including writes.
    locks.point(&prepared, 7, b"c").unwrap();
    for key in [b"b".as_slice(), b"c", b"e\0"] {
        assert!(locks.point(&fresh, 7, key).is_err());
    }
    assert!(locks.range(&fresh, 7, b"a", Some(b"z")).is_err());
    locks.point(&fresh, 7, b"f").unwrap();
    locks.range(&fresh, 7, b"", Some(b"b")).unwrap();
    locks.range(&fresh, 7, b"f", None).unwrap();
    locks.point(&fresh, 8, b"c").unwrap();
    drop(prepared);
    assert!(guard.drained().unwrap());
    let io = guard.enter().unwrap();
    io.point(7, b"c").unwrap();
    io.range(7, b"b", Some(b"f")).unwrap();
    assert!(io.point(7, b"f").is_err());
    assert!(io.point(8, b"c").is_err());
    assert!(io.range(7, b"b", None).is_err());
    assert!(io.range(7, b"a", Some(b"f")).is_err());
    assert!(guard.finish().is_err());
    assert!(locks.reset().is_err());
    drop(io);
    // Guard drop does not reopen ordinary admission.
    drop(guard);
    assert!(locks.point(&fresh, 7, b"c").is_err());
    locks
        .acquire(request(1, b"b", Some(b"f")))
        .unwrap()
        .finish()
        .unwrap();
    locks.point(&fresh, 7, b"c").unwrap();
}

#[test]
fn duplicate_identity_overlap_and_terminal_replay_are_checked() {
    let locks = RangeLocks::new(Arc::new(EpochDomain::default()));
    let guard = locks.acquire(request(2, b"", None)).unwrap();
    let duplicate = locks.acquire(request(2, b"", None)).unwrap();
    let mut changed = request(2, b"", None);
    changed.identity.push(99);
    assert!(locks.acquire(changed).is_err());
    assert!(locks.acquire(request(2, b"b", None)).is_err());
    assert!(locks.acquire(request(3, b"b", None)).is_err());
    guard.finish().unwrap();
    duplicate.finish().unwrap();
    assert!(duplicate.enter().is_err());
    assert!(locks.acquire(request(2, b"", None)).is_err());
    assert!(locks.acquire(request(1, b"b", None)).is_err());
    let next = locks.acquire(request(3, b"b", None)).unwrap();
    duplicate.finish().unwrap();
    next.enter().unwrap().point(7, b"b").unwrap();
}

#[test]
fn reset_invalidates_old_guards_without_releasing_recovered_work() {
    let epochs = Arc::new(EpochDomain::default());
    let locks = RangeLocks::new(epochs.clone());
    let guard = locks.acquire(request(1, b"", None)).unwrap();
    locks.reset().unwrap();
    let recovered_prepared = epochs.enter();
    let recovered = locks.acquire(request(1, b"", None)).unwrap();
    assert!(guard.finish().is_err());
    assert!(guard.drained().is_err());
    assert!(guard.enter().is_err());
    assert!(!recovered.drained().unwrap());
    drop(recovered_prepared);
    assert!(recovered.drained().unwrap());
}

#[test]
fn invalid_and_foreign_access_fail_and_adjacent_ranges_do_not_overlap() {
    let epochs = Arc::new(EpochDomain::default());
    let locks = RangeLocks::new(epochs.clone());
    for request in [
        request(0, b"", None),
        request(1, b"b", Some(b"b")),
        request(1, b"z", Some(b"a")),
    ] {
        assert!(locks.acquire(request).is_err());
    }
    let foreign = Arc::new(EpochDomain::default()).enter();
    assert!(locks.point(&foreign, 7, b"a").is_err());
    assert!(locks.range(&foreign, 7, b"", None).is_err());
    let left = locks.acquire(request(1, b"", Some(b"b"))).unwrap();
    let right = locks.acquire(request(2, b"b", None)).unwrap();
    left.enter().unwrap().point(7, b"a").unwrap();
    right.enter().unwrap().point(7, b"b").unwrap();
}

#[test]
fn concurrent_admission_and_acquisition_have_no_unprotected_gap() {
    for _ in 0..64 {
        let epochs = Arc::new(EpochDomain::default());
        let locks = RangeLocks::new(epochs.clone());
        let start = Arc::new(std::sync::Barrier::new(2));
        let worker = {
            let epochs = epochs.clone();
            let locks = locks.clone();
            let start = start.clone();
            std::thread::spawn(move || {
                start.wait();
                let pin = epochs.enter();
                let admitted = locks.point(&pin, 7, b"c").is_ok();
                (pin, admitted)
            })
        };
        start.wait();
        let guard = locks.acquire(request(1, b"b", Some(b"f"))).unwrap();
        let (pin, admitted) = worker.join().unwrap();
        // If admission won the race, its retained pin prevents an exclusive grant.
        assert!(!admitted || !guard.drained().unwrap());
        drop(pin);
        assert!(guard.drained().unwrap());
    }
}

#[test]
fn cancelling_pending_migration_preserves_old_transaction_protection() {
    let epochs = Arc::new(EpochDomain::default());
    let locks = RangeLocks::new(epochs.clone());
    let uncertain = epochs.enter();
    let aborted = locks.acquire(request(1, b"b", Some(b"f"))).unwrap();
    let fresh = epochs.enter();
    assert!(locks.point(&fresh, 7, b"c").is_err());
    // The controller has published Abort/Serving; cancelling need not wait.
    aborted.finish().unwrap();
    locks.point(&fresh, 7, b"c").unwrap();
    let next = locks.acquire(request(2, b"b", Some(b"f"))).unwrap();
    drop(fresh);
    assert!(!next.drained().unwrap());
    // Cancelling migration never releases a transaction's prepare/unknown pin.
    drop(uncertain);
    assert!(next.drained().unwrap());
}
