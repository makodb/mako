use std::collections::BTreeSet;
use std::sync::Arc;

use mako_history::{
    check_opacity, check_strict_serializability, state_insert, CheckFailureKind, CheckOptions,
    History, Interval, LogicalClock, Observation, Operation, Row, ScanDirection, State,
    TerminalCall, TerminalOutcome, TimedOperation, Transaction,
};

const TABLE: u64 = 7;

fn options() -> CheckOptions {
    CheckOptions::default()
}

fn commit(transaction: &mut Transaction, invocation: u64, response: u64) {
    transaction.finish(TerminalCall::commit(
        invocation,
        response,
        TerminalOutcome::Committed,
    ));
}

#[test]
fn legal_serial_history_models_ryw_scans_and_binary_bytes() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"a\0".to_vec(), b"old".to_vec());
    state_insert(&mut initial, TABLE, b"b".to_vec(), Vec::new());
    state_insert(&mut initial, 99, b"a\0".to_vec(), b"other-table".to_vec());

    let mut transaction = Transaction::new(1, Interval::completed(1, 2));
    transaction
        .push(TimedOperation::completed(
            3,
            4,
            Operation::get(TABLE, b"a\0".to_vec()),
            Observation::Get(Some(b"old".to_vec())),
        ))
        .push(TimedOperation::completed(
            5,
            6,
            Operation::put(TABLE, b"a\0".to_vec(), b"new\xff".to_vec()),
            Observation::Put { created: false },
        ))
        .push(TimedOperation::completed(
            7,
            8,
            Operation::get(TABLE, b"a\0".to_vec()),
            Observation::Get(Some(b"new\xff".to_vec())),
        ))
        .push(TimedOperation::completed(
            9,
            10,
            Operation::remove(TABLE, b"a\0".to_vec()),
            Observation::Remove { existed: true },
        ))
        .push(TimedOperation::completed(
            11,
            12,
            Operation::insert(TABLE, b"a\0".to_vec(), b"final\0".to_vec()),
            Observation::Insert { inserted: true },
        ))
        .push(TimedOperation::completed(
            13,
            14,
            Operation::scan(TABLE, Vec::new(), None, ScanDirection::Forward),
            Observation::Scan(vec![
                Row::new(b"a\0".to_vec(), b"final\0".to_vec()),
                Row::new(b"b".to_vec(), Vec::new()),
            ]),
        ))
        .push(TimedOperation::completed(
            15,
            16,
            Operation::scan(
                TABLE,
                Vec::new(),
                Some(b"c".to_vec()),
                ScanDirection::Reverse,
            ),
            Observation::Scan(vec![
                Row::new(b"b".to_vec(), Vec::new()),
                Row::new(b"a\0".to_vec(), b"final\0".to_vec()),
            ]),
        ));
    commit(&mut transaction, 17, 18);

    let mut final_state = initial.clone();
    state_insert(
        &mut final_state,
        TABLE,
        b"a\0".to_vec(),
        b"final\0".to_vec(),
    );
    let mut history = History::new(initial);
    history
        .set_observed_final_state(final_state)
        .push(transaction);

    let strict = check_strict_serializability(&history, options()).unwrap();
    assert_eq!(strict.serialization, vec![1]);
    let opaque = check_opacity(&history, options()).unwrap();
    assert_eq!(opaque.serialization, vec![1]);
    assert!(opaque.prefixes_checked >= 9);
}

