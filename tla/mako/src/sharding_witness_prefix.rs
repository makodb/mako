//! Small proof checkpoints for the constructive handoff execution. These are
//! reached states, not initial-state assumptions or guards of the protocol.
use vstd::prelude::*;
use super::{p,t,Execution,constants,valid,last,grant,start,step,network};

verus! {

pub open spec fn seeded_cells() -> Map<int,p::Cell> {
    Map::empty().insert(0int,p::Cell { value:Some(10int),writer:0 })
        .insert(1int,p::Cell { value:Some(11int),writer:0 })
        .insert(2int,p::Cell { value:Some(20int),writer:0 })
}
pub open spec fn resolved() -> p::Session { p::Session { held:Map::empty(),resolved:true } }
pub open spec fn seeded_placement() -> p::State {
    let c = constants(); let s = p::initial(c);
    p::State { logical:seeded_cells(),sessions:Map::empty().insert(0int,resolved()),
        physical:IMap::new(|q:(int,int)| s.physical.contains_key(q), |q:(int,int)|
            if c.owners[q.1] == q.0 { p::Replica { cell:seeded_cells()[q.1],..s.physical[q] } }
            else { s.physical[q] }), ..s }
}
pub proof fn seed_execution() -> (e:Execution)
    ensures valid(e),last(e).placement == seeded_placement(),
        last(e).txns.dom() == Set::empty().insert(0int)
{
    hide(t::behavior);
    let e = start();
    let seed = Map::empty().insert(0int,t::Op::Put { value:10 })
        .insert(1int,t::Op::Put { value:11 }).insert(2int,t::Op::Put { value:20 });
    let e = step(e,t::Action::Begin { txn:0,body:seed });
    let e = network(e,p::Action::Acquire { txn:0,key:0,grant:grant(0,0) });
    let e = network(e,p::Action::Acquire { txn:0,key:1,grant:grant(0,0) });
    let e = network(e,p::Action::Acquire { txn:0,key:2,grant:grant(1,0) });
    assert(t::read_keys(seed) =~= Set::<int>::empty());
    assert(last(e).placement.sessions[0].held.dom() =~= seed.dom());
    let e = step(e,t::Action::Commit { txn:0 });
    let e = step(e,t::Action::Reply { txn:0 });
    let e = network(e,p::Action::Release { txn:0,key:0 });
    let e = network(e,p::Action::Release { txn:0,key:1 });
    let e = network(e,p::Action::Release { txn:0,key:2 });
    assert(last(e).placement.logical =~= seeded_cells());
    assert(last(e).placement.physical =~= seeded_placement().physical);
    assert(last(e).placement.sessions[0].held =~= Map::<int,p::Grant>::empty());
    assert(last(e).placement.sessions =~= seeded_placement().sessions);
    e
}

pub open spec fn background_packet() -> p::Packet {
    p::Packet { generation:1,round:0,key:0,cell:p::Cell { value:Some(10int),writer:0 } }
}
pub open spec fn prepared_base() -> p::State {
    let s = seeded_placement();
    let stage = p::Replica { cell:p::empty_cell(),role:p::Role::Stage,epoch:1,
        fence:1,terminal:false,round:0,covered:false };
    p::State { active:Some(1nat),next_generation:2,
        plans:Map::empty().insert(1nat,p::Plan { nonce:100,src:0,dst:1,table:0,lo:0,hi:Some(2int),
            keys:Set::empty().insert(0int).insert(1int),old:p::directory(s) }),
        phases:Map::empty().insert(1nat,p::Phase::Copy),
        commands:Set::empty().insert((1nat,p::Command::Start)),
        packets:Set::empty().insert(background_packet()),
        physical:s.physical.insert((1int,0int),stage).insert((1int,1int),stage),..s }
}
pub proof fn prepare_execution(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == seeded_placement()
    ensures valid(z),last(z).placement == prepared_base(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    assert(p::selected(constants(),0,0,Some(2int)) =~= Set::empty().insert(0int).insert(1int));
    let z = network(e,p::Action::Begin { nonce:100,src:0,dst:1,table:0,lo:0,hi:Some(2int) });
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Start,owner:1 });
    let z = network(z,p::Action::Capture { generation:1,round:0,key:0 });
    assert(last(z).placement.physical =~= prepared_base().physical);
    assert(last(z).placement.plans[1].keys =~= Set::empty().insert(0int).insert(1int));
    assert(last(z).placement.plans =~= prepared_base().plans);
    z
}
pub open spec fn frozen_base() -> p::State {
    let s = prepared_base();
    p::State { phases:s.phases.insert(1nat,p::Phase::Freezing),
        commands:s.commands.insert((1nat,p::Command::Freeze)),
        physical:s.physical
            .insert((0int,0int),p::Replica { role:p::Role::Frozen,fence:1,terminal:false,..s.physical[(0int,0int)] })
            .insert((0int,1int),p::Replica { role:p::Role::Frozen,fence:1,terminal:false,..s.physical[(0int,1int)] }),..s }
}
pub open spec fn add_body() -> Map<int,t::Op> {
    Map::empty().insert(0int,t::Op::Add { delta:5 }).insert(2int,t::Op::Read)
}
pub open spec fn writer_placement() -> p::State {
    let s = prepared_base();
    p::State { sessions:s.sessions.insert(1int,p::Session { resolved:false,
        held:Map::empty().insert(0int,grant(0,0)).insert(2int,grant(1,0)) }),..s }
}
pub proof fn writer_execution(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == prepared_base(),
        last(e).txns.dom() == Set::empty().insert(0int)
    ensures valid(z),last(z).placement == writer_placement(),
        last(z).txns.dom() == Set::empty().insert(0int).insert(1int),
        last(z).txns[1].body == add_body(),last(z).txns[1].outcome is Open,
        last(z).txns[1].replied.is_none(),
        last(z).txns[1].reads == Map::empty().insert(0int,seeded_cells()[0]).insert(2int,seeded_cells()[2])
{
    hide(t::behavior);
    let z = step(e,t::Action::Begin { txn:1,body:add_body() });
    let z = network(z,p::Action::Acquire { txn:1,key:0,grant:grant(0,0) });
    let z = network(z,p::Action::Acquire { txn:1,key:2,grant:grant(1,0) });
    let z = step(z,t::Action::Fetch { txn:1,key:0 });
    let z = step(z,t::Action::Fetch { txn:1,key:2 });
    assert(last(z).placement.sessions =~= writer_placement().sessions);
    assert(last(z).txns[1].reads =~= Map::empty().insert(0int,seeded_cells()[0]).insert(2int,seeded_cells()[2]));
    z
}
pub open spec fn admitted_placement() -> p::State {
    p::State { sessions:Map::empty().insert(0int,resolved())
        .insert(1int,p::Session { resolved:false,
            held:Map::empty().insert(0int,grant(0,0)).insert(2int,grant(1,0)) })
        .insert(2int,p::Session { resolved:false,held:Map::empty().insert(1int,grant(0,0)) }),
        ..frozen_base() }
}
pub proof fn admitted_execution() -> (e:Execution)
    ensures valid(e),last(e).placement == admitted_placement(),
        last(e).txns.dom() == Set::empty().insert(0int).insert(1int).insert(2int),
        last(e).txns[1].body == add_body(),last(e).txns[1].outcome is Open,
        last(e).txns[1].replied.is_none(),
        last(e).txns[1].reads == Map::empty().insert(0int,seeded_cells()[0]).insert(2int,seeded_cells()[2]),
        last(e).txns[2].body == Map::empty().insert(1int,t::Op::Read),
        last(e).txns[2].outcome is Open,last(e).txns[2].reads == Map::<int,p::Cell>::empty(),
        last(e).txns[2].replied.is_none()
{
    hide(t::behavior);
    let e = seed_execution();
    let e = prepare_execution(e);
    let e = writer_execution(e);
    let e = step(e,t::Action::Begin { txn:2,body:Map::empty().insert(1int,t::Op::Read) });
    let e = network(e,p::Action::Acquire { txn:2,key:1,grant:grant(0,0) });
    let e = network(e,p::Action::RequestFreeze);
    assert(p::local_guard(last(e).placement,1,p::Command::Freeze,0));
    let e = network(e,p::Action::Deliver { generation:1,command:p::Command::Freeze,owner:0 });
    assert(last(e).placement.sessions[2].held.contains_key(1));
    assert(last(e).placement.plans[1].keys.contains(1));
    assert(!p::enabled(constants(),last(e).placement,p::Action::Drain { generation:1 }));
    assert(last(e).placement.physical =~= admitted_placement().physical);
    assert(last(e).placement.plans[1].keys =~= Set::empty().insert(0int).insert(1int));
    assert(last(e).placement.plans =~= admitted_placement().plans);
    assert(last(e).placement.sessions =~= admitted_placement().sessions);
    assert(last(e).txns[1].reads =~= Map::empty().insert(0int,seeded_cells()[0]).insert(2int,seeded_cells()[2]));
    e
}
pub open spec fn closed_placement() -> p::State {
    let s = frozen_base();
    let written = p::Cell { value:Some(15int),writer:1 };
    p::State { logical:seeded_cells().insert(0,written),
        physical:s.physical.insert((0,0),p::Replica { cell:written,..s.physical[(0,0)] }),
        sessions:Map::empty().insert(0int,resolved()).insert(1int,resolved()).insert(2int,resolved()),..s }
}
pub proof fn closed_execution() -> (e:Execution)
    ensures valid(e),last(e).placement == closed_placement(),
        last(e).txns.dom() == Set::empty().insert(0int).insert(1int).insert(2int),
        last(e).txns[1].reads[0].value == Some(10int),
        last(e).txns[1].reads[2].value == Some(20int),last(e).txns[1].replied.is_some(),
        last(e).txns[2].reads[1].value == Some(11int),last(e).txns[2].replied.is_some()
{
    hide(t::behavior);
    let e = admitted_execution();
    let body = add_body();
    assert(t::read_keys(body) =~= body.dom());
    let e = step(e,t::Action::Commit { txn:1 });
    let e = step(e,t::Action::Reply { txn:1 });
    let e = network(e,p::Action::Release { txn:1,key:0 });
    let e = network(e,p::Action::Release { txn:1,key:2 });
    assert(last(e).placement.sessions[2].held.contains_key(1));
    assert(last(e).placement.plans[1].keys.contains(1));
    assert(!p::enabled(constants(),last(e).placement,p::Action::Drain { generation:1 }));
    let e = step(e,t::Action::Fetch { txn:2,key:1 });
    assert(t::read_keys(last(e).txns[2].body) =~= Set::empty().insert(1int));
    assert(t::writes(last(e).txns[2].body,last(e).txns[2].reads) =~= Map::<int,Option<int>>::empty());
    let e = step(e,t::Action::Commit { txn:2 });
    let e = step(e,t::Action::Reply { txn:2 });
    let e = network(e,p::Action::Release { txn:2,key:1 });
    assert(last(e).placement.logical =~= closed_placement().logical);
    assert(last(e).placement.physical =~= closed_placement().physical);
    assert(last(e).placement.sessions[0].held =~= Map::<int,p::Grant>::empty());
    assert(last(e).placement.sessions[1].held =~= Map::<int,p::Grant>::empty());
    assert(last(e).placement.sessions[2].held =~= Map::<int,p::Grant>::empty());
    assert(last(e).placement.sessions =~= closed_placement().sessions);
    e
}

pub open spec fn final_packet(key:int) -> p::Packet {
    p::Packet { generation:1,round:1,key,cell:closed_placement().logical[key] }
}
pub open spec fn round_placement() -> p::State {
    let s = closed_placement();
    p::State { physical:s.physical
        .insert((1int,0int),p::Replica { round:1,covered:false,..s.physical[(1int,0int)] })
        .insert((1int,1int),p::Replica { round:1,covered:false,..s.physical[(1int,1int)] }),
        phases:s.phases.insert(1nat,p::Phase::Final),commands:s.commands.insert((1nat,p::Command::Final)),
        certificates:s.certificates.insert((1nat,p::Certificate::Drained)),
        received:s.received.insert((1nat,p::Certificate::Drained)),..s }
}
pub proof fn round_execution(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == closed_placement()
    ensures valid(z),last(z).placement == round_placement(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::Drain { generation:1 });
    let z = network(z,p::Action::Receive { generation:1,certificate:p::Certificate::Drained });
    let z = network(z,p::Action::RequestFinal);
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Final,owner:1 });
    assert(last(z).placement.physical =~= round_placement().physical);
    z
}
pub open spec fn copied_placement() -> p::State {
    let s = round_placement();
    p::State { physical:s.physical
        .insert((1int,0int),p::Replica { cell:s.logical[0],covered:true,..s.physical[(1int,0int)] })
        .insert((1int,1int),p::Replica { cell:s.logical[1],covered:true,..s.physical[(1int,1int)] }),
        packets:s.packets.insert(final_packet(0)).insert(final_packet(1)),..s }
}
pub proof fn copied_execution(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == round_placement()
    ensures valid(z),last(z).placement == copied_placement(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::Capture { generation:1,round:1,key:0 });
    let z = network(z,p::Action::DeliverCopy { packet:final_packet(0) });
    let z = network(z,p::Action::Capture { generation:1,round:1,key:1 });
    let z = network(z,p::Action::DeliverCopy { packet:final_packet(1) });
    assert(last(z).placement.physical =~= copied_placement().physical);
    z
}
pub open spec fn sealed_placement() -> p::State {
    let s = closed_placement();
    p::State {
        physical:s.physical
            .insert((1int,0int),p::Replica { cell:s.logical[0],role:p::Role::Ready,round:1,covered:true,..s.physical[(1int,0int)] })
            .insert((1int,1int),p::Replica { cell:s.logical[1],role:p::Role::Ready,round:1,covered:true,..s.physical[(1int,1int)] }),
        phases:s.phases.insert(1,p::Phase::Final),
        commands:s.commands.insert((1,p::Command::Final)),
        certificates:s.certificates.insert((1,p::Certificate::Drained)).insert((1,p::Certificate::Ready)),
        received:s.received.insert((1,p::Certificate::Drained)).insert((1,p::Certificate::Ready)),
        packets:s.packets.insert(final_packet(0)).insert(final_packet(1)),..s }
}
pub proof fn sealed_execution() -> (e:Execution)
    ensures valid(e),last(e).placement == sealed_placement(),
        last(e).txns.dom() == Set::empty().insert(0int).insert(1int).insert(2int),
        last(e).txns[1].reads[0].value == Some(10int),
        last(e).txns[1].reads[2].value == Some(20int),last(e).txns[1].replied.is_some(),
        last(e).txns[2].reads[1].value == Some(11int),last(e).txns[2].replied.is_some()
{
    hide(t::behavior);
    let e = closed_execution();
    let e = round_execution(e);
    let e = copied_execution(e);
    let before = last(e).placement;
    let e = network(e,p::Action::DeliverCopy { packet:background_packet() });
    assert(last(e).placement == before);
    let e = network(e,p::Action::Seal { generation:1 });
    let e = network(e,p::Action::Receive { generation:1,certificate:p::Certificate::Ready });
    assert(last(e).placement.physical =~= sealed_placement().physical);
    e
}

} // verus!
