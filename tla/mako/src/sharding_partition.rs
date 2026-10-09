//! Ordered-partition metadata abstraction of config_manager.h:1106-1161 and
//! cluster_config.h:101-125 at integrated source 44c6b5d5c278a2916a3112cf2fa4d6d7c2a4691f.
//! Nonnegative integer keys order-embed a finite set of observed byte-string keys,
//! with 0 the empty-string minimum. This is not a byte-encoding implementation.
//! None is the source's empty upper bound (+infinity), not the key 0. Mathematical
//! lengths/shard integers abstract allocation and machine-width conversions;
//! neither overflow nor arbitrary-byte-string C++ refinement is proved here.
//! Sorted insertion replaces append-and-sort; owners at inserted boundaries are
//! read from the original routing function. Remapping tests segment STARTS, then
//! coalescing removes adjacent equal owners. No desired route is a definition of
//! the transformation. These are conditional metadata theorems, not C++ refinement
//! or data-migration safety; valid ranges/partitions are hypotheses, not API guards.
use vstd::prelude::*;

verus! {

pub struct Segment { pub start: int, pub shard: int }

pub open spec fn ordered(p: Seq<Segment>) -> bool {
    forall|i: int, j: int| 0 <= i < j < p.len() ==>
        #[trigger] p[i].start < #[trigger] p[j].start
}
pub open spec fn wellformed(p: Seq<Segment>) -> bool {
    p.len() > 0 && p[0].start == 0 && ordered(p)
        && forall|i: int| 0 <= i < p.len() ==> #[trigger] p[i].shard >= 0
}
/// On an ordered covering sequence, the greatest start at or below k wins.
pub open spec fn route(p: Seq<Segment>, k: int) -> int
    decreases p.len(),
{
    if p.len() == 0 { 0 }
    else if p.last().start <= k { p.last().shard }
    else { route(p.drop_last(), k) }
}
pub open spec fn boundary(p: Seq<Segment>, x: int) -> bool {
    exists|i: int| 0 <= i < p.len() && #[trigger] p[i].start == x
}
pub open spec fn in_range(k: int, lo: int, hi: Option<int>) -> bool {
    lo <= k && match hi { Some(h) => k < h, None => true }
}
pub open spec fn valid_range(lo: int, hi: Option<int>) -> bool {
    0 <= lo && match hi { Some(h) => lo < h, None => true }
}

pub open spec fn insert_boundary(p: Seq<Segment>, x: int, owner: int) -> Seq<Segment>
    decreases p.len(),
{
    if p.len() == 0 || p.last().start < x {
        p.push(Segment { start: x, shard: owner })
    } else if p.last().start == x { p }
    else { insert_boundary(p.drop_last(), x, owner).push(p.last()) }
}
pub open spec fn remap(p: Seq<Segment>, lo: int, hi: Option<int>, dest: int) -> Seq<Segment> {
    Seq::new(p.len(), |i: int| Segment {
        start: p[i].start,
        shard: if in_range(p[i].start, lo, hi) { dest } else { p[i].shard },
    })
}
pub open spec fn coalesce(p: Seq<Segment>) -> Seq<Segment>
    decreases p.len(),
{
    if p.len() == 0 { p }
    else {
        let q = coalesce(p.drop_last());
        if q.len() > 0 && q.last().shard == p.last().shard { q }
        else { q.push(p.last()) }
    }
}
pub open spec fn materialize(p: Seq<Segment>, lo: int, hi: Option<int>) -> Seq<Segment> {
    let q = insert_boundary(p, lo, route(p, lo));
    match hi { Some(h) => insert_boundary(q, h, route(p, h)), None => q }
}
pub open spec fn reassign(p: Seq<Segment>, lo: int, hi: Option<int>, dest: int) -> Seq<Segment> {
    coalesce(remap(materialize(p, lo, hi), lo, hi, dest))
}

proof fn ordered_prefix(p: Seq<Segment>)
    requires ordered(p), p.len() > 0,
    ensures ordered(p.drop_last()),
{
    assert forall|i: int, j: int| 0 <= i < j < p.drop_last().len() implies
        #[trigger] p.drop_last()[i].start < #[trigger] p.drop_last()[j].start by {
        assert(p.drop_last()[i] == p[i]);
        assert(p.drop_last()[j] == p[j]);
    }
}
proof fn ordered_append(p: Seq<Segment>, s: Segment)
    requires ordered(p), forall|i: int| 0 <= i < p.len() ==> #[trigger] p[i].start < s.start,
    ensures ordered(p.push(s)),
{
    assert forall|i: int, j: int| 0 <= i < j < p.push(s).len() implies
        #[trigger] p.push(s)[i].start < #[trigger] p.push(s)[j].start by {
        if j < p.len() { assert(p.push(s)[j] == p[j]); }
        else { assert(p.push(s)[j] == s); }
        assert(p.push(s)[i] == p[i]);
    }
}

