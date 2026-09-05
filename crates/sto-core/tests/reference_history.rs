use std::{
    sync::{Arc, Barrier},
    thread,
};

use sto_core::{
    AbortReason, AccessError, CapacityError, CommitOutcome, Conflict, Runtime, RuntimeConfig,
    TxnCounter, WorkerContext,
};

fn visit_sequences<T: Copy>(alphabet: &[T], max_len: usize, mut visit: impl FnMut(&[T])) {
    fn visit_exact<T: Copy>(
        alphabet: &[T],
        remaining: usize,
        sequence: &mut Vec<T>,
        visit: &mut impl FnMut(&[T]),
    ) {
        if remaining == 0 {
            visit(sequence);
            return;
        }

        for operation in alphabet {
            sequence.push(*operation);
            visit_exact(alphabet, remaining - 1, sequence, visit);
            sequence.pop();
        }
    }

    let mut sequence = Vec::with_capacity(max_len);
    for len in 1..=max_len {
        visit_exact(alphabet, len, &mut sequence, &mut visit);
    }
}

fn read_counter(worker: &mut WorkerContext, counter: &TxnCounter) -> i64 {
    let mut transaction = worker.begin().unwrap();
    let value = counter.get(&mut transaction).unwrap();
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    value
}

fn read_pair(worker: &mut WorkerContext, first: &TxnCounter, second: &TxnCounter) -> (i64, i64) {
    let mut transaction = worker.begin().unwrap();
    let pair = (
        first.get(&mut transaction).unwrap(),
        second.get(&mut transaction).unwrap(),
    );
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    pair
}

#[derive(Clone, Copy, Debug)]
enum CounterOperation {
    Increment(i64),
    Get,
}

#[test]
fn same_item_histories_match_a_wrapping_reference_model() {
    const OPERATIONS: &[CounterOperation] = &[
        CounterOperation::Increment(i64::MIN),
        CounterOperation::Increment(-1),
        CounterOperation::Increment(0),
        CounterOperation::Increment(1),
        CounterOperation::Increment(i64::MAX),
        CounterOperation::Get,
    ];

    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let initial = i64::MAX - 2;
    let counter = TxnCounter::new(&runtime, initial).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut committed = initial;
    let mut history_index = 0_usize;

    visit_sequences(OPERATIONS, 4, |operations| {
        let mut transaction = worker.begin().unwrap();
        let mut staged_delta = 0_i64;

        for operation in operations {
            match *operation {
                CounterOperation::Increment(delta) => {
                    counter.increment(&mut transaction, delta).unwrap();
                    staged_delta = staged_delta.wrapping_add(delta);
                }
                CounterOperation::Get => {
                    assert_eq!(
                        counter.get(&mut transaction).unwrap(),
                        committed.wrapping_add(staged_delta),
                        "history {history_index}: {operations:?}"
                    );
                }
            }
        }

        if history_index.is_multiple_of(7) {
            assert_eq!(transaction.abort().reason(), &AbortReason::Explicit);
        } else {
            assert!(matches!(
                transaction.commit().unwrap(),
                CommitOutcome::Committed(_)
            ));
            committed = committed.wrapping_add(staged_delta);
        }

        assert_eq!(
            read_counter(&mut worker, &counter),
            committed,
            "committed state after history {history_index}: {operations:?}"
        );
        history_index += 1;
    });

    assert_eq!(history_index, 1_554);
}

#[derive(Clone, Copy, Debug)]
enum PairOperation {
    IncrementFirst(i64),
    IncrementSecond(i64),
    GetFirst,
    GetSecond,
}

