//! Actual owner-local registry effects compose without replacing other owners.
use super::*;
use crate::leases::{self as leases,SessionView};
use crate::types::{TxnId,Grant,Status};
verus! {
pub enum AdmissionCall {
    Point { table:u64,coordinate:Seq<u8> },
    Range { table:u64,lo:Seq<u8>,hi:Option<Seq<u8>> },
}
pub struct AdmissionRecord {
    pub before:Participant,pub after:Participant,pub id:TxnId,pub grant:Grant,
    pub status:Status,pub call:AdmissionCall,
}
impl AdmissionRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let b=self.before; let z=self.after; let id=self.id; let g=self.grant; let status=self.status;
        let a=ctx.addresses(); let v=s.placement; let owner=b.owner_view();
        b.wf() && z.wf() && registry_wf(b.lease_view()) && registry_wf(z.lease_view())
            && z.owner_view()==owner && s.registries.contains_key(owner) && s.registries[owner]==b.lease_view()
            && z.same_metadata(b)
            && session_observed(b.lease_view(),v,id,a,owner)
            && (status!=Status::Ok ==> z.lease_view()==b.lease_view())
            && (status==Status::Ok ==> g.owner==owner)
            && (z.lease_view()!=b.lease_view() ==> !v.sessions[transaction(id)].resolved)
            && (forall|k:int| a.contains_key(k) ==> v.physical.contains_key((owner as int,k))
                && b.local_meta(a[k].0,a[k].1).is_some()
                && crate::participant_proofs::metadata(b.local_meta(a[k].0,a[k].1).unwrap(),p::replica(v,owner as int,k)))
            && match self.call {
                AdmissionCall::Point { table,coordinate } => {
                    (status==Status::Ok ==> z.lease_view()==b.lease_view()
                        || leases::acquire_effect(b.lease_view(),z.lease_view(),id,table,coordinate,g,status))
                        && (z.lease_view()!=b.lease_view() ==> !b.open_hold(id,table,coordinate,g))
                        && (status==Status::Ok && !b.open_hold(id,table,coordinate,g) ==>
                            b.local_meta(table,coordinate).is_some()
                                && b.local_meta(table,coordinate).unwrap().role is Serving
                                && b.local_meta(table,coordinate).unwrap().epoch==g.epoch)
                },
                AdmissionCall::Range { table,lo,hi } => {
                    (status==Status::Ok ==> z.lease_view()==b.lease_view()
                        || leases::acquire_range_effect(b.lease_view(),z.lease_view(),id,table,lo,hi,g,status))
                        && (z.lease_view()!=b.lease_view() ==> !b.open_range(id,table,lo,hi,g))
                        && (status==Status::Ok && !b.open_range(id,table,lo,hi,g) ==>
                            forall|coordinate:Seq<u8>| crate::directory::inside(coordinate,lo,hi) ==>
                                b.local_meta(table,coordinate).is_some()
                                    && b.local_meta(table,coordinate).unwrap().role is Serving
                                    && b.local_meta(table,coordinate).unwrap().epoch==g.epoch)
                },
            }
    }
    pub open spec fn image(&self,ctx:SourceContext,s:SourceState) -> SourceState {
        let placement=if self.after.lease_view()==self.before.lease_view() { s.placement }
            else { registration_state(s.placement,self.after.lease_view(),self.id,ctx.addresses()) };
        SourceState { placement,registries:s.registries.insert(self.before.owner_view(),self.after.lease_view()),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires p::inv(c,s.placement),s.registries_match(ctx),self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(ctx,s).placement,
            self.image(ctx,s).registries_match(ctx),
    {
        let tracked out=match self.call {
            AdmissionCall::Point { table,coordinate } => native_point_admission(c,s.placement,&self.before,&self.after,
                self.id,table,coordinate,self.grant,self.status,ctx.addresses()),
            AdmissionCall::Range { table,lo,hi } => native_range_admission(c,s.placement,&self.before,&self.after,
                self.id,table,lo,hi,self.grant,self.status,ctx.addresses()),
        };
        let next=self.image(ctx,s);
        assert(next.registries.dom() =~= s.registries.dom());
        let owner=self.before.owner_view();
        let before=self.before.lease_view();
        let after=self.after.lease_view();
        if before==after {
            assert(next.registries =~= s.registries);
            registries_frame(s.registries,s.placement,next.placement,ctx.addresses());
        } else {
            assert(after =~= before.insert(self.id.client,after[self.id.client]));
            assert forall|k:int| next.placement.sessions[transaction(self.id)].held.contains_key(k)
                && next.placement.sessions[transaction(self.id)].held[k].owner!=owner as int implies
                s.placement.sessions.contains_key(transaction(self.id))
                    && s.placement.sessions[transaction(self.id)].held.contains_key(k)
                    && s.placement.sessions[transaction(self.id)].held[k]==next.placement.sessions[transaction(self.id)].held[k] by {
                if held_image(after[self.id.client],ctx.addresses()).contains_key(k) {
                    assert(owned_holds(next.placement.sessions[transaction(self.id)].held,owner).contains_key(k));
                }
            }
            registry_replaced(s.registries,s.placement,next.placement,ctx.addresses(),owner,self.id,after);
        }
        out
    }
}
pub enum RegistryCall { Begin,Finish }
pub struct RegistryRecord {
    pub before:Participant,pub after:Participant,
    pub id:TxnId,pub status:Status,pub call:RegistryCall,
}
impl RegistryRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let b=self.before.lease_view(); let z=self.after.lease_view();
        let id=self.id; let status=self.status; let v=s.placement; let owner=self.before.owner_view();
        self.after.same_metadata(self.before) && self.after.owner_view()==owner
            && s.registries.contains_key(owner) && s.registries[owner]==b && match self.call {
            RegistryCall::Begin => leases::begin_effect(b,z,id,status)
                && (b!=z && v.sessions.contains_key(transaction(id)) ==> !v.sessions[transaction(id)].resolved
                    && owned_holds(v.sessions[transaction(id)].held,owner).dom()==Set::<int>::empty())
                && (b==z && status==Status::Ok ==> session_observed(b,v,id,ctx.addresses(),owner)),
            RegistryCall::Finish => leases::finish_effect(b,z,id,status)
                && session_observed(b,v,id,ctx.addresses(),owner)
                && s.completed.contains(id) && v.sessions[transaction(id)].resolved,
        }
    }
    pub open spec fn image(&self,ctx:SourceContext,s:SourceState) -> SourceState {
        let v=s.placement; let t=transaction(self.id);
        let placement=match self.call {
            RegistryCall::Begin => if self.status==Status::Ok && !v.sessions.contains_key(t) {
                p::State { sessions:v.sessions.insert(t,p::Session { held:Map::empty(),resolved:false }),..v }
            } else { v },
            RegistryCall::Finish => if self.status==Status::Ok {
                p::State { sessions:v.sessions.insert(t,p::Session {
                    held:v.sessions[t].held.remove_keys(held_image(self.before.lease_view()[self.id.client],ctx.addresses()).dom()),
                    resolved:true }),..v }
            } else { v },
        };
        SourceState { placement,registries:s.registries.insert(self.before.owner_view(),self.after.lease_view()),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires s.registries_match(ctx),self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(ctx,s).placement,
            self.image(ctx,s).registries_match(ctx),
    {
        let before=self.before.lease_view(); let after=self.after.lease_view(); let owner=self.before.owner_view();
        let tracked out=match self.call {
            RegistryCall::Begin => native_begin(c,s.placement,before,after,self.id,self.status,ctx.addresses(),owner),
            RegistryCall::Finish => {
                // Only a known callback for an earlier actual settlement inserts
                // this identity into the fold's completion ledger.
                let tracked completion=EngineCompletion { id:self.id };
                reveal(EngineCompletion::matches);
                native_finish(c,s.placement,before,after,self.id,self.status,ctx.addresses(),owner,&completion)
            },
        };
        let next=self.image(ctx,s);
        assert(next.registries.dom() =~= s.registries.dom());
        if before==after {
            assert(next.registries =~= s.registries);
            assert(next.placement.sessions.dom() =~= s.placement.sessions.dom());
            if self.call is Finish {
                assert(held_image(before[self.id.client],ctx.addresses()) =~= Map::<int,p::Grant>::empty());
                assert(next.placement.sessions[transaction(self.id)].held =~= s.placement.sessions[transaction(self.id)].held);
            }
            assert forall|t:int| s.placement.sessions.contains_key(t) implies
                next.placement.sessions[t].held==s.placement.sessions[t].held by {}
            registries_frame(s.registries,s.placement,next.placement,ctx.addresses());
        } else {
            let t=transaction(self.id);
            assert(self.status==Status::Ok);
            assert(next.placement.sessions.dom() =~= s.placement.sessions.dom().insert(t));
            if before.contains_key(self.id.client) && before[self.id.client].sequence!=self.id.sequence {
                assert(before[self.id.client].holds.len()==0);
                assert(held_image(before[self.id.client],ctx.addresses()) =~= Map::<int,p::Grant>::empty());
            }
            assert forall|other:u32| other!=owner implies owned_holds(next.placement.sessions[t].held,other)==
                if s.placement.sessions.contains_key(t) { owned_holds(s.placement.sessions[t].held,other) }
                else { Map::<int,p::Grant>::empty() } by {
                assert(owned_holds(next.placement.sessions[t].held,other) =~=
                    if s.placement.sessions.contains_key(t) { owned_holds(s.placement.sessions[t].held,other) }
                    else { Map::<int,p::Grant>::empty() });
            }
            registry_replaced(s.registries,s.placement,next.placement,ctx.addresses(),owner,self.id,after);
        }
        out
    }
}
} // verus!
