//! Concrete infinite witness for the service theorem's actual-call fairness.
//! The terminal suffix continues legal obsolete retries indefinitely. These
//! retries are permitted but no longer required once the migration is inactive.
use super::*;

verus! {

pub open spec fn constants() -> p::Constants {
    p::Constants {
        keys: set![0int], shards: set![0int,1int],
        table: map![0int => 0int], coordinate: map![0int => 0int], owners: map![0int => 0int],
    }
}
pub open spec fn begin_action() -> p::Action {
    p::Action::Begin { nonce: 0, src: 0, dst: 1, table: 0, lo: 0, hi: None }
}
pub open spec fn started() -> p::State {
    p::dispatch(constants(),p::initial(constants()),begin_action())
}
pub open spec fn prefix_actions() -> Seq<p::Action> {
    seq![p::Action::Abort,
        p::Action::Deliver { generation: 1, command: p::Command::Abort, owner: 0 },
        p::Action::Deliver { generation: 1, command: p::Command::Abort, owner: 1 },
        p::Action::Receive { generation: 1, certificate: p::Certificate::SourceDone },
        p::Action::Receive { generation: 1, certificate: p::Certificate::DestinationDone },
        p::Action::Finish, p::Action::Reply { nonce: 0 }]
}
pub open spec fn tail_actions() -> Seq<p::Action> {
    seq![p::Action::Deliver { generation: 1, command: p::Command::Start, owner: 0 },
        p::Action::Deliver { generation: 1, command: p::Command::Start, owner: 1 },
        p::Action::Deliver { generation: 1, command: p::Command::Abort, owner: 0 },
        p::Action::Deliver { generation: 1, command: p::Command::Abort, owner: 1 },
        p::Action::Receive { generation: 1, certificate: p::Certificate::SourceDone },
        p::Action::Receive { generation: 1, certificate: p::Certificate::DestinationDone },
        p::Action::Reply { nonce: 0 }]
}
pub open spec fn prefix_state(i: nat) -> p::State
    decreases i
{
    if i == 0 { started() }
    else { p::dispatch(constants(),prefix_state((i-1) as nat),prefix_actions()[(i-1) as int]) }
}
pub open spec fn example() -> Execution {
    Execution {
        state: |t: nat| if t <= 7 { prefix_state(t) } else { prefix_state(7) },
        action: |t: nat| if t < 7 { prefix_actions()[t as int] }
            else { tail_actions()[((t-7)%7) as int] },
    }
}
pub open spec fn recovered_prefix() -> Seq<p::State> {
    seq![p::initial(constants()),started()]
}

pub proof fn started_facts()
    ensures p::constants_ok(constants()), p::behavior(constants(),recovered_prefix()),
        sh::progress_inv(constants(),started()), started().plans.contains_key(1),
        started().plans[1].keys == set![0int], started().sessions == Map::<int,p::Session>::empty(),
        p::current(started(),1),started().phases[1] is Copy,
        started().plans[1].src == 0,started().plans[1].dst == 1,started().plans[1].nonce == 0,
        started().received == Set::<(nat,p::Certificate)>::empty()
{
    let c = constants();
    assert(p::constants_ok(c));
    sh::progress_initial(c);
    assert(p::enabled(c,p::initial(c),begin_action()));
    sh::progress_step(c,p::initial(c),begin_action());
    assert(p::next(c,p::initial(c),started()));
}

pub proof fn prefix_facts(i: nat)
    requires i <= 7
    ensures sh::progress_inv(constants(),prefix_state(i)),
        prefix_state(i).sessions == Map::<int,p::Session>::empty(),
        prefix_state(i).plans.contains_key(1), prefix_state(i).plans[1] == started().plans[1]
    decreases i
{
    if i == 0 { started_facts(); }
    else {
        let j = (i-1) as nat;
        prefix_facts(j);
        sh::progress_step(constants(),prefix_state(j),prefix_actions()[j as int]);
        sh::metadata_monotone(constants(),prefix_state(j),prefix_actions()[j as int],1);
        assert(!(prefix_actions()[j as int] is Open || prefix_actions()[j as int] is Acquire
            || prefix_actions()[j as int] is Resolve || prefix_actions()[j as int] is Release));
    }
}

pub open spec fn prefix_metadata(s:p::State,i:nat)->bool {
    &&& s.active == if i <= 5 { Some(1nat) } else { None }
    &&& s.phases[1] == if i == 0 { p::Phase::Copy } else { p::Phase::Aborted }
    &&& s.commands == if i == 0 { set![(1nat,p::Command::Start)] }
        else { set![(1nat,p::Command::Start),(1nat,p::Command::Abort)] }
    &&& s.certificates == if i < 2 { Set::empty() } else if i < 3 { set![(1nat,p::Certificate::SourceDone)] }
        else { set![(1nat,p::Certificate::SourceDone),(1nat,p::Certificate::DestinationDone)] }
    &&& s.received == if i < 4 { Set::empty() } else if i < 5 { set![(1nat,p::Certificate::SourceDone)] }
        else { set![(1nat,p::Certificate::SourceDone),(1nat,p::Certificate::DestinationDone)] }
    &&& s.outcomes == if i == 0 { Map::empty() } else { map![0int => p::Outcome { generation:1,committed:false }] }
    &&& s.replies == if i < 7 { Map::empty() } else { map![0int => p::Outcome { generation:1,committed:false }] }
    &&& s.packets == Set::<p::Packet>::empty()
}

pub proof fn prefix_metadata_facts(i:nat)
    requires i <= 7
    ensures prefix_metadata(prefix_state(i),i)
    decreases i
{
    started_facts(); prefix_facts(i);
    if i > 0 {
        let j = (i-1) as nat;
        prefix_metadata_facts(j); prefix_facts(j);
        if i == 2 || i == 3 {
            sh::terminal_shape_supplies_cleanup(constants(),prefix_state(j),1);
            assert(p::local_guard(prefix_state(j),1,p::Command::Abort,if i == 2 { 0 } else { 1 }));
        }
        assert(p::enabled(constants(),prefix_state(j),prefix_actions()[j as int]));
        reveal(prefix_state);
    }
}

pub proof fn prefix_timing(i:nat)
    requires i <= 7
    ensures i <= 5 ==> p::current(prefix_state(i),1),
        i >= 1 ==> prefix_state(i).phases[1] is Aborted,
        i >= 2 ==> prefix_state(i).certificates.contains((1nat,p::Certificate::SourceDone)),
        i >= 3 ==> prefix_state(i).certificates.contains((1nat,p::Certificate::DestinationDone))
{
    prefix_metadata_facts(i);
}

pub proof fn singleton_image(s:p::State)
    requires s.plans.contains_key(1),s.plans[1].keys == set![0int]
    ensures finite_image(s,1,seq![0int])
{
    assert forall|k:int| s.plans[1].keys.contains(k) <==> seq![0int].contains(k) by {
        if seq![0int].contains(k) { assert(k == 0); }
        if k == 0 {
            assert(seq![0int][0] == k);
            assert(seq![0int].contains(k));
        }
    }
}

pub open spec fn closed(s: p::State) -> bool {
    &&& sh::progress_inv(constants(),s)
    &&& completed(s,1)
    &&& s.active is None
    &&& s.plans[1].src == 0 && s.plans[1].dst == 1 && s.plans[1].nonce == 0
    &&& s.plans[1].keys == set![0int]
    &&& s.phases[1] is Aborted
    &&& s.commands == set![(1nat,p::Command::Start),(1nat,p::Command::Abort)]
    &&& s.certificates == set![(1nat,p::Certificate::SourceDone),(1nat,p::Certificate::DestinationDone)]
    &&& s.received == s.certificates
    &&& s.packets == Set::<p::Packet>::empty()
    &&& s.sessions == Map::<int,p::Session>::empty()
}

pub proof fn terminal_facts()
    ensures closed(prefix_state(7))
{
    started_facts(); prefix_facts(7); prefix_metadata_facts(7);
}

pub proof fn tail_stutters(s: p::State, i: int)
    requires closed(s), 0 <= i < 7
    ensures p::dispatch(constants(),s,tail_actions()[i]) == s
{
    let a = tail_actions()[i];
    match a {
        p::Action::Deliver { generation,command,owner } => {
            p::lemma_obsolete_control_rejected(constants(),s,generation,command,owner);
        },
        p::Action::Receive { generation,certificate } => {
            assert(s.received.insert((generation,certificate)) =~= s.received);
        },
        p::Action::Reply { nonce } => {
            assert(s.replies.insert(nonce,s.outcomes[nonce]) =~= s.replies);
        },
        _ => {},
    }
}

pub proof fn enabled_tail_index(s: p::State, a: p::Action) -> (i: int)
    requires closed(s), primitive(s,1,a), p::enabled(constants(),s,a)
    ensures 0 <= i < 7, tail_actions()[i] == a
{
    match a {
        p::Action::Deliver { generation,command,owner } => {
            if command is Start { if owner == 0 { 0 } else { 1 } }
            else { if owner == 0 { 2 } else { 3 } }
        },
        p::Action::Receive { generation,certificate } => {
            if certificate is SourceDone { 4 } else { 5 }
        },
        p::Action::Reply { nonce } => { 6 },
        p::Action::Drain { generation } | p::Action::Seal { generation } => {
            sh::old_local_work_disabled(constants(),s,1); 0
        },
        p::Action::Capture { generation,round,key } => {
            assert(key == 0 && round == 1);
            assert(p::plan_inv(constants(),s,1));
            assert(!p::source_frozen(s,1));
            0
        },
        _ => { 0 },
    }
}

pub proof fn actual_infinite_witness()
    ensures p::constants_ok(constants()), p::behavior(constants(),recovered_prefix()),
        recovered_prefix().last() == example().state(0),
        stable_steps(constants(),example(),0), fair(constants(),example(),0,1),
        engines_settle(example(),0,1), finite_admission(example(),0,1),
        example().state(0).plans.contains_key(1), finite_image(example().state(0),1,seq![0int]),
        completed(example().state(7),1)
{
    started_facts();
    terminal_facts();
    let b = example();
    singleton_image(b.state(0));
    assert forall|t: nat| t >= 0 implies b.state(t+1) == p::dispatch(constants(),b.state(t),b.action(t)) by {
        if t < 7 { reveal(prefix_state); }
        else { tail_stutters(prefix_state(7),((t-7)%7) as int); }
    }
    assert forall|a: p::Action| call_fair(constants(),b,0,1,a) by {
        assert forall|t: nat| t >= 0 && #[trigger] continuously_needed(constants(),b,1,a,t) implies
            exists|u: nat| u >= t && b.action(u) == a by {
            let v = if t < 7 { 7nat } else { t };
            assert(b.state(v) == prefix_state(7));
            let i = enabled_tail_index(prefix_state(7),a);
            let u = (7 + 7*(t+1) + i) as nat;
            assert(u >= t && u >= 7);
            assert((u-7)%7 == i);
            assert(b.action(u) == a);
        }
    }
    assert forall|txn: int, t: nat| t >= 0 && selected_holders(b.state(t),1).contains(txn) implies
        exists|u: nat| u >= t && b.state(u).sessions.contains_key(txn) && b.state(u).sessions[txn].resolved by {
        if t <= 7 { prefix_facts(t); }
        else { prefix_facts(7); }
        assert(!b.state(t).sessions.contains_key(txn));
    }
    assert forall|t: nat| t >= 0 && sh::source_frozen(b.state(t),1) implies
        exists|holds: Seq<(int,int)>| exact_holds(b.state(t),1,holds) by {
        if t <= 7 { prefix_facts(t); }
        else { prefix_facts(7); }
        assert(exact_holds(b.state(t),1,Seq::empty()));
    }
}

} // verus!
