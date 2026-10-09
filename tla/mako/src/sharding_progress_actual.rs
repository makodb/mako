//! Service-facing liveness for actual, unrestricted placement executions.
//! There is no successful script, chosen transaction write payload, reserved
//! settlement slot, or pre-freeze admission closure in this interface.
use super::*;
use super::shape as sh;
#[path = "sharding_progress_actual_witness.rs"]
pub mod witness;

verus! {

pub struct Execution {
    pub state: spec_fn(nat) -> p::State,
    pub action: spec_fn(nat) -> p::Action,
}

impl Execution {
    pub open spec fn state(self, t: nat) -> p::State { (self.state)(t) }
    pub open spec fn action(self, t: nat) -> p::Action { (self.action)(t) }
}

/// Before `base` recovery/failures may occur. At `base` the recovered state is
/// an exact reachable placement state; thereafter successful atomic calls and
/// authentic deliveries use dispatch. Failed I/O/dropped messages may stutter.
/// The recovery adapter establishing this suffix remains outside this proof.
pub open spec fn stable_steps(c: p::Constants, b: Execution, base: nat) -> bool {
    forall|t: nat| t >= base ==> b.state(t+1) == p::dispatch(c,b.state(t),b.action(t))
}

pub open spec fn execution(c: p::Constants, b: Execution, base: nat) -> bool {
    sh::progress_inv(c,b.state(base)) && stable_steps(c,b,base)
}

/// Only concrete calls needed by this migration are fair. Transactions are
/// NOT resolved by fairness of an exact Resolve payload. Admission, ordinary
/// writes, background work, and predecision Abort remain unconstrained.
/// Inactive generations have no control/copy/receipt retry obligations, and an
/// already-retained reply needs no further retry. Fairness never assumes
/// eventual drain, copy coverage, phase advancement, or completion.
pub open spec fn primitive(s: p::State, g: nat, a: p::Action) -> bool {
    match a {
        p::Action::RequestFreeze | p::Action::RequestFinal | p::Action::RequestRetire
        | p::Action::Commit | p::Action::Finish => p::current(s,g),
        p::Action::Deliver { generation,owner,.. } => p::current(s,g) && generation == g
            && (owner == s.plans[g].src || owner == s.plans[g].dst),
        p::Action::Drain { generation } => p::current(s,g) && generation == g
            && s.phases[g] == p::Phase::Freezing,
        // A final mirror needs the still-fenced source. Once an administrative
        // abort wins, only its cleanup is required; an old final job may reject.
        p::Action::Seal { generation } => p::current(s,g) && generation == g
            && s.phases[g] == p::Phase::Final,
        p::Action::Receive { generation,certificate } => p::current(s,g) && generation == g
            && !s.received.contains((g,certificate)),
        p::Action::Capture { generation,round,key } => p::current(s,g) && generation == g && round == 1
            && s.plans[g].keys.contains(key),
        p::Action::DeliverCopy { packet } => p::current(s,g) && packet.generation == g && packet.round == 1,
        p::Action::Release { key,.. } => s.plans[g].keys.contains(key),
        p::Action::Reply { nonce } => nonce == s.plans[g].nonce && !s.replies.contains_key(nonce),
        _ => false,
    }
}

pub open spec fn continuously_needed(c:p::Constants,b:Execution,g:nat,a:p::Action,t:nat) -> bool {
    forall|u:nat| u >= t ==> primitive(b.state(u),g,a) && p::enabled(c,b.state(u),a)
}

pub open spec fn call_fair(c: p::Constants, b: Execution, base: nat, g: nat, a: p::Action) -> bool {
    forall|t: nat| t >= base && #[trigger] continuously_needed(c,b,g,a,t) ==>
        exists|u: nat| u >= t && b.action(u) == a
}

pub open spec fn fair(c: p::Constants, b: Execution, base: nat, g: nat) -> bool {
    forall|a: p::Action| call_fair(c,b,base,g,a)
}

pub open spec fn cleanup_fair(c: p::Constants, b: Execution, base: nat, g: nat) -> bool {
    forall|a: p::Action| a is Deliver || a is Receive || a is Finish || a is Reply ==>
        call_fair(c,b,base,g,a)
}

/// Trusted engine terminality is existential: the actual transaction may
/// commit ANY legal writes or abort. Release is a separate fair primitive.
pub open spec fn engines_settle(b: Execution, base: nat, g: nat) -> bool {
    forall|txn: int, t: nat| t >= base && selected_holders(b.state(t),g).contains(txn) ==>
        exists|u: nat| u >= t && b.state(u).sessions.contains_key(txn) && b.state(u).sessions[txn].resolved
}

pub open spec fn exact_holds(s: p::State, g: nat, holds: Seq<(int,int)>) -> bool {
    forall|txn: int, key: int| holds.contains((txn,key)) <==>
        s.plans[g].keys.contains(key) && s.sessions.contains_key(txn) && s.sessions[txn].held.contains_key(key)
}

/// Finiteness is needed at the actual physical freeze, not at Begin. There is
/// no bound on the number of admissions before Freeze eventually takes effect.
pub open spec fn finite_admission(b: Execution, base: nat, g: nat) -> bool {
    forall|t: nat| t >= base && sh::source_frozen(b.state(t),g) ==>
        exists|holds: Seq<(int,int)>| exact_holds(b.state(t),g,holds)
}

pub open spec fn finite_image(s: p::State, g: nat, keys: Seq<int>) -> bool {
    forall|k: int| s.plans[g].keys.contains(k) <==> keys.contains(k)
}

pub proof fn reachable_progress_shape(c: p::Constants, states: Seq<p::State>)
    requires p::constants_ok(c), p::behavior(c,states)
    ensures sh::progress_inv(c,states.last())
    decreases states.len()
{
    if states.len() == 1 { sh::progress_initial(c); }
    else {
        let prefix = states.drop_last();
        assert(p::behavior(c,prefix));
        reachable_progress_shape(c,prefix);
        let i = (states.len()-2) as int;
        assert(p::next(c,states[i],states[i+1]));
        assert(prefix.last() == states[(states.len()-2) as int]);
        assert(states.last() == states[(states.len()-1) as int]);
        assert(p::next(c,prefix.last(),states.last()));
        let a = choose|a: p::Action| states.last() == p::dispatch(c,prefix.last(),a);
        sh::progress_step(c,prefix.last(),a);
    }
}

pub proof fn state_at(c: p::Constants, b: Execution, base: nat, t: nat)
    requires execution(c,b,base), t >= base
    ensures sh::progress_inv(c,b.state(t)),p::inv(c,b.state(t)),
        forall|g:nat| p::current(b.state(t),g) ==> b.state(t).plans.contains_key(g)
            && sh::command_shape(b.state(t),g) && sh::bulk_shape(b.state(t),g) && p::plan_inv(c,b.state(t),g)
    decreases t-base
{
    if t > base {
        state_at(c,b,base,(t-1) as nat);
        sh::progress_step(c,b.state((t-1) as nat),b.action((t-1) as nat));
    }
    assert forall|g:nat| p::current(b.state(t),g) implies b.state(t).plans.contains_key(g)
        && sh::command_shape(b.state(t),g) && sh::bulk_shape(b.state(t),g) && p::plan_inv(c,b.state(t),g) by {
        sh::active_facts(c,b.state(t),g);
    }
}

pub proof fn metadata_span(c: p::Constants, b: Execution, base: nat, g: nat, t: nat, u: nat)
    requires execution(c,b,base), base <= t <= u,
        b.state(t).plans.contains_key(g) || p::current(b.state(t),g)
    ensures b.state(u).plans.contains_key(g), b.state(u).plans[g] == b.state(t).plans[g],
        sh::phase_number(b.state(u),g) >= sh::phase_number(b.state(t),g),
        b.state(t).commands.subset_of(b.state(u).commands),
        b.state(t).certificates.subset_of(b.state(u).certificates),
        b.state(t).received.subset_of(b.state(u).received),
        b.state(t).packets.subset_of(b.state(u).packets),
        !p::current(b.state(t),g) ==> !p::current(b.state(u),g),
        p::terminal(b.state(t).phases[g]) ==>
            b.state(u).phases[g] == b.state(t).phases[g]
                && b.state(u).outcomes[b.state(t).plans[g].nonce] == b.state(t).outcomes[b.state(t).plans[g].nonce]
    decreases u-t
{
    state_at(c,b,base,t);
    if u > t {
        let v = (u-1) as nat;
        metadata_span(c,b,base,g,t,v);
        state_at(c,b,base,v);
        sh::metadata_monotone(c,b.state(v),b.action(v),g);
        if p::terminal(b.state(t).phases[g]) {
            theorem_decision_never_reverts(c,b.state(v),b.action(v),g);
        }
    }
}

pub proof fn session_span(c: p::Constants, b: Execution, base: nat, txn: int, t: nat, u: nat)
    requires execution(c,b,base), base <= t <= u, b.state(t).sessions.contains_key(txn)
    ensures b.state(u).sessions.contains_key(txn),
        b.state(t).sessions[txn].resolved ==> b.state(u).sessions[txn].resolved
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        session_span(c,b,base,txn,t,v);
        state_at(c,b,base,v);
        sh::session_monotone(c,b.state(v),b.action(v),txn);
    }
}

