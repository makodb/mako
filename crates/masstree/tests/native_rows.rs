#![cfg(mtree_native_integration)]
use masstree::{
    rows::{Rows, Value, Visibility},
    Error, NativeStatus, Runtime, RuntimeConfig, ScanDirection, ScanRequest,
};
fn setup() -> (Runtime, masstree::Worker, Rows) {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let rows = Rows::new(&runtime, &worker, 16, 4096).unwrap();
    (runtime, worker, rows)
}
fn publish(row: &masstree::rows::Row, bytes: Option<&[u8]>, version: u64, time: u32) {
    let value = Value::new(bytes).unwrap();
    let mut guard = row.lock(0).unwrap();
    let mut write = guard.prepare(&value).unwrap();
    guard.install(&mut write, version, time).unwrap();
    guard.release(1).unwrap();
}
#[test]
fn native_buffers_are_retained_without_copy_and_outlive_rows() {
    let (_, worker, rows) = setup();
    let (row, fresh) = rows.resolve(&worker, b"binary\0key").unwrap();
    assert!(fresh);
    let bytes: Vec<u8> = (0..65537).map(|n| (n % 251) as u8).collect();
    publish(&row, Some(&bytes), 2, 100);
    let old = row.read(None, None, false).unwrap();
    let again = row.read(None, None, false).unwrap();
    assert_eq!(
        old.value.bytes().unwrap().as_ptr(),
        again.value.bytes().unwrap().as_ptr()
    );
    publish(&row, Some(b"new"), 3, 200);
    assert!(row.check(2, None).is_err());
    drop(again);
    drop(row);
    drop(rows);
    drop(worker);
    assert_eq!(old.value.bytes(), Some(bytes.as_slice()));
}
#[test]
fn locks_abort_and_checkpoint_obey_native_ownership() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"key").unwrap();
    publish(&row, Some(b"old"), 2, 100);
    let mut guard = row.lock(4).unwrap();
    assert!(row.lock(5).is_err());
    assert!(row.read(None, None, false).is_err());
    assert_eq!(
        row.read(None, None, true).unwrap().value.bytes(),
        Some(b"old".as_slice())
    );
    let write = guard
        .prepare(&Value::new(Some(b"aborted")).unwrap())
        .unwrap();
    drop(write);
    guard.release(0).unwrap();
    assert_eq!(
        row.read(None, None, false).unwrap().value.bytes(),
        Some(b"old".as_slice())
    );
    assert_eq!(rows.len(), 1);
}
#[test]
fn historical_tombstones_pruning_and_failed_watermarks_are_native() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"history").unwrap();
    publish(&row, Some(b"old"), 2, 100);
    publish(&row, None, 3, 200);
    publish(&row, Some(b"new"), 4, 300);
    let mut visibility = Visibility {
        term: 1,
        stable: [u32::MAX; 10],
        failed: u32::MAX,
    };
    visibility.stable[0] = 10;
    let old = row.read(None, Some(visibility), false).unwrap();
    assert_eq!(old.value.bytes(), Some(b"old".as_slice()));
    visibility.stable[0] = 20;
    let dead = row.read(None, Some(visibility), false).unwrap();
    assert_eq!(dead.time_term, 200);
    assert_eq!(dead.value.bytes(), None);
    rows.advance_floor(20);
    publish(&row, Some(b"last"), 5, 400);
    visibility.stable[0] = 10;
    assert!(row.read(None, Some(visibility), false).is_err());
    assert_eq!(old.value.bytes(), Some(b"old".as_slice()));
    visibility.stable[0] = u32::MAX;
    visibility.failed = 39;
    assert!(row.read(None, Some(visibility), false).is_err());
}
#[test]
fn native_directory_owns_binary_keys_and_enforces_capacity() {
    let (runtime, worker, _) = setup();
    let rows = Rows::new(&runtime, &worker, 4, 16).unwrap();
    for key in [b"".as_slice(), b"\0", b"a\0", b"\xff"] {
        rows.resolve(&worker, key).unwrap();
    }
    assert_eq!(rows.generation(), 4);
    assert!(!rows.resolve(&worker, b"a\0").unwrap().1);
    assert!(matches!(
        rows.resolve(&worker, b"extra"),
        Err(Error::Native(NativeStatus::OutOfMemory))
    ));
    for direction in [ScanDirection::Forward, ScanDirection::Reverse] {
        let scan = rows.scan(&worker, ScanRequest::new(direction)).unwrap();
        assert!(scan.end);
        let mut expected = vec![vec![], vec![0], vec![b'a', 0], vec![255]];
        if direction == ScanDirection::Reverse {
            expected.reverse();
        }
        assert_eq!(
            scan.entries
                .iter()
                .map(|r| r.key().to_vec())
                .collect::<Vec<_>>(),
            expected
        );
    }
}
#[test]
fn abandoning_a_native_lock_fences_the_table() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"key").unwrap();
    drop(row.lock(0).unwrap());
    assert!(matches!(
        row.read(None, None, false),
        Err(Error::Native(NativeStatus::Poisoned))
    ));
    assert!(rows.resolve(&worker, b"another").is_err());
}
#[test]
fn exhausted_budget_rejects_before_directory_allocation_and_reclaims_row_owners() {
    let (runtime, worker, _) = setup();
    let before = runtime.created_tree_count();
    let tiny = masstree::rows::Budget::new(1).unwrap();
    assert!(Rows::with_budget(&runtime, &worker, 10, 100, &tiny).is_err());
    assert_eq!(runtime.created_tree_count(), before);
    assert_eq!(tiny.used(), 0);
    let budget = masstree::rows::Budget::new(8192).unwrap();
    let rows = Rows::with_budget(&runtime, &worker, 10, 100, &budget).unwrap();
    let (row, _) = rows.resolve(&worker, b"key").unwrap();
    publish(&row, Some(b"snapshot"), 2, 100);
    let snapshot = row.read(None, None, false).unwrap();
    assert!(budget.used() > 0);
    drop(rows);
    assert!(budget.used() > 0);
    drop(row);
    assert_eq!(budget.used(), 0);
    assert_eq!(snapshot.value.bytes(), Some(b"snapshot".as_slice()));
}

