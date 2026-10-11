//! Observed scan/copy/seal effects, including explicit private-byte cleanup.
//! Streaming handlers expand into these primitives even when their final RPC fails.
use super::*;
use crate::participant::ControlResult;
use crate::types::{Certificate,Status};
verus! {
pub struct CaptureRecord {
    pub node:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,pub source:Image,
    pub key:int,pub writer:int,pub cursor:Option<crate::storage::Identity>,pub row:Option<crate::types::Row>,
}
impl CaptureRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let labels=ctx.window(self.keys); let v=s.placement;
        ctx.plan_observed(s,self.plan,self.keys) && transfer::local_coupling(&self.node,self.plan,v,labels)
            && self.node.capture_authorized(self.plan) && self.keys.to_set().contains(self.key)
            && s.registries[self.node.owner_view()]==self.node.lease_view()
            && s.images.contains_key(self.plan.source as int) && self.source==s.images[self.plan.source as int]
            && crate::storage::first(self.source,self.plan.range,self.cursor,self.row)
            && transfer::scan_covers(self.plan.range,self.cursor,self.row,labels[self.key])
            && transfer::observed(self.source,labels[self.key],self.writer)==p::replica(v,self.plan.source as int,self.key).cell
    }
    pub open spec fn image(&self,ctx:SourceContext,s:SourceState) -> SourceState {
        let packet=transfer::packet(self.plan.generation as nat,self.key,
            transfer::observed(self.source,ctx.window(self.keys)[self.key],self.writer));
        SourceState { placement:p::State { packets:s.placement.packets.insert(packet),..s.placement },..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires ctx.wf(c),p::inv(c,s.placement),s.registries_match(ctx),self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(ctx,s).placement,
    {
        ctx.drained(c,s,&self.node,self.plan,self.keys);
        captured(c,&self.node,self.plan,s.placement,self.keys,ctx.window(self.keys),self.source,
            self.key,self.writer,self.cursor,self.row)
    }
}
pub struct CopyRecord {
    pub node:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,pub key:int,
    pub before:Image,pub after:Image,pub value:Option<Seq<u8>>,pub source_cell:p::Cell,
}
impl CopyRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let labels=ctx.window(self.keys); let v=s.placement; let owner=self.plan.destination as int;
        ctx.plan_observed(s,self.plan,self.keys) && transfer::local_coupling(&self.node,self.plan,v,labels)
            && self.node.transfer_authorized(self.plan,false) && self.keys.to_set().contains(self.key)
            && s.registries[self.node.owner_view()]==self.node.lease_view()
            && s.images.contains_key(owner) && self.before==s.images[owner]
            && v.physical.contains_key((owner,self.key))
            && v.packets.contains(transfer::packet(self.plan.generation as nat,self.key,self.source_cell))
            && self.after==match self.value { Some(bytes)=>self.before.insert(labels[self.key],bytes),None=>self.before.remove(labels[self.key]) }
            && self.source_cell.value==crate::sharding_bytes::value_option(self.value)
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        let owner=self.plan.destination as int; let v=s.placement;
        SourceState {
            placement:p::State { physical:v.physical.insert((owner,self.key),p::Replica {
                cell:self.source_cell,covered:true,..v.physical[(owner,self.key)] }),..v },
            images:s.images.insert(owner,self.after),..s
        }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        let writers=Map::new(self.keys.to_set(),|k:int| p::replica(s.placement,self.plan.destination as int,k).cell.writer);
        copied(c,&self.node,self.plan,s.placement,self.keys,ctx.window(self.keys),self.key,self.before,self.after,
            self.value,writers,writers.insert(self.key,self.source_cell.writer),self.source_cell)
    }
}
pub struct SealRecord {
    pub before:Participant,pub after:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,
    pub current:Image,pub initial:Image,pub source:Image,pub writers:Map<int,int>,pub result:ControlResult,
}
impl SealRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let labels=ctx.window(self.keys); let v=s.placement;
        ctx.plan_observed(s,self.plan,self.keys) && transfer::local_coupling(&self.before,self.plan,v,labels)
            && s.registries[self.before.owner_view()]==self.before.lease_view()
            && self.after.lease_view()==self.before.lease_view()
            && self.before.transfer_authorized(self.plan,false) && self.after.seal_frame(self.before,self.plan)
            && self.after.owner_view()==self.before.owner_view()
            && s.images.contains_key(self.plan.destination as int) && self.current==s.images[self.plan.destination as int]
            && crate::storage::complete(self.current,self.initial,self.source,self.plan.range)
            && (forall|k:int| self.keys.to_set().contains(k) ==> v.physical.contains_key((self.plan.destination as int,k))
                && v.physical[(self.plan.destination as int,k)].covered
                && v.physical[(self.plan.destination as int,k)].cell==transfer::observed(self.current,labels[k],self.writers[k]))
            && self.result.status==Status::Ok && self.result.certificate==Some(Certificate::Ready)
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { placement:p::State {
            certificates:s.placement.certificates.insert((self.plan.generation as nat,p::Certificate::Ready)),
            ..ready_state(s.placement,self.plan.destination as int,self.keys)
        },emitted:s.emitted.insert((self.plan.generation,self.after.owner_view(),Certificate::Ready)),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        sealed(c,&self.before,&self.after,self.plan,s.placement,self.keys,ctx.window(self.keys),
            self.current,self.initial,self.source,self.writers)
    }
}