pub open spec fn stays(b: Execution, g: nat, start: nat, phase: p::Phase) -> bool {
    forall|u: nat| u >= start ==> p::current(b.state(u),g) && b.state(u).phases[g] == phase
}

pub proof fn local_span(c: p::Constants, b: Execution, base: nat, g: nat,
    start: nat, t: nat, u: nat, phase: p::Phase)
    requires execution(c,b,base), base <= start <= t <= u, stays(b,g,start,phase), !p::terminal(phase)
    ensures sh::source_frozen(b.state(t),g) && phase is Freezing ==> sh::source_frozen(b.state(u),g),
        sh::destination_started(b.state(t),g) && phase is Final ==> sh::destination_started(b.state(u),g),
        sh::destination_final(b.state(t),g) && phase is Final ==> sh::destination_final(b.state(u),g),
        forall|k: int| b.state(t).plans[g].keys.contains(k) && sh::destination_final(b.state(t),g)
            && phase is Final && p::replica(b.state(t),b.state(t).plans[g].dst,k).covered ==>
            p::replica(b.state(u),b.state(u).plans[g].dst,k).covered
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        local_span(c,b,base,g,start,t,v,phase);
        metadata_span(c,b,base,g,t,v);
        state_at(c,b,base,v);
        sh::same_phase_local_step(c,b.state(v),b.action(v),g);
    }
}

pub proof fn frozen_holds_span(c: p::Constants, b: Execution, base: nat, g: nat,
    freeze: nat, t: nat, u: nat)
    requires execution(c,b,base), base <= freeze <= t <= u,
        stays(b,g,freeze,p::Phase::Freezing), sh::source_frozen(b.state(freeze),g)
    ensures forall|txn: int, key: int| b.state(freeze).plans[g].keys.contains(key)
        && b.state(u).sessions.contains_key(txn) && b.state(u).sessions[txn].held.contains_key(key) ==>
        b.state(t).sessions.contains_key(txn) && b.state(t).sessions[txn].held.contains_key(key)
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        frozen_holds_span(c,b,base,g,freeze,t,v);
        local_span(c,b,base,g,freeze,freeze,v,p::Phase::Freezing);
        metadata_span(c,b,base,g,freeze,v);
        state_at(c,b,base,v);
        sh::frozen_holds_step(c,b.state(v),b.action(v),g);
    }
}