/// Insertion retains every old boundary, adds x once, and preserves all routes
/// when its owner is the old owner at x. This theorem is unbounded in p.len().
pub proof fn insertion_preserves_routes(p: Seq<Segment>, x: int)
    requires wellformed(p), x >= 0,
    ensures
        wellformed(insert_boundary(p, x, route(p, x))),
        forall|k: int| #[trigger] route(insert_boundary(p, x, route(p, x)), k) == route(p, k),
        forall|b: int| #[trigger] boundary(insert_boundary(p, x, route(p, x)), b)
            == (boundary(p, b) || b == x),
{
    route_nonnegative(p, x);
    insertion_shape(p, x, route(p, x));
    assert forall|k: int| #[trigger] route(insert_boundary(p, x, route(p, x)), k) == route(p, k) by {
        insertion_route(p, x, k);
    }
    assert forall|b: int| #[trigger] boundary(insert_boundary(p, x, route(p, x)), b)
        == (boundary(p, b) || b == x) by {
        insertion_boundaries(p, x, route(p, x), b);
    }
}

proof fn route_nonnegative(p: Seq<Segment>, k: int)
    requires forall|i: int| 0 <= i < p.len() ==> #[trigger] p[i].shard >= 0,
    ensures route(p, k) >= 0,
    decreases p.len(),
{
    if p.len() > 0 && p.last().start > k {
        route_nonnegative(p.drop_last(), k);
    }
}

proof fn insertion_shape(p: Seq<Segment>, x: int, owner: int)
    requires ordered(p), owner >= 0,
        forall|i: int| 0 <= i < p.len() ==> #[trigger] p[i].shard >= 0,
    ensures
        ordered(insert_boundary(p, x, owner)),
        insert_boundary(p, x, owner).len() > 0,
        insert_boundary(p, x, owner).last().start ==
            if p.len() > 0 && p.last().start > x { p.last().start } else { x },
        p.len() > 0 && p[0].start <= x ==>
            insert_boundary(p, x, owner)[0].start == p[0].start,
        forall|i: int| 0 <= i < insert_boundary(p, x, owner).len() ==>
            #[trigger] insert_boundary(p, x, owner)[i].shard >= 0,
    decreases p.len(),
{
    let z = insert_boundary(p, x, owner);
    if p.len() == 0 || p.last().start < x {
        assert forall|i: int| 0 <= i < p.len() implies #[trigger] p[i].start < x by {
            if i < p.len() - 1 { assert(p[i].start < p.last().start); }
        }
        ordered_append(p, Segment { start: x, shard: owner });
    } else if p.last().start != x {
        let t = p.drop_last();
        ordered_prefix(p);
        insertion_shape(t, x, owner);
        let q = insert_boundary(t, x, owner);
        assert forall|i: int| 0 <= i < q.len() implies #[trigger] q[i].start < p.last().start by {
            if i < q.len() - 1 { assert(q[i].start < q.last().start); }
            if t.len() > 0 { assert(t.last().start < p.last().start); }
        }
        ordered_append(q, p.last());
        assert forall|i: int| 0 <= i < z.len() implies #[trigger] z[i].shard >= 0 by {
            if i < q.len() { assert(z[i] == q[i]); }
        }
    }
}