pub struct DeleteRecord {
    pub node:Participant,pub plan:MigrationPlan,pub cell:Cell,pub before:Image,pub after:Image,
}
impl DeleteRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let owner=self.node.owner_view() as int;
        s.plans.contains_key(self.plan.generation) && s.plans[self.plan.generation]==self.plan
            && self.node.transfer_authorized(self.plan,true)
            && crate::storage::in_range(self.plan.range,self.cell)
            && self.node.local_meta(self.cell.0,self.cell.1.0).is_some()
            && (forall|key:int| ctx.labels.contains_key(key) && ctx.labels[key]==self.cell ==>
                crate::participant_proofs::metadata(self.node.local_meta(self.cell.0,self.cell.1.0).unwrap(),
                    p::replica(s.placement,owner,key)))
            && s.registries[self.node.owner_view()]==self.node.lease_view()
            && s.images.contains_key(owner) && self.before==s.images[owner] && self.after==self.before.remove(self.cell)
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { images:s.images.insert(self.node.owner_view() as int,self.after),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        if exists|key:int| ctx.labels.contains_key(key) && ctx.labels[key]==self.cell {
            let key=choose|key:int| ctx.labels.contains_key(key) && ctx.labels[key]==self.cell;
            cleaned(c,s.placement,&self.node,self.plan,key,self.cell,self.before,self.after)
        } else {
            // Unmodelled private bytes still change the actual image timeline.
            // Their deletion cannot change any represented physical cell.
            assert forall|key:int| ctx.labels.contains_key(key) implies
                self.before.contains_key(ctx.labels[key])==self.after.contains_key(ctx.labels[key])
                    && (self.before.contains_key(ctx.labels[key]) ==>
                        self.before[ctx.labels[key]]==self.after[ctx.labels[key]]) by {
                assert(ctx.labels[key]!=self.cell);
            }
            empty_segment(c,s.placement)
        }
    }
}

