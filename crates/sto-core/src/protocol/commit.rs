//! Distributed OCC preparation and two-phase commit over named adapter effects.
//! No backend supplies an assumed whole-transaction commit operation.
use crate::protocol::{
    journal::{Outcome, Transaction},
    wire::{Error, ErrorCode, Result},
};
use crate::protocol::verified::two_phase::{Coordinator, Decision};

/// Membership is fixed for the lifetime of this driver. Each index identifies
/// the same participant for locking, preparation and completion, including the
/// local coordinator. Implementations must not install during lock or prepare.
pub trait Effects {
    fn participants(&self) -> usize;
    /// Lock all of this member's frozen writes. No final read validation yet.
    fn lock(&mut self, member: usize) -> Result<()>;
    /// Select the common timestamp and save coordinator registration. Called
    /// only after every member's write locks are acknowledged.
    fn register(&mut self) -> Result<Transaction>;
    /// Validate the frozen observations/predicates and durably save the intent
    /// before returning Yes. Errors may follow a saved prepare record.
    fn prepare(&mut self, member: usize, transaction: &Transaction) -> Result<()>;
    /// Atomically establish or return the existing authoritative decision. An
    /// error leaves the decision unresolved and must not cause autonomous abort.
    fn decide(&mut self, transaction: &Transaction, proposed: Outcome) -> Result<Outcome>;
    /// Complete the saved decision, idempotently. Commit errors cannot rollback.
    fn complete(
        &mut self,
        member: usize,
        transaction: &Transaction,
        outcome: Outcome,
    ) -> Result<()>;
    /// Used only before any participant has been sent Prepare. Visit every
    /// member even if one cleanup fails. A failure leaves the attempt fenced.
    fn abort_unprepared(&mut self, member: usize) -> Result<()>;
}

#[derive(Clone, Debug)]
pub struct Completion {
    pub transaction: Transaction,
    pub outcome: Outcome,
    pub acknowledged: bool,
    pub failure: Option<Error>,
    pub delivery_failure: Option<Error>,
}

fn unknown(error: Error) -> Error {
    Error::new(
        ErrorCode::Unknown,
        format!("2PC decision or completion unresolved: {error}"),
    )
}

fn abort_unprepared<E: Effects>(effects: &mut E, count: usize, cause: Error) -> Error {
    let mut failure = None;
    for member in 0..count {
        if let Err(error) = effects.abort_unprepared(member) {
            failure.get_or_insert(error);
        }
    }
    failure.map_or(cause, unknown)
}

/// Completion reports an irrevocable outcome separately from delivery failure.
/// The caller must not retry the transaction body after Commit or Unknown.
pub fn coordinate<E: Effects>(effects: &mut E) -> Result<Completion> {
    let count = effects.participants();
    let mut coordinator = Coordinator::new(count)
        .ok_or_else(|| Error::new(ErrorCode::Invalid, "invalid 2PC participant count"))?;
    for member in 0..count {
        if let Err(error) = effects.lock(member) {
            return Err(abort_unprepared(effects, count, error));
        }
        assert!(coordinator.record_locked(member));
    }
    assert!(coordinator.all_locked());
    let transaction = match effects.register() {
        Ok(transaction) => transaction,
        Err(error) => return Err(abort_unprepared(effects, count, error)),
    };
    // Invalid registration is an adapter failure, never authorization to send
    // Prepare using a different membership than the lock barrier covered.
    if let Err(error) = transaction.validate() {
        return Err(abort_unprepared(effects, count, error));
    }
    if transaction.members.len() != count {
        return Err(abort_unprepared(
            effects,
            count,
            Error::new(
                ErrorCode::Invalid,
                "registered membership differs from lock barrier",
            ),
        ));
    }
    let mut failure = None;
    for member in 0..count {
        if let Err(error) = effects.prepare(member, &transaction) {
            coordinator.refuse();
            failure = Some(error);
            break;
        }
        assert!(coordinator.record_prepared(member));
    }
    let proposed = match coordinator.proposed() {
        Decision::Commit => Outcome::Commit,
        Decision::Abort => Outcome::Abort,
        Decision::Pending => unreachable!("verified proposal is terminal"),
    };
    let outcome = effects.decide(&transaction, proposed).map_err(unknown)?;
    if !coordinator.record_decision(outcome.decision(), true) {
        return Err(unknown(Error::new(
            ErrorCode::Invalid,
            "decision conflicts with retained votes",
        )));
    }
    let mut delivery_failure = None;
    for member in 0..count {
        match effects.complete(member, &transaction, outcome) {
            Ok(()) => {
                assert!(coordinator.acknowledge(member, outcome.decision()));
            }
            Err(error) => {
                delivery_failure.get_or_insert_with(|| {
                    Error::new(
                        error.code,
                        format!("participant {member} completion: {error}"),
                    )
                });
                failure.get_or_insert(error);
            }
        }
    }
    Ok(Completion {
        transaction,
        outcome,
        acknowledged: coordinator.complete(),
        failure,
        delivery_failure,
    })
}
