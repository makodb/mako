use mako_history::{
    check_application, state_insert, ApplicationCheckFailureKind, ApplicationCommit,
    ApplicationCommitOutcome, ApplicationHistory, BackendAttempt, BackendAttemptOutcome, CacheSeq,
    CheckOptions, FrontierObservation, History, Interval, ModelMutation, Observation, Operation,
    Semantics, State, TerminalCall, TerminalOutcome, TimedOperation, Transaction,
    WaitAppliedObservation, WaitAppliedOutcome,
};

const TABLE: u64 = 7;

fn seq(value: u64) -> CacheSeq {
    CacheSeq::new(value).unwrap()
}

fn check(
    history: &ApplicationHistory,
) -> Result<mako_history::ApplicationWitness, mako_history::ApplicationCheckFailure> {
    check_application(history, Semantics::Opacity, CheckOptions::default())
}

fn two_ordered_writes() -> ApplicationHistory {
    let mut first = Transaction::new(1, Interval::completed(1, 2));
    first.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"x\0".to_vec(), b"one\xff".to_vec()),
        Observation::Put { created: true },
    ));
    // T1 installs while its response is delayed. T2 observes the install and
    // returns from commit before T1, so response order is deliberately not the
    // serialization/cache order.
    first.finish(TerminalCall::commit(5, 18, TerminalOutcome::Committed));

    let mut second = Transaction::new(2, Interval::completed(6, 7));
    second
        .push(TimedOperation::completed(
            10,
            11,
            Operation::get(TABLE, b"x\0".to_vec()),
            Observation::Get(Some(b"one\xff".to_vec())),
        ))
        .push(TimedOperation::completed(
            12,
            13,
            Operation::put(TABLE, b"y\xff".to_vec(), b"two\0".to_vec()),
            Observation::Put { created: true },
        ));
    second.finish(TerminalCall::commit(14, 17, TerminalOutcome::Committed));

    let mut final_state = State::new();
    state_insert(
        &mut final_state,
        TABLE,
        b"x\0".to_vec(),
        b"one\xff".to_vec(),
    );
    state_insert(
        &mut final_state,
        TABLE,
        b"y\xff".to_vec(),
        b"two\0".to_vec(),
    );
    let mut transactions = History::new(State::new());
    transactions
        .set_observed_final_state(final_state.clone())
        .push(second)
        .push(first);

    let mut history = ApplicationHistory::new(transactions);
    history.commits.extend([
        ApplicationCommit::new(
            2,
            Interval::completed(14, 17),
            ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(2) },
        ),
        ApplicationCommit::new(
            1,
            Interval::completed(5, 18),
            ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(1) },
        ),
    ]);
    history.backend_attempts.extend([
        BackendAttempt::new(
            seq(1),
            Interval::completed(20, 21),
            vec![ModelMutation::put(TABLE, b"x\0", b"one\xff")],
            BackendAttemptOutcome::Failed,
        ),
        BackendAttempt::new(
            seq(1),
            Interval::completed(22, 23),
            vec![ModelMutation::put(TABLE, b"x\0", b"one\xff")],
            BackendAttemptOutcome::Succeeded,
        ),
        BackendAttempt::new(
            seq(2),
            Interval::completed(24, 25),
            vec![ModelMutation::put(TABLE, b"y\xff", b"two\0")],
            BackendAttemptOutcome::Succeeded,
        ),
    ]);
    history.frontiers.push(FrontierObservation {
        interval: Interval::completed(26, 27),
        highest_acknowledged: Some(seq(2)),
        applied: Some(seq(2)),
        visible_state: Some(final_state.clone()),
        backend_state: Some(final_state),
    });
    history.waits.push(WaitAppliedObservation {
        interval: Interval::completed(28, 29),
        outcome: WaitAppliedOutcome::Ok {
            target: Some(seq(2)),
        },
    });
    history
}

#[test]
fn full_application_history_accepts_response_reordering_retry_and_binary_batches() {
    let history = two_ordered_writes();
    check_application(
        &history,
        Semantics::StrictSerializability,
        CheckOptions::default(),
    )
    .unwrap();
    let witness = check(&history).unwrap();
    assert_eq!(witness.cache_order.serialization, vec![1, 2]);
    assert_eq!(witness.successful_backend_prefix, 2);
}