#[test]
fn borrowed_row_tokens_require_the_original_live_table() {
    let (runtime, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"kept").unwrap();
    publish(&row, Some(b"bytes"), 2, 0);
    let token = row.token();
    drop(row);
    let borrowed = rows.borrow(token).unwrap();
    assert_eq!(borrowed.key(), b"kept");
    let owning = borrowed.into_owned();
    let other = Rows::new(&runtime, &worker, 16, 4096).unwrap();
    assert!(matches!(other.borrow(token), Err(Error::WrongRuntime)));
    drop(rows);
    assert_eq!(
        owning.read(None, None, false).unwrap().value.bytes(),
        Some(b"bytes".as_slice())
    );
    drop(owning);
    let later = Rows::new(&runtime, &worker, 16, 4096).unwrap();
    assert!(matches!(later.borrow(token), Err(Error::WrongRuntime)));
}

#[test]
fn atomic_owner_version_word_preserves_the_full_core_domain() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"edge").unwrap();
    let mut guard = row.lock(65534).unwrap();
    assert!(row.check(1, None).is_err());
    row.check(1, Some(65534)).unwrap();
    assert!(row.lock(65535).is_err());
    let value = Value::new(Some(b"last")).unwrap();
    let mut write = guard.prepare(&value).unwrap();
    let maximum = u64::MAX >> 16;
    guard.install(&mut write, maximum, 0).unwrap();
    row.check(maximum, Some(65534)).unwrap();
    assert!(row.check(1, Some(65534)).is_err());
    guard.release(1).unwrap();
    row.check(maximum, None).unwrap();
    assert_eq!(row.read(None, None, false).unwrap().version, maximum);
}

#[test]
fn leaf_observations_keep_native_storage_alive_across_splits_and_threads() {
    let (runtime, worker, _) = setup();
    let rows = Rows::new(&runtime, &worker, 4096, 1 << 20).unwrap();
    let mut key = vec![0; 1024];
    let first = rows.lookup(&worker, &key).unwrap().observed.unwrap();
    let other = rows.clone();
    let native = runtime.clone();
    std::thread::spawn(move || {
        let worker = native.attach().unwrap();
        for i in 0..100u64 {
            key[1016..].copy_from_slice(&i.to_be_bytes());
            other.resolve(&worker, &key).unwrap();
        }
    })
    .join()
    .unwrap();
    assert!(first.leaf.check(first.version).is_err());
    let miss = rows.lookup(&worker, b"z").unwrap().observed.unwrap();
    drop(rows);
    drop(worker);
    drop(runtime);
    miss.leaf.check(miss.version).unwrap();
}

