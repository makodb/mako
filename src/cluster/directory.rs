//! Immutable per-table routing snapshots. Validation precedes all allocation.
use vstd::prelude::*;
use crate::types::{Boundary, Grant, Status};
use crate::bytes::{compare, copy_bytes};
#[cfg(verus_keep_ghost)]
use crate::bytes::{cmp_spec, cmp_laws};
use crate::directory_proofs::*;
verus! {
pub struct RouteTable { pub table: u64, pub boundaries: Vec<Boundary> }
pub open spec fn ordered(p: Seq<Boundary>) -> bool {
    forall|i: int, j: int| 0 <= i < j < p.len() ==>
        cmp_spec(#[trigger] p[i].start@, #[trigger] p[j].start@) < 0
}
pub open spec fn wellformed(p: Seq<Boundary>) -> bool {
    p.len() > 0 && p[0].start@ == Seq::<u8>::empty() && ordered(p)
}
pub open spec fn canonical(p: Seq<Boundary>) -> bool {
    forall|i: int| 0 < i < p.len() ==> #[trigger] p[i].grant != p[i-1].grant
}
pub open spec fn route(p: Seq<Boundary>, k: Seq<u8>) -> Grant
    decreases p.len(),
{
    if p.len() == 0 { Grant { owner: 0, epoch: 0 } }
    else if cmp_spec(p.last().start@,k) <= 0 { p.last().grant }
    else { route(p.drop_last(),k) }
}
pub open spec fn inside(k: Seq<u8>, lo: Seq<u8>, hi: Option<Seq<u8>>) -> bool {
    cmp_spec(lo,k) <= 0 && match hi { Some(h) => cmp_spec(k,h) < 0, None => true }
}
pub open spec fn proper(lo: Seq<u8>, hi: Option<Seq<u8>>) -> bool {
    match hi { Some(h) => cmp_spec(lo,h) < 0, None => true }
}
pub open spec fn source_owned(p: Seq<Boundary>, lo: Seq<u8>, hi: Option<Seq<u8>>, src: u32) -> bool {
    forall|k: Seq<u8>| inside(k,lo,hi) ==> #[trigger] route(p,k).owner == src
}
pub open spec fn hi_view(hi: Option<&[u8]>) -> Option<Seq<u8>> {
    match hi { Some(h) => Some(h@), None => None }
}
impl RouteTable {
    pub fn new(table: u64, owner: u32) -> (r: Self)
        ensures r.table == table, wellformed(r.boundaries@), canonical(r.boundaries@),
            forall|k: Seq<u8>| #[trigger] route(r.boundaries@,k) == (Grant { owner, epoch: 0 }),
    {
        let mut boundaries = Vec::new();
        boundaries.push(Boundary { start: Vec::new(), grant: Grant { owner, epoch: 0 } });
        proof { assert forall|k: Seq<u8>| route(boundaries@,k) == (Grant { owner, epoch: 0 }) by { cmp_laws(Seq::empty(),k); } }
        Self { table, boundaries }
    }
    pub fn lookup(&self, key: &[u8]) -> (g: Grant)
        requires wellformed(self.boundaries@),
        ensures g == route(self.boundaries@,key@),
    {
        let mut lower = 1usize;
        let mut upper = self.boundaries.len();
        proof { cmp_laws(Seq::empty(),key@); }
        while lower < upper
            invariant 1 <= lower <= upper <= self.boundaries.len(), wellformed(self.boundaries@),
                forall|j: int| 0 <= j < lower ==> cmp_spec(self.boundaries@[j].start@,key@) <= 0,
                forall|j: int| upper <= j < self.boundaries.len() ==> cmp_spec(self.boundaries@[j].start@,key@) > 0,
            decreases upper - lower,
        {
            let mid = lower + (upper - lower) / 2;
            let b = &self.boundaries[mid];
            if compare(&b.start,key) <= 0 {
                proof {
                    assert forall|j: int| 0 <= j <= mid implies cmp_spec(self.boundaries@[j].start@,key@) <= 0 by {
                        if j < mid { crate::bytes::cmp_trans(self.boundaries@[j].start@,b.start@,key@); }
                    }
                }
                lower = mid + 1;
            } else {
                proof {
                    cmp_laws(b.start@,key@);
                    assert forall|j: int| mid <= j < self.boundaries.len() implies cmp_spec(self.boundaries@[j].start@,key@) > 0 by {
                        if j > mid {
                            crate::bytes::cmp_trans(key@,b.start@,self.boundaries@[j].start@);
                            cmp_laws(key@,self.boundaries@[j].start@);
                        }
                    }
                }
                upper = mid;
            }
        }
        proof { selected_route(self.boundaries@,lower as int - 1,key@); }
        self.boundaries[lower - 1].grant
    }
    pub fn move_range(&self, lo: &[u8], hi: Option<&[u8]>, src: u32, dst: u32, epoch: u64) -> (r: Result<Self,Status>)
        requires wellformed(self.boundaries@), canonical(self.boundaries@),
        ensures
            r.is_ok() == (src != dst && proper(lo@,hi_view(hi)) && source_owned(self.boundaries@,lo@,hi_view(hi),src)),
            match r {
                Ok(q) => q.table == self.table && wellformed(q.boundaries@) && canonical(q.boundaries@)
                    && forall|k: Seq<u8>| #[trigger] route(q.boundaries@,k) ==
                        if inside(k,lo@,hi_view(hi)) { Grant { owner: dst, epoch } } else { route(self.boundaries@,k) },
                Err(_) => true,
            },
    {
        if src == dst { return Err(Status::Invalid); }
        if let Some(h) = hi { if compare(lo,h) >= 0 { return Err(Status::Invalid); } }
        if !self.owns_range(lo,hi,src) { return Err(Status::Retry); }
        let mut out = self.prefix_replacement(lo,Grant { owner: dst, epoch });
        if let Some(h) = hi {
            proof { all_starts_below(out@,lo@,h@); }
            self.restore_suffix(&mut out,h);
        }
        proof {
            assert forall|k: Seq<u8>| route(out@,k) ==
                if inside(k,lo@,hi_view(hi)) { Grant { owner: dst, epoch } } else { route(self.boundaries@,k) } by {
                if let Some(h) = hi {
                    if cmp_spec(k,h@) >= 0 { cmp_laws(k,h@); crate::bytes::cmp_trans(lo@,h@,k); }
                }
            }
        }
        Ok(Self { table: self.table, boundaries: out })
    }
    fn owns_range(&self, lo: &[u8], hi: Option<&[u8]>, src: u32) -> (yes: bool)
        requires wellformed(self.boundaries@), proper(lo@,hi_view(hi)),
        ensures yes == source_owned(self.boundaries@,lo@,hi_view(hi),src),
    {
        let at_lo = self.lookup(lo);
        if at_lo.owner != src {
            proof { cmp_laws(lo@,lo@); assert(inside(lo@,lo@,hi_view(hi))); }
            return false;
        }
        let mut i = 0usize;
        while i < self.boundaries.len()
            invariant i <= self.boundaries.len(), wellformed(self.boundaries@),
                proper(lo@,hi_view(hi)), route(self.boundaries@,lo@).owner == src,
                forall|j: int| 0 <= j < i && inside(self.boundaries@[j].start@,lo@,hi_view(hi)) ==>
                    self.boundaries@[j].grant.owner == src,
            decreases self.boundaries.len() - i,
        {
            let b = &self.boundaries[i];
            let above_lo = compare(&b.start,lo) >= 0;
            let below_hi = match hi { Some(h) => compare(&b.start,h) < 0, None => true };
            proof { cmp_laws(b.start@,lo@); }
            if above_lo && below_hi && b.grant.owner != src {
                proof { route_at_boundary(self.boundaries@,i as int); }
                return false;
            }
            i += 1;
        }
        proof { uniform_source(self.boundaries@,lo@,hi_view(hi),src); }
        true
    }
    fn prefix_replacement(&self, lo: &[u8], grant: Grant) -> (out: Vec<Boundary>)
        requires wellformed(self.boundaries@), canonical(self.boundaries@),
        ensures wellformed(out@), canonical(out@),
            forall|x: int| 0 <= x < out.len() ==> cmp_spec(out@[x].start@,lo@) <= 0,
            forall|k: Seq<u8>| #[trigger] route(out@,k) ==
                if cmp_spec(lo@,k) <= 0 { grant } else { route(self.boundaries@,k) },
    {
        let (mut out,n) = self.copy_prefix(lo);
        proof { copied_prefix(self.boundaries@,out@,n as int,lo@); }
        append(&mut out,lo,grant);
        out
    }
    fn copy_prefix(&self,lo: &[u8]) -> (r: (Vec<Boundary>,usize))
        requires wellformed(self.boundaries@), canonical(self.boundaries@),
        ensures r.1 <= self.boundaries.len(), r.0.len() == r.1,
            equivalent(r.0@,self.boundaries@.take(r.1 as int)),
            forall|j: int| 0 <= j < r.1 ==> cmp_spec(self.boundaries@[j].start@,lo@) < 0,
            r.1 < self.boundaries.len() ==> cmp_spec(self.boundaries@[r.1 as int].start@,lo@) >= 0,
    {
        let mut out: Vec<Boundary> = Vec::new();
        let mut n = 0usize;
        while n < self.boundaries.len() && compare(&self.boundaries[n].start,lo) < 0
            invariant n <= self.boundaries.len(), out.len() == n,
                wellformed(self.boundaries@), canonical(self.boundaries@),
                equivalent(out@,self.boundaries@.take(n as int)),
                forall|j: int| 0 <= j < n ==> cmp_spec(self.boundaries@[j].start@,lo@) < 0,
            decreases self.boundaries.len() - n,
        {
            let b = &self.boundaries[n];
            out.push(Boundary { start: copy_bytes(&b.start), grant: b.grant });
            n += 1;
            proof { assert(equivalent(out@,self.boundaries@.take(n as int))); }
        }
        (out,n)
    }
    fn restore_suffix(&self, out: &mut Vec<Boundary>, h: &[u8])
        requires wellformed(self.boundaries@), wellformed(old(out)@), canonical(old(out)@),
            forall|x: int| 0 <= x < old(out).len() ==> cmp_spec(old(out)@[x].start@,h@) < 0,
        ensures wellformed(final(out)@), canonical(final(out)@),
            forall|k: Seq<u8>| #[trigger] route(final(out)@,k) ==
                if cmp_spec(k,h@) < 0 { route(old(out)@,k) } else { route(self.boundaries@,k) },
    {
            let ghost initial = out@;
            let original = self.lookup(h);
            append(out,h,original);
            proof {
                assert forall|k: Seq<u8>| route(out@,k) ==
                    if cmp_spec(k,h@) < 0 { route(initial,k) } else { original } by { cmp_laws(k,h@); }
            }
            let mut j = 0usize;
            while j < self.boundaries.len()
                invariant j <= self.boundaries.len(), wellformed(out@), canonical(out@), wellformed(self.boundaries@),
                    original == route(self.boundaries@,h@),
                    forall|k: Seq<u8>| #[trigger] route(out@,k) ==
                        if cmp_spec(k,h@) < 0 {
                            route(initial,k)
                        } else { suffix_route(self.boundaries@.take(j as int),h@,k,original) },
                    forall|x: int| 0 <= x < out.len() ==> cmp_spec(out@[x].start@,h@) <= 0
                        || exists|z: int| 0 <= z < j && out@[x].start@ == self.boundaries@[z].start@,
                decreases self.boundaries.len() - j,
            {
                let ghost before = out@;
                let b = &self.boundaries[j];
                if compare(&b.start,h) > 0 {
                    proof { suffix_append_pre(self.boundaries@,out@,j as int,h@); }
                    append(out,&b.start,b.grant);
                }
                proof {
                    assert(self.boundaries@.take(j as int + 1).drop_last() == self.boundaries@.take(j as int));
                    assert(self.boundaries@.take(j as int + 1).last() == *b);
                    assert forall|k: Seq<u8>| route(out@,k) ==
                        if cmp_spec(k,h@) < 0 {
                            route(initial,k)
                        } else { suffix_route(self.boundaries@.take(j as int + 1),h@,k,original) } by {
                        cmp_laws(b.start@,h@);
                        if cmp_spec(k,h@) < 0 && cmp_spec(h@,b.start@) < 0 {
                            crate::bytes::cmp_trans(k,h@,b.start@); cmp_laws(k,b.start@);
                        }
                    }
                    assert forall|x: int| 0 <= x < out.len() implies cmp_spec(out@[x].start@,h@) <= 0
                        || exists|z: int| 0 <= z < j + 1 && out@[x].start@ == self.boundaries@[z].start@ by {
                        if x < before.len() {
                            if cmp_spec(out@[x].start@,h@) > 0 {
                                let z = choose|z: int| 0 <= z < j && before[x].start@ == self.boundaries@[z].start@;
                                assert(out@[x].start@ == self.boundaries@[z].start@);
                            }
                        } else { assert(out@[x].start@ == self.boundaries@[j as int].start@); }
                    }
                }
                j += 1;
            }
            proof {
                assert(self.boundaries@.take(j as int) == self.boundaries@);
                assert forall|k: Seq<u8>| route(out@,k) ==
                    if cmp_spec(k,h@) < 0 { route(initial,k) } else { route(self.boundaries@,k) } by {
                    if cmp_spec(k,h@) >= 0 { cmp_laws(k,h@); suffix_complete(self.boundaries@,h@,k); }
                }
            }
    }
}
} // verus!