pub proof fn fair_call(c: p::Constants, b: Execution, base: nat, g: nat, t: nat, a: p::Action) -> (u: nat)
    requires call_fair(c,b,base,g,a), t >= base,
        forall|v: nat| v >= t ==> primitive(b.state(v),g,a) && p::enabled(c,b.state(v),a)
    ensures u >= t, b.action(u) == a, p::enabled(c,b.state(u),a)
{
    assert(continuously_needed(c,b,g,a,t));
    choose|u: nat| u >= t && b.action(u) == a
}

pub proof fn deliver_command(c: p::Constants, b: Execution, base: nat, g: nat,
    t: nat, cmd: p::Command, owner: int) -> (u: nat)
    requires execution(c,b,base), t >= base, b.state(t).plans.contains_key(g),
        call_fair(c,b,base,g,p::Action::Deliver { generation: g, command: cmd, owner }),
        b.state(t).commands.contains((g,cmd)),
        forall|v: nat| v >= t ==> p::current(b.state(v),g),
        owner == b.state(t).plans[g].src || owner == b.state(t).plans[g].dst
    ensures u >= t, b.action(u) == (p::Action::Deliver { generation: g, command: cmd, owner })
{
    let a = p::Action::Deliver { generation: g, command: cmd, owner };
    assert forall|v: nat| v >= t implies primitive(b.state(v),g,a) && p::enabled(c,b.state(v),a) by {
        metadata_span(c,b,base,g,t,v);
    }
    fair_call(c,b,base,g,t,a)
}

pub proof fn receive_certificate(c: p::Constants, b: Execution, base: nat, g: nat,
    t: nat, cert: p::Certificate) -> (u: nat)
    requires execution(c,b,base), t >= base, b.state(t).plans.contains_key(g),
        call_fair(c,b,base,g,p::Action::Receive { generation: g, certificate: cert }),
        b.state(t).certificates.contains((g,cert)),
        cert is SourceDone || cert is DestinationDone || (forall|v: nat| v >= t ==> p::current(b.state(v),g))
    ensures u >= t, b.state(u).received.contains((g,cert))
{
    if !(exists|u: nat| u >= t && b.state(u).received.contains((g,cert))) {
        let a = p::Action::Receive { generation: g, certificate: cert };
        assert forall|v: nat| v >= t implies primitive(b.state(v),g,a) && p::enabled(c,b.state(v),a) by {
            metadata_span(c,b,base,g,t,v);
            state_at(c,b,base,v);
            if !p::current(b.state(v),g) {
                assert(b.state(v).received.contains((g,p::Certificate::SourceDone))
                    && b.state(v).received.contains((g,p::Certificate::DestinationDone)));
            }
        }
        let u = fair_call(c,b,base,g,t,a);
        assert(b.state(u+1).received.contains((g,cert)));
    }
    choose|u: nat| u >= t && b.state(u).received.contains((g,cert))
}

pub proof fn perpetual_copy_impossible(c: p::Constants, b: Execution, base: nat, g: nat, t: nat)
    requires execution(c,b,base), fair(c,b,base,g), t >= base, stays(b,g,t,p::Phase::Copy)
    ensures false
{
    let a = p::Action::RequestFreeze;
    assert forall|u: nat| u >= t implies primitive(b.state(u),g,a) && p::enabled(c,b.state(u),a) by { }
    let u = fair_call(c,b,base,g,t,a);
    assert(b.state(u+1).phases[g] is Freezing);
}

/// The first actual Freeze delivery freezes the whole source range; there may
/// have been arbitrarily many selected-range acquisitions before this instant.
pub proof fn freeze_delivery(c: p::Constants, s: p::State, g: nat)
    requires sh::progress_inv(c,s), p::current(s,g), s.phases[g] is Freezing
    ensures sh::source_frozen(p::dispatch(c,s,p::Action::Deliver {
        generation: g, command: p::Command::Freeze, owner: s.plans[g].src }),g)
{
    sh::active_facts(c,s,g);
    let k = sh::nonempty_plan(c,s,g);
    assert(p::key_inv(c,s,k));
    assert(sh::source_pre(s,g) || sh::source_frozen(s,g));
    if sh::source_pre(s,g) {
        assert(p::local_guard(s,g,p::Command::Freeze,s.plans[g].src)) by {
            assert forall|q: int| s.plans[g].keys.contains(q) implies
                p::replica(s,s.plans[g].src,q).epoch == s.plans[g].old[q].epoch by {
                sh::selected_key(c,s,g,q);
            }
        }
    }
}

pub open spec fn held(s: p::State, txn: int, key: int) -> bool {
    s.sessions.contains_key(txn) && s.sessions[txn].held.contains_key(key)
}

/// A particular old hold eventually disappears. Resolve's value is not chosen
/// here: engine terminality supplies an actual terminal state, after which
/// fair Release is enabled unless somebody already released the hold.
pub proof fn old_hold_eventually_released(c: p::Constants, b: Execution, base: nat, g: nat,
    freeze: nat, now: nat, txn: int, key: int) -> (u: nat)
    requires execution(c,b,base), fair(c,b,base,g), engines_settle(b,base,g),
        base <= freeze <= now, stays(b,g,freeze,p::Phase::Freezing),
        sh::source_frozen(b.state(freeze),g), b.state(freeze).plans[g].keys.contains(key)
    ensures u >= now, !held(b.state(u),txn,key)
{
    metadata_span(c,b,base,g,freeze,now);
    if !held(b.state(now),txn,key) { now }
    else {
        assert(selected_holders(b.state(now),g).contains(txn));
        let done = choose|u: nat| u >= now && b.state(u).sessions.contains_key(txn) && b.state(u).sessions[txn].resolved;
        if !(exists|u: nat| u >= done && !held(b.state(u),txn,key)) {
            let a = p::Action::Release { txn,key };
            assert forall|v: nat| v >= done implies primitive(b.state(v),g,a) && p::enabled(c,b.state(v),a) by {
                metadata_span(c,b,base,g,freeze,v);
                session_span(c,b,base,txn,done,v);
            }
            let v = fair_call(c,b,base,g,done,a);
            assert(!held(b.state(v+1),txn,key));
        }
        choose|u: nat| u >= done && !held(b.state(u),txn,key)
    }
}