#[test]
fn private_write_preparation_cannot_publish_without_the_matching_guard() {
    let (_, worker, rows) = setup();
    let (one, _) = rows.resolve(&worker, b"one").unwrap();
    let (two, _) = rows.resolve(&worker, b"two").unwrap();
    let value = Value::new(Some(b"prepared")).unwrap();
    let mut write = one.prepare(&value).unwrap();
    assert_eq!(one.read(None, None, false).unwrap().value.bytes(), None);
    let mut wrong = two.lock(0).unwrap();
    assert!(wrong.install(&mut write, 2, 0).is_err());
    wrong.release(0).unwrap();
    let mut right = one.lock(0).unwrap();
    right.install(&mut write, 2, 0).unwrap();
    right.release(1).unwrap();
    assert_eq!(
        one.read(None, None, false).unwrap().value.bytes(),
        Some(b"prepared".as_slice())
    );
    assert_eq!(two.read(None, None, false).unwrap().value.bytes(), None);
}

#[test]
fn embedded_values_copy_on_shared_snapshot_and_outlive_the_table() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"inline").unwrap();
    publish(&row, Some(b"original"), 2, 0);
    let old = row.read(None, None, false).unwrap().value;
    let original_address = old.bytes().unwrap().as_ptr();
    assert_eq!(
        row.read(None, None, false)
            .unwrap()
            .value
            .bytes()
            .unwrap()
            .as_ptr(),
        original_address
    );
    publish(&row, Some(b"new"), 3, 0);
    assert_eq!(old.bytes(), Some(b"original".as_slice()));
    let mut buffer = Vec::new();
    assert_eq!(row.read_into(&mut buffer).unwrap(), (true, 3));
    assert_eq!(buffer, b"new");
    drop(old);
    // The old embedded allocation is now free for reuse, but a held heap
    // snapshot must still survive the transition back to embedded storage.
    let heap = row.read(None, None, false).unwrap().value;
    publish(&row, Some(b"reused"), 4, 0);
    let last = row.read(None, None, false).unwrap().value;
    assert_eq!(last.bytes().unwrap().as_ptr(), original_address);
    assert_eq!(heap.bytes(), Some(b"new".as_slice()));
    drop(row);
    drop(rows);
    drop(worker);
    assert_eq!(last.bytes(), Some(b"reused".as_slice()));
    assert_eq!(heap.bytes(), Some(b"new".as_slice()));
}

#[test]
fn copy_reads_resize_and_distinguish_empty_values_from_tombstones() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"copy").unwrap();
    let mut buffer = Vec::new();
    for (index, size) in [0, 8, 16, 17, 1024, 65537, 1].into_iter().enumerate() {
        let bytes: Vec<u8> = (0..size).map(|n| (n % 251) as u8).collect();
        let version = index as u64 + 2;
        publish(&row, Some(&bytes), version, 0);
        assert_eq!(row.read_into(&mut buffer).unwrap(), (true, version));
        assert_eq!(buffer, bytes);
    }
    publish(&row, None, 20, 0);
    assert_eq!(row.read_into(&mut buffer).unwrap(), (false, 20));
    assert!(buffer.is_empty());
    let mut guard = row.lock(7).unwrap();
    assert_eq!(
        row.read_into(&mut buffer),
        Err(Error::Native(NativeStatus::Busy))
    );
    guard.release(0).unwrap();
}

#[test]
fn protocol_tickets_validate_table_lifetime_and_write_identity() {
    let (runtime, worker, rows) = setup();
    let other = Rows::new(&runtime, &worker, 16, 4096).unwrap();
    let (row, _) = rows.resolve(&worker, b"ticket").unwrap();
    let token = row.token();
    let masstree::rows::Located::Missing(leaf, version) = rows.locate(&worker, b"missing").unwrap()
    else {
        panic!("unexpected row");
    };
    rows.check_leaf(leaf, version).unwrap();
    assert_eq!(other.check_leaf(leaf, version), Err(Error::WrongRuntime));
    let value = Value::new(Some(b"new")).unwrap();
    let mut write = row.prepare(&value).unwrap();
    let mut ticket = rows.lock_ticket(token, 0).unwrap();
    assert_eq!(
        other.install_ticket(&mut ticket, &mut write, 2, 0),
        Err(Error::WrongRuntime)
    );
    assert_eq!(
        other.release_ticket(&mut ticket, 0),
        Err(Error::WrongRuntime)
    );
    rows.install_ticket(&mut ticket, &mut write, 2, 0).unwrap();
    rows.release_ticket(&mut ticket, 1).unwrap();
    assert!(rows.release_ticket(&mut ticket, 1).is_err());
    let pending = row.prepare(&value).unwrap();
    let abandoned = rows.lock_ticket(token, 1).unwrap();
    drop(row);
    drop(rows);
    assert_eq!(
        other.release_ticket(&mut { abandoned }, 0),
        Err(Error::WrongRuntime)
    );
    // Dropping private scratch never dereferences its now-destroyed row.
    drop(pending);
    drop(write);
}

