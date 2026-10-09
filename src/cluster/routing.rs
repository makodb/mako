//! Whole immutable routing observations and allocation-free full-grant scans.
//! Installation runs under the native owner's mutex. Clone `snapshot_arc` under
//! that lock, then release it before invoking scan callbacks: one retained Arc
//! pins every boundary and the version across blocking/reentrant native calls.
//! Initial version zero is valid. Later committed generations may skip aborted
//! generations; stale versions return Retry, equal identical versions stutter,
//! and conflicting equal versions return Invalid. The first accepted snapshot
//! seals the logical table catalog. Each later install must retain all its IDs.
//! Half-open scans are split at full owner/epoch boundaries, not owner alone;
//! forward and reverse iterators retain return-migration incarnation boundaries.
use vstd::prelude::*;
use std::sync::Arc;
use crate::types::{Grant, Status};
use crate::bytes::compare;
use crate::directory::RouteTable;
#[cfg(verus_keep_ghost)]
use crate::bytes::{cmp_spec,cmp_laws,cmp_trans};
#[cfg(verus_keep_ghost)]
use crate::directory::{wellformed,canonical,route,inside,proper,hi_view};
#[cfg(verus_keep_ghost)]
use crate::directory_proofs::equivalent;
#[cfg(verus_keep_ghost)]
use crate::routing_proofs::*;
verus! {
pub struct Snapshot { pub version: u64, pub tables: Vec<RouteTable> }

pub open spec fn table_valid(t: RouteTable, nodes: Seq<u32>, version: u64) -> bool {
    wellformed(t.boundaries@) && canonical(t.boundaries@)
    && forall|i: int| 0 <= i < t.boundaries.len() ==>
        nodes.contains(t.boundaries@[i].grant.owner) && t.boundaries@[i].grant.epoch <= version
}
pub open spec fn snapshot_valid(s: Snapshot, nodes: Seq<u32>) -> bool {
    (forall|i: int| 0 <= i < s.tables.len() ==> table_valid(s.tables@[i],nodes,s.version))
    && (forall|i: int,j: int| 0 <= i < j < s.tables.len() ==> s.tables@[i].table != s.tables@[j].table)
}
pub open spec fn same_snapshot(a: Snapshot,b: Snapshot) -> bool {
    a.version == b.version && a.tables.len() == b.tables.len()
    && forall|i: int| 0 <= i < a.tables.len() ==> a.tables@[i].table == b.tables@[i].table
        && equivalent(a.tables@[i].boundaries@,b.tables@[i].boundaries@)
}
pub open spec fn has_table(s: Snapshot,id: u64) -> bool {
    exists|i: int| 0 <= i < s.tables.len() && s.tables@[i].table == id
}
pub open spec fn contains_catalog(a: Snapshot,b: Snapshot) -> bool {
    forall|i: int| 0 <= i < b.tables.len() ==> #[trigger] has_table(a,b.tables@[i].table)
}
pub open spec fn snapshot_lookup(s: Snapshot,table: u64,key: Seq<u8>) -> Option<Grant> {
    if exists|i: int| 0 <= i < s.tables.len() && s.tables@[i].table == table {
        let i = choose|i: int| 0 <= i < s.tables.len() && s.tables@[i].table == table;
        Some(route(s.tables@[i].boundaries@,key))
    } else { None }
}

pub fn member(nodes: &[u32],owner: u32) -> (yes: bool)
    ensures yes == nodes@.contains(owner),
{
    let mut i = 0usize;
    while i < nodes.len()
        invariant i <= nodes.len(), forall|j: int| 0 <= j < i ==> nodes@[j] != owner,
        decreases nodes.len()-i,
    {
        if nodes[i] == owner { return true; }
        i += 1;
    }
    false
}
pub fn validate_table(t: &RouteTable,nodes: &[u32],version: u64) -> (yes: bool)
    ensures yes == table_valid(*t,nodes@,version),
{
    if t.boundaries.len() == 0 { return false; }
    if t.boundaries[0].start.len() != 0 { return false; }
    let mut i = 0usize;
    while i < t.boundaries.len()
        invariant i <= t.boundaries.len(), t.boundaries.len() > 0,
            t.boundaries@[0].start@ == Seq::<u8>::empty(),
            forall|j: int| 0 <= j < i ==> nodes@.contains(t.boundaries@[j].grant.owner)
                && t.boundaries@[j].grant.epoch <= version,
            forall|j: int,k: int| 0 <= j < k < i ==> cmp_spec(t.boundaries@[j].start@,t.boundaries@[k].start@) < 0,
            forall|j: int| 0 < j < i ==> #[trigger] t.boundaries@[j].grant != t.boundaries@[j-1].grant,
        decreases t.boundaries.len()-i,
    {
        let b = &t.boundaries[i];
        if !member(nodes,b.grant.owner) || b.grant.epoch > version { return false; }
        if i > 0 {
            if compare(&t.boundaries[i-1].start,&b.start) >= 0
                || (t.boundaries[i-1].grant.owner == b.grant.owner && t.boundaries[i-1].grant.epoch == b.grant.epoch) { return false; }
            proof {
                assert forall|j: int| 0 <= j < i implies cmp_spec(t.boundaries@[j].start@,b.start@) < 0 by {
                    if j < i-1 { cmp_trans(t.boundaries@[j].start@,t.boundaries@[i as int-1].start@,b.start@); }
                }
            }
        }
        i += 1;
    }
    true
}
impl Snapshot {
    pub fn validate(&self,nodes: &[u32]) -> (yes: bool)
        ensures yes == snapshot_valid(*self,nodes@),
    {
        let mut i = 0usize;
        while i < self.tables.len()
            invariant i <= self.tables.len(),
                forall|j: int| 0 <= j < i ==> table_valid(self.tables@[j],nodes@,self.version),
                forall|j: int,k: int| 0 <= j < k < i ==> self.tables@[j].table != self.tables@[k].table,
            decreases self.tables.len()-i,
        {
            if !validate_table(&self.tables[i],nodes,self.version) { return false; }
            let mut j = 0usize;
            while j < i
                invariant j <= i < self.tables.len(),
                    forall|k: int| 0 <= k < j ==> self.tables@[k].table != self.tables@[i as int].table,
                decreases i-j,
            {
                if self.tables[j].table == self.tables[i].table { return false; }
                j += 1;
            }
            i += 1;
        }
        true
    }
    pub fn table(&self,id: u64) -> (out: Option<&RouteTable>)
        ensures match out {
            Some(t) => t.table == id && exists|i: int| 0 <= i < self.tables.len() && *t == self.tables@[i],
            None => forall|i: int| 0 <= i < self.tables.len() ==> self.tables@[i].table != id,
        },
    {
        let mut i = 0usize;
        while i < self.tables.len()
            invariant i <= self.tables.len(), forall|j: int| 0 <= j < i ==> self.tables@[j].table != id,
            decreases self.tables.len()-i,
        {
            if self.tables[i].table == id { return Some(&self.tables[i]); }
            i += 1;
        }
        None
    }
    fn contains_catalog(&self,other: &Self) -> (yes: bool)
        ensures yes == contains_catalog(*self,*other),
    {
        let mut i = 0usize;
        while i < other.tables.len()
            invariant i <= other.tables.len(),
                forall|j: int| 0 <= j < i ==> #[trigger] has_table(*self,other.tables@[j].table),
            decreases other.tables.len()-i,
        {
            if self.table(other.tables[i].table).is_none() {
                proof { assert(!has_table(*self,other.tables@[i as int].table)); }
                return false;
            }
            i += 1;
        }
        true
    }
    pub fn segments<'a>(&'a self,table: u64,lo: &'a [u8],hi: Option<&'a [u8]>) -> (out: Result<ScanSegments<'a>,Status>)
        requires forall|i: int| 0 <= i < self.tables.len() ==> wellformed(self.tables@[i].boundaries@) && canonical(self.tables@[i].boundaries@),
        ensures match out { Ok(it) => it.wf() && it.position() == Some(lo@) && it.upper() == hi_view(hi)
            && it.table().table == table && exists|i: int| 0 <= i < self.tables.len() && self.tables@[i] == it.table(), Err(_) => true },
    {
        if let Some(h) = hi { if compare(lo,h) >= 0 { return Err(Status::Invalid); } }
        let t = match self.table(table) { Some(t) => t, None => return Err(Status::NotFound) };
        let index = lower_boundary(t,lo);
        Ok(ScanSegments { table: t, index, cursor: Some(lo), hi })
    }
    pub fn reverse_segments<'a>(&'a self,table: u64,lo: &'a [u8],hi: Option<&'a [u8]>) -> (out: Result<ReverseScanSegments<'a>,Status>)
        requires forall|i: int| 0 <= i < self.tables.len() ==> wellformed(self.tables@[i].boundaries@) && canonical(self.tables@[i].boundaries@),
        ensures match out { Ok(it) => it.wf() && it.position() == Some(hi_view(hi)) && it.lower() == lo@
            && it.table().table == table && exists|i: int| 0 <= i < self.tables.len() && self.tables@[i] == it.table(), Err(_) => true },
    {
        if let Some(h) = hi { if compare(lo,h) >= 0 { return Err(Status::Invalid); } }
        let t = match self.table(table) { Some(t) => t, None => return Err(Status::NotFound) };
        let index = match hi {
            None => t.boundaries.len()-1,
            Some(h) => {
                let i = lower_boundary(t,h);
                if compare(&t.boundaries[i].start,h) == 0 {
                    proof { cmp_laws(Seq::empty(),lo@); }
                    i-1
                } else { i }
            },
        };
        Ok(ReverseScanSegments { table: t,index,lo,hi,done: false })
    }
    pub fn same(&self,other: &Self) -> (yes: bool)
        ensures yes == same_snapshot(*self,*other),
    {
        if self.version != other.version || self.tables.len() != other.tables.len() { return false; }
        let mut i = 0usize;
        while i < self.tables.len()
            invariant self.version == other.version, self.tables.len() == other.tables.len(), i <= self.tables.len(),
                forall|j: int| 0 <= j < i ==> self.tables@[j].table == other.tables@[j].table
                    && equivalent(self.tables@[j].boundaries@,other.tables@[j].boundaries@),
            decreases self.tables.len()-i,
        {
            let a = &self.tables[i]; let b = &other.tables[i];
            if !same_table(a,b) { return false; }
            i += 1;
        }
        true
    }
}
fn same_table(a: &RouteTable,b: &RouteTable) -> (yes: bool)
    ensures yes == (a.table == b.table && equivalent(a.boundaries@,b.boundaries@)),
{
    if a.table != b.table || a.boundaries.len() != b.boundaries.len() { return false; }
    let mut j = 0usize;
    while j < a.boundaries.len()
        invariant j <= a.boundaries.len(), a.table == b.table, a.boundaries.len() == b.boundaries.len(),
            forall|k: int| 0 <= k < j ==> a.boundaries@[k].start@ == b.boundaries@[k].start@
                && a.boundaries@[k].grant == b.boundaries@[k].grant,
        decreases a.boundaries.len()-j,
    {
        if a.boundaries[j].grant.owner != b.boundaries[j].grant.owner || a.boundaries[j].grant.epoch != b.boundaries[j].grant.epoch
            || compare(&a.boundaries[j].start,&b.boundaries[j].start) != 0 { return false; }
        j += 1;
    }
    true
}
pub struct RoutingCache { nodes: Vec<u32>, current: Option<Arc<Snapshot>> }
impl RoutingCache {
    pub closed spec fn nodes(&self) -> Seq<u32> { self.nodes@ }
    pub closed spec fn observation(&self) -> Option<Snapshot> { match self.current { Some(s) => Some(*s), None => None } }
    pub closed spec fn wf(&self) -> bool {
        self.nodes.len() > 0 && match self.current { Some(s) => snapshot_valid(*s,self.nodes@), None => true }
    }
    pub fn new(nodes: Vec<u32>) -> (out: Result<Self,Status>)
        ensures match out { Ok(c) => c.wf() && c.nodes() == nodes@ && c.observation() is None, Err(_) => true },
    {
        if nodes.len() == 0 { return Err(Status::Invalid); }
        let mut i = 0usize;
        while i < nodes.len()
            invariant i <= nodes.len(),
            decreases nodes.len()-i,
        {
            let mut j = 0usize;
            while j < i
                invariant j <= i < nodes.len(),
                decreases i-j,
            {
                if nodes[j] == nodes[i] { return Err(Status::Invalid); }
                j += 1;
            }
            i += 1;
        }
        Ok(Self { nodes, current: None })
    }
    pub fn snapshot(&self) -> (out: Option<&Snapshot>)
        ensures match out { Some(s) => self.observation() == Some(*s), None => self.observation() is None },
    { match &self.current { Some(s) => Some(&**s), None => None } }
    pub fn snapshot_arc(&self) -> (out: Option<Arc<Snapshot>>)
        ensures match out { Some(s) => self.observation() == Some(*s), None => self.observation() is None },
    { match &self.current { Some(s) => Some(Arc::clone(s)), None => None } }
    pub fn install(&mut self,snapshot: Snapshot) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).nodes() == old(self).nodes(),
            install_effect(old(self).observation(),final(self).observation(),snapshot,status),
    {
        if !snapshot.validate(&self.nodes) { return Status::Invalid; }
        if let Some(current) = &self.current {
            if snapshot.version < current.version { return Status::Retry; }
            if snapshot.version == current.version {
                return if current.same(&snapshot) { Status::Ok } else { Status::Invalid };
            }
            if !current.contains_catalog(&snapshot) || !snapshot.contains_catalog(current) { return Status::Invalid; }
        }
        self.current = Some(Arc::new(snapshot));
        Status::Ok
    }
    pub fn lookup(&self,table: u64,key: &[u8]) -> (out: Option<Grant>)
        requires self.wf(),
        ensures out == match self.observation() { Some(s) => snapshot_lookup(s,table,key@), None => None },
    {
        match &self.current {
            Some(s) => match s.table(table) {
                Some(t) => { let snapshot: &Snapshot = s; proof { found_table(*snapshot,*t,table,key@,self.nodes@); } Some(t.lookup(key)) },
                None => None,
            },
            None => None,
        }
    }
    pub fn segments<'a>(&'a self,table: u64,lo: &'a [u8],hi: Option<&'a [u8]>) -> (out: Result<ScanSegments<'a>,Status>)
        requires self.wf(),
        ensures match out { Ok(it) => it.wf() && it.position() == Some(lo@) && it.upper() == hi_view(hi)
            && it.table().table == table && exists|s: Snapshot| self.observation() == Some(s)
                && exists|i: int| 0 <= i < s.tables.len() && s.tables@[i] == it.table(), Err(_) => true },
    {
        match &self.current {
            Some(s) => s.segments(table,lo,hi),
            None => Err(Status::NotFound),
        }
    }
}
pub struct ScanSegment<'a> { pub lo: &'a [u8], pub hi: Option<&'a [u8]>, pub grant: Grant }
pub struct ScanSegments<'a> { table: &'a RouteTable, index: usize, cursor: Option<&'a [u8]>, hi: Option<&'a [u8]> }
impl<'a> ScanSegments<'a> {
    pub closed spec fn table(&self) -> RouteTable { *self.table }
    pub closed spec fn position(&self) -> Option<Seq<u8>> { hi_view(self.cursor) }
    pub closed spec fn upper(&self) -> Option<Seq<u8>> { hi_view(self.hi) }
    pub closed spec fn remaining(&self) -> nat { if self.cursor is Some { (self.table.boundaries.len()-self.index) as nat } else { 0 } }
    pub closed spec fn wf(&self) -> bool {
        wellformed(self.table.boundaries@) && canonical(self.table.boundaries@) && self.index < self.table.boundaries.len()
        && match self.cursor { Some(k) => proper(k@,hi_view(self.hi))
            && cmp_spec(self.table.boundaries@[self.index as int].start@,k@) <= 0
            && (self.index+1 < self.table.boundaries.len() ==> cmp_spec(k@,self.table.boundaries@[self.index as int+1].start@) < 0), None => true }
    }
    pub fn next(&mut self) -> (out: Option<ScanSegment<'a>>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).table() == old(self).table(), final(self).upper() == old(self).upper(),
            segment_step(old(self).table(),old(self).position(),old(self).upper(),final(self).position(),out),
            out is Some ==> final(self).remaining() < old(self).remaining(),
    {
        let lo = match self.cursor { Some(lo) => lo, None => return None };
        let grant = self.table.boundaries[self.index].grant;
        let next = self.index + 1;
        if next < self.table.boundaries.len() {
            let start = self.table.boundaries[next].start.as_slice();
            let before_upper = match self.hi { Some(h) => compare(start,h) < 0, None => true };
            if before_upper {
                proof { segment_routes(self.table.boundaries@,self.index as int,lo@,Some(start@)); }
                self.index = next;
                self.cursor = Some(start);
                proof { cmp_laws(start@,start@); }
                return Some(ScanSegment { lo, hi: Some(start), grant });
            }
            proof { if let Some(h) = self.hi { cmp_laws(start@,h@); } }
        }
        proof { segment_routes(self.table.boundaries@,self.index as int,lo@,hi_view(self.hi)); }
        self.cursor = None;
        Some(ScanSegment { lo, hi: self.hi, grant })
    }
}
pub struct ReverseScanSegments<'a> { table: &'a RouteTable, index: usize, lo: &'a [u8], hi: Option<&'a [u8]>, done: bool }
impl<'a> ReverseScanSegments<'a> {
    pub closed spec fn table(&self) -> RouteTable { *self.table }
    pub closed spec fn lower(&self) -> Seq<u8> { self.lo@ }
    pub closed spec fn position(&self) -> Option<Option<Seq<u8>>> { if self.done { None } else { Some(hi_view(self.hi)) } }
    pub closed spec fn remaining(&self) -> nat { if self.done { 0 } else { self.index as nat+1 } }
    pub closed spec fn wf(&self) -> bool {
        wellformed(self.table.boundaries@) && canonical(self.table.boundaries@) && self.index < self.table.boundaries.len()
        && (!self.done ==> proper(self.lo@,hi_view(self.hi))
            && proper(self.table.boundaries@[self.index as int].start@,hi_view(self.hi))
            && (self.index+1 < self.table.boundaries.len() ==> self.hi is Some
                && cmp_spec(self.hi.unwrap()@,self.table.boundaries@[self.index as int+1].start@) <= 0))
    }
    pub fn next(&mut self) -> (out: Option<ScanSegment<'a>>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).table() == old(self).table(), final(self).lower() == old(self).lower(),
            reverse_segment_step(old(self).table(),old(self).lower(),old(self).position(),final(self).position(),out),
            out is Some ==> final(self).remaining() < old(self).remaining(),
    {
        hide(route); hide(cmp_spec);
        if self.done { return None; }
        let start = self.table.boundaries[self.index].start.as_slice();
        let grant = self.table.boundaries[self.index].grant;
        let hi = self.hi;
        if compare(self.lo,start) < 0 {
            proof {
                cmp_laws(Seq::empty(),self.lo@); cmp_laws(start@,start@);
                cmp_laws(start@,self.lo@);
                cmp_laws(self.lo@,self.lo@);
                assert(self.index > 0) by {
                    if self.index == 0 {
                        assert(start@ == Seq::<u8>::empty());
                        assert(cmp_spec(start@,self.lo@) <= 0);
                        assert(cmp_spec(self.lo@,start@) >= 0);
                    }
                }
                assert(cmp_spec(self.table.boundaries@[self.index as int-1].start@,start@) < 0);
                segment_routes(self.table.boundaries@,self.index as int,start@,hi_view(hi));
            }
            self.hi = Some(start); self.index -= 1;
            return Some(ScanSegment { lo: start,hi,grant });
        }
        proof { cmp_laws(self.lo@,start@); segment_routes(self.table.boundaries@,self.index as int,self.lo@,hi_view(hi)); }
        self.done = true;
        Some(ScanSegment { lo: self.lo,hi,grant })
    }
}
/// Locate the existing directory cell, preserving its full owner/epoch grant.
fn lower_boundary(t: &RouteTable,key: &[u8]) -> (index: usize)
    requires wellformed(t.boundaries@),
    ensures index < t.boundaries.len(), cmp_spec(t.boundaries@[index as int].start@,key@) <= 0,
        index+1 < t.boundaries.len() ==> cmp_spec(key@,t.boundaries@[index as int+1].start@) < 0,
{
    let mut lower = 1usize; let mut upper = t.boundaries.len();
    proof { cmp_laws(Seq::empty(),key@); }
    while lower < upper
        invariant 1 <= lower <= upper <= t.boundaries.len(), wellformed(t.boundaries@),
            forall|j: int| 0 <= j < lower ==> cmp_spec(t.boundaries@[j].start@,key@) <= 0,
            forall|j: int| upper <= j < t.boundaries.len() ==> cmp_spec(t.boundaries@[j].start@,key@) > 0,
        decreases upper-lower,
    {
        let mid = lower+(upper-lower)/2;
        if compare(&t.boundaries[mid].start,key) <= 0 {
            proof { assert forall|j: int| 0 <= j <= mid implies cmp_spec(t.boundaries@[j].start@,key@) <= 0 by {
                if j < mid { cmp_trans(t.boundaries@[j].start@,t.boundaries@[mid as int].start@,key@); }
            } }
            lower = mid+1;
        } else {
            proof { cmp_laws(t.boundaries@[mid as int].start@,key@);
                assert forall|j: int| mid <= j < t.boundaries.len() implies cmp_spec(t.boundaries@[j].start@,key@) > 0 by {
                    if j > mid { cmp_trans(key@,t.boundaries@[mid as int].start@,t.boundaries@[j].start@); cmp_laws(key@,t.boundaries@[j].start@); }
                }
            }
            upper = mid;
        }
    }
    proof { if lower < t.boundaries.len() { cmp_laws(t.boundaries@[lower as int].start@,key@); } }
    lower-1
}
} // verus!
