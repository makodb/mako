//! Enabled two-key handoff followed by a one-key return and a cross-shard read.
//! Checkpoints describe reached states; none is a protocol admission condition.
use vstd::prelude::*;
use vstd::imap::IMap;
use super::sharding_placement as p;
use super::sharding_transactions as t;
use super::sharding_witness::{Execution,constants,valid,last,grant,step,network,prefix};

verus! {

pub open spec fn keys(hi: int) -> Set<int> {
    if hi == 2 { Set::empty().insert(0int).insert(1int) } else { Set::empty().insert(0int) }
}
pub open spec fn packet(g: nat, k: int) -> p::Packet {
    p::Packet { generation:g,round:1,key:k,cell:prefix::seeded_cells()[k] }
}
pub open spec fn base_ok(b: p::State, g: nat, src: int, dst: int, hi: int) -> bool {
    &&& g > 0 && (hi == 1 || hi == 2)
    &&& src != dst && constants().shards.contains(src) && constants().shards.contains(dst)
    &&& b.logical == prefix::seeded_cells()
    &&& b.sessions == Map::empty().insert(0int,prefix::resolved())
    &&& b.active.is_none() && b.next_generation == g
    &&& !b.outcomes.contains_key(100 + g as int)
    &&& b.directory.len() > 0
    &&& b.physical.dom() == p::initial(constants()).physical.dom()
    &&& forall|k:int| keys(hi).contains(k) ==> {
        let a = p::replica(b,src,k); let d = p::replica(b,dst,k);
        &&& p::directory(b)[k].owner == src
        &&& a.role is Serving && a.epoch == p::directory(b)[k].epoch
        &&& a.cell == prefix::seeded_cells()[k] && a.fence < g
        &&& d.role is Empty && d.fence < g
    }
}

/// Stages: started, drained, captured, ready, retired, decided, delivered, finished.
pub open spec fn checkpoint(b:p::State,g:nat,src:int,dst:int,hi:int,n:nat) -> p::State {
    let selected = keys(hi);
    let commands = b.commands.insert((g,p::Command::Start));
    let commands = if n >= 1 { commands.insert((g,p::Command::Freeze)) } else { commands };
    let commands = if n >= 2 { commands.insert((g,p::Command::Final)) } else { commands };
    let commands = if n >= 4 { commands.insert((g,p::Command::Retire)) } else { commands };
    let commands = if n >= 5 { commands.insert((g,p::Command::Commit)) } else { commands };
    let certificates = if n >= 1 { b.certificates.insert((g,p::Certificate::Drained)) } else { b.certificates };
    let certificates = if n >= 3 { certificates.insert((g,p::Certificate::Ready)) } else { certificates };
    let certificates = if n >= 4 { certificates.insert((g,p::Certificate::Retired)) } else { certificates };
    let certificates = if n >= 6 { certificates.insert((g,p::Certificate::SourceDone)).insert((g,p::Certificate::DestinationDone)) } else { certificates };
    let received = if n >= 1 { b.received.insert((g,p::Certificate::Drained)) } else { b.received };
    let received = if n >= 3 { received.insert((g,p::Certificate::Ready)) } else { received };
    let received = if n >= 4 { received.insert((g,p::Certificate::Retired)) } else { received };
    let received = if n >= 6 { received.insert((g,p::Certificate::SourceDone)).insert((g,p::Certificate::DestinationDone)) } else { received };
    let packets = if n >= 2 { b.packets.insert(packet(g,0)) } else { b.packets };
    let packets = if n >= 2 && hi == 2 { packets.insert(packet(g,1)) } else { packets };
    p::State {
        active:if n == 7 { None } else { Some(g) }, next_generation:g+1,
        plans:b.plans.insert(g,p::Plan { nonce:100+g as int,src,dst,table:0,lo:0,hi:Some(hi),keys:selected,old:p::directory(b) }),
        phases:b.phases.insert(g,if n == 0 { p::Phase::Copy } else if n == 1 { p::Phase::Freezing }
            else if n <= 3 { p::Phase::Final } else if n == 4 { p::Phase::Retiring } else { p::Phase::Committed }),
        commands,certificates,received,packets,
        directory:if n >= 5 { b.directory.push(Map::new(p::directory(b).dom(),|k:int|
            if selected.contains(k) { grant(dst,g) } else { p::directory(b)[k] })) } else { b.directory },
        outcomes:if n >= 5 { b.outcomes.insert(100+g as int,p::Outcome { generation:g,committed:true }) } else { b.outcomes },
        physical:IMap::new(|q:(int,int)| b.physical.contains_key(q),|q:(int,int)| {
            let r = b.physical[q];
            if selected.contains(q.1) && q.0 == src && n >= 1 {
                p::Replica { role:if n >= 6 { p::Role::Empty } else if n >= 4 { p::Role::Retired } else { p::Role::Frozen },
                    cell:if n >= 6 { p::empty_cell() } else { r.cell },fence:g,terminal:n>=6,..r }
            } else if selected.contains(q.1) && q.0 == dst {
                p::Replica { cell:if n >= 3 { prefix::seeded_cells()[q.1] } else { p::empty_cell() },
                    role:if n >= 6 { p::Role::Serving } else if n >= 3 { p::Role::Ready } else { p::Role::Stage },
                    epoch:g,fence:g,terminal:n>=6,round:if n>=2 { 1nat } else { 0nat },covered:n>=3 }
            } else { r }
        }),..b
    }
}

proof fn begin_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),last(e).placement == b,base_ok(b,g,src,dst,hi)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,0),last(z).txns == last(e).txns
{
    hide(t::behavior);
    assert(p::selected(constants(),0,0,Some(hi)) =~= keys(hi));
    let z = network(e,p::Action::Begin { nonce:100+g as int,src,dst,table:0,lo:0,hi:Some(hi) });
    let z = network(z,p::Action::Deliver { generation:g,command:p::Command::Start,owner:dst });
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,0).physical);
    z
}
proof fn drain_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,0)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,1),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestFreeze);
    let z = network(z,p::Action::Deliver { generation:g,command:p::Command::Freeze,owner:src });
    let z = network(z,p::Action::Drain { generation:g });
    let z = network(z,p::Action::Receive { generation:g,certificate:p::Certificate::Drained });
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,1).physical);
    assert(last(z).placement.phases =~= checkpoint(b,g,src,dst,hi,1).phases);
    z
}
proof fn capture_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,1)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,2),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestFinal);
    let z = network(z,p::Action::Deliver { generation:g,command:p::Command::Final,owner:dst });
    let z = network(z,p::Action::Capture { generation:g,round:1,key:0 });
    let z = if hi == 2 { network(z,p::Action::Capture { generation:g,round:1,key:1 }) } else { z };
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,2).physical);
    assert(last(z).placement.phases =~= checkpoint(b,g,src,dst,hi,2).phases);
    z
}
proof fn ready_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,2)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,3),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::DeliverCopy { packet:packet(g,0) });
    let z = if hi == 2 { network(z,p::Action::DeliverCopy { packet:packet(g,1) }) } else { z };
    let z = network(z,p::Action::Seal { generation:g });
    let z = network(z,p::Action::Receive { generation:g,certificate:p::Certificate::Ready });
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,3).physical);
    z
}
proof fn retire_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,3)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,4),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::RequestRetire);
    let z = network(z,p::Action::Deliver { generation:g,command:p::Command::Retire,owner:src });
    let z = network(z,p::Action::Receive { generation:g,certificate:p::Certificate::Retired });
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,4).physical);
    assert(last(z).placement.phases =~= checkpoint(b,g,src,dst,hi,4).phases);
    z
}
proof fn decide_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,4)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,5),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::Commit);
    assert(last(z).placement.phases =~= checkpoint(b,g,src,dst,hi,5).phases);
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,5).physical);
    z
}
proof fn deliver_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,5)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,6),last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = network(e,p::Action::Deliver { generation:g,command:p::Command::Commit,owner:dst });
    let z = network(z,p::Action::Deliver { generation:g,command:p::Command::Commit,owner:src });
    let z = network(z,p::Action::Receive { generation:g,certificate:p::Certificate::SourceDone });
    let z = network(z,p::Action::Receive { generation:g,certificate:p::Certificate::DestinationDone });
    assert(last(z).placement.physical =~= checkpoint(b,g,src,dst,hi,6).physical);
    assert(last(z).placement.certificates =~= checkpoint(b,g,src,dst,hi,6).certificates);
    z
}
proof fn finish_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),base_ok(b,g,src,dst,hi),last(e).placement == checkpoint(b,g,src,dst,hi,6)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,7),last(z).txns == last(e).txns
{
    hide(t::behavior);
    network(e,p::Action::Finish)
}

