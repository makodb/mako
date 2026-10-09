//! Exact point and half-open interval leases for sequential client transactions.
//!
//! The retained session is also its client's non-wrapping sequence fence. A
//! terminal empty session may be replaced, but its sequence may never be reused.
//! The registry does not authorize Serving/epoch admission: the participant
//! checks that and registers under the same range lock. Existing open holders
//! remain usable while the participant is Frozen. Reads and writes share this
//! registry; resolving is terminal and releasing before resolution is forbidden.
//!
//! Every scope protects all logical rows at its table/coordinates, including
//! absent rows and distinct warehouse aliases. Range endpoints are exact byte
//! coordinates, with no hashed buckets or deadlines. Overlapping registrations
//! in a session must agree on the full owner/epoch grant. Repeated contained
//! acquisitions allocate nothing. Point release removes only an explicit point
//! registration; an independently registered scan still protects that point.
//! Finish resolves and clears all scopes while retaining the sequence fence.
//! Drain scans all intersections, including resolved but unreleased sessions.
use std::collections::{HashMap, hash_map::Entry};
use vstd::prelude::*;
#[cfg(verus_keep_ghost)]
use vstd::std_specs::iter::IteratorSpec;
use crate::bytes;
use crate::types::{Grant, KeyRange, Status, TxnId};

