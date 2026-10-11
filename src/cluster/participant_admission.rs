//! Derived, non-durable snapshot certification. Only this update-time validator
//! visits snapshot ranges; matching captured admissions never look up metadata.
use super::*;
use crate::routing::Snapshot;
#[cfg(verus_keep_ghost)]
use crate::routing::{snapshot_lookup, captured, captured_range};
#[cfg(verus_keep_ghost)]
use crate::directory::{inside, hi_view, wellformed};

verus! {
impl Participant {
    pub open spec fn snapshot_authority(&self, s: Snapshot) -> bool {
        forall|table:u64,key:Seq<u8>| snapshot_lookup(s,table,key).is_some()
            && snapshot_lookup(s,table,key).unwrap().owner == self.owner_view() ==>
                self.local_meta(table,key).is_some()
                && self.local_meta(table,key).unwrap().role == Role::Serving
                && self.local_meta(table,key).unwrap().ownership_version
                    == snapshot_lookup(s,table,key).unwrap().ownership_version
    }
    pub proof fn certified_authority(&self)
        requires self.wf(),
        ensures self.certified_snapshot().is_some() ==>
            self.snapshot_authority(self.certified_snapshot().unwrap())
                && self.certified_snapshot().unwrap().version != NO_SNAPSHOT,
    {}
    pub fn fast_snapshot_version(&self) -> (out: Option<u64>)
        requires self.wf(),
        ensures out == match self.certified_snapshot() { Some(s) => Some(s.version), None => None },
    { self.fast_version }

    /// Must precede every authority mutation, including replayed controls.
    /// Receipts, leases and local metadata are deliberately untouched.
    pub fn invalidate_snapshot(&mut self)
        ensures final(self).certified_snapshot() == None,
            final(self).certification_wf(), final(self).same_metadata(*old(self)),
            final(self).lease_view() == old(self).lease_view(),
            old(self).wf() ==> final(self).wf(),
    {
        self.fast_version = None;
        self.certified = Ghost(None);
    }

    /// Validate each assigned half-open snapshot interval against the local
    /// metadata, once under the same mutex used by controls and admissions.
    pub fn certify_snapshot(&mut self, snapshot: &Snapshot) -> (yes: bool)
        requires old(self).wf(),
            forall|i:int| 0 <= i < snapshot.tables.len() ==>
                wellformed(snapshot.tables@[i].boundaries@),
        ensures final(self).wf(), final(self).same_metadata(*old(self)),
            final(self).lease_view() == old(self).lease_view(),
            yes ==> final(self).certified_snapshot() == Some(*snapshot)
                && final(self).snapshot_authority(*snapshot),
            !yes ==> final(self).certified_snapshot() == None,
    {
        self.invalidate_snapshot();
        if snapshot.version == NO_SNAPSHOT { return false; }
        let mut t = 0usize;
        while t < snapshot.tables.len()
            invariant self.wf(), self.certified_snapshot() == None,
                self.same_metadata(*old(self)), self.lease_view() == old(self).lease_view(),
                snapshot.version != NO_SNAPSHOT, t <= snapshot.tables.len(),
                forall|i:int| 0 <= i < snapshot.tables.len() ==>
                    wellformed(snapshot.tables@[i].boundaries@),
                forall|i:int,key:Seq<u8>| 0 <= i < t &&
                    crate::directory::route(snapshot.tables@[i].boundaries@,key).owner == self.owner_view() ==>
                        self.local_meta(snapshot.tables@[i].table,key).is_some()
                        && self.local_meta(snapshot.tables@[i].table,key).unwrap().role == Role::Serving
                        && self.local_meta(snapshot.tables@[i].table,key).unwrap().ownership_version
                            == crate::directory::route(snapshot.tables@[i].boundaries@,key).ownership_version,
            decreases snapshot.tables.len()-t,
        {
            let table = &snapshot.tables[t];
            let mut i = 0usize;
            while i < table.boundaries.len()
                invariant self.wf(), i <= table.boundaries.len(),
                    self.certified_snapshot() == None,
                    self.same_metadata(*old(self)), self.lease_view() == old(self).lease_view(),
                    forall|n:int,key:Seq<u8>| 0 <= n < t &&
                        crate::directory::route(snapshot.tables@[n].boundaries@,key).owner == self.owner_view() ==>
                            self.local_meta(snapshot.tables@[n].table,key).is_some()
                            && self.local_meta(snapshot.tables@[n].table,key).unwrap().role == Role::Serving
                            && self.local_meta(snapshot.tables@[n].table,key).unwrap().ownership_version
                                == crate::directory::route(snapshot.tables@[n].boundaries@,key).ownership_version,
                    wellformed(table.boundaries@),
                    forall|j:int,key:Seq<u8>| 0 <= j < i && table.boundaries@[j].grant.owner == self.owner_view()
                        && inside(key,table.boundaries@[j].start@,
                            if j+1 < table.boundaries.len() { Some(table.boundaries@[j+1].start@) } else { None }) ==>
                        self.local_meta(table.table,key).is_some()
                        && self.local_meta(table.table,key).unwrap().role == Role::Serving
                        && self.local_meta(table.table,key).unwrap().ownership_version == table.boundaries@[j].grant.ownership_version,
                decreases table.boundaries.len()-i,
            {
                let boundary = &table.boundaries[i];
                if boundary.grant.owner == self.owner {
                    let local = match self.tables.get(&table.table) { Some(local) => local, None => return false };
                    let hi: Option<&[u8]> = if i+1 < table.boundaries.len() {
                        Some(table.boundaries[i+1].start.as_slice())
                    } else { None };
                    if !local.serving_range(&boundary.start,hi,boundary.grant.ownership_version) { return false; }
                }
                i += 1;
            }
            proof {
                assert forall|key:Seq<u8>| crate::directory::route(table.boundaries@,key).owner == self.owner_view()
                    implies self.local_meta(table.table,key).is_some()
                        && self.local_meta(table.table,key).unwrap().role == Role::Serving
                        && self.local_meta(table.table,key).unwrap().ownership_version
                            == crate::directory::route(table.boundaries@,key).ownership_version by {
                    let j = crate::directory_proofs::selected(table.boundaries@,key);
                    crate::directory_proofs::selected_route(table.boundaries@,j,key);
                    if j+1 < table.boundaries.len() {
                        crate::bytes::cmp_laws(table.boundaries@[j+1].start@,key);
                    }
                    assert(inside(key,table.boundaries@[j].start@,
                        if j+1 < table.boundaries.len() { Some(table.boundaries@[j+1].start@) } else { None }));
                }
            }
            t += 1;
        }
        proof {
            assert forall|table:u64,key:Seq<u8>| snapshot_lookup(*snapshot,table,key).is_some()
                && snapshot_lookup(*snapshot,table,key).unwrap().owner == self.owner_view()
                implies self.local_meta(table,key).is_some()
                    && self.local_meta(table,key).unwrap().role == Role::Serving
                    && self.local_meta(table,key).unwrap().ownership_version
                        == snapshot_lookup(*snapshot,table,key).unwrap().ownership_version by {
                let i = choose|i:int| 0 <= i < snapshot.tables.len() && snapshot.tables@[i].table == table;
                assert(snapshot_lookup(*snapshot,table,key) == Some(crate::directory::route(snapshot.tables@[i].boundaries@,key)));
            }
        }
        self.certified = Ghost(Some(*snapshot));
        self.fast_version = Some(snapshot.version);
        true
    }

    /// A routing/publication premise, not an authority premise. Checked capture
    /// and immutable publication-history membership derive it. Foreign requests
    /// without that provenance must use NO_SNAPSHOT, never a relabeled version.
    pub open spec fn routed_point_provenance(&self, table:u64,key:Seq<u8>,r:RoutedGrant) -> bool {
        self.certified_snapshot().is_some() && r.snapshot_version == self.certified_snapshot().unwrap().version ==>
            captured(self.certified_snapshot().unwrap(),table,key,r)
    }
    pub open spec fn routed_range_provenance(&self, table:u64,lo:Seq<u8>,hi:Option<Seq<u8>>,r:RoutedGrant) -> bool {
        self.certified_snapshot().is_some() && r.snapshot_version == self.certified_snapshot().unwrap().version ==>
            captured_range(self.certified_snapshot().unwrap(),table,lo,hi,r)
    }

    pub fn acquire_routed(&mut self,id:TxnId,table:u64,coordinate:&[u8],r:RoutedGrant) -> (out:Status)
        requires old(self).wf(), old(self).routed_point_provenance(table,coordinate@,r),
        ensures final(self).wf(), final(self).same_metadata(*old(self)),
            old(self).point_acquire_effect(*final(self),id,table,coordinate@,r.grant,out),
            out == Status::Ok ==> final(self).open_hold(id,table,coordinate@,r.grant) && r.grant.owner == old(self).owner_view(),
            out == Status::Ok && !old(self).open_hold(id,table,coordinate@,r.grant) && old(self).table_wf(table) ==>
                old(self).local_meta(table,coordinate@).is_some()
                && old(self).local_meta(table,coordinate@).unwrap().role is Serving
                && old(self).local_meta(table,coordinate@).unwrap().ownership_version == r.grant.ownership_version,
            out != Status::Ok ==> final(self).lease_view() == old(self).lease_view(),
            out == Status::Ok ==> final(self).lease_view() == old(self).lease_view()
                || crate::leases::acquire_effect(old(self).lease_view(),final(self).lease_view(),id,table,coordinate@,r.grant,out),
            final(self).lease_view() != old(self).lease_view() ==> !old(self).open_hold(id,table,coordinate@,r.grant),
    {
        if self.fast_version != Some(r.snapshot_version) || r.snapshot_version == NO_SNAPSHOT {
            return self.acquire(id,table,coordinate,r.grant);
        }
        if r.grant.owner != self.owner { return Status::Retry; }
        if let Some(held) = self.leases.held(id,table,coordinate) {
            proof {
                self.lease_facts();
                assert(self.open_hold(id,table,coordinate@,held));
                assert(exists|g:Grant| self.open_hold(id,table,coordinate@,g));
                if self.open_hold(id,table,coordinate@,r.grant) {
                    crate::leases::point_grant_unique(self.lease_view()[id.client],table,coordinate@,held,r.grant);
                }
                assert((held == r.grant) == self.open_hold(id,table,coordinate@,r.grant));
            }
            return if held.owner == r.grant.owner && held.ownership_version == r.grant.ownership_version {
                Status::Ok
            } else { Status::Retry };
        }
        proof {
            self.certified_authority();
            assert(captured(self.certified_snapshot().unwrap(),table,coordinate@,r));
            assert(self.local_meta(table,coordinate@).is_some());
            assert(self.local_meta(table,coordinate@).unwrap().role == Role::Serving);
            assert(self.local_meta(table,coordinate@).unwrap().ownership_version == r.grant.ownership_version);
        }
        self.leases.acquire(id,table,coordinate,r.grant)
    }

    pub fn acquire_range_routed(&mut self,id:TxnId,table:u64,lo:&[u8],hi:Option<&[u8]>,r:RoutedGrant) -> (out:Status)
        requires old(self).wf(), old(self).routed_range_provenance(table,lo@,hi_view(hi),r),
        ensures final(self).wf(), final(self).same_metadata(*old(self)),
            old(self).range_acquire_effect(*final(self),id,table,lo@,hi_view(hi),r.grant,out),
            out == Status::Ok ==> final(self).open_range(id,table,lo@,hi_view(hi),r.grant)
                && r.grant.owner == old(self).owner_view(),
            out == Status::Ok && !old(self).open_range(id,table,lo@,hi_view(hi),r.grant) ==>
                forall|k:Seq<u8>| inside(k,lo@,hi_view(hi)) ==>
                    old(self).local_meta(table,k).is_some()
                    && old(self).local_meta(table,k).unwrap().role is Serving
                    && old(self).local_meta(table,k).unwrap().ownership_version == r.grant.ownership_version,
            out != Status::Ok ==> final(self).lease_view() == old(self).lease_view(),
            out == Status::Ok ==> final(self).lease_view() == old(self).lease_view()
                || crate::leases::acquire_range_effect(old(self).lease_view(),final(self).lease_view(),id,table,lo@,hi_view(hi),r.grant,out),
            final(self).lease_view() != old(self).lease_view() ==> !old(self).open_range(id,table,lo@,hi_view(hi),r.grant),
    {
        if self.fast_version != Some(r.snapshot_version) || r.snapshot_version == NO_SNAPSHOT {
            return self.acquire_range(id,table,lo,hi,r.grant);
        }
        if let Some(h) = hi { if compare(lo,h) >= 0 { return Status::Invalid; } }
        if r.grant.owner != self.owner { return Status::Retry; }
        if let Some(held) = self.leases.held_range(id,table,lo,hi) {
            proof {
                self.lease_facts();
                assert(self.open_range(id,table,lo@,hi_view(hi),held));
                assert(exists|g:Grant| self.open_range(id,table,lo@,hi_view(hi),g));
                if self.open_range(id,table,lo@,hi_view(hi),r.grant) {
                    crate::leases::range_grant_unique(self.lease_view()[id.client],table,lo@,hi_view(hi),held,r.grant);
                }
                assert((held == r.grant) == self.open_range(id,table,lo@,hi_view(hi),r.grant));
            }
            return if held.owner == r.grant.owner && held.ownership_version == r.grant.ownership_version {
                Status::Ok
            } else { Status::Retry };
        }
        proof {
            self.certified_authority();
            assert(captured_range(self.certified_snapshot().unwrap(),table,lo@,hi_view(hi),r));
            assert forall|key:Seq<u8>| inside(key,lo@,hi_view(hi)) implies
                self.local_meta(table,key).is_some()
                && self.local_meta(table,key).unwrap().role == Role::Serving
                && self.local_meta(table,key).unwrap().ownership_version == r.grant.ownership_version by {
                assert(snapshot_lookup(self.certified_snapshot().unwrap(),table,key) == Some(r.grant));
            }
            crate::bytes::cmp_laws(lo@,lo@);
            assert(inside(lo@,lo@,hi_view(hi)));
            assert(self.tables@.contains_key(table));
        }
        self.leases.acquire_range(id,table,lo,hi,r.grant)
    }
}
} // verus!
