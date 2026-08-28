#![cfg(mtree_native_integration)]

use masstree::{
    Error, InsertOutcome, KeyBound, NativeStatus, RecordId, Runtime, RuntimeConfig, RuntimeHealth,
    ScanDirection, ScanRequest, ScanResume, ScanStopReason,
};

#[test]
fn negotiated_point_directory_round_trip_and_cross_worker_read() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    assert_eq!(runtime.health().unwrap(), RuntimeHealth::Healthy);
    assert_ne!(runtime.build_id().low, 0);
    assert_ne!(runtime.build_id().high, 0);

    let worker = runtime.attach().unwrap();
    let tree = runtime.create_tree(&worker).unwrap();
    let key = [0, 0xff, b'r', 0, b's', b't'];
    let candidate = RecordId::new(101).unwrap();
    assert_eq!(tree.get(&worker, &key).unwrap(), None);
    assert_eq!(
        tree.get_or_insert(&worker, &key, candidate).unwrap(),
        InsertOutcome::Inserted(candidate)
    );
    assert_eq!(tree.get(&worker, &key).unwrap(), Some(candidate));

    let other_runtime = runtime.clone();
    let other_tree = tree.clone();
    std::thread::spawn(move || {
        let other_worker = other_runtime.attach().unwrap();
        assert_eq!(
            other_tree.get(&other_worker, &key).unwrap(),
            Some(candidate)
        );
        assert_eq!(
            other_tree
                .get_or_insert(&other_worker, &key, RecordId::new(202).unwrap())
                .unwrap(),
            InsertOutcome::Existing(candidate)
        );
        other_worker.quiesce().unwrap();
    })
    .join()
    .unwrap();

    worker.quiesce().unwrap();
}

#[test]
fn one_shot_fixed_reads_reuse_results_and_end_native_guards() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let tree = runtime.create_tree(&worker).unwrap();
    let present = RecordId::new(401).unwrap();
    let empty = RecordId::new(402).unwrap();
    tree.get_or_insert(&worker, b"fixed/present", present)
        .unwrap();
    tree.get_or_insert(&worker, b"", empty).unwrap();

    let keys = [*b"fixed/present", *b"fixed/missing", *b"fixed/present"];
    let mut results = Vec::new();
    tree.get_fixed(&worker, &keys, &mut results).unwrap();
    assert_eq!(results.len(), keys.len());
    assert_eq!(results[0].record_id(), Some(present));
    assert_eq!(results[1].record_id(), None);
    assert_eq!(results[2].record_id(), Some(present));

    let retained_capacity = results.capacity();
    tree.get_fixed(&worker, &[] as &[[u8; 13]], &mut results)
        .unwrap();
    assert!(results.is_empty());
    assert_eq!(results.capacity(), retained_capacity);

    let empty_keys = [[], []];
    tree.get_fixed(&worker, &empty_keys, &mut results).unwrap();
    assert_eq!(results.len(), empty_keys.len());
    assert!(results
        .iter()
        .all(|result| result.record_id() == Some(empty)));

    /* The one-shot boundary retains no native scope or RCU state. */
    worker.quiesce().unwrap();
    let after = RecordId::new(403).unwrap();
    assert_eq!(
        tree.get_or_insert(&worker, b"fixed/after", after).unwrap(),
        InsertOutcome::Inserted(after)
    );

    let oversized = [[0_u8; 1025]];
    let error = tree
        .get_fixed(&worker, &oversized, &mut results)
        .unwrap_err();
    assert_eq!(
        error,
        Error::KeyTooLarge {
            length: 1025,
            maximum: runtime.max_key_length(),
        }
    );
    assert!(results.is_empty());
    worker.quiesce().unwrap();
}

