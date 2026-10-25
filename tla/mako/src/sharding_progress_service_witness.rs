//! A concrete service execution: metadata, real finite cleanup traversal,
//! actual emission, then receipt. Eight microsteps per placement call leave
//! room for raw scan/loop-exit/seal/emission; certificates are not wire records.
use super::*;
use super::a::witness as w;

verus! {

pub open spec fn placement() -> a::Execution {
    a::Execution {
        state:|t:nat| w::example().state((t/8) as nat),
        action:|t:nat| if t%8 == 7 { w::example().action((t/8) as nat) } else { p::Action::Stutter },
    }
}
pub open spec fn source_record() -> Emission {
    Emission { generation:1,owner:0,certificate:p::Certificate::SourceDone }
}
pub open spec fn destination_record() -> Emission {
    Emission { generation:1,owner:1,certificate:p::Certificate::DestinationDone }
}
pub open spec fn network() -> Network {
    Network { records:|t:nat|
        (if t >= 18 { set![source_record()] } else { Set::<Emission>::empty() })
            .union(if t >= 28 { set![destination_record()] } else { Set::<Emission>::empty() }) }
}
pub open spec fn raw_start(cleanup:bool) -> io::Driver {
    io::initial(if cleanup { io::Kind::Cleanup } else { io::Kind::Local },Map::empty(),Map::empty(),0)
}
pub open spec fn raw_run(cleanup:bool,start:nat) -> io::Execution {
    io::Execution {
        states:|t:nat| io::iterate(raw_start(cleanup),if t >= start { (t-start) as nat } else { 0 }),
        ticks:|t:nat| io::Tick::Ok,
    }
}
pub open spec fn physical_world() -> World<int> {
    World {
        images:|_t:nat,_owner:int| Map::empty(),
        windows:|_g:nat| physical::Window { contains:|k:int| k == 0,logical:map![0int => 0int] },
        jobs:|id:nat| if id == 0 {
            physical::Job { id:0,generation:1,owner:0,origin:16,labels:Seq::empty(),run:raw_run(false,16) }
        } else {
            physical::Job { id,generation:1,owner:1,origin:24,labels:Seq::empty(),run:raw_run(true,24) }
        },
        issued:|t:nat| if t == 16 { set![0nat] } else if t == 24 { set![1nat] } else { Set::empty() },
    }
}
pub proof fn raw_runs(cleanup:bool,start:nat)
    ensures io::runs(raw_run(cleanup,start),start),io::primitive_service(raw_run(cleanup,start),start),
        io::stable_raw_io(raw_run(cleanup,start),start),
        raw_run(cleanup,start).state(start) == raw_start(cleanup)
{
    io::initial_wf(if cleanup { io::Kind::Cleanup } else { io::Kind::Local },Map::empty(),Map::empty(),0);
    assert forall|t:nat| t >= start implies raw_run(cleanup,start).state(t+1)
        == io::step(raw_run(cleanup,start).state(t),raw_run(cleanup,start).tick(t)) by {
        reveal(io::iterate);
    }
    assert forall|r:io::Request,t:nat| t >= start
        && #[trigger] io::pending_forever(raw_run(cleanup,start),r,t)
        implies exists|u:nat| u >= t && raw_run(cleanup,start).tick(u) is Ok by {
        assert(raw_run(cleanup,start).tick(t) is Ok);
    }
}
pub proof fn done_persists(x:io::Execution,base:nat,t:nat,u:nat)
    requires io::runs(x,base),base <= t <= u,x.state(t).stage is Done
    ensures x.state(u) == x.state(t)
    decreases u-t
{
    if u > t {
        let v = (u-1) as nat;
        done_persists(x,base,t,v); io::state_wf(x,base,v); io::step_wf(x.state(v),x.tick(v));
    }
}
pub proof fn raw_done(cleanup:bool,start:nat,t:nat)
    requires t >= start + if cleanup { 4nat } else { 2nat }
    ensures raw_run(cleanup,start).state(t).stage is Done
{
    raw_runs(cleanup,start);
    let end = start + if cleanup { 4nat } else { 2nat };
    reveal_with_fuel(io::iterate,5);
    assert(raw_run(cleanup,start).state(end).stage is Done);
    done_persists(raw_run(cleanup,start),start,end,t);
}

pub proof fn placement_facts()
    ensures p::constants_ok(w::constants()),p::behavior(w::constants(),w::recovered_prefix()),
        w::recovered_prefix().last() == placement().state(0),
        a::execution(w::constants(),placement(),0)
{
    w::actual_infinite_witness(); w::started_facts();
    assert forall|t:nat| t >= 0 implies placement().state(t+1)
        == p::dispatch(w::constants(),placement().state(t),placement().action(t)) by {
        if t%8 == 7 {
            assert((t+1)/8 == t/8+1);
        } else { assert((t+1)/8 == t/8); }
    }
}
pub proof fn point_facts(t:nat)
    ensures placement().state(t).plans.contains_key(1),
        placement().state(t).plans[1].src == 0,placement().state(t).plans[1].dst == 1,
        placement().state(t).plans[1].nonce == 0,
        placement().state(t).plans[1].keys == set![0int],
        placement().state(t).sessions == Map::<int,p::Session>::empty(),
        t >= 16 ==> placement().state(t).certificates.contains((1nat,p::Certificate::SourceDone)),
        t >= 24 ==> placement().state(t).certificates.contains((1nat,p::Certificate::DestinationDone)),
        t >= 8 ==> placement().state(t).phases[1] is Aborted,
        t >= 56 ==> w::closed(placement().state(t))
{
    w::started_facts();
    let q = if t/8 <= 7 { (t/8) as nat } else { 7nat };
    w::prefix_facts(q); w::prefix_timing(q);
    assert(placement().state(t) == w::prefix_state(q));
    if t >= 56 { w::terminal_facts(); }
}
pub proof fn job_facts(id:nat)
    requires id == 0 || id == 1
    ensures physical::job_bound(placement(),physical_world(),1,id)
{
    let j = physical_world().job(id);
    point_facts(j.origin); raw_runs(id == 1,j.origin);
    assert(physical::encode(Map::<int,Seq<u8>>::empty(),Seq::<int>::empty()) =~= Map::<nat,Seq<u8>>::empty());
    assert forall|t:nat| #[trigger] physical::until_done(j,t) implies {
        &&& physical::logical_window(placement(),physical_world(),1,t)
        &&& j.run.state(t).image == physical::encode(physical_world().image(t,j.owner),j.labels)
        &&& physical::outside(physical_world().image(t,j.owner),physical_world().image(j.origin,j.owner),physical_world().window(1))
        &&& if j.run.state(j.origin).kind is Local { physical_world().image(t,j.owner) == physical_world().image(j.origin,j.owner) }
            else { physical::inventory(physical_world().image(t,j.owner),physical_world().window(1),j.labels) }
        &&& !(j.run.state(j.origin).kind is Mirror)
    } by {
        point_facts(t); io::state_wf(j.run,j.origin,t);
        assert(j.run.state(t).image =~= Map::<nat,Seq<u8>>::empty());
    }
}