proof fn complete_move(e:Execution,b:p::State,g:nat,src:int,dst:int,hi:int) -> (z:Execution)
    requires valid(e),last(e).placement == b,base_ok(b,g,src,dst,hi)
    ensures valid(z),last(z).placement == checkpoint(b,g,src,dst,hi,7),last(z).txns == last(e).txns
{
    hide(t::behavior);
    hide(checkpoint);
    let z = begin_move(e,b,g,src,dst,hi);
    let z = drain_move(z,b,g,src,dst,hi);
    let z = capture_move(z,b,g,src,dst,hi);
    let z = ready_move(z,b,g,src,dst,hi);
    let z = retire_move(z,b,g,src,dst,hi);
    let z = decide_move(z,b,g,src,dst,hi);
    let z = deliver_move(z,b,g,src,dst,hi);
    finish_move(z,b,g,src,dst,hi)
}

pub open spec fn forward() -> p::State {
    checkpoint(prefix::seeded_placement(),1,0,1,2,7)
}
pub open spec fn returned() -> p::State { checkpoint(forward(),2,1,0,1,7) }

proof fn movement_bases()
    ensures base_ok(prefix::seeded_placement(),1,0,1,2),base_ok(forward(),2,1,0,1)
{
    assert(prefix::seeded_placement().physical.dom() =~= p::initial(constants()).physical.dom());
    assert(forward().physical.dom() =~= p::initial(constants()).physical.dom());
}