#[test]
fn terminal_noop_conflict_and_abort_allocate_no_sequences() {
    let mut no_op = Transaction::new(1, Interval::completed(1, 2));
    no_op.push(TimedOperation::completed(
        3,
        4,
        Operation::get(TABLE, b"missing".to_vec()),
        Observation::Get(None),
    ));
    no_op.finish(TerminalCall::commit(5, 6, TerminalOutcome::Committed));

    let mut conflict = Transaction::new(2, Interval::completed(7, 8));
    conflict.push(TimedOperation::completed(
        9,
        10,
        Operation::put(TABLE, b"c".to_vec(), b"value".to_vec()),
        Observation::Put { created: true },
    ));
    conflict.finish(TerminalCall::commit(11, 12, TerminalOutcome::Conflict));

    let mut aborted = Transaction::new(3, Interval::completed(13, 14));
    aborted.push(TimedOperation::completed(
        15,
        16,
        Operation::put(TABLE, b"a".to_vec(), b"value".to_vec()),
        Observation::Put { created: true },
    ));
    aborted.finish(TerminalCall::abort(17, 18));

    let mut transactions = History::new(State::new());
    transactions
        .set_observed_final_state(State::new())
        .push(aborted)
        .push(no_op)
        .push(conflict);
    let mut history = ApplicationHistory::new(transactions);
    history.commits.extend([
        ApplicationCommit::new(
            1,
            Interval::completed(5, 6),
            ApplicationCommitOutcome::AcknowledgedNoWrite,
        ),
        ApplicationCommit::new(
            2,
            Interval::completed(11, 12),
            ApplicationCommitOutcome::Conflict,
        ),
        ApplicationCommit::new(
            3,
            Interval::completed(17, 18),
            ApplicationCommitOutcome::Aborted,
        ),
    ]);

    let witness = check(&history).unwrap();
    assert_eq!(witness.successful_backend_prefix, 0);
}

#[test]
fn same_key_operations_collapse_to_the_final_ryw_mutation() {
    let mut initial = State::new();
    state_insert(&mut initial, TABLE, b"key".to_vec(), b"old".to_vec());
    let mut transaction = Transaction::new(1, Interval::completed(1, 2));
    transaction
        .push(TimedOperation::completed(
            3,
            4,
            Operation::put(TABLE, b"key".to_vec(), b"first".to_vec()),
            Observation::PutWithPrevious {
                previous: Some(b"old".to_vec()),
            },
        ))
        .push(TimedOperation::completed(
            5,
            6,
            Operation::put(TABLE, b"key".to_vec(), b"second".to_vec()),
            Observation::PutWithPrevious {
                previous: Some(b"first".to_vec()),
            },
        ))
        .push(TimedOperation::completed(
            7,
            8,
            Operation::remove(TABLE, b"key".to_vec()),
            Observation::RemoveWithPrevious {
                previous: Some(b"second".to_vec()),
            },
        ))
        .push(TimedOperation::completed(
            9,
            10,
            Operation::insert(TABLE, b"key".to_vec(), b"final\0".to_vec()),
            Observation::InsertWithPrevious { previous: None },
        ));
    transaction.finish(TerminalCall::commit(11, 12, TerminalOutcome::Committed));
    let mut final_state = State::new();
    state_insert(
        &mut final_state,
        TABLE,
        b"key".to_vec(),
        b"final\0".to_vec(),
    );
    let mut transactions = History::new(initial);
    transactions
        .set_observed_final_state(final_state)
        .push(transaction);
    let mut history = ApplicationHistory::new(transactions);
    history.commits.push(ApplicationCommit::new(
        1,
        Interval::completed(11, 12),
        ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(1) },
    ));
    history.backend_attempts.push(BackendAttempt::new(
        seq(1),
        Interval::completed(13, 14),
        vec![ModelMutation::put(TABLE, b"key", b"final\0")],
        BackendAttemptOutcome::Succeeded,
    ));

    assert_eq!(check(&history).unwrap().successful_backend_prefix, 1);
}