pub proof fn no_needed_calls(t:nat,action:p::Action)
    requires t >= 56
    ensures !(a::primitive(placement().state(t),1,action) && p::enabled(w::constants(),placement().state(t),action))
{
    point_facts(t);
    match action {
        p::Action::Release { .. } => {},
        p::Action::Reply { .. } => {},
        _ => {},
    }
}

pub proof fn witness_scheduling()
    ensures submitted_work(w::constants(),placement(),physical_world(),0,1),
        submitted_receipts(w::constants(),placement(),physical_world(),network(),0,1),
        wire_fair(w::constants(),placement(),network(),0,1)
{
    let b = placement(); let net = network(); let c = w::constants();
    let world = physical_world();
    assert forall|action:p::Action,t:nat| t >= 0 && !(action is Receive)
        && #[trigger] a::continuously_needed(c,b,1,action,t)
        implies if action is Seal {
            exists|id:nat,start:nat,stable:nat| start >= t
                && seal_binding(b,world,1,id,start) && io::primitive_service(world.job(id).run,start) && io::stable_raw_io(world.job(id).run,stable)
        } else {
            exists|x:Atomic,start:nat,stable:nat| start >= t
                && atomic_binding(b,world,1,action,x,start) && atomic_returns(x,start) && atomic_stable(x,stable)
        } by {
        let end = if t < 56 { 56nat } else { t };
        no_needed_calls(end,action);
    }
    assert forall|t:nat,cert:p::Certificate| t >= 0
        && #[trigger] a::continuously_needed(c,b,1,p::Action::Receive { generation:1,certificate:cert },t)
        && !net.emitted(t).contains(emission(b.state(t),1,cert)) implies
        exists|id:nat,start:nat,stable:nat| start >= t
            && receipt_binding(b,world,net,1,cert,id,start) && io::primitive_service(world.job(id).run,start) && io::stable_raw_io(world.job(id).run,stable) by {
        let end = if t < 56 { 56nat } else { t };
        no_needed_calls(end,p::Action::Receive { generation:1,certificate:cert });
    }
    assert forall|t:nat,cert:p::Certificate| t >= 0
        && #[trigger] emitted_forever(c,b,net,1,cert,t)
        implies exists|u:nat| u >= t && b.action(u) == (p::Action::Receive { generation:1,certificate:cert }) by {
        let end = if t < 56 { 56nat } else { t };
        no_needed_calls(end,p::Action::Receive { generation:1,certificate:cert });
    }
}

