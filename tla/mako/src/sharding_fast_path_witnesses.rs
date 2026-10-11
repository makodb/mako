//! Enabled executions: fast scans, freeze-before-publication, and stale-view
//! fallback after an actual placement migration (not an assumed final state).
use vstd::prelude::*;
use super::*;
use super::proofs::*;

verus! {

pub open spec fn constants() -> p::Constants {
    p::Constants {
        keys: Set::empty().insert(0int).insert(1int),
        shards: Set::empty().insert(0int).insert(1int),
        table: Map::empty().insert(0int,0int).insert(1int,0int),
        coordinate: Map::empty().insert(0int,0int).insert(1int,1int),
        owners: Map::empty().insert(0int,0int).insert(1int,0int),
    }
}
pub struct Execution { pub states: Seq<State>, pub actions: Seq<Action> }
pub open spec fn last(e: Execution) -> State { e.states.last() }
pub open spec fn valid(e: Execution) -> bool {
    behavior(constants(),e.states,e.actions) && inv(constants(),last(e))
}
pub proof fn start() -> (e: Execution)
    ensures valid(e), last(e) == initial(constants())
{
    lemma_init(constants());
    Execution { states: seq![initial(constants())], actions: Seq::empty() }
}
pub proof fn step(e: Execution, a: Action) -> (z: Execution)
    requires valid(e), enabled(constants(),last(e),a)
    ensures valid(z), last(z) == apply(constants(),last(e),a),
        z.states == e.states.push(apply(constants(),last(e),a)), z.actions == e.actions.push(a)
{
    lemma_step(constants(),last(e),a);
    assert(behavior(constants(),e.states.push(apply(constants(),last(e),a)),e.actions.push(a)));
    Execution { states: e.states.push(apply(constants(),last(e),a)), actions: e.actions.push(a) }
}
pub proof fn placement(e: Execution, a: p::Action) -> (z: Execution)
    requires valid(e), p::enabled(constants(),last(e).placement,a)
    ensures valid(z), last(z) == placement_step(constants(),last(e),a)
{ step(e,Action::Placement { action: a }) }

pub proof fn fast_scan_and_freeze() -> (e: Execution)
    ensures valid(e), last(e).placement.directory.len() == 1,
        last(e).requests[0].snapshot == Some(0nat),
        p::replica(last(e).placement,0,0).role is Frozen,
        !fast(last(e),last(e).requests[0]),
        !can_admit(constants(),last(e),0,1,0),
        last(e).placement.sessions[0].held.contains_key(0),
        !p::enabled(constants(),last(e).placement,p::Action::Drain { generation: 1 })
{
    hide(behavior);
    let e = start();
    let e = placement(e,p::Action::Cache { client: 0,snapshot: 0 });
    let scope = Scope::Scan { table: 0,lo: 0,hi: Some(2int) };
    assert(keys(constants(),scope) =~= constants().keys);
    let e = step(e,Action::Capture { id: 0,client: 0,scope,representative: 0 });
    let e = step(e,Action::Certify { owner: 0,snapshot: 0 });
    assert(fast(last(e),last(e).requests[0]));
    range_authority(constants(),last(e),0);
    assert(exact(constants(),last(e).placement,last(e).requests[0]));
    let e = placement(e,p::Action::Open { txn: 0 });
    assert(can_admit(constants(),last(e),0,0,0));
    assert(can_admit(constants(),last(e),0,0,1));
    let e = step(e,Action::Admit { id: 0,txn: 0,key: 0 });
    let e = placement(e,p::Action::Open { txn: 1 });
    let e = placement(e,p::Action::Begin { nonce: 0,src: 0,dst: 1,table: 0,lo: 0,hi: Some(1int) });
    let e = placement(e,p::Action::RequestFreeze);
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Freeze,owner: 0 });
    // Publication is still version zero; equality alone would accept incorrectly.
    assert(last(e).requests[0].snapshot == Some((last(e).placement.directory.len()-1) as nat));
    frozen_rejects_matching_snapshot(constants(),last(e),0,1,0);
    held_lease_still_blocks_drain(constants(),last(e),1,0,0);
    e
}

