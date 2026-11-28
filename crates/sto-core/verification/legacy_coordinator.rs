//! Historical scalar coordinator retained for regression tests and proofs.
//! Production OCC+2PC uses protocol::commit and protocol::verified::two_phase.
//! Each effect below is an explicit contract. In particular, the engine does
//! not supply an assumed whole-transaction commit operation.
use vstd::prelude::*;

verus! {

pub const OK: i32 = 0;
pub const RETRY: i32 = 3;
pub const FATAL: i32 = 5;
pub const CAPACITY: i32 = 6;
pub const UNKNOWN: i32 = 7;
pub const COMMITTED_ERROR: i32 = 8;
pub const MAX_TIMESTAMP: u32 = (u32::MAX - 9) / 10;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Stage {
    Collecting, Flushed, Locked, Chosen, LocalValidated, Validated,
    Installing, LocalInstalled, RemoteInstalled, Logged, Closed, Uncertain,
}

pub open spec fn before_install(s: Stage) -> bool {
    s == Stage::Collecting || s == Stage::Flushed || s == Stage::Locked
        || s == Stage::Chosen || s == Stage::LocalValidated || s == Stage::Validated
}
pub open spec fn timestamp(t: u32) -> bool { 0 < t <= MAX_TIMESTAMP }
pub open spec fn failed(s: i32) -> bool { s != OK }

/// These contracts are obligations of the storage/RPC/clock/log adapter. `stage` records
/// completed protocol facts, not a clock or a component's internal algorithm.
/// Its validation stages mean all local or contacted remote participants are
/// certified and retain their locks. Successful installation/logging preserve
/// that decision. An error after starting installation never implies rollback.
pub trait Effects {
    spec fn stage(&self) -> Stage;
    spec fn decision(&self) -> u32;
    spec fn dependency(&self) -> u32;
    spec fn install_started(&self) -> bool;
    /// Conditional progress premise for this attempt: certification succeeds,
    /// peers/log respond, and the clock has capacity for the required reserves.
    /// Safety never requires this premise. The production adapter does not
    /// claim it for a live network or a contended transaction.
    spec fn available(&self) -> bool;

    /// Stage all pending remote mutations without installing committed rows.
    fn flush(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::Collecting, !old(self).install_started(),
        ensures final(self).available() == old(self).available(), !final(self).install_started(), final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::Flushed,
            status != OK ==> before_install(final(self).stage()),
            old(self).available() ==> status == OK;
    fn lock(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::Flushed, !old(self).install_started(),
        ensures final(self).available() == old(self).available(), !final(self).install_started(), final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::Locked,
            status != OK ==> before_install(final(self).stage()),
            old(self).available() ==> status == OK;
    /// A clock reservation is positive, bounded and greater than `minimum`.
    /// Failure, including exhaustion, is allowed; it never authorizes install.
    fn reserve(&mut self, minimum: u32) -> (result: Result<u32,i32>)
        requires old(self).stage() == Stage::Locked, !old(self).install_started(),
        ensures final(self).available() == old(self).available(), final(self).stage() == old(self).stage(), !final(self).install_started(),
            final(self).dependency() == old(self).dependency(),
            result is Ok ==> timestamp(result->Ok_0) && result->Ok_0 > minimum,
            result is Err ==> failed(result->Err_0),
            old(self).available() && minimum < MAX_TIMESTAMP ==> result is Ok;
    /// Returns the greatest proposed timestamp of contacted participants, or
    /// zero when none exist. No commit decision is made by this primitive.
    fn remote_timestamp(&mut self) -> (result: Result<u32,i32>)
        requires old(self).stage() == Stage::Locked, !old(self).install_started(),
        ensures final(self).available() == old(self).available(), final(self).stage() == old(self).stage(), !final(self).install_started(),
            final(self).dependency() == old(self).dependency(),
            result is Ok ==> result->Ok_0 <= MAX_TIMESTAMP,
            result is Err ==> failed(result->Err_0),
            old(self).available() ==> result is Ok;
    fn read_dependency(&self) -> (value: u32)
        ensures value == self.dependency(), self.available() ==> value < MAX_TIMESTAMP;
    fn choose(&mut self, value: u32)
        requires old(self).stage() == Stage::Locked, !old(self).install_started(),
            timestamp(value), value > old(self).dependency(),
        ensures final(self).available() == old(self).available(), final(self).stage() == Stage::Chosen, !final(self).install_started(),
            final(self).decision() == value, final(self).dependency() == old(self).dependency();
    fn validate_local(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::Chosen, !old(self).install_started(),
        ensures final(self).available() == old(self).available(), !final(self).install_started(), final(self).decision() == old(self).decision(),
            final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::LocalValidated,
            status != OK ==> before_install(final(self).stage()),
            old(self).available() ==> status == OK;
    fn validate_remote(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::LocalValidated, !old(self).install_started(),
        ensures final(self).available() == old(self).available(), !final(self).install_started(), final(self).decision() == old(self).decision(),
            final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::Validated,
            status != OK ==> before_install(final(self).stage()),
            old(self).available() ==> status == OK;
    fn abort_remote(&mut self) -> (status: i32)
        requires before_install(old(self).stage()), !old(self).install_started(),
        ensures final(self).available() == old(self).available(), before_install(final(self).stage()), !final(self).install_started(),
            old(self).available() ==> status == OK;
    fn finish_abort(&mut self) -> (status: i32)
        requires before_install(old(self).stage()), !old(self).install_started(),
        ensures final(self).available() == old(self).available(), !final(self).install_started(),
            old(self).available() ==> status == OK;
    fn install_local(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::Validated, !old(self).install_started(),
            timestamp(old(self).decision()), old(self).decision() > old(self).dependency(),
        ensures final(self).available() == old(self).available(), final(self).install_started(), final(self).decision() == old(self).decision(),
            final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::LocalInstalled,
            old(self).available() ==> status == OK;
    fn install_remote(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::LocalInstalled, old(self).install_started(),
        ensures final(self).available() == old(self).available(), final(self).install_started(), final(self).decision() == old(self).decision(),
            final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::RemoteInstalled,
            old(self).available() ==> status == OK;
    /// OK means the configured durability policy is met for this decision.
    /// Volatile policy explicitly promises no crash recovery. Raft policy
    /// requires the current-term admission and committed replicated record.
    fn append_log(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::RemoteInstalled, old(self).install_started(),
        ensures final(self).available() == old(self).available(), final(self).install_started(), final(self).decision() == old(self).decision(),
            final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::Logged,
            old(self).available() ==> status == OK;
    fn finish_commit(&mut self) -> (status: i32)
        requires old(self).stage() == Stage::Logged, old(self).install_started(),
        ensures final(self).available() == old(self).available(), final(self).install_started(), final(self).decision() == old(self).decision(),
            final(self).dependency() == old(self).dependency(),
            status == OK ==> final(self).stage() == Stage::Closed,
            old(self).available() ==> status == OK;
    /// Retain the uncertainty fence even if local physical cleanup succeeds.
    fn quarantine(&mut self)
        requires old(self).install_started(),
        ensures final(self).available() == old(self).available(), final(self).install_started(), final(self).stage() == Stage::Uncertain,
            final(self).decision() == old(self).decision(), final(self).dependency() == old(self).dependency();
}

fn prepare<E: Effects>(effects: &mut E) -> (status: i32)
    requires old(effects).stage() == Stage::Collecting, !old(effects).install_started(),
    ensures !final(effects).install_started(), before_install(final(effects).stage()),
        final(effects).available() == old(effects).available(),
        old(effects).available() ==> status == OK,
        status == OK ==> final(effects).stage() == Stage::Validated
            && timestamp(final(effects).decision()) && final(effects).decision() > final(effects).dependency(),
{
    let status = effects.flush();
    if status != OK { return status; }
    let status = effects.lock();
    if status != OK { return status; }
    let mut proposal = match effects.reserve(0) { Ok(t) => t, Err(e) => return e };
    let remote = match effects.remote_timestamp() { Ok(t) => t, Err(e) => return e };
    if remote > proposal { proposal = remote; }
    let dependency = effects.read_dependency();
    if proposal <= dependency {
        proposal = match effects.reserve(dependency) { Ok(t) => t, Err(e) => return e };
    }
    effects.choose(proposal);
    let status = effects.validate_local();
    if status != OK { return status; }
    effects.validate_remote()
}

/// The runtime and deterministic driver call this same executable function.
/// Each irreversible step requires the preceding certification or publication
/// fact. Every installation/log failure quarantines the attempt.
pub fn coordinate<E: Effects>(effects: &mut E) -> (status: i32)
    requires old(effects).stage() == Stage::Collecting, !old(effects).install_started(),
    ensures
        old(effects).available() ==> status == OK,
        status == OK ==> final(effects).stage() == Stage::Closed && final(effects).install_started()
            && timestamp(final(effects).decision()) && final(effects).decision() > final(effects).dependency(),
        status == RETRY || status == CAPACITY ==> !final(effects).install_started(),
        final(effects).install_started() && status != OK && status != COMMITTED_ERROR && status != FATAL
            ==> status == UNKNOWN && final(effects).stage() == Stage::Uncertain,
{
    let prepared = prepare(effects);
    if prepared != OK {
        let remote_cleanup = effects.abort_remote();
        let cleanup = effects.finish_abort();
        if cleanup != OK { return cleanup; }
        if remote_cleanup != OK { return FATAL; }
        return prepared;
    }
    if effects.install_local() != OK {
        effects.quarantine();
        return UNKNOWN;
    }
    if effects.install_remote() != OK {
        effects.quarantine();
        return UNKNOWN;
    }
    if effects.append_log() != OK {
        effects.quarantine();
        return UNKNOWN;
    }
    let status = effects.finish_commit();
    // Even a misclassified backend close cannot turn an installed attempt into
    // a retryable abort. Keep definite committed failure and fatal diagnoses.
    if status == OK || status == COMMITTED_ERROR || status == FATAL { return status; }
    effects.quarantine();
    UNKNOWN
}

} // verus!
