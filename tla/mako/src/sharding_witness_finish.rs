//! Small reached-state checkpoints for decision, activation, cleanup and reply.
use vstd::prelude::*;
use super::{p,t,Execution,constants,valid,last,grant,step,network};
use super::prefix::{sealed_placement,resolved};

verus! {

pub open spec fn committed_outcome() -> p::Outcome { p::Outcome { generation:1,committed:true } }
pub open spec fn decided() -> p::State {
    let s = sealed_placement();
    p::State { physical:s.physical
        .insert((0int,0int),p::Replica { role:p::Role::Retired,..s.physical[(0int,0int)] })
        .insert((0int,1int),p::Replica { role:p::Role::Retired,..s.physical[(0int,1int)] }),
        phases:s.phases.insert(1nat,p::Phase::Committed),
        commands:s.commands.insert((1nat,p::Command::Retire)).insert((1nat,p::Command::Commit)),
        certificates:s.certificates.insert((1nat,p::Certificate::Retired)),
        received:s.received.insert((1nat,p::Certificate::Retired)),
        directory:s.directory.push(p::directory(s).insert(0,grant(1,1)).insert(1,grant(1,1))),
        outcomes:s.outcomes.insert(100,committed_outcome()),..s }
}
pub proof fn decide(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == sealed_placement()
    ensures valid(z),last(z).placement == decided(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestRetire);
    assert(p::local_guard(last(z).placement,1,p::Command::Retire,0));
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Retire,owner:0 });
    let z = network(z,p::Action::Receive { generation:1,certificate:p::Certificate::Retired });
    let z = network(z,p::Action::Commit);
    assert(last(z).placement.physical =~= decided().physical);
    assert(p::directory(last(z).placement) =~= p::directory(decided()));
    assert(last(z).placement.directory =~= decided().directory);
    assert(last(z).placement.certificates =~= decided().certificates);
    assert(last(z).placement.commands =~= decided().commands);
    assert(last(z).placement.phases =~= decided().phases);
    assert(last(z).placement.received =~= decided().received);
    assert(last(z).placement.outcomes =~= decided().outcomes);
    z
}

pub open spec fn live_reader() -> p::State {
    let s = decided();
    p::State { physical:s.physical
        .insert((0int,0int),p::Replica { cell:p::empty_cell(),role:p::Role::Empty,terminal:true,..s.physical[(0int,0int)] })
        .insert((0int,1int),p::Replica { cell:p::empty_cell(),role:p::Role::Empty,terminal:true,..s.physical[(0int,1int)] })
        .insert((1int,0int),p::Replica { role:p::Role::Serving,terminal:true,..s.physical[(1int,0int)] })
        .insert((1int,1int),p::Replica { role:p::Role::Serving,terminal:true,..s.physical[(1int,1int)] }),
        sessions:s.sessions.insert(3int,p::Session { resolved:false,held:Map::empty().insert(0int,grant(1,1)) }),
        certificates:s.certificates.insert((1nat,p::Certificate::SourceDone)).insert((1nat,p::Certificate::DestinationDone)),..s }
}
pub proof fn activate_with_reader(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == decided(),!last(e).txns.dom().contains(3)
    ensures valid(z),last(z).placement == live_reader(),
        last(z).txns == last(e).txns.insert(3,last(z).txns[3]),
        last(z).txns[3].body == Map::empty().insert(0int,t::Op::Read),
        last(z).txns[3].reads == Map::<int,p::Cell>::empty(),
        last(z).txns[3].outcome is Open,last(z).txns[3].replied.is_none()
{
    hide(t::behavior);
    let z = step(e,t::Action::Begin { txn:3,body:Map::empty().insert(0int,t::Op::Read) });
    assert(!p::admission(last(z).placement,3,0,grant(0,0)));
    assert(!p::admission(last(z).placement,3,0,grant(1,1)));
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Commit,owner:1 });
    let z = network(z,p::Action::Acquire { txn:3,key:0,grant:grant(1,1) });
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Commit,owner:0 });
    assert(last(z).placement.physical =~= live_reader().physical);
    assert(last(z).placement.sessions =~= live_reader().sessions);
    assert(last(z).placement.certificates =~= live_reader().certificates);
    z
}

pub open spec fn settled_reader() -> p::State {
    let s = live_reader();
    p::State { active:None,
        received:s.received.insert((1nat,p::Certificate::SourceDone)).insert((1nat,p::Certificate::DestinationDone)),
        replies:s.replies.insert(100,committed_outcome()),..s }
}
pub proof fn finish_administration(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == live_reader()
    ensures valid(z),last(z).placement == settled_reader(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    assert(!p::enabled(constants(),last(e).placement,p::Action::Finish));
    let z = network(e,p::Action::Receive { generation:1,certificate:p::Certificate::SourceDone });
    assert(!p::enabled(constants(),last(z).placement,p::Action::Finish));
    let z = network(z,p::Action::Receive { generation:1,certificate:p::Certificate::DestinationDone });
    let z = network(z,p::Action::Finish);
    // The reverse request would otherwise have a valid source and free job slot.
    assert(!p::enabled(constants(),last(z).placement,
        p::Action::Begin { nonce:100,src:1,dst:0,table:0,lo:0,hi:Some(2int) }));
    let z = network(z,p::Action::Reply { nonce:100 });
    let z = network(z,p::Action::Reply { nonce:100 });
    assert(last(z).placement.received =~= settled_reader().received);
    assert(last(z).placement.replies =~= settled_reader().replies);
    z
}

pub open spec fn completed() -> p::State {
    let s = settled_reader();
    p::State { sessions:s.sessions.insert(3int,resolved()),..s }
}
pub proof fn finish_read(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == settled_reader(),
        last(e).txns[3].body == Map::empty().insert(0int,t::Op::Read),
        last(e).txns.dom().contains(3),last(e).txns[3].reads == Map::<int,p::Cell>::empty(),
        last(e).txns[3].outcome is Open,last(e).txns[3].replied.is_none()
    ensures valid(z),last(z).placement == completed(),
        last(z).txns == last(e).txns.insert(3,last(z).txns[3]),
        last(z).txns[3].reads[0].value == Some(15int),last(z).txns[3].replied.is_some()
{
    hide(t::behavior);
    let z = step(e,t::Action::Fetch { txn:3,key:0 });
    assert(last(z).txns[3].reads[0].value == Some(15int));
    assert(t::read_keys(last(z).txns[3].body) =~= Set::empty().insert(0int));
    assert(t::writes(last(z).txns[3].body,last(z).txns[3].reads) =~= Map::<int,Option<int>>::empty());
    let z = step(z,t::Action::Commit { txn:3 });
    let z = step(z,t::Action::Reply { txn:3 });
    let z = network(z,p::Action::Release { txn:3,key:0 });
    assert(last(z).placement.logical =~= completed().logical);
    assert(last(z).placement.physical =~= completed().physical);
    assert(last(z).placement.sessions[3].held =~= Map::<int,p::Grant>::empty());
    assert(last(z).placement.sessions =~= completed().sessions);
    z
}

} // verus!
