use sto_core::{AbortReason, CapacityError, CommitOutcome, Runtime, RuntimeConfig, TxnCounter};
use sto_test_datatypes::{TxnHashMap, TxnQueue, TxnVec};

fn assert_committed(outcome: CommitOutcome) {
    assert!(matches!(outcome, CommitOutcome::Committed(_)));
}

#[test]
fn heterogeneous_transaction_commits_all_four_datatypes() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map = TxnHashMap::<u64, u64>::new(&runtime).unwrap();
    let vector = TxnVec::from_iter(&runtime, [1]).unwrap();
    let queue = TxnQueue::from_iter(&runtime, [2]).unwrap();
    let counter = TxnCounter::new(&runtime, 3).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    map.insert(&mut transaction, 4, 40).unwrap();
    vector.push(&mut transaction, 5).unwrap();
    queue.push_back(&mut transaction, 6).unwrap();
    counter.increment(&mut transaction, 7).unwrap();
    assert_committed(transaction.commit().unwrap());

    let mut transaction = worker.begin().unwrap();
    assert_eq!(map.get(&mut transaction, &4).unwrap(), Some(40));
    assert_eq!(vector.to_vec(&mut transaction).unwrap(), [1, 5]);
    assert_eq!(queue.to_vec(&mut transaction).unwrap(), [2, 6]);
    assert_eq!(counter.get(&mut transaction).unwrap(), 10);
    assert_committed(transaction.commit().unwrap());
}

#[test]
fn lock_plan_capacity_abort_publishes_none_of_four_writes() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(3)).unwrap();
    let map = TxnHashMap::<u64, u64>::new(&runtime).unwrap();
    let vector = TxnVec::from_iter(&runtime, [1]).unwrap();
    let queue = TxnQueue::from_iter(&runtime, [2]).unwrap();
    let counter = TxnCounter::new(&runtime, 3).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    map.insert(&mut transaction, 4, 40).unwrap();
    vector.push(&mut transaction, 5).unwrap();
    queue.push_back(&mut transaction, 6).unwrap();
    counter.increment(&mut transaction, 7).unwrap();
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Capacity(CapacityError::LockLimit))
    );

    let mut transaction = worker.begin().unwrap();
    assert_eq!(map.get(&mut transaction, &4).unwrap(), None);
    assert_eq!(vector.to_vec(&mut transaction).unwrap(), [1]);
    assert_eq!(queue.to_vec(&mut transaction).unwrap(), [2]);
    assert_eq!(counter.get(&mut transaction).unwrap(), 3);
    assert_committed(transaction.commit().unwrap());
}