#[test]
fn exact_previous_value_observations_are_checked() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"k", b"old");

    let mut transaction = Transaction::new(1, Interval::completed(1, 2));
    transaction
        .push(TimedOperation::completed(
            3,
            4,
            Operation::put(TABLE, b"k", b"new"),
            Observation::PutWithPrevious {
                previous: Some(b"old".to_vec()),
            },
        ))
        .push(TimedOperation::completed(
            5,
            6,
            Operation::insert(TABLE, b"k", b"ignored"),
            Observation::InsertWithPrevious {
                previous: Some(b"new".to_vec()),
            },
        ))
        .push(TimedOperation::completed(
            7,
            8,
            Operation::remove(TABLE, b"k"),
            Observation::RemoveWithPrevious {
                previous: Some(b"new".to_vec()),
            },
        ))
        .push(TimedOperation::completed(
            9,
            10,
            Operation::insert(TABLE, b"k", b"final"),
            Observation::InsertWithPrevious { previous: None },
        ))
        .push(TimedOperation::completed(
            11,
            12,
            Operation::remove(TABLE, b"missing"),
            Observation::RemoveWithPrevious { previous: None },
        ));
    commit(&mut transaction, 13, 14);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"k", b"final");
    let mut history = History::new(initial);
    history
        .set_observed_final_state(final_state)
        .push(transaction);
    check_strict_serializability(&history, options()).unwrap();
    assert!(history.to_replay_text().contains("previous=0x6f6c64"));

    for (index, corrupted_observation) in [
        Observation::PutWithPrevious {
            previous: Some(b"wrong".to_vec()),
        },
        Observation::InsertWithPrevious {
            previous: Some(b"wrong".to_vec()),
        },
        Observation::RemoveWithPrevious {
            previous: Some(b"wrong".to_vec()),
        },
    ]
    .into_iter()
    .enumerate()
    {
        let mut corrupted = history.clone();
        corrupted.transactions[0].operations[index].observation = Some(corrupted_observation);
        assert_eq!(
            check_strict_serializability(&corrupted, options())
                .unwrap_err()
                .kind,
            CheckFailureKind::NoLegalSerialization
        );
    }
}

#[test]
fn commit_response_order_is_not_serialization_order() {
    let mut first = Transaction::new(1, Interval::completed(1, 2));
    first.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"y".to_vec(), b"1".to_vec()),
        Observation::Put { created: true },
    ));
    // T1 installs while this call is in flight, but its response is delayed
    // until after T2 has read the installed value and returned from commit.
    commit(&mut first, 5, 14);

    let mut second = Transaction::new(2, Interval::completed(6, 7));
    second.push(TimedOperation::completed(
        10,
        11,
        Operation::get(TABLE, b"y".to_vec()),
        Observation::Get(Some(b"1".to_vec())),
    ));
    commit(&mut second, 12, 13);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"y".to_vec(), b"1".to_vec());
    let mut history = History::new(State::new());
    history
        .set_observed_final_state(final_state)
        // Store them opposite the expected order as an additional guard.
        .push(second)
        .push(first);

    let witness = check_strict_serializability(&history, options()).unwrap();
    assert_eq!(witness.serialization, vec![1, 2]);
    check_opacity(&history, options()).unwrap();
}

#[test]
fn committed_read_write_cycle_is_rejected() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"x".to_vec(), b"0".to_vec());
    state_insert(&mut initial, TABLE, b"y".to_vec(), b"0".to_vec());

    let mut left = Transaction::new(1, Interval::completed(1, 2));
    left.push(TimedOperation::completed(
        5,
        6,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Get(Some(b"0".to_vec())),
    ));
    left.push(TimedOperation::completed(
        9,
        10,
        Operation::put(TABLE, b"y".to_vec(), b"1".to_vec()),
        Observation::Put { created: false },
    ));
    commit(&mut left, 13, 15);

    let mut right = Transaction::new(2, Interval::completed(3, 4));
    right.push(TimedOperation::completed(
        7,
        8,
        Operation::get(TABLE, b"y".to_vec()),
        Observation::Get(Some(b"0".to_vec())),
    ));
    right.push(TimedOperation::completed(
        11,
        12,
        Operation::put(TABLE, b"x".to_vec(), b"1".to_vec()),
        Observation::Put { created: false },
    ));
    commit(&mut right, 14, 16);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"x".to_vec(), b"1".to_vec());
    state_insert(&mut final_state, TABLE, b"y".to_vec(), b"1".to_vec());
    let mut history = History::new(initial);
    history
        .set_observed_final_state(final_state)
        .push(left)
        .push(right);

    let error = check_strict_serializability(&history, options()).unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::NoLegalSerialization);
    assert!(error.detail.contains("GET") || error.detail.contains("final state"));
    assert!(error.replay.contains("mako-history-v1"));
}

