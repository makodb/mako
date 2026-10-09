//! A finite live-process abort/retry execution. Delivery authenticates issuance;
//! participant fences, rather than the master's phase, reject late messages.
use vstd::prelude::*;
use super::sharding_placement as p;
use super::sharding_transactions as t;
use super::sharding_witness::{Execution,constants,valid,last,grant,step,network};
use super::sharding_witness::prefix::{seed_execution,seeded_placement,resolved};

verus! {

pub open spec fn begin(nonce: int) -> p::Action {
    p::Action::Begin { nonce,src:0,dst:1,table:0,lo:0,hi:Some(1int) }
}
pub open spec fn old_packet() -> p::Packet {
    p::Packet { generation:1,round:0,key:0,cell:p::Cell { value:Some(10int),writer:0 } }
}
pub open spec fn plan(nonce: int) -> p::Plan {
    p::Plan { nonce,src:0,dst:1,table:0,lo:0,hi:Some(1int),
        keys:Set::empty().insert(0int),old:p::directory(seeded_placement()) }
}
pub open spec fn abort_result() -> p::Outcome { p::Outcome { generation:1,committed:false } }
pub open spec fn prepared() -> p::State {
    let s = seeded_placement();
    p::State { active:Some(1nat),next_generation:2,
        plans:Map::empty().insert(1nat,plan(200)),
        phases:Map::empty().insert(1nat,p::Phase::Copy),
        commands:Set::empty().insert((1nat,p::Command::Start)),
        packets:Set::empty().insert(old_packet()),
        physical:s.physical.insert((1int,0int),p::Replica { cell:old_packet().cell,
            role:p::Role::Stage,epoch:1,fence:1,terminal:false,round:0,covered:true }),..s }
}
pub proof fn prepare(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == seeded_placement()
    ensures valid(z),last(z).placement == prepared(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    assert(p::selected(constants(),0,0,Some(1int)) =~= Set::empty().insert(0int));
    let z = network(e,begin(200));
    assert(last(z).placement.plans[1].keys =~= Set::empty().insert(0int));
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Start,owner:1 });
    let z = network(z,p::Action::Capture { generation:1,round:0,key:0 });
    let z = network(z,p::Action::DeliverCopy { packet:old_packet() });
    assert(last(z).placement.physical =~= prepared().physical);
    assert(last(z).placement.plans =~= prepared().plans);
    z
}

pub open spec fn frozen() -> p::State {
    let s = prepared();
    p::State { physical:s.physical.insert((0int,0int),p::Replica {
            role:p::Role::Frozen,fence:1,terminal:false,..s.physical[(0int,0int)] }),
        phases:s.phases.insert(1nat,p::Phase::Freezing),
        commands:s.commands.insert((1nat,p::Command::Freeze)),
        certificates:s.certificates.insert((1nat,p::Certificate::Drained)),..s }
}
pub proof fn freeze_with_delayed_response(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == prepared()
    ensures valid(z),last(z).placement == frozen(),last(z).txns == last(e).txns,
        !p::enabled(constants(),last(z).placement,p::Action::RequestFinal),
        last(z).placement.certificates.contains((1nat,p::Certificate::Drained)),
        !last(z).placement.received.contains((1nat,p::Certificate::Drained)),
        p::replica(last(z).placement,1,0).role is Stage,
        forall|txn:int| !p::admission(last(z).placement,txn,0,grant(1,1))
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestFreeze);
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Freeze,owner:0 });
    let z = network(z,p::Action::Drain { generation:1 });
    assert(last(z).placement.physical =~= frozen().physical);
    z
}

