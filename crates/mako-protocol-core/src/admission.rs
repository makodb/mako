//! Participant identity, terminal receipts and installation uncertainty.
//! The adapter checks payload equality and validity, and records a reply only
//! after its engine has performed the operation and required cleanup.
use vstd::prelude::*;

verus! {

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Phase { Idle, Active, Sealed, Aborted, Committed, Unknown }
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Operation { Ordinary, Validate, Install, Abort }
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Header {
    pub incarnation: u64,
    pub attempt: u64,
    pub sequence: u64,
    pub term: u32,
    pub node: u32,
    pub worker: u32,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Refusal {
    Lease, Invalid, Unknown, Stream, Sequence, TerminalAborted,
    TerminalCommitted, NeedsValidation, Sealed, PriorActive, MissingParticipant,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Decision { Begin, Continue, Replay, AbortedReplay, Reject(Refusal) }
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Claim { Existing(u64), New, Invalid, Unknown, Busy, Collision }

pub open spec fn active_spec(phase: Phase) -> bool {
    phase == Phase::Active || phase == Phase::Sealed
}
pub fn active(phase: Phase) -> (yes: bool)
    ensures yes == active_spec(phase),
{ matches!(phase, Phase::Active | Phase::Sealed) }

pub open spec fn same_attempt(a: Header, b: Header) -> bool {
    a.incarnation == b.incarnation && a.attempt == b.attempt && a.term == b.term
}
pub open spec fn same_stream(a: Header, b: Header) -> bool {
    a.node == b.node && a.worker == b.worker
}
pub open spec fn valid_header(h: Header) -> bool {
    h.incarnation != 0 && h.attempt != 0 && h.sequence != 0 && h.term < 10
}
pub open spec fn current(lease: Option<u64>, h: Header, minimum_term: u32, valid: bool) -> bool {
    lease == Some(h.incarnation) && valid_header(h) && valid && h.term >= minimum_term
}
pub open spec fn may_execute(d: Decision) -> bool {
    d == Decision::Begin || d == Decision::Continue
}
pub open spec fn begins(phase: Phase, lease: Option<u64>, last: Option<Header>, request: Header,
    operation: Operation, minimum_term: u32, valid: bool) -> bool {
    current(lease,request,minimum_term,valid) && phase != Phase::Unknown && operation != Operation::Install
        && match last {
            None => true,
            Some(prior) => same_stream(request,prior) && !same_attempt(request,prior)
                && (request.term > prior.term || (request.incarnation == prior.incarnation
                    && request.attempt > prior.attempt && !active_spec(phase))),
        }
}
pub open spec fn continues(phase: Phase, lease: Option<u64>, last: Option<Header>, request: Header,
    operation: Operation, minimum_term: u32, valid: bool) -> bool {
    current(lease,request,minimum_term,valid) && phase != Phase::Unknown
        && phase != Phase::Aborted && phase != Phase::Committed
        && (operation == Operation::Install ==> phase == Phase::Sealed)
        && (phase == Phase::Sealed ==> operation == Operation::Install || operation == Operation::Abort)
        && match last {
            None => false,
            Some(prior) => same_stream(request,prior) && same_attempt(request,prior)
                && request.sequence > prior.sequence,
        }
}

/// Total entry point. Even malformed headers and inconsistent receipt flags
/// are rejected before they can authorize a physical effect.
pub fn admit(phase: Phase, lease: Option<u64>, last: Option<Header>, request: Header,
    operation: Operation, minimum_term: u32, valid: bool, exact_receipt: bool) -> (out: Decision)
    ensures
        out == Decision::Begin <==> begins(phase,lease,last,request,operation,minimum_term,valid),
        out == Decision::Continue <==> continues(phase,lease,last,request,operation,minimum_term,valid),
        may_execute(out) ==> current(lease, request, minimum_term, valid) && phase != Phase::Unknown,
        out == Decision::Replay ==> current(lease, request, minimum_term, valid)
            && exact_receipt && last == Some(request),
        current(lease, request, minimum_term, valid) && exact_receipt && last == Some(request)
            ==> out == Decision::Replay,
        phase == Phase::Unknown ==> !may_execute(out),
        out == Decision::Begin ==> operation != Operation::Install,
        out == Decision::Continue ==> last is Some && same_attempt(request, last->Some_0)
            && same_stream(request, last->Some_0) && request.sequence > last->Some_0.sequence
            && phase != Phase::Aborted && phase != Phase::Committed,
        out == Decision::Continue && operation == Operation::Install ==> phase == Phase::Sealed,
        out == Decision::Continue && phase == Phase::Sealed ==>
            operation == Operation::Install || operation == Operation::Abort,
        out == Decision::AbortedReplay ==> phase == Phase::Aborted && operation == Operation::Abort
            && last is Some && same_attempt(request,last->Some_0) && same_stream(request,last->Some_0)
            && request.sequence > last->Some_0.sequence && current(lease,request,minimum_term,valid),
        out == Decision::Begin && last is Some ==> same_stream(request,last->Some_0)
            && (request.term > last->Some_0.term ||
                (request.incarnation == last->Some_0.incarnation && request.attempt > last->Some_0.attempt
                    && !active_spec(phase))),
{
    if !matches!(lease, Some(value) if value == request.incarnation) { return Decision::Reject(Refusal::Lease); }
    if request.incarnation == 0 || request.attempt == 0 || request.sequence == 0
        || request.term >= 10 || !valid || request.term < minimum_term {
        return Decision::Reject(Refusal::Invalid);
    }
    if let Some(prior) = last {
        if exact_receipt && request.incarnation == prior.incarnation && request.attempt == prior.attempt
            && request.sequence == prior.sequence && request.term == prior.term
            && request.node == prior.node && request.worker == prior.worker {
            return Decision::Replay;
        }
    }
    if matches!(phase, Phase::Unknown) { return Decision::Reject(Refusal::Unknown); }
    if let Some(prior) = last {
        if request.node != prior.node || request.worker != prior.worker {
            return Decision::Reject(Refusal::Stream);
        }
        if request.incarnation == prior.incarnation && request.attempt == prior.attempt && request.term == prior.term {
            if request.sequence <= prior.sequence { return Decision::Reject(Refusal::Sequence); }
            if matches!(phase, Phase::Aborted) && matches!(operation, Operation::Abort) { return Decision::AbortedReplay; }
            if matches!(phase, Phase::Aborted) { return Decision::Reject(Refusal::TerminalAborted); }
            if matches!(phase, Phase::Committed) { return Decision::Reject(Refusal::TerminalCommitted); }
            if matches!(operation, Operation::Install) && !matches!(phase, Phase::Sealed) {
                return Decision::Reject(Refusal::NeedsValidation);
            }
            if matches!(phase, Phase::Sealed) && !matches!(operation, Operation::Install) && !matches!(operation, Operation::Abort) {
                return Decision::Reject(Refusal::Sealed);
            }
            return Decision::Continue;
        }
        if request.term <= prior.term && (request.incarnation != prior.incarnation
            || request.attempt <= prior.attempt || active(phase)) {
            return Decision::Reject(Refusal::PriorActive);
        }
    }
    if matches!(operation, Operation::Install) { return Decision::Reject(Refusal::MissingParticipant); }
    Decision::Begin
}

pub fn claim(phase: Phase, binding: Option<(u64,u64)>, nonce: u64, fresh: u64) -> (out: Claim)
    ensures
        out == Claim::New <==> nonce != 0 && fresh != 0 && phase != Phase::Unknown && !active_spec(phase)
            && match binding { None => true, Some((current,lease)) => nonce != current && fresh != lease },
        out == Claim::New ==> nonce != 0 && fresh != 0 && phase != Phase::Unknown && !active_spec(phase)
            && (binding is Some ==> fresh != binding->Some_0.1),
        out is Existing ==> nonce != 0 && fresh != 0 && binding == Some((nonce,out->Existing_0)),
        nonce != 0 && fresh != 0 && binding is Some && binding->Some_0.0 == nonce ==>
            out == Claim::Existing(binding->Some_0.1),
        phase == Phase::Unknown ==> out != Claim::New,
{
    if nonce == 0 || fresh == 0 { return Claim::Invalid; }
    if let Some((current,lease)) = binding { if nonce == current { return Claim::Existing(lease); } }
    if matches!(phase, Phase::Unknown) { return Claim::Unknown; }
    if active(phase) { return Claim::Busy; }
    if let Some((_,lease)) = binding { if lease == fresh { return Claim::Collision; } }
    Claim::New
}

pub fn completed(operation: Operation, success: bool, uncertain_error: bool) -> (phase: Phase)
    ensures
        phase == Phase::Committed <==> operation == Operation::Install && success,
        phase == Phase::Sealed <==> operation == Operation::Validate && success,
        phase == Phase::Unknown <==> !success &&
            (operation == Operation::Install || operation == Operation::Abort || uncertain_error),
        phase == Phase::Aborted <==> (operation == Operation::Abort && success) ||
            (!success && operation != Operation::Install && operation != Operation::Abort && !uncertain_error),
        phase == Phase::Active <==> operation == Operation::Ordinary && success,
{
    match operation {
        Operation::Install => if success { Phase::Committed } else { Phase::Unknown },
        Operation::Abort => if success { Phase::Aborted } else { Phase::Unknown },
        _ => if !success { if uncertain_error { Phase::Unknown } else { Phase::Aborted } }
            else if matches!(operation, Operation::Validate) { Phase::Sealed } else { Phase::Active },
    }
}

pub fn release_protection(phase: Phase) -> (yes: bool)
    ensures yes == (phase == Phase::Committed || phase == Phase::Aborted),
        phase == Phase::Unknown ==> !yes,
{ matches!(phase, Phase::Committed | Phase::Aborted) }

pub fn fence_client(success: bool, retryable: bool, cleanup_failed: bool) -> (yes: bool)
    ensures yes == (cleanup_failed || (!success && !retryable)),
{ cleanup_failed || (!success && !retryable) }

} // verus!