#[test]
fn same_key_round_trip_and_failed_modifiers_are_true_noops() {
    let mut initial = State::new();
    state_insert(
        &mut initial,
        TABLE,
        b"present".to_vec(),
        b"original".to_vec(),
    );
    let mut transaction = Transaction::new(1, Interval::completed(1, 2));
    transaction
        .push(TimedOperation::completed(
            3,
            4,
            Operation::put(TABLE, b"round-trip".to_vec(), b"temporary".to_vec()),
            Observation::Put { created: true },
        ))
        .push(TimedOperation::completed(
            5,
            6,
            Operation::remove(TABLE, b"round-trip".to_vec()),
            Observation::Remove { existed: true },
        ))
        .push(TimedOperation::completed(
            7,
            8,
            Operation::insert(TABLE, b"present".to_vec(), b"ignored".to_vec()),
            Observation::Insert { inserted: false },
        ))
        .push(TimedOperation::completed(
            9,
            10,
            Operation::remove(TABLE, b"missing".to_vec()),
            Observation::Remove { existed: false },
        ));
    transaction.finish(TerminalCall::commit(11, 12, TerminalOutcome::Committed));

    let mut transactions = History::new(initial.clone());
    transactions
        .set_observed_final_state(initial)
        .push(transaction);
    let mut history = ApplicationHistory::new(transactions);
    history.commits.push(ApplicationCommit::new(
        1,
        Interval::completed(11, 12),
        ApplicationCommitOutcome::AcknowledgedNoWrite,
    ));

    assert_eq!(check(&history).unwrap().successful_backend_prefix, 0);
}

fn sparse_unknown_history() -> ApplicationHistory {
    let mut first = Transaction::new(1, Interval::completed(1, 2));
    first.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"a".to_vec(), b"1".to_vec()),
        Observation::Put { created: true },
    ));
    first.finish(TerminalCall::commit(5, 6, TerminalOutcome::Committed));

    let mut unknown = Transaction::new(2, Interval::completed(7, 8));
    unknown.push(TimedOperation::completed(
        9,
        10,
        Operation::put(TABLE, b"b".to_vec(), b"2".to_vec()),
        Observation::Put { created: true },
    ));
    unknown.finish(TerminalCall::pending_commit(11));

    let mut later = Transaction::new(3, Interval::completed(12, 13));
    later.push(TimedOperation::completed(
        14,
        15,
        Operation::put(TABLE, b"c".to_vec(), b"3".to_vec()),
        Observation::Put { created: true },
    ));
    later.finish(TerminalCall::commit(16, 17, TerminalOutcome::Committed));

    let mut transactions = History::new(State::new());
    transactions.push(first).push(unknown).push(later);
    let mut history = ApplicationHistory::new(transactions);
    history.commits.extend([
        ApplicationCommit::new(
            1,
            Interval::completed(5, 6),
            ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(1) },
        ),
        ApplicationCommit::new(
            2,
            Interval::completed(11, 18),
            ApplicationCommitOutcome::UnknownPinned { seq: seq(2) },
        ),
        ApplicationCommit::new(
            3,
            Interval::completed(16, 17),
            ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(3) },
        ),
    ]);
    history.backend_attempts.push(BackendAttempt::new(
        seq(1),
        Interval::completed(19, 20),
        vec![ModelMutation::put(TABLE, b"a", b"1")],
        BackendAttemptOutcome::Succeeded,
    ));
    let mut backend_state = State::new();
    state_insert(&mut backend_state, TABLE, b"a".to_vec(), b"1".to_vec());
    history.frontiers.push(FrontierObservation {
        interval: Interval::completed(21, 22),
        highest_acknowledged: Some(seq(3)),
        applied: Some(seq(1)),
        visible_state: None,
        backend_state: Some(backend_state),
    });
    history.waits.push(WaitAppliedObservation {
        interval: Interval::completed(23, 24),
        outcome: WaitAppliedOutcome::Unknown { seq: seq(2) },
    });

    history
}

#[test]
fn unknown_slot_makes_acknowledged_max_sparse_but_applied_remains_dense() {
    let history = sparse_unknown_history();
    let witness = check(&history).unwrap();
    assert_eq!(witness.successful_backend_prefix, 1);
}

