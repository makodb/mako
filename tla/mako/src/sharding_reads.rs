//! Source-derived operational slice of the successful live migration driver.
//! It retains the gap between RunNontxnOp's moved/owner checks and the later
//! one-op storage transaction. No process failure, lost RPC, checksum collision,
//! writer, stale cache or incorrect migration source is needed for the witness.
//!
//! Source: sharding-rebase 44c6b5d5, server.cc:745-770,820-825;
//! shard_data_plane.cc:164-167; storage/mbta_wrapper.hh:527-545,571-597.
//! This is a protocol counterexample, not a C++ memory-model refinement proof.
use vstd::prelude::*;

verus! {

pub enum Phase { Idle, Copying, Locked, Drained, Ready, Dropping, Dropped, Published, Complete }
pub enum Reader {
    Idle,
    Checked { invoked: nat },
    Fetched { invoked: nat, value: Option<int> },
    Returned { invoked: nat, at: nat, value: Option<int> },
}
pub struct State {
    pub source: Option<int>,
    pub destination: Option<int>,
    pub phase: Phase,
    pub background_done: bool,
    pub caught_up: bool,
    pub final_copied: bool,
    pub source_frozen: bool,
    pub source_moved: bool,
    pub route: int,
    pub reader: Reader,
    pub tick: nat,
}
pub enum Action {
    Begin, BackgroundCopy, Freeze, Drain, CatchupCopy, FinalSyncCopy, Verify,
    MarkMoved, RemoveSource, Publish, Finish, CheckRead, FetchRead, ReplyRead, Stutter,
}

pub open spec fn initial(value: int) -> State {
    State { source: Some(value), destination: None, phase: Phase::Idle,
        background_done: false, caught_up: false, final_copied: false,
        source_frozen: false, source_moved: false, route: 0, reader: Reader::Idle, tick: 0 }
}
pub open spec fn after_mark(p: Phase) -> bool {
    p is Dropping || p is Dropped || p is Published || p is Complete
}
pub open spec fn after_delete(p: Phase) -> bool {
    p is Dropped || p is Published || p is Complete
}
pub open spec fn enabled(s: State, digest: spec_fn(Option<int>) -> int, a: Action) -> bool {
    match a {
        Action::Begin => s.phase is Idle,
        Action::BackgroundCopy => s.phase is Copying,
        Action::Freeze => s.phase is Copying && s.background_done,
        // This slice contains no writers: the production drain is genuinely zero.
        Action::Drain => s.phase is Locked,
        Action::CatchupCopy => s.phase is Drained,
        Action::FinalSyncCopy => s.phase is Drained && s.caught_up,
        Action::Verify => s.phase is Drained && s.final_copied
            && digest(s.source) == digest(s.destination),
        Action::MarkMoved => s.phase is Ready,
        Action::RemoveSource => s.phase is Dropping,
        Action::Publish => s.phase is Dropped,
        Action::Finish => s.phase is Published,
        // Both admission checks succeed together here. The bad execution only
        // pauses AFTER both, so this coarsening does not create the race.
        Action::CheckRead => s.reader is Idle && !s.source_moved && s.route == 0,
        // A fresh one-op transaction begins here; neither check is repeated.
        Action::FetchRead => s.reader is Checked,
        Action::ReplyRead => s.reader is Fetched,
        Action::Stutter => true,
    }
}
pub open spec fn apply(s: State, a: Action) -> State {
    let z = match a {
        Action::Begin => State { phase: Phase::Copying, ..s },
        Action::BackgroundCopy => State { destination: s.source, background_done: true, ..s },
        Action::Freeze => State { phase: Phase::Locked, source_frozen: true, ..s },
        Action::Drain => State { phase: Phase::Drained, ..s },
        Action::CatchupCopy => State { destination: s.source, caught_up: true, ..s },
        Action::FinalSyncCopy => State { destination: s.source, final_copied: true, ..s },
        Action::Verify => State { phase: Phase::Ready, ..s },
        Action::MarkMoved => State { phase: Phase::Dropping, source_moved: true, ..s },
        Action::RemoveSource => State { phase: Phase::Dropped, source: None, ..s },
        Action::Publish => State { phase: Phase::Published, route: 1, ..s },
        Action::Finish => State { phase: Phase::Complete, ..s },
        Action::CheckRead => State { reader: Reader::Checked { invoked: s.tick }, ..s },
        Action::FetchRead => State { reader: Reader::Fetched {
            invoked: s.reader->Checked_invoked, value: s.source }, ..s },
        Action::ReplyRead => State { reader: Reader::Returned {
            invoked: s.reader->Fetched_invoked, at: s.tick, value: s.reader->Fetched_value }, ..s },
        Action::Stutter => s,
    };
    State { tick: s.tick + 1, ..z }
}
pub open spec fn next(s: State, z: State, digest: spec_fn(Option<int>) -> int) -> bool {
    exists|a: Action| #[trigger] enabled(s, digest, a) && z == apply(s, a)
}
pub open spec fn behavior(states: Seq<State>, value: int, digest: spec_fn(Option<int>) -> int) -> bool {
    states.len() > 0 && states[0] == initial(value)
        && forall|j: int| 0 <= j < states.len() - 1 ==> #[trigger] next(states[j], states[j + 1], digest)
}

/// The handoff may make the old route temporarily unavailable, but there is
/// no application delete: the logical key must remain present throughout.
pub open spec fn logical_value(s: State) -> Option<int> {
    if s.source_moved { s.destination } else { s.source }
}
pub open spec fn inv(s: State, value: int) -> bool {
    &&& s.source_moved == after_mark(s.phase)
    &&& s.source == if after_delete(s.phase) { None } else { Some(value) }
    &&& (s.background_done ==> s.destination == Some(value))
    &&& (!(s.phase is Idle || s.phase is Copying) ==> s.background_done)
    &&& s.route == if s.phase is Published || s.phase is Complete { 1int } else { 0int }
    &&& (s.caught_up ==> s.background_done)
    &&& (s.final_copied ==> s.caught_up)
    &&& (after_mark(s.phase) || s.phase is Ready ==> s.final_copied)
    &&& (s.phase is Idle || s.phase is Copying <==> !s.source_frozen)
}
pub proof fn lemma_init(value: int)
    ensures inv(initial(value), value)
{}
pub proof fn lemma_step(s: State, value: int, digest: spec_fn(Option<int>) -> int, a: Action)
    requires inv(s, value), enabled(s, digest, a)
    ensures inv(apply(s, a), value)
{
    match a {
        Action::Begin => {}, Action::BackgroundCopy => {}, Action::Freeze => {},
        Action::Drain => {}, Action::CatchupCopy => {}, Action::FinalSyncCopy => {},
        Action::Verify => {}, Action::MarkMoved => {}, Action::RemoveSource => {},
        Action::Publish => {}, Action::Finish => {}, Action::CheckRead => {},
        Action::FetchRead => {}, Action::ReplyRead => {}, Action::Stutter => {},
    }
}
pub proof fn theorem_invariant(states: Seq<State>, value: int, digest: spec_fn(Option<int>) -> int, j: int)
    requires behavior(states, value, digest), 0 <= j < states.len()
    ensures inv(states[j], value), logical_value(states[j]) == Some(value)
    decreases j
{
    if j == 0 { lemma_init(value); } else {
        let p = j - 1;
        theorem_invariant(states, value, digest, p);
        assert(next(states[p], states[p + 1], digest));
        let a = choose|a: Action| #[trigger] enabled(states[p], digest, a) && states[p + 1] == apply(states[p], a);
        lemma_step(states[p], value, digest, a);
    }
}

/// A completed read must match the logical value at some instant between
/// invocation and response. None is a successful "not found", not a retry.
pub open spec fn read_linearizable(states: Seq<State>) -> bool {
    states.len() > 0 && match states.last().reader {
        Reader::Returned { invoked, at, value } => exists|j: int| 0 <= j < states.len()
            && invoked <= states[j].tick <= at && value == logical_value(#[trigger] states[j]),
        _ => true,
    }
}
pub proof fn theorem_clean_miss_is_not_linearizable(states: Seq<State>, value: int,
    digest: spec_fn(Option<int>) -> int)
    requires behavior(states, value, digest), states.last().reader is Returned,
        states.last().reader->Returned_value == None::<int>
    ensures !read_linearizable(states)
{
    if read_linearizable(states) {
        let r = states.last().reader;
        let j = choose|j: int| 0 <= j < states.len()
            && r->Returned_invoked <= states[j].tick <= r->Returned_at
            && r->Returned_value == logical_value(#[trigger] states[j]);
        theorem_invariant(states, value, digest, j);
    }
}
pub proof fn lemma_fetch_before_move(s: State, value: int)
    requires inv(s, value), s.reader is Checked, !s.source_moved
    ensures apply(s, Action::FetchRead).reader is Fetched,
        apply(s, Action::FetchRead).reader->Fetched_value == Some(value)
{}

pub proof fn begin(value: int, digest: spec_fn(Option<int>) -> int) -> (states: Seq<State>)
    ensures behavior(states, value, digest), states == seq![initial(value)]
{ seq![initial(value)] }
pub proof fn extend(states: Seq<State>, value: int, digest: spec_fn(Option<int>) -> int, a: Action)
    -> (after: Seq<State>)
    requires behavior(states, value, digest), enabled(states.last(), digest, a)
    ensures behavior(after, value, digest), after == states.push(apply(states.last(), a)),
        after.last() == apply(states.last(), a), after.len() == states.len() + 1
{
    let after = states.push(apply(states.last(), a));
    assert(after[0] == states[0]);
    assert forall|j: int| 0 <= j < after.len() - 1 implies #[trigger] next(after[j], after[j + 1], digest) by {
        if j < states.len() - 1 {
            assert(after[j] == states[j]);
            assert(after[j + 1] == states[j + 1]);
        } else {
            assert(after[j] == states.last());
            assert(after[j + 1] == apply(states.last(), a));
        }
    }
    after
}

/// Universally quantified over the checksum: equal copied bytes suffice.
/// No injectivity assumption or corrupt transfer is used in this execution.
pub proof fn witness_admitted_read_crosses_drop(value: int, digest: spec_fn(Option<int>) -> int)
    -> (states: Seq<State>)
    ensures behavior(states, value, digest), states.last().phase is Complete,
        states.last().route == 1, states.last().destination == Some(value),
        states.last().source == None::<int>, states.last().reader is Returned,
        states.last().reader->Returned_value == None::<int>, !read_linearizable(states)
{
    let t = begin(value, digest);
    let t = extend(t, value, digest, Action::Begin);
    let t = extend(t, value, digest, Action::BackgroundCopy);
    let t = extend(t, value, digest, Action::Freeze);
    let t = extend(t, value, digest, Action::Drain);
    let t = extend(t, value, digest, Action::CatchupCopy);
    let t = extend(t, value, digest, Action::FinalSyncCopy);
    let t = extend(t, value, digest, Action::Verify);
    let t = extend(t, value, digest, Action::CheckRead);
    let t = extend(t, value, digest, Action::MarkMoved);
    let t = extend(t, value, digest, Action::RemoveSource);
    let t = extend(t, value, digest, Action::Publish);
    let t = extend(t, value, digest, Action::Finish);
    let t = extend(t, value, digest, Action::FetchRead);
    let t = extend(t, value, digest, Action::ReplyRead);
    theorem_clean_miss_is_not_linearizable(t, value, digest);
    t
}

} // verus!