#[test]
fn cross_object_histories_commit_or_abort_as_one_reference_step() {
    const OPERATIONS: &[PairOperation] = &[
        PairOperation::IncrementFirst(i64::MIN),
        PairOperation::IncrementFirst(3),
        PairOperation::IncrementSecond(-5),
        PairOperation::IncrementSecond(i64::MAX),
        PairOperation::GetFirst,
        PairOperation::GetSecond,
    ];

    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let first = TxnCounter::new(&runtime, 11).unwrap();
    let second = TxnCounter::new(&runtime, -17).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut committed = (11_i64, -17_i64);
    let mut history_index = 0_usize;

    visit_sequences(OPERATIONS, 3, |operations| {
        let mut transaction = worker.begin().unwrap();
        let mut staged = (0_i64, 0_i64);

        for operation in operations {
            match *operation {
                PairOperation::IncrementFirst(delta) => {
                    first.increment(&mut transaction, delta).unwrap();
                    staged.0 = staged.0.wrapping_add(delta);
                }
                PairOperation::IncrementSecond(delta) => {
                    second.increment(&mut transaction, delta).unwrap();
                    staged.1 = staged.1.wrapping_add(delta);
                }
                PairOperation::GetFirst => assert_eq!(
                    first.get(&mut transaction).unwrap(),
                    committed.0.wrapping_add(staged.0),
                    "history {history_index}: {operations:?}"
                ),
                PairOperation::GetSecond => assert_eq!(
                    second.get(&mut transaction).unwrap(),
                    committed.1.wrapping_add(staged.1),
                    "history {history_index}: {operations:?}"
                ),
            }
        }

        if history_index.is_multiple_of(5) {
            assert_eq!(transaction.abort().reason(), &AbortReason::Explicit);
        } else {
            assert!(matches!(
                transaction.commit().unwrap(),
                CommitOutcome::Committed(_)
            ));
            committed.0 = committed.0.wrapping_add(staged.0);
            committed.1 = committed.1.wrapping_add(staged.1);
        }

        assert_eq!(
            read_pair(&mut worker, &first, &second),
            committed,
            "committed pair after history {history_index}: {operations:?}"
        );
        history_index += 1;
    });

    assert_eq!(history_index, 258);
}

#[test]
fn lock_capacity_failure_cannot_partially_publish_a_cross_object_write() {
    let runtime = Runtime::new(RuntimeConfig::new().with_max_locks_per_transaction(1)).unwrap();
    let first = TxnCounter::new(&runtime, 7).unwrap();
    let second = TxnCounter::new(&runtime, 9).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut transaction = worker.begin().unwrap();
    first.increment(&mut transaction, 10).unwrap();
    second.increment(&mut transaction, 20).unwrap();
    assert_eq!(
        transaction.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Capacity(CapacityError::LockLimit))
    );
    assert_eq!(read_pair(&mut worker, &first, &second), (7, 9));
}

#[test]
fn a_concurrent_reader_never_commits_a_torn_cross_object_snapshot() {
    let runtime = Runtime::new(RuntimeConfig::new()).unwrap();
    let first = TxnCounter::new(&runtime, 0).unwrap();
    let second = TxnCounter::new(&runtime, 0).unwrap();
    let rendezvous = Arc::new(Barrier::new(2));

    let writer_runtime = Arc::clone(&runtime);
    let writer_first = first.clone();
    let writer_second = second.clone();
    let writer_rendezvous = Arc::clone(&rendezvous);
    let writer = thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        writer_rendezvous.wait();

        let mut transaction = worker.begin().unwrap();
        writer_first.increment(&mut transaction, 1).unwrap();
        writer_second.increment(&mut transaction, 1).unwrap();
        assert!(matches!(
            transaction.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));

        writer_rendezvous.wait();
    });

    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    assert_eq!(first.get(&mut transaction).unwrap(), 0);
    rendezvous.wait();
    rendezvous.wait();

    match second.get(&mut transaction) {
        Ok(value) => {
            assert_eq!(value, 1);
            assert!(matches!(
                transaction.commit().unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(
                    Conflict::ReadValidation | Conflict::Opacity
                ))
            ));
        }
        Err(AccessError::Conflict(Conflict::ReadValidation | Conflict::Opacity)) => {
            assert!(matches!(
                transaction.commit().unwrap(),
                CommitOutcome::Aborted(AbortReason::Doomed)
            ));
        }
        Err(error) => panic!("unexpected second-read outcome: {error:?}"),
    }

    writer.join().unwrap();
    assert_eq!(read_pair(&mut worker, &first, &second), (1, 1));
}
