use std::num::NonZeroUsize;

use sto_core::{CommitOutcome, Runtime, RuntimeConfig};
use sto_test_datatypes::{TxnHashMap, TxnQueue, TxnVec};

fn assert_send_sync<T: Send + Sync>() {}

fn assert_committed(outcome: CommitOutcome) {
    assert!(matches!(outcome, CommitOutcome::Committed(_)));
}

#[test]
fn handles_are_send_sync_and_clones_preserve_object_identity() {
    assert_send_sync::<TxnHashMap<u64, String>>();
    assert_send_sync::<TxnVec<String>>();
    assert_send_sync::<TxnQueue<String>>();

    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map = TxnHashMap::<u64, String>::with_bucket_count(&runtime, NonZeroUsize::new(4).unwrap())
        .unwrap();
    let vector = TxnVec::from_iter(&runtime, [String::from("a")]).unwrap();
    let queue = TxnQueue::from_iter(&runtime, [String::from("b")]).unwrap();

    assert_eq!(map.object_id(), map.clone().object_id());
    assert_eq!(vector.object_id(), vector.clone().object_id());
    assert_eq!(queue.object_id(), queue.clone().object_id());
    assert_ne!(map.object_id(), vector.object_id());
    assert_ne!(vector.object_id(), queue.object_id());
}

#[test]
fn map_operations_compose_and_abort_leaves_no_trace() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let mut worker = runtime.attach().unwrap();
    let map = TxnHashMap::<u64, String>::new(&runtime).unwrap();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(map.get(&mut transaction, &7).unwrap(), None);
    assert_eq!(
        map.insert(&mut transaction, 7, String::from("a")).unwrap(),
        None
    );
    assert_eq!(map.get(&mut transaction, &7).unwrap().as_deref(), Some("a"));
    assert_eq!(
        map.insert(&mut transaction, 7, String::from("b"))
            .unwrap()
            .as_deref(),
        Some("a")
    );
    assert!(map.contains_key(&mut transaction, &7).unwrap());
    assert_eq!(
        map.remove(&mut transaction, &7).unwrap().as_deref(),
        Some("b")
    );
    assert_eq!(
        map.insert(&mut transaction, 7, String::from("c")).unwrap(),
        None
    );
    assert_eq!(map.len(&mut transaction).unwrap(), 1);
    assert_committed(transaction.commit().unwrap());

    let mut transaction = worker.begin().unwrap();
    assert_eq!(
        map.remove(&mut transaction, &7).unwrap().as_deref(),
        Some("c")
    );
    map.insert(&mut transaction, 9, String::from("temporary"))
        .unwrap();
    transaction.abort();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(map.get(&mut transaction, &7).unwrap().as_deref(), Some("c"));
    assert_eq!(map.get(&mut transaction, &9).unwrap(), None);
    assert_eq!(map.to_btree_map(&mut transaction).unwrap().len(), 1);
    assert_committed(transaction.commit().unwrap());
}

#[test]
fn vector_sequence_operations_have_transactional_bounds() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let mut worker = runtime.attach().unwrap();
    let vector = TxnVec::from_iter(&runtime, [10, 20]).unwrap();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(vector.len(&mut transaction).unwrap(), 2);
    let get_error = vector.get(&mut transaction, 2).unwrap().unwrap_err();
    assert_eq!(get_error.index(), 2);
    assert_eq!(get_error.vector_len(), 2);
    assert_eq!(
        vector.set(&mut transaction, 2, 99).unwrap().unwrap_err(),
        get_error
    );
    let insert_error = vector.insert(&mut transaction, 3, 99).unwrap().unwrap_err();
    assert_eq!(insert_error.index(), 3);
    assert_eq!(insert_error.vector_len(), 2);
    assert_eq!(
        vector
            .remove(&mut transaction, usize::MAX)
            .unwrap()
            .unwrap_err()
            .vector_len(),
        2
    );
    assert!(!transaction.is_doomed());
    vector.push(&mut transaction, 30).unwrap();
    assert_eq!(vector.set(&mut transaction, 1, 21).unwrap().unwrap(), 20);
    vector.insert(&mut transaction, 1, 15).unwrap().unwrap();
    assert_eq!(vector.remove(&mut transaction, 0).unwrap().unwrap(), 10);
    assert_eq!(vector.pop(&mut transaction).unwrap(), Some(30));
    assert_eq!(vector.to_vec(&mut transaction).unwrap(), [15, 21]);
    assert_committed(transaction.commit().unwrap());

    let mut transaction = worker.begin().unwrap();
    vector.push(&mut transaction, 99).unwrap();
    drop(transaction);

    let mut transaction = worker.begin().unwrap();
    assert_eq!(vector.to_vec(&mut transaction).unwrap(), [15, 21]);
    assert_committed(transaction.commit().unwrap());
}

#[test]
fn queue_is_fifo_and_staged_changes_can_be_aborted() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let mut worker = runtime.attach().unwrap();
    let queue = TxnQueue::from_iter(&runtime, [1, 2]).unwrap();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(queue.front(&mut transaction).unwrap(), Some(1));
    assert_eq!(queue.back(&mut transaction).unwrap(), Some(2));
    queue.push_back(&mut transaction, 3).unwrap();
    assert_eq!(queue.pop_front(&mut transaction).unwrap(), Some(1));
    assert_eq!(queue.to_vec(&mut transaction).unwrap(), [2, 3]);
    assert_committed(transaction.commit().unwrap());

    let mut transaction = worker.begin().unwrap();
    assert_eq!(queue.pop_front(&mut transaction).unwrap(), Some(2));
    queue.push_back(&mut transaction, 4).unwrap();
    transaction.abort();

    let mut transaction = worker.begin().unwrap();
    assert_eq!(queue.to_vec(&mut transaction).unwrap(), [2, 3]);
    assert_committed(transaction.commit().unwrap());
}
