use mako_protocol::{
    commit::{self, Effects},
    journal::{self, Entry, Member, Outcome, Record, Transaction, TransactionId},
    redo::Row,
    wire::{self, Error, ErrorCode, Result},
};
use mako_protocol_core::two_phase::{self as core, Coordinator, Decision, Phase};
use std::collections::BTreeMap;

fn transaction() -> Transaction {
    Transaction {
        version: journal::VERSION,
        id: TransactionId {
            coordinator: 0,
            worker: 0,
            incarnation: 10,
            attempt: 1,
        },
        time_term: 500,
        durable: true,
        members: (0..3)
            .map(|owner| Member {
                owner,
                lease: 100 + u64::from(owner),
                endpoints: vec![format!("shard-{owner}")],
            })
            .collect(),
    }
}
fn row(owner: usize) -> Row {
    Row {
        table: 1,
        key: vec![owner as u8],
        value: Some(vec![42]),
        time_term: 500,
    }
}
fn failed() -> Error {
    Error::new(ErrorCode::Transport, "injected lost acknowledgement")
}

#[derive(Clone, Copy)]
struct Fault {
    step: &'static str,
    member: usize,
    after: bool,
}

/// The saved bytes and rows survive simulated process destruction. Volatile
/// locks, votes and call state do not. No native storage or consensus runs here.
struct World {
    saved: Vec<Option<Vec<u8>>>,
    rows: Vec<BTreeMap<Vec<u8>, Vec<u8>>>,
    locks: [bool; 3],
    fault: Option<Fault>,
    events: Vec<(&'static str, usize)>,
    transaction: Transaction,
}
impl World {
    fn new() -> Self {
        Self {
            saved: vec![None; 3],
            rows: vec![BTreeMap::new(); 3],
            locks: [false; 3],
            fault: None,
            events: vec![],
            transaction: transaction(),
        }
    }
    fn fault(&self, step: &str, member: usize, after: bool) -> Result<()> {
        if self
            .fault
            .is_some_and(|f| f.step == step && f.member == member && f.after == after)
        {
            Err(failed())
        } else {
            Ok(())
        }
    }
    fn entry(&self, owner: usize) -> Option<Entry> {
        self.saved[owner]
            .as_ref()
            .map(|bytes| wire::decode(bytes).unwrap())
    }
    fn apply(&mut self, owner: usize, record: Record) -> Result<Entry> {
        let prior = self.entry(owner);
        let transition = journal::transition(owner as u32, true, prior.as_ref(), &record)?;
        for row in transition.rows {
            match row.value {
                Some(value) => {
                    self.rows[owner].insert(row.key, value);
                }
                None => {
                    self.rows[owner].remove(&row.key);
                }
            }
        }
        self.saved[owner] = Some(wire::encode(&transition.entry)?);
        Ok(transition.entry)
    }
    fn recover(&mut self) -> Outcome {
        self.locks = [false; 3];
        self.fault = None;
        self.events.clear();
        // Restore every prepared write lock before admitting new transactions.
        for member in 0..3 {
            self.locks[member] = self.entry(member).is_some_and(|entry| entry.unresolved());
        }
        // This simulates an authoritative restarted coordinator after fencing
        // its old process. A live participant cannot make this Abort decision.
        let outcome = self.entry(0).unwrap().decision.unwrap_or(Outcome::Abort);
        self.apply(
            0,
            Record::Decide {
                transaction: self.transaction.clone(),
                outcome,
            },
        )
        .unwrap();
        for member in 0..3 {
            self.complete(member, &self.transaction.clone(), outcome)
                .unwrap();
        }
        outcome
    }
}
impl Effects for World {
    fn participants(&self) -> usize {
        3
    }
    fn lock(&mut self, member: usize) -> Result<()> {
        self.events.push(("lock", member));
        self.fault("lock", member, false)?;
        self.locks[member] = true;
        self.fault("lock", member, true)
    }
    fn register(&mut self) -> Result<Transaction> {
        self.events.push(("register", 0));
        assert!(self.locks.iter().all(|locked| *locked));
        self.fault("register", 0, false)?;
        let transaction = self.transaction.clone();
        self.apply(0, Record::Begin(transaction.clone()))?;
        self.fault("register", 0, true)?;
        Ok(transaction)
    }
    fn prepare(&mut self, member: usize, transaction: &Transaction) -> Result<()> {
        self.events.push(("prepare", member));
        assert!(
            self.locks.iter().all(|locked| *locked),
            "validation preceded lock barrier"
        );
        assert!(
            self.rows.iter().all(BTreeMap::is_empty),
            "prepare installed a write"
        );
        self.fault("prepare", member, false)?;
        self.apply(
            member,
            Record::Prepare {
                receipt: None,
                transaction: transaction.clone(),
                rows: vec![row(member)],
            },
        )?;
        self.fault("prepare", member, true)
    }
    fn decide(&mut self, transaction: &Transaction, outcome: Outcome) -> Result<Outcome> {
        self.events.push(("decide", 0));
        self.fault("decide", 0, false)?;
        if outcome == Outcome::Commit {
            assert!((0..3).all(|i| self.entry(i).is_some_and(|entry| entry.unresolved())));
        }
        let saved = self.apply(
            0,
            Record::Decide {
                transaction: transaction.clone(),
                outcome,
            },
        )?;
        self.fault("decide", 0, true)?;
        Ok(saved.decision.unwrap())
    }
    fn complete(
        &mut self,
        member: usize,
        transaction: &Transaction,
        outcome: Outcome,
    ) -> Result<()> {
        self.events.push(("complete", member));
        assert_eq!(
            self.entry(0).unwrap().decision,
            Some(outcome),
            "completion without saved decision"
        );
        self.fault("complete", member, false)?;
        self.apply(
            member,
            Record::Complete {
                transaction: transaction.clone(),
                outcome,
            },
        )?;
        self.locks[member] = false;
        self.fault("complete", member, true)
    }
    fn abort_unprepared(&mut self, member: usize) -> Result<()> {
        self.events.push(("abort_unprepared", member));
        assert!(!self.entry(member).is_some_and(|entry| entry.unresolved()));
        self.locks[member] = false;
        Ok(())
    }
}

#[test]
fn successful_commit_has_a_global_lock_barrier_and_saved_decision_before_rows() {
    let mut world = World::new();
    let result = commit::coordinate(&mut world).unwrap();
    assert_eq!(result.outcome, Outcome::Commit);
    assert!(result.acknowledged && result.failure.is_none());
    assert_eq!(
        &world.events[..4],
        &[("lock", 0), ("lock", 1), ("lock", 2), ("register", 0)]
    );
    for member in 0..3 {
        assert_eq!(world.rows[member].get(&vec![member as u8]), Some(&vec![42]));
        assert!(!world.locks[member]);
    }
}

#[test]
fn lock_failures_clean_every_member_without_validation_or_installation() {
    for member in 0..3 {
        for after in [false, true] {
            let mut world = World::new();
            world.fault = Some(Fault {
                step: "lock",
                member,
                after,
            });
            assert!(commit::coordinate(&mut world).is_err());
            assert_eq!(world.locks, [false; 3]);
            assert!(world.rows.iter().all(BTreeMap::is_empty));
            assert_eq!(
                world
                    .events
                    .iter()
                    .filter(|(step, _)| *step == "abort_unprepared")
                    .count(),
                3
            );
            assert!(!world.events.iter().any(|(step, _)| *step == "prepare"));
        }
    }
}

#[test]
fn lost_prepare_acknowledgements_resolve_with_authoritative_abort() {
    for member in 0..3 {
        for after in [false, true] {
            let mut world = World::new();
            world.fault = Some(Fault {
                step: "prepare",
                member,
                after,
            });
            let result = commit::coordinate(&mut world).unwrap();
            assert_eq!(result.outcome, Outcome::Abort);
            assert!(result.acknowledged);
            assert!(world.rows.iter().all(BTreeMap::is_empty));
            assert_eq!(world.locks, [false; 3]);
            assert!(!world
                .events
                .iter()
                .any(|(step, _)| *step == "abort_unprepared"));
            assert_eq!(world.recover(), Outcome::Abort);
            assert!(
                world
                    .apply(
                        member,
                        Record::Prepare {
                            receipt: None,
                            transaction: transaction(),
                            rows: vec![row(member)],
                        }
                    )
                    .is_err(),
                "delayed prepare revived abort"
            );
        }
    }
}

#[test]
fn uncertain_decision_never_aborts_and_recovery_obeys_the_saved_record() {
    for after in [false, true] {
        let mut world = World::new();
        world.fault = Some(Fault {
            step: "decide",
            member: 0,
            after,
        });
        assert_eq!(
            commit::coordinate(&mut world).unwrap_err().code,
            ErrorCode::Unknown
        );
        assert_eq!(world.locks, [true; 3]);
        assert!(world.rows.iter().all(BTreeMap::is_empty));
        assert!(!world
            .events
            .iter()
            .any(|(step, _)| *step == "complete" || *step == "abort_unprepared"));
        assert_eq!(
            world.recover(),
            if after {
                Outcome::Commit
            } else {
                Outcome::Abort
            }
        );
        assert!(world.rows.iter().all(|rows| rows.is_empty() != after));
    }
}

#[test]
fn partial_commit_delivery_is_completed_idempotently_after_restart() {
    for member in 0..3 {
        for after in [false, true] {
            let mut world = World::new();
            world.fault = Some(Fault {
                step: "complete",
                member,
                after,
            });
            let result = commit::coordinate(&mut world).unwrap();
            assert_eq!(result.outcome, Outcome::Commit);
            assert!(!result.acknowledged);
            assert_eq!(
                world
                    .events
                    .iter()
                    .filter(|(step, _)| *step == "complete")
                    .count(),
                3
            );
            assert_eq!(world.recover(), Outcome::Commit);
            let before = world.rows.clone();
            for owner in 0..3 {
                let entry = world.entry(owner).unwrap();
                let duplicate = journal::transition(
                    owner as u32,
                    true,
                    Some(&entry),
                    &Record::Complete {
                        transaction: transaction(),
                        outcome: Outcome::Commit,
                    },
                )
                .unwrap();
                assert!(!duplicate.changed && duplicate.rows.is_empty());
            }
            assert_eq!(world.recover(), Outcome::Commit);
            assert_eq!(world.rows, before);
        }
    }
}

#[test]
fn repeated_votes_cannot_replace_other_members_or_skip_the_lock_barrier() {
    let mut coordinator = Coordinator::new(3).unwrap();
    assert!(coordinator.record_locked(0));
    assert!(!coordinator.record_prepared(0));
    for member in 1..3 {
        assert!(coordinator.record_locked(member));
    }
    for _ in 0..100 {
        assert!(coordinator.record_prepared(0));
    }
    assert!(!coordinator.record_decision(Decision::Commit, true));
    assert!(coordinator.record_prepared(1));
    assert!(!coordinator.record_decision(Decision::Commit, true));
    assert!(coordinator.record_prepared(2));
    assert_eq!(coordinator.proposed(), Decision::Commit);
    assert!(!coordinator.record_decision(Decision::Commit, false));
    assert!(!coordinator.acknowledge(0, Decision::Commit));
    assert!(coordinator.record_decision(Decision::Commit, true));
    assert!(!coordinator.record_decision(Decision::Abort, true));
    assert!(!coordinator.record_locked(3));
    for member in 0..3 {
        assert!(coordinator.acknowledge(member, Decision::Commit));
    }
    assert!(coordinator.complete());
}

#[test]
fn preparing_or_prepared_work_cannot_autonomously_abort_or_release_protection() {
    let preparing = core::begin_prepare(Phase::Locked, true, true);
    assert_eq!(preparing, Phase::Preparing);
    assert_eq!(core::prepared(preparing, false), preparing);
    let prepared = core::prepared(preparing, true);
    for phase in [preparing, prepared] {
        assert!(!core::may_abort_locally(phase));
        assert!(core::retains_protection(phase));
        assert_eq!(core::finish(phase, Decision::Abort, false, true), phase);
        assert_eq!(core::finish(phase, Decision::Commit, true, false), phase);
    }
    assert_eq!(
        core::finish(prepared, Decision::Commit, true, true),
        Phase::Committed
    );
    assert_eq!(
        core::finish(Phase::Committed, Decision::Abort, true, true),
        Phase::Committed
    );
    assert_eq!(
        core::finish(Phase::Aborted, Decision::Commit, true, true),
        Phase::Aborted
    );
}

#[test]
fn journal_rejects_changed_intents_decisions_membership_and_durability() {
    let mut world = World::new();
    commit::coordinate(&mut world).unwrap();
    assert!(world
        .apply(
            0,
            Record::Decide {
                transaction: transaction(),
                outcome: Outcome::Abort
            }
        )
        .is_err());
    for owner in 0..3 {
        assert!(world
            .apply(
                owner,
                Record::Complete {
                    transaction: transaction(),
                    outcome: Outcome::Abort
                }
            )
            .is_err());
        let mut changed = row(owner);
        changed.value = Some(vec![43]);
        assert!(world
            .apply(
                owner,
                Record::Prepare {
                    receipt: None,
                    transaction: transaction(),
                    rows: vec![changed]
                }
            )
            .is_err());
    }
    let mut changed = transaction();
    changed.members[1].lease += 1;
    assert!(world.apply(0, Record::Begin(changed)).is_err());
    assert!(journal::transition(0, false, None, &Record::Begin(transaction())).is_err());
    assert!(journal::transition(
        1,
        true,
        None,
        &Record::Complete {
            transaction: transaction(),
            outcome: Outcome::Commit,
        }
    )
    .is_err());
}

#[test]
fn volatile_aggregate_intents_do_not_expand_rpc_or_durable_frame_limits() {
    let mut transaction = transaction();
    transaction.durable = false;
    let rows: Vec<_> = (0..160u32)
        .map(|key| Row {
            table: 1,
            key: key.to_be_bytes().to_vec(),
            value: Some(vec![7; 32 * 1024]),
            time_term: transaction.time_term,
        })
        .collect();
    let record = Record::Prepare {
        transaction: transaction.clone(),
        rows: rows.clone(),
        receipt: None,
    };
    assert!(mako_protocol::wire::encode(&record).is_err());
    assert!(journal::transition(0, false, None, &record).is_ok());
    transaction.durable = true;
    assert_eq!(
        journal::transition(
            0,
            true,
            None,
            &Record::Prepare {
                transaction,
                rows,
                receipt: None
            }
        )
        .err()
        .unwrap()
        .code,
        ErrorCode::Capacity
    );
}