/// Source and unchanged-range requests are captured BEFORE the migration.
/// No lease is held, so the real drain/copy/seal/retire guards all make progress.
pub proof fn migrated() -> (e: Execution)
    ensures valid(e), last(e).placement.directory.len() == 2,
        last(e).requests.contains_key(0), last(e).requests.contains_key(1),
        last(e).placement.sessions.contains_key(0),
        last(e).requests[0].snapshot == Some(0nat), last(e).requests[1].snapshot == Some(0nat),
        last(e).requests[0].grant == (p::Grant { owner: 0,ownership_version: 0 }),
        last(e).requests[1].grant == (p::Grant { owner: 0,ownership_version: 0 }),
        keys(constants(),last(e).requests[0].scope) == Set::empty().insert(1int),
        keys(constants(),last(e).requests[1].scope) == Set::empty().insert(0int),
        p::replica(last(e).placement,0,0).role is Empty,
        p::replica(last(e).placement,1,0).role is Serving,
        p::replica(last(e).placement,0,1).role is Serving,
        p::replica(last(e).placement,0,1).ownership_version == 0,
        p::directory(last(e).placement)[1] == (p::Grant { owner: 0,ownership_version: 0 }),
        p::directory(last(e).placement)[0] == (p::Grant { owner: 1,ownership_version: 1 }),
        last(e).placement.sessions[0] == (p::Session { held: Map::empty(),resolved: false })
{
    hide(behavior);
    let e = start();
    let e = placement(e,p::Action::Cache { client: 0,snapshot: 0 });
    let scope = Scope::Scan { table: 0,lo: 1,hi: Some(2int) };
    assert(keys(constants(),scope) =~= Set::empty().insert(1int));
    let e = step(e,Action::Capture { id: 0,client: 0,scope,representative: 1 });
    let e = step(e,Action::Capture { id: 1,client: 0,scope: Scope::Point { key: 0 },representative: 0 });
    let e = placement(e,p::Action::Open { txn: 0 });
    assert(p::selected(constants(),0,0,Some(1int)) =~= Set::empty().insert(0int));
    let e = placement(e,p::Action::Begin { nonce: 0,src: 0,dst: 1,table: 0,lo: 0,hi: Some(1int) });
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Start,owner: 1 });
    let e = placement(e,p::Action::RequestFreeze);
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Freeze,owner: 0 });
    let e = placement(e,p::Action::Drain { generation: 1 });
    let e = placement(e,p::Action::Receive { generation: 1,certificate: p::Certificate::Drained });
    let e = placement(e,p::Action::RequestFinal);
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Final,owner: 1 });
    let e = placement(e,p::Action::Capture { generation: 1,round: 1,key: 0 });
    let packet = p::Packet { generation: 1,round: 1,key: 0,cell: p::empty_cell() };
    let e = placement(e,p::Action::DeliverCopy { packet });
    let e = placement(e,p::Action::Seal { generation: 1 });
    let e = placement(e,p::Action::Receive { generation: 1,certificate: p::Certificate::Ready });
    let e = placement(e,p::Action::RequestRetire);
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Retire,owner: 0 });
    let e = placement(e,p::Action::Receive { generation: 1,certificate: p::Certificate::Retired });
    let e = placement(e,p::Action::Commit);
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Commit,owner: 1 });
    let e = placement(e,p::Action::Deliver { generation: 1,command: p::Command::Commit,owner: 0 });
    e
}

pub proof fn old_snapshot_fallback() -> (e: Execution)
    ensures valid(e), last(e).requests[0].snapshot == Some(0nat), last(e).certified[0] == 1,
        !fast(last(e),last(e).requests[0]),
        p::live_lease(last(e).placement,0,1),
        !can_admit(constants(),last(e),1,0,0)
{
    hide(behavior);
    let e = migrated();
    // Derive the universal certification check from the two actual model keys.
    assert(certifiable(constants(),last(e).placement,0,1));
    let e = step(e,Action::Certify { owner: 0,snapshot: 1 });
    assert(!fast(last(e),last(e).requests[0]));
    assert(exact(constants(),last(e).placement,last(e).requests[0]));
    assert(can_admit(constants(),last(e),0,0,1));
    assert(!can_admit(constants(),last(e),1,0,0));
    let e = step(e,Action::Admit { id: 0,txn: 0,key: 1 });
    e
}

pub proof fn absent_certificate_fallback() -> (e: Execution)
    ensures valid(e), last(e).certified == Map::<int,nat>::empty(),
        last(e).requests[0].snapshot == Some(0nat),
        p::live_lease(last(e).placement,0,0)
{
    hide(behavior);
    let e = start();
    let e = placement(e,p::Action::Cache { client: 0,snapshot: 0 });
    let e = step(e,Action::Capture { id: 0,client: 0,scope: Scope::Point { key: 0 },representative: 0 });
    let e = placement(e,p::Action::Open { txn: 0 });
    assert(!fast(last(e),last(e).requests[0]));
    let e = step(e,Action::Admit { id: 0,txn: 0,key: 0 });
    e
}

} // verus!