#[test]
fn real_time_precedence_rejects_a_stale_read() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"x".to_vec(), b"0".to_vec());

    let mut writer = Transaction::new(1, Interval::completed(1, 2));
    writer.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"x".to_vec(), b"1".to_vec()),
        Observation::Put { created: false },
    ));
    commit(&mut writer, 5, 6);

    let mut stale = Transaction::new(2, Interval::completed(7, 8));
    stale.push(TimedOperation::completed(
        9,
        10,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Get(Some(b"0".to_vec())),
    ));
    commit(&mut stale, 11, 12);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"x".to_vec(), b"1".to_vec());
    let mut history = History::new(initial);
    history
        .set_observed_final_state(final_state)
        // Reverse storage cannot defeat the response-before-begin edge.
        .push(stale)
        .push(writer);

    let error = check_strict_serializability(&history, options()).unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::NoLegalSerialization);
}

#[test]
fn committed_phantom_dependency_cycle_is_rejected() {
    let mut scanner = Transaction::new(1, Interval::completed(1, 2));
    scanner.push(TimedOperation::completed(
        5,
        6,
        Operation::scan(
            TABLE,
            b"p/".to_vec(),
            Some(b"p0".to_vec()),
            ScanDirection::Forward,
        ),
        Observation::Scan(Vec::new()),
    ));
    scanner.push(TimedOperation::completed(
        9,
        10,
        Operation::put(TABLE, b"guard".to_vec(), b"1".to_vec()),
        Observation::Put { created: true },
    ));
    commit(&mut scanner, 13, 15);

    let mut inserter = Transaction::new(2, Interval::completed(3, 4));
    inserter.push(TimedOperation::completed(
        7,
        8,
        Operation::get(TABLE, b"guard".to_vec()),
        Observation::Get(None),
    ));
    inserter.push(TimedOperation::completed(
        11,
        12,
        Operation::put(TABLE, b"p/new".to_vec(), b"row".to_vec()),
        Observation::Put { created: true },
    ));
    commit(&mut inserter, 14, 16);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"guard".to_vec(), b"1".to_vec());
    state_insert(&mut final_state, TABLE, b"p/new".to_vec(), b"row".to_vec());
    let mut history = History::new(State::new());
    history
        .set_observed_final_state(final_state)
        .push(scanner)
        .push(inserter);

    assert_eq!(
        check_strict_serializability(&history, options())
            .unwrap_err()
            .kind,
        CheckFailureKind::NoLegalSerialization
    );
}

#[test]
fn aborted_observations_are_ignored_by_strict_but_rejected_by_opacity() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"x".to_vec(), b"0".to_vec());

    let mut live_writer = Transaction::new(1, Interval::completed(1, 2));
    live_writer.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"x".to_vec(), b"1".to_vec()),
        Observation::Put { created: false },
    ));

    let mut aborted_reader = Transaction::new(2, Interval::completed(5, 6));
    aborted_reader.push(TimedOperation::completed(
        7,
        8,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Get(Some(b"1".to_vec())),
    ));
    aborted_reader.finish(TerminalCall::abort(9, 10));

    let mut history = History::new(initial.clone());
    history
        .set_observed_final_state(initial)
        .push(live_writer)
        .push(aborted_reader);

    check_strict_serializability(&history, options()).unwrap();
    let error = check_opacity(&history, options()).unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::NoLegalSerialization);
    assert_eq!(error.prefix_tick, Some(8));
}

#[test]
fn incompatible_inflight_observations_are_an_opacity_violation() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"x".to_vec(), b"0".to_vec());

    let mut reader = Transaction::new(1, Interval::completed(1, 2));
    reader.push(TimedOperation::completed(
        3,
        4,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Get(Some(b"0".to_vec())),
    ));
    reader.push(TimedOperation::completed(
        11,
        12,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Get(Some(b"1".to_vec())),
    ));

    let mut writer = Transaction::new(2, Interval::completed(5, 6));
    writer.push(TimedOperation::completed(
        7,
        8,
        Operation::put(TABLE, b"x".to_vec(), b"1".to_vec()),
        Observation::Put { created: false },
    ));
    commit(&mut writer, 9, 10);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"x".to_vec(), b"1".to_vec());
    let mut history = History::new(initial);
    history
        .set_observed_final_state(final_state)
        .push(reader)
        .push(writer);

    check_strict_serializability(&history, options()).unwrap();
    let error = check_opacity(&history, options()).unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::NoLegalSerialization);
    assert_eq!(error.prefix_tick, Some(12));
}