proof fn forward_execution() -> (e:Execution)
    ensures valid(e),last(e).placement == forward(),last(e).txns.dom() == Set::empty().insert(0int)
{
    hide(t::behavior);
    let e = prefix::seed_execution();
    movement_bases();
    complete_move(e,prefix::seeded_placement(),1,0,1,2)
}

pub open spec fn old_authentic(s:p::State) -> bool {
    &&& s.plans[1].src == 0 && s.plans[1].dst == 1 && s.plans[1].keys.contains(0)
    &&& s.commands.contains((1,p::Command::Freeze)) && s.commands.contains((1,p::Command::Commit))
    &&& s.packets.contains(packet(1,0))
    &&& p::replica(s,0,0).fence == 2 && p::replica(s,1,0).fence == 2
}
proof fn old_command(e:Execution,command:p::Command,owner:int) -> (z:Execution)
    requires valid(e),old_authentic(last(e).placement),
        command is Freeze || command is Commit,owner == 0 || owner == 1
    ensures valid(z),last(z).placement == last(e).placement,last(z).txns == last(e).txns
{
    hide(t::behavior);
    let s = last(e).placement;
    assert(!p::local_guard(s,1,command,0));
    assert(!p::local_guard(s,1,command,1));
    let z = network(e,p::Action::Deliver { generation:1,command,owner });
    assert(last(z).placement.physical =~= s.physical);
    z
}
proof fn old_messages(e:Execution) -> (z:Execution)
    requires valid(e),old_authentic(last(e).placement)
    ensures valid(z),last(z).placement == last(e).placement,last(z).txns == last(e).txns
{
    hide(t::behavior);
    let z = old_command(e,p::Command::Freeze,0);
    let z = old_command(z,p::Command::Commit,0);
    let z = old_command(z,p::Command::Commit,1);
    assert(!p::copy_guard(last(z).placement,packet(1,0)));
    network(z,p::Action::DeliverCopy { packet:packet(1,0) })
}

