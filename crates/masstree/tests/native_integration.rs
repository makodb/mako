#![cfg(mtree_native_integration)]

use masstree::{
    BoundedRecordIdScanResume, Error, InsertOutcome, KeyBound, NativeStatus, PackedScanResume,
    PackedScanScratch, PublicationDisposition, RecordId, Runtime, RuntimeConfig, RuntimeHealth,
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

    /* The trusted fixed bridge must reuse an active worker-wide RCU scope. */
    let rcu_scope = worker.rcu_scope().unwrap();
    tree.get_fixed(&worker, &keys, &mut results).unwrap();
    assert_eq!(results[0].record_id(), Some(present));
    assert_eq!(results[1].record_id(), None);
    assert_eq!(results[2].record_id(), Some(present));
    rcu_scope.close().unwrap();

    /* A rejected operation still clears every previously populated result. */
    let read_scope = tree.read_scope(&worker).unwrap();
    assert_eq!(
        tree.get_fixed(&worker, &keys, &mut results).unwrap_err(),
        Error::Native(NativeStatus::ActiveGuards)
    );
    assert_eq!(results.len(), keys.len());
    assert!(results.iter().all(|result| result.record_id().is_none()));
    read_scope.close().unwrap();

    tree.get_fixed(&worker, &keys, &mut results).unwrap();
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
fn trusted_fixed_insert_batch_preserves_order_and_reuses_scratch() {
    fn padded(key: [u8; 4]) -> [u8; 16] {
        let mut padded = [0_u8; 16];
        padded[..4].copy_from_slice(&key);
        padded
    }

    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let tree = runtime.create_tree(&worker).unwrap();
    let initial_keys = [padded(*b"bi00"), padded(*b"bi01"), padded(*b"bi02")];
    let initial_candidates = [
        RecordId::new(501).unwrap(),
        RecordId::new(502).unwrap(),
        RecordId::new(503).unwrap(),
    ];
    let mut results = Vec::new();
    tree.get_or_insert_fixed_strided::<4, 16>(
        &worker,
        &initial_keys,
        &initial_candidates,
        &mut results,
    )
    .unwrap();
    for (result, candidate) in results.iter().zip(initial_candidates) {
        assert_eq!(
            result.classification(candidate).unwrap(),
            (PublicationDisposition::CandidateInserted, Some(candidate))
        );
    }

    let retained_capacity = results.capacity();
    let mixed_keys = [padded(*b"bi00"), padded(*b"bi03"), padded(*b"bi03")];
    let mixed_candidates = [
        RecordId::new(601).unwrap(),
        RecordId::new(602).unwrap(),
        RecordId::new(603).unwrap(),
    ];
    tree.get_or_insert_fixed_strided::<4, 16>(
        &worker,
        &mixed_keys,
        &mixed_candidates,
        &mut results,
    )
    .unwrap();
    assert_eq!(results.capacity(), retained_capacity);
    assert_eq!(
        results[0].classification(mixed_candidates[0]).unwrap(),
        (
            PublicationDisposition::CandidateProvenUnpublished,
            Some(initial_candidates[0])
        )
    );
    assert_eq!(
        results[1].classification(mixed_candidates[1]).unwrap(),
        (
            PublicationDisposition::CandidateInserted,
            Some(mixed_candidates[1])
        )
    );
    assert_eq!(
        results[2].classification(mixed_candidates[2]).unwrap(),
        (
            PublicationDisposition::CandidateProvenUnpublished,
            Some(mixed_candidates[1])
        )
    );
    assert_eq!(
        tree.get(&worker, b"bi03").unwrap(),
        Some(mixed_candidates[1])
    );

    let blocked_keys = [padded(*b"bi06")];
    let blocked_candidates = [RecordId::new(604).unwrap()];
    let scope = tree.read_scope(&worker).unwrap();
    assert_eq!(
        tree.get_or_insert_fixed_strided::<4, 16>(
            &worker,
            &blocked_keys,
            &blocked_candidates,
            &mut results,
        )
        .unwrap_err(),
        Error::Native(NativeStatus::ActiveGuards)
    );
    assert_eq!(
        results[0].classification(blocked_candidates[0]).unwrap(),
        (PublicationDisposition::FailureBeforePublication, None)
    );
    scope.close().unwrap();
    assert_eq!(tree.get(&worker, b"bi06").unwrap(), None);

    let duplicate_candidates = [mixed_candidates[0], mixed_candidates[0]];
    let rejected_keys = [padded(*b"bi04"), padded(*b"bi05")];
    assert_eq!(
        tree.get_or_insert_fixed_strided::<4, 16>(
            &worker,
            &rejected_keys,
            &duplicate_candidates,
            &mut results,
        )
        .unwrap_err(),
        Error::InvalidBatch("candidate record identities are not distinct")
    );
    assert!(results.is_empty());
    assert_eq!(tree.get(&worker, b"bi04").unwrap(), None);
    assert_eq!(
        tree.get_or_insert_fixed_strided::<4, 16>(
            &worker,
            &rejected_keys,
            &mixed_candidates[..1],
            &mut results,
        )
        .unwrap_err(),
        Error::InvalidBatch("key and candidate batch lengths differ")
    );
    assert!(results.is_empty());
    assert_eq!(
        tree.get_or_insert_fixed_strided::<17, 16>(
            &worker,
            &rejected_keys,
            &duplicate_candidates,
            &mut results,
        )
        .unwrap_err(),
        Error::InvalidBatch("key length exceeds key stride")
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

    let packed_first = tree.scan_packed_chunk(&worker, request).unwrap();
    assert_eq!(packed_first.stop_reason(), ScanStopReason::EntryCapacity);
    let packed_keys: Vec<_> = packed_first.entries().map(|entry| entry.key()).collect();
    assert_eq!(packed_keys, [b"s/".as_slice(), b"s/\0"]);
    let PackedScanResume::Exclusive(packed_resume) = packed_first.resume() else {
        panic!("a progressing packed scan must borrow its last key")
    };
    let packed_second = tree
        .scan_packed_chunk(
            &worker,
            request.with_lower(KeyBound::Excluded(packed_resume)),
        )
        .unwrap();
    assert_eq!(packed_second.stop_reason(), ScanStopReason::End);
    assert_eq!(
        packed_second
            .entries()
            .next()
            .expect("one packed continuation entry")
            .key(),
        b"s/a"
    );

    let mut scratch = PackedScanScratch::default();
    let reusable_resume = {
        let reusable = tree
            .scan_packed_chunk_reusing(&worker, request, &mut scratch)
            .unwrap();
        assert_eq!(reusable.stop_reason(), ScanStopReason::EntryCapacity);
        assert_eq!(
            reusable
                .entries()
                .map(|entry| entry.key().to_vec())
                .collect::<Vec<_>>(),
            [b"s/".to_vec(), b"s/\0".to_vec()]
        );
        let PackedScanResume::Exclusive(resume) = reusable.resume() else {
            panic!("reusable packed scan must expose its last key")
        };
        resume.to_vec()
    };
    let retained_entries = scratch.entry_capacity();
    let retained_arena = scratch.key_arena_capacity();
    {
        let reusable = tree
            .scan_packed_chunk_reusing(
                &worker,
                request
                    .with_lower(KeyBound::Excluded(&reusable_resume))
                    .with_entry_capacity(1)
                    .with_key_arena_capacity(4),
                &mut scratch,
            )
            .unwrap();
        assert_eq!(
            reusable.entries().next().map(|entry| entry.key()),
            Some(&b"s/a"[..])
        );
    }
    assert_eq!(scratch.entry_capacity(), retained_entries);
    assert_eq!(scratch.key_arena_capacity(), retained_arena);

    // The original owned API remains a compatibility wrapper over the packed
    // representation.
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
#[allow(
    unsafe_code,
    reason = "the test exercises the wrapper-private trusted scan contract"
)]
fn trusted_bounded_record_id_scan_keeps_bounds_and_inclusive_next_continuation() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let tree = runtime.create_tree(&worker).unwrap();
    let duplicate = RecordId::new(702).unwrap();
    for (key, record_id) in [
        (b"a".as_slice(), RecordId::new(701).unwrap()),
        (b"b".as_slice(), duplicate),
        (b"c".as_slice(), duplicate),
        (b"d".as_slice(), RecordId::new(704).unwrap()),
        (b"e".as_slice(), RecordId::new(705).unwrap()),
    ] {
        tree.get_or_insert(&worker, key, record_id).unwrap();
    }

    let mut scratch = PackedScanScratch::default();
    // SAFETY: This test owns the tree and uses the native forward bounded
    // contract through no other access path while each call runs.
    let no_continuation_space = unsafe {
        tree.scan_record_ids_bounded_reusing_trusted(&worker, b"b", b"e", 2, 0, &mut scratch)
    }
    .unwrap();
    assert_eq!(no_continuation_space.len(), 0);
    assert_eq!(
        no_continuation_space.stop_reason(),
        ScanStopReason::KeyArenaCapacity
    );
    assert_eq!(
        no_continuation_space.resume(),
        BoundedRecordIdScanResume::UnchangedInput
    );
    assert_eq!(no_continuation_space.next_key_bytes_required(), 1);

    let first = unsafe {
        tree.scan_record_ids_bounded_reusing_trusted(&worker, b"b", b"e", 2, 1, &mut scratch)
    }
    .unwrap();
    assert_eq!(
        first.record_ids().collect::<Vec<_>>(),
        [duplicate, duplicate]
    );
    assert_eq!(first.stop_reason(), ScanStopReason::EntryCapacity);
    let BoundedRecordIdScanResume::InclusiveNext(next) = first.resume() else {
        panic!("a full RecordId chunk must retain the first omitted key")
    };
    assert_eq!(next, b"d");
    let next = next.to_vec();

    let second = unsafe {
        tree.scan_record_ids_bounded_reusing_trusted(&worker, &next, b"e", 2, 1, &mut scratch)
    }
    .unwrap();
    assert_eq!(
        second.record_ids().collect::<Vec<_>>(),
        [RecordId::new(704).unwrap()]
    );
    assert_eq!(second.stop_reason(), ScanStopReason::End);
    assert_eq!(second.resume(), BoundedRecordIdScanResume::None);
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

#[test]
fn worker_rcu_scope_spans_trees_and_drops_during_unwind() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let worker = runtime.attach().unwrap();
    let first = runtime.create_tree(&worker).unwrap();
    let second = runtime.create_tree(&worker).unwrap();
    let first_id = RecordId::new(501).unwrap();
    let second_id = RecordId::new(502).unwrap();

    {
        let scope = worker.rcu_scope().unwrap();
        assert_eq!(
            first
                .get_or_insert(&worker, b"rcu/first", first_id)
                .unwrap(),
            InsertOutcome::Inserted(first_id)
        );
        assert_eq!(first.get(&worker, b"rcu/first").unwrap(), Some(first_id));
        assert_eq!(
            second
                .get_or_insert(&worker, b"rcu/second", second_id)
                .unwrap(),
            InsertOutcome::Inserted(second_id)
        );
        assert_eq!(second.get(&worker, b"rcu/second").unwrap(), Some(second_id));
        let scan = second
            .scan_chunk(&worker, ScanRequest::new(ScanDirection::Forward))
            .unwrap();
        assert_eq!(scan.entries().len(), 1);
        assert_eq!(scan.entries()[0].record_id(), second_id);

        assert_eq!(
            worker.rcu_scope().unwrap_err(),
            Error::Native(NativeStatus::ActiveGuards)
        );
        assert_eq!(
            first.read_scope(&worker).unwrap_err(),
            Error::Native(NativeStatus::ActiveGuards)
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
        scope.close().unwrap();
    }
    worker.quiesce().unwrap();

    let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _scope = worker.rcu_scope().unwrap();
        assert_eq!(first.get(&worker, b"rcu/first").unwrap(), Some(first_id));
        panic!("exercise RcuScope drop during unwind");
    }));
    assert!(unwind.is_err());
    worker.quiesce().unwrap();
}
