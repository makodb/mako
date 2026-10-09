//! Metadata prefixes and output receipts are separate source events.
//! In particular a failed outer cleanup RPC cannot undo a successful prefix.
use super::*;
use crate::participant::{ControlResult,Cleanup};
use crate::types::{Command,Certificate,Status};
verus! {
pub struct MetadataRecord {
    pub before:Participant,pub after:Participant,pub plan:MigrationPlan,pub command:Command,
    pub result:ControlResult,pub keys:Seq<int>,
}
impl MetadataRecord {
    pub open spec fn fresh(&self) -> bool {
        self.result.status==Status::Ok && !self.before.command_seen(self.plan.generation,self.command)
    }
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let b=self.before; let z=self.after; let plan=self.plan; let command=self.command;
        let v=s.placement; let labels=ctx.window(self.keys);
        ctx.owners.contains(b.owner_view()) && s.registries[b.owner_view()]==b.lease_view()
            && z.owner_view()==b.owner_view() && z.lease_view()==b.lease_view()
            && (self.result.status==Status::Ok ==> z.has_receipt(plan.generation))
            && if !self.fresh() { z.same_metadata(b) && z.local_unchanged(b) } else {
                ctx.plan_observed(s,plan,self.keys) && transfer::local_coupling(&b,plan,v,labels)
                    && z.control_frame(b,plan,command)
                    && (forall|coordinate:Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,coordinate) ==>
                        b.command_at(plan,command,b.drained_view(plan.range),coordinate))
                    && match command {
                        Command::Start | Command::Final => b.owner_view()==plan.destination,
                        Command::Freeze | Command::Retire => b.owner_view()==plan.source,
                        Command::Commit | Command::Abort => b.owner_view()==plan.source || b.owner_view()==plan.destination,
                    }
                    && (command==Command::Retire ==> b.capture_authorized(plan))
                    && v.commands.contains((plan.generation as nat,crate::participant_proofs::command(command)))
                    && (forall|k:int| self.keys.to_set().contains(k) ==>
                        v.physical.contains_key((b.owner_view() as int,k))
                            && v.plans[plan.generation as nat].old[k].epoch==crate::directory::route(plan.old@,labels[k].1.0).epoch)
            }
    }
    pub open spec fn image(&self,ctx:SourceContext,s:SourceState) -> SourceState {
        SourceState { placement:if self.fresh() {
            metadata_image(s.placement,&self.after,self.plan,self.command,self.keys,ctx.window(self.keys))
        } else { s.placement },..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires ctx.wf(c),p::inv(c,s.placement),s.registries_match(ctx),self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(ctx,s).placement,
    {
        if self.fresh() {
            if self.command==Command::Retire {
                ctx.drained(c,s,&self.before,self.plan,self.keys);
            }
            metadata_delivered(c,&self.before,&self.after,self.plan,self.command,&self.result,
                s.placement,self.keys,ctx.window(self.keys))
        } else {
            // This event is only the metadata call, whose actual fields frame.
            // It is never used to erase an enclosing transfer's byte effects.
            empty_segment(c,s.placement)
        }
    }
}

/// Non-cleanup returns can be produced again after their first metadata prefix.
/// Cleanup-required returns must instead pass CleanupRecord's byte/capability check.
pub struct ImmediateReceipt {
    pub node:Participant,pub plan:MigrationPlan,pub command:Command,pub result:ControlResult,
}
impl ImmediateReceipt {
    pub open spec fn certificate(&self) -> Certificate {
        if self.command==Command::Retire { Certificate::Retired }
        else if self.node.owner_view()==self.plan.source { Certificate::SourceDone }
        else { Certificate::DestinationDone }
    }
    pub open spec fn observed(&self,s:SourceState) -> bool {
        let plan=self.plan; let owner=self.node.owner_view(); let v=s.placement;
        s.plans.contains_key(plan.generation) && s.plans[plan.generation]==plan
            && v.plans.contains_key(plan.generation as nat)
            && v.plans[plan.generation as nat].src==plan.source
            && v.plans[plan.generation as nat].dst==plan.destination && plan.source!=plan.destination
            && self.result.status==Status::Ok && self.result.cleanup==Cleanup::None
            && self.result.certificate==Some(self.certificate())
            && v.certificates.contains((plan.generation as nat,master::certificate(self.certificate())))
            && if self.command==Command::Retire { owner==plan.source }
                else { self.command==Command::Commit && owner==plan.destination
                    || self.command==Command::Abort && owner==plan.source }
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { emitted:s.emitted.insert((self.plan.generation,self.node.owner_view(),self.certificate())),..s }
    }
    pub proof fn certify(&self,c:p::Constants,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        if self.command==Command::Retire {
            let tracked emitted=retired_emitted(&self.node,self.plan,self.command,&self.result,s.placement);
        } else {
            let tracked emitted=noncleanup_terminal_emitted(&self.node,self.plan,self.command,&self.result,s.placement);
        }
        empty_segment(c,s.placement)
    }
}

pub struct DrainRecord {
    pub node:Participant,pub plan:MigrationPlan,pub result:ControlResult,pub keys:Seq<int>,
}
impl DrainRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        ctx.plan_observed(s,self.plan,self.keys)
            && transfer::local_coupling(&self.node,self.plan,s.placement,ctx.window(self.keys))
            && s.registries[self.node.owner_view()]==self.node.lease_view()
            && self.result.status==Status::Ok && self.result.cleanup==Cleanup::None
            && self.result.certificate==Some(Certificate::Drained) && self.node.capture_authorized(self.plan)
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState {
            placement:p::State { certificates:s.placement.certificates.insert((self.plan.generation as nat,p::Certificate::Drained)),..s.placement },
            emitted:s.emitted.insert((self.plan.generation,self.node.owner_view(),Certificate::Drained)),..s
        }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires ctx.wf(c),p::inv(c,s.placement),s.registries_match(ctx),self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        ctx.drained(c,s,&self.node,self.plan,self.keys);
        let tracked out=drained(c,&self.node,self.plan,&self.result,s.placement,self.keys,ctx.window(self.keys));
        let tracked emitted=drain_emitted(&self.node,self.plan,&self.result,out.after(s.placement));
        out
    }
}
} // verus!