pub proof fn release_finite_holds(c: p::Constants, b: Execution, base: nat, g: nat,
    freeze: nat, now: nat, work: Seq<(int,int)>) -> (u: nat)
    requires execution(c,b,base), fair(c,b,base,g), engines_settle(b,base,g),
        base <= freeze <= now, stays(b,g,freeze,p::Phase::Freezing), sh::source_frozen(b.state(freeze),g),
        forall|i: int| 0 <= i < work.len() ==> b.state(freeze).plans[g].keys.contains(work[i].1)
    ensures u >= now, forall|i: int| 0 <= i < work.len() ==> !held(b.state(u),work[i].0,work[i].1)
    decreases work.len()
{
    if work.len() == 0 { now }
    else {
        let pair = work.last();
        let v = old_hold_eventually_released(c,b,base,g,freeze,now,pair.0,pair.1);
        let u = release_finite_holds(c,b,base,g,freeze,v,work.drop_last());
        frozen_holds_span(c,b,base,g,freeze,v,u);
        assert forall|i: int| 0 <= i < work.len() implies !held(b.state(u),work[i].0,work[i].1) by {
            if i < work.len()-1 { assert(work[i] == work.drop_last()[i]); }
            else { assert(work[i] == pair); }
        }
        u
    }
}

pub proof fn perpetual_freezing_impossible(c: p::Constants, b: Execution, base: nat, g: nat, t: nat)
    requires execution(c,b,base), fair(c,b,base,g), engines_settle(b,base,g), finite_admission(b,base,g),
        t >= base, stays(b,g,t,p::Phase::Freezing)
    ensures false
{
    state_at(c,b,base,t);
    let f = deliver_command(c,b,base,g,t,p::Command::Freeze,b.state(t).plans[g].src);
    state_at(c,b,base,f);
    metadata_span(c,b,base,g,t,f);
    freeze_delivery(c,b.state(f),g);
    let freeze = f+1;
    assert(sh::source_frozen(b.state(freeze),g));
    let work = choose|work: Seq<(int,int)>| exact_holds(b.state(freeze),g,work);
    assert(exact_holds(b.state(freeze),g,work));
    assert forall|i: int| 0 <= i < work.len() implies b.state(freeze).plans[g].keys.contains(work[i].1) by {
        assert(work.contains(work[i]));
        assert(work.contains((work[i].0,work[i].1)));
    }
    let drained_at = release_finite_holds(c,b,base,g,freeze,freeze,work);
    frozen_holds_span(c,b,base,g,freeze,freeze,drained_at);
    metadata_span(c,b,base,g,freeze,drained_at);
    assert(p::drained(b.state(drained_at),b.state(drained_at).plans[g].keys)) by {
        assert forall|txn: int, key: int| b.state(drained_at).sessions.contains_key(txn)
            && b.state(drained_at).plans[g].keys.contains(key) implies
            !b.state(drained_at).sessions[txn].held.contains_key(key) by {
            if held(b.state(drained_at),txn,key) {
                assert(work.contains((txn,key)));
                let i = choose|i: int| 0 <= i < work.len() && work[i] == (txn,key);
            }
        }
    }
    let a = p::Action::Drain { generation: g };
    assert forall|u: nat| u >= drained_at implies primitive(b.state(u),g,a) && p::enabled(c,b.state(u),a) by {
        local_span(c,b,base,g,freeze,freeze,u,p::Phase::Freezing);
        frozen_holds_span(c,b,base,g,freeze,drained_at,u);
        metadata_span(c,b,base,g,freeze,u);
    }
    let d = fair_call(c,b,base,g,drained_at,a);
    assert(b.state(d+1).certificates.contains((g,p::Certificate::Drained)));
    metadata_span(c,b,base,g,t,d+1);
    let received = receive_certificate(c,b,base,g,d+1,p::Certificate::Drained);
    let a = p::Action::RequestFinal;
    assert forall|u: nat| u >= received implies primitive(b.state(u),g,a) && p::enabled(c,b.state(u),a) by {
        metadata_span(c,b,base,g,received,u);
    }
    let u = fair_call(c,b,base,g,received,a);
    assert(b.state(u+1).phases[g] is Final);
}

pub proof fn start_delivery(c: p::Constants, s: p::State, g: nat)
    requires sh::progress_inv(c,s), p::current(s,g), s.phases[g] is Final
    ensures sh::destination_started(p::dispatch(c,s,p::Action::Deliver {
        generation: g, command: p::Command::Start, owner: s.plans[g].dst }),g)
{
    sh::active_facts(c,s,g);
    let k = sh::nonempty_plan(c,s,g);
    assert(p::key_inv(c,s,k));
    assert(sh::destination_pre(s,g) || sh::destination_started(s,g));
    if sh::destination_pre(s,g) { assert(p::local_guard(s,g,p::Command::Start,s.plans[g].dst)); }
}

pub proof fn final_delivery(c: p::Constants, s: p::State, g: nat)
    requires sh::progress_inv(c,s), p::current(s,g), s.phases[g] is Final, sh::destination_started(s,g)
    ensures sh::destination_final(p::dispatch(c,s,p::Action::Deliver {
        generation: g, command: p::Command::Final, owner: s.plans[g].dst }),g)
{
    sh::active_facts(c,s,g);
    let k = sh::nonempty_plan(c,s,g);
    if sh::destination_stage(s,g,0) { assert(p::local_guard(s,g,p::Command::Final,s.plans[g].dst)); }
}