pub open spec fn abort_decided() -> p::State {
    let s = frozen();
    p::State { phases:s.phases.insert(1nat,p::Phase::Aborted),
        commands:s.commands.insert((1nat,p::Command::Abort)),
        outcomes:s.outcomes.insert(200int,abort_result()),..s }
}
pub open spec fn source_done() -> p::State {
    let s = abort_decided();
    p::State { physical:s.physical.insert((0int,0int),p::Replica {
            role:p::Role::Serving,terminal:true,..s.physical[(0int,0int)] }),
        certificates:s.certificates.insert((1nat,p::Certificate::SourceDone)),
        received:s.received.insert((1nat,p::Certificate::SourceDone)),..s }
}
pub proof fn abort_and_restore_source(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == frozen()
    ensures valid(z),last(z).placement == source_done(),last(z).txns == last(e).txns,
        !p::enabled(constants(),last(z).placement,p::Action::Finish),
        p::replica(last(z).placement,0,0).cell.value == Some(10int),
        p::directory(last(z).placement)[0] == grant(0,0),
        forall|txn:int| !p::admission(last(z).placement,txn,0,grant(1,1))
{
    hide(t::behavior);
    let z = network(e,p::Action::Abort);
    assert(last(z).placement == abort_decided());
    assert(!p::enabled(constants(),last(z).placement,p::Action::Finish));
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Abort,owner:0 });
    assert(!p::enabled(constants(),last(z).placement,p::Action::Finish));
    let z = network(z,p::Action::Receive { generation:1,certificate:p::Certificate::SourceDone });
    assert(last(z).placement.physical =~= source_done().physical);
    z
}