pub proof fn wire_origin(t:nat,cert:p::Certificate)->(id:nat)
    requires network().emitted(t).contains(emission(placement().state(t),1,cert))
    ensures receipt_origin(placement(),physical_world(),1,cert,id,t)
{
    point_facts(t);
    if cert is SourceDone {
        job_facts(0); raw_done(false,16,t); 0
    } else {
        assert(cert is DestinationDone);
        job_facts(1); raw_done(true,24,t); 1
    }
}

pub proof fn witness_network()
    ensures emission_registry(placement(),physical_world(),network(),0,1)
{
    let b = placement(); let world = physical_world(); let net = network();
    w::started_facts();
    assert(b.state(0) == w::started());
    assert forall|t:nat,cert:p::Certificate| t >= 0
        && net.emitted(t).contains(emission(b.state(t),1,cert)) implies b.state(t).certificates.contains((1nat,cert)) by {
        point_facts(t);
    }
    assert forall|t:nat,cert:p::Certificate| t >= 0
        && net.emitted(t).contains(emission(b.state(t),1,cert)) implies
        exists|id:nat| receipt_origin(b,world,1,cert,id,t) by {
        wire_origin(t,cert);
    }
    assert forall|t:nat,cert:p::Certificate| t >= 0
        && b.action(t) == (p::Action::Receive { generation:1,certificate:cert }) implies
        net.emitted(t).contains(emission(b.state(t),1,cert)) by {
        point_facts(t);
        let q = (t/8) as nat;
        assert(t%8 == 7);
        if q < 7 { assert(q == 3 || q == 4); }
        else { assert(t >= 56); }
        assert(cert is SourceDone || cert is DestinationDone);
    }
    assert forall|t:nat,u:nat,cert:p::Certificate| 0 <= t <= u
        && net.emitted(t).contains(emission(b.state(t),1,cert))
        && p::current(b.state(u),1) && !b.state(u).received.contains((1nat,cert)) implies
        net.emitted(u).contains(emission(b.state(u),1,cert)) by {
        point_facts(t); point_facts(u);
    }
}

pub proof fn witness_engines()
    ensures a::engines_settle(placement(),0,1),a::finite_admission(placement(),0,1)
{
    let b = placement();
    assert forall|txn:int,t:nat| t >= 0 && selected_holders(b.state(t),1).contains(txn) implies
        exists|u:nat| u >= t && b.state(u).sessions.contains_key(txn) && b.state(u).sessions[txn].resolved by {
        point_facts(t);
    }
    assert forall|t:nat| t >= 0 && shape::source_frozen(b.state(t),1) implies
        exists|holds:Seq<(int,int)>| a::exact_holds(b.state(t),1,holds) by {
        point_facts(t);
        assert(a::exact_holds(b.state(t),1,Seq::empty()));
    }
}

pub proof fn actual_service_witness()
    ensures p::constants_ok(w::constants()),p::behavior(w::constants(),w::recovered_prefix()),
        w::recovered_prefix().last() == placement().state(0),a::stable_steps(w::constants(),placement(),0),
        placement().state(0).plans.contains_key(1),a::finite_image(placement().state(0),1,seq![0int]),
        a::engines_settle(placement(),0,1),a::finite_admission(placement(),0,1),
        submitted_work(w::constants(),placement(),physical_world(),0,1),
        submitted_receipts(w::constants(),placement(),physical_world(),network(),0,1),
        emission_registry(placement(),physical_world(),network(),0,1),
        wire_fair(w::constants(),placement(),network(),0,1),completed(placement().state(56),1)
{
    placement_facts(); point_facts(0); point_facts(56);
    w::singleton_image(placement().state(0));
    witness_scheduling(); witness_network(); witness_engines();
}

} // verus!
