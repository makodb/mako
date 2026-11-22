use mako_clock::distributed::{DistributedTerm, DistributedTimeTerm, DistributedTimestamp};
use sto_core::{
    AbortReason, CommitOutcome, Conflict, ParticipantError, ParticipantPhase, Runtime,
    RuntimeConfig, TxnCell, TxnCounter,
};

fn decision() -> DistributedTimeTerm {
    DistributedTimeTerm::new(DistributedTimestamp::MAX, DistributedTerm::new(9).unwrap())
}

#[test]
fn incremental_counter_reads_and_repeated_writes_keep_the_same_attempt() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let counter = TxnCounter::new(&runtime, 10).unwrap();
    let cell = TxnCell::new(&runtime, 0).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut participant = worker.begin().unwrap().into_participant();
    assert!(participant.is_empty());
    participant
        .with_transaction(|txn| counter.increment(txn, 2))
        .unwrap();
    assert!(participant.has_writes());
    participant.lock_writes().unwrap();
    participant
        .with_transaction(|txn| {
            assert_eq!(counter.get(txn)?, 12);
            counter.increment(txn, 3)?;
            cell.set(txn, 1)
        })
        .unwrap();
    participant.lock_writes().unwrap();
    participant
        .with_transaction(|txn| cell.set(txn, 7))
        .unwrap();
    participant.validate().unwrap();
    participant.install(decision()).unwrap();
    assert_eq!(participant.phase(), ParticipantPhase::Installed);
    assert!(matches!(
        participant.finish(false).unwrap(),
        CommitOutcome::Committed(_)
    ));
    let mut read = worker.begin().unwrap();
    assert_eq!(counter.get(&mut read).unwrap(), 15);
    assert_eq!(cell.get(&mut read).unwrap(), 7);
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn partial_lock_conflict_releases_prior_guards_and_preserves_payloads() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let first = TxnCell::new(&runtime, 10).unwrap();
    let second = TxnCell::new(&runtime, 20).unwrap();
    let mut reader_worker = runtime.attach().unwrap();
    std::thread::scope(|scope| {
        let (locked_tx, locked_rx) = std::sync::mpsc::channel();
        let (release_tx, release_rx) = std::sync::mpsc::channel::<()>();
        let runtime = &runtime;
        let second = &second;
        let blocker = scope.spawn(move || {
            let mut worker = runtime.attach().unwrap();
            let mut participant = worker.begin().unwrap().into_participant();
            participant
                .with_transaction(|txn| second.set(txn, 21))
                .unwrap();
            participant.lock_writes().unwrap();
            locked_tx.send(()).unwrap();
            // Dropping the sender also releases this thread if an assertion
            // unwinds, so the scoped join cannot deadlock the test.
            let _ = release_rx.recv();
            participant.abort().unwrap();
        });
        locked_rx.recv().unwrap();
        let mut failed = reader_worker.begin().unwrap().into_participant();
        failed
            .with_transaction(|txn| {
                first.set(txn, 11)?;
                second.set(txn, 22)
            })
            .unwrap();
        assert_eq!(
            failed.lock_writes(),
            Err(ParticipantError::Aborted(AbortReason::Conflict(
                Conflict::LockBusy
            )))
        );
        assert_eq!(failed.phase(), ParticipantPhase::Finished);
        assert_eq!(
            failed.finish(true).unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::LockBusy))
        );
        let mut read = reader_worker.begin().unwrap();
        assert_eq!(first.get(&mut read).unwrap(), 10);
        assert!(matches!(
            read.commit().unwrap(),
            CommitOutcome::Committed(_)
        ));
        drop(release_tx);
        blocker.join().unwrap();
    });
    let mut read = reader_worker.begin().unwrap();
    assert_eq!(second.get(&mut read).unwrap(), 20);
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn installed_drop_commits_but_collecting_and_validated_drop_abort() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1).unwrap();
    let mut worker = runtime.attach().unwrap();
    for validate in [false, true] {
        let mut participant = worker.begin().unwrap().into_participant();
        participant
            .with_transaction(|txn| cell.set(txn, 2))
            .unwrap();
        participant.lock_writes().unwrap();
        if validate {
            participant.validate().unwrap();
        }
        drop(participant);
        let mut read = worker.begin().unwrap();
        assert_eq!(cell.get(&mut read).unwrap(), 1);
        read.abort();
    }
    let mut participant = worker.begin().unwrap().into_participant();
    participant
        .with_transaction(|txn| cell.set(txn, 3))
        .unwrap();
    participant.validate().unwrap();
    participant.install(decision()).unwrap();
    drop(participant);
    let mut read = worker.begin().unwrap();
    assert_eq!(cell.get(&mut read).unwrap(), 3);
    assert!(matches!(
        read.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn read_only_participant_certifies_and_terminates_without_install() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1).unwrap();
    let mut reader_worker = runtime.attach().unwrap();
    let mut participant = reader_worker.begin().unwrap().into_participant();
    assert_eq!(
        participant.with_transaction(|txn| cell.get(txn)).unwrap(),
        1
    );
    assert!(!participant.has_writes());
    std::thread::scope(|scope| {
        scope
            .spawn(|| {
                let mut writer_worker = runtime.attach().unwrap();
                let mut write = writer_worker.begin().unwrap();
                cell.set(&mut write, 2).unwrap();
                write.commit().unwrap();
            })
            .join()
            .unwrap();
    });
    assert_eq!(
        participant.validate(),
        Err(ParticipantError::Aborted(AbortReason::Conflict(
            Conflict::ReadValidation
        )))
    );
    participant.abort().unwrap();
    let mut participant = reader_worker.begin().unwrap().into_participant();
    assert_eq!(
        participant.with_transaction(|txn| cell.get(txn)).unwrap(),
        2
    );
    participant.validate().unwrap();
    assert!(matches!(
        participant.finish(true).unwrap(),
        CommitOutcome::Committed(_)
    ));
}

#[test]
fn operation_unwind_restores_frame_ownership_and_drop_releases_held_locks() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let cell = TxnCell::new(&runtime, 1).unwrap();
    let mut worker = runtime.attach().unwrap();
    let mut participant = worker.begin().unwrap().into_participant();
    participant
        .with_transaction(|txn| cell.set(txn, 2))
        .unwrap();
    participant.lock_writes().unwrap();
    let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _: Result<(), _> = participant.with_transaction(|_| panic!("operation unwind"));
    }));
    assert!(unwind.is_err());
    drop(participant);
    let mut read = worker.begin().unwrap();
    assert_eq!(cell.get(&mut read).unwrap(), 1);
    read.abort();
}