#[test]
fn unknown_without_an_acknowledged_suffix_is_outside_an_earlier_wait_snapshot() {
    let mut history = sparse_unknown_history();
    history
        .transactions
        .transactions
        .retain(|transaction| transaction.id != 3);
    history.commits.retain(|commit| commit.transaction != 3);
    history.frontiers[0].highest_acknowledged = Some(seq(1));

    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::BarrierViolation);
    assert!(failure.detail.contains("no acknowledged barrier through 2"));
}

#[test]
fn acknowledged_suffix_must_have_started_before_unknown_fail_stop() {
    let mut history = sparse_unknown_history();
    let unknown = history
        .commits
        .iter_mut()
        .find(|commit| commit.transaction == 2)
        .unwrap();
    unknown.interval.response = Some(15);

    let failure = check(&history).unwrap_err();
    assert_eq!(
        failure.kind,
        ApplicationCheckFailureKind::MalformedApplicationHistory
    );
    assert!(failure.detail.contains("fail-stopped admission"));
}

#[test]
fn committed_suffix_blocked_by_a_prior_unknown_is_visible_but_unacknowledged() {
    let mut unknown = Transaction::new(1, Interval::completed(1, 2));
    unknown.push(TimedOperation::completed(
        3,
        4,
        Operation::put(TABLE, b"unknown".to_vec(), b"maybe".to_vec()),
        Observation::Put { created: true },
    ));
    unknown.finish(TerminalCall::pending_commit(5));

    let mut pinned = Transaction::new(2, Interval::completed(6, 7));
    pinned.push(TimedOperation::completed(
        8,
        9,
        Operation::put(TABLE, b"pinned".to_vec(), b"visible".to_vec()),
        Observation::Put { created: true },
    ));
    pinned.finish(TerminalCall::commit(10, 11, TerminalOutcome::Committed));

    let mut transactions = History::new(State::new());
    transactions.push(unknown).push(pinned);
    let mut history = ApplicationHistory::new(transactions);
    history.commits.extend([
        ApplicationCommit::new(
            1,
            Interval::completed(5, 12),
            ApplicationCommitOutcome::UnknownPinned { seq: seq(1) },
        ),
        ApplicationCommit::new(
            2,
            Interval::completed(10, 11),
            ApplicationCommitOutcome::CommittedPinned {
                seq: seq(2),
                prior_unknown: seq(1),
            },
        ),
    ]);
    history.waits.push(WaitAppliedObservation {
        interval: Interval::completed(13, 14),
        outcome: WaitAppliedOutcome::Ok { target: None },
    });

    let witness = check(&history).unwrap();
    assert_eq!(witness.cache_order.serialization, vec![1, 2]);
    assert_eq!(witness.successful_backend_prefix, 0);

    history.backend_attempts.push(BackendAttempt::new(
        seq(2),
        Interval::completed(15, 16),
        vec![ModelMutation::put(TABLE, b"pinned", b"visible")],
        BackendAttemptOutcome::Succeeded,
    ));
    assert_eq!(
        check(&history).unwrap_err().kind,
        ApplicationCheckFailureKind::BackendViolation
    );
}

#[test]
fn backend_state_may_lead_the_published_applied_frontier() {
    let mut history = two_ordered_writes();
    // Sequence 2's atomic backend batch has completed and is visible in the
    // backend snapshot, but the writer has not yet published applied=2.
    history.frontiers[0].applied = Some(seq(1));

    let witness = check(&history).unwrap();
    assert_eq!(witness.successful_backend_prefix, 2);
}

#[test]
fn application_check_never_hides_a_transaction_oracle_failure() {
    let mut history = two_ordered_writes();
    let second = history
        .transactions
        .transactions
        .iter_mut()
        .find(|transaction| transaction.id == 2)
        .unwrap();
    second.operations[0].observation = Some(Observation::Get(Some(b"never-written".to_vec())));

    let failure = check(&history).unwrap_err();
    assert_eq!(
        failure.kind,
        ApplicationCheckFailureKind::TransactionHistory
    );
    assert!(failure.transaction_failure.is_some());
}

