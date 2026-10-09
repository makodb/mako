//! Finite observed-coordinate refinement. Ranks are constructed from a finite
//! support; they are not an assumed encoding of the infinite byte-string order.
//! Including a query and both endpoints in the support proves every individual
//! native route observation against the independent scalar partition model.
use vstd::prelude::*;
use crate::types::{Boundary, Grant};
use crate::bytes::{cmp_spec, cmp_laws, cmp_trans};
use crate::directory::{route, ordered, wellformed, inside, proper};
use crate::sharding_partition as scalar;
verus! {
pub open spec fn sorted(s: Seq<Seq<u8>>) -> bool {
    forall|i: int,j: int| 0 <= i < j < s.len() ==> cmp_spec(#[trigger] s[i],#[trigger] s[j]) < 0
}
pub open spec fn insert(s: Seq<Seq<u8>>, k: Seq<u8>) -> Seq<Seq<u8>>
    decreases s.len(),
{
    if s.len() == 0 || cmp_spec(s.last(),k) < 0 { s.push(k) }
    else if s.last() == k { s }
    else { insert(s.drop_last(),k).push(s.last()) }
}
pub proof fn sorted_prefix(s: Seq<Seq<u8>>)
    requires sorted(s), s.len() > 0,
    ensures sorted(s.drop_last()),
{
    assert forall|i: int,j: int| 0 <= i < j < s.drop_last().len() implies
        cmp_spec(#[trigger] s.drop_last()[i],#[trigger] s.drop_last()[j]) < 0 by {}
}
pub proof fn sorted_push(s: Seq<Seq<u8>>, k: Seq<u8>)
    requires sorted(s), forall|i: int| 0 <= i < s.len() ==> cmp_spec(s[i],k) < 0,
    ensures sorted(s.push(k)),
{
    assert forall|i: int,j: int| 0 <= i < j < s.push(k).len() implies
        cmp_spec(#[trigger] s.push(k)[i],#[trigger] s.push(k)[j]) < 0 by {
        if j < s.len() { assert(s.push(k)[j] == s[j]); }
    }
}
proof fn inserted_shape(s: Seq<Seq<u8>>, k: Seq<u8>)
    requires sorted(s),
    ensures sorted(insert(s,k)), insert(s,k).len() > 0,
        insert(s,k).last() == if s.len() > 0 && cmp_spec(k,s.last()) < 0 { s.last() } else { k },
    decreases s.len(),
{
    if s.len() == 0 || cmp_spec(s.last(),k) < 0 {
        if s.len() > 0 {
            cmp_laws(s.last(),k);
            assert forall|i: int| 0 <= i < s.len() implies cmp_spec(s[i],k) < 0 by {
                if i < s.len()-1 { cmp_trans(s[i],s.last(),k); }
            }
        }
        sorted_push(s,k);
    } else if s.last() == k { cmp_laws(k,k); }
    else {
        cmp_laws(s.last(),k);
        sorted_prefix(s); inserted_shape(s.drop_last(),k);
        let q = insert(s.drop_last(),k);
        assert forall|i: int| 0 <= i < q.len() implies cmp_spec(q[i],s.last()) < 0 by {
            if s.len() > 1 { assert(cmp_spec(s.drop_last().last(),s.last()) < 0); }
            if i < q.len()-1 { cmp_trans(q[i],q.last(),s.last()); }
        }
        sorted_push(q,s.last());
    }
}
proof fn inserted_contains(s: Seq<Seq<u8>>, k: Seq<u8>, x: Seq<u8>)
    ensures insert(s,k).contains(x) == (s.contains(x) || x == k),
    decreases s.len(),
{
    if s.len() == 0 || cmp_spec(s.last(),k) < 0 {
        contains_push(s,k,x);
    } else if s.last() == k {
        assert(s[s.len()-1] == k);
        assert(s.contains(k));
    } else {
        inserted_contains(s.drop_last(),k,x);
        contains_push(s.drop_last(),s.last(),x);
        contains_push(insert(s.drop_last(),k),s.last(),x);
        assert(s.drop_last().push(s.last()) == s);
    }
}
pub proof fn inserted(s: Seq<Seq<u8>>, k: Seq<u8>)
    requires sorted(s),
    ensures sorted(insert(s,k)), insert(s,k).len() > 0,
        insert(s,k).last() == if s.len() > 0 && cmp_spec(k,s.last()) < 0 { s.last() } else { k },
        forall|x: Seq<u8>| #[trigger] insert(s,k).contains(x) == (s.contains(x) || x == k),
{
    inserted_shape(s,k);
    assert forall|x: Seq<u8>| insert(s,k).contains(x) == (s.contains(x) || x == k) by {
        inserted_contains(s,k,x);
    }
}
pub open spec fn support(keys: Seq<Seq<u8>>) -> Seq<Seq<u8>>
    decreases keys.len(),
{
    if keys.len() == 0 { seq![Seq::<u8>::empty()] }
    else { insert(support(keys.drop_last()),keys.last()) }
}
pub proof fn finite_support(keys: Seq<Seq<u8>>)
    ensures sorted(support(keys)), support(keys).len() > 0,
        support(keys)[0] == Seq::<u8>::empty(),
        forall|k: Seq<u8>| keys.contains(k) ==> #[trigger] support(keys).contains(k),
    decreases keys.len(),
{
    if keys.len() > 0 {
        finite_support(keys.drop_last()); inserted(support(keys.drop_last()),keys.last());
        assert(keys.drop_last().push(keys.last()) == keys);
        assert forall|k: Seq<u8>| keys.contains(k) implies #[trigger] support(keys).contains(k) by {
            contains_push(keys.drop_last(),keys.last(),k);
        }
        let s = support(keys);
        assert(s.contains(Seq::<u8>::empty()));
        let i = choose|i: int| 0 <= i < s.len() && s[i] == Seq::<u8>::empty();
        if i > 0 { cmp_laws(Seq::empty(),s[0]); cmp_laws(s[0],Seq::empty()); }
    }
}
pub proof fn contains_push(s: Seq<Seq<u8>>, k: Seq<u8>, x: Seq<u8>)
    ensures s.push(k).contains(x) == (s.contains(x) || x == k),
{
    if s.push(k).contains(x) {
        let i = choose|i: int| 0 <= i < s.push(k).len() && s.push(k)[i] == x;
        if i < s.len() { assert(s[i] == x); }
    }
    if s.contains(x) {
        let i = choose|i: int| 0 <= i < s.len() && s[i] == x;
        assert(s.push(k)[i] == x);
    }
    if x == k { assert(s.push(k)[s.len() as int] == x); }
}
pub open spec fn rank(s: Seq<Seq<u8>>, k: Seq<u8>) -> int {
    if s.contains(k) { choose|i: int| 0 <= i < s.len() && s[i] == k } else { 0 }
}
pub proof fn rank_order(s: Seq<Seq<u8>>, a: Seq<u8>, b: Seq<u8>)
    requires sorted(s), s.contains(a), s.contains(b),
    ensures 0 <= rank(s,a) < s.len(), s[rank(s,a)] == a,
        (rank(s,a) < rank(s,b)) == (cmp_spec(a,b) < 0),
        (rank(s,a) == rank(s,b)) == (a == b),
        (rank(s,a) <= rank(s,b)) == (cmp_spec(a,b) <= 0),
{
    let i = rank(s,a); let j = rank(s,b);
    cmp_laws(a,b);
    if i < j { assert(cmp_spec(s[i],s[j]) < 0); }
    if j < i { assert(cmp_spec(s[j],s[i]) < 0); }
}
/// A scalar shard names the complete grant, not only its owner. Equal encoded
/// shards are therefore exactly the grants which native coalescing may merge.
pub open spec fn grant_id(g: Grant) -> int { g.owner as int * 18446744073709551616 + g.epoch as int }
pub proof fn grant_id_injective(a: Grant,b: Grant)
    ensures grant_id(a) >= 0, (grant_id(a) == grant_id(b)) == (a == b),
{
    if a.owner < b.owner { assert(grant_id(a) < grant_id(b)); }
    if b.owner < a.owner { assert(grant_id(b) < grant_id(a)); }
}
pub open spec fn represented(p: Seq<Boundary>, s: Seq<Seq<u8>>) -> bool {
    sorted(s) && s.len() > 0 && s[0] == Seq::<u8>::empty()
        && forall|i: int| 0 <= i < p.len() ==> s.contains(#[trigger] p[i].start@)
}
pub open spec fn image(p: Seq<Boundary>, s: Seq<Seq<u8>>) -> Seq<scalar::Segment> {
    Seq::new(p.len(),|i: int| scalar::Segment { start: rank(s,p[i].start@), shard: grant_id(p[i].grant) })
}
pub proof fn image_shape(p: Seq<Boundary>, s: Seq<Seq<u8>>)
    requires wellformed(p), represented(p,s),
    ensures scalar::wellformed(image(p,s)),
{
    let q = image(p,s);
    rank_order(s,Seq::empty(),Seq::empty());
    if rank(s,Seq::empty()) > 0 { assert(cmp_spec(s[0],s[rank(s,Seq::empty())]) < 0); cmp_laws(Seq::empty(),Seq::empty()); }
    assert forall|i: int,j: int| 0 <= i < j < q.len() implies #[trigger] q[i].start < #[trigger] q[j].start by {
        rank_order(s,p[i].start@,p[j].start@);
    }
    assert forall|i: int| 0 <= i < q.len() implies #[trigger] q[i].shard >= 0 by { grant_id_injective(p[i].grant,p[i].grant); }
}
pub proof fn image_route(p: Seq<Boundary>, s: Seq<Seq<u8>>, k: Seq<u8>)
    requires represented(p,s), s.contains(k),
    ensures scalar::route(image(p,s),rank(s,k)) == grant_id(route(p,k)),
    decreases p.len(),
{
    if p.len() > 0 {
        rank_order(s,p.last().start@,k);
        if cmp_spec(p.last().start@,k) > 0 {
            assert(represented(p.drop_last(),s)) by {
                assert forall|i: int| 0 <= i < p.drop_last().len() implies s.contains(#[trigger] p.drop_last()[i].start@) by {}
            }
            assert(image(p.drop_last(),s) =~= image(p,s).drop_last());
            image_route(p.drop_last(),s,k);
        }
    }
}
pub open spec fn image_hi(s: Seq<Seq<u8>>, hi: Option<Seq<u8>>) -> Option<int> {
    match hi { Some(h) => Some(rank(s,h)), None => None }
}
/// The executable move_range postcondition supplies the final premise. Any
/// finite observation support can be extended with the old/new boundaries,
/// endpoints and query by finite_support; no global byte-to-integer codec exists
/// in this refinement. Epochs remain distinguished by grant_id.
pub proof fn replacement_refines(p: Seq<Boundary>, q: Seq<Boundary>, s: Seq<Seq<u8>>,
    lo: Seq<u8>, hi: Option<Seq<u8>>, dst: Grant, k: Seq<u8>)
    requires wellformed(p), represented(p,s), represented(q,s), s.contains(lo), s.contains(k),
        match hi { Some(h) => s.contains(h), None => true }, proper(lo,hi),
        route(q,k) == if inside(k,lo,hi) { dst } else { route(p,k) },
    ensures scalar::route(image(q,s),rank(s,k)) == scalar::route(
        scalar::reassign(image(p,s),rank(s,lo),image_hi(s,hi),grant_id(dst)),rank(s,k)),
{
    image_shape(p,s); image_route(p,s,k); image_route(q,s,k);
    rank_order(s,lo,k); grant_id_injective(dst,dst);
    if let Some(h) = hi { rank_order(s,lo,h); rank_order(s,k,h); }
    scalar::reassign_partition_theorem(image(p,s),rank(s,lo),image_hi(s,hi),grant_id(dst),rank(s,k));
}
pub open spec fn observation_keys(p: Seq<Boundary>, q: Seq<Boundary>,
    lo: Seq<u8>, hi: Option<Seq<u8>>, k: Seq<u8>) -> Seq<Seq<u8>> {
    Seq::new(p.len(),|i: int| p[i].start@) + Seq::new(q.len(),|i: int| q[i].start@)
        + seq![lo,k] + match hi { Some(h) => seq![h], None => Seq::empty() }
}
pub proof fn observation_support(p: Seq<Boundary>, q: Seq<Boundary>,
    lo: Seq<u8>, hi: Option<Seq<u8>>, k: Seq<u8>) -> (s: Seq<Seq<u8>>)
    ensures s == support(observation_keys(p,q,lo,hi,k)),
        represented(p,s), represented(q,s), s.contains(lo), s.contains(k),
        match hi { Some(h) => s.contains(h), None => true },
{
    let keys = observation_keys(p,q,lo,hi,k);
    finite_support(keys);
    let s = support(keys);
    assert(keys[p.len() as int + q.len() as int] == lo);
    assert(keys[p.len() as int + q.len() as int + 1] == k);
    if let Some(h) = hi { assert(keys.last() == h); }
    assert forall|i: int| 0 <= i < p.len() implies s.contains(#[trigger] p[i].start@) by {
        assert(keys[i] == p[i].start@);
    }
    assert forall|i: int| 0 <= i < q.len() implies s.contains(#[trigger] q[i].start@) by {
        assert(keys[p.len() as int + i] == q[i].start@);
    }
    s
}
pub proof fn replacement_observation(p: Seq<Boundary>, q: Seq<Boundary>,
    lo: Seq<u8>, hi: Option<Seq<u8>>, dst: Grant, k: Seq<u8>)
    requires wellformed(p), proper(lo,hi),
        route(q,k) == if inside(k,lo,hi) { dst } else { route(p,k) },
    ensures ({
        let s = support(observation_keys(p,q,lo,hi,k));
        scalar::route(image(q,s),rank(s,k)) == scalar::route(
            scalar::reassign(image(p,s),rank(s,lo),image_hi(s,hi),grant_id(dst)),rank(s,k))
    }),
{
    let s = observation_support(p,q,lo,hi,k);
    replacement_refines(p,q,s,lo,hi,dst,k);
}
} // verus!
