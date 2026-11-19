//! Inductive native invariants used by the master/byte observation coupling.
//! These are derived from constructor and handler field-effect contracts; they
//! are not environmental promises about a successful migration.
use vstd::prelude::*;
use crate::migration as n;
use crate::migration_refinement as r;
use crate::types::{TxnId, KeyRange, Status};

verus! {
pub open spec fn table(v: n::Native,slot: int) -> u64 { v.snapshots[v.current[slot] as int].table }
pub open spec fn routing(v: n::Native) -> bool {
    (forall|i: int,j: int| 0 <= i < j < v.current.len() ==> table(v,i) != table(v,j))
    && (forall|i: int| 0 <= i < v.records.len() ==> {
        let rec = #[trigger] v.records[i];
        r::prepared(v,i) && table(v,rec.slot as int) == rec.plan.range.table
        && v.nodes.contains(rec.plan.source) && v.nodes.contains(rec.plan.destination)
        && rec.plan.source != rec.plan.destination
    })
    && match v.active {
        None => true,
        Some(i) => (v.current[v.records[i as int].slot as int] ==
            if v.controls[i as int].phase is Committed { v.records[i as int].proposal } else { v.records[i as int].previous })
            && if v.controls[i as int].phase is Committed { v.published_version == v.records[i as int].plan.generation }
                else { v.published_version < v.records[i as int].plan.generation },
    }
}
pub proof fn initialized(v: n::Native)
    requires n::bootstrap(v.nodes,v.snapshots), v.records.len() == 0, v.active is None,
        v.current.len() == v.snapshots.len(), forall|i: int| 0 <= i < v.current.len() ==> v.current[i] == i,
    ensures routing(v),
{}
pub proof fn begin_preserves(b: n::Native,z: n::Native,id: TxnId,src: u32,dst: u32,range: KeyRange,g: u64)
    requires n::layout(b), routing(b), n::begin_effect(b,z,id,src,dst,range,Ok(g)),
    ensures routing(z), z.nodes == b.nodes, z.current.len() == b.current.len(),
        forall|slot: int| 0 <= slot < b.current.len() ==> table(z,slot) == table(b,slot),
{
    let i = b.records.len() as int;
    r::begin_prepares(b,z,id,src,dst,range,g);
    assert forall|slot: int| 0 <= slot < b.current.len() implies table(z,slot) == table(b,slot) by {
        assert(b.current[slot] < b.snapshots.len());
    }
    assert forall|j: int| 0 <= j < z.records.len() implies r::prepared(z,j)
        && table(z,z.records[j].slot as int) == z.records[j].plan.range.table
        && z.nodes.contains(z.records[j].plan.source) && z.nodes.contains(z.records[j].plan.destination)
        && z.records[j].plan.source != z.records[j].plan.destination by {
        if j < i {
            r::prepared_after_begin(b,z,j,id,src,dst,range,g);
            assert(table(z,z.records[j].slot as int) == table(b,b.records[j].slot as int));
        }
    }
}
pub proof fn request_preserves(b: n::Native,z: n::Native,request: n::Request,status: Status)
    requires n::layout(b), routing(b), n::transition_effect(b,z,request,status),
    ensures routing(z), z.nodes == b.nodes, z.current.len() == b.current.len(),
        forall|slot: int| 0 <= slot < b.current.len() ==> table(z,slot) == table(b,slot),
{
    assert forall|slot: int| 0 <= slot < b.current.len() implies table(z,slot) == table(b,slot) by {
        if status == Status::Ok && request is Commit {
            let i = b.active.unwrap() as int;
            assert(r::prepared(b,i));
            if slot == b.records[i].slot { assert(table(b,slot) == b.records[i].plan.range.table); }
        }
    }
    assert forall|j: int| 0 <= j < z.records.len() implies r::prepared(z,j)
        && table(z,z.records[j].slot as int) == z.records[j].plan.range.table
        && z.nodes.contains(z.records[j].plan.source) && z.nodes.contains(z.records[j].plan.destination)
        && z.records[j].plan.source != z.records[j].plan.destination by {
        r::prepared_retained(b,z,j);
        assert(table(z,z.records[j].slot as int) == table(b,b.records[j].slot as int));
    }
    if let Some(i) = b.active {
        assert(b.current[b.records[i as int].slot as int] ==
            if b.controls[i as int].phase is Committed { b.records[i as int].proposal } else { b.records[i as int].previous });
        match request { n::Request::Freeze => {}, n::Request::Final => {}, n::Request::Retire => {},
            n::Request::Commit => {}, n::Request::Abort => {}, n::Request::Finish => {} }
    }
}
/// Receipt/reply effects change neither a phase nor any immutable routing field.
pub proof fn observation_preserves(b: n::Native,z: n::Native)
    requires n::layout(b), routing(b), z.nodes == b.nodes, z.current == b.current, z.snapshots == b.snapshots,
        z.records == b.records, z.active == b.active, z.controls.len() == b.controls.len(),
        z.published_version == b.published_version,
        forall|i: int| 0 <= i < b.records.len() ==> z.controls[i].phase == b.controls[i].phase,
    ensures routing(z),
{
    assert forall|i: int,j: int| 0 <= i < j < z.current.len() implies table(z,i) != table(z,j) by {
        assert(table(z,i) == table(b,i));
        assert(table(z,j) == table(b,j));
    }
    assert forall|i: int| 0 <= i < z.records.len() implies r::prepared(z,i)
        && table(z,z.records[i].slot as int) == z.records[i].plan.range.table
        && z.nodes.contains(z.records[i].plan.source) && z.nodes.contains(z.records[i].plan.destination)
        && z.records[i].plan.source != z.records[i].plan.destination by {
        r::prepared_retained(b,z,i);
        assert(table(z,z.records[i].slot as int) == table(b,b.records[i].slot as int));
    }
    if let Some(i) = b.active { assert(z.controls[i as int].phase == b.controls[i as int].phase); }
}
pub proof fn received_preserves(b: n::Native,z: n::Native,g: u64,owner: u32,cert: crate::types::Certificate,status: Status)
    requires n::layout(b), routing(b), n::receive_effect(b,z,g,owner,cert,status),
    ensures routing(z),
{
    observation_preserves(b,z);
}
pub proof fn reply_preserves(b: n::Native,z: n::Native,id: TxnId,result: Option<crate::types::Outcome>)
    requires n::layout(b), routing(b), n::reply_effect(b,z,id,result),
    ensures routing(z),
{
    observation_preserves(b,z);
}
/// Unique stable table slots discharge the publication observer's table test.
pub proof fn unique_observed_slot(c: crate::sharding_placement::Constants,v: n::Native,o: r::Observations,i: int,k: int)
    requires n::layout(v), routing(v), r::observes(c,v,o), 0 <= i < v.records.len(), c.keys.contains(k),
    ensures (c.table[k] == v.records[i].plan.range.table) == (o.slots[k] == v.records[i].slot),
{
    let a = o.slots[k] as int; let b = v.records[i].slot as int;
    assert(table(v,b) == v.records[i].plan.range.table);
    if a < b { assert(table(v,a) != table(v,b)); }
    if b < a { assert(table(v,b) != table(v,a)); }
}
/// Successful Commit has Retiring phase, so the inductive native invariant
/// identifies the pre-publication snapshot without assuming the model step.
pub proof fn precommit_snapshot(b: n::Native,z: n::Native)
    requires n::layout(b), routing(b), n::transition_effect(b,z,n::Request::Commit,Status::Ok),
    ensures b.active is Some, r::prepared(b,b.active.unwrap() as int),
        b.current[b.records[b.active.unwrap() as int].slot as int] == b.records[b.active.unwrap() as int].previous,
{}
/// Whole master request closure. The only directory premise is the existing
/// observation coupling; publication is derived from the native index mutation
/// and the byte-directory replacement theorem.
pub proof fn request_segment(c: crate::sharding_placement::Constants,b: n::Native,z: n::Native,o: r::Observations,
    s: crate::sharding_placement::State,request: n::Request,status: Status) -> (segment: crate::ghost_log::Segment)
    requires n::layout(b), routing(b), r::master(b,s), n::transition_effect(b,z,request,status),
        r::observes(c,b,o), crate::sharding_placement::directory(s) == r::directory(c,b,o),
        request is Commit && status == Status::Ok ==> {
            let i = b.active.unwrap() as int;
            r::range_observed(o,b.records[i].plan.range)
            && s.plans[s.active.unwrap()].keys == r::selected(c,o,b.records[i].plan.range)
        },
    ensures crate::ghost_log::certificate(c,s,segment), routing(z),
        r::master(z,crate::ghost_log::apply_writes(s,segment.writes)),
        crate::sharding_placement::directory(crate::ghost_log::apply_writes(s,segment.writes)) == r::directory(c,z,o),
        segment.writes == r::transition_writes(b,z,request,r::directory(c,z,o),status),
{
    let snapshot = r::directory(c,z,o);
    request_preserves(b,z,request,status);
    if request is Commit && status == Status::Ok {
        precommit_snapshot(b,z);
        let i = b.active.unwrap() as int;
        assert(r::controls_at(b,s,i));
        assert forall|k: int| c.keys.contains(k) implies
            (c.table[k] == b.records[i].plan.range.table) == (o.slots[k] == b.records[i].slot) by {
            unique_observed_slot(c,b,o,i,k);
        }
        r::publication_observation(c,b,z,o,s);
    } else {
        assert(snapshot =~= r::directory(c,b,o));
    }
    r::transition_replay(b,z,s,c,request,status,snapshot);
    r::transition_projection(c,b,z,s,request,status,snapshot);
    r::transition_fields(b,z,s,request,snapshot,status);
    let writes = r::transition_writes(b,z,request,snapshot,status);
    r::segment(c,s,writes,r::action(request))
}

pub proof fn old_plan_observation(c: crate::sharding_placement::Constants,b: n::Native,z: n::Native,o: r::Observations,
    id: TxnId,src: u32,dst: u32,range: KeyRange,g: u64,k: int)
    requires n::layout(b), n::begin_effect(b,z,id,src,dst,range,Ok(g)), r::observes(c,b,o),
        c.keys.contains(k), c.table[k] == range.table, o.slots[k] == z.records.last().slot,
    ensures r::grant(crate::directory::route(z.records.last().plan.old@,o.bytes[k])) == r::begin_plan(c,b,z,o).old[k],
{
    crate::directory_proofs::equivalent_routes(z.records.last().plan.old@,
        b.snapshots[z.records.last().previous as int].boundaries@,o.bytes[k]);
}

/// A logical empty-cell observation at lo witnesses the range independently of
/// whether the physical storage currently contains any row in that range.
pub proof fn nonempty_observation(c: crate::sharding_placement::Constants,v: n::Native,o: r::Observations,range: KeyRange,k: int)
    requires n::layout(v), r::observes(c,v,o), r::range_observed(o,range),
        crate::directory::proper(range.lo@,n::upper(range)), c.keys.contains(k),
        c.table[k] == range.table, o.bytes[k] == range.lo@,
    ensures r::selected(c,o,range) != Set::<int>::empty(),
{
    crate::bytes::cmp_laws(range.lo@,range.lo@);
    r::range_observation(c,v,o,range,k);
    assert(r::selected(c,o,range).contains(k));
}

pub proof fn publication_version_monotone(b: n::Native,z: n::Native,request: n::Request,status: Status)
    requires n::layout(b), routing(b), n::transition_effect(b,z,request,status),
    ensures b.published_version <= z.published_version,
        request is Commit && status == Status::Ok ==> z.published_version == b.records[b.active.unwrap() as int].plan.generation,
        !(request is Commit && status == Status::Ok) ==> z.published_version == b.published_version,
{}

pub proof fn begin_segment(c: crate::sharding_placement::Constants,b: n::Native,z: n::Native,o: r::Observations,
    s: crate::sharding_placement::State,id: TxnId,src: u32,dst: u32,range: KeyRange,g: u64) -> (segment: crate::ghost_log::Segment)
    requires n::layout(b), n::layout(z), routing(b), r::master(b,s),
        n::begin_effect(b,z,id,src,dst,range,Ok(g)), r::observes(c,b,o), r::range_observed(o,range),
        crate::sharding_placement::directory(s) == r::directory(c,b,o),
        forall|node: u32| b.nodes.contains(node) ==> c.shards.contains(node as int),
        r::selected(c,o,range) != Set::<int>::empty(),
    ensures crate::ghost_log::certificate(c,s,segment), routing(z),
        r::master(z,crate::ghost_log::apply_writes(s,segment.writes)),
        crate::sharding_placement::directory(crate::ghost_log::apply_writes(s,segment.writes)) == r::directory(c,z,o),
        segment.writes == r::begin_writes(c,b,z,o),
{
    begin_preserves(b,z,id,src,dst,range,g);
    assert(r::observes(c,z,o));
    assert(r::directory(c,z,o) =~= r::directory(c,b,o));
    assert forall|k: int| c.keys.contains(k) && c.table[k] == range.table implies o.slots[k] == z.records.last().slot by {
        unique_observed_slot(c,z,o,b.records.len() as int,k);
    }
    r::begin_replay(c,b,z,o,s,id,src,dst,range,g);
    r::begin_projection(c,b,z,o,s,id,src,dst,range,g);
    r::replay_begin_fields(s,z.records.last().plan.generation as nat,r::begin_plan(c,b,z,o),
        r::phase(z.controls.last().phase),z.next_generation as nat,r::active(z));
    r::segment(c,s,r::begin_writes(c,b,z,o),r::begin_action(o,id,src,dst,range))
}

} // verus!