/// One actual final-round packet is captured and then fairly delivered. Its
/// contents are obtained from that execution, not selected in a witness script.
pub proof fn key_eventually_covered(c: p::Constants, b: Execution, base: nat, g: nat,
    final_at: nat, now: nat, key: int) -> (u: nat)
    requires execution(c,b,base), fair(c,b,base,g), base <= final_at <= now,
        stays(b,g,final_at,p::Phase::Final), sh::destination_final(b.state(final_at),g),
        b.state(final_at).plans[g].keys.contains(key)
    ensures u >= now, p::replica(b.state(u),b.state(u).plans[g].dst,key).covered,
        p::replica(b.state(u),b.state(u).plans[g].dst,key).cell == b.state(u).logical[key]
{
    let capture = p::Action::Capture { generation: g, round: 1, key };
    assert forall|u: nat| u >= now implies primitive(b.state(u),g,capture) && p::enabled(c,b.state(u),capture) by {
        state_at(c,b,base,u);
        metadata_span(c,b,base,g,final_at,u);
        assert(p::plan_inv(c,b.state(u),g));
        assert(p::key_inv(c,b.state(u),key));
    }
    let t = fair_call(c,b,base,g,now,capture);
    let packet = p::Packet { generation: g, round: 1, key,
        cell: p::replica(b.state(t),b.state(t).plans[g].src,key).cell };
    assert(b.state(t+1).packets.contains(packet));
    metadata_span(c,b,base,g,final_at,t+1);
    let delivery = p::Action::DeliverCopy { packet };
    assert forall|v: nat| v >= t+1 implies primitive(b.state(v),g,delivery) && p::enabled(c,b.state(v),delivery) by {
        metadata_span(c,b,base,g,t+1,v);
    }
    let v = fair_call(c,b,base,g,t+1,delivery);
    state_at(c,b,base,v);
    local_span(c,b,base,g,final_at,final_at,v,p::Phase::Final);
    metadata_span(c,b,base,g,final_at,v);
    if sh::destination_stage(b.state(v),g,1) { assert(p::copy_guard(b.state(v),packet)); }
    assert(p::replica(b.state(v+1),b.state(v+1).plans[g].dst,key).covered);
    state_at(c,b,base,v+1);
    local_span(c,b,base,g,final_at,final_at,v+1,p::Phase::Final);
    assert(p::key_inv(c,b.state(v+1),key));
    v+1
}

/// Exact source-image stability follows from the proved drain/lease invariant
/// and atomic successful transactions; it is not a checksum or fairness axiom.
pub proof fn final_image_span(c: p::Constants, b: Execution, base: nat, g: nat,
    start: nat, t: nat, u: nat)
    requires execution(c,b,base), base <= start <= t <= u,
        stays(b,g,start,p::Phase::Final)
    ensures forall|k: int| b.state(t).plans[g].keys.contains(k) ==>
        b.state(u).logical[k] == b.state(t).logical[k]
            && p::replica(b.state(u),b.state(u).plans[g].src,k).cell
                == p::replica(b.state(t),b.state(t).plans[g].src,k).cell
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        final_image_span(c,b,base,g,start,t,v);
        metadata_span(c,b,base,g,t,v);
        metadata_span(c,b,base,g,t,u);
        state_at(c,b,base,v);
        assert(p::plan_inv(c,b.state(v),g));
        assert forall|k: int| b.state(t).plans[g].keys.contains(k) implies
            b.state(u).logical[k] == b.state(t).logical[k]
                && p::replica(b.state(u),b.state(u).plans[g].src,k).cell
                    == p::replica(b.state(t),b.state(t).plans[g].src,k).cell by {
            if p::enabled(c,b.state(v),b.action(v)) {
                p::lemma_frozen_store_stable(c,b.state(v),b.action(v),g,k);
            }
        }
    }
}

pub proof fn copy_finite_image(c: p::Constants, b: Execution, base: nat, g: nat,
    final_at: nat, now: nat, keys: Seq<int>) -> (u: nat)
    requires execution(c,b,base), fair(c,b,base,g), base <= final_at <= now,
        stays(b,g,final_at,p::Phase::Final), sh::destination_final(b.state(final_at),g),
        forall|i: int| 0 <= i < keys.len() ==> b.state(final_at).plans[g].keys.contains(keys[i])
    ensures u >= now, forall|i: int| 0 <= i < keys.len() ==>
        p::replica(b.state(u),b.state(u).plans[g].dst,keys[i]).covered
            && p::replica(b.state(u),b.state(u).plans[g].dst,keys[i]).cell
                == b.state(final_at).logical[keys[i]]
    decreases keys.len()
{
    if keys.len() == 0 { now }
    else {
        let key = keys.last();
        let v = key_eventually_covered(c,b,base,g,final_at,now,key);
        let u = copy_finite_image(c,b,base,g,final_at,v,keys.drop_last());
        local_span(c,b,base,g,final_at,v,u,p::Phase::Final);
        local_span(c,b,base,g,final_at,final_at,v,p::Phase::Final);
        metadata_span(c,b,base,g,final_at,v);
        assert forall|i: int| 0 <= i < keys.len() implies
            p::replica(b.state(u),b.state(u).plans[g].dst,keys[i]).covered by {
            if i < keys.len()-1 { assert(keys[i] == keys.drop_last()[i]); }
            else { assert(keys[i] == key); }
        }
        state_at(c,b,base,u);
        local_span(c,b,base,g,final_at,final_at,u,p::Phase::Final);
        metadata_span(c,b,base,g,final_at,u);
        final_image_span(c,b,base,g,final_at,final_at,u);
        assert forall|i: int| 0 <= i < keys.len() implies
            p::replica(b.state(u),b.state(u).plans[g].dst,keys[i]).cell
                == b.state(final_at).logical[keys[i]] by {
            assert(p::key_inv(c,b.state(u),keys[i]));
        }
        u
    }
}