#[test]
fn pending_commit_may_be_completed_as_committed_for_opacity() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"x".to_vec(), b"0".to_vec());

    let mut pending_writer = Transaction::new(1, Interval::completed(1, 2));
    pending_writer.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"x".to_vec(), b"1".to_vec()),
        Observation::Put { created: false },
    ));
    pending_writer.finish(TerminalCall::pending_commit(5));

    let mut observer = Transaction::new(2, Interval::completed(6, 7));
    observer.push(TimedOperation::completed(
        8,
        9,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Get(Some(b"1".to_vec())),
    ));
    commit(&mut observer, 10, 11);

    let mut final_state = State::new();
    state_insert(&mut final_state, TABLE, b"x".to_vec(), b"1".to_vec());
    let mut history = History::new(initial);
    history
        .set_observed_final_state(final_state)
        .push(pending_writer)
        .push(observer);

    let witness = check_opacity(&history, options()).unwrap();
    assert_eq!(witness.serialization, vec![1, 2]);
    assert_eq!(witness.pending_commits_applied, vec![1]);
}

#[test]
fn malformed_intervals_and_observations_fail_before_search() {
    let mut malformed = Transaction::new(1, Interval::completed(1, 2));
    malformed.push(TimedOperation::completed(
        2,
        3,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Put { created: true },
    ));
    let mut history = History::new(State::new());
    history.push(malformed);

    let error = check_strict_serializability(&history, options()).unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::MalformedHistory);
    assert!(error.detail.contains("before the prior response"));

    let mut mismatched = Transaction::new(2, Interval::completed(4, 5));
    mismatched.push(TimedOperation::completed(
        6,
        7,
        Operation::get(TABLE, b"x".to_vec()),
        Observation::Put { created: true },
    ));
    let mut history = History::new(State::new());
    history.push(mismatched);
    let error = check_opacity(&history, options()).unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::MalformedHistory);
    assert!(error.detail.contains("incompatible observation"));
}

#[test]
fn search_budget_exhaustion_is_never_reported_as_success() {
    let mut transaction = Transaction::new(1, Interval::completed(1, 2));
    commit(&mut transaction, 3, 4);
    let mut history = History::new(State::new());
    history
        .set_observed_final_state(State::new())
        .push(transaction);

    let error = check_strict_serializability(
        &history,
        CheckOptions {
            max_search_nodes: 1,
        },
    )
    .unwrap_err();
    assert_eq!(error.kind, CheckFailureKind::SearchBudgetExceeded);
    assert_eq!(error.explored_nodes, 1);
}

#[test]
fn operation_conflict_preserves_prior_observations_without_applying_writes() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"x".to_vec(), b"0".to_vec());
    let mut aborted = Transaction::new(1, Interval::completed(1, 2));
    aborted
        .push(TimedOperation::completed(
            3,
            4,
            Operation::get(TABLE, b"x".to_vec()),
            Observation::Get(Some(b"0".to_vec())),
        ))
        .push(TimedOperation::completed(
            5,
            6,
            Operation::put(TABLE, b"x".to_vec(), b"1".to_vec()),
            Observation::Conflict,
        ));
    let mut history = History::new(initial.clone());
    history.set_observed_final_state(initial).push(aborted);
    check_opacity(&history, options()).unwrap();
}

#[test]
fn logical_clock_is_unique_across_recording_threads() {
    let clock = Arc::new(LogicalClock::default());
    let mut workers = Vec::new();
    for _ in 0..4 {
        let clock = Arc::clone(&clock);
        workers.push(std::thread::spawn(move || {
            (0..100).map(|_| clock.next()).collect::<Vec<_>>()
        }));
    }
    let ticks: BTreeSet<_> = workers
        .into_iter()
        .flat_map(|worker| worker.join().unwrap())
        .collect();
    assert_eq!(ticks.len(), 400);
    assert_eq!(ticks.first(), Some(&1));
    assert_eq!(ticks.last(), Some(&400));
}