verus! {

pub enum ScopeEndView {
    Point,
    Range(Option<Seq<u8>>),
}

pub struct ScopeView {
    pub table: u64,
    pub coordinate: Seq<u8>,
    pub end: ScopeEndView,
    pub grant: Grant,
}

pub open spec fn hi_view(hi: Option<&[u8]>) -> Option<Seq<u8>> {
    match hi { Some(h) => Some(h@), None => None }
}

pub open spec fn range_hi(range: KeyRange) -> Option<Seq<u8>> {
    match range.hi { Some(h) => Some(h@), None => None }
}

pub open spec fn proper(lo: Seq<u8>, hi: Option<Seq<u8>>) -> bool {
    match hi { Some(h) => bytes::cmp_spec(lo,h) < 0, None => true }
}

pub open spec fn inside(lo: Seq<u8>, hi: Option<Seq<u8>>, c: Seq<u8>) -> bool {
    bytes::cmp_spec(lo,c) <= 0
        && match hi { Some(h) => bytes::cmp_spec(c,h) < 0, None => true }
}

pub open spec fn upper_covers(outer: Option<Seq<u8>>, inner: Option<Seq<u8>>) -> bool {
    match outer {
        None => true,
        Some(o) => match inner { Some(i) => bytes::cmp_spec(i,o) <= 0, None => false },
    }
}

impl ScopeView {
    pub open spec fn valid(self) -> bool {
        match self.end { ScopeEndView::Point => true, ScopeEndView::Range(h) => proper(self.coordinate,h) }
    }

    pub open spec fn contains(self, table: u64, c: Seq<u8>) -> bool {
        self.table == table && match self.end {
            ScopeEndView::Point => self.coordinate == c,
            ScopeEndView::Range(h) => inside(self.coordinate,h,c),
        }
    }

    pub open spec fn covers_range(self, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>) -> bool {
        self.table == table && proper(lo,hi) && match self.end {
            ScopeEndView::Point => false,
            ScopeEndView::Range(h) => bytes::cmp_spec(self.coordinate,lo) <= 0 && upper_covers(h,hi),
        }
    }

    pub open spec fn overlaps_range(self, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>) -> bool {
        self.table == table && proper(lo,hi) && match self.end {
            ScopeEndView::Point => inside(lo,hi,self.coordinate),
            ScopeEndView::Range(h) => proper(self.coordinate,h)
                && (match hi { Some(upper) => bytes::cmp_spec(self.coordinate,upper) < 0, None => true })
                && (match h { Some(upper) => bytes::cmp_spec(lo,upper) < 0, None => true }),
        }
    }

    pub open spec fn overlaps(self, other: Self) -> bool {
        match other.end {
            ScopeEndView::Point => self.contains(other.table,other.coordinate),
            ScopeEndView::Range(h) => self.overlaps_range(other.table,other.coordinate,h),
        }
    }

    pub open spec fn covers(self, other: Self) -> bool {
        match other.end {
            ScopeEndView::Point => self.contains(other.table,other.coordinate),
            ScopeEndView::Range(h) => self.covers_range(other.table,other.coordinate,h),
        }
    }
}

pub proof fn overlap_symmetric(a: ScopeView, b: ScopeView)
    ensures a.overlaps(b) == b.overlaps(a),
{
    match (a.end,b.end) {
        (ScopeEndView::Point, ScopeEndView::Range(h)) => {
            if inside(b.coordinate,h,a.coordinate) {
                if let Some(upper) = h { bytes::cmp_trans(b.coordinate,a.coordinate,upper); }
            }
        },
        (ScopeEndView::Range(h), ScopeEndView::Point) => {
            if inside(a.coordinate,h,b.coordinate) {
                if let Some(upper) = h { bytes::cmp_trans(a.coordinate,b.coordinate,upper); }
            }
        },
        _ => {},
    }
}

pub proof fn common_point_overlaps(a: ScopeView, b: ScopeView, table: u64, c: Seq<u8>)
    requires a.contains(table,c), b.contains(table,c),
    ensures a.overlaps(b),
{
    match (a.end,b.end) {
        (ScopeEndView::Range(ah),ScopeEndView::Range(bh)) => {
            if let Some(h) = ah {
                bytes::cmp_trans(a.coordinate,c,h); bytes::cmp_trans(b.coordinate,c,h);
            }
            if let Some(h) = bh {
                bytes::cmp_trans(b.coordinate,c,h); bytes::cmp_trans(a.coordinate,c,h);
            }
        },
        _ => { overlap_symmetric(a,b); },
    }
}

/// Endpoint intersection is exact over all finite byte strings, not an
/// approximation by range starts or existing rows.
pub proof fn overlap_has_point(a: ScopeView, b: ScopeView)
    requires a.overlaps(b),
    ensures exists|c: Seq<u8>| a.contains(a.table,c) && b.contains(a.table,c),
{
    match (a.end,b.end) {
        (ScopeEndView::Point,_) => {
            overlap_symmetric(a,b);
            assert(a.contains(a.table,a.coordinate) && b.contains(a.table,a.coordinate));
        },
        (_,ScopeEndView::Point) => {
            assert(a.contains(a.table,b.coordinate) && b.contains(a.table,b.coordinate));
        },
        (ScopeEndView::Range(_),ScopeEndView::Range(_)) => {
            bytes::cmp_laws(a.coordinate,b.coordinate);
            bytes::cmp_laws(a.coordinate,a.coordinate);
            bytes::cmp_laws(b.coordinate,b.coordinate);
            let c = if bytes::cmp_spec(a.coordinate,b.coordinate) <= 0 { b.coordinate } else { a.coordinate };
            assert(a.contains(a.table,c) && b.contains(a.table,c));
        },
    }
}

pub proof fn covered_point(a: ScopeView, b: ScopeView, table: u64, c: Seq<u8>)
    requires a.covers(b), b.contains(table,c),
    ensures a.contains(table,c),
{
    if let ScopeEndView::Range(bh) = b.end {
        let ah = a.end->Range_0;
        bytes::cmp_trans(a.coordinate,b.coordinate,c);
        if let Some(h) = ah {
            let k = bh.unwrap();
            bytes::cmp_trans(c,k,h);
        }
    }
}

pub proof fn covers_overlap(a: ScopeView, b: ScopeView)
    requires b.valid(), a.covers(b),
    ensures a.overlaps(b),
{
    bytes::cmp_laws(b.coordinate,b.coordinate);
    assert(b.contains(b.table,b.coordinate));
    covered_point(a,b,b.table,b.coordinate);
    common_point_overlaps(a,b,b.table,b.coordinate);
}

pub struct SessionView {
    pub sequence: u64,
    pub terminal: bool,
    pub holds: Seq<ScopeView>,
}

impl SessionView {
    pub open spec fn wf(self) -> bool {
        (forall|i: int| 0 <= i < self.holds.len() ==> self.holds[i].valid())
        && forall|i: int, j: int| 0 <= i < j < self.holds.len() ==>
            (#[trigger] self.holds[i]) != (#[trigger] self.holds[j])
                && (self.holds[i].overlaps(self.holds[j]) ==> self.holds[i].grant == self.holds[j].grant)
    }

    pub open spec fn has(self, table: u64, coordinate: Seq<u8>, grant: Grant) -> bool {
        exists|i: int| 0 <= i < self.holds.len()
            && self.holds[i].contains(table,coordinate) && self.holds[i].grant == grant
    }

    pub open spec fn scoped(self, table: u64, coordinate: Seq<u8>) -> bool {
        exists|i: int| 0 <= i < self.holds.len() && self.holds[i].contains(table,coordinate)
    }

    pub open spec fn range_scoped(self, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>) -> bool {
        exists|i: int| 0 <= i < self.holds.len() && self.holds[i].covers_range(table,lo,hi)
    }

    pub open spec fn has_range(self, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>, grant: Grant) -> bool {
        exists|i: int| 0 <= i < self.holds.len()
            && self.holds[i].covers_range(table,lo,hi) && self.holds[i].grant == grant
    }

    pub open spec fn point_scoped(self, table: u64, coordinate: Seq<u8>) -> bool {
        exists|i: int| 0 <= i < self.holds.len() && self.holds[i].table == table
            && self.holds[i].coordinate == coordinate && self.holds[i].end is Point
    }

    pub open spec fn compatible(self, scope: ScopeView) -> bool {
        forall|i: int| 0 <= i < self.holds.len() && self.holds[i].overlaps(scope)
            ==> self.holds[i].grant == scope.grant
    }

    pub open spec fn covering(self, scope: ScopeView) -> bool {
        exists|i: int| 0 <= i < self.holds.len() && self.holds[i].covers(scope)
    }

    pub open spec fn drained(self, range: KeyRange) -> bool {
        forall|i: int| 0 <= i < self.holds.len() ==>
            !self.holds[i].overlaps_range(range.table,range.lo@,range_hi(range))
    }
}

enum ScopeEnd {
    Point,
    Range(Option<Vec<u8>>),
}

struct Scope {
    table: u64,
    coordinate: Vec<u8>,
    end: ScopeEnd,
    grant: Grant,
}

impl View for Scope {
    type V = ScopeView;
    closed spec fn view(&self) -> ScopeView {
        ScopeView { table: self.table, coordinate: self.coordinate@, grant: self.grant,
            end: match self.end {
                ScopeEnd::Point => ScopeEndView::Point,
                ScopeEnd::Range(hi) => ScopeEndView::Range(match hi { Some(h) => Some(h@), None => None }),
            } }
    }
}

fn proper_range(lo: &[u8], hi: Option<&[u8]>) -> (out: bool)
    ensures out == proper(lo@,hi_view(hi)),
{
    match hi { Some(h) => bytes::compare(lo,h) < 0, None => true }
}

impl Scope {
    fn contains(&self, table: u64, c: &[u8]) -> (out: bool)
        ensures out == self@.contains(table,c@),
    {
        if self.table != table { return false; }
        match &self.end {
            ScopeEnd::Point => {
                let order = bytes::compare(&self.coordinate,c);
                proof { bytes::cmp_laws(self.coordinate@,c@); }
                order == 0
            },
            ScopeEnd::Range(hi) => {
                if bytes::compare(&self.coordinate,c) > 0 { return false; }
                match hi { Some(h) => bytes::compare(c,h) < 0, None => true }
            },
        }
    }

    fn point_matches(&self, table: u64, c: &[u8]) -> (out: bool)
        ensures out == (self@.table == table && self@.coordinate == c@ && self@.end is Point),
    {
        match &self.end { ScopeEnd::Point => self.contains(table,c), _ => false }
    }

    fn covers_range(&self, table: u64, lo: &[u8], hi: Option<&[u8]>) -> (out: bool)
        requires proper(lo@,hi_view(hi)),
        ensures out == self@.covers_range(table,lo@,hi_view(hi)),
    {
        if self.table != table { return false; }
        match &self.end {
            ScopeEnd::Point => false,
            ScopeEnd::Range(end) => {
                if bytes::compare(&self.coordinate,lo) > 0 { return false; }
                match end {
                    None => true,
                    Some(upper) => match hi { Some(h) => bytes::compare(h,upper) <= 0, None => false },
                }
            },
        }
    }

    fn overlaps_range(&self, table: u64, lo: &[u8], hi: Option<&[u8]>) -> (out: bool)
        requires proper(lo@,hi_view(hi)),
        ensures out == self@.overlaps_range(table,lo@,hi_view(hi)),
    {
        if self.table != table { return false; }
        match &self.end {
            ScopeEnd::Point => {
                if bytes::compare(lo,&self.coordinate) > 0 { return false; }
                match hi { Some(h) => bytes::compare(&self.coordinate,h) < 0, None => true }
            },
            ScopeEnd::Range(end) => {
                if let Some(h) = end {
                    if bytes::compare(&self.coordinate,h) >= 0 || bytes::compare(lo,h) >= 0 { return false; }
                }
                match hi { Some(h) => bytes::compare(&self.coordinate,h) < 0, None => true }
            },
        }
    }
}

struct Session {
    sequence: u64,
    terminal: bool,
    holds: Vec<Scope>,
}

impl View for Session {
    type V = SessionView;
    closed spec fn view(&self) -> SessionView {
        SessionView {
            sequence: self.sequence,
            terminal: self.terminal,
            holds: self.holds@.map(|_i: int, h: Scope| h@),
        }
    }
}

pub open spec fn point_scope(table: u64, c: Seq<u8>, grant: Grant) -> ScopeView {
    ScopeView { table, coordinate: c, end: ScopeEndView::Point, grant }
}

pub open spec fn range_scope(table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>, grant: Grant) -> ScopeView {
    ScopeView { table, coordinate: lo, end: ScopeEndView::Range(hi), grant }
}

pub open spec fn registration(before: SessionView, after: SessionView, scope: ScopeView, status: Status) -> bool {
    if !scope.valid() || !before.compatible(scope) {
        status == Status::Invalid && after == before
    } else if before.covering(scope) {
        status == Status::Ok && after == before
    } else {
        status == Status::Ok && after == (SessionView { holds: before.holds.push(scope), ..before })
    }
}

pub proof fn registration_success(before: SessionView, after: SessionView, scope: ScopeView)
    requires registration(before,after,scope,Status::Ok),
    ensures after.sequence == before.sequence, after.terminal == before.terminal,
        exists|i: int| 0 <= i < after.holds.len()
            && after.holds[i].covers(scope) && after.holds[i].grant == scope.grant,
{
    if before.covering(scope) {
        let i = choose|i: int| 0 <= i < before.holds.len() && before.holds[i].covers(scope);
        covers_overlap(before.holds[i],scope);
        assert(after.holds[i].grant == scope.grant);
    } else {
        bytes::cmp_laws(scope.coordinate,scope.coordinate);
        if let ScopeEndView::Range(Some(h)) = scope.end { bytes::cmp_laws(h,h); }
        assert(after.holds[before.holds.len() as int] == scope);
        assert(scope.covers(scope));
    }
}

proof fn appended_scope(before: SessionView, after: SessionView, scope: ScopeView)
    requires before.wf(), scope.valid(), before.compatible(scope), !before.covering(scope),
        after.holds == before.holds.push(scope),
    ensures after.wf(),
{
    bytes::cmp_laws(scope.coordinate,scope.coordinate);
    if let ScopeEndView::Range(Some(h)) = scope.end { bytes::cmp_laws(h,h); }
    assert(scope.covers(scope));
    assert forall|i: int| 0 <= i < after.holds.len() implies after.holds[i].valid() by {};
    assert forall|i: int, j: int| 0 <= i < j < after.holds.len() implies
        (#[trigger] after.holds[i]) != (#[trigger] after.holds[j])
            && (after.holds[i].overlaps(after.holds[j]) ==> after.holds[i].grant == after.holds[j].grant) by {
        if j == before.holds.len() {
            assert(before.holds[i] != scope);
            if before.holds[i].overlaps(scope) { assert(before.holds[i].grant == scope.grant); }
        } else {
            assert(before.holds[i] != before.holds[j]);
            assert(before.holds[i].overlaps(before.holds[j]) ==> before.holds[i].grant == before.holds[j].grant);
        }
    }
}

impl Session {
    fn new(sequence: u64) -> (out: Self)
        ensures out@ == (SessionView { sequence, terminal: false, holds: Seq::empty() }),
    {
        let out = Self { sequence, terminal: false, holds: Vec::new() };
        proof { assert(out@.holds =~= Seq::<ScopeView>::empty()); }
        out
    }

    fn find(&self, table: u64, coordinate: &[u8]) -> (out: Option<usize>)
        ensures match out {
            Some(i) => i < self@.holds.len() && self@.holds[i as int].contains(table,coordinate@),
            None => !self@.scoped(table,coordinate@),
        },
    {
        let mut i = 0;
        while i < self.holds.len()
            invariant i <= self.holds.len(),
                forall|j: int| 0 <= j < i ==> !self@.holds[j].contains(table,coordinate@),
            decreases self.holds.len() - i,
        {
            if self.holds[i].contains(table,coordinate) { return Some(i); }
            i += 1;
        }
        None
    }

    fn acquire(&mut self, table: u64, coordinate: &[u8], grant: Grant) -> (out: Status)
        requires old(self)@.wf(), !old(self).terminal,
        ensures final(self)@.wf(),
            registration(old(self)@,final(self)@,point_scope(table,coordinate@,grant),out),
    {
        let ghost before = self@;
        let ghost wanted = point_scope(table,coordinate@,grant);
        if let Some(i) = self.find(table,coordinate) {
            let h = &self.holds[i];
            if h.grant.owner != grant.owner || h.grant.epoch != grant.epoch { return Status::Invalid; }
            proof {
                assert forall|j: int| 0 <= j < before.holds.len() && before.holds[j].overlaps(wanted)
                    implies before.holds[j].grant == grant by {
                    common_point_overlaps(before.holds[j],before.holds[i as int],table,coordinate@);
                    if i < j { overlap_symmetric(before.holds[i as int],before.holds[j]); }
                }
            }
            return Status::Ok;
        }
        let owned = bytes::copy_bytes(coordinate);
        self.holds.push(Scope { table, coordinate: owned, end: ScopeEnd::Point, grant });
        proof {
            assert(self@.holds =~= before.holds.push(wanted));
            appended_scope(before,self@,wanted);
        }
        Status::Ok
    }

    fn acquire_range(&mut self, table: u64, lo: &[u8], hi: Option<&[u8]>, grant: Grant) -> (out: Status)
        requires old(self)@.wf(), !old(self).terminal, proper(lo@,hi_view(hi)),
        ensures final(self)@.wf(),
            registration(old(self)@,final(self)@,range_scope(table,lo@,hi_view(hi),grant),out),
    {
        let ghost before = self@;
        let ghost wanted = range_scope(table,lo@,hi_view(hi),grant);
        let mut covered = false;
        let mut i = 0;
        while i < self.holds.len()
            invariant i <= self.holds.len(), self@ == before, before.wf(), !before.terminal, wanted.valid(),
                wanted == range_scope(table,lo@,hi_view(hi),grant),
                forall|j: int| 0 <= j < i && before.holds[j].overlaps(wanted)
                    ==> before.holds[j].grant == grant,
                covered == (exists|j: int| 0 <= j < i && before.holds[j].covers(wanted)),
            decreases self.holds.len() - i,
        {
            let h = &self.holds[i];
            proof { assert(before.holds[i as int] == h@); }
            if h.overlaps_range(table,lo,hi)
                && (h.grant.owner != grant.owner || h.grant.epoch != grant.epoch) {
                return Status::Invalid;
            }
            if h.covers_range(table,lo,hi) { covered = true; }
            i += 1;
        }
        if covered { return Status::Ok; }
        let coordinate = bytes::copy_bytes(lo);
        let end = match hi { Some(h) => Some(bytes::copy_bytes(h)), None => None };
        self.holds.push(Scope { table, coordinate, end: ScopeEnd::Range(end), grant });
        proof {
            assert(self@.holds =~= before.holds.push(wanted));
            appended_scope(before,self@,wanted);
        }
        Status::Ok
    }

    fn release(&mut self, table: u64, coordinate: &[u8]) -> (out: Status)
        requires old(self)@.wf(), old(self).terminal,
        ensures final(self)@.wf(), point_release(old(self)@,final(self)@,table,coordinate@,out),
    {
        let ghost before = self@;
        let mut i = 0;
        while i < self.holds.len()
            invariant i <= self.holds.len(), self@ == before, before == old(self)@, before.wf(), before.terminal,
                forall|j: int| 0 <= j < i ==> !(before.holds[j].table == table
                    && before.holds[j].coordinate == coordinate@ && before.holds[j].end is Point),
            decreases self.holds.len() - i,
        {
            if self.holds[i].point_matches(table,coordinate) {
                proof {
                    assert(before.holds[i as int].table == table);
                    assert(before.holds[i as int].coordinate == coordinate@);
                    assert(before.holds[i as int].end is Point);
                }
                self.holds.swap_remove(i);
                proof {
                    assert(self@.holds =~= before.holds.update(i as int,before.holds.last()).drop_last());
                    removed_scope(before,self@,i as int);
                    assert(point_release(before,self@,table,coordinate@,Status::Ok));
                    assert(point_release(old(self)@,self@,table,coordinate@,Status::Ok));
                }
                return Status::Ok;
            }
            i += 1;
        }
        Status::NotFound
    }

    fn drained(&self, range: &KeyRange) -> (out: bool)
        ensures out == self@.drained(*range),
    {
        let hi = match &range.hi { Some(h) => Some(h.as_slice()), None => None };
        if !proper_range(&range.lo,hi) { return true; }
        let mut i = 0;
        while i < self.holds.len()
            invariant i <= self.holds.len(), hi_view(hi) == range_hi(*range), proper(range.lo@,hi_view(hi)),
                forall|j: int| 0 <= j < i ==>
                    !self@.holds[j].overlaps_range(range.table,range.lo@,range_hi(*range)),
            decreases self.holds.len() - i,
        {
            if self.holds[i].overlaps_range(range.table,&range.lo,hi) {
                proof { assert(self@.holds[i as int].overlaps_range(range.table,range.lo@,range_hi(*range))); }
                return false;
            }
            i += 1;
        }
        true
    }
}

pub open spec fn point_release(before: SessionView, after: SessionView,
    table: u64, coordinate: Seq<u8>, status: Status) -> bool {
    after.sequence == before.sequence && after.terminal == before.terminal
        && (status == Status::Ok <==> before.point_scoped(table,coordinate))
        && (status == Status::Ok || status == Status::NotFound)
        && (status != Status::Ok ==> after == before)
        && forall|h: ScopeView| after.holds.contains(h) <==> before.holds.contains(h)
            && !(h.table == table && h.coordinate == coordinate && h.end is Point)
}

/// Swap-removal preserves every other registration, including overlapping
/// range holds. Point release does not punch a hole in an independently held scan.
proof fn removed_scope(before: SessionView, after: SessionView, removed: int)
    requires before.wf(), 0 <= removed < before.holds.len(),
        after.sequence == before.sequence, after.terminal == before.terminal,
        before.holds[removed].end is Point,
        after.holds == before.holds.update(removed,before.holds.last()).drop_last(),
    ensures after.wf(),
        point_release(before,after,before.holds[removed].table,before.holds[removed].coordinate,Status::Ok),
{
    let n = before.holds.len() as int;
    assert forall|j: int| 0 <= j < after.holds.len() implies
        after.holds[j] == before.holds[if j == removed { n - 1 } else { j }] by {};
    assert forall|j: int| 0 <= j < after.holds.len() implies after.holds[j].valid() by {};
    assert forall|a: int,b: int| 0 <= a < b < after.holds.len() implies
        (#[trigger] after.holds[a]) != (#[trigger] after.holds[b])
            && (after.holds[a].overlaps(after.holds[b]) ==> after.holds[a].grant == after.holds[b].grant) by {
        let x = if a == removed { n - 1 } else { a };
        let y = if b == removed { n - 1 } else { b };
        if y < x { overlap_symmetric(before.holds[x],before.holds[y]); }
        if x < y {
            assert(before.holds[x] != before.holds[y]);
            assert(before.holds[x].overlaps(before.holds[y]) ==> before.holds[x].grant == before.holds[y].grant);
        } else {
            assert(before.holds[y] != before.holds[x]);
            assert(before.holds[y].overlaps(before.holds[x]) ==> before.holds[y].grant == before.holds[x].grant);
        }
    }
    let target = before.holds[removed];
    assert forall|j: int| 0 <= j < before.holds.len() && before.holds[j].table == target.table
        && before.holds[j].coordinate == target.coordinate && before.holds[j].end is Point implies j == removed by {
        let h = before.holds[j];
        assert(h.overlaps(target));
        if removed < j { overlap_symmetric(target,h); }
        assert(h.grant == target.grant);
        assert(h == target);
    }
    assert forall|h: ScopeView| after.holds.contains(h) <==> before.holds.contains(h)
        && !(h.table == target.table && h.coordinate == target.coordinate && h.end is Point) by {
        if after.holds.contains(h) {
            let j = choose|j: int| 0 <= j < after.holds.len() && after.holds[j] == h;
            let k = if j == removed { n - 1 } else { j };
            assert(before.holds[k] == h);
            assert(before.holds.contains(h));
        }
        if before.holds.contains(h) && !(h.table == target.table && h.coordinate == target.coordinate && h.end is Point) {
            let k = choose|k: int| 0 <= k < before.holds.len() && before.holds[k] == h;
            let j = if k == n - 1 { removed } else { k };
            assert(after.holds[j] == h);
            assert(after.holds.contains(h));
        }
    }
}

pub struct LeaseRegistry {
    sessions: HashMap<u64, Session>,
}

impl View for LeaseRegistry {
    type V = Map<u64, SessionView>;
    closed spec fn view(&self) -> Map<u64, SessionView> {
        Map::new(self.sessions@.dom(), |client: u64| self.sessions@[client]@)
    }
}

impl LeaseRegistry {
    pub open spec fn wf(&self) -> bool {
        forall|client: u64| self@.contains_key(client) ==> self@[client].wf()
    }

    pub open spec fn holds(&self, id: TxnId, table: u64, coordinate: Seq<u8>, grant: Grant) -> bool {
        self@.contains_key(id.client) && self@[id.client].sequence == id.sequence
            && self@[id.client].has(table, coordinate, grant)
    }

    pub open spec fn holds_range(&self, id: TxnId, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>, grant: Grant) -> bool {
        self@.contains_key(id.client) && self@[id.client].sequence == id.sequence
            && self@[id.client].has_range(table,lo,hi,grant)
    }

    pub open spec fn drained_spec(&self, range: KeyRange) -> bool {
        forall|client: u64| self@.contains_key(client) ==> self@[client].drained(range)
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out@ == Map::<u64, SessionView>::empty(),
    {
        Self { sessions: HashMap::new() }
    }

    pub fn begin(&mut self, id: TxnId) -> (out: Status)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            begin_effect(old(self)@, final(self)@, id, out),
    {
        let ghost before = self@;
        let out = match self.sessions.entry(id.client) {
            Entry::Vacant(e) => {
                e.insert(Session::new(id.sequence));
                Status::Ok
            }
            Entry::Occupied(e) => {
                let s = e.into_mut();
                proof { assert(s@ == before[id.client]); }
                if id.sequence < s.sequence || (id.sequence == s.sequence && s.terminal) {
                    Status::Invalid
                } else if id.sequence == s.sequence {
                    Status::Ok
                } else if !s.terminal || !s.holds.is_empty() {
                    Status::Busy
                } else {
                    // Retain the empty allocation; replacing a fence never wraps it.
                    s.sequence = id.sequence;
                    s.terminal = false;
                    proof { assert(s@.holds =~= Seq::<ScopeView>::empty()); }
                    Status::Ok
                }
            }
        };
        proof {
            assert(self@ =~= before.insert(id.client, self@[id.client]));
            if before.contains_key(id.client) && self@[id.client] == before[id.client] {
                assert(self@ =~= before);
            }
            assert forall|c: u64| self@.contains_key(c) implies self@[c].wf() by {
                if c != id.client { assert(self@[c] == before[c]); }
            }
        }
        out
    }

    pub fn acquire(&mut self, id: TxnId, table: u64, coordinate: &[u8], grant: Grant) -> (out: Status)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            acquire_effect(old(self)@, final(self)@, id, table, coordinate@, grant, out),
            out != Status::Ok ==> final(self)@ == old(self)@,
            out == Status::Ok ==> final(self).holds(id,table,coordinate@,grant)
                && !final(self)@[id.client].terminal,
    {
        let ghost before = self@;
        let out = match self.sessions.entry(id.client) {
            Entry::Vacant(_) => Status::NotFound,
            Entry::Occupied(e) => {
                let s = e.into_mut();
                proof { assert(s@ == before[id.client]); assert(s@.wf()); }
                if s.sequence != id.sequence || s.terminal {
                    Status::Invalid
                } else {
                    s.acquire(table, coordinate, grant)
                }
            }
        };
        proof {
            if before.contains_key(id.client) {
                assert(self@ =~= before.insert(id.client, self@[id.client]));
                if self@[id.client] == before[id.client] { assert(self@ =~= before); }
            } else { assert(self@ =~= before); }
            assert forall|c: u64| self@.contains_key(c) implies self@[c].wf() by {
                if c != id.client { assert(self@[c] == before[c]); }
            }
            if out == Status::Ok {
                registration_success(before[id.client],self@[id.client],point_scope(table,coordinate@,grant));
            }
        }
        out
    }

    /// Register one exact half-open interval. The caller must authorize this
    /// entire interval and registration under the same participant mutex.
    pub fn acquire_range(&mut self, id: TxnId, table: u64, lo: &[u8], hi: Option<&[u8]>, grant: Grant) -> (out: Status)
        requires old(self).wf(),
        ensures final(self).wf(),
            acquire_range_effect(old(self)@,final(self)@,id,table,lo@,hi_view(hi),grant,out),
            out != Status::Ok ==> final(self)@ == old(self)@,
            out == Status::Ok ==> final(self).holds_range(id,table,lo@,hi_view(hi),grant)
                && !final(self)@[id.client].terminal,
    {
        let ghost before = self@;
        // Validate before session lookup; malformed intervals never succeed.
        if !proper_range(lo,hi) { return Status::Invalid; }
        let out = match self.sessions.entry(id.client) {
            Entry::Vacant(_) => Status::NotFound,
            Entry::Occupied(e) => {
                let s = e.into_mut();
                proof { assert(s@ == before[id.client]); assert(s@.wf()); }
                if s.sequence != id.sequence || s.terminal { Status::Invalid }
                else { s.acquire_range(table,lo,hi,grant) }
            },
        };
        proof {
            if before.contains_key(id.client) {
                assert(self@ =~= before.insert(id.client,self@[id.client]));
                if self@[id.client] == before[id.client] { assert(self@ =~= before); }
            } else { assert(self@ =~= before); }
            assert forall|c: u64| self@.contains_key(c) implies self@[c].wf() by {
                if c != id.client { assert(self@[c] == before[c]); }
            }
            if out == Status::Ok {
                registration_success(before[id.client],self@[id.client],range_scope(table,lo@,hi_view(hi),grant));
            }
        }
        out
    }

    pub fn resolve(&mut self, id: TxnId) -> (out: Status)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            resolve_effect(old(self)@, final(self)@, id, out),
    {
        let ghost before = self@;
        let out = match self.sessions.entry(id.client) {
            Entry::Vacant(_) => Status::NotFound,
            Entry::Occupied(e) => {
                let s = e.into_mut();
                proof { assert(s@ == before[id.client]); assert(s@.wf()); }
                if s.sequence != id.sequence {
                    Status::Invalid
                } else {
                    s.terminal = true;
                    proof { assert(s@.holds =~= before[id.client].holds); }
                    Status::Ok
                }
            }
        };
        proof {
            if before.contains_key(id.client) {
                assert(self@ =~= before.insert(id.client, self@[id.client]));
                if self@[id.client] == before[id.client] { assert(self@ =~= before); }
            } else { assert(self@ =~= before); }
            assert forall|c: u64| self@.contains_key(c) implies self@[c].wf() by {
                if c != id.client { assert(self@[c] == before[c]); }
                assert(self@[c].holds == before[c].holds);
            }
        }
        out
    }

    /// Called only at the engine's transaction terminal/cleanup boundary, never
    /// for an individual RPC or a retry. Preserve the sequence fence while
    /// resolving and releasing every exact coordinate under the caller's lock.
    pub fn finish(&mut self, id: TxnId) -> (out: Status)
        requires old(self).wf(),
        ensures final(self).wf(), finish_effect(old(self)@,final(self)@,id,out),
    {
        let ghost before = self@;
        let out = match self.sessions.entry(id.client) {
            Entry::Vacant(_) => Status::NotFound,
            Entry::Occupied(e) => {
                let s = e.into_mut();
                proof { assert(s@ == before[id.client]); }
                if s.sequence != id.sequence { Status::Invalid }
                else {
                    s.terminal = true;
                    s.holds.clear();
                    proof { assert(s@.holds =~= Seq::<ScopeView>::empty()); }
                    Status::Ok
                }
            }
        };
        proof {
            if before.contains_key(id.client) {
                assert(self@ =~= before.insert(id.client,self@[id.client]));
                if self@[id.client] == before[id.client] { assert(self@ =~= before); }
            } else { assert(self@ =~= before); }
            assert forall|c: u64| self@.contains_key(c) implies self@[c].wf() by {
                if c != id.client { assert(self@[c] == before[c]); }
            }
        }
        out
    }

    pub fn release(&mut self, id: TxnId, table: u64, coordinate: &[u8]) -> (out: Status)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            release_effect(old(self)@, final(self)@, id, table, coordinate@, out),
    {
        let ghost before = self@;
        let out = match self.sessions.entry(id.client) {
            Entry::Vacant(_) => Status::NotFound,
            Entry::Occupied(e) => {
                let s = e.into_mut();
                proof { assert(s@ == before[id.client]); assert(s@.wf()); }
                if s.sequence != id.sequence || !s.terminal {
                    Status::Invalid
                } else {
                    s.release(table, coordinate)
                }
            }
        };
        proof {
            if before.contains_key(id.client) {
                assert(self@ =~= before.insert(id.client, self@[id.client]));
                if self@[id.client] == before[id.client] { assert(self@ =~= before); }
            } else { assert(self@ =~= before); }
            assert forall|c: u64| self@.contains_key(c) implies self@[c].wf() by {
                if c != id.client { assert(self@[c] == before[c]); }
            }
        }
        out
    }

    /// Access to a still-open held scope is independent of participant role.
    /// The caller applies its Serving/Frozen held-access policy under its lock.
    pub fn held(&self, id: TxnId, table: u64, coordinate: &[u8]) -> (out: Option<Grant>)
        requires self.wf(),
        ensures
            match out {
                Some(g) => self.holds(id, table, coordinate@, g) && !self@[id.client].terminal,
                None => !(self@.contains_key(id.client) && self@[id.client].sequence == id.sequence
                    && !self@[id.client].terminal && self@[id.client].scoped(table, coordinate@)),
            },
    {
        let Some(s) = self.sessions.get(&id.client) else { return None; };
        if s.sequence != id.sequence || s.terminal { return None; }
        let Some(i) = s.find(table, coordinate) else { return None; };
        Some(s.holds[i].grant)
    }

    /// A single registered containing range suffices. A fragmented union is
    /// deliberately not a held-range capability; acquire_range can register it.
    pub fn held_range(&self, id: TxnId, table: u64, lo: &[u8], hi: Option<&[u8]>) -> (out: Option<Grant>)
        requires self.wf(),
        ensures match out {
            Some(g) => self.holds_range(id,table,lo@,hi_view(hi),g) && !self@[id.client].terminal,
            None => !(self@.contains_key(id.client) && self@[id.client].sequence == id.sequence
                && !self@[id.client].terminal && self@[id.client].range_scoped(table,lo@,hi_view(hi))),
        },
    {
        if !proper_range(lo,hi) { return None; }
        let Some(s) = self.sessions.get(&id.client) else { return None; };
        if s.sequence != id.sequence || s.terminal { return None; }
        let mut i = 0;
        while i < s.holds.len()
            invariant i <= s.holds.len(), self@.contains_key(id.client), s@ == self@[id.client],
                s.sequence == id.sequence, !s.terminal, proper(lo@,hi_view(hi)),
                forall|j: int| 0 <= j < i ==> !s@.holds[j].covers_range(table,lo@,hi_view(hi)),
            decreases s.holds.len() - i,
        {
            if s.holds[i].covers_range(table,lo,hi) {
                proof {
                    assert(s@.holds[i as int].covers_range(table,lo@,hi_view(hi)));
                    assert(s@.has_range(table,lo@,hi_view(hi),s@.holds[i as int].grant));
                }
                return Some(s.holds[i].grant);
            }
            i += 1;
        }
        None
    }

    pub fn drained(&self, range: &KeyRange) -> (out: bool)
        ensures out == self.drained_spec(*range),
    {
        let mut iter = self.sessions.values();
        let ghost all = iter.remaining();
        proof {
            assert forall|c: u64| self.sessions@.contains_key(c) implies
                all.unref().to_set().contains(self.sessions@[c]) by {
                assert(self.sessions@.values().contains(self.sessions@[c]));
            }
        }
        loop
            invariant
                forall|s: &Session| all.contains(s) && !iter.remaining().contains(s) ==> s@.drained(*range),
                forall|s: &Session| iter.remaining().contains(s) ==> all.contains(s),
                all.unref().to_set() == self.sessions@.values(),
                iter.decrease() is Some,
            decreases iter.decrease().unwrap(),
        {
            let ghost remaining = iter.remaining();
            let Some(s) = iter.next() else {
                proof {
                    assert forall|c: u64| self@.contains_key(c) implies self@[c].drained(*range) by {
                        let value = self.sessions@[c];
                        assert(all.unref().to_set().contains(value));
                        let i = choose|i: int| 0 <= i < all.len() && *all[i] == value;
                        assert(all.contains(all[i]));
                    }
                }
                return true;
            };
            if !s.drained(range) {
                proof {
                    assert(all.contains(s));
                    let i = choose|i: int| 0 <= i < all.len() && all[i] == s;
                    assert(all.unref()[i] == *s);
                    assert(all.unref().to_set().contains(*s));
                    let c = choose|c: u64| self.sessions@.contains_key(c) && self.sessions@[c] == *s;
                    assert(!self@[c].drained(*range));
                }
                return false;
            }
            proof {
                assert forall|other: &Session| all.contains(other) && !iter.remaining().contains(other)
                    implies other@.drained(*range) by {
                    if remaining.contains(other) {
                        let j = choose|j: int| 0 <= j < remaining.len() && remaining[j] == other;
                        if j != 0 { assert(iter.remaining()[j - 1] == other); }
                        assert(other == s);
                    }
                }
            }
        }
    }
}

pub open spec fn frame(before: Map<u64, SessionView>, after: Map<u64, SessionView>, client: u64) -> bool {
    before.dom() == after.dom()
        && forall|c: u64| before.contains_key(c) && c != client ==> after[c] == before[c]
}

pub open spec fn begin_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId, status: Status) -> bool {
    if !before.contains_key(id.client) {
        status == Status::Ok && after == before.insert(id.client,
            SessionView { sequence: id.sequence, terminal: false, holds: Seq::empty() })
    } else {
        let s = before[id.client];
        if id.sequence < s.sequence || (id.sequence == s.sequence && s.terminal) {
            status == Status::Invalid && after == before
        } else if id.sequence == s.sequence {
            status == Status::Ok && after == before
        } else if !s.terminal || s.holds.len() != 0 {
            status == Status::Busy && after == before
        } else {
            status == Status::Ok && after == before.insert(id.client,
                SessionView { sequence: id.sequence, terminal: false, holds: Seq::empty() })
        }
    }
}

pub open spec fn register_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    scope: ScopeView, status: Status) -> bool {
    if !scope.valid() {
        status == Status::Invalid && after == before
    } else if !before.contains_key(id.client) {
        status == Status::NotFound && after == before
    } else if before[id.client].sequence != id.sequence || before[id.client].terminal {
        status == Status::Invalid && after == before
    } else {
        after == before.insert(id.client,after[id.client])
            && registration(before[id.client],after[id.client],scope,status)
    }
}

pub open spec fn acquire_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, grant: Grant, status: Status) -> bool {
    register_effect(before,after,id,point_scope(table,coordinate,grant),status)
}

pub open spec fn acquire_range_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>, grant: Grant, status: Status) -> bool {
    register_effect(before,after,id,range_scope(table,lo,hi,grant),status)
}

pub open spec fn resolve_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId, status: Status) -> bool {
    if !before.contains_key(id.client) {
        status == Status::NotFound && after == before
    } else if before[id.client].sequence != id.sequence {
        status == Status::Invalid && after == before
    } else {
        status == Status::Ok && after == before.insert(id.client, SessionView { terminal: true, ..before[id.client] })
    }
}

pub open spec fn finish_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId, status: Status) -> bool {
    if !before.contains_key(id.client) {
        status == Status::NotFound && after == before
    } else if before[id.client].sequence != id.sequence {
        status == Status::Invalid && after == before
    } else {
        status == Status::Ok && after == before.insert(id.client,
            SessionView { sequence: id.sequence, terminal: true, holds: Seq::empty() })
    }
}

pub open spec fn release_effect(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, status: Status) -> bool {
    if !before.contains_key(id.client) {
        status == Status::NotFound && after == before
    } else if before[id.client].sequence != id.sequence || !before[id.client].terminal {
        status == Status::Invalid && after == before
    } else {
        frame(before,after,id.client)
            && point_release(before[id.client],after[id.client],table,coordinate,status)
    }
}

} // verus!
