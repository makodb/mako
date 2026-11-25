//! Transaction-owned two-phase commit state. Journal acknowledgements are
//! supplied by the journal adapter; timeouts are never acknowledgements.
use vstd::prelude::*;

verus! {

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Decision { Pending, Commit, Abort }

pub fn same_decision(a: Decision, b: Decision) -> (same: bool)
    ensures same == (a == b),
{
    match a {
        Decision::Pending => matches!(b, Decision::Pending),
        Decision::Commit => matches!(b, Decision::Commit),
        Decision::Abort => matches!(b, Decision::Abort),
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Vote { Working, Locked, Prepared }

pub struct Coordinator {
    votes: Vec<Vote>,
    acknowledgements: Vec<bool>,
    decision: Decision,
    refused: bool,
}

impl Coordinator {
    pub closed spec fn wf(&self) -> bool {
        0 < self.votes.len() <= 256
        && self.acknowledgements.len() == self.votes.len()
        && (matches!(self.decision, Decision::Commit) ==> self.prepared() && !self.refused)
        && (matches!(self.decision, Decision::Pending) ==>
            forall|i: int| 0 <= i < self.acknowledgements.len() ==> !self.acknowledgements[i])
    }
    pub closed spec fn locked(&self) -> bool {
        forall|i: int| 0 <= i < self.votes.len() ==> self.votes[i] != Vote::Working
    }
    pub closed spec fn prepared(&self) -> bool {
        forall|i: int| 0 <= i < self.votes.len() ==> self.votes[i] == Vote::Prepared
    }
    pub closed spec fn outcome(&self) -> Decision { self.decision }
    pub closed spec fn vetoed(&self) -> bool { self.refused }
    pub closed spec fn count(&self) -> nat { self.votes.len() as nat }
    pub closed spec fn member_locked(&self, member: int) -> bool {
        0 <= member < self.votes.len() && self.votes[member] != Vote::Working
    }
    pub closed spec fn member_prepared(&self, member: int) -> bool {
        0 <= member < self.votes.len() && self.votes[member] == Vote::Prepared
    }
    pub closed spec fn finished(&self) -> bool {
        !matches!(self.decision, Decision::Pending)
            && forall|i: int| 0 <= i < self.acknowledgements.len() ==> self.acknowledgements[i]
    }

    pub fn new(participants: usize) -> (out: Option<Self>)
        ensures
            out is Some <==> 0 < participants <= 256,
            out is Some ==> out->Some_0.wf() && out->Some_0.count() == participants
                && out->Some_0.outcome() == Decision::Pending && !out->Some_0.vetoed(),
    {
        if participants == 0 || participants > 256 { return None; }
        let mut votes: Vec<Vote> = Vec::new();
        let mut acknowledgements: Vec<bool> = Vec::new();
        let mut i = 0;
        while i < participants
            invariant
                0 < participants <= 256, i <= participants,
                votes.len() == i, acknowledgements.len() == i,
                forall|j: int| 0 <= j < i ==> votes[j] == Vote::Working,
                forall|j: int| 0 <= j < i ==> !acknowledgements[j],
            decreases participants - i,
        {
            votes.push(Vote::Working);
            acknowledgements.push(false);
            i += 1;
        }
        Some(Self { votes, acknowledgements, decision: Decision::Pending, refused: false })
    }

    pub fn all_locked(&self) -> (yes: bool)
        ensures yes == self.locked(),
    {
        let mut i = 0;
        while i < self.votes.len()
            invariant i <= self.votes.len(),
                forall|j: int| 0 <= j < i ==> self.votes[j] != Vote::Working,
            decreases self.votes.len() - i,
        {
            if matches!(self.votes[i], Vote::Working) { return false; }
            i += 1;
        }
        true
    }

    pub fn all_prepared(&self) -> (yes: bool)
        ensures yes == self.prepared(),
    {
        let mut i = 0;
        while i < self.votes.len()
            invariant i <= self.votes.len(),
                forall|j: int| 0 <= j < i ==> self.votes[j] == Vote::Prepared,
            decreases self.votes.len() - i,
        {
            if !matches!(self.votes[i], Vote::Prepared) { return false; }
            i += 1;
        }
        true
    }

    /// Call only after that member's write-lock operation has succeeded.
    pub fn record_locked(&mut self, member: usize) -> (accepted: bool)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).count() == old(self).count(),
            final(self).outcome() == old(self).outcome(), final(self).vetoed() == old(self).vetoed(),
            old(self).locked() ==> final(self).locked(),
            old(self).prepared() ==> final(self).prepared(),
            accepted <==> old(self).outcome() == Decision::Pending && !old(self).vetoed() && member < old(self).count(),
            accepted ==> final(self).member_locked(member as int),
    {
        if !matches!(self.decision, Decision::Pending) || self.refused || member >= self.votes.len() { return false; }
        if matches!(self.votes[member], Vote::Working) { self.votes.set(member, Vote::Locked); }
        true
    }

    /// The caller must not start final read validation until all_locked is true.
    /// A successful vote also acknowledges the member's saved prepare record.
    pub fn record_prepared(&mut self, member: usize) -> (accepted: bool)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).count() == old(self).count(),
            final(self).outcome() == old(self).outcome(), final(self).vetoed() == old(self).vetoed(),
            accepted ==> old(self).outcome() == Decision::Pending && !old(self).vetoed()
                && old(self).locked() && final(self).locked(),
            accepted <==> old(self).outcome() == Decision::Pending && !old(self).vetoed()
                && member < old(self).count() && old(self).locked(),
            accepted ==> final(self).member_prepared(member as int),
            old(self).prepared() ==> final(self).prepared(),
    {
        if !matches!(self.decision, Decision::Pending) || self.refused || member >= self.votes.len()
            || !self.all_locked() { return false; }
        self.votes.set(member, Vote::Prepared);
        true
    }

    pub fn refuse(&mut self)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).outcome() == old(self).outcome(),
            old(self).outcome() == Decision::Pending ==> final(self).vetoed(),
    {
        if matches!(self.decision, Decision::Pending) { self.refused = true; }
    }

    /// This proposes a record. It does not authorize installation until saved.
    pub fn proposed(&self) -> (out: Decision)
        requires self.wf(),
        ensures
            self.outcome() != Decision::Pending ==> out == self.outcome(),
            self.outcome() == Decision::Pending ==>
                (matches!(out, Decision::Commit) <==> self.prepared() && !self.vetoed()),
            !matches!(out, Decision::Pending),
    {
        if !matches!(self.decision, Decision::Pending) { return self.decision; }
        if !self.refused && self.all_prepared() { Decision::Commit } else { Decision::Abort }
    }

    /// `saved` acknowledges this exact immutable decision, never merely a send.
    pub fn record_decision(&mut self, decision: Decision, saved: bool) -> (accepted: bool)
        requires old(self).wf(),
        ensures final(self).wf(),
            accepted ==> saved && !matches!(decision, Decision::Pending) && final(self).outcome() == decision,
            final(self).outcome() == Decision::Commit ==> final(self).prepared() && !final(self).vetoed(),
            old(self).outcome() != Decision::Pending ==> final(self).outcome() == old(self).outcome(),
            !accepted ==> final(self).outcome() == old(self).outcome(),
            accepted <==> saved && !matches!(decision, Decision::Pending)
                && if old(self).outcome() != Decision::Pending { old(self).outcome() == decision }
                   else { decision == Decision::Abort || (old(self).prepared() && !old(self).vetoed()) },
    {
        if !saved || matches!(decision, Decision::Pending) { return false; }
        if !matches!(self.decision, Decision::Pending) { return same_decision(self.decision, decision); }
        if matches!(decision, Decision::Commit) && (self.refused || !self.all_prepared()) { return false; }
        self.decision = decision;
        true
    }

    pub fn decision(&self) -> (out: Decision)
        ensures out == self.outcome(),
    { self.decision }

    pub fn acknowledge(&mut self, member: usize, decision: Decision) -> (accepted: bool)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).outcome() == old(self).outcome(),
            accepted ==> decision == old(self).outcome() && !matches!(decision, Decision::Pending),
    {
        if member >= self.acknowledgements.len() || matches!(decision, Decision::Pending)
            || !same_decision(decision, self.decision) { return false; }
        self.acknowledgements.set(member, true);
        true
    }

    pub fn complete(&self) -> (yes: bool)
        requires self.wf(),
        ensures yes == self.finished(),
    {
        if matches!(self.decision, Decision::Pending) { return false; }
        let mut i = 0;
        while i < self.acknowledgements.len()
            invariant i <= self.acknowledgements.len(), !matches!(self.decision, Decision::Pending),
                forall|j: int| 0 <= j < i ==> self.acknowledgements[j],
            decreases self.acknowledgements.len() - i,
        {
            if !self.acknowledgements[i] { return false; }
            i += 1;
        }
        true
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Phase { Executing, Locked, Preparing, Prepared, Committed, Aborted }

/// A preparing record may already be durable after a lost acknowledgement.
/// Both Preparing and Prepared therefore forbid autonomous abort or teardown.
pub fn retains_protection(phase: Phase) -> (yes: bool)
    ensures yes <==> !matches!(phase, Phase::Committed) && !matches!(phase, Phase::Aborted),
{ !matches!(phase, Phase::Committed) && !matches!(phase, Phase::Aborted) }

pub fn may_abort_locally(phase: Phase) -> (yes: bool)
    ensures yes <==> matches!(phase, Phase::Executing) || matches!(phase, Phase::Locked),
{ matches!(phase, Phase::Executing) || matches!(phase, Phase::Locked) }

pub fn begin_prepare(phase: Phase, all_locked: bool, validated: bool) -> (out: Phase)
    ensures
        matches!(out, Phase::Preparing) <==> matches!(phase, Phase::Preparing)
            || (matches!(phase, Phase::Locked) && all_locked && validated),
        !matches!(phase, Phase::Locked) || !all_locked || !validated ==> out == phase,
{ if matches!(phase, Phase::Locked) && all_locked && validated { Phase::Preparing } else { phase } }

pub fn prepared(phase: Phase, saved: bool) -> (out: Phase)
    ensures
        matches!(out, Phase::Prepared) <==> matches!(phase, Phase::Prepared) || (matches!(phase, Phase::Preparing) && saved),
        !matches!(phase, Phase::Preparing) || !saved ==> out == phase,
{ if matches!(phase, Phase::Preparing) && saved { Phase::Prepared } else { phase } }

/// A confirmed decision must come from this transaction's coordinator journal.
/// Failed/unknown application keeps the old protection; retry the same decision.
pub fn finish(phase: Phase, decision: Decision, confirmed: bool, applied: bool) -> (out: Phase)
    ensures
        matches!(phase, Phase::Committed) || matches!(phase, Phase::Aborted) ==> out == phase,
        matches!(out, Phase::Committed) && !matches!(phase, Phase::Committed) ==>
            matches!(phase, Phase::Prepared) && matches!(decision, Decision::Commit) && confirmed && applied,
        matches!(out, Phase::Aborted) && !matches!(phase, Phase::Aborted) ==>
            !matches!(phase, Phase::Committed) && matches!(decision, Decision::Abort) && confirmed && applied,
        !confirmed || !applied ==> out == phase,
        matches!(phase, Phase::Prepared) && matches!(decision, Decision::Commit) && confirmed && applied ==> matches!(out, Phase::Committed),
        !matches!(phase, Phase::Committed) && matches!(decision, Decision::Abort) && confirmed && applied ==> matches!(out, Phase::Aborted),
{
    if matches!(phase, Phase::Committed) || matches!(phase, Phase::Aborted) || !confirmed || !applied { return phase; }
    if matches!(decision, Decision::Abort) { return Phase::Aborted; }
    if matches!(phase, Phase::Prepared) && matches!(decision, Decision::Commit) { return Phase::Committed; }
    phase
}

} // verus!