/// A raw mirror also visits cells outside the finite logical observation domain.
/// Keep their authentic scan observations rather than dropping their bytes from
/// the engine-image timeline or extending the logical key universe by fiat.
pub type PrivatePacket = (u64,Cell,Option<Seq<u8>>);
pub struct PrivateCaptureRecord {
    pub node:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,pub cell:Cell,pub source:Image,
    pub cursor:Option<crate::storage::Identity>,pub row:Option<crate::types::Row>,
}
impl PrivateCaptureRecord {
    pub open spec fn packet(&self) -> PrivatePacket {
        (self.plan.generation,self.cell,
            if self.source.contains_key(self.cell) { Some(self.source[self.cell]) } else { None })
    }
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        ctx.plan_observed(s,self.plan,self.keys)
            && transfer::local_coupling(&self.node,self.plan,s.placement,ctx.window(self.keys))
            && self.node.capture_authorized(self.plan)
            && s.registries[self.node.owner_view()]==self.node.lease_view()
            && (forall|key:int| ctx.labels.contains_key(key) ==> ctx.labels[key]!=self.cell)
            && s.images.contains_key(self.plan.source as int) && self.source==s.images[self.plan.source as int]
            && crate::storage::in_range(self.plan.range,self.cell)
            && crate::storage::first(self.source,self.plan.range,self.cursor,self.row)
            && transfer::scan_covers(self.plan.range,self.cursor,self.row,self.cell)
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { private_packets:s.private_packets.insert(self.packet()),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires ctx.wf(c),p::inv(c,s.placement),s.registries_match(ctx),self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        ctx.drained(c,s,&self.node,self.plan,self.keys);
        empty_segment(c,s.placement)
    }
}
pub struct PrivateCopyRecord {
    pub node:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,pub packet:PrivatePacket,
    pub before:Image,pub after:Image,
}
impl PrivateCopyRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        ctx.plan_observed(s,self.plan,self.keys)
            && transfer::local_coupling(&self.node,self.plan,s.placement,ctx.window(self.keys))
            && self.node.transfer_authorized(self.plan,false)
            && s.registries[self.node.owner_view()]==self.node.lease_view()
            && s.private_packets.contains(self.packet) && self.packet.0==self.plan.generation
            && (forall|key:int| ctx.labels.contains_key(key) ==> ctx.labels[key]!=self.packet.1)
            && crate::storage::in_range(self.plan.range,self.packet.1)
            && s.images.contains_key(self.plan.destination as int) && self.before==s.images[self.plan.destination as int]
            && self.after==match self.packet.2 {
                Some(bytes)=>self.before.insert(self.packet.1,bytes),None=>self.before.remove(self.packet.1),
            }
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { images:s.images.insert(self.plan.destination as int,self.after),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        assert forall|key:int| ctx.labels.contains_key(key) implies
            self.before.contains_key(ctx.labels[key])==self.after.contains_key(ctx.labels[key])
                && (self.before.contains_key(ctx.labels[key]) ==>
                    self.before[ctx.labels[key]]==self.after[ctx.labels[key]]) by {
            assert(ctx.labels[key]!=self.packet.1);
        }
        empty_segment(c,s.placement)
    }
}

/// A complete cleanup result follows all actual delete primitives. Its retained
/// capability and empty-image proof are needed even for a successful duplicate.
pub struct CleanupReceipt {
    pub before:Participant,pub after:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,
    pub initial:Image,pub current:Image,pub result:ControlResult,
}
impl CleanupReceipt {
    pub open spec fn certificate(&self) -> Certificate {
        if self.before.owner_view()==self.plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }
    }
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        ctx.plan_observed(s,self.plan,self.keys)
            && transfer::local_coupling(&self.before,self.plan,s.placement,ctx.window(self.keys))
            && self.before.transfer_authorized(self.plan,true) && self.after.local_unchanged(self.before)
            && self.after.cleanup_done(self.plan.generation)
            && self.after.lease_view()==self.before.lease_view()
            && s.registries[self.before.owner_view()]==self.before.lease_view()
            && self.result.status==Status::Ok && self.result.certificate==Some(self.certificate())
            && s.images.contains_key(self.before.owner_view() as int) && self.current==s.images[self.before.owner_view() as int]
            && crate::storage::complete(self.current,self.initial,Map::empty(),self.plan.range)
            && s.placement.certificates.contains((self.plan.generation as nat,master::certificate(self.certificate())))
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { emitted:s.emitted.insert((self.plan.generation,self.after.owner_view(),self.certificate())),..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    {
        let tracked (effect,emitted)=cleanup_emitted(c,&self.before,&self.after,self.plan,self.result,
            s.placement,self.keys,ctx.window(self.keys),self.initial,self.current);
        effect
    }
}

/// This is a failed individual storage/transfer call with an actual unchanged
/// image, not permission to collapse a handler's earlier metadata/copy prefix.
pub struct UnchangedTransfer {
    pub before:Participant,pub after:Participant,pub plan:MigrationPlan,pub keys:Seq<int>,
    pub initial:Image,pub current:Image,pub result:ControlResult,
}
impl UnchangedTransfer {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        self.result.status!=Status::Ok && self.result.certificate.is_none() && self.current==self.initial
            && self.after.same_metadata(self.before) && self.after.local_unchanged(self.before)
            && self.after.lease_view()==self.before.lease_view()
            && s.registries[self.before.owner_view()]==self.before.lease_view()
            && s.images.contains_key(self.before.owner_view() as int) && self.initial==s.images[self.before.owner_view() as int]
            && transfer::local_coupling(&self.before,self.plan,s.placement,ctx.window(self.keys))
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==s.placement,
    {
        transfer_unchanged(c,&self.before,&self.after,self.plan,s.placement,ctx.window(self.keys),
            self.initial,self.current,self.result)
    }
}
} // verus!
