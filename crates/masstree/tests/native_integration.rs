#![cfg(mtree_native_integration)]

use masstree::{
    InsertOutcome, KeyBound, RecordId, Runtime, RuntimeConfig, RuntimeHealth, ScanDirection,
    ScanRequest, ScanResume, ScanStopReason,
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