proof fn return_capture(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == forward()
    ensures valid(z),last(z).placement == checkpoint(forward(),2,1,0,1,2),last(z).txns == last(e).txns
{
    hide(t::behavior);
    movement_bases();
    let z = begin_move(e,forward(),2,1,0,1);
    let z = drain_move(z,forward(),2,1,0,1);
    let z = capture_move(z,forward(),2,1,0,1);
    let before = last(z).placement;
    assert(p::replica(before,0,0).role is Stage);
    let z = old_messages(z);
    assert(last(z).placement.physical == before.physical);
    z
}
proof fn return_finish(e:Execution) -> (z:Execution)
    requires valid(e),last(e).placement == checkpoint(forward(),2,1,0,1,2)
    ensures valid(z),last(z).placement == returned(),last(z).txns == last(e).txns
{
    hide(t::behavior);
    movement_bases();
    let z = ready_move(e,forward(),2,1,0,1);
    let z = retire_move(z,forward(),2,1,0,1);
    let z = decide_move(z,forward(),2,1,0,1);
    let z = deliver_move(z,forward(),2,1,0,1);
    let z = finish_move(z,forward(),2,1,0,1);
    let before = last(z).placement;
    assert(p::replica(before,0,0).role is Serving);
    let z = old_messages(z);
    assert(last(z).placement.physical == before.physical);
    z
}

/// A rejected route is included in the trace, but cannot be a progressing step.
proof fn rejected(e:Execution,a:t::Action) -> (z:Execution)
    requires valid(e),!t::enabled(constants(),last(e),a)
    ensures valid(z),last(z) == last(e),
        z.states == e.states.push(last(e)),z.actions == e.actions.push(a)
{
    t::lemma_behavior_extend(constants(),e.states,e.actions,a);
    Execution { states:e.states.push(last(e)),actions:e.actions.push(a) }
}

proof fn split_read(e:Execution) -> (z:Execution)
    requires valid(e),!last(e).txns.contains_key(1),!last(e).placement.sessions.contains_key(1),
        last(e).placement.physical.contains_key((0,0)),last(e).placement.physical.contains_key((1,1)),
        p::replica(last(e).placement,0,0).role is Serving,p::replica(last(e).placement,0,0).epoch == 2,
        p::replica(last(e).placement,1,1).role is Serving,p::replica(last(e).placement,1,1).epoch == 1
    ensures valid(z),
        last(z).placement == (p::State { sessions:last(e).placement.sessions.insert(1,prefix::resolved()),..last(e).placement }),
        last(z).txns[1].reads[0] == p::replica(last(e).placement,0,0).cell,
        last(z).txns[1].reads[1] == p::replica(last(e).placement,1,1).cell,
        last(z).txns[1].outcome is Committed,last(z).txns[1].replied.is_some()
{
    hide(t::behavior);
    let body = Map::empty().insert(0int,t::Op::Read).insert(1int,t::Op::Read);
    let z = step(e,t::Action::Begin { txn:1,body });
    assert(!p::admission(last(z).placement,1,0,grant(0,0)));
    let z = rejected(z,t::Action::Placement { action:p::Action::Acquire { txn:1,key:0,grant:grant(0,0) } });
    let z = network(z,p::Action::Acquire { txn:1,key:0,grant:grant(0,2) });
    let z = network(z,p::Action::Acquire { txn:1,key:1,grant:grant(1,1) });
    let z = step(z,t::Action::Fetch { txn:1,key:0 });
    let z = step(z,t::Action::Fetch { txn:1,key:1 });
    assert(t::read_keys(body) =~= body.dom());
    assert(last(z).txns[1].reads.dom() =~= body.dom());
    assert(t::writes(body,last(z).txns[1].reads) =~= Map::<int,Option<int>>::empty());
    let z = step(z,t::Action::Commit { txn:1 });
    let z = step(z,t::Action::Reply { txn:1 });
    let z = network(z,p::Action::Release { txn:1,key:0 });
    let z = network(z,p::Action::Release { txn:1,key:1 });
    assert(last(z).placement.physical =~= last(e).placement.physical);
    assert(last(z).placement.logical =~= last(e).placement.logical);
    assert(last(z).placement.sessions[1].held =~= Map::<int,p::Grant>::empty());
    assert(last(z).placement.sessions =~= last(e).placement.sessions.insert(1,prefix::resolved()));
    z
}

/// Both moves finish; old messages are delivered at return staging and serving.
/// The fresh read rejects epoch zero despite owner zero, then reads across owners
/// zero and one with the returned and retained grants, commits, and replies.
pub proof fn witness_partial_return_and_fenced_read() -> (e:Execution)
    ensures valid(e),
        last(e).placement.active.is_none(),
        last(e).placement.outcomes[101] == (p::Outcome { generation:1,committed:true }),
        last(e).placement.outcomes[102] == (p::Outcome { generation:2,committed:true }),
        p::directory(last(e).placement)[0] == grant(0,2),
        p::directory(last(e).placement)[1] == grant(1,1),
        p::directory(last(e).placement)[2] == grant(1,0),
        last(e).placement.logical[0].value == Some(10int),
        last(e).placement.logical[1].value == Some(11int),
        last(e).placement.logical[2].value == Some(20int),
        p::replica(last(e).placement,1,1) == p::replica(forward(),1,1),
        p::replica(last(e).placement,0,2) == p::replica(prefix::seeded_placement(),0,2),
        p::replica(last(e).placement,1,2) == p::replica(prefix::seeded_placement(),1,2),
        last(e).txns[1].reads[0].value == Some(10int),
        last(e).txns[1].reads[1].value == Some(11int),
        last(e).txns[1].outcome is Committed,last(e).txns[1].replied.is_some(),
        last(e).placement.sessions[1].held.is_empty()
{
    hide(t::behavior);
    let e = forward_execution();
    let e = return_capture(e);
    let e = return_finish(e);
    split_read(e)
}

} // verus!
