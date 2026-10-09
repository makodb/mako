//! Concrete install/iterator effects and their immutable model observations.
#![cfg(verus_keep_ghost)]
use vstd::prelude::*;
use crate::types::{Boundary,Grant,Status};
use crate::bytes::{cmp_spec,cmp_laws,cmp_trans};
use crate::directory::{RouteTable,wellformed,canonical,route,inside,proper,hi_view};
use crate::directory_proofs::{equivalent_routes,selected_route};
use crate::routing::*;
#[cfg(verus_keep_ghost)]
use crate::{sharding_placement as p,ghost_log as g};
verus! {
pub open spec fn install_effect(before: Option<Snapshot>,after: Option<Snapshot>,incoming: Snapshot,status: Status) -> bool {
    if after == before { status != Status::Ok || before is Some && same_snapshot(before.unwrap(),incoming) }
    else { status == Status::Ok && after == Some(incoming)
        && (before is Some ==> before.unwrap().version < incoming.version
            && contains_catalog(before.unwrap(),incoming) && contains_catalog(incoming,before.unwrap())) }
}
pub proof fn cache_no_regression(before: Option<Snapshot>,after: Option<Snapshot>,incoming: Snapshot,status: Status)
    requires install_effect(before,after,incoming,status),
    ensures before is Some ==> after is Some && after.unwrap().version >= before.unwrap().version,
        before is Some && after is Some && before.unwrap().version == after.unwrap().version ==> before == after,
        status != Status::Ok ==> before == after,
{}
pub proof fn found_table(s: Snapshot,t: RouteTable,id: u64,key: Seq<u8>,nodes: Seq<u32>)
    requires snapshot_valid(s,nodes), t.table == id,
        exists|i: int| 0 <= i < s.tables.len() && t == s.tables@[i],
    ensures wellformed(t.boundaries@), snapshot_lookup(s,id,key) == Some(route(t.boundaries@,key)),
{
    let i = choose|i: int| 0 <= i < s.tables.len() && t == s.tables@[i];
    let j = choose|j: int| 0 <= j < s.tables.len() && s.tables@[j].table == id;
    if i != j { assert(s.tables@[i].table != s.tables@[j].table); }
}
pub proof fn identical_observations(a: Snapshot,b: Snapshot,table: u64,key: Seq<u8>)
    requires same_snapshot(a,b),
        forall|i: int,j: int| 0 <= i < j < a.tables.len() ==> a.tables@[i].table != a.tables@[j].table,
    ensures snapshot_lookup(a,table,key) == snapshot_lookup(b,table,key),
{
    if exists|i: int| 0 <= i < a.tables.len() && a.tables@[i].table == table {
        let i = choose|i: int| 0 <= i < a.tables.len() && a.tables@[i].table == table;
        assert(b.tables@[i].table == table);
        let j = choose|j: int| 0 <= j < b.tables.len() && b.tables@[j].table == table;
        if i != j { assert(a.tables@[i].table != a.tables@[j].table); }
        equivalent_routes(a.tables@[i].boundaries@,b.tables@[i].boundaries@,key);
    }
}
pub proof fn identical_validity(a: Snapshot,b: Snapshot,nodes: Seq<u32>)
    requires same_snapshot(a,b), snapshot_valid(a,nodes),
    ensures snapshot_valid(b,nodes),
{
    assert forall|i: int| 0 <= i < b.tables.len() implies table_valid(b.tables@[i],nodes,b.version) by {
        let x = a.tables@[i].boundaries@;
        let y = b.tables@[i].boundaries@;
        crate::directory_proofs::equivalent_shape(y,x);
        assert(y[0].start@ == x[0].start@);
        assert forall|j: int| 0 <= j < y.len() implies nodes.contains(y[j].grant.owner) && y[j].grant.epoch <= b.version by {
            assert(y[j].grant == x[j].grant);
        }
    }
    assert forall|i: int,j: int| 0 <= i < j < b.tables.len() implies b.tables@[i].table != b.tables@[j].table by {
        assert(a.tables@[i].table == b.tables@[i].table);
        assert(a.tables@[j].table == b.tables@[j].table);
    }
}
pub open spec fn segment_step(t: RouteTable,position: Option<Seq<u8>>,upper: Option<Seq<u8>>,
    next: Option<Seq<u8>>,out: Option<ScanSegment>) -> bool {
    match out {
        None => position is None && next is None,
        Some(s) => position == Some(s.lo@) && proper(s.lo@,hi_view(s.hi))
            && (match next { Some(k) => hi_view(s.hi) == Some(k) && proper(k,upper), None => hi_view(s.hi) == upper })
            && (forall|k: Seq<u8>| inside(k,s.lo@,hi_view(s.hi)) ==> route(t.boundaries@,k) == s.grant)
            && (forall|i: int| 0 <= i < t.boundaries.len() && cmp_spec(s.lo@,t.boundaries@[i].start@) < 0
                ==> !inside(t.boundaries@[i].start@,s.lo@,hi_view(s.hi)))
            && (next is Some ==> exists|i: int| 0 < i < t.boundaries.len()
                && t.boundaries@[i].start@ == next.unwrap() && t.boundaries@[i-1].grant == s.grant),
    }
}
pub open spec fn reverse_segment_step(t: RouteTable,lower: Seq<u8>,position: Option<Option<Seq<u8>>>,
    next: Option<Option<Seq<u8>>>,out: Option<ScanSegment>) -> bool {
    match out {
        None => position is None && next is None,
        Some(s) => position == Some(hi_view(s.hi)) && proper(s.lo@,hi_view(s.hi))
            && (match next { Some(h) => h == Some(s.lo@) && proper(lower,h), None => s.lo@ == lower })
            && (forall|k: Seq<u8>| inside(k,s.lo@,hi_view(s.hi)) ==> route(t.boundaries@,k) == s.grant)
            && (forall|i: int| 0 <= i < t.boundaries.len() && cmp_spec(s.lo@,t.boundaries@[i].start@) < 0
                ==> !inside(t.boundaries@[i].start@,s.lo@,hi_view(s.hi)))
            && (next is Some ==> exists|i: int| 0 < i < t.boundaries.len() && t.boundaries@[i].start@ == s.lo@),
    }
}
pub proof fn segment_routes(p: Seq<Boundary>,index: int,lo: Seq<u8>,hi: Option<Seq<u8>>)
    requires wellformed(p), 0 <= index < p.len(), proper(lo,hi),
        cmp_spec(p[index].start@,lo) <= 0,
        index+1 < p.len() ==> hi is Some && cmp_spec(hi.unwrap(),p[index+1].start@) <= 0,
    ensures forall|k: Seq<u8>| inside(k,lo,hi) ==> route(p,k) == p[index].grant,
        forall|i: int| 0 <= i < p.len() && cmp_spec(lo,p[i].start@) < 0 ==> !inside(p[i].start@,lo,hi),
{
    assert forall|k: Seq<u8>| inside(k,lo,hi) implies route(p,k) == p[index].grant by {
        cmp_trans(p[index].start@,lo,k);
        assert forall|j: int| index < j < p.len() implies cmp_spec(#[trigger] p[j].start@,k) > 0 by {
            cmp_trans(k,hi.unwrap(),p[index+1].start@);
            if index+1 < j { cmp_trans(k,p[index+1].start@,p[j].start@); }
            cmp_laws(k,p[j].start@);
        }
        selected_route(p,index,k);
    }
    assert forall|i: int| 0 <= i < p.len() && cmp_spec(lo,p[i].start@) < 0 implies !inside(p[i].start@,lo,hi) by {
        if i < index { cmp_trans(p[i].start@,p[index].start@,lo); cmp_laws(lo,p[i].start@); }
        else if i == index { cmp_laws(lo,p[i].start@); }
        else {
            if index+1 < i { cmp_trans(hi.unwrap(),p[index+1].start@,p[i].start@); }
            cmp_laws(hi.unwrap(),p[i].start@);
        }
    }
}
/// One iterator step partitions exactly the unconsumed half-open interval.
/// Induction with the strict `remaining` decrease yields complete finite scans.
pub proof fn segment_exact_partition(t: RouteTable,position: Option<Seq<u8>>,upper: Option<Seq<u8>>,
    next: Option<Seq<u8>>,s: ScanSegment,key: Seq<u8>)
    requires segment_step(t,position,upper,next,Some(s)),
    ensures inside(key,position.unwrap(),upper) == (inside(key,s.lo@,hi_view(s.hi))
        || next is Some && inside(key,next.unwrap(),upper)),
        !(inside(key,s.lo@,hi_view(s.hi)) && next is Some && inside(key,next.unwrap(),upper)),
{
    if let Some(k) = next {
        cmp_laws(s.lo@,k); cmp_laws(k,key);
        if cmp_spec(k,key) <= 0 { cmp_trans(s.lo@,k,key); }
        if let Some(h) = upper {
            if cmp_spec(key,k) < 0 { cmp_trans(key,k,h); }
        }
    }
}
pub proof fn reverse_segment_exact_partition(t: RouteTable,lower: Seq<u8>,position: Option<Option<Seq<u8>>>,
    next: Option<Option<Seq<u8>>>,s: ScanSegment,key: Seq<u8>)
    requires reverse_segment_step(t,lower,position,next,Some(s)),
    ensures inside(key,lower,position.unwrap()) == (inside(key,s.lo@,hi_view(s.hi))
        || next is Some && inside(key,lower,next.unwrap())),
        !(inside(key,s.lo@,hi_view(s.hi)) && next is Some && inside(key,lower,next.unwrap())),
{
    if next is Some {
        cmp_laws(s.lo@,key);
        if cmp_spec(s.lo@,key) <= 0 { cmp_trans(lower,s.lo@,key); }
        if let Some(h) = s.hi {
            if cmp_spec(key,s.lo@) < 0 { cmp_trans(key,s.lo@,h@); }
        }
    }
}
pub open spec fn complete_segments(t: RouteTable,position: Option<Seq<u8>>,upper: Option<Seq<u8>>,segments: Seq<ScanSegment>) -> bool
    decreases segments.len(),
{
    if segments.len() == 0 { position is None }
    else {
        let next = if segments.len() == 1 { None } else { Some(segments[1].lo@) };
        segment_step(t,position,upper,next,Some(segments[0]))
            && complete_segments(t,next,upper,segments.skip(1))
    }
}
/// Every point, including absent physical rows, appears in exactly one segment.
pub proof fn complete_scan_partition(t: RouteTable,position: Option<Seq<u8>>,upper: Option<Seq<u8>>,segments: Seq<ScanSegment>,key: Seq<u8>)
    requires complete_segments(t,position,upper,segments),
    ensures (position is Some && inside(key,position.unwrap(),upper)) ==
            (exists|i: int| 0 <= i < segments.len() && inside(key,segments[i].lo@,hi_view(segments[i].hi))),
        forall|i: int,j: int| 0 <= i < j < segments.len() ==> !(inside(key,segments[i].lo@,hi_view(segments[i].hi))
            && inside(key,segments[j].lo@,hi_view(segments[j].hi))),
        forall|i: int| 0 <= i < segments.len() && inside(key,segments[i].lo@,hi_view(segments[i].hi))
            ==> route(t.boundaries@,key) == segments[i].grant,
    decreases segments.len(),
{
    if segments.len() > 0 {
        let next = if segments.len() == 1 { None } else { Some(segments[1].lo@) };
        segment_exact_partition(t,position,upper,next,segments[0],key);
        complete_scan_partition(t,next,upper,segments.skip(1),key);
        assert((exists|i: int| 0 <= i < segments.len() && inside(key,segments[i].lo@,hi_view(segments[i].hi)))
            == (inside(key,segments[0].lo@,hi_view(segments[0].hi))
                || exists|i: int| 0 <= i < segments.skip(1).len() && inside(key,segments.skip(1)[i].lo@,hi_view(segments.skip(1)[i].hi)))) by {
            if exists|i: int| 0 <= i < segments.len() && inside(key,segments[i].lo@,hi_view(segments[i].hi)) {
                let i = choose|i: int| 0 <= i < segments.len() && inside(key,segments[i].lo@,hi_view(segments[i].hi));
                if i > 0 { assert(segments.skip(1)[i-1] == segments[i]); }
            }
            if exists|i: int| 0 <= i < segments.skip(1).len() && inside(key,segments.skip(1)[i].lo@,hi_view(segments.skip(1)[i].hi)) {
                let i = choose|i: int| 0 <= i < segments.skip(1).len() && inside(key,segments.skip(1)[i].lo@,hi_view(segments.skip(1)[i].hi));
                assert(segments.skip(1)[i] == segments[i+1]);
            }
        }
        assert forall|i: int,j: int| 0 <= i < j < segments.len() implies !(inside(key,segments[i].lo@,hi_view(segments[i].hi))
            && inside(key,segments[j].lo@,hi_view(segments[j].hi))) by {
            assert(segments.skip(1)[j-1] == segments[j]);
            if i > 0 { assert(segments.skip(1)[i-1] == segments[i]); }
        }
        assert forall|i: int| 0 <= i < segments.len() && inside(key,segments[i].lo@,hi_view(segments[i].hi))
            implies route(t.boundaries@,key) == segments[i].grant by {
            if i > 0 { assert(segments.skip(1)[i-1] == segments[i]); }
        }
    }
}
/// Trusted transport provenance selects a published immutable index; it does
/// not equate the sparse committed-generation version with that dense index.
pub open spec fn observed(s: Snapshot,model: p::State,index: nat,
    tables: Map<int,u64>,coordinates: Map<int,Seq<u8>>) -> bool {
    index < model.directory.len() && tables.dom() == coordinates.dom()
    && model.directory[index as int].dom() == tables.dom()
    && forall|k: int| tables.contains_key(k) ==> match snapshot_lookup(s,tables[k],coordinates[k]) {
        Some(grant) => model.directory[index as int][k] == (p::Grant { owner: grant.owner as int,epoch: grant.epoch as nat }),
        None => false,
    }
}
pub proof fn cache_install_refines(c: p::Constants,model: p::State,client: int,index: nat,
    before: Option<Snapshot>,after: Option<Snapshot>,incoming: Snapshot,status: Status,
    tables: Map<int,u64>,coordinates: Map<int,Seq<u8>>)
    requires install_effect(before,after,incoming,status), after != before,
        observed(incoming,model,index,tables,coordinates),
    ensures p::enabled(c,model,p::Action::Cache { client,snapshot: index }),
        g::apply_write(model,g::Write::View { client,snapshot: index }) == p::apply(c,model,p::Action::Cache { client,snapshot: index }),
        after == Some(incoming),
        forall|k: int| tables.contains_key(k) ==> snapshot_lookup(after.unwrap(),tables[k],coordinates[k])
            == Some(Grant { owner: model.directory[index as int][k].owner as u32,
                epoch: model.directory[index as int][k].epoch as u64 }),
{}
} // verus!