pub proof fn final_destination_eventually_started(c:p::Constants,b:Execution,base:nat,g:nat,t:nat)->(u:nat)
    requires execution(c,b,base),fair(c,b,base,g),t >= base,stays(b,g,t,p::Phase::Final)
    ensures u >= t,sh::destination_final(b.state(u),g)
{
    state_at(c,b,base,t);
    let start = deliver_command(c,b,base,g,t,p::Command::Start,b.state(t).plans[g].dst);
    metadata_span(c,b,base,g,t,start); state_at(c,b,base,start);
    start_delivery(c,b.state(start),g); state_at(c,b,base,start+1);
    let finish = deliver_command(c,b,base,g,start+1,p::Command::Final,b.state(start+1).plans[g].dst);
    local_span(c,b,base,g,t,start+1,finish,p::Phase::Final);
    metadata_span(c,b,base,g,start+1,finish);
    state_at(c,b,base,finish); final_delivery(c,b.state(finish),g);
    finish+1
}

pub proof fn final_image_eventually_covered(c:p::Constants,b:Execution,base:nat,g:nat,t:nat,keys:Seq<int>)->(u:nat)
    requires execution(c,b,base),fair(c,b,base,g),t >= base,stays(b,g,t,p::Phase::Final),
        finite_image(b.state(t),g,keys),sh::destination_final(b.state(t),g)
    ensures u >= t,sh::destination_final(b.state(u),g),
        forall|k:int| b.state(u).plans[g].keys.contains(k) ==>
            p::replica(b.state(u),b.state(u).plans[g].dst,k).covered
                && p::replica(b.state(u),b.state(u).plans[g].dst,k).cell == b.state(u).logical[k]
{
    assert forall|i:int| 0 <= i < keys.len() implies b.state(t).plans[g].keys.contains(keys[i]) by {
        assert(keys.contains(keys[i]));
    }
    let u = copy_finite_image(c,b,base,g,t,t,keys);
    metadata_span(c,b,base,g,t,u); local_span(c,b,base,g,t,t,u,p::Phase::Final);
    final_image_span(c,b,base,g,t,t,u);
    assert forall|k:int| b.state(u).plans[g].keys.contains(k) implies
        p::replica(b.state(u),b.state(u).plans[g].dst,k).covered
            && p::replica(b.state(u),b.state(u).plans[g].dst,k).cell == b.state(u).logical[k] by {
        assert(keys.contains(k));
        let i = choose|i:int| 0 <= i < keys.len() && keys[i] == k;
    }
    u
}

pub proof fn final_ready_eventually_available(c:p::Constants,b:Execution,base:nat,g:nat,t:nat)->(u:nat)
    requires execution(c,b,base),fair(c,b,base,g),t >= base,stays(b,g,t,p::Phase::Final),
        sh::destination_final(b.state(t),g),
        forall|k:int| b.state(t).plans[g].keys.contains(k) ==> p::replica(b.state(t),b.state(t).plans[g].dst,k).covered
    ensures u >= t,b.state(u).certificates.contains((g,p::Certificate::Ready))
{
    if !(exists|u:nat| u >= t && b.state(u).certificates.contains((g,p::Certificate::Ready))) {
        let seal = p::Action::Seal { generation:g };
        assert forall|u:nat| u >= t implies primitive(b.state(u),g,seal) && p::enabled(c,b.state(u),seal) by {
            state_at(c,b,base,u); local_span(c,b,base,g,t,t,u,p::Phase::Final);
            metadata_span(c,b,base,g,t,u);
            assert(!sh::destination_ready(b.state(u),g));
            assert(sh::destination_stage(b.state(u),g,1));
        }
        let u = fair_call(c,b,base,g,t,seal);
        assert(b.state(u+1).certificates.contains((g,p::Certificate::Ready)));
    }
    choose|u:nat| u >= t && b.state(u).certificates.contains((g,p::Certificate::Ready))
}

pub proof fn perpetual_final_impossible(c: p::Constants, b: Execution, base: nat, g: nat,
    t: nat, keys: Seq<int>)
    requires execution(c,b,base), fair(c,b,base,g), t >= base,
        stays(b,g,t,p::Phase::Final), finite_image(b.state(t),g,keys)
    ensures false
{
    let final_at = final_destination_eventually_started(c,b,base,g,t);
    metadata_span(c,b,base,g,t,final_at);
    let copied = final_image_eventually_covered(c,b,base,g,final_at,keys);
    assert forall|k:int| b.state(copied).plans[g].keys.contains(k) implies
        p::replica(b.state(copied),b.state(copied).plans[g].dst,k).covered by {
        assert(p::replica(b.state(copied),b.state(copied).plans[g].dst,k).covered
            && p::replica(b.state(copied),b.state(copied).plans[g].dst,k).cell == b.state(copied).logical[k]);
    }
    let ready = final_ready_eventually_available(c,b,base,g,copied);
    metadata_span(c,b,base,g,t,ready);
    let received = receive_certificate(c,b,base,g,ready,p::Certificate::Ready);
    let request = p::Action::RequestRetire;
    assert forall|u: nat| u >= received implies primitive(b.state(u),g,request) && p::enabled(c,b.state(u),request) by {
        metadata_span(c,b,base,g,received,u);
    }
    let u = fair_call(c,b,base,g,received,request);
    assert(b.state(u+1).phases[g] is Retiring);
}

pub proof fn retire_delivery(c: p::Constants, s: p::State, g: nat)
    requires sh::progress_inv(c,s), p::current(s,g), s.phases[g] is Retiring
    ensures p::dispatch(c,s,p::Action::Deliver { generation: g, command: p::Command::Retire,
        owner: s.plans[g].src }).certificates.contains((g,p::Certificate::Retired))
{
    sh::active_facts(c,s,g);
    let k = sh::nonempty_plan(c,s,g);
    assert(p::key_inv(c,s,k));
    assert(p::plan_inv(c,s,g));
    assert(sh::source_frozen(s,g) || sh::source_retired(s,g));
    if sh::source_frozen(s,g) { assert(p::local_guard(s,g,p::Command::Retire,s.plans[g].src)); }
}