#[test]
fn cache_sequence_order_must_be_a_legal_serialization_not_response_order() {
    let mut history = two_ordered_writes();
    history.commits[0].outcome = ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(1) };
    history.commits[1].outcome = ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(2) };
    history.backend_attempts.clear();
    history.frontiers.clear();
    history.waits.clear();

    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::IllegalCacheOrder);
}

#[test]
fn allocated_sequences_must_be_unique_and_dense_even_with_unknowns() {
    let mut duplicate = two_ordered_writes();
    duplicate.commits[0].outcome = ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(1) };
    duplicate.backend_attempts.clear();
    duplicate.frontiers.clear();
    duplicate.waits.clear();
    assert_eq!(
        check(&duplicate).unwrap_err().kind,
        ApplicationCheckFailureKind::MalformedApplicationHistory
    );

    let mut gap = two_ordered_writes();
    gap.commits[0].outcome = ApplicationCommitOutcome::AcknowledgedWrite { seq: seq(3) };
    gap.backend_attempts.clear();
    gap.frontiers.clear();
    gap.waits.clear();
    let failure = check(&gap).unwrap_err();
    assert_eq!(
        failure.kind,
        ApplicationCheckFailureKind::MalformedApplicationHistory
    );
    assert!(failure.detail.contains("not dense"));
}

#[test]
fn deliberately_corrupted_mutation_turns_the_harness_red_with_binary_replay() {
    let mut history = two_ordered_writes();
    check(&history).unwrap();
    history.backend_attempts[2].mutations[0] = ModelMutation::put(TABLE, b"y\xff", b"corrupt\0");

    let first = check(&history).unwrap_err();
    let second = check(&history).unwrap_err();
    assert_eq!(first.kind, ApplicationCheckFailureKind::MutationMismatch);
    assert!(first.detail.contains("canonical final mutation set"));
    assert!(first.replay.contains("0x79ff"));
    assert!(first.replay.contains("0x636f727275707400"));
    assert_eq!(first.replay, second.replay);
}

#[test]
fn partial_or_duplicate_backend_replay_is_rejected() {
    let mut partial = two_ordered_writes();
    partial.backend_attempts[2].mutations.clear();
    assert_eq!(
        check(&partial).unwrap_err().kind,
        ApplicationCheckFailureKind::MutationMismatch
    );

    let mut duplicate = two_ordered_writes();
    duplicate.backend_attempts.insert(
        2,
        BackendAttempt::new(
            seq(1),
            Interval::completed(24, 25),
            vec![ModelMutation::put(TABLE, b"x\0", b"one\xff")],
            BackendAttemptOutcome::Succeeded,
        ),
    );
    duplicate.backend_attempts[3].interval = Interval::completed(26, 27);
    duplicate.frontiers.clear();
    duplicate.waits.clear();
    assert_eq!(
        check(&duplicate).unwrap_err().kind,
        ApplicationCheckFailureKind::BackendViolation
    );
}

#[test]
fn failed_batch_must_retry_before_the_worker_advances() {
    let mut history = two_ordered_writes();
    history.backend_attempts.remove(1);
    history.backend_attempts[1].interval = Interval::completed(22, 23);
    history.frontiers.clear();
    history.waits.clear();
    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::BackendViolation);
    assert!(failure.detail.contains("instead of retrying"));
}

#[test]
fn backend_events_cannot_be_predated_before_their_transactions() {
    let mut history = two_ordered_writes();
    history.backend_attempts[0].interval = Interval::completed(0, 19);

    // Without owner/event causality, these dense batches look internally
    // consistent even though they materialize writes before their producers.
    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::BackendViolation);
    assert!(failure.detail.contains("before T1's commit invocation"));
}