#[test]
fn native_scratch_can_be_returned_from_later_thread_local_destructors() {
    std::thread::spawn(|| {
        thread_local! {
            static LATE: std::cell::RefCell<Option<Value>> = const { std::cell::RefCell::new(None) };
        }
        // Register the Rust owner before the native pool's TLS cleanup. Its
        // value may be destroyed after the native scratch pool already closed.
        LATE.with(|slot| assert!(slot.borrow().is_none()));
        for _ in 0..128 { drop(Value::new(Some(b"scratch")).unwrap()); }
        let value = Value::new(Some(b"late")).unwrap();
        LATE.with(|slot| *slot.borrow_mut() = Some(value));
    }).join().unwrap();
}

#[test]
fn optimistic_copies_never_mix_versions_across_inline_and_heap_updates() {
    use std::sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Barrier,
    };
    fn payload(n: u64) -> Vec<u8> {
        let word = n.wrapping_mul(0x9e3779b97f4a7c15);
        let mut value = Vec::new();
        for _ in 0..if n % 8 == 0 { 16 } else { 1 } {
            value.extend_from_slice(&word.to_le_bytes());
            value.extend_from_slice(&(!word).to_le_bytes());
        }
        value
    }
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"seqlock").unwrap();
    publish(&row, Some(&payload(0)), 2, 0);
    let row = Arc::new(row);
    let start = Arc::new(Barrier::new(4));
    let finished = Arc::new(AtomicBool::new(false));
    let readers: Vec<_> = (0..3)
        .map(|_| {
            let (row, start, finished) = (row.clone(), start.clone(), finished.clone());
            std::thread::spawn(move || {
                let mut bytes = Vec::new();
                let mut successes = 0;
                start.wait();
                while !finished.load(Ordering::Acquire) || successes < 100 {
                    match row.read_into(&mut bytes) {
                        Ok((true, version)) => {
                            assert_eq!(bytes, payload(version - 2));
                            successes += 1;
                        }
                        Err(Error::Native(NativeStatus::Busy)) => {}
                        other => panic!("unexpected copy result: {other:?}"),
                    }
                }
            })
        })
        .collect();
    start.wait();
    for n in 1..5000 {
        publish(&row, Some(&payload(n)), n + 2, 0);
    }
    finished.store(true, Ordering::Release);
    for reader in readers {
        reader.join().unwrap();
    }
}

#[test]
fn local_and_timestamped_updates_preserve_pins_across_history_pruning() {
    let (_, worker, rows) = setup();
    let (row, _) = rows.resolve(&worker, b"mixed-history").unwrap();
    publish(&row, Some(b"local"), 2, 0);
    let local = row.read(None, None, false).unwrap();
    publish(&row, Some(b"remote-one"), 3, 100);
    let mut visibility = Visibility {
        term: 1,
        stable: [u32::MAX; 10],
        failed: u32::MAX,
    };
    visibility.stable[0] = 0;
    assert_eq!(
        row.read(None, Some(visibility), false)
            .unwrap()
            .value
            .bytes(),
        Some(b"local".as_slice())
    );
    rows.advance_floor(10);
    publish(&row, Some(b"remote-two"), 4, 200);
    visibility.stable[0] = 10;
    let historical = row.read(None, Some(visibility), false).unwrap();
    assert_eq!(historical.value.bytes(), Some(b"remote-one".as_slice()));

    // Two consecutive local publications discard timestamped history. Retained
    // snapshots must survive, including the original embedded allocation.
    publish(&row, Some(b"local-again"), 5, 0);
    let large = vec![0x91; 4096];
    publish(&row, Some(&large), 6, 0);
    publish(&row, Some(b"remote-three"), 7, 300);
    visibility.stable[0] = 20;
    // The earlier floor already excludes the timestamp-zero local version.
    assert!(row.read(None, Some(visibility), false).is_err());
    publish(&row, Some(b"remote-four"), 8, 400);
    visibility.stable[0] = 30;
    assert_eq!(
        row.read(None, Some(visibility), false)
            .unwrap()
            .value
            .bytes(),
        Some(b"remote-three".as_slice())
    );
    rows.advance_floor(50);
    publish(&row, Some(b"remote-five"), 9, 500);
    visibility.stable[0] = 40;
    assert!(row.read(None, Some(visibility), false).is_err());
    drop(row);
    drop(rows);
    drop(worker);
    assert_eq!(local.value.bytes(), Some(b"local".as_slice()));
    assert_eq!(historical.value.bytes(), Some(b"remote-one".as_slice()));
}