proof fn insertion_boundaries(p: Seq<Segment>, x: int, owner: int, b: int)
    ensures boundary(insert_boundary(p, x, owner), b) == (boundary(p, b) || b == x),
    decreases p.len(),
{
    let z = insert_boundary(p, x, owner);
    if p.len() == 0 || p.last().start < x {
        boundary_push(p, Segment { start: x, shard: owner }, b);
    } else if p.last().start == x {
        assert(boundary(p, x));
    } else {
        insertion_boundaries(p.drop_last(), x, owner, b);
        boundary_push(insert_boundary(p.drop_last(), x, owner), p.last(), b);
        assert(p.drop_last().push(p.last()) == p);
        boundary_push(p.drop_last(), p.last(), b);
    }
}
proof fn boundary_push(p: Seq<Segment>, s: Segment, b: int)
    ensures boundary(p.push(s), b) == (boundary(p, b) || s.start == b),
{
    let z = p.push(s);
    if boundary(z, b) {
        let i = choose|i: int| 0 <= i < z.len() && z[i].start == b;
        if i < p.len() { assert(p[i].start == b); }
    }
    if boundary(p, b) {
        let i = choose|i: int| 0 <= i < p.len() && p[i].start == b;
        assert(z[i].start == b);
    }
    if b == s.start { assert(z[p.len() as int].start == b); }
}

proof fn insertion_route(p: Seq<Segment>, x: int, k: int)
    requires ordered(p),
    ensures route(insert_boundary(p, x, route(p, x)), k) == route(p, k),
    decreases p.len(),
{
    let z = insert_boundary(p, x, route(p, x));
    if p.len() == 0 || p.last().start < x {
        assert(z.drop_last() == p);
    } else if p.last().start > x {
        ordered_prefix(p);
        insertion_route(p.drop_last(), x, k);
        let q = insert_boundary(p.drop_last(), x, route(p, x));
        assert(z.last() == p.last());
        assert(z.drop_last() == q);
        assert(route(q, k) == route(p.drop_last(), k));
    }
}

pub open spec fn selects(p: Seq<Segment>, i: int, k: int) -> bool {
    0 <= i < p.len() && p[i].start <= k
        && forall|j: int| i < j < p.len() ==> #[trigger] p[j].start > k
}
/// Coverage and greatest-start lookup, without a fixed bound on segments or keys.
pub proof fn greatest_start_exists(p: Seq<Segment>, k: int)
    requires wellformed(p), k >= 0,
    ensures exists|i: int| selects(p, i, k),
    decreases p.len(),
{
    if p.last().start <= k { assert(selects(p, p.len() as int - 1, k)); }
    else {
        ordered_prefix(p);
        assert(p.len() > 1);
        assert(wellformed(p.drop_last()));
        greatest_start_exists(p.drop_last(), k);
        let i = choose|i: int| selects(p.drop_last(), i, k);
        assert forall|j: int| i < j < p.len() implies #[trigger] p[j].start > k by {
            if j < p.len() - 1 { assert(p.drop_last()[j] == p[j]); }
        }
        assert(selects(p, i, k));
    }
}
pub proof fn route_at_selected(p: Seq<Segment>, i: int, k: int)
    requires selects(p, i, k),
    ensures route(p, k) == p[i].shard,
    decreases p.len(),
{
    if i < p.len() - 1 {
        assert(p.last().start > k);
        assert(selects(p.drop_last(), i, k));
        route_at_selected(p.drop_last(), i, k);
    }
}

pub proof fn remap_routes(p: Seq<Segment>, lo: int, hi: Option<int>, dest: int, k: int)
    requires wellformed(p), valid_range(lo, hi), dest >= 0, k >= 0,
        boundary(p, lo), match hi { Some(h) => boundary(p, h), None => true },
    ensures wellformed(remap(p, lo, hi, dest)),
        route(remap(p, lo, hi, dest), k) == if in_range(k, lo, hi) { dest } else { route(p, k) },
{
    let z = remap(p, lo, hi, dest);
    assert(ordered(z)) by {
        assert forall|i: int, j: int| 0 <= i < j < z.len() implies
            #[trigger] z[i].start < #[trigger] z[j].start by {
            assert(z[i].start == p[i].start);
            assert(z[j].start == p[j].start);
        }
    }
    greatest_start_exists(p, k);
    let i = choose|i: int| selects(p, i, k);
    route_at_selected(p, i, k);
    assert(selects(z, i, k)) by {
        assert forall|j: int| i < j < z.len() implies #[trigger] z[j].start > k by {
            assert(z[j].start == p[j].start);
        }
    }
    route_at_selected(z, i, k);
    let a = choose|a: int| 0 <= a < p.len() && p[a].start == lo;
    if lo <= k && p[i].start < lo {
        assert(a > i);
        assert(p[a].start > k);
    }
    if let Some(h) = hi {
        let b = choose|b: int| 0 <= b < p.len() && p[b].start == h;
        if h <= k && p[i].start < h {
            assert(b > i);
            assert(p[b].start > k);
        }
    }
    assert(in_range(p[i].start, lo, hi) == in_range(k, lo, hi));
}

