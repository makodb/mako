//! Concrete byte interval metadata used by Participant, independently checked
//! without depending on the native storage capability implementation.
use vstd::prelude::*;
use crate::types::*;
use crate::bytes::{compare,copy_bytes};
#[cfg(verus_keep_ghost)]
#[path = "participant_intervals.rs"]
pub mod intervals;
verus! {
pub struct MetaBoundary { pub start: Vec<u8>, pub meta: ReplicaMeta }
pub struct MetaTable { pub boundaries: Vec<MetaBoundary> }

pub open spec fn meta_ordered(p: Seq<MetaBoundary>) -> bool {
    forall|i: int,j: int| 0 <= i < j < p.len() ==>
        crate::bytes::cmp_spec(#[trigger] p[i].start@,#[trigger] p[j].start@) < 0
}
pub open spec fn meta_wf(p: Seq<MetaBoundary>) -> bool {
    p.len() > 0 && p[0].start@ == Seq::<u8>::empty() && meta_ordered(p)
}
pub open spec fn meta_route(p: Seq<MetaBoundary>, k: Seq<u8>) -> Option<ReplicaMeta>
    decreases p.len(),
{
    if p.len() == 0 { None }
    else if crate::bytes::cmp_spec(p.last().start@,k) <= 0 { Some(p.last().meta) }
    else { meta_route(p.drop_last(),k) }
}
pub proof fn selected_meta(p: Seq<MetaBoundary>, i: int, k: Seq<u8>)
    requires 0 <= i < p.len(), crate::bytes::cmp_spec(p[i].start@,k) <= 0,
        forall|j: int| i < j < p.len() ==> crate::bytes::cmp_spec(#[trigger] p[j].start@,k) > 0,
    ensures meta_route(p,k) == Some(p[i].meta),
    decreases p.len(),
{
    if i < p.len()-1 { selected_meta(p.drop_last(),i,k); }
}
pub proof fn selected_meta_index(p: Seq<MetaBoundary>, k: Seq<u8>) -> (i: int)
    requires meta_wf(p),
    ensures 0 <= i < p.len(), crate::bytes::cmp_spec(p[i].start@,k) <= 0,
        forall|j: int| i < j < p.len() ==> crate::bytes::cmp_spec(#[trigger] p[j].start@,k) > 0,
        meta_route(p,k) == Some(p[i].meta),
    decreases p.len(),
{
    crate::bytes::cmp_laws(Seq::empty(),k);
    if crate::bytes::cmp_spec(p.last().start@,k) <= 0 { p.len() as int - 1 }
    else {
        assert(p.len() > 1);
        assert(meta_wf(p.drop_last())) by {
            assert forall|a: int,b: int| 0 <= a < b < p.drop_last().len() implies
                crate::bytes::cmp_spec(p.drop_last()[a].start@,p.drop_last()[b].start@) < 0 by {}
        }
        let i = selected_meta_index(p.drop_last(),k);
        assert forall|j: int| i < j < p.len() implies crate::bytes::cmp_spec(p[j].start@,k) > 0 by {
            if j < p.len()-1 { assert(p[j] == p.drop_last()[j]); }
        }
        i
    }
}
pub proof fn uniform_meta(p: Seq<MetaBoundary>, range: KeyRange, predicate: spec_fn(ReplicaMeta) -> bool)
    requires meta_wf(p), meta_route(p,range.lo@).is_some(), predicate(meta_route(p,range.lo@).unwrap()),
        forall|i: int| 0 <= i < p.len() && crate::bytes::contains_spec(range,range.table,p[i].start@) ==> predicate(p[i].meta),
    ensures forall|k: Seq<u8>| crate::bytes::contains_spec(range,range.table,k) ==>
        meta_route(p,k).is_some() && predicate(meta_route(p,k).unwrap()),
{
    assert forall|k: Seq<u8>| crate::bytes::contains_spec(range,range.table,k) implies
        meta_route(p,k).is_some() && predicate(meta_route(p,k).unwrap()) by {
        let i = selected_meta_index(p,k);
        if crate::bytes::cmp_spec(range.lo@,p[i].start@) <= 0 {
            if let Some(h) = range.hi { crate::bytes::cmp_trans(p[i].start@,k,h@); }
            assert(crate::bytes::contains_spec(range,range.table,p[i].start@));
        } else {
            crate::bytes::cmp_laws(range.lo@,p[i].start@);
            assert forall|j: int| i < j < p.len() implies crate::bytes::cmp_spec(p[j].start@,range.lo@) > 0 by {
                crate::bytes::cmp_laws(p[j].start@,k);
                crate::bytes::cmp_trans(range.lo@,k,p[j].start@);
                crate::bytes::cmp_laws(range.lo@,p[j].start@);
            }
            selected_meta(p,i,range.lo@);
        }
    }
}

/// These scalar guards are the guards used by the real interval walk below.
/// Authentic command issuance and the immutable envelope are transport premises.
pub fn command_guard(m: ReplicaMeta, old_epoch: u64, generation: u64,
    command: Command, source: bool, destination: bool, drained: bool) -> (yes: bool)
    ensures yes == guard_spec(m,old_epoch,generation,command,source,destination,drained),
{
    match command {
        Command::Start => destination && matches!(m.role,Role::Empty) && m.fence < generation,
        Command::Freeze => source && matches!(m.role,Role::Serving) && m.epoch == old_epoch && m.fence < generation,
        Command::Final => destination && matches!(m.role,Role::Stage) && m.fence == generation && !m.terminal && m.round == 0,
        Command::Retire => source && matches!(m.role,Role::Frozen) && m.fence == generation && !m.terminal && drained,
        Command::Commit => ((destination && matches!(m.role,Role::Ready)) || (source && matches!(m.role,Role::Retired)))
            && m.fence == generation && !m.terminal,
        Command::Abort => (source || destination) && (m.fence < generation || (m.fence == generation && !m.terminal)),
    }
}

pub open spec fn guard_spec(m: ReplicaMeta, old_epoch: u64, generation: u64,
    command: Command, source: bool, destination: bool, drained: bool) -> bool {
    match command {
        Command::Start => destination && m.role is Empty && m.fence < generation,
        Command::Freeze => source && m.role is Serving && m.epoch == old_epoch && m.fence < generation,
        Command::Final => destination && m.role is Stage && m.fence == generation && !m.terminal && m.round == 0,
        Command::Retire => source && m.role is Frozen && m.fence == generation && !m.terminal && drained,
        Command::Commit => ((destination && m.role is Ready) || (source && m.role is Retired))
            && m.fence == generation && !m.terminal,
        Command::Abort => (source || destination) && (m.fence < generation || (m.fence == generation && !m.terminal)),
    }
}

pub open spec fn effect_spec(m: ReplicaMeta, generation: u64, command: Command, source: bool) -> ReplicaMeta {
    match command {
        Command::Start => ReplicaMeta { epoch: generation, fence: generation, terminal: false, role: Role::Stage, round: 0 },
        Command::Freeze => ReplicaMeta { role: Role::Frozen, fence: generation, terminal: false, ..m },
        Command::Final => ReplicaMeta { round: 1, ..m },
        Command::Retire => ReplicaMeta { role: Role::Retired, ..m },
        Command::Commit => ReplicaMeta { role: if source { Role::Empty } else { Role::Serving }, terminal: true, ..m },
        Command::Abort => ReplicaMeta { role: if source { Role::Serving } else { Role::Empty }, fence: generation, terminal: true, ..m },
    }
}

pub fn command_effect(m: ReplicaMeta, generation: u64, command: Command, source: bool) -> (out: ReplicaMeta)
    ensures out == effect_spec(m,generation,command,source),
{
    match command {
        Command::Start => ReplicaMeta { epoch: generation, fence: generation, terminal: false, role: Role::Stage, round: 0 },
        Command::Freeze => ReplicaMeta { role: Role::Frozen, fence: generation, terminal: false, ..m },
        Command::Final => ReplicaMeta { round: 1, ..m },
        Command::Retire => ReplicaMeta { role: Role::Retired, ..m },
        Command::Commit => ReplicaMeta { role: if source { Role::Empty } else { Role::Serving }, terminal: true, ..m },
        Command::Abort => ReplicaMeta { role: if source { Role::Serving } else { Role::Empty }, fence: generation, terminal: true, ..m },
    }
}

pub fn copy_guard(m: ReplicaMeta, generation: u64, round: u64) -> (yes: bool)
    ensures yes == (m.role is Stage && m.fence == generation && !m.terminal && m.round == round),
{ matches!(m.role,Role::Stage) && m.fence == generation && !m.terminal && m.round == round }

pub fn cleanup_guard(m: ReplicaMeta, generation: u64) -> (yes: bool)
    ensures yes == (m.role is Empty && m.fence == generation && m.terminal),
{ matches!(m.role,Role::Empty) && m.fence == generation && m.terminal }
pub fn role_equal(a: Role, b: Role) -> (yes: bool)
    ensures yes == (a == b),
{
    matches!((a,b),(Role::Empty,Role::Empty) | (Role::Serving,Role::Serving)
        | (Role::Frozen,Role::Frozen) | (Role::Retired,Role::Retired)
        | (Role::Stage,Role::Stage) | (Role::Ready,Role::Ready))
}
fn meta_equal(a: ReplicaMeta, b: ReplicaMeta) -> (yes: bool)
    ensures yes == (a == b),
{
    a.epoch == b.epoch && a.fence == b.fence && a.terminal == b.terminal && a.round == b.round && role_equal(a.role,b.role)
}
fn below_hi(k: &[u8], hi: &Option<Vec<u8>>) -> (yes: bool)
    ensures yes == match *hi { Some(h) => crate::bytes::cmp_spec(k@,h@) < 0, None => true },
{
    match hi { Some(h) => compare(k,h) < 0, None => true }
}
fn below_slice(k: &[u8], hi: Option<&[u8]>) -> (yes: bool)
    ensures yes == match crate::directory::hi_view(hi) { Some(h) => crate::bytes::cmp_spec(k@,h) < 0, None => true },
{
    match hi { Some(h) => compare(k,h) < 0, None => true }
}
impl MetaTable {
    pub(super) fn serving_range(&self, lo: &[u8], hi: Option<&[u8]>, epoch: u64) -> (yes: bool)
        ensures yes && meta_wf(self.boundaries@) ==> forall|k: Seq<u8>|
            crate::directory::inside(k,lo@,crate::directory::hi_view(hi)) ==>
                meta_route(self.boundaries@,k).is_some()
                && meta_route(self.boundaries@,k).unwrap().role is Serving
                && meta_route(self.boundaries@,k).unwrap().epoch == epoch,
    {
        let at = match self.index(lo) { Some(i) => i, None => return false };
        let first = self.boundaries[at].meta;
        if !matches!(first.role,Role::Serving) || first.epoch != epoch { return false; }
        let mut i = at + 1;
        proof {
            if meta_wf(self.boundaries@) {
                assert forall|j: int| 0 <= j < i && crate::directory::inside(self.boundaries@[j].start@,lo@,crate::directory::hi_view(hi)) implies
                    self.boundaries@[j].meta.role is Serving && self.boundaries@[j].meta.epoch == epoch by {
                    if j < at {
                        crate::bytes::cmp_trans(self.boundaries@[j].start@,self.boundaries@[at as int].start@,lo@);
                        crate::bytes::cmp_laws(self.boundaries@[j].start@,lo@);
                    }
                }
            }
        }
        while i < self.boundaries.len() && below_slice(&self.boundaries[i].start,hi)
            invariant i <= self.boundaries.len(),
                meta_wf(self.boundaries@) ==> meta_route(self.boundaries@,lo@).is_some()
                    && meta_route(self.boundaries@,lo@).unwrap().role is Serving
                    && meta_route(self.boundaries@,lo@).unwrap().epoch == epoch,
                meta_wf(self.boundaries@) ==> forall|j: int| 0 <= j < i &&
                    crate::directory::inside(self.boundaries@[j].start@,lo@,crate::directory::hi_view(hi)) ==>
                        self.boundaries@[j].meta.role is Serving && self.boundaries@[j].meta.epoch == epoch,
            decreases self.boundaries.len()-i,
        {
            let m = self.boundaries[i].meta;
            if !matches!(m.role,Role::Serving) || m.epoch != epoch { return false; }
            i += 1;
        }
        proof {
            if meta_wf(self.boundaries@) {
                assert forall|j: int| 0 <= j < self.boundaries.len() &&
                    crate::directory::inside(self.boundaries@[j].start@,lo@,crate::directory::hi_view(hi)) implies
                        self.boundaries@[j].meta.role is Serving && self.boundaries@[j].meta.epoch == epoch by {
                    if j >= i {
                        if let Some(h) = hi {
                            crate::bytes::cmp_laws(self.boundaries@[i as int].start@,h@);
                            if j > i { crate::bytes::cmp_trans(h@,self.boundaries@[i as int].start@,self.boundaries@[j].start@); }
                            crate::bytes::cmp_laws(h@,self.boundaries@[j].start@);
                        }
                    }
                }
                intervals::uniform_bounds(self.boundaries@,lo@,crate::directory::hi_view(hi),
                    |m: ReplicaMeta| m.role is Serving && m.epoch == epoch);
            }
        }
        true
    }
    fn index(&self, coordinate: &[u8]) -> (out: Option<usize>)
        ensures out.is_some() ==> out.unwrap() < self.boundaries.len(),
            meta_wf(self.boundaries@) ==> out.is_some()
                && meta_route(self.boundaries@,coordinate@) == Some(self.boundaries@[out.unwrap() as int].meta),
            meta_wf(self.boundaries@) ==> crate::bytes::cmp_spec(self.boundaries@[out.unwrap() as int].start@,coordinate@) <= 0
                && (forall|j: int| out.unwrap() < j < self.boundaries.len() ==> crate::bytes::cmp_spec(self.boundaries@[j].start@,coordinate@) > 0),
    {
        if self.boundaries.len() == 0 { return None; }
        let mut lo = 1usize;
        let mut hi = self.boundaries.len();
        proof { crate::bytes::cmp_laws(Seq::empty(),coordinate@); }
        while lo < hi
            invariant 1 <= lo <= hi <= self.boundaries.len(),
                meta_wf(self.boundaries@) ==> (
                    (forall|j: int| 0 <= j < lo ==> crate::bytes::cmp_spec(#[trigger] self.boundaries@[j].start@,coordinate@) <= 0)
                    && (forall|j: int| hi <= j < self.boundaries.len() ==> crate::bytes::cmp_spec(#[trigger] self.boundaries@[j].start@,coordinate@) > 0)),
            decreases hi - lo,
        {
            let mid = lo + (hi - lo) / 2;
            if compare(&self.boundaries[mid].start,coordinate) <= 0 {
                proof {
                    if meta_wf(self.boundaries@) {
                        assert forall|j: int| 0 <= j <= mid implies crate::bytes::cmp_spec(self.boundaries@[j].start@,coordinate@) <= 0 by {
                            if j < mid { crate::bytes::cmp_trans(self.boundaries@[j].start@,self.boundaries@[mid as int].start@,coordinate@); }
                        }
                    }
                }
                lo = mid + 1;
            } else {
                proof {
                    if meta_wf(self.boundaries@) {
                        crate::bytes::cmp_laws(self.boundaries@[mid as int].start@,coordinate@);
                        assert forall|j: int| mid <= j < self.boundaries.len() implies crate::bytes::cmp_spec(self.boundaries@[j].start@,coordinate@) > 0 by {
                            if j > mid {
                                crate::bytes::cmp_trans(coordinate@,self.boundaries@[mid as int].start@,self.boundaries@[j].start@);
                                crate::bytes::cmp_laws(coordinate@,self.boundaries@[j].start@);
                            }
                        }
                    }
                }
                hi = mid;
            }
        }
        proof { if meta_wf(self.boundaries@) { selected_meta(self.boundaries@,lo as int-1,coordinate@); } }
        Some(lo - 1)
    }
    pub(super) fn lookup(&self, coordinate: &[u8]) -> (out: Option<ReplicaMeta>)
        ensures meta_wf(self.boundaries@) ==> out == meta_route(self.boundaries@,coordinate@),
    {
        match self.index(coordinate) { Some(i) => Some(self.boundaries[i].meta), None => None }
    }
    pub(super) fn push(out: &mut Vec<MetaBoundary>, start: &[u8], meta: ReplicaMeta)
        ensures
            (old(out).len() == 0 || crate::bytes::cmp_spec(old(out)@.last().start@,start@) <= 0) ==>
                forall|k: Seq<u8>| meta_route(final(out)@,k) ==
                    if crate::bytes::cmp_spec(start@,k) <= 0 { Some(meta) } else { meta_route(old(out)@,k) },
            meta_ordered(old(out)@) && (forall|i: int| 0 <= i < old(out).len() ==> crate::bytes::cmp_spec(old(out)@[i].start@,start@) < 0)
                ==> meta_ordered(final(out)@),
            final(out).len() > 0,
            final(out)@[0].start@ == if old(out).len() > 0 { old(out)@[0].start@ } else { start@ },
            (forall|i: int| 0 <= i < old(out).len() ==> crate::bytes::cmp_spec(old(out)@[i].start@,start@) < 0) ==>
                forall|i: int| 0 <= i < final(out).len() ==> crate::bytes::cmp_spec(final(out)@[i].start@,start@) <= 0,
            forall|i: int| 0 <= i < final(out).len() ==> final(out)@[i].start@ == start@
                || exists|j: int| 0 <= j < old(out).len() && final(out)@[i].start@ == old(out)@[j].start@,
    {
        let ghost before = out@;
        proof { crate::bytes::cmp_laws(start@,start@); }
        if out.len() == 0 || !meta_equal(out[out.len()-1].meta,meta) {
            out.push(MetaBoundary { start: copy_bytes(start), meta });
        }
        proof {
            if before.len() > 0 && before.last().meta == meta {
                assert forall|k: Seq<u8>| crate::bytes::cmp_spec(before.last().start@,start@) <= 0
                    implies meta_route(out@,k) == if crate::bytes::cmp_spec(start@,k) <= 0 { Some(meta) } else { meta_route(before,k) } by {
                    if crate::bytes::cmp_spec(start@,k) <= 0 { crate::bytes::cmp_trans(before.last().start@,start@,k); }
                }
            } else {
                assert forall|k: Seq<u8>| meta_route(out@,k) ==
                    if crate::bytes::cmp_spec(start@,k) <= 0 { Some(meta) } else { meta_route(before,k) } by {
                    assert(out@.drop_last() =~= before);
                }
                assert forall|a: int,b: int| meta_ordered(before)
                    && (forall|j: int| 0 <= j < before.len() ==> crate::bytes::cmp_spec(before[j].start@,start@) < 0)
                    && 0 <= a < b < out.len() implies crate::bytes::cmp_spec(out@[a].start@,out@[b].start@) < 0 by {}
                assert forall|a: int| 0 <= a < out.len() implies out@[a].start@ == start@
                    || exists|j: int| 0 <= j < before.len() && out@[a].start@ == before[j].start@ by {
                    if a < before.len() { assert(out@[a].start@ == before[a].start@); }
                }
            }
        }
    }
    fn copy_prefix(&self, lo: &[u8]) -> (r: (Vec<MetaBoundary>,usize))
        ensures r.1 <= self.boundaries.len(),
            meta_wf(self.boundaries@) ==> meta_ordered(r.0@)
                && (r.0.len() == 0 <==> r.1 == 0)
                && (r.0.len() > 0 ==> r.0@[0].start@ == Seq::<u8>::empty())
                && (r.0.len() == 0 ==> lo@ == Seq::<u8>::empty())
                && (forall|j: int| 0 <= j < r.0.len() ==> crate::bytes::cmp_spec(r.0@[j].start@,lo@) < 0)
                && (forall|j: int| 0 <= j < r.1 ==> crate::bytes::cmp_spec(self.boundaries@[j].start@,lo@) < 0)
                && (r.1 < self.boundaries.len() ==> crate::bytes::cmp_spec(self.boundaries@[r.1 as int].start@,lo@) >= 0)
                && (forall|k: Seq<u8>| crate::bytes::cmp_spec(lo@,k) > 0 ==> meta_route(r.0@,k) == meta_route(self.boundaries@,k)),
    {
        let mut out = Vec::new();
        let mut i = 0usize;
        while i < self.boundaries.len() && compare(&self.boundaries[i].start,lo) < 0
            invariant i <= self.boundaries.len(), intervals::equivalent(out@,self.boundaries@.take(i as int)),
                forall|j: int| 0 <= j < i ==> crate::bytes::cmp_spec(self.boundaries@[j].start@,lo@) < 0,
            decreases self.boundaries.len()-i,
        {
            let b = &self.boundaries[i];
            out.push(MetaBoundary { start:copy_bytes(&b.start),meta:b.meta });
            i += 1;
            proof { assert(intervals::equivalent(out@,self.boundaries@.take(i as int))); }
        }
        proof {
            if meta_wf(self.boundaries@) {
                intervals::copied_prefix(self.boundaries@,out@,i as int,lo@);
            }
        }
        (out,i)
    }
    fn restore_suffix(&self, out: &mut Vec<MetaBoundary>, hi: &[u8])
        ensures meta_wf(self.boundaries@) && meta_wf(old(out)@)
            && (forall|j: int| 0 <= j < old(out).len() ==> crate::bytes::cmp_spec(old(out)@[j].start@,hi@) < 0) ==>
                meta_wf(final(out)@)
                && (forall|k: Seq<u8>| meta_route(final(out)@,k) == if crate::bytes::cmp_spec(k,hi@) < 0 {
                    meta_route(old(out)@,k)
                } else { meta_route(self.boundaries@,k) }),
    {
        let ghost p = self.boundaries@;
        let ghost before = out@;
        let ghost good = meta_wf(p) && meta_wf(before)
            && (forall|j: int| 0 <= j < before.len() ==> crate::bytes::cmp_spec(before[j].start@,hi@) < 0);
        let at = match self.index(hi) { Some(i) => i, None => return };
        let m = self.boundaries[at].meta;
        Self::push(out,hi,m);
        let mut i = at + 1;
        proof {
            if good {
                if i < p.len() { crate::bytes::cmp_laws(p[i as int].start@,hi@); }
                intervals::prefix_constant(p,i as int,hi@);
                assert forall|k: Seq<u8>| meta_route(out@,k) == if crate::bytes::cmp_spec(k,hi@) < 0 {
                    meta_route(before,k)
                } else { meta_route(p.take(i as int),k) } by { crate::bytes::cmp_laws(k,hi@); }
                assert forall|j: int| 0 <= j < out.len() && i < p.len() implies crate::bytes::cmp_spec(out@[j].start@,p[i as int].start@) < 0 by {
                    crate::bytes::cmp_trans(out@[j].start@,hi@,p[i as int].start@);
                }
            }
        }
        while i < self.boundaries.len()
            invariant i <= self.boundaries.len(), self.boundaries@ == p,
                good == (meta_wf(p) && meta_wf(before)
                    && (forall|j: int| 0 <= j < before.len() ==> crate::bytes::cmp_spec(before[j].start@,hi@) < 0)),
                good ==> 0 < i && meta_wf(out@),
                good ==> i == p.len() || crate::bytes::cmp_spec(hi@,p[i as int].start@) < 0,
                good ==> forall|j: int| 0 <= j < out.len() && i < p.len() ==> crate::bytes::cmp_spec(out@[j].start@,p[i as int].start@) < 0,
                good ==> forall|k: Seq<u8>| meta_route(out@,k) ==
                    if crate::bytes::cmp_spec(k,hi@) < 0 { meta_route(before,k) }
                    else { meta_route(p.take(i as int),k) },
            decreases self.boundaries.len() - i,
        {
            let b = &self.boundaries[i];
            Self::push(out,&b.start,b.meta);
            i += 1;
            proof {
                if good {
                    if i < p.len() { crate::bytes::cmp_trans(hi@,p[i as int-1].start@,p[i as int].start@); }
                    assert forall|j: int| 0 <= j < out.len() && i < p.len() implies crate::bytes::cmp_spec(out@[j].start@,p[i as int].start@) < 0 by {
                        crate::bytes::cmp_trans(out@[j].start@,p[i as int-1].start@,p[i as int].start@);
                    }
                    assert(p.take(i as int).drop_last() =~= p.take(i as int-1));
                    assert forall|k: Seq<u8>| crate::bytes::cmp_spec(k,hi@) < 0 implies crate::bytes::cmp_spec(p[i as int-1].start@,k) > 0 by {
                        crate::bytes::cmp_trans(k,hi@,p[i as int-1].start@); crate::bytes::cmp_laws(k,p[i as int-1].start@);
                    }
                }
            }
        }
        proof { assert(p.take(p.len() as int) =~= p); }
    }
    fn replace_middle(&self, range: &KeyRange, generation: u64, command: Option<Command>, source: bool) -> (out: Vec<MetaBoundary>)
        ensures meta_wf(self.boundaries@)
            && crate::directory::proper(range.lo@,match range.hi { Some(h) => Some(h@), None => None }) ==>
                meta_wf(out@)
                && (forall|j: int| 0 <= j < out.len() ==> match range.hi { Some(h) => crate::bytes::cmp_spec(#[trigger] out@[j].start@,h@) < 0, None => true })
                && (forall|k: Seq<u8>| (match range.hi { Some(h) => crate::bytes::cmp_spec(k,h@) < 0, None => true }) ==>
                    meta_route(out@,k) == if crate::bytes::cmp_spec(range.lo@,k) <= 0 {
                        match meta_route(self.boundaries@,k) { Some(m) => Some(intervals::transformed(m,generation,command,source)), None => None }
                    } else { meta_route(self.boundaries@,k) }),
    {
        let ghost p = self.boundaries@;
        let ghost good = meta_wf(p) && crate::directory::proper(range.lo@,match &range.hi { Some(h) => Some(h@), None => None });
        let at = match self.index(&range.lo) { Some(i) => i, None => return Vec::new() };
        let m = self.boundaries[at].meta;
        let (mut out,_) = self.copy_prefix(&range.lo);
        let next = match command { Some(c) => command_effect(m,generation,c,source),
            None => ReplicaMeta { role: Role::Ready, ..m } };
        Self::push(&mut out,&range.lo,next);
        let mut i = at + 1;
        proof {
            if good {
                if i < p.len() { crate::bytes::cmp_laws(p[i as int].start@,range.lo@); }
                intervals::prefix_constant(p,i as int,range.lo@);
                assert forall|j: int| 0 <= j < out.len() implies
                    (i < p.len() ==> crate::bytes::cmp_spec(out@[j].start@,p[i as int].start@) < 0)
                    && (match range.hi { Some(h) => crate::bytes::cmp_spec(out@[j].start@,h@) < 0, None => true }) by {
                    if i < p.len() { crate::bytes::cmp_trans(out@[j].start@,range.lo@,p[i as int].start@); }
                    if let Some(h) = range.hi { crate::bytes::cmp_trans(out@[j].start@,range.lo@,h@); }
                }
                if let Some(h) = &range.hi { crate::bytes::cmp_trans(p[i as int-1].start@,range.lo@,h@); }
            }
        }
        while i < self.boundaries.len() && below_hi(&self.boundaries[i].start,&range.hi)
            invariant i <= self.boundaries.len(), self.boundaries@ == p,
                good == (meta_wf(p) && crate::directory::proper(range.lo@,match range.hi { Some(h) => Some(h@), None => None })),
                good ==> 0 < i && meta_wf(out@),
                good ==> match range.hi { Some(h) => crate::bytes::cmp_spec(p[i as int-1].start@,h@) < 0, None => true },
                good ==> i == p.len() || crate::bytes::cmp_spec(range.lo@,p[i as int].start@) < 0,
                good ==> forall|j: int| 0 <= j < out.len() ==>
                    (i < p.len() ==> crate::bytes::cmp_spec(out@[j].start@,p[i as int].start@) < 0)
                    && (match range.hi { Some(h) => crate::bytes::cmp_spec(out@[j].start@,h@) < 0, None => true }),
                good ==> forall|k: Seq<u8>| meta_route(out@,k) ==
                    if crate::bytes::cmp_spec(range.lo@,k) > 0 { meta_route(p,k) }
                    else { match meta_route(p.take(i as int),k) { Some(m) => Some(intervals::transformed(m,generation,command,source)), None => None } },
            decreases self.boundaries.len() - i,
        {
            let b = &self.boundaries[i];
            let next = match command { Some(c) => command_effect(b.meta,generation,c,source),
                None => ReplicaMeta { role: Role::Ready, ..b.meta } };
            Self::push(&mut out,&b.start,next);
            i += 1;
            proof {
                if good {
                    if i < p.len() { crate::bytes::cmp_trans(range.lo@,p[i as int-1].start@,p[i as int].start@); }
                    assert forall|j: int| 0 <= j < out.len() implies
                        (i < p.len() ==> crate::bytes::cmp_spec(out@[j].start@,p[i as int].start@) < 0)
                        && (match range.hi { Some(h) => crate::bytes::cmp_spec(out@[j].start@,h@) < 0, None => true }) by {
                        if i < p.len() { crate::bytes::cmp_trans(out@[j].start@,p[i as int-1].start@,p[i as int].start@); }
                        if let Some(h) = range.hi { crate::bytes::cmp_trans(out@[j].start@,p[i as int-1].start@,h@); }
                    }
                    assert(p.take(i as int).drop_last() =~= p.take(i as int-1));
                    assert forall|k: Seq<u8>| meta_route(out@,k) ==
                        if crate::bytes::cmp_spec(range.lo@,k) > 0 { meta_route(p,k) }
                        else { match meta_route(p.take(i as int),k) { Some(m) => Some(intervals::transformed(m,generation,command,source)), None => None } } by {
                        if crate::bytes::cmp_spec(range.lo@,k) > 0 {
                            crate::bytes::cmp_laws(range.lo@,k); crate::bytes::cmp_trans(k,range.lo@,p[i as int-1].start@);
                            crate::bytes::cmp_laws(k,p[i as int-1].start@);
                        }
                    }
                }
            }
        }
        if let Some(hi) = &range.hi {
            proof {
                if good {
                    intervals::routes_below(p,i as int,hi@);
                    assert forall|k: Seq<u8>| crate::bytes::cmp_spec(k,hi@) < 0 implies meta_route(p.take(i as int),k) == meta_route(p,k) by {
                        crate::bytes::cmp_laws(k,hi@);
                    }
                }
            }
        }
        proof {
            assert(p.take(p.len() as int) =~= p);
            if good {
                assert forall|k: Seq<u8>| (match range.hi { Some(h) => crate::bytes::cmp_spec(k,h@) < 0, None => true }) implies
                    meta_route(out@,k) == if crate::bytes::cmp_spec(range.lo@,k) <= 0 {
                        match meta_route(p,k) { Some(m) => Some(intervals::transformed(m,generation,command,source)), None => None }
                    } else { meta_route(p,k) } by {
                    crate::bytes::cmp_laws(range.lo@,k);
                    if let Some(h) = range.hi { crate::bytes::cmp_laws(k,h@); }
                }
            }
        }
        out
    }
    /// Materialize both endpoints, transform every intersected piece, and merge
    /// only full metadata equality. Old fences outside a partial return survive.
    pub(super) fn transform(&mut self, range: &KeyRange, generation: u64, command: Option<Command>, source: bool)
        ensures meta_wf(old(self).boundaries@)
            && crate::directory::proper(range.lo@,match range.hi { Some(h) => Some(h@), None => None }) ==>
                meta_wf(final(self).boundaries@)
                && intervals::update_frame(old(self).boundaries@,final(self).boundaries@,*range,generation,command,source),
    {
        let mut out = self.replace_middle(range,generation,command,source);
        if let Some(hi) = &range.hi { self.restore_suffix(&mut out,hi); }
        self.boundaries = out;
    }
}
} // verus!
