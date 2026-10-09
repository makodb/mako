//! Byte-interval facts used by the native metadata writer. No placement action
//! is executed here; a frame describes only the actual changed coordinates.
use vstd::prelude::*;
use crate::bytes::{cmp_spec,cmp_laws,cmp_trans};
use crate::participant::{MetaBoundary,meta_route,meta_ordered,meta_wf,selected_meta};
use crate::types::{ReplicaMeta,KeyRange,Command,Role};
verus! {
pub open spec fn transformed(m: ReplicaMeta, g: u64, c: Option<Command>, source: bool) -> ReplicaMeta {
    match c { Some(c) => crate::participant::effect_spec(m,g,c,source), None => ReplicaMeta { role: Role::Ready, ..m } }
}
pub open spec fn update_frame(before: Seq<MetaBoundary>, after: Seq<MetaBoundary>, range: KeyRange,
    g: u64, c: Option<Command>, source: bool) -> bool {
    forall|k: Seq<u8>| meta_route(after,k) == if crate::bytes::contains_spec(range,range.table,k) {
        match meta_route(before,k) { Some(m) => Some(transformed(m,g,c,source)), None => None }
    } else { meta_route(before,k) }
}
pub open spec fn equivalent(a: Seq<MetaBoundary>, b: Seq<MetaBoundary>) -> bool {
    a.len() == b.len() && forall|i: int| 0 <= i < a.len() ==> a[i].start@ == b[i].start@ && a[i].meta == b[i].meta
}
pub proof fn equivalent_route(a: Seq<MetaBoundary>, b: Seq<MetaBoundary>, k: Seq<u8>)
    requires equivalent(a,b),
    ensures meta_route(a,k) == meta_route(b,k),
    decreases a.len(),
{
    if a.len() > 0 {
        assert(a.last().start@ == b.last().start@ && a.last().meta == b.last().meta);
        if cmp_spec(a.last().start@,k) > 0 { equivalent_route(a.drop_last(),b.drop_last(),k); }
    }
}
pub proof fn routes_below(p: Seq<MetaBoundary>, n: int, lo: Seq<u8>)
    requires meta_wf(p), 0 <= n <= p.len(),
        n < p.len() ==> cmp_spec(p[n].start@,lo) >= 0,
    ensures forall|k: Seq<u8>| cmp_spec(lo,k) > 0 ==> meta_route(p,k) == meta_route(p.take(n),k),
{
    assert forall|k: Seq<u8>| cmp_spec(lo,k) > 0 implies meta_route(p,k) == meta_route(p.take(n),k) by {
        assert forall|j: int| n <= j < p.len() implies cmp_spec(p[j].start@,k) > 0 by {
            cmp_laws(p[n].start@,lo);
            cmp_laws(lo,k);
            if j > n { cmp_trans(lo,p[n].start@,p[j].start@); }
            cmp_trans(k,lo,p[j].start@); cmp_laws(k,p[j].start@);
        }
        prefix_route(p,n,k);
    }
}
pub proof fn copied_prefix(p: Seq<MetaBoundary>, out: Seq<MetaBoundary>, n: int, lo: Seq<u8>)
    requires meta_wf(p), 0 <= n <= p.len(), equivalent(out,p.take(n)),
        forall|j: int| 0 <= j < n ==> cmp_spec(#[trigger] p[j].start@,lo) < 0,
        n < p.len() ==> cmp_spec(p[n].start@,lo) >= 0,
    ensures meta_ordered(out),
        out.len() == 0 ==> lo == Seq::<u8>::empty(),
        out.len() > 0 ==> out[0].start@ == Seq::<u8>::empty(),
        forall|j: int| 0 <= j < out.len() ==> cmp_spec(#[trigger] out[j].start@,lo) < 0,
        forall|k: Seq<u8>| cmp_spec(lo,k) > 0 ==> meta_route(out,k) == meta_route(p,k),
{
    routes_below(p,n,lo);
    if n == 0 { cmp_laws(Seq::empty(),lo); }
    assert forall|a: int,b: int| 0 <= a < b < out.len() implies cmp_spec(out[a].start@,out[b].start@) < 0 by {}
    assert forall|k: Seq<u8>| cmp_spec(lo,k) > 0 implies meta_route(out,k) == meta_route(p,k) by {
        equivalent_route(out,p.take(n),k);
    }
}
pub proof fn prefix_constant(p: Seq<MetaBoundary>, n: int, split: Seq<u8>)
    requires meta_wf(p), 0 < n <= p.len(), cmp_spec(p[n-1].start@,split) <= 0,
        n < p.len() ==> cmp_spec(split,p[n].start@) < 0,
    ensures forall|k: Seq<u8>| cmp_spec(split,k) <= 0 ==> meta_route(p.take(n),k) == meta_route(p,split),
{
    assert forall|j: int| n <= j < p.len() implies cmp_spec(p[j].start@,split) > 0 by {
        if j > n { cmp_trans(split,p[n].start@,p[j].start@); }
        cmp_laws(split,p[j].start@);
    }
    selected_meta(p,n-1,split);
    assert forall|k: Seq<u8>| cmp_spec(split,k) <= 0 implies meta_route(p.take(n),k) == meta_route(p,split) by {
        cmp_trans(p[n-1].start@,split,k);
        assert(p.take(n).last() == p[n-1]);
    }
}

pub proof fn prefix_route(p: Seq<MetaBoundary>, n: int, k: Seq<u8>)
    requires 0 <= n <= p.len(), forall|j: int| n <= j < p.len() ==> cmp_spec(#[trigger] p[j].start@,k) > 0,
    ensures meta_route(p,k) == meta_route(p.take(n),k),
    decreases p.len()-n,
{
    if n < p.len() {
        prefix_route(p.drop_last(),n,k);
        assert(p.drop_last().take(n) =~= p.take(n));
    }
    else { assert(p.take(n) =~= p); }
}

pub proof fn append_same(p: Seq<MetaBoundary>, start: Seq<u8>, m: ReplicaMeta, k: Seq<u8>)
    requires p.len() > 0, p.last().meta == m, cmp_spec(p.last().start@,start) <= 0,
    ensures cmp_spec(start,k) <= 0 ==> meta_route(p,k) == Some(m),
{
    if cmp_spec(start,k) <= 0 { cmp_trans(p.last().start@,start,k); }
}

/// An interval split copies the predecessor's entire metadata, not just its
/// role. This is the reason a partial return cannot erase a neighboring fence.
pub proof fn split_frame(p: Seq<MetaBoundary>, out: Seq<MetaBoundary>, split: Seq<u8>, i: int)
    requires meta_wf(p), 0 <= i < p.len(), cmp_spec(p[i].start@,split) <= 0,
        forall|j: int| i < j < p.len() ==> cmp_spec(#[trigger] p[j].start@,split) > 0,
        forall|k: Seq<u8>| meta_route(out,k) ==
            if cmp_spec(split,k) <= 0 && (i+1 == p.len() || cmp_spec(k,p[i+1].start@) < 0) {
                Some(p[i].meta)
            } else { meta_route(p,k) },
    ensures forall|k: Seq<u8>| meta_route(out,k) == meta_route(p,k),
{
    assert forall|k: Seq<u8>| meta_route(out,k) == meta_route(p,k) by {
        if cmp_spec(split,k) <= 0 && (i+1 == p.len() || cmp_spec(k,p[i+1].start@) < 0) {
            cmp_trans(p[i].start@,split,k);
            assert forall|j: int| i < j < p.len() implies cmp_spec(p[j].start@,k) > 0 by {
                if j > i+1 { cmp_trans(k,p[i+1].start@,p[j].start@); }
                cmp_laws(k,p[j].start@);
            }
            selected_meta(p,i,k);
        }
    }
}

pub proof fn outside_preserves_incarnation(before: Seq<MetaBoundary>, after: Seq<MetaBoundary>, range: KeyRange,
    g: u64, c: Option<Command>, source: bool, coordinate: Seq<u8>)
    requires update_frame(before,after,range,g,c,source),
        !crate::bytes::contains_spec(range,range.table,coordinate),
    ensures meta_route(after,coordinate) == meta_route(before,coordinate),
{ }
pub proof fn uniform_bounds(p: Seq<MetaBoundary>, lo: Seq<u8>, hi: Option<Seq<u8>>, predicate: spec_fn(ReplicaMeta) -> bool)
    requires meta_wf(p), meta_route(p,lo).is_some(), predicate(meta_route(p,lo).unwrap()),
        forall|i: int| 0 <= i < p.len() && crate::directory::inside(p[i].start@,lo,hi) ==> predicate(p[i].meta),
    ensures forall|k: Seq<u8>| crate::directory::inside(k,lo,hi) ==> meta_route(p,k).is_some() && predicate(meta_route(p,k).unwrap()),
{
    assert forall|k: Seq<u8>| crate::directory::inside(k,lo,hi) implies
        meta_route(p,k).is_some() && predicate(meta_route(p,k).unwrap()) by {
        let i = crate::participant::selected_meta_index(p,k);
        if cmp_spec(lo,p[i].start@) <= 0 {
            if let Some(h) = hi { cmp_trans(p[i].start@,k,h); }
        } else {
            cmp_laws(lo,p[i].start@);
            assert forall|j: int| i < j < p.len() implies cmp_spec(p[j].start@,lo) > 0 by {
                cmp_laws(p[j].start@,k); cmp_trans(lo,k,p[j].start@); cmp_laws(lo,p[j].start@);
            }
            selected_meta(p,i,lo);
        }
    }
}
} // verus!