pub proof fn perpetual_retiring_impossible(c: p::Constants, b: Execution, base: nat, g: nat, t: nat)
    requires execution(c,b,base), fair(c,b,base,g), t >= base, stays(b,g,t,p::Phase::Retiring)
    ensures false
{
    state_at(c,b,base,t);
    let retired = deliver_command(c,b,base,g,t,p::Command::Retire,b.state(t).plans[g].src);
    state_at(c,b,base,retired);
    metadata_span(c,b,base,g,t,retired);
    retire_delivery(c,b.state(retired),g);
    metadata_span(c,b,base,g,t,retired+1);
    let received = receive_certificate(c,b,base,g,retired+1,p::Certificate::Retired);
    let a = p::Action::Commit;
    assert forall|u: nat| u >= received implies primitive(b.state(u),g,a) && p::enabled(c,b.state(u),a) by {
        metadata_span(c,b,base,g,received,u);
    }
    let u = fair_call(c,b,base,g,received,a);
    assert(b.state(u+1).phases[g] is Committed);
}

pub proof fn phase_eventually_advances(c: p::Constants, b: Execution, base: nat, g: nat,
    t: nat, keys: Seq<int>) -> (u: nat)
    requires execution(c,b,base), fair(c,b,base,g), engines_settle(b,base,g), finite_admission(b,base,g),
        t >= base, b.state(t).plans.contains_key(g), sh::phase_number(b.state(t),g) < 4,
        finite_image(b.state(t),g,keys)
    ensures u >= t, sh::phase_number(b.state(u),g) > sh::phase_number(b.state(t),g)
{
    if !(exists|u: nat| u >= t && sh::phase_number(b.state(u),g) > sh::phase_number(b.state(t),g)) {
        let phase = b.state(t).phases[g];
        assert forall|u: nat| u >= t implies p::current(b.state(u),g) && b.state(u).phases[g] == phase by {
            metadata_span(c,b,base,g,t,u);
            state_at(c,b,base,u);
            assert(p::plan_inv(c,b.state(u),g));
        }
        match phase {
            p::Phase::Copy => perpetual_copy_impossible(c,b,base,g,t),
            p::Phase::Freezing => perpetual_freezing_impossible(c,b,base,g,t),
            p::Phase::Final => perpetual_final_impossible(c,b,base,g,t,keys),
            p::Phase::Retiring => perpetual_retiring_impossible(c,b,base,g,t),
            _ => {},
        }
    }
    choose|u: nat| u >= t && sh::phase_number(b.state(u),g) > sh::phase_number(b.state(t),g)
}

pub proof fn eventually_decides(c: p::Constants, b: Execution, base: nat, g: nat,
    t: nat, keys: Seq<int>) -> (u: nat)
    requires execution(c,b,base), fair(c,b,base,g), engines_settle(b,base,g), finite_admission(b,base,g),
        t >= base, b.state(t).plans.contains_key(g), finite_image(b.state(t),g,keys)
    ensures u >= t, p::terminal(b.state(u).phases[g])
    decreases 4-sh::phase_number(b.state(t),g)
{
    if p::terminal(b.state(t).phases[g]) { t }
    else {
        let v = phase_eventually_advances(c,b,base,g,t,keys);
        metadata_span(c,b,base,g,t,v);
        eventually_decides(c,b,base,g,v,keys)
    }
}

/// Source and destination certificates are obtained independently. If Finish
/// already ran, reachable-state provenance supplies BOTH receipts instead;
/// correct Raft alone is not assumed to manufacture either certificate.
pub proof fn terminal_certificate(c: p::Constants, b: Execution, base: nat, g: nat,
    t: nat, source: bool) -> (u: nat)
    requires execution(c,b,base), cleanup_fair(c,b,base,g), t >= base,
        b.state(t).plans.contains_key(g), p::terminal(b.state(t).phases[g])
    ensures u >= t, b.state(u).certificates.contains((g,
        if source { p::Certificate::SourceDone } else { p::Certificate::DestinationDone }))
{
    let cert = if source { p::Certificate::SourceDone } else { p::Certificate::DestinationDone };
    if !(exists|u: nat| u >= t && b.state(u).certificates.contains((g,cert))) {
        state_at(c,b,base,t);
        assert(sh::command_shape(b.state(t),g));
        let cmd = terminal_command(b.state(t),g);
        let owner = if source { b.state(t).plans[g].src } else { b.state(t).plans[g].dst };
        assert forall|v: nat| v >= t implies p::current(b.state(v),g) by {
            metadata_span(c,b,base,g,t,v);
            state_at(c,b,base,v);
            if !p::current(b.state(v),g) {
                assert(b.state(v).received.contains((g,cert)));
                assert(b.state(v).certificates.contains((g,cert)));
            }
        }
        let v = deliver_command(c,b,base,g,t,cmd,owner);
        metadata_span(c,b,base,g,t,v);
        state_at(c,b,base,v);
        sh::terminal_shape_supplies_cleanup(c,b.state(v),g);
        assert(b.state(v+1).certificates.contains((g,cert)));
    }
    choose|u: nat| u >= t && b.state(u).certificates.contains((g,cert))
}

