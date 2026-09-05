use std::{
    sync::{
        mpsc::{self, Receiver, Sender},
        Arc,
    },
    thread::{self, JoinHandle},
    time::Duration,
};

use sto_core::{AbortReason, CommitOutcome, Conflict, Runtime, RuntimeConfig, TxnArray};
use sto_test_datatypes::{TxnHashMap, TxnVec};

const COORDINATION_TIMEOUT: Duration = Duration::from_secs(30);

fn assert_committed(outcome: CommitOutcome) {
    assert!(
        matches!(outcome, CommitOutcome::Committed(_)),
        "expected a committed transaction, got {outcome:?}"
    );
}

fn spawn_write_skew_worker(
    runtime: Arc<Runtime>,
    values: TxnArray<i64>,
    write_index: usize,
    ready: Sender<usize>,
    release: Receiver<()>,
) -> JoinHandle<CommitOutcome> {
    thread::spawn(move || {
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let x = values.get(&mut transaction, 0).unwrap().unwrap();
        let y = values.get(&mut transaction, 1).unwrap().unwrap();
        assert_eq!((x, y), (1, 1));

        if x == 1 && y == 1 {
            values
                .set(&mut transaction, write_index, 0)
                .unwrap()
                .unwrap();
        }

        ready.send(write_index).unwrap();
        release
            .recv_timeout(COORDINATION_TIMEOUT)
            .expect("write-skew worker timed out waiting for commit release");
        transaction.commit().unwrap()
    })
}

#[test]
fn write_skew_across_independent_items_cannot_commit_both_writers() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    // TxnArray slots have independent versions and locks. This makes the test
    // exercise read validation, rather than a coincidental write/write lock
    // conflict between x and y.
    let values = TxnArray::new(&runtime, [1_i64, 1]).unwrap();
    let (ready_sender, ready_receiver) = mpsc::channel();
    let (release_first_sender, release_first_receiver) = mpsc::channel();
    let (release_second_sender, release_second_receiver) = mpsc::channel();

    let first = spawn_write_skew_worker(
        Arc::clone(&runtime),
        values.clone(),
        0,
        ready_sender.clone(),
        release_first_receiver,
    );
    let second = spawn_write_skew_worker(
        Arc::clone(&runtime),
        values.clone(),
        1,
        ready_sender,
        release_second_receiver,
    );

    let mut ready = [
        ready_receiver
            .recv_timeout(COORDINATION_TIMEOUT)
            .expect("timed out waiting for the first write-skew worker"),
        ready_receiver
            .recv_timeout(COORDINATION_TIMEOUT)
            .expect("timed out waiting for the second write-skew worker"),
    ];
    ready.sort_unstable();
    assert_eq!(ready, [0, 1]);

    // Both transactions have read x=y=1 and staged disjoint writes. Commit
    // the first while the second remains active, then certify the stale
    // second transaction. Their lifetimes overlap, so committing both would
    // be the classic write-skew cycle.
    release_first_sender.send(()).unwrap();
    let first_outcome = first.join().unwrap();
    assert_committed(first_outcome);

    release_second_sender.send(()).unwrap();
    let second_outcome = second.join().unwrap();
    assert_eq!(
        second_outcome,
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );
    assert!(!matches!(
        (first_outcome, second_outcome),
        (CommitOutcome::Committed(_), CommitOutcome::Committed(_))
    ));

    let mut worker = runtime.attach().unwrap();
    let mut verify = worker.begin().unwrap();
    let x = values.get(&mut verify, 0).unwrap().unwrap();
    let y = values.get(&mut verify, 1).unwrap().unwrap();
    assert_eq!((x, y), (0, 1));
    assert!(x + y >= 1, "write skew broke the x + y >= 1 invariant");
    assert_committed(verify.commit().unwrap());
}

