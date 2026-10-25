//! Native admission is an immutable observation protected by the caller's STO
//! epoch, not a durable lease. Publication copies all authoritative metadata;
//! point/range reads never register or consult the mutable participant again.
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use crate::participant::{Participant, MetaTable};
#[cfg(verus_keep_ghost)]
use crate::participant::{meta_route, meta_wf};
use crate::routing::Snapshot;
use crate::types::*;

verus! {
pub struct AdmissionSnapshot {
    owner: u32,
    tables: HashMap<u64,MetaTable>,
    routes: Arc<Snapshot>,
}
impl AdmissionSnapshot {
    pub closed spec fn wf(&self) -> bool {
        forall|table:u64|self.tables@.contains_key(table) ==> meta_wf(self.tables@[table].boundaries@)
    }
    pub closed spec fn local_meta(&self, table:u64, key:Seq<u8>) -> Option<ReplicaMeta> {
        if self.tables@.contains_key(table) {meta_route(self.tables@[table].boundaries@,key)} else {None}
    }
    pub closed spec fn owner_view(&self) -> u32 {self.owner}
    pub closed spec fn routing_view(&self) -> Snapshot {*self.routes}
    pub open spec fn authorizes(&self, table:u64, key:Seq<u8>, grant:Grant) -> bool {
        grant.owner == self.owner_view() && self.local_meta(table,key).is_some()
            && self.local_meta(table,key).unwrap().role is Serving
            && self.local_meta(table,key).unwrap().ownership_version == grant.ownership_version
    }
    /// Executable publication with exact all-coordinate equality, including None.
    /// Routing labels are deliberately not a premise of metadata authority.
    pub fn capture(participant:&Participant, routes:Arc<Snapshot>) -> (out:Self)
        requires participant.wf(),
        ensures out.wf(), out.owner_view()==participant.owner_view(), out.routing_view()==*routes,
            forall|table:u64,key:Seq<u8>|out.local_meta(table,key)==participant.local_meta(table,key),
    {
        Self {owner:participant.owner(), tables:participant.admission_tables(), routes}
    }
    pub fn routing(&self) -> (out:&Snapshot)
        ensures *out==self.routing_view(),
    { &self.routes }
    pub fn route(&self, table:u64, key:&[u8]) -> (out:Result<RoutedGrant,Status>)
        requires exists|nodes:Seq<u32>|crate::routing::snapshot_valid(self.routing_view(),nodes),
        ensures match out {Ok(r)=>crate::routing::captured(self.routing_view(),table,key@,r),Err(_)=>true},
    {
        match self.routes.capture(table,key) {Some(route)=>Ok(route),None=>Err(Status::NotFound)}
    }
    /// Incoming labels, including a forged matching snapshot number, convey no
    /// provenance. Exact captured metadata decides every request instead.
    pub fn point(&self, table:u64, key:&[u8], route:RoutedGrant) -> (out:Status)
        requires self.wf(),
        ensures (out==Status::Ok) == self.authorizes(table,key@,route.grant),
    {
        if route.grant.owner != self.owner {return Status::Retry;}
        let table = match self.tables.get(&table) {Some(table)=>table,None=>return Status::Retry};
        match table.lookup(key) {
            Some(meta) if matches!(meta.role,Role::Serving) && meta.ownership_version==route.grant.ownership_version => Status::Ok,
            _ => Status::Retry,
        }
    }
    pub fn range(&self, table:u64, lo:&[u8], hi:Option<&[u8]>, route:RoutedGrant) -> (out:Status)
        requires self.wf(),
        ensures out==Status::Ok ==> crate::directory::proper(lo@,crate::directory::hi_view(hi))
            && forall|key:Seq<u8>|crate::directory::inside(key,lo@,crate::directory::hi_view(hi)) ==>
                self.authorizes(table,key,route.grant),
    {
        if let Some(hi)=hi {if crate::bytes::compare(lo,hi)>=0 {return Status::Invalid;}}
        if route.grant.owner != self.owner {return Status::Retry;}
        let metadata = match self.tables.get(&table) {Some(metadata)=>metadata,None=>return Status::Retry};
        if metadata.serving_range(lo,hi,route.grant.ownership_version) {Status::Ok} else {Status::Retry}
    }
    /// Capturing Frozen cannot authorize any coordinate of that frozen extent.
    pub proof fn frozen_rejects(&self, table:u64, key:Seq<u8>, grant:Grant)
        requires self.local_meta(table,key).is_some(), self.local_meta(table,key).unwrap().role is Frozen,
        ensures !self.authorizes(table,key,grant),
    {}
    /// Different immutable captures have independent authority even if their
    /// routing labels are equal. Actual engine drain is a separate host premise.
    pub proof fn freeze_separates_captures(&self, fresh:Self, table:u64, key:Seq<u8>, grant:Grant)
        requires self.local_meta(table,key).is_some(),
            self.local_meta(table,key).unwrap().role is Serving,
            self.local_meta(table,key).unwrap().ownership_version==grant.ownership_version,
            self.owner_view()==grant.owner,
            fresh.local_meta(table,key).is_some(), fresh.local_meta(table,key).unwrap().role is Frozen,
        ensures self.authorizes(table,key,grant), !fresh.authorizes(table,key,grant),
    {}
}
} // verus!

#[cfg(all(test, not(verus_keep_ghost)))]
#[path = "epoch_admission_tests.rs"]
mod tests;