pub proof fn terminal_eventually_completes(c: p::Constants, b: Execution, base: nat, g: nat, t: nat)
    -> (u: nat)
    requires execution(c,b,base), cleanup_fair(c,b,base,g), t >= base,
        b.state(t).plans.contains_key(g), p::terminal(b.state(t).phases[g])
    ensures u >= t, completed(b.state(u),g), b.state(u).phases[g] == b.state(t).phases[g]
{
    let src = terminal_certificate(c,b,base,g,t,true);
    metadata_span(c,b,base,g,t,src);
    let dst = terminal_certificate(c,b,base,g,src,false);
    metadata_span(c,b,base,g,src,dst);
    let src_received = receive_certificate(c,b,base,g,dst,p::Certificate::SourceDone);
    metadata_span(c,b,base,g,t,src_received);
    metadata_span(c,b,base,g,dst,src_received);
    let both = receive_certificate(c,b,base,g,src_received,p::Certificate::DestinationDone);
    metadata_span(c,b,base,g,src_received,both);
    metadata_span(c,b,base,g,t,both);
    if !(exists|u: nat| u >= both && !p::current(b.state(u),g)) {
        let finish = p::Action::Finish;
        assert forall|v: nat| v >= both implies primitive(b.state(v),g,finish) && p::enabled(c,b.state(v),finish) by {
            metadata_span(c,b,base,g,both,v);
        }
        let v = fair_call(c,b,base,g,both,finish);
        assert(!p::current(b.state(v+1),g));
    }
    let finished = choose|u: nat| u >= both && !p::current(b.state(u),g);
    metadata_span(c,b,base,g,t,finished);
    let nonce = b.state(t).plans[g].nonce;
    let reply = p::Action::Reply { nonce };
    if !(exists|u: nat| u >= finished && b.state(u).replies.contains_key(nonce)) {
        assert forall|v: nat| v >= finished implies primitive(b.state(v),g,reply) && p::enabled(c,b.state(v),reply) by {
            metadata_span(c,b,base,g,t,v);
            state_at(c,b,base,v);
            assert(p::plan_inv(c,b.state(v),g));
        }
        let v = fair_call(c,b,base,g,finished,reply);
        assert(b.state(v+1).replies.contains_key(nonce));
    }
    let u = choose|u: nat| u >= finished && b.state(u).replies.contains_key(nonce);
    metadata_span(c,b,base,g,t,u);
    metadata_span(c,b,base,g,both,u);
    metadata_span(c,b,base,g,finished,u);
    state_at(c,b,base,u);
    assert(completed(b.state(u),g));
    u
}

/// Main service theorem. Arbitrary selected-range admissions are permitted
/// before physical Freeze; holders may independently commit arbitrary legal
/// writes or abort and release in any order; unrelated work continues forever.
/// Recovery may enter at ANY reachable phase, including committed-but-not-yet
/// activated or aborted-with-either-receipt-missing. No fair-progress-step or
/// preconstructed successful trace appears among the assumptions.
pub proof fn theorem_actual_eventual_completion(c: p::Constants, b: Execution, base: nat,
    g: nat, keys: Seq<int>, recovered_prefix: Seq<p::State>) -> (u: nat)
    requires p::constants_ok(c), p::behavior(c,recovered_prefix),
        recovered_prefix.last() == b.state(base), stable_steps(c,b,base),
        fair(c,b,base,g), engines_settle(b,base,g), finite_admission(b,base,g),
        b.state(base).plans.contains_key(g), finite_image(b.state(base),g,keys)
    ensures u >= base, completed(b.state(u),g)
{
    reachable_progress_shape(c,recovered_prefix);
    assert(execution(c,b,base));
    let decided = eventually_decides(c,b,base,g,base,keys);
    metadata_span(c,b,base,g,base,decided);
    terminal_eventually_completes(c,b,base,g,decided)
}

/// Already-decided recovery requires neither future transaction settlement nor
/// copy fairness to establish its invariant. Its reachable history supplies
/// whole-range application shape; cleanup_ready is a derived lemma, never a
/// restart premise. The decision may be Committed OR Aborted.
pub proof fn theorem_recovered_terminal_eventual_completion(c: p::Constants, b: Execution,
    base: nat, g: nat, recovered_prefix: Seq<p::State>) -> (u: nat)
    requires p::constants_ok(c), p::behavior(c,recovered_prefix),
        recovered_prefix.last() == b.state(base), stable_steps(c,b,base), cleanup_fair(c,b,base,g),
        b.state(base).plans.contains_key(g), p::terminal(b.state(base).phases[g])
    ensures u >= base, completed(b.state(u),g),
        b.state(u).phases[g] == b.state(base).phases[g]
{
    reachable_progress_shape(c,recovered_prefix);
    terminal_eventually_completes(c,b,base,g,base)
}

pub proof fn no_abort_span(c: p::Constants, b: Execution, base: nat, g: nat, t: nat, u: nat)
    requires execution(c,b,base), base <= t <= u, b.state(t).plans.contains_key(g),
        !(b.state(t).phases[g] is Aborted),
        forall|v: nat| v >= t ==> !(p::current(b.state(v),g) && b.action(v) is Abort)
    ensures !(b.state(u).phases[g] is Aborted)
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        no_abort_span(c,b,base,g,t,v);
        metadata_span(c,b,base,g,t,v);
        state_at(c,b,base,v);
        if p::enabled(c,b.state(v),b.action(v)) {
            match b.action(v) {
                p::Action::Begin { .. } => {
                    assert(p::plan_inv(c,b.state(v),g));
                    assert(g < b.state(v).next_generation);
                },
                p::Action::Abort => { assert(!p::current(b.state(v),g)); },
                _ => {},
            }
        }
    }
}

/// Successful commit, rather than the commit-or-abort disjunction, when no
/// administrator/error policy chooses Abort for this migration. This does not
/// constrain the commit/abort outcomes of ordinary application transactions.
pub proof fn theorem_actual_commit_without_abort(c: p::Constants, b: Execution,
    base: nat, g: nat, keys: Seq<int>, recovered_prefix: Seq<p::State>) -> (u: nat)
    requires p::constants_ok(c), p::behavior(c,recovered_prefix),
        recovered_prefix.last() == b.state(base), stable_steps(c,b,base),
        fair(c,b,base,g), engines_settle(b,base,g), finite_admission(b,base,g),
        b.state(base).plans.contains_key(g), finite_image(b.state(base),g,keys),
        !(b.state(base).phases[g] is Aborted),
        forall|v: nat| v >= base ==> !(p::current(b.state(v),g) && b.action(v) is Abort)
    ensures u >= base, completed(b.state(u),g), b.state(u).phases[g] is Committed
{
    let u = theorem_actual_eventual_completion(c,b,base,g,keys,recovered_prefix);
    reachable_progress_shape(c,recovered_prefix);
    no_abort_span(c,b,base,g,base,u);
    u
}

} // verus!
