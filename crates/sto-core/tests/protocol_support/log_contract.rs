//! The same contract assertions run with the isolated memory backend and the
//! native local/Raft adapters. This helper has no backend dependencies.
use sto_core::protocol::{
    contracts::CommitLog,
    journal::{self, Member, Outcome, Record, Transaction, TransactionId},
};

pub fn check(log: &dyn CommitLog, durable: bool) {
    assert_eq!(log.durable(), durable);
    assert!(log.ready());
    assert!(log.entries().unwrap().is_empty());
    let transaction = Transaction {
        version: journal::VERSION,
        id: TransactionId {
            coordinator: 0,
            worker: 0,
            incarnation: 17,
            attempt: 1,
        },
        time_term: 500,
        durable,
        members: vec![Member {
            owner: 0,
            lease: 1,
            endpoints: vec!["owner-0".into()],
        }],
    };
    assert!(log.lookup(transaction.id).unwrap().is_none());
    log.apply(&Record::Begin(transaction.clone())).unwrap();
    let prepare = Record::Prepare {
        transaction: transaction.clone(),
        rows: vec![],
        receipt: None,
    };
    let prepared = log.apply(&prepare).unwrap();
    assert!(prepared.unresolved());
    assert_eq!(log.apply(&prepare).unwrap(), prepared);
    let mut changed = transaction.clone();
    changed.time_term += 1;
    assert!(log.apply(&Record::Begin(changed)).is_err());
    let mut wrong_policy = transaction.clone();
    wrong_policy.id.attempt += 1;
    wrong_policy.durable = !durable;
    assert!(log.apply(&Record::Begin(wrong_policy)).is_err());
    let commit = Record::Decide {
        transaction: transaction.clone(),
        outcome: Outcome::Commit,
    };
    log.apply(&commit).unwrap();
    assert!(log
        .apply(&Record::Decide {
            transaction: transaction.clone(),
            outcome: Outcome::Abort
        })
        .is_err());
    let complete = Record::Complete {
        transaction: transaction.clone(),
        outcome: Outcome::Commit,
    };
    let finished = log.apply(&complete).unwrap();
    assert!(!finished.unresolved());
    assert_eq!(log.apply(&complete).unwrap(), finished);
    assert!(log
        .apply(&Record::Complete {
            transaction: transaction.clone(),
            outcome: Outcome::Abort
        })
        .is_err());
    assert_eq!(log.lookup(transaction.id).unwrap(), Some(finished.clone()));
    assert_eq!(log.entries().unwrap(), vec![finished]);
}
