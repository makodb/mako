use std::num::NonZeroUsize;
use std::sync::Arc;
use std::thread;

use sto_core::{AbortReason, CommitOutcome, Conflict, Runtime, RuntimeConfig};
use sto_test_datatypes::{TxnHashMap, TxnQueue, TxnVec};

fn assert_committed(outcome: CommitOutcome) {
    assert!(matches!(outcome, CommitOutcome::Committed(_)));
}

#[test]
fn a_map_miss_is_invalidated_by_an_insert_into_the_same_bucket() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map =
        TxnHashMap::<u64, u64>::with_bucket_count(&runtime, NonZeroUsize::new(1).unwrap()).unwrap();
    let mut stale_worker = runtime.attach().unwrap();
    let mut stale = stale_worker.begin().unwrap();
    assert_eq!(map.get(&mut stale, &10).unwrap(), None);

    let writer_runtime = Arc::clone(&runtime);
    let writer_map = map.clone();
    thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        writer_map.insert(&mut transaction, 20, 1).unwrap();
        assert_committed(transaction.commit().unwrap());
    })
    .join()
    .unwrap();

    assert_eq!(
        stale.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
}

#[test]
fn a_stale_map_writer_cannot_overwrite_a_newer_bucket_snapshot() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map =
        TxnHashMap::<u64, u64>::with_bucket_count(&runtime, NonZeroUsize::new(1).unwrap()).unwrap();
    let mut stale_worker = runtime.attach().unwrap();
    let mut stale = stale_worker.begin().unwrap();
    map.insert(&mut stale, 10, 1).unwrap();

    let writer_runtime = Arc::clone(&runtime);
    let writer_map = map.clone();
    thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        writer_map.insert(&mut transaction, 20, 2).unwrap();
        assert_committed(transaction.commit().unwrap());
    })
    .join()
    .unwrap();

    assert_eq!(
        stale.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    let mut verify = stale_worker.begin().unwrap();
    assert_eq!(map.get(&mut verify, &10).unwrap(), None);
    assert_eq!(map.get(&mut verify, &20).unwrap(), Some(2));
    assert_committed(verify.commit().unwrap());
}

#[test]
fn different_map_buckets_do_not_conflict() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map = TxnHashMap::<u64, u64>::with_bucket_count(&runtime, NonZeroUsize::new(16).unwrap())
        .unwrap();
    let first = 0;
    let second = (1..10_000)
        .find(|candidate| map.bucket_index(candidate) != map.bucket_index(&first))
        .expect("sixteen buckets must separate at least one sampled key");

    let mut first_worker = runtime.attach().unwrap();
    let mut first_transaction = first_worker.begin().unwrap();
    map.insert(&mut first_transaction, first, 11).unwrap();

    let second_runtime = Arc::clone(&runtime);
    let second_map = map.clone();
    thread::spawn(move || {
        let mut worker = second_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        second_map.insert(&mut transaction, second, 22).unwrap();
        assert_committed(transaction.commit().unwrap());
    })
    .join()
    .unwrap();

    assert_committed(first_transaction.commit().unwrap());

    let mut verify = first_worker.begin().unwrap();
    assert_eq!(map.get(&mut verify, &first).unwrap(), Some(11));
    assert_eq!(map.get(&mut verify, &second).unwrap(), Some(22));
    assert_committed(verify.commit().unwrap());
}

#[test]
fn one_transaction_can_publish_multiple_map_buckets() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map = TxnHashMap::<u64, u64>::with_bucket_count(&runtime, NonZeroUsize::new(16).unwrap())
        .unwrap();
    let first = 0;
    let second = (1..10_000)
        .find(|candidate| map.bucket_index(candidate) != map.bucket_index(&first))
        .expect("sixteen buckets must separate at least one sampled key");
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    map.insert(&mut transaction, first, 11).unwrap();
    map.insert(&mut transaction, second, 22).unwrap();
    assert_committed(transaction.commit().unwrap());

    let mut verify = worker.begin().unwrap();
    assert_eq!(map.get(&mut verify, &first).unwrap(), Some(11));
    assert_eq!(map.get(&mut verify, &second).unwrap(), Some(22));
    assert_committed(verify.commit().unwrap());
}

#[test]
fn vector_snapshot_detects_a_stale_read() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let vector = TxnVec::from_iter(&runtime, [1]).unwrap();
    let mut stale_worker = runtime.attach().unwrap();
    let mut stale = stale_worker.begin().unwrap();
    assert_eq!(vector.get(&mut stale, 0).unwrap().unwrap(), 1);

    let writer_runtime = Arc::clone(&runtime);
    let writer_vector = vector.clone();
    thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        writer_vector.set(&mut transaction, 0, 2).unwrap().unwrap();
        assert_committed(transaction.commit().unwrap());
    })
    .join()
    .unwrap();

    assert_eq!(
        stale.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
}

#[test]
fn a_stale_vector_writer_cannot_overwrite_a_newer_snapshot() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let vector = TxnVec::from_iter(&runtime, [0]).unwrap();
    let mut stale_worker = runtime.attach().unwrap();
    let mut stale = stale_worker.begin().unwrap();
    assert_eq!(vector.set(&mut stale, 0, 1).unwrap().unwrap(), 0);

    let writer_runtime = Arc::clone(&runtime);
    let writer_vector = vector.clone();
    thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            writer_vector.set(&mut transaction, 0, 2).unwrap().unwrap(),
            0
        );
        assert_committed(transaction.commit().unwrap());
    })
    .join()
    .unwrap();

    assert_eq!(
        stale.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    let mut verify = stale_worker.begin().unwrap();
    assert_eq!(vector.get(&mut verify, 0).unwrap().unwrap(), 2);
    assert_committed(verify.commit().unwrap());
}

#[test]
fn queue_snapshot_detects_a_stale_read() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let queue = TxnQueue::from_iter(&runtime, [1]).unwrap();
    let mut stale_worker = runtime.attach().unwrap();
    let mut stale = stale_worker.begin().unwrap();
    assert_eq!(queue.front(&mut stale).unwrap(), Some(1));

    let writer_runtime = Arc::clone(&runtime);
    let writer_queue = queue.clone();
    thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        writer_queue.push_back(&mut transaction, 2).unwrap();
        assert_committed(transaction.commit().unwrap());
    })
    .join()
    .unwrap();

    assert_eq!(
        stale.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
}