/// Coalescing retains the first boundary of an equal-owner run and preserves
/// every route. The proof covers empty intermediate prefixes as well.
proof fn coalescing_shape(p: Seq<Segment>)
    requires ordered(p),
    ensures
        ordered(coalesce(p)),
        (p.len() == 0) == (coalesce(p).len() == 0),
        p.len() > 0 ==> coalesce(p)[0].start == p[0].start
            && coalesce(p).last().shard == p.last().shard
            && coalesce(p).last().start <= p.last().start,
        (forall|i: int| 0 <= i < p.len() ==> #[trigger] p[i].shard >= 0)
            ==> (forall|i: int| 0 <= i < coalesce(p).len() ==>
                #[trigger] coalesce(p)[i].shard >= 0),
        forall|i: int| 0 <= i < coalesce(p).len() - 1 ==>
            #[trigger] coalesce(p)[i].shard != coalesce(p)[i + 1].shard,
    decreases p.len(),
{
    if p.len() > 0 {
        let t = p.drop_last();
        ordered_prefix(p);
        coalescing_shape(t);
        let q = coalesce(t);
        let z = coalesce(p);
        if q.len() > 0 && q.last().shard == p.last().shard {
            assert(q.last().start <= p.last().start);
        } else {
            assert forall|i: int| 0 <= i < q.len() implies #[trigger] q[i].start < p.last().start by {
                if i < q.len() - 1 { assert(q[i].start < q.last().start); }
                assert(q.last().start <= t.last().start);
                assert(t.last().start < p.last().start);
            }
            ordered_append(q, p.last());
            assert forall|i: int| 0 <= i < z.len() - 1 implies
                #[trigger] z[i].shard != z[i + 1].shard by {
                assert(z[i] == q[i]);
                if i + 1 < q.len() { assert(z[i + 1] == q[i + 1]); }
                else { assert(z[i + 1] == p.last()); }
            }
        }
    }
}

pub proof fn coalescing_preserves_routes(p: Seq<Segment>)
    requires ordered(p),
    ensures
        wellformed(p) ==> wellformed(coalesce(p)),
        forall|k: int| #[trigger] route(coalesce(p), k) == route(p, k),
        forall|i: int| 0 <= i < coalesce(p).len() - 1 ==>
            #[trigger] coalesce(p)[i].shard != coalesce(p)[i + 1].shard,
{
    coalescing_shape(p);
    assert forall|k: int| #[trigger] route(coalesce(p), k) == route(p, k) by {
        coalescing_route(p, k);
    }
}
proof fn coalescing_route(p: Seq<Segment>, k: int)
    requires ordered(p),
    ensures route(coalesce(p), k) == route(p, k),
    decreases p.len(),
{
    if p.len() > 0 {
        ordered_prefix(p);
        coalescing_shape(p.drop_last());
        coalescing_route(p.drop_last(), k);
        let q = coalesce(p.drop_last());
        let z = coalesce(p);
        if q.len() > 0 && q.last().shard == p.last().shard {
            assert(q.last().start <= p.last().start);
            if k >= p.last().start { assert(route(q, k) == q.last().shard); }
        } else {
            assert(z.last() == p.last());
            assert(z.drop_last() == q);
        }
    }
}

/// The actual boundary-insert / segment-remap / coalesce transformation assigns
/// exactly the finite interval or suffix requested; all other routes are unchanged.
pub proof fn reassign_partition_theorem(p: Seq<Segment>, lo: int, hi: Option<int>, dest: int, k: int)
    requires wellformed(p), valid_range(lo, hi), dest >= 0, k >= 0,
    ensures wellformed(reassign(p, lo, hi, dest)),
        route(reassign(p, lo, hi, dest), k) ==
            if in_range(k, lo, hi) { dest } else { route(p, k) },
{
    insertion_preserves_routes(p, lo);
    let q = insert_boundary(p, lo, route(p, lo));
    if let Some(h) = hi { insertion_preserves_routes(q, h); }
    let m = materialize(p, lo, hi);
    assert(wellformed(m));
    assert(boundary(m, lo));
    remap_routes(m, lo, hi, dest, k);
    coalescing_preserves_routes(remap(m, lo, hi, dest));
}

} // verus!
