//! The executable command walk samples the union of local metadata boundaries
//! and immutable old-grant boundaries. This proves its guard over every byte.
use vstd::prelude::*;
use crate::types::{Boundary,Grant,ReplicaMeta};
use crate::bytes::{cmp_spec,cmp_laws,cmp_trans};
use crate::directory::{wellformed,route,inside};
use crate::participant::{MetaBoundary,meta_wf,meta_route,selected_meta,selected_meta_index};
verus! {
pub open spec fn joint(p: Seq<MetaBoundary>, old: Seq<Boundary>, k: Seq<u8>, predicate: spec_fn(ReplicaMeta,Grant)->bool) -> bool {
    meta_route(p,k).is_some() && predicate(meta_route(p,k).unwrap(),route(old,k))
}
pub proof fn joint_uniform(p: Seq<MetaBoundary>, old: Seq<Boundary>, lo: Seq<u8>, hi: Option<Seq<u8>>,
    predicate: spec_fn(ReplicaMeta,Grant)->bool)
    requires meta_wf(p), wellformed(old), joint(p,old,lo,predicate),
        forall|i:int| 0 <= i < p.len() && inside(p[i].start@,lo,hi) ==> joint(p,old,p[i].start@,predicate),
        forall|i:int| 0 <= i < old.len() && inside(old[i].start@,lo,hi) ==> joint(p,old,old[i].start@,predicate),
    ensures forall|k:Seq<u8>| inside(k,lo,hi) ==> joint(p,old,k,predicate),
{
    assert forall|k:Seq<u8>| inside(k,lo,hi) implies joint(p,old,k,predicate) by {
        let m = selected_meta_index(p,k);
        let o = crate::directory_proofs::selected(old,k);
        let a = p[m].start@;
        let b = old[o].start@;
        cmp_laws(lo,a); cmp_laws(lo,b); cmp_laws(a,b);
        let probe = if cmp_spec(lo,a) >= 0 && cmp_spec(lo,b) >= 0 { lo }
            else if cmp_spec(a,b) >= 0 { a } else { b };
        if probe == a && cmp_spec(lo,a) > 0 { cmp_trans(b,a,lo); }
        if probe == b && cmp_spec(lo,b) > 0 { cmp_trans(a,b,lo); }
        assert(cmp_spec(lo,probe) <= 0);
        assert(cmp_spec(a,probe) <= 0 && cmp_spec(b,probe) <= 0);
        assert(cmp_spec(probe,k) <= 0);
        if let Some(h) = hi { cmp_trans(probe,k,h); }
        assert(inside(probe,lo,hi));
        if probe == lo { assert(joint(p,old,probe,predicate)); }
        else if probe == a { assert(joint(p,old,p[m].start@,predicate)); }
        else { assert(joint(p,old,old[o].start@,predicate)); }
        assert forall|j:int| m < j < p.len() implies cmp_spec(p[j].start@,probe) > 0 by {
            cmp_laws(p[j].start@,k); cmp_trans(probe,k,p[j].start@); cmp_laws(probe,p[j].start@);
        }
        assert forall|j:int| o < j < old.len() implies cmp_spec(old[j].start@,probe) > 0 by {
            cmp_laws(old[j].start@,k); cmp_trans(probe,k,old[j].start@); cmp_laws(probe,old[j].start@);
        }
        selected_meta(p,m,probe);
        crate::directory_proofs::selected_route(old,o,probe);
    }
}
} // verus!
