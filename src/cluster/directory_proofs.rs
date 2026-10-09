//! Proofs over the executable boundary sequence, not a replacement model oracle.
use vstd::prelude::*;
use crate::types::{Boundary, Grant};
use crate::bytes::copy_bytes;
#[cfg(verus_keep_ghost)]
use crate::bytes::{cmp_spec, cmp_laws, cmp_trans};
#[cfg(verus_keep_ghost)]
use crate::directory::{ordered, wellformed, canonical, route, inside, proper, source_owned};
verus! {
pub open spec fn equivalent(p: Seq<Boundary>, q: Seq<Boundary>) -> bool {
    p.len() == q.len() && forall|i: int| 0 <= i < p.len() ==>
        p[i].start@ == q[i].start@ && p[i].grant == q[i].grant
}
pub proof fn equivalent_shape(p: Seq<Boundary>, q: Seq<Boundary>)
    requires equivalent(p,q), ordered(q), canonical(q),
    ensures ordered(p), canonical(p),
{
    assert forall|i: int,j: int| 0 <= i < j < p.len() implies
        cmp_spec(#[trigger] p[i].start@,#[trigger] p[j].start@) < 0 by {
        assert(p[i].start@ == q[i].start@); assert(p[j].start@ == q[j].start@);
    }
    assert forall|i: int| 0 < i < p.len() implies #[trigger] p[i].grant != p[i-1].grant by {}
}
pub proof fn equivalent_routes(p: Seq<Boundary>, q: Seq<Boundary>, k: Seq<u8>)
    requires equivalent(p,q),
    ensures route(p,k) == route(q,k),
    decreases p.len(),
{
    if p.len() > 0 {
        assert(p.last().start@ == q.last().start@);
        assert(p.last().grant == q.last().grant);
        if cmp_spec(p.last().start@,k) > 0 {
            assert(equivalent(p.drop_last(),q.drop_last())) by {
                assert forall|i: int| 0 <= i < p.drop_last().len() implies
                    p.drop_last()[i].start@ == q.drop_last()[i].start@
                    && p.drop_last()[i].grant == q.drop_last()[i].grant by {}
            }
            equivalent_routes(p.drop_last(),q.drop_last(),k);
        }
    }
}
pub proof fn prefix_shape(p: Seq<Boundary>, n: int)
    requires 0 <= n <= p.len(), ordered(p), canonical(p),
    ensures ordered(p.take(n)), canonical(p.take(n)),
{
    assert forall|i: int,j: int| 0 <= i < j < p.take(n).len() implies
        cmp_spec(#[trigger] p.take(n)[i].start@,#[trigger] p.take(n)[j].start@) < 0 by {}
    assert forall|i: int| 0 < i < p.take(n).len() implies #[trigger] p.take(n)[i].grant != p.take(n)[i-1].grant by {}
}
pub proof fn copied_prefix(p: Seq<Boundary>, out: Seq<Boundary>, n: int, lo: Seq<u8>)
    requires wellformed(p), canonical(p), 0 <= n <= p.len(), equivalent(out,p.take(n)),
        forall|j: int| 0 <= j < n ==> cmp_spec(p[j].start@,lo) < 0,
        n < p.len() ==> cmp_spec(p[n].start@,lo) >= 0,
    ensures ordered(out), canonical(out),
        out.len() == 0 ==> lo == Seq::<u8>::empty(),
        out.len() > 0 ==> out[0].start@ == Seq::<u8>::empty(),
        forall|j: int| 0 <= j < out.len() ==> cmp_spec(out[j].start@,lo) < 0,
        forall|k: Seq<u8>| cmp_spec(lo,k) > 0 ==> route(out,k) == route(p,k),
{
    prefix_shape(p,n);
    equivalent_shape(out,p.take(n));
    prefix_routes_below(p,n,lo);
    if n == 0 { cmp_laws(Seq::empty(),lo); }
    assert forall|j: int| 0 <= j < out.len() implies cmp_spec(out[j].start@,lo) < 0 by {
        assert(out[j].start@ == p.take(n)[j].start@);
    }
    assert forall|k: Seq<u8>| cmp_spec(lo,k) > 0 implies route(out,k) == route(p,k) by {
        cmp_laws(lo,k);
        equivalent_routes(out,p.take(n),k);
    }
}
pub proof fn selected(p: Seq<Boundary>, k: Seq<u8>) -> (i: int)
    requires wellformed(p),
    ensures 0 <= i < p.len(), cmp_spec(p[i].start@,k) <= 0,
        forall|j: int| i < j < p.len() ==> cmp_spec(#[trigger] p[j].start@,k) > 0,
        route(p,k) == p[i].grant,
    decreases p.len(),
{
    cmp_laws(Seq::empty(),k);
    if cmp_spec(p.last().start@,k) <= 0 {
        p.len() as int - 1
    } else {
        assert(p.len() > 1);
        assert(ordered(p.drop_last())) by {
            assert forall|i: int,j: int| 0 <= i < j < p.drop_last().len() implies
                cmp_spec(#[trigger] p.drop_last()[i].start@,#[trigger] p.drop_last()[j].start@) < 0 by {}
        }
        let i = selected(p.drop_last(),k);
        assert forall|j: int| i < j < p.len() implies cmp_spec(#[trigger] p[j].start@,k) > 0 by {
            if j < p.len()-1 { assert(p[j] == p.drop_last()[j]); }
        }
        assert(p[i] == p.drop_last()[i]);
        i
    }
}
pub proof fn selected_route(p: Seq<Boundary>, i: int, k: Seq<u8>)
    requires 0 <= i < p.len(), cmp_spec(p[i].start@,k) <= 0,
        forall|j: int| i < j < p.len() ==> cmp_spec(#[trigger] p[j].start@,k) > 0,
    ensures route(p,k) == p[i].grant,
    decreases p.len(),
{
    if i < p.len()-1 { selected_route(p.drop_last(),i,k); }
}
pub proof fn route_at_boundary(p: Seq<Boundary>, i: int)
    requires ordered(p), 0 <= i < p.len(),
    ensures route(p,p[i].start@) == p[i].grant,
{
    cmp_laws(p[i].start@,p[i].start@);
    assert forall|j: int| i < j < p.len() implies cmp_spec(#[trigger] p[j].start@,p[i].start@) > 0 by {
        cmp_laws(p[i].start@,p[j].start@);
    }
    selected_route(p,i,p[i].start@);
}
pub proof fn uniform_source(p: Seq<Boundary>, lo: Seq<u8>, hi: Option<Seq<u8>>, src: u32)
    requires wellformed(p), proper(lo,hi), route(p,lo).owner == src,
        forall|i: int| 0 <= i < p.len() && inside(p[i].start@,lo,hi) ==> p[i].grant.owner == src,
    ensures source_owned(p,lo,hi,src),
{
    assert forall|k: Seq<u8>| inside(k,lo,hi) implies #[trigger] route(p,k).owner == src by {
        let i = selected(p,k);
        if cmp_spec(lo,p[i].start@) <= 0 {
            if let Some(h) = hi { cmp_trans(p[i].start@,k,h); }
            assert(inside(p[i].start@,lo,hi));
        } else {
            cmp_laws(lo,p[i].start@);
            assert forall|j: int| i < j < p.len() implies cmp_spec(#[trigger] p[j].start@,lo) > 0 by {
                cmp_laws(p[j].start@,k); cmp_trans(lo,k,p[j].start@); cmp_laws(lo,p[j].start@);
            }
            selected_route(p,i,lo);
        }
    }
}
pub proof fn route_suffix_above(p: Seq<Boundary>, n: int, k: Seq<u8>)
    requires 0 <= n <= p.len(),
        forall|j: int| n <= j < p.len() ==> cmp_spec(#[trigger] p[j].start@,k) > 0,
    ensures route(p,k) == route(p.take(n),k),
    decreases p.len() - n,
{
    if n < p.len() {
        route_suffix_above(p.drop_last(),n,k);
        assert(p.drop_last().take(n) == p.take(n));
    } else { assert(p.take(n) == p); }
}
pub proof fn prefix_routes_below(p: Seq<Boundary>, n: int, lo: Seq<u8>)
    requires ordered(p), 0 <= n <= p.len(), n < p.len() ==> cmp_spec(p[n].start@,lo) >= 0,
    ensures forall|k: Seq<u8>| cmp_spec(k,lo) < 0 ==> route(p.take(n),k) == route(p,k),
{
    assert forall|k: Seq<u8>| cmp_spec(k,lo) < 0 implies route(p.take(n),k) == route(p,k) by {
        assert forall|j: int| n <= j < p.len() implies cmp_spec(#[trigger] p[j].start@,k) > 0 by {
            cmp_laws(p[n].start@,lo); cmp_trans(k,lo,p[n].start@);
            if j > n { cmp_trans(k,p[n].start@,p[j].start@); }
            cmp_laws(k,p[j].start@);
        }
        route_suffix_above(p,n,k);
    }
}
pub proof fn route_after_last(p: Seq<Boundary>, k: Seq<u8>)
    requires p.len() > 0, cmp_spec(p.last().start@,k) <= 0,
    ensures route(p,k) == p.last().grant,
{}
/// Coalescing may remove an endpoint only when the entire grant agrees.
pub fn append(out: &mut Vec<Boundary>, start: &[u8], grant: Grant)
    requires ordered(old(out)@), canonical(old(out)@),
        forall|i: int| 0 <= i < old(out).len() ==> cmp_spec(old(out)@[i].start@,start@) < 0,
    ensures ordered(final(out)@), canonical(final(out)@), final(out).len() > 0,
        old(out).len() <= final(out).len() <= old(out).len() + 1,
        forall|i: int| 0 <= i < old(out).len() ==> final(out)@[i] == old(out)@[i],
        final(out).len() > old(out).len() ==> final(out)@.last().start@ == start@ && final(out)@.last().grant == grant,
        old(out).len() == 0 ==> final(out)@[0].start@ == start@,
        forall|i: int| 0 <= i < final(out).len() ==> cmp_spec(final(out)@[i].start@,start@) <= 0,
        forall|k: Seq<u8>| #[trigger] route(final(out)@,k) ==
            if cmp_spec(start@,k) <= 0 { grant } else { route(old(out)@,k) },
{
    let ghost before = out@;
    if out.len() > 0 {
        let last = &out[out.len()-1];
        if last.grant.owner == grant.owner && last.grant.epoch == grant.epoch {
            proof {
                assert(last.grant == grant);
                assert forall|k: Seq<u8>| route(out@,k) ==
                    if cmp_spec(start@,k) <= 0 { grant } else { route(before,k) } by {
                    if cmp_spec(start@,k) <= 0 { cmp_trans(last.start@,start@,k); }
                }
            }
            return;
        }
    }
    out.push(Boundary { start: copy_bytes(start), grant });
    proof {
        cmp_laws(start@,start@);
        assert(out@.drop_last() == before);
        assert forall|i: int,j: int| 0 <= i < j < out.len() implies
            cmp_spec(#[trigger] out@[i].start@,#[trigger] out@[j].start@) < 0 by {
            if j < before.len() { assert(out@[j] == before[j]); }
        }
        assert forall|i: int| 0 < i < out.len() implies #[trigger] out@[i].grant != out@[i-1].grant by {
            if i < before.len() { assert(out@[i] == before[i]); }
        }
    }
}
pub proof fn all_starts_below(p: Seq<Boundary>, lo: Seq<u8>, hi: Seq<u8>)
    requires cmp_spec(lo,hi) < 0,
        forall|i: int| 0 <= i < p.len() ==> cmp_spec(p[i].start@,lo) <= 0,
    ensures forall|i: int| 0 <= i < p.len() ==> cmp_spec(p[i].start@,hi) < 0,
{
    assert forall|i: int| 0 <= i < p.len() implies cmp_spec(p[i].start@,hi) < 0 by { cmp_trans(p[i].start@,lo,hi); }
}
pub open spec fn suffix_route(p: Seq<Boundary>, h: Seq<u8>, k: Seq<u8>, base: Grant) -> Grant
    decreases p.len(),
{
    if p.len() == 0 { base }
    else if cmp_spec(h,p.last().start@) < 0 && cmp_spec(p.last().start@,k) <= 0 { p.last().grant }
    else { suffix_route(p.drop_last(),h,k,base) }
}
pub proof fn suffix_append_pre(p: Seq<Boundary>, out: Seq<Boundary>, j: int, h: Seq<u8>)
    requires ordered(p), 0 <= j < p.len(), cmp_spec(p[j].start@,h) > 0,
        forall|x: int| 0 <= x < out.len() ==> cmp_spec(out[x].start@,h) <= 0
            || exists|z: int| 0 <= z < j && out[x].start@ == p[z].start@,
    ensures forall|x: int| 0 <= x < out.len() ==> cmp_spec(out[x].start@,p[j].start@) < 0,
{
    cmp_laws(p[j].start@,h);
    assert forall|x: int| 0 <= x < out.len() implies cmp_spec(out[x].start@,p[j].start@) < 0 by {
        if cmp_spec(out[x].start@,h) <= 0 { cmp_trans(out[x].start@,h,p[j].start@); }
        else {
            let z = choose|z: int| 0 <= z < j && out[x].start@ == p[z].start@;
            assert(cmp_spec(p[z].start@,p[j].start@) < 0);
        }
    }
}
pub proof fn suffix_complete(p: Seq<Boundary>, h: Seq<u8>, k: Seq<u8>)
    requires ordered(p), cmp_spec(h,k) <= 0,
    ensures suffix_route(p,h,k,route(p,h)) == route(p,k),
    decreases p.len(),
{
    if p.len() > 0 {
        if cmp_spec(h,p.last().start@) < 0 {
            cmp_laws(h,p.last().start@);
            if cmp_spec(p.last().start@,k) > 0 {
                assert(ordered(p.drop_last())) by {
                    assert forall|i: int,j: int| 0 <= i < j < p.drop_last().len() implies
                        cmp_spec(#[trigger] p.drop_last()[i].start@,#[trigger] p.drop_last()[j].start@) < 0 by {}
                }
                suffix_complete(p.drop_last(),h,k);
            }
        } else {
            cmp_laws(h,p.last().start@); cmp_trans(p.last().start@,h,k);
            suffix_constant(p,h,k,route(p,h));
        }
    }
}
pub proof fn suffix_constant(p: Seq<Boundary>, h: Seq<u8>, k: Seq<u8>, base: Grant)
    requires ordered(p), p.len() > 0 ==> cmp_spec(p.last().start@,h) <= 0,
    ensures suffix_route(p,h,k,base) == base,
    decreases p.len(),
{
    if p.len() > 0 {
        cmp_laws(p.last().start@,h);
        assert(ordered(p.drop_last())) by {
            assert forall|i: int,j: int| 0 <= i < j < p.drop_last().len() implies
                cmp_spec(#[trigger] p.drop_last()[i].start@,#[trigger] p.drop_last()[j].start@) < 0 by {}
        }
        if p.len() > 1 { cmp_trans(p.drop_last().last().start@,p.last().start@,h); }
        suffix_constant(p.drop_last(),h,k,base);
    }
}
} // verus!