#[test]
fn a_staged_write_is_not_visible_and_explicit_abort_discards_it() {
    const OLD: i64 = 7;
    const UNCOMMITTED: i64 = 91_337;

    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let vector = TxnVec::from_iter(&runtime, [OLD]).unwrap();
    let (staged_sender, staged_receiver) = mpsc::channel();
    let (abort_sender, abort_receiver) = mpsc::channel();

    let writer_runtime = Arc::clone(&runtime);
    let writer_vector = vector.clone();
    let writer = thread::spawn(move || {
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            writer_vector
                .set(&mut transaction, 0, UNCOMMITTED)
                .unwrap()
                .unwrap(),
            OLD
        );
        staged_sender.send(()).unwrap();
        abort_receiver
            .recv_timeout(COORDINATION_TIMEOUT)
            .expect("writer timed out waiting for explicit-abort release");
        transaction.abort()
    });

    staged_receiver
        .recv_timeout(COORDINATION_TIMEOUT)
        .expect("timed out waiting for the writer to stage its value");
    let mut reader_worker = runtime.attach().unwrap();
    let mut reader = reader_worker.begin().unwrap();
    // This is not an opacity assertion. The writer has not installed its
    // intent, so another transaction must read the committed value and may
    // commit normally while the writer remains active.
    assert_eq!(vector.get(&mut reader, 0).unwrap().unwrap(), OLD);
    assert_committed(reader.commit().unwrap());

    abort_sender.send(()).unwrap();
    let abort = writer.join().unwrap();
    assert_eq!(abort.reason(), &AbortReason::Explicit);

    let mut verify = reader_worker.begin().unwrap();
    assert_eq!(vector.get(&mut verify, 0).unwrap().unwrap(), OLD);
    assert_committed(verify.commit().unwrap());
}

#[test]
fn a_heterogeneous_mixed_snapshot_cannot_commit() {
    const KEY: u64 = 4;
    const OLD: i64 = 11;
    const NEW: i64 = 29;

    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let map = TxnHashMap::<u64, i64>::new(&runtime).unwrap();
    let vector = TxnVec::from_iter(&runtime, [OLD]).unwrap();
    let mut reader_worker = runtime.attach().unwrap();

    let mut initialize = reader_worker.begin().unwrap();
    assert_eq!(map.insert(&mut initialize, KEY, OLD).unwrap(), None);
    assert_committed(initialize.commit().unwrap());

    let (start_writer_sender, start_writer_receiver) = mpsc::channel();
    let writer_runtime = Arc::clone(&runtime);
    let writer_map = map.clone();
    let writer_vector = vector.clone();
    let writer = thread::spawn(move || {
        start_writer_receiver
            .recv_timeout(COORDINATION_TIMEOUT)
            .expect("heterogeneous writer timed out waiting for its release");
        let mut worker = writer_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            writer_map.insert(&mut transaction, KEY, NEW).unwrap(),
            Some(OLD)
        );
        assert_eq!(
            writer_vector
                .set(&mut transaction, 0, NEW)
                .unwrap()
                .unwrap(),
            OLD
        );
        transaction.commit().unwrap()
    });

    let mut reader = reader_worker.begin().unwrap();
    let map_value = map.get(&mut reader, &KEY).unwrap().unwrap();
    assert_eq!(map_value, OLD);

    start_writer_sender.send(()).unwrap();
    assert_committed(writer.join().unwrap());

    // Serializable mode is deliberately nonopaque. The active reader may
    // observe this old/new pair during execution, but final validation must
    // prevent that fractured view from becoming a committed transaction.
    let vector_value = vector.get(&mut reader, 0).unwrap().unwrap();
    assert_eq!((map_value, vector_value), (OLD, NEW));
    assert_eq!(
        reader.commit().unwrap(),
        CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
    );

    let mut verify = reader_worker.begin().unwrap();
    assert_eq!(map.get(&mut verify, &KEY).unwrap(), Some(NEW));
    assert_eq!(vector.get(&mut verify, 0).unwrap().unwrap(), NEW);
    assert_committed(verify.commit().unwrap());
}
