//! Constructive schedules exercise the proposed protocol, not deployed code.
use vstd::prelude::*;
use super::*;
use super::proofs::*;

verus! {

pub open spec fn constants_two(value: Option<int>) -> Constants {
    Constants { nodes: Set::empty().insert(0).insert(1),
        initial: Grant { owner: 0, version: 0 }, value }
}
pub proof fn advance(c: Constants, s: State, a: Action) -> (out: State)
    requires constants(c), inv(c,s), enabled(c,s,a)
    ensures out == apply(s,a), inv(c,out)
{
    step_preserves(c,s,a);
    apply(s,a)
}
pub proof fn prepared_writer(c: Constants, s: State, id: int, value: Option<int>) -> (out: State)
    requires constants(c), inv(c,s), !s.txns.contains_key(id), !s.locks.contains_key(s.route.owner),
        s.serving[s.route.owner] == Some(s.route.version)
    ensures inv(c,out), out.txns.contains_key(id), pending(out.txns[id]), out.txns[id].read_ok,
        out.txns[id].value == value, out.txns[id].grant == s.route,
        out.route == s.route, out.data == s.data, out.logical == s.logical,
        out.next_generation == s.next_generation, out.movement == s.movement,
        out.txns.dom() == s.txns.dom().insert(id),
        forall|other: int| other != id ==> out.txns.contains_key(other) == s.txns.contains_key(other)
            && (s.txns.contains_key(other) ==> out.txns[other] == s.txns[other])
{
    let a = advance(c,s,Action::Capture { id,writes: true,value });
    let b = advance(c,a,Action::Lock { id });
    let d = advance(c,b,Action::Seal { id });
    let e = advance(c,d,Action::ValidateAuthority { id });
    advance(c,e,Action::ValidateData { id })
}

/// A locked insert into an initially absent coordinate cannot be lost. A live
/// read-only transaction does NOT block handoff; it blocks only reclamation.
pub proof fn insertion_and_reader_grace()
{
    let c = constants_two(None);
    initial_safe(c);
    let s0 = initial(c);
    let s1 = advance(c,s0,Action::Capture { id: 2,writes: false,value: None });
    let s2 = advance(c,s1,Action::Observe { id: 2 });
    let reader_sealed = advance(c,s2,Action::Seal { id: 2 });
    let reader_checked = advance(c,reader_sealed,Action::ValidateData { id: 2 });
    let s3 = prepared_writer(c,reader_checked,1,Some(7));
    let s4 = advance(c,s3,Action::PrepareMove { destination: 1, generation: 1 });
    let s5 = advance(c,s4,Action::Freeze { generation: 1 });
    assert(s5.data[0].value is None);
    pending_writer_blocks_copy(c,s5,1);
    let s6 = advance(c,s5,Action::Install { id: 1 });
    let s7 = advance(c,s6,Action::Release { id: 1 });
    let s8 = advance(c,s7,Action::Copy { generation: 1 });
    let s9 = advance(c,s8,Action::Retire { generation: 1 });
    let s10 = advance(c,s9,Action::Publish { generation: 1 });
    let s11 = advance(c,s10,Action::Activate { generation: 1 });
    assert(s11.route.owner == 1 && s11.data[1].value == Some(7));
    assert(!s11.txns[2].finished);
    assert(s11.txns[2].sealed && s11.txns[2].read_ok && !s11.txns[2].authority_ok);
    assert(!s11.txns[1].finished && !s11.txns[1].locked && s11.txns[1].installed);
    lifetime_prevents_reclaim(c,s11,2);
    assert(!enabled(c,s11,Action::ValidateAuthority { id: 2 }));
    let s12 = advance(c,s11,Action::Abort { id: 2 });
    let s13 = advance(c,s12,Action::Finish { id: 2 });
    let writer_cleaned = advance(c,s13,Action::Finish { id: 1 });
    let s14 = advance(c,writer_cleaned,Action::Reclaim { generation: 1 });
    assert(!s14.available.contains(0) && s14.data[1].value == Some(7));
}

pub proof fn move_without_pins(c: Constants, s: State, destination: int) -> (out: State)
    requires constants(c), inv(c,s), s.movement is None,
        c.nodes.contains(destination), destination != s.route.owner,
        forall|id: int| s.txns.contains_key(id) ==> s.txns[id].finished
    ensures inv(c,out), out.movement is None, out.txns == s.txns,
        out.route == (Grant { owner: destination, version: s.next_generation }),
        out.next_generation == s.next_generation + 1, out.logical == s.logical
{
    let g = s.next_generation;
    let a = advance(c,s,Action::PrepareMove { destination, generation: g });
    let b = advance(c,a,Action::Freeze { generation: g });
    pins_clear_implies_unlocked(c,b,s.route.owner);
    let d = advance(c,b,Action::Copy { generation: g });
    let e = advance(c,d,Action::Retire { generation: g });
    let f = advance(c,e,Action::Publish { generation: g });
    let h = advance(c,f,Action::Activate { generation: g });
    advance(c,h,Action::Reclaim { generation: g })
}

pub proof fn return_to_same_owner_is_not_aba()
{
    let c = constants_two(Some(3));
    initial_safe(c);
    let a = move_without_pins(c,initial(c),1);
    let b = move_without_pins(c,a,0);
    assert(b.route.owner == c.initial.owner && b.route.version == 2);
    assert(b.serving[0] != Some(c.initial.version));
    assert(b.logical.value == Some(3));
    stale_control_rejected(c,b,1);
}

/// Cancelled staging can retain destination-only bytes. A later exact copy of
/// source absence must delete them, rather than only upsert returned rows.
pub proof fn delete_removes_destination_only_value()
{
    let c = constants_two(Some(3));
    initial_safe(c);
    let a = advance(c,initial(c),Action::PrepareMove { destination: 1, generation: 1 });
    let b = advance(c,a,Action::Freeze { generation: 1 });
    let d = advance(c,b,Action::Copy { generation: 1 });
    let e = advance(c,d,Action::Cancel { generation: 1 });
    let f = prepared_writer(c,e,1,None);
    let g = advance(c,f,Action::Install { id: 1 });
    let h = advance(c,g,Action::Finish { id: 1 });
    let i = advance(c,h,Action::PrepareMove { destination: 1, generation: 2 });
    assert(i.data[0].value is None && i.data[1].value == Some(3));
    let j = advance(c,i,Action::Freeze { generation: 2 });
    let k = advance(c,j,Action::Copy { generation: 2 });
    let l = advance(c,k,Action::Retire { generation: 2 });
    let m = advance(c,l,Action::Publish { generation: 2 });
    assert(m.data[1].value is None && m.logical.value is None);
}

/// Checked counterexample to weakening Copy's intent exclusion. The offending
/// copy is explicitly NOT enabled in the correct algorithm.
pub proof fn ignoring_absent_intent_loses_committed_insert()
{
    let c = constants_two(None);
    initial_safe(c);
    let a = prepared_writer(c,initial(c),1,Some(9));
    let b = advance(c,a,Action::PrepareMove { destination: 1, generation: 1 });
    let d = advance(c,b,Action::Freeze { generation: 1 });
    pending_writer_blocks_copy(c,d,1);
    assert(!enabled(c,d,Action::Copy { generation: 1 }));
    let bad_copy = apply(d,Action::Copy { generation: 1 });
    let installed = apply(bad_copy,Action::Install { id: 1 });
    let retired = apply(installed,Action::Retire { generation: 1 });
    let published = apply(retired,Action::Publish { generation: 1 });
    assert(published.logical.value == Some(9));
    assert(published.data[published.route.owner].value is None);
}

/// Routing is unchanged while final copy runs. Locking and validating ONLY
/// that routing publication afterwards permits a lost update.
pub proof fn unchanged_directory_is_insufficient()
{
    let c = constants_two(Some(1));
    initial_safe(c);
    let a = advance(c,initial(c),Action::Capture { id: 1,writes: true,value: Some(2) });
    let b = advance(c,a,Action::PrepareMove { destination: 1, generation: 1 });
    let d = advance(c,b,Action::Freeze { generation: 1 });
    let e = advance(c,d,Action::Copy { generation: 1 });
    let f = advance(c,e,Action::Lock { id: 1 });
    let g = advance(c,f,Action::Seal { id: 1 });
    assert(g.route == c.initial);
    assert(!enabled(c,g,Action::ValidateAuthority { id: 1 }));
    let bad_validation = apply(g,Action::ValidateAuthority { id: 1 });
    let checked = apply(bad_validation,Action::ValidateData { id: 1 });
    let installed = apply(checked,Action::Install { id: 1 });
    let retired = apply(installed,Action::Retire { generation: 1 });
    let published = apply(retired,Action::Publish { generation: 1 });
    assert(published.logical.value == Some(2));
    assert(published.data[published.route.owner].value == Some(1));
}

/// An unrelated range can change the participant epoch without revoking this
/// footprint. Exact fallback still permits a real write, not merely admission.
pub proof fn unrelated_epoch_change_falls_back()
{
    let c = constants_two(Some(5));
    initial_safe(c);
    let a = advance(c,initial(c),Action::Capture { id: 1,writes: true,value: Some(9) });
    let b = advance(c,a,Action::Observe { id: 1 });
    let d = advance(c,b,Action::Lock { id: 1 });
    let e = advance(c,d,Action::Seal { id: 1 });
    let f = advance(c,e,Action::EpochBump { owner: 0 });
    assert(f.txns[1].epoch != f.epochs[0]);
    let g = advance(c,f,Action::ValidateAuthority { id: 1 });
    let h = advance(c,g,Action::ValidateData { id: 1 });
    let i = advance(c,h,Action::Install { id: 1 });
    let j = advance(c,i,Action::Finish { id: 1 });
    assert(j.logical.value == Some(9) && j.txns[1].finished);
}

/// A fully validated reader can finish after handoff and a newer destination
/// write. Its old observation is retained; only cleanup waits for its lifetime.
pub proof fn validated_reader_finishes_after_handoff()
{
    let c = constants_two(Some(5));
    initial_safe(c);
    let a = advance(c,initial(c),Action::Capture { id: 1,writes: false,value: None });
    let b = advance(c,a,Action::Observe { id: 1 });
    let d = advance(c,b,Action::Seal { id: 1 });
    let e = advance(c,d,Action::ValidateAuthority { id: 1 });
    let f = advance(c,e,Action::ValidateData { id: 1 });
    // Gaps in the global generation sequence are legal in a row projection.
    let g = advance(c,f,Action::PrepareMove { destination: 1, generation: 8 });
    let h = advance(c,g,Action::Freeze { generation: 8 });
    let i = advance(c,h,Action::Copy { generation: 8 });
    let j = advance(c,i,Action::Retire { generation: 8 });
    let k = advance(c,j,Action::Publish { generation: 8 });
    let l = advance(c,k,Action::Activate { generation: 8 });
    let m = prepared_writer(c,l,2,Some(9));
    let n = advance(c,m,Action::Install { id: 2 });
    let p = advance(c,n,Action::Finish { id: 2 });
    assert(p.logical.value == Some(9));
    assert(p.txns[1].observed.unwrap().value == Some(5));
    lifetime_prevents_reclaim(c,p,1);
    let q = advance(c,p,Action::Finish { id: 1 });
    let r = advance(c,q,Action::Reclaim { generation: 8 });
    assert(r.txns[1].finished && !r.txns[1].aborted);
    assert(r.data[1].value == Some(9) && !r.available.contains(0));
}

} // verus!