#[test]
fn application_events_must_not_reuse_transaction_or_application_ticks() {
    let mut transaction_collision = two_ordered_writes();
    transaction_collision.waits[0] = WaitAppliedObservation {
        // T2's terminal response is also tick 17. With a non-strict `before`
        // comparison, this impossible alias used to hide both acknowledgements
        // from the barrier snapshot and make an empty success appear legal.
        interval: Interval::completed(17, 19),
        outcome: WaitAppliedOutcome::Ok { target: None },
    };
    let failure =
        check(&transaction_collision).expect_err("an application event reused a transaction tick");
    assert_eq!(
        failure.kind,
        ApplicationCheckFailureKind::MalformedApplicationHistory
    );
    assert!(failure.detail.contains("logical tick 17 is shared"));

    let mut application_collision = two_ordered_writes();
    // Sequence 2's backend response is tick 25. The intervals merely touch,
    // so the ordinary frontier checks accept the snapshot; global clock
    // uniqueness must still reject the alias.
    application_collision.frontiers[0].interval = Interval::completed(25, 27);
    let failure =
        check(&application_collision).expect_err("two application events reused one logical tick");
    assert_eq!(
        failure.kind,
        ApplicationCheckFailureKind::MalformedApplicationHistory
    );
    assert!(failure.detail.contains("logical tick 25 is shared"));
}

#[test]
fn wait_target_must_be_possible_by_the_barrier_response() {
    let mut history = two_ordered_writes();
    history.waits[0].interval = Interval::completed(8, 9);

    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::BarrierViolation);
    assert!(failure.detail.contains("greatest acknowledgement"));
}

#[test]
fn real_time_separated_wait_targets_never_regress() {
    let mut history = two_ordered_writes();
    history.frontiers.clear();
    history
        .transactions
        .transactions
        .iter_mut()
        .find(|transaction| transaction.id == 2)
        .unwrap()
        .terminal
        .as_mut()
        .unwrap()
        .interval
        .response = Some(40);
    history
        .commits
        .iter_mut()
        .find(|commit| commit.transaction == 2)
        .unwrap()
        .interval
        .response = Some(40);
    history.waits.push(WaitAppliedObservation {
        interval: Interval::completed(30, 31),
        outcome: WaitAppliedOutcome::Ok {
            target: Some(seq(1)),
        },
    });

    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::BarrierViolation);
    assert!(failure.detail.contains("regresses below"));
}

#[test]
fn deliberately_premature_or_corrupt_frontier_turns_the_harness_red() {
    let mut missing_ready = two_ordered_writes();
    check(&missing_ready).unwrap();
    missing_ready.frontiers[0].highest_acknowledged = Some(seq(1));
    let failure = check(&missing_ready).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::FrontierViolation);
    assert!(failure.detail.contains("misses real-time acknowledged"));

    let mut premature_applied = two_ordered_writes();
    premature_applied.backend_attempts.pop();
    premature_applied.frontiers[0].backend_state = None;
    premature_applied.waits.clear();
    assert_eq!(
        check(&premature_applied).unwrap_err().kind,
        ApplicationCheckFailureKind::FrontierViolation
    );

    let mut stale_state = two_ordered_writes();
    stale_state.frontiers[0]
        .backend_state
        .as_mut()
        .unwrap()
        .remove(&mako_history::StateKey::new(TABLE, b"y\xff".to_vec()));
    let failure = check(&stale_state).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::FrontierViolation);
    assert!(failure.detail.contains("backend state differs"));
}

#[test]
fn wait_ok_must_cover_every_acknowledged_write_before_invocation() {
    let mut history = two_ordered_writes();
    history.waits[0].outcome = WaitAppliedOutcome::Ok {
        target: Some(seq(1)),
    };
    let failure = check(&history).unwrap_err();
    assert_eq!(failure.kind, ApplicationCheckFailureKind::BarrierViolation);
    assert!(failure.detail.contains("required acknowledged frontier 2"));
}

#[test]
fn empty_wait_barrier_is_a_valid_success() {
    let mut history = ApplicationHistory::new(History::new(State::new()));
    history.waits.push(WaitAppliedObservation {
        interval: Interval::completed(1, 2),
        outcome: WaitAppliedOutcome::Ok { target: None },
    });

    assert_eq!(check(&history).unwrap().successful_backend_prefix, 0);
}

#[test]
fn zero_is_not_a_cache_sequence() {
    assert!(CacheSeq::new(0).is_none());
    assert!(CacheSeq::try_from(0).is_err());
    assert_eq!(CacheSeq::try_from(9).unwrap().get(), 9);
}
