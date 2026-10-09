//! Pure lemmas: none of these APIs mutates or trusts executable storage.
use super::*;

verus! {

pub proof fn append_write(s: p::State, writes: Seq<Write>, w: Write)
    ensures apply_writes(s,writes.push(w)) == apply_write(apply_writes(s,writes),w),
        writes_ok(s,writes.push(w)) == (writes_ok(s,writes) && writable(apply_writes(s,writes),w)),
{
    assert(writes.push(w).drop_last() =~= writes);
}
pub proof fn single_write(s: p::State, w: Write)
    ensures apply_writes(s,seq![w]) == apply_write(s,w),
        writes_ok(s,seq![w]) == writable(s,w),
{
    assert(seq![w] =~= Seq::<Write>::empty().push(w));
    append_write(s,Seq::empty(),w);
}
pub proof fn concat_writes(s: p::State, a: Seq<Write>, b: Seq<Write>)
    ensures apply_writes(s,a+b) == apply_writes(apply_writes(s,a),b),
        writes_ok(s,a+b) == (writes_ok(s,a) && writes_ok(apply_writes(s,a),b)),
    decreases b.len(),
{
    if b.len() == 0 { assert(a+b =~= a); }
    else {
        concat_writes(s,a,b.drop_last());
        assert(a+b =~= (a+b.drop_last()).push(b.last()));
        append_write(s,a+b.drop_last(),b.last());
    }
}

/// Every address except the written entry is framed, including map domains.
pub proof fn write_frame(s: p::State, w: Write, a: Address)
    requires a != address(w),
    ensures same_at(s,apply_write(s,w),a),
{
    match w {
        Write::Logical { .. } => {}, Write::Replica { .. } => {},
        Write::ReplicaCell { .. } => {}, Write::ReplicaRole { .. } => {},
        Write::ReplicaEpoch { .. } => {}, Write::ReplicaFence { .. } => {},
        Write::ReplicaTerminal { .. } => {}, Write::ReplicaRound { .. } => {},
        Write::ReplicaCovered { .. } => {}, Write::Session { .. } => {},
        Write::Held { .. } => {}, Write::Release { .. } => {}, Write::Resolved { .. } => {},
        Write::DirectoryAppend { .. } => {}, Write::View { .. } => {},
        Write::NextGeneration { .. } => {}, Write::Active { .. } => {},
        Write::Plan { .. } => {}, Write::Phase { .. } => {}, Write::Command { .. } => {},
        Write::Certificate { .. } => {}, Write::Received { .. } => {},
        Write::Packet { .. } => {}, Write::Outcome { .. } => {}, Write::Reply { .. } => {},
    }
}
pub proof fn writes_frame(s: p::State, writes: Seq<Write>, a: Address)
    requires forall|i: int| 0 <= i < writes.len() ==> address(writes[i]) != a,
    ensures same_at(s,apply_writes(s,writes),a),
    decreases writes.len(),
{
    if writes.len() > 0 {
        writes_frame(s,writes.drop_last(),a);
        write_frame(apply_writes(s,writes.drop_last()),writes.last(),a);
    }
}

/// Accepted-action certificate: guard and real endpoint are separate caller
/// obligations. This lemma does not derive either from an action label.
pub proof fn accepted(c: p::Constants, before: p::State, after: p::State, action: p::Action)
    requires p::enabled(c,before,action), after == p::apply(c,before,action),
    ensures path(c,seq![before,after],seq![action]),
{}
/// Disabled request: exact equality of ALL projected fields is required.
pub proof fn rejected(c: p::Constants, before: p::State, after: p::State, action: p::Action)
    requires !p::enabled(c,before,action), after == before,
    ensures path(c,seq![before,after],seq![action]),
{}
/// An enabled delivery can reject at its local fenced guard. The caller must
/// prove that guard's effect is the identity, not merely pass an error status.
pub proof fn local_rejection(c: p::Constants, before: p::State, after: p::State, action: p::Action)
    requires after == before, p::dispatch(c,before,action) == before,
    ensures path(c,seq![before,after],seq![action]),
{}
pub proof fn stutter(c: p::Constants, before: p::State, after: p::State)
    requires after == before,
    ensures path(c,seq![before,after],seq![p::Action::Stutter]),
{}

pub proof fn concat_path(c: p::Constants, a: Seq<p::State>, aa: Seq<p::Action>,
    b: Seq<p::State>, ba: Seq<p::Action>)
    requires path(c,a,aa), path(c,b,ba), a.last() == b[0],
    ensures path(c,a+b.drop_first(),aa+ba),
        (a+b.drop_first())[0] == a[0], (a+b.drop_first()).last() == b.last(),
{
    let states = a+b.drop_first();
    let actions = aa+ba;
    assert forall|i: int| 0 <= i < actions.len() implies
        states[i+1] == p::dispatch(c,states[i],actions[i]) by {
        if i < aa.len() {
            assert(states[i] == a[i]);
            assert(states[i+1] == a[i+1]);
        } else {
            let j = i-aa.len();
            assert(states[i] == b[j]);
            assert(states[i+1] == b[j+1]);
            assert(actions[i] == ba[j]);
        }
    }
}

pub proof fn append_segment(c: p::Constants, initial: p::State, log: Seq<Segment>, segment: Segment)
    requires log_ok(c,initial,log), certificate(c,replay(initial,log),segment),
    ensures log_ok(c,initial,log.push(segment)),
        replay(initial,log.push(segment)) == apply_writes(replay(initial,log),segment.writes),
{
    assert(log.push(segment).drop_last() =~= log);
}

/// Constructor coupling is a real caller obligation, including empty physical
/// replicas and canonical logical key/coordinate identities.
pub proof fn initialize(c: p::Constants, actual: p::State) -> (j: Journal)
    requires p::constants_ok(c), actual == p::initial(c),
    ensures closed(c,p::initial(c),j,actual), p::inv(c,actual),
        j.closed == Seq::<Segment>::empty(), j.pending is None,
{
    p::lemma_init(c);
    Journal { closed: Seq::empty(), pending: None }
}
pub proof fn open_segment(c: p::Constants, initial: p::State, j: Journal, actual: p::State)
    -> (out: Journal)
    requires closed(c,initial,j,actual),
    ensures mid(c,initial,out,actual), out.closed == j.closed,
        out.pending == Some(Seq::<Write>::empty()),
{
    Journal { closed: j.closed, pending: Some(Seq::empty()) }
}

/// Invoke next to the native mutation. `after == apply_write(before,w)` must
/// follow from that mutation's concrete view and frame proof, not a model call.
pub proof fn record_write(c: p::Constants, initial: p::State, j: Journal,
    before: p::State, after: p::State, w: Write) -> (out: Journal)
    requires mid(c,initial,j,before), j.pending is Some,
        writable(before,w), after == apply_write(before,w),
    ensures mid(c,initial,out,after), out.closed == j.closed,
        out.pending == Some(j.pending.unwrap().push(w)),
{
    append_write(replay(initial,j.closed),j.pending.unwrap(),w);
    Journal { closed: j.closed, pending: Some(j.pending.unwrap().push(w)) }
}

/// The generic wrapper binds the same write rule to an actual native view.
pub proof fn record_native<C>(c: p::Constants, initial: p::State, j: Journal,
    before: C, after: C, view: spec_fn(C) -> p::State, w: Write) -> (out: Journal)
    requires mid(c,initial,j,view(before)), j.pending is Some,
        writable(view(before),w), view(after) == apply_write(view(before),w),
    ensures mid(c,initial,out,view(after)), coupled(initial,out,after,view),
        out.closed == j.closed, out.pending == Some(j.pending.unwrap().push(w)),
{
    record_write(c,initial,j,view(before),view(after),w)
}

pub proof fn record_batch(c: p::Constants, initial: p::State, j: Journal,
    before: p::State, after: p::State, writes: Seq<Write>) -> (out: Journal)
    requires mid(c,initial,j,before), j.pending is Some,
        writes_ok(before,writes), after == apply_writes(before,writes),
    ensures mid(c,initial,out,after), out.closed == j.closed,
        out.pending == Some(j.pending.unwrap()+writes),
{
    concat_writes(replay(initial,j.closed),j.pending.unwrap(),writes);
    Journal { closed: j.closed, pending: Some(j.pending.unwrap()+writes) }
}

/// Close is a proof operation only: its caller has already proved the guard,
/// update and intermediate path correspondences from native observations.
pub proof fn close_segment(c: p::Constants, initial: p::State, j: Journal, actual: p::State,
    states: Seq<p::State>, actions: Seq<p::Action>) -> (out: Journal)
    requires mid(c,initial,j,actual), j.pending is Some,
        path(c,states,actions), states[0] == replay(initial,j.closed), states.last() == actual,
    ensures closed(c,initial,out,actual),
        out.closed == j.closed.push(Segment { writes: j.pending.unwrap(), states, actions }),
        out.pending is None,
{
    let segment = Segment { writes: j.pending.unwrap(), states, actions };
    append_segment(c,initial,j.closed,segment);
    Journal { closed: j.closed.push(segment), pending: None }
}

/// This is valid even with nonempty pending instrumentation, but only when its
/// entire projected effect is exactly identity. No status-to-stutter shortcut.
pub proof fn close_stutter(c: p::Constants, initial: p::State, j: Journal, actual: p::State)
    -> (out: Journal)
    requires mid(c,initial,j,actual), j.pending is Some,
        actual == replay(initial,j.closed),
    ensures closed(c,initial,out,actual), out.closed == j.closed.push(Segment {
        writes: j.pending.unwrap(), states: seq![actual,actual], actions: seq![p::Action::Stutter] }),
{
    stutter(c,actual,actual);
    close_segment(c,initial,j,actual,seq![actual,actual],seq![p::Action::Stutter])
}

pub proof fn prefix_ok(c: p::Constants, initial: p::State, log: Seq<Segment>, end: int)
    requires log_ok(c,initial,log), 0 <= end <= log.len(),
    ensures log_ok(c,initial,log.take(end)),
    decreases log.len(),
{
    if end == log.len() { assert(log.take(end) =~= log); }
    else {
        assert(log.drop_last().take(end) =~= log.take(end));
        prefix_ok(c,initial,log.drop_last(),end);
    }
}

/// Arbitrary finite history, including every step INSIDE every batch.
pub proof fn flatten(c: p::Constants, initial: p::State, log: Seq<Segment>)
    requires log_ok(c,initial,log),
    ensures path(c,behavior_states(initial,log),behavior_actions(log)),
        behavior_states(initial,log)[0] == initial,
        behavior_states(initial,log).last() == replay(initial,log),
    decreases log.len(),
{
    if log.len() > 0 {
        flatten(c,initial,log.drop_last());
        concat_path(c,behavior_states(initial,log.drop_last()),behavior_actions(log.drop_last()),
            log.last().states,log.last().actions);
    }
}

pub proof fn path_next(c: p::Constants, states: Seq<p::State>, actions: Seq<p::Action>)
    requires path(c,states,actions),
    ensures forall|i: int| 0 <= i < states.len()-1 ==> #[trigger] p::next(c,states[i],states[i+1]),
{
    assert forall|i: int| 0 <= i < states.len()-1 implies #[trigger] p::next(c,states[i],states[i+1]) by {
        assert(states[i+1] == p::dispatch(c,states[i],actions[i]));
    }
}