#[test]
fn copied_scan_bounds_directions_and_resumption_round_trip() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let tree = runtime.create_tree(&worker).unwrap();
    for (raw, key) in [b"s/".as_slice(), b"s/\0", b"s/a", b"outside"]
        .into_iter()
        .enumerate()
    {
        tree.get_or_insert(&worker, key, RecordId::new(raw as u64 + 1).unwrap())
            .unwrap();
    }

    let request = ScanRequest::new(ScanDirection::Forward)
        .with_lower(KeyBound::Included(b"s/"))
        .with_upper(KeyBound::Excluded(b"s0"))
        .with_entry_capacity(2)
        .with_key_arena_capacity(32);
    let first = tree.scan_chunk(&worker, request).unwrap();
    assert_eq!(first.stop_reason(), ScanStopReason::EntryCapacity);
    assert_eq!(first.entries()[0].key(), b"s/");
    assert_eq!(first.entries()[1].key(), b"s/\0");
    let ScanResume::Exclusive(resume) = first.resume() else {
        panic!("a progressing capacity stop must return its last key")
    };

    let second = tree
        .scan_chunk(&worker, request.with_lower(KeyBound::Excluded(resume)))
        .unwrap();
    assert_eq!(second.stop_reason(), ScanStopReason::End);
    assert_eq!(second.entries().len(), 1);
    assert_eq!(second.entries()[0].key(), b"s/a");

    let reverse = tree
        .scan_chunk(
            &worker,
            ScanRequest::new(ScanDirection::Reverse)
                .with_lower(KeyBound::Included(b"s/"))
                .with_upper(KeyBound::Included(b"s/a")),
        )
        .unwrap();
    let reverse_keys: Vec<&[u8]> = reverse.entries().iter().map(|entry| entry.key()).collect();
    assert_eq!(reverse_keys, [b"s/a".as_slice(), b"s/\0", b"s/"]);

    let too_small = tree
        .scan_chunk(
            &worker,
            request.with_entry_capacity(1).with_key_arena_capacity(1),
        )
        .unwrap();
    assert_eq!(too_small.stop_reason(), ScanStopReason::KeyArenaCapacity);
    assert_eq!(too_small.resume(), &ScanResume::UnchangedInput);
    assert_eq!(too_small.next_key_bytes_required(), 2);
    worker.quiesce().unwrap();
}

#[test]
fn scoped_reads_end_before_insert_and_drop_during_unwind() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let tree = runtime.create_tree(&worker).unwrap();
    let cloned_tree = tree.clone();
    let present = RecordId::new(301).unwrap();
    tree.get_or_insert(&worker, b"scope/present", present)
        .unwrap();

    {
        let mut scope = tree.read_scope(&worker).unwrap();
        let batch_keys = [*b"scope/present", *b"scope/missing", *b"scope/present"];
        let mut batch_results = Vec::new();
        scope.get_fixed(&batch_keys, &mut batch_results).unwrap();
        assert_eq!(batch_results.len(), batch_keys.len());
        assert_eq!(batch_results[0].record_id(), Some(present));
        assert_eq!(batch_results[1].record_id(), None);
        assert_eq!(batch_results[2].record_id(), Some(present));
        let retained_capacity = batch_results.capacity();
        scope
            .get_fixed(&[] as &[[u8; 13]], &mut batch_results)
            .unwrap();
        assert!(batch_results.is_empty());
        assert_eq!(batch_results.capacity(), retained_capacity);
        for _ in 0..32 {
            assert_eq!(scope.get(b"scope/present").unwrap(), Some(present));
        }
        assert_eq!(scope.get(b"scope/missing").unwrap(), None);
        assert_eq!(
            tree.get(&worker, b"scope/present"),
            Err(Error::Native(NativeStatus::ActiveGuards))
        );
        assert_eq!(
            cloned_tree.get(&worker, b"scope/present"),
            Err(Error::Native(NativeStatus::ActiveGuards))
        );
        assert_eq!(
            tree.read_scope(&worker).unwrap_err(),
            Error::Native(NativeStatus::ActiveGuards)
        );
        assert_eq!(runtime.attach().unwrap_err(), Error::DuplicateWorker);
        let blocked = tree
            .get_or_insert(&worker, b"scope/missing", RecordId::new(302).unwrap())
            .unwrap_err();
        assert_eq!(blocked.error(), Error::Native(NativeStatus::ActiveGuards));
        assert_eq!(
            tree.scan_chunk(&worker, ScanRequest::new(ScanDirection::Forward)),
            Err(Error::Native(NativeStatus::ActiveGuards))
        );
        assert_eq!(
            runtime.create_tree(&worker).unwrap_err(),
            Error::Native(NativeStatus::ActiveGuards)
        );
        assert_eq!(
            runtime.shutdown(&worker),
            Err(Error::Native(NativeStatus::ActiveGuards))
        );
        assert_eq!(
            worker.quiesce(),
            Err(Error::Native(NativeStatus::ActiveGuards))
        );
    }

    let missing = RecordId::new(302).unwrap();
    assert_eq!(
        tree.get_or_insert(&worker, b"scope/missing", missing)
            .unwrap(),
        InsertOutcome::Inserted(missing)
    );

    let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut scope = tree.read_scope(&worker).unwrap();
        assert_eq!(scope.get(b"scope/missing").unwrap(), Some(missing));
        panic!("exercise ReadScope drop during unwind");
    }));
    assert!(unwind.is_err());

    let after_unwind = RecordId::new(303).unwrap();
    assert_eq!(
        tree.get_or_insert(&worker, b"scope/after-unwind", after_unwind)
            .unwrap(),
        InsertOutcome::Inserted(after_unwind)
    );
    tree.read_scope(&worker).unwrap().close().unwrap();
    worker.quiesce().unwrap();
}