pub open spec fn aborted() -> p::State {
    let s = source_done();
    p::State { physical:s.physical.insert((1int,0int),p::Replica {
            cell:p::empty_cell(),role:p::Role::Empty,terminal:true,..s.physical[(1int,0int)] }),
        certificates:s.certificates.insert((1nat,p::Certificate::DestinationDone)),
        received:s.received.insert((1nat,p::Certificate::DestinationDone)),active:None,
        replies:s.replies.insert(200int,abort_result()),..s }
}
pub proof fn finish_abort(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == source_done()
    ensures valid(z),last(z).placement == aborted(),last(z).txns == last(e).txns,
        last(z).placement.active.is_none(),last(z).placement.outcomes[200] == abort_result(),
        last(z).placement.replies[200] == abort_result()
{
    hide(t::behavior);
    let z = network(e,p::Action::Deliver { generation:1,command:p::Command::Abort,owner:1 });
    assert(!p::enabled(constants(),last(z).placement,p::Action::Finish));
    assert(last(z).placement.certificates.contains((1nat,p::Certificate::DestinationDone)));
    let z = network(z,p::Action::Receive { generation:1,certificate:p::Certificate::DestinationDone });
    let z = network(z,p::Action::Finish);
    assert(!last(z).placement.replies.contains_key(200));
    let z = network(z,p::Action::Reply { nonce:200 });
    let z = network(z,p::Action::Reply { nonce:200 });
    assert(last(z).placement.physical =~= aborted().physical);
    assert(last(z).placement.replies =~= aborted().replies);
    z
}

// Rejected administrative retries are intentional stutters, unlike every
// progressing step, which is passed to the enabled-only shared helpers.
pub proof fn rejected(e: Execution, a: t::Action) -> (z: Execution)
    requires valid(e),!t::enabled(constants(),last(e),a)
    ensures valid(z),last(z) == last(e),z.actions == e.actions.push(a),
        z.states == e.states.push(last(e))
{
    t::lemma_behavior_extend(constants(),e.states,e.actions,a);
    Execution { states:e.states.push(last(e)),actions:e.actions.push(a) }
}

pub open spec fn read_complete() -> p::State {
    let s = aborted();
    p::State { sessions:s.sessions.insert(1int,resolved()),..s }
}
pub open spec fn client_result() -> t::Result {
    t::Result::Committed { reads:Map::empty().insert(0int,Some(10int)) }
}
pub proof fn read_after_abort(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == aborted(),!last(e).txns.contains_key(1)
    ensures valid(z),last(z).placement == read_complete(),
        t::successful(last(z),1),t::retained_result(last(z),1) == Some(client_result())
{
    hide(t::behavior);
    let z = rejected(e,t::Action::Placement { action:begin(200) });
    let body = Map::empty().insert(0int,t::Op::Read);
    let z = step(z,t::Action::Begin { txn:1,body });
    let z = network(z,p::Action::Acquire { txn:1,key:0,grant:grant(0,0) });
    assert(p::read(last(z).placement,1,0) == Some(old_packet().cell));
    let z = step(z,t::Action::Fetch { txn:1,key:0 });
    assert(t::read_keys(body) =~= body.dom());
    assert(t::writes(body,last(z).txns[1].reads) =~= Map::<int,Option<int>>::empty());
    let z = step(z,t::Action::Commit { txn:1 });
    let z = step(z,t::Action::Reply { txn:1 });
    let z = network(z,p::Action::Release { txn:1,key:0 });
    assert(last(z).placement.logical =~= read_complete().logical);
    assert(last(z).placement.physical =~= read_complete().physical);
    assert(last(z).placement.sessions[1].held =~= Map::<int,p::Grant>::empty());
    assert(last(z).placement.sessions =~= read_complete().sessions);
    assert(Map::new(last(z).txns[1].reads.dom(),|k:int| last(z).txns[1].reads[k].value)
        =~= Map::empty().insert(0int,Some(10int)));
    z
}

pub open spec fn fresh() -> p::State {
    let s = read_complete();
    p::State { active:Some(2nat),next_generation:3,plans:s.plans.insert(2nat,plan(201)),
        phases:s.phases.insert(2nat,p::Phase::Copy),commands:s.commands.insert((2nat,p::Command::Start)),
        physical:s.physical.insert((1int,0int),p::Replica { cell:p::empty_cell(),
            role:p::Role::Stage,epoch:2,fence:2,terminal:false,round:0,covered:false }),..s }
}
pub proof fn fresh_attempt(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == read_complete()
    ensures valid(z),last(z).placement == fresh(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    assert(p::selected(constants(),0,0,Some(1int)) =~= Set::empty().insert(0int));
    let z = network(e,begin(201));
    assert(last(z).placement.plans[2].keys =~= Set::empty().insert(0int));
    let z = network(z,p::Action::Deliver { generation:2,command:p::Command::Start,owner:1 });
    assert(last(z).placement.plans =~= fresh().plans);
    assert(last(z).placement.physical =~= fresh().physical);
    z
}

pub proof fn late_messages(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == fresh()
    ensures valid(z),last(z).placement == fresh(),last(z).txns == last(e).txns,
        last(z).placement.outcomes[200] == abort_result(),
        last(z).placement.replies[200] == abort_result(),
        p::enabled(constants(),last(z).placement,p::Action::RequestFreeze),
        !last(z).placement.outcomes.contains_key(201)
{
    hide(t::behavior);
    assert(last(e).placement.plans[1].keys.contains(0int));
    assert(p::replica(last(e).placement,0,0).fence == 1);
    assert(p::replica(last(e).placement,0,0).terminal);
    assert(!p::local_guard(last(e).placement,1,p::Command::Abort,0));
    assert(!p::local_guard(last(e).placement,1,p::Command::Abort,1));
    assert(!p::local_guard(last(e).placement,1,p::Command::Freeze,0));
    assert(!p::copy_guard(last(e).placement,old_packet()));
    let z = network(e,p::Action::Deliver { generation:1,command:p::Command::Abort,owner:0 });
    assert(last(z).placement.physical =~= fresh().physical);
    assert(last(z).placement == fresh());
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Abort,owner:1 });
    assert(last(z).placement.physical =~= fresh().physical);
    assert(last(z).placement == fresh());
    let z = network(z,p::Action::Deliver { generation:1,command:p::Command::Freeze,owner:0 });
    assert(last(z).placement.physical =~= fresh().physical);
    assert(last(z).placement == fresh());
    let z = network(z,p::Action::DeliverCopy { packet:old_packet() });
    assert(last(z).placement == fresh());
    let z = rejected(z,t::Action::Placement { action:begin(200) });
    let z = network(z,p::Action::Reply { nonce:200 });
    assert(last(z).placement.replies =~= fresh().replies);
    z
}

/// The first generation is fully aborted and its result has been retried. A
/// source read returned 10. Generation two is genuinely prepared and remains
/// live, but this witness deliberately does not claim that it has committed.
pub proof fn witness_abort_retry_and_late_messages() -> (e: Execution)
    ensures valid(e),last(e).placement == fresh(),
        t::successful(last(e),1),t::retained_result(last(e),1) == Some(client_result()),
        p::directory(last(e).placement)[0] == grant(0,0),
        p::replica(last(e).placement,0,0).cell.value == Some(10int),
        p::replica(last(e).placement,0,0).role is Serving,
        p::replica(last(e).placement,1,0).role is Stage,
        last(e).placement.outcomes[200] == abort_result(),
        last(e).placement.replies[200] == abort_result(),
        last(e).placement.active == Some(2nat),
        p::enabled(constants(),last(e).placement,p::Action::RequestFreeze),
        !last(e).placement.outcomes.contains_key(201)
{
    hide(t::behavior);
    let e = seed_execution();
    let e = prepare(e);
    let e = freeze_with_delayed_response(e);
    let e = abort_and_restore_source(e);
    let e = finish_abort(e);
    let e = read_after_abort(e);
    let e = fresh_attempt(e);
    late_messages(e)
}

pub open spec fn retry_frozen() -> p::State {
    let s = fresh();
    p::State { physical:s.physical.insert((0int,0int),p::Replica {
            role:p::Role::Frozen,fence:2,terminal:false,..s.physical[(0int,0int)] }),
        phases:s.phases.insert(2nat,p::Phase::Freezing),
        commands:s.commands.insert((2nat,p::Command::Freeze)),
        certificates:s.certificates.insert((2nat,p::Certificate::Drained)),
        received:s.received.insert((2nat,p::Certificate::Drained)),..s }
}
pub proof fn freeze_retry(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == fresh()
    ensures valid(z),last(z).placement == retry_frozen(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestFreeze);
    let z = network(z,p::Action::Deliver { generation:2,command:p::Command::Freeze,owner:0 });
    let z = network(z,p::Action::Drain { generation:2 });
    let z = network(z,p::Action::Receive { generation:2,certificate:p::Certificate::Drained });
    assert(last(z).placement.physical =~= retry_frozen().physical);
    z
}

pub open spec fn retry_packet() -> p::Packet {
    p::Packet { generation:2,round:1,key:0,cell:old_packet().cell }
}
pub open spec fn retry_ready() -> p::State {
    let s = retry_frozen();
    p::State { physical:s.physical.insert((1int,0int),p::Replica {
            cell:retry_packet().cell,role:p::Role::Ready,round:1,covered:true,..s.physical[(1int,0int)] }),
        phases:s.phases.insert(2nat,p::Phase::Final),
        commands:s.commands.insert((2nat,p::Command::Final)),
        packets:s.packets.insert(retry_packet()),
        certificates:s.certificates.insert((2nat,p::Certificate::Ready)),
        received:s.received.insert((2nat,p::Certificate::Ready)),..s }
}
pub proof fn copy_retry(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == retry_frozen()
    ensures valid(z),last(z).placement == retry_ready(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestFinal);
    let z = network(z,p::Action::Deliver { generation:2,command:p::Command::Final,owner:1 });
    let z = network(z,p::Action::Capture { generation:2,round:1,key:0 });
    let z = network(z,p::Action::DeliverCopy { packet:retry_packet() });
    let z = network(z,p::Action::Seal { generation:2 });
    let z = network(z,p::Action::Receive { generation:2,certificate:p::Certificate::Ready });
    assert(last(z).placement.physical =~= retry_ready().physical);
    z
}

pub open spec fn retry_retired() -> p::State {
    let s = retry_ready();
    p::State { physical:s.physical.insert((0int,0int),p::Replica {
            role:p::Role::Retired,..s.physical[(0int,0int)] }),
        phases:s.phases.insert(2nat,p::Phase::Retiring),
        commands:s.commands.insert((2nat,p::Command::Retire)),
        certificates:s.certificates.insert((2nat,p::Certificate::Retired)),
        received:s.received.insert((2nat,p::Certificate::Retired)),..s }
}
pub proof fn retire_retry(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == retry_ready()
    ensures valid(z),last(z).placement == retry_retired(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestRetire);
    let z = network(z,p::Action::Deliver { generation:2,command:p::Command::Retire,owner:0 });
    let z = network(z,p::Action::Receive { generation:2,certificate:p::Certificate::Retired });
    assert(last(z).placement.physical =~= retry_retired().physical);
    z
}

pub open spec fn retry_result() -> p::Outcome { p::Outcome { generation:2,committed:true } }
pub open spec fn retry_decided() -> p::State {
    let s = retry_retired();
    p::State { phases:s.phases.insert(2nat,p::Phase::Committed),
        commands:s.commands.insert((2nat,p::Command::Commit)),
        outcomes:s.outcomes.insert(201int,retry_result()),
        directory:s.directory.push(p::directory(s).insert(0int,grant(1,2))),..s }
}
pub proof fn commit_retry(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == retry_retired()
    ensures valid(z),last(z).placement == retry_decided(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::Commit);
    assert(p::directory(last(z).placement) =~= p::directory(retry_decided()));
    assert(last(z).placement.directory =~= retry_decided().directory);
    z
}

pub open spec fn retry_destination() -> p::State {
    let s = retry_decided();
    p::State { physical:s.physical.insert((1int,0int),p::Replica {
            role:p::Role::Serving,terminal:true,..s.physical[(1int,0int)] }),
        certificates:s.certificates.insert((2nat,p::Certificate::DestinationDone)),
        received:s.received.insert((2nat,p::Certificate::DestinationDone)),..s }
}
pub proof fn activate_retry(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == retry_decided()
    ensures valid(z),last(z).placement == retry_destination(),last(z).txns == last(e).txns,
        !p::enabled(constants(),last(z).placement,p::Action::Finish)
{
    hide(t::behavior);
    let z = network(e,p::Action::Deliver { generation:2,command:p::Command::Commit,owner:1 });
    let z = network(z,p::Action::Receive { generation:2,certificate:p::Certificate::DestinationDone });
    assert(last(z).placement.physical =~= retry_destination().physical);
    z
}

pub open spec fn retry_finished() -> p::State {
    let s = retry_destination();
    p::State { physical:s.physical
            .insert((0int,0int),p::Replica { cell:p::empty_cell(),role:p::Role::Empty,
                terminal:true,..s.physical[(0int,0int)] }),
        certificates:s.certificates.insert((2nat,p::Certificate::SourceDone)),
        received:s.received.insert((2nat,p::Certificate::SourceDone)),
        active:None,replies:s.replies.insert(201int,retry_result()),..s }
}
pub proof fn finish_retry(e: Execution) -> (z: Execution)
    requires valid(e),last(e).placement == retry_destination()
    ensures valid(z),last(z).placement == retry_finished(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    assert(!p::enabled(constants(),last(e).placement,p::Action::Finish));
    let z = network(e,p::Action::Deliver { generation:2,command:p::Command::Commit,owner:0 });
    let z = network(z,p::Action::Receive { generation:2,certificate:p::Certificate::SourceDone });
    let z = network(z,p::Action::Finish);
    let z = network(z,p::Action::Reply { nonce:201 });
    let z = network(z,p::Action::Reply { nonce:200 });
    assert(last(z).placement.physical =~= retry_finished().physical);
    assert(last(z).placement.certificates =~= retry_finished().certificates);
    assert(last(z).placement.received =~= retry_finished().received);
    assert(last(z).placement.replies =~= retry_finished().replies);
    z
}

/// Extension of the live retry checkpoint: authentic final-copy capture and all
/// independently received certificates complete generation two without changing
/// the admitted read result or the first generation's retained aborted outcome.
pub proof fn witness_abort_then_successful_retry() -> (e: Execution)
    ensures valid(e),last(e).placement == retry_finished(),
        t::successful(last(e),1),t::retained_result(last(e),1) == Some(client_result()),
        last(e).placement.logical[0].value == Some(10int),
        p::directory(last(e).placement)[0] == grant(1,2),
        p::directory(last(e).placement)[1] == grant(0,0),
        p::replica(last(e).placement,0,0).role is Empty,
        p::replica(last(e).placement,1,0).role is Serving,
        p::replica(last(e).placement,1,0).cell.value == Some(10int),
        last(e).placement.outcomes[200] == abort_result(),
        last(e).placement.replies[200] == abort_result(),
        last(e).placement.outcomes[201] == retry_result(),
        last(e).placement.replies[201] == retry_result(),
        last(e).placement.active.is_none()
{
    hide(t::behavior);
    let e = witness_abort_retry_and_late_messages();
    let e = freeze_retry(e);
    let e = copy_retry(e);
    let e = retire_retry(e);
    let e = commit_retry(e);
    let e = activate_retry(e);
    finish_retry(e)
}

} // verus!