pub proof fn finite_behavior(c: p::Constants, log: Seq<Segment>)
    requires p::constants_ok(c), log_ok(c,p::initial(c),log),
    ensures p::behavior(c,behavior_states(p::initial(c),log)),
        p::inv(c,replay(p::initial(c),log)),
        forall|i: int| 0 <= i < behavior_states(p::initial(c),log).len() ==>
            #[trigger] p::inv(c,behavior_states(p::initial(c),log)[i]),
{
    let states = behavior_states(p::initial(c),log);
    flatten(c,p::initial(c),log);
    path_next(c,states,behavior_actions(log));
    assert(p::behavior(c,states));
    assert forall|i: int| 0 <= i < states.len() implies #[trigger] p::inv(c,states[i]) by {
        p::theorem_invariant(c,states,i);
    }
    p::theorem_invariant(c,states,states.len()-1);
}

/// Only closed native boundaries inherit invariance, not partially replayed
/// field writes. Representation, concurrency and engine atomicity remain the
/// caller's obligations rather than trusted semantics of this logger.
pub proof fn closed_native_invariant<C>(c: p::Constants, j: Journal, native: C,
    view: spec_fn(C) -> p::State)
    requires p::constants_ok(c), closed(c,p::initial(c),j,view(native)),
    ensures p::inv(c,view(native)), p::behavior(c,behavior_states(p::initial(c),j.closed)),
{
    finite_behavior(c,j.closed);
}

} // verus!
