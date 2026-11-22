//! Enabled constructive schedules and explicitly disabled bad-order steps.
//! Unchecked apply calls below occur only in named counterexamples; they do not
//! claim reachability in the correct protocol. The induction is in proofs.rs.
use vstd::prelude::*;
use super::*;
use super::proofs::*;

verus! {

pub open spec fn constants_two(value: Option<int>) -> Constants {
    let scope = admission::interval(0,0,None);
    Constants { nodes: Set::empty().insert(0).insert(1), scope, anchor: (0,0,0),
        initial: Grant { owner: 0, version: 0 },
        image: IMap::new(|k: Key| scope.contains(k), |k: Key|
            if k == (0,0,0) { value } else { None }) }
}
pub proof fn initial_two(value: Option<int>) -> (c: Constants)
    ensures c == constants_two(value), constants(c), inv(c,initial(c))
{
    let c = constants_two(value);
    assert(c.image.dom() =~= c.scope);
    assert(c.scope.contains(c.anchor));
    initial_safe(c);
    c
}

/// Finite-support data inside an unbounded ownership/absence scope.
pub open spec fn row_image(c: Constants, value: Option<int>) -> Image {
    IMap::new(|k: Key| c.scope.contains(k), |k: Key|
        if k == c.anchor { value } else { None })
}
pub proof fn row_image_domain(c: Constants, value: Option<int>)
    ensures row_image(c,value).dom() == c.scope
{
    assert(row_image(c,value).dom() =~= c.scope);
}
pub proof fn advance(c: Constants, s: State, a: Action) -> (out: State)
    requires constants(c), inv(c,s), enabled(c,s,a)
    ensures out == apply(c,s,a), inv(c,out)
{
    step_preserves(c,s,a);
    apply(c,s,a)
}
pub open spec fn entered_state(c: Constants, s: State, node: int, id: int, worker: int) -> State {
    State { local: s.local.insert(node,admission::witnesses::enrolled(s.local[node],id,worker)), ..s }
}
pub proof fn enter(c: Constants, s: State, node: int, id: int, worker: int) -> (out: State)
    requires constants(c), inv(c,s), c.nodes.contains(node), !s.local[node].readers.contains_key(id)
    ensures inv(c,out), out == entered_state(c,s,node,id,worker),
        out.local[node].epoch == s.local[node].epoch,
        out.local[node].history == s.local[node].history,
        out.local[node].floor == s.local[node].floor,
        out.local[node].completed == s.local[node].completed,
        out.local[node].readers.contains_key(id),
        out.local[node].readers[id].entered && out.local[node].readers[id].announced,
        !out.local[node].readers[id].finished,
        out.local[node].readers[id].epoch == s.local[node].epoch,
        out.local[node].readers[id].view is Some,
        !s.local[node].caches.contains_key(worker) ==>
            out.local[node].readers[id].view == Some(admission::current_index(s.local[node]))
{
    let local = admission::witnesses::enroll(s.local[node],id,worker);
    let a = advance(c,s,Action::Local { node, action: admission::Action::Probe { id,worker } });
    let b = advance(c,a,Action::Local { node, action: admission::Action::Announce { id } });
    let d = advance(c,b,Action::Local { node, action: admission::Action::Validate { id } });
    let out = advance(c,d,Action::Local { node, action: admission::Action::Capture { id } });
    assert(out.local[node] == local);
    assert(out.local =~= s.local.insert(node,local));
    out
}
pub open spec fn enrolled_state(c: Constants, s: State, id: int, worker: int,
    writes: bool, replacement: Image) -> State {
    apply(c,entered_state(c,s,s.route.owner,id,worker),
        Action::Enroll { node: s.route.owner, id, grant: s.route, writes, replacement })
}
pub proof fn enroll_fresh(c: Constants, s: State, id: int, worker: int,
    writes: bool, replacement: Image) -> (out: State)
    requires constants(c), inv(c,s), current_serving(s), !s.txns.contains_key(id),
        !s.local[s.route.owner].readers.contains_key(id),
        !s.local[s.route.owner].caches.contains_key(worker), replacement.dom() == c.scope
    ensures inv(c,out), out == enrolled_state(c,s,id,worker,writes,replacement)
{
    let e = enter(c,s,s.route.owner,id,worker);
    assert forall|k: Key| c.scope.contains(k)
        implies admission::authorized(e.local[s.route.owner],id,k,s.route) by {}
    advance(c,e,Action::Enroll { node: s.route.owner,id,grant: s.route,writes,replacement })
}
pub open spec fn quiet(c: Constants, s: State) -> bool {
    forall|n: int, id: int| c.nodes.contains(n) && s.local[n].readers.contains_key(id)
        ==> s.local[n].readers[id].finished
}
pub proof fn quiescent_move(c: Constants, s: State, destination: int) -> (out: State)
    requires constants(c), inv(c,s), s.movement is None, quiet(c,s),
        c.nodes.contains(destination), destination != s.route.owner
    ensures inv(c,out), out.movement is None, quiet(c,out), out.logical == s.logical,
        out.route == (Grant { owner: destination, version: s.next_generation }),
        out.next_generation == s.next_generation + 1,
        out.txns == s.txns,
        forall|n: int| c.nodes.contains(n) ==> out.local[n].readers == s.local[n].readers
            && out.local[n].caches == s.local[n].caches
{
    let g = s.next_generation;
    let a = advance(c,s,Action::Prepare { destination, generation: g });
    let b = advance(c,a,Action::Freeze { generation: g });
    let d = advance(c,b,Action::EstablishCut { generation: g });
    assert(admission::drained(d.local[s.route.owner],d.movement.unwrap().cut.unwrap()));
    let e = advance(c,d,Action::Drain { generation: g });
    let f = advance(c,e,Action::Copy { generation: g });
    let h = advance(c,f,Action::Publish { generation: g });
    let i = advance(c,h,Action::Activate { generation: g });
    advance(c,i,Action::Reclaim { generation: g })
}

/// The old writer completes successfully AFTER Freeze and multiple ticks, with
/// the same pin/view even across a logical-engine retry. Cleanup and Exit are
/// distinct: both must precede the barrier. A real destination read observes 7.
pub proof fn old_writer_finishes_after_freeze_and_ticks()
{
    let c = initial_two(None);
    row_image_domain(c,Some(7));
    let w = enroll_fresh(c,initial(c),1,10,true,row_image(c,Some(7)));
    let a = advance(c,w,Action::Prepare { destination: 1, generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    assert(b.local[0].epoch == w.local[0].epoch);
    let d = advance(c,b,Action::Local { node: 0, action: admission::Action::Tick });
    let e = advance(c,d,Action::Local { node: 1, action: admission::Action::Tick });
    let f = advance(c,e,Action::Local { node: 0, action: admission::Action::Tick });
    let g = advance(c,f,Action::EstablishCut { generation: 1 });
    let h = advance(c,g,Action::Local { node: 0, action: admission::Action::Retry { id: 1 } });
    assert(h.local[0].readers[1].epoch == 1);
    assert(h.local[0].readers[1].view == w.local[0].readers[1].view);
    assert(h.local[0].readers[1].attempt == 1);
    assert(h.local[0].readers[1].epoch < h.local[0].epoch);
    assert(h.local[0].epoch == 4 && h.local[1].epoch == 2);
    assert(h.movement.unwrap().cut == Some(4nat));
    pin_covers_effects_and_cleanup(c,h,1);
    admission::proofs::old_announcement_blocks_completion(h.local[0],4,1);
    assert(!enabled(c,h,Action::Drain { generation: 1 }));
    assert(!enabled(c,h,Action::Copy { generation: 1 }));
    let committed = advance(c,h,Action::EngineCommit { id: 1 });
    assert(committed.logical[c.anchor] == Some(7));
    assert(!enabled(c,committed,Action::Local { node: 0, action: admission::Action::Exit { id: 1 } }));
    let cleaned = advance(c,committed,Action::Cleanup { id: 1 });
    assert(!enabled(c,cleaned,Action::Drain { generation: 1 }));
    let exited = advance(c,cleaned,Action::Local { node: 0, action: admission::Action::Exit { id: 1 } });
    let drained = advance(c,exited,Action::Drain { generation: 1 });
    let copied = advance(c,drained,Action::Copy { generation: 1 });
    let published = advance(c,copied,Action::Publish { generation: 1 });
    let active = advance(c,published,Action::Activate { generation: 1 });
    let done = advance(c,active,Action::Reclaim { generation: 1 });
    let reader = enroll_fresh(c,done,2,20,false,done.logical);
    engine_success_matches_logical(c,reader,2);
    let read = advance(c,reader,Action::EngineCommit { id: 2 });
    assert(read.txns[2].observed[c.anchor] == Some(7));
    every_scope_coordinate_agrees(c,read);
}

/// Freeze does not magically invalidate a worker cache. Before Tick a new
/// logical transaction may legitimately use the old Serving object; it too is
/// drained, and its successful insertion is included in the exact final image.
pub proof fn cached_enrollment_between_publication_and_cut()
{
    let c = initial_two(None);
    row_image_domain(c,Some(11));
    let warm = enter(c,initial(c),0,9,10);
    let idle = advance(c,warm,Action::Local { node: 0, action: admission::Action::Exit { id: 9 } });
    let a = advance(c,idle,Action::Prepare { destination: 1, generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let old = enter(c,b,0,1,10);
    assert(admission::current_view(old.local[0]) == IMap::<Key,Grant>::empty());
    assert(old.local[0].readers[1].view == Some(0nat));
    let writer = advance(c,old,Action::Enroll { node: 0,id: 1,grant: c.initial,
        writes: true,replacement: row_image(c,Some(11)) });
    let cut = advance(c,writer,Action::EstablishCut { generation: 1 });
    assert(!enabled(c,cut,Action::Drain { generation: 1 }));
    let committed = advance(c,cut,Action::EngineCommit { id: 1 });
    let cleaned = advance(c,committed,Action::Cleanup { id: 1 });
    let exited = advance(c,cleaned,Action::Local { node: 0, action: admission::Action::Exit { id: 1 } });
    let drained = advance(c,exited,Action::Drain { generation: 1 });
    let copied = advance(c,drained,Action::Copy { generation: 1 });
    assert(copied.data[1][c.anchor] == Some(11));
}

/// An unrelated/empty reader in the same process conservatively delays the
/// cut, although it has no Txn and causes no source effect.
pub proof fn unrelated_reader_conservatively_delays_cut()
{
    let c = initial_two(Some(1));
    let reader = enter(c,initial(c),0,99,99);
    assert(!reader.txns.contains_key(99));
    let a = advance(c,reader,Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    assert(!enabled(c,d,Action::Drain { generation: 1 }));
    let e = advance(c,d,Action::Local { node: 0,action: admission::Action::Exit { id: 99 } });
    let f = advance(c,e,Action::Drain { generation: 1 });
    let g = advance(c,f,Action::Copy { generation: 1 });
    assert(g.data[1] == c.image);
}

pub proof fn late_rpc_and_late_announcement_cannot_reenroll()
{
    let c = initial_two(Some(1));
    row_image_domain(c,Some(8));
    let probe = advance(c,initial(c),Action::Local { node: 0,
        action: admission::Action::Probe { id: 99,worker: 99 } });
    let a = advance(c,probe,Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    let e = advance(c,d,Action::Drain { generation: 1 });
    let late = advance(c,e,Action::Local { node: 0,action: admission::Action::Announce { id: 99 } });
    assert(!enabled(c,late,Action::Local { node: 0,action: admission::Action::Validate { id: 99 } }));
    let fresh = enter(c,late,0,1,10);
    late_old_request_rejected(c,fresh,1,true,row_image(c,Some(8)));
    assert(fresh.local[0].readers[1].epoch == fresh.local[0].epoch);
    assert(!enabled(c,fresh,Action::Enroll { node: 0,id: 1,grant: c.initial,
        writes: true,replacement: row_image(c,Some(8)) }));
}

pub proof fn return_to_same_owner_is_not_aba()
{
    let c = initial_two(Some(3));
    let a = quiescent_move(c,initial(c),1);
    let b = quiescent_move(c,a,0);
    assert(b.route.owner == 0 && b.route.version == 2);
    assert(b.logical == c.image && b.data[0] == c.image);
    let next = advance(c,b,Action::Prepare { destination: 1,generation: 3 });
    stale_control_rejected(c,next,1);
    assert(!enabled(c,next,Action::Freeze { generation: 1 }));
    let bad_stale_control = apply(c,next,Action::Freeze { generation: 1 });
    assert(bad_stale_control.movement.unwrap().generation == 3);
    assert(bad_stale_control.movement.unwrap().phase is Frozen);
    let fresh = enter(c,b,0,70,70);
    assert(!enabled(c,fresh,Action::Enroll { node: 0,id: 70,grant: c.initial,
        writes: false,replacement: c.image }));
    let enrolled = advance(c,fresh,Action::Enroll { node: 0,id: 70,grant: b.route,
        writes: false,replacement: c.image });
    let read = advance(c,enrolled,Action::EngineCommit { id: 70 });
    assert(read.txns[70].observed == c.image);
}

/// Constructively repeat A->B->A an arbitrary natural number of times. This
/// witness and the behavior induction have no fixed generation/ID horizon.
pub proof fn arbitrary_round_trips(c: Constants, s: State, rounds: nat) -> (out: State)
    requires constants(c), inv(c,s), quiet(c,s), s.movement is None,
        c.nodes.contains(0), c.nodes.contains(1), s.route.owner == 0
    ensures inv(c,out), quiet(c,out), out.movement is None, out.route.owner == 0,
        out.next_generation == s.next_generation + 2 * rounds,
        out.logical == s.logical
    decreases rounds
{
    if rounds == 0 { s }
    else {
        let a = quiescent_move(c,s,1);
        let b = quiescent_move(c,a,0);
        arbitrary_round_trips(c,b,(rounds-1) as nat)
    }
}

/// The correct Copy is disabled. Running it anyway before the old writer
/// settles demonstrably loses a committed insertion into an absent scope.
pub proof fn missing_drain_loses_old_authorized_insert()
{
    let c = initial_two(None);
    row_image_domain(c,Some(9));
    let w = enroll_fresh(c,initial(c),1,10,true,row_image(c,Some(9)));
    let a = advance(c,w,Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    assert(!enabled(c,d,Action::Copy { generation: 1 }));
    let bad_copy = apply(c,d,Action::Copy { generation: 1 });
    assert(enabled(c,bad_copy,Action::EngineCommit { id: 1 }));
    let committed = apply(c,bad_copy,Action::EngineCommit { id: 1 });
    let published = apply(c,committed,Action::Publish { generation: 1 });
    assert(published.logical[c.anchor] == Some(9));
    assert(published.data[published.route.owner][c.anchor] is None);
}

/// Premature quiescence must not be hidden by a fresh authority/pin check in
/// EngineCommit: the old successful effect remains enabled after the bad Exit.
pub proof fn premature_exit_allows_effect_after_drain_and_copy()
{
    let c = initial_two(None);
    row_image_domain(c,Some(9));
    let w = enroll_fresh(c,initial(c),1,10,true,row_image(c,Some(9)));
    let a = advance(c,w,Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    pin_covers_effects_and_cleanup(c,d,1);
    let bad_exit = apply(c,d,Action::Local { node: 0,action: admission::Action::Exit { id: 1 } });
    assert(enabled(c,bad_exit,Action::Drain { generation: 1 }));
    let drained = apply(c,bad_exit,Action::Drain { generation: 1 });
    assert(enabled(c,drained,Action::Copy { generation: 1 }));
    let copied = apply(c,drained,Action::Copy { generation: 1 });
    assert(enabled(c,copied,Action::EngineCommit { id: 1 }));
    let committed = apply(c,copied,Action::EngineCommit { id: 1 });
    let published = apply(c,committed,Action::Publish { generation: 1 });
    assert(published.logical[c.anchor] == Some(9));
    assert(published.data[1][c.anchor] is None);
}

/// Even after the logical effect is settled, cleanup still owns the storage
/// lifetime. Releasing that protection early admits reclamation underneath it.
pub proof fn premature_exit_before_cleanup_allows_reclaim()
{
    let c = initial_two(None);
    row_image_domain(c,Some(9));
    let w = enroll_fresh(c,initial(c),1,10,true,row_image(c,Some(9)));
    let committed = advance(c,w,Action::EngineCommit { id: 1 });
    let a = advance(c,committed,Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    pin_covers_effects_and_cleanup(c,d,1);
    let bad_exit = apply(c,d,Action::Local { node: 0,action: admission::Action::Exit { id: 1 } });
    assert(enabled(c,bad_exit,Action::Drain { generation: 1 }));
    let drained = apply(c,bad_exit,Action::Drain { generation: 1 });
    let copied = apply(c,drained,Action::Copy { generation: 1 });
    let published = apply(c,copied,Action::Publish { generation: 1 });
    let active = apply(c,published,Action::Activate { generation: 1 });
    assert(enabled(c,active,Action::Reclaim { generation: 1 }));
    let reclaimed = apply(c,active,Action::Reclaim { generation: 1 });
    assert(!reclaimed.txns[1].cleaned && reclaimed.data[0] == blank(c));
    assert(enabled(c,reclaimed,Action::Cleanup { id: 1 }));
}

pub proof fn late_enrollment_bypass_loses_effect()
{
    let c = initial_two(None);
    row_image_domain(c,Some(9));
    let a = advance(c,initial(c),Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    let e = advance(c,d,Action::Drain { generation: 1 });
    let f = advance(c,e,Action::Copy { generation: 1 });
    let fresh = enter(c,f,0,1,10);
    let request = Action::Enroll { node: 0,id: 1,grant: c.initial,
        writes: true,replacement: row_image(c,Some(9)) };
    assert(!enabled(c,fresh,request));
    let forged = apply(c,fresh,request);
    assert(enabled(c,forged,Action::EngineCommit { id: 1 }));
    let committed = apply(c,forged,Action::EngineCommit { id: 1 });
    let published = apply(c,committed,Action::Publish { generation: 1 });
    assert(published.logical[c.anchor] == Some(9) && published.data[1][c.anchor] is None);
}

/// Cancellation keeps staging bytes. A subsequent deletion and migration
/// therefore distinguish exact scope replacement from copying only live rows.
pub proof fn cancellation_and_absence_remove_destination_only_bytes()
{
    let c = initial_two(Some(3));
    let a = advance(c,initial(c),Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::EstablishCut { generation: 1 });
    let e = advance(c,d,Action::Drain { generation: 1 });
    let f = advance(c,e,Action::Copy { generation: 1 });
    let cancelled = advance(c,f,Action::Cancel { generation: 1 });
    assert(blank(c).dom() =~= c.scope);
    let w = enroll_fresh(c,cancelled,1,10,true,blank(c));
    let committed = advance(c,w,Action::EngineCommit { id: 1 });
    let cleaned = advance(c,committed,Action::Cleanup { id: 1 });
    let exited = advance(c,cleaned,Action::Local { node: 0,action: admission::Action::Exit { id: 1 } });
    let p = advance(c,exited,Action::Prepare { destination: 1,generation: 2 });
    let frozen = advance(c,p,Action::Freeze { generation: 2 });
    let cut = advance(c,frozen,Action::EstablishCut { generation: 2 });
    let drained = advance(c,cut,Action::Drain { generation: 2 });
    assert(drained.data[0][c.anchor] is None && drained.data[1][c.anchor] == Some(3));
    let copied = advance(c,drained,Action::Copy { generation: 2 });
    assert(copied.data[1][c.anchor] is None);
    let published = advance(c,copied,Action::Publish { generation: 2 });
    assert(published.data[published.route.owner] == blank(c));
    // Checked bad implementation: an upsert-only row walk leaves the old 3.
    let upserts = IMap::new(|k: Key| c.scope.contains(k), |k: Key|
        if drained.data[0][k] is Some { drained.data[0][k] } else { drained.data[1][k] });
    let bad_copy = State { data: copied.data.insert(1,upserts), ..copied };
    let bad_publication = apply(c,bad_copy,Action::Publish { generation: 2 });
    assert(bad_publication.logical[c.anchor] is None);
    assert(bad_publication.data[1][c.anchor] == Some(3));
}

/// A generic request certificate cannot be replaced by a first-row check.
/// This counterexample concerns the Enroll API, not a reachable uniform scope.
pub proof fn first_row_does_not_certify_absent_aliases()
{
    let c = constants_two(None);
    let one = IMap::empty().insert(c.anchor,c.initial);
    let local = admission::initial(one);
    let a = admission::apply(local,admission::Action::Probe { id: 1,worker: 10 });
    let b = admission::apply(a,admission::Action::Announce { id: 1 });
    let d = admission::apply(b,admission::Action::Validate { id: 1 });
    let e = admission::apply(d,admission::Action::Capture { id: 1 });
    let s = State { local: initial(c).local.insert(0,e), ..initial(c) };
    let alias: Key = (0,0,1);
    assert(admission::authorized(e,1,c.anchor,c.initial));
    assert(c.scope.contains(alias) && !admission::authorized(e,1,alias,c.initial));
    assert(!scope_authorized(c,s,0,1,c.initial));
    assert(!enabled(c,s,Action::Enroll { node: 0,id: 1,grant: c.initial,writes: false,replacement: c.image }));
}

// Keep the constructive schedule compositional: each phase returns the facts
// needed by the next phase, rather than expanding every earlier state update.
spec fn sampled_packets(c: Constants) -> Map<int,Packet> {
    let old = Packet { generation: 1, round: 1, destination: 1,
        key: c.anchor, value: Some(1) };
    Map::empty().insert(100,old).insert(101,old)
        .insert(102,Packet { value: Some(2), ..old })
}
spec fn updated_precopy(c: Constants, s: State) -> bool {
    s.route == c.initial && s.next_generation == 2
        && s.movement == Some(Move { source: c.initial, destination: 1,
            generation: 1, phase: Phase::Prepared, frozen_index: None, cut: None,
            round: 1, precopy_open: true, precopy_joined: false })
        && s.logical == row_image(c,Some(2)) && current_serving(s)
        && s.local[0].epoch == 1 && quiet(c,s)
        && !s.txns.contains_key(2) && !s.local[0].readers.contains_key(2)
        && !s.local[0].caches.contains_key(20)
        && s.packets == sampled_packets(c) && s.effects.is_empty()
}
proof fn observe_concurrent_update(c: Constants) -> (out: State)
    requires c == constants_two(Some(1)), constants(c), inv(c,initial(c))
    ensures inv(c,out), updated_precopy(c,out)
{
    row_image_domain(c,Some(2));
    let a = advance(c,initial(c),Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::StartPrecopy { generation: 1 });
    let delayed = advance(c,b,Action::Observe { generation: 1,id: 100,key: c.anchor });
    let old = advance(c,delayed,Action::Observe { generation: 1,id: 101,key: c.anchor });
    let writer = enroll_fresh(c,old,1,10,true,row_image(c,Some(2)));
    let updated = advance(c,writer,Action::EngineCommit { id: 1 });
    let clean = advance(c,updated,Action::Cleanup { id: 1 });
    let idle = advance(c,clean,Action::Local { node: 0,action: admission::Action::Exit { id: 1 } });
    let newer = advance(c,idle,Action::Observe { generation: 1,id: 102,key: c.anchor });
    assert(newer.packets[101].value == Some(1) && newer.packets[102].value == Some(2));
    assert(newer.packets =~= sampled_packets(c));
    newer
}

proof fn delete_before_drain(c: Constants, newer: State) -> (out: State)
    requires c == constants_two(Some(1)), constants(c), inv(c,newer),
        updated_precopy(c,newer)
    ensures inv(c,out), out.route == c.initial, out.next_generation == 2,
        out.logical == row_image(c,None), current_move(out,1),
        out.movement.unwrap().source == c.initial,
        out.movement.unwrap().destination == 1,
        out.movement.unwrap().phase is Frozen,
        out.movement.unwrap().cut == Some(2nat),
        out.movement.unwrap().round == 1,
        out.movement.unwrap().precopy_open,
        !out.movement.unwrap().precopy_joined,
        out.local[0].completed == 2,
        out.packets == sampled_packets(c), out.effects.is_empty()
{
    row_image_domain(c,None);
    let deleter = enroll_fresh(c,newer,2,20,true,row_image(c,None));
    let frozen = advance(c,deleter,Action::Freeze { generation: 1 });
    let cut = advance(c,frozen,Action::EstablishCut { generation: 1 });
    assert(cut.local[0].readers[2].epoch == 1);
    assert(cut.movement.unwrap().cut == Some(2nat));
    pin_covers_effects_and_cleanup(c,cut,2);
    admission::proofs::old_announcement_blocks_completion(cut.local[0],2,2);
    assert(!enabled(c,cut,Action::Drain { generation: 1 }));
    let deleted = advance(c,cut,Action::EngineCommit { id: 2 });
    let cleaned = advance(c,deleted,Action::Cleanup { id: 2 });
    let exited = advance(c,cleaned,Action::Local { node: 0,action: admission::Action::Exit { id: 2 } });
    let drained = advance(c,exited,Action::Drain { generation: 1 });
    drained
}

/// Background reads interleave with an update and a delete. Packets are stale,
/// accepted effects can finish after closure, but neither final nor cancellation
/// may overtake them. Each phase constructs only enabled inductive steps.
pub proof fn background_precopy_corrects_concurrent_update_and_delete()
{
    let c = initial_two(Some(1));
    let newer = observe_concurrent_update(c);
    let drained = delete_before_drain(c,newer);
    let accepted = advance(c,drained,Action::AcceptPrecopy { id: 101 });
    let stale = advance(c,accepted,Action::FinishPrecopy { id: 101 });
    assert(stale.data[1][c.anchor] == Some(1) && stale.logical[c.anchor] is None);
    let outstanding = advance(c,stale,Action::AcceptPrecopy { id: 102 });
    let closed = advance(c,outstanding,Action::ClosePrecopy { generation: 1 });
    assert(!enabled(c,closed,Action::AcceptPrecopy { id: 100 }));
    assert(!enabled(c,closed,Action::JoinPrecopy { generation: 1 }));
    assert(!enabled(c,closed,Action::Copy { generation: 1 }));
    assert(!enabled(c,closed,Action::Cancel { generation: 1 }));

    // Counterexample: a final operation that overtakes a previously accepted
    // effect can be overwritten even though new packet admission is fenced.
    let bad_final = apply(c,closed,Action::Copy { generation: 1 });
    let late_effect = apply(c,bad_final,Action::FinishPrecopy { id: 102 });
    assert(late_effect.logical[c.anchor] is None);
    assert(late_effect.data[1][c.anchor] == Some(2));
    assert(!inv(c,late_effect));

    let finished = advance(c,closed,Action::FinishPrecopy { id: 102 });
    let joined = advance(c,finished,Action::JoinPrecopy { generation: 1 });
    assert(joined.data[1][c.anchor] == Some(2));
    precopy::final_requires_join(c,joined,1);
    precopy::final_deletes_stale_private_row(c,joined,1,c.anchor);
    let exact = advance(c,joined,Action::Copy { generation: 1 });
    assert(exact.data[1] == exact.logical && exact.data[1][c.anchor] is None);
    precopy::old_packet_rejected(c,exact,100);
    let published = advance(c,exact,Action::Publish { generation: 1 });
    // Counterexample: bypassing the destination fence after final lets an old
    // packet overwrite the canonical image. This is deliberately unchecked.
    let bad_accept = apply(c,published,Action::AcceptPrecopy { id: 100 });
    let bad_packet = apply(c,bad_accept,Action::FinishPrecopy { id: 100 });
    assert(bad_packet.data[bad_packet.route.owner][c.anchor] == Some(1));
    assert(bad_packet.logical[c.anchor] is None && !inv(c,bad_packet));

    let active = advance(c,published,Action::Activate { generation: 1 });
    let done = advance(c,active,Action::Reclaim { generation: 1 });
    let returning = advance(c,done,Action::Prepare { destination: 0,generation: 2 });
    let next = advance(c,returning,Action::StartPrecopy { generation: 2 });
    precopy::old_packet_rejected(c,next,100);
    assert(next.data[next.route.owner] == next.logical);
}

pub proof fn delayed_packet_cannot_cross_precopy_round()
{
    let c = initial_two(Some(5));
    let a = advance(c,initial(c),Action::Prepare { destination: 1,generation: 1 });
    let b = advance(c,a,Action::StartPrecopy { generation: 1 });
    let p = advance(c,b,Action::Observe { generation: 1,id: 100,key: c.anchor });
    let closed = advance(c,p,Action::ClosePrecopy { generation: 1 });
    let joined = advance(c,closed,Action::JoinPrecopy { generation: 1 });
    let next = advance(c,joined,Action::StartPrecopy { generation: 1 });
    assert(next.packets[100].round == 1 && next.movement.unwrap().round == 2);
    assert(next.movement.unwrap().precopy_open);
    precopy::old_packet_rejected(c,next,100);
    let close_again = advance(c,next,Action::ClosePrecopy { generation: 1 });
    let join_again = advance(c,close_again,Action::JoinPrecopy { generation: 1 });
    let cancelled = advance(c,join_again,Action::Cancel { generation: 1 });
    let fresh = advance(c,cancelled,Action::Prepare { destination: 1,generation: 2 });
    let fresh_round = advance(c,fresh,Action::StartPrecopy { generation: 2 });
    // Same destination and round: only the generation distinguishes this packet.
    assert(fresh_round.packets[100].round == fresh_round.movement.unwrap().round);
    assert(fresh_round.packets[100].destination == fresh_round.movement.unwrap().destination);
    precopy::old_packet_rejected(c,fresh_round,100);
}

} // verus!
