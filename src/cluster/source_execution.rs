//! Construct placement histories from raw native source contracts, not an input
//! journal, supplied model actions, or pre-certified transition history.
//!
//! The embedding is immutable for the entire finite execution. Engine images
//! and owner-local registries form explicit source chains. Actual engine
//! settlement is separate from known callback delivery; generated participant
//! receipts are separate from semantic certificates and authenticated receipt.
//! Foreign canonical-handle/transport adapters remain named interfaces. This is
//! the live native envelope, not an implementation of the recovery extensions.
use super::*;
use crate::migration_refinement as master;
use crate::types::{TxnId,Certificate,Status};
#[path="source_control_history.rs"]
mod control_history;
pub use control_history::*;
#[path="source_engine_history.rs"]
mod engine_history;
pub use engine_history::*;
#[path="source_lease_history.rs"]
mod lease_history;
pub use lease_history::*;
#[path="source_metadata_history.rs"]
mod metadata_history;
pub use metadata_history::*;
#[path="source_transfer_history.rs"]
mod transfer_history;
pub use transfer_history::*;
#[path="source_causality.rs"]
mod causality;
pub use causality::*;
verus! {
pub struct SourceContext {
    pub labels:Map<int,Cell>,pub observations:master::Observations,pub owners:Set<u32>,
}
impl SourceContext {
    pub open spec fn wf(&self,c:p::Constants) -> bool {
        p::constants_ok(c) && self.labels.dom()==c.keys && self.observations.bytes.dom()==c.keys
            && (forall|k:int| c.keys.contains(k) ==> self.labels[k].0==c.table[k]
                && self.labels[k].1.0==self.observations.bytes[k])
            && (forall|a:int,b:int| self.labels.contains_key(a) && self.labels.contains_key(b)
                && self.labels[a]==self.labels[b] ==> a==b)
            && (forall|owner:u32| self.owners.contains(owner) ==> c.shards.contains(owner as int))
            && (forall|owner:int| c.shards.contains(owner) ==>
                exists|native:u32| self.owners.contains(native) && owner==native as int)
    }
    pub open spec fn addresses(&self) -> crate::leases_proofs::Addresses {
        Map::new(self.labels.dom(),|k:int| (self.labels[k].0,self.labels[k].1.0))
    }
    pub open spec fn window(&self,keys:Seq<int>) -> Map<int,Cell> {
        Map::new(keys.to_set(),|k:int| self.labels[k])
    }
    pub open spec fn plan_observed(&self,s:SourceState,plan:MigrationPlan,keys:Seq<int>) -> bool {
        s.plans.contains_key(plan.generation) && s.plans[plan.generation]==plan
            && keys.to_set().subset_of(self.labels.dom())
            && transfer::labels_cover(plan,s.placement,keys,self.window(keys))
            && (forall|k:int| self.labels.contains_key(k) ==>
                crate::storage::in_range(plan.range,self.labels[k])==keys.to_set().contains(k))
    }
    /// Local drain becomes global drain through the derived distributed registry
    /// invariant, never an assumed equality with the desired placement guard.
    pub proof fn drained(&self,c:p::Constants,s:SourceState,node:&Participant,plan:MigrationPlan,keys:Seq<int>)
        requires self.wf(c),p::inv(c,s.placement),s.registries_match(*self),
            self.plan_observed(s,plan,keys),transfer::local_coupling(node,plan,s.placement,self.window(keys)),
            node.capture_authorized(plan),s.registries[node.owner_view()]==node.lease_view(),
        ensures p::drained(s.placement,s.placement.plans[plan.generation as nat].keys),
    {
        let v=s.placement;
        let owner=node.owner_view();
        let a=self.addresses();
        node.drained_lease_view(plan.range);
        assert(p::plan_inv(c,v,plan.generation as nat));
        assert(c.shards.contains(owner as int));
        let configured=choose|n:u32| self.owners.contains(n) && n as int==owner as int;
        assert(configured==owner);
        assert(self.owners.contains(owner));
        assert forall|k:int| keys.to_set().contains(k) implies
            crate::bytes::contains_spec(plan.range,a[k].0,a[k].1) && p::directory(v)[k].owner==owner as int by {
            assert(c.keys.contains(k));
            assert(p::key_inv(c,v,k));
            assert(self.window(keys)[k]==self.labels[k]);
            assert(node.range_role(plan,crate::types::Role::Frozen,None,false));
            assert(p::replica(v,owner as int,k).role is Frozen);
        }
        global_drain_from_source(c,v,s.registries,a,plan.range,keys.to_set(),owner);
    }
}
pub struct SourceState {
    pub placement:p::State,pub images:EngineImages,pub registries:Registries,
    pub plans:Map<u64,MigrationPlan>,pub emitted:Set<(u64,u32,Certificate)>,
    pub settled:Map<TxnId,EngineOutcome>,pub completed:Set<TxnId>,
    pub private_packets:Set<PrivatePacket>,
    pub nodes:Map<u32,Participant>,
}
impl SourceState {
    pub open spec fn registries_match(&self,ctx:SourceContext) -> bool {
        self.registries.dom()==ctx.owners && registries_observed(self.registries,self.placement,ctx.addresses())
    }
    /// Loader leases are ghost scopes, not native registry entries. Activation
    /// begins with empty native registries and no fabricated response/outcome.
    pub open spec fn activated(ctx:SourceContext,placement:p::State,images:EngineImages,nodes:Map<u32,Participant>) -> Self {
        Self { placement,images,nodes,registries:Map::new(ctx.owners,|owner:u32| Map::empty()),
            plans:Map::empty(),emitted:Set::empty(),settled:Map::empty(),completed:Set::empty(),
            private_packets:Set::empty() }
    }
    pub open spec fn nodes_initial(ctx:SourceContext,nodes:Map<u32,Participant>) -> bool {
        nodes.dom()==ctx.owners && forall|owner:u32| nodes.contains_key(owner) ==>
            nodes[owner].wf() && nodes[owner].owner_view()==owner
                && nodes[owner].lease_view()==Map::<u64,crate::leases::SessionView>::empty()
                && forall|g:u64| !nodes[owner].has_receipt(g)
    }
}
pub struct CacheRecord {
    pub client:int,pub index:nat,pub before:Option<crate::routing::Snapshot>,
    pub after:Option<crate::routing::Snapshot>,pub incoming:crate::routing::Snapshot,pub status:Status,
}
impl CacheRecord {
    pub open spec fn tables(ctx:SourceContext) -> Map<int,u64> {
        Map::new(ctx.labels.dom(),|k:int| ctx.labels[k].0)
    }
    pub open spec fn coordinates(ctx:SourceContext) -> Map<int,Seq<u8>> {
        Map::new(ctx.labels.dom(),|k:int| ctx.labels[k].1.0)
    }
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        crate::routing_proofs::install_effect(self.before,self.after,self.incoming,self.status)
            && (self.before is Some ==> s.placement.views.contains_key(self.client)
                && crate::routing_proofs::observed(self.before.unwrap(),s.placement,s.placement.views[self.client],Self::tables(ctx),Self::coordinates(ctx)))
            && (self.after!=self.before ==> crate::routing_proofs::observed(self.incoming,s.placement,self.index,Self::tables(ctx),Self::coordinates(ctx)))
    }
    pub open spec fn image(&self,s:SourceState) -> SourceState {
        SourceState { placement:if self.after==self.before { s.placement }
            else { p::State { views:s.placement.views.insert(self.client,self.index),..s.placement } },..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(s).placement,
    { cache_installed(c,s.placement,self.client,self.index,self.before,self.after,self.incoming,self.status,Self::tables(ctx),Self::coordinates(ctx)) }
}

pub enum SourceEvent {
    Control(ControlRecord),Metadata(MetadataRecord),Registry(RegistryRecord),Admission(AdmissionRecord),
    Engine(EngineRecord),Capture(CaptureRecord),Copy(CopyRecord),Seal(SealRecord),Delete(DeleteRecord),
    Drain(DrainRecord),Immediate(ImmediateReceipt),Cleanup(CleanupReceipt),Unchanged(UnchangedTransfer),Cache(CacheRecord),
    CapturePrivate(PrivateCaptureRecord),CopyPrivate(PrivateCopyRecord),
}
impl SourceEvent {
    /// Actual receiver snapshots, not merely metadata-compatible fresh objects.
    pub closed spec fn native_transition(&self) -> Option<(Participant,Participant)> {
        match *self {
            Self::Metadata(e)=>Some((e.before,e.after)),
            Self::Registry(e)=>Some((e.before,e.after)),
            Self::Admission(e)=>Some((e.before,e.after)),
            Self::Seal(e)=>Some((e.before,e.after)),
            Self::Cleanup(e)=>Some((e.before,e.after)),
            Self::Unchanged(e)=>Some((e.before,e.after)),
            Self::Capture(e)=>Some((e.node,e.node)),Self::Copy(e)=>Some((e.node,e.node)),
            Self::Delete(e)=>Some((e.node,e.node)),Self::Drain(e)=>Some((e.node,e.node)),
            Self::Immediate(e)=>Some((e.node,e.node)),
            Self::CapturePrivate(e)=>Some((e.node,e.node)),Self::CopyPrivate(e)=>Some((e.node,e.node)),
            _=>None,
        }
    }
    pub closed spec fn native_observed(&self,s:SourceState) -> bool {
        match self.native_transition() {
            None=>true,
            Some((before,after))=>s.nodes.contains_key(before.owner_view())
                && s.nodes[before.owner_view()]==before && before.wf() && after.wf()
                && after.owner_view()==before.owner_view() && after.retains_receipts(before),
        }
    }
    pub closed spec fn native_image(&self,nodes:Map<u32,Participant>) -> Map<u32,Participant> {
        match self.native_transition() {
            None=>nodes,Some((before,after))=>nodes.insert(before.owner_view(),after),
        }
    }
    pub closed spec fn observed(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> bool {
        self.native_observed(s) && match *self {
            Self::Control(e)=>e.observed(c,ctx,s),Self::Metadata(e)=>e.observed(ctx,s),
            Self::Registry(e)=>e.observed(ctx,s),Self::Admission(e)=>e.observed(ctx,s),
            Self::Engine(e)=>e.observed(ctx,s),Self::Capture(e)=>e.observed(ctx,s),
            Self::Copy(e)=>e.observed(ctx,s),Self::Seal(e)=>e.observed(ctx,s),Self::Delete(e)=>e.observed(ctx,s),
            Self::Drain(e)=>e.observed(ctx,s),Self::Immediate(e)=>e.observed(s),Self::Cleanup(e)=>e.observed(ctx,s),
            Self::Unchanged(e)=>e.observed(ctx,s),Self::Cache(e)=>e.observed(ctx,s),
            Self::CapturePrivate(e)=>e.observed(ctx,s),Self::CopyPrivate(e)=>e.observed(ctx,s),
        }
    }
    pub closed spec fn image(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> SourceState {
        let next=match *self {
            Self::Control(e)=>e.image(c,ctx,s),Self::Metadata(e)=>e.image(ctx,s),
            Self::Registry(e)=>e.image(ctx,s),Self::Admission(e)=>e.image(ctx,s),
            Self::Engine(e)=>e.image(ctx,s),Self::Capture(e)=>e.image(ctx,s),
            Self::Copy(e)=>e.image(s),Self::Seal(e)=>e.image(s),Self::Delete(e)=>e.image(s),
            Self::Drain(e)=>e.image(s),Self::Immediate(e)=>e.image(s),Self::Cleanup(e)=>e.image(s),
            Self::Unchanged(e)=>s,Self::Cache(e)=>e.image(s),
            Self::CapturePrivate(e)=>e.image(s),Self::CopyPrivate(e)=>e.image(s),
        };
        SourceState { nodes:self.native_image(s.nodes),..next }
    }
    pub proof fn receipt_step(&self,c:p::Constants,ctx:SourceContext,s:SourceState,owner:u32,g:u64)
        requires self.observed(c,ctx,s),s.nodes.contains_key(owner),s.nodes[owner].has_receipt(g),
        ensures self.image(c,ctx,s).nodes.contains_key(owner),self.image(c,ctx,s).nodes[owner].has_receipt(g),
    {
        if let Some((before,after))=self.native_transition() {
            if before.owner_view()==owner { assert(after.has_receipt(g)); }
        }
    }
    proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires ctx.wf(c),p::inv(c,s.placement),s.registries_match(ctx),self.observed(c,ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(c,ctx,s).placement,
            self.image(c,ctx,s).registries_match(ctx),
    {
        let tracked out=match *self {
            Self::Control(e)=>e.certify(c,ctx,s),Self::Metadata(e)=>e.certify(c,ctx,s),
            Self::Registry(e)=>e.certify(c,ctx,s),Self::Admission(e)=>e.certify(c,ctx,s),
            Self::Engine(e)=>e.certify(c,ctx,s),Self::Capture(e)=>e.certify(c,ctx,s),
            Self::Copy(e)=>e.certify(c,ctx,s),Self::Seal(e)=>e.certify(c,ctx,s),Self::Delete(e)=>e.certify(c,ctx,s),
            Self::Drain(e)=>e.certify(c,ctx,s),Self::Immediate(e)=>e.certify(c,s),Self::Cleanup(e)=>e.certify(c,ctx,s),
            Self::Unchanged(e)=>e.certify(c,ctx,s),Self::Cache(e)=>e.certify(c,ctx,s),
            Self::CapturePrivate(e)=>e.certify(c,ctx,s),Self::CopyPrivate(e)=>e.certify(c,ctx,s),
        };
        match *self {
            Self::Registry(_) | Self::Admission(_) => {},
            _ => {
                if let Self::Control(e)=*self {
                    if let ControlCall::Begin { result:Ok(_),.. }=e.call {
                        master::replay_begin_fields(s.placement,e.after.records.last().plan.generation as nat,
                            master::begin_plan(c,e.before,e.after,ctx.observations),master::phase(e.after.controls.last().phase),
                            e.after.next_generation as nat,master::active(e.after));
                    }
                }
                if let Self::Engine(EngineRecord::Settle { id,.. })=*self {
                    assert(s.placement.sessions.contains_key(transaction(id)));
                }
                let next=self.image(c,ctx,s);
                assert(next.registries==s.registries);
                assert(next.placement.sessions.dom() =~= s.placement.sessions.dom());
                assert forall|t:int| s.placement.sessions.contains_key(t) implies
                    next.placement.sessions[t].held==s.placement.sessions[t].held by {}
                registries_frame(s.registries,s.placement,next.placement,ctx.addresses());
            },
        }
        out
    }
}

/// No model path, target invariant, segment, or journal occurs in this premise.
/// Each next image is calculated solely by the selected source record's fields.
pub open spec fn source_trace(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>) -> bool {
    states.len()==events.len()+1 && forall|i:int| 0<=i<events.len() ==>
        events[i].observed(c,ctx,states[i]) && states[i+1]==events[i].image(c,ctx,states[i])
}
proof fn trace_prefix(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>)
    requires source_trace(c,ctx,states,events),events.len()>0,
    ensures source_trace(c,ctx,states.drop_last(),events.drop_last()),
{
    assert forall|i:int| 0<=i<events.drop_last().len() implies
        events.drop_last()[i].observed(c,ctx,states.drop_last()[i])
            && states.drop_last()[i+1]==events.drop_last()[i].image(c,ctx,states.drop_last()[i]) by {
        assert(events[i].observed(c,ctx,states[i]));
    }
}
/// Only this private fold accepts a prefix ledger; public entry points construct
/// that ledger from initialization or consume the actual exclusive loader.
proof fn extend_source(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,
    tracked prefix:Execution) -> (tracked out:Execution)
    requires ctx.wf(c),source_trace(c,ctx,states,events),prefix.closed(c,states[0].placement),
        states[0].registries_match(ctx),
    ensures out.closed(c,states.last().placement),
        forall|i:int| 0<=i<states.len() ==> #[trigger] p::inv(c,states[i].placement),
        forall|i:int| 0<=i<states.len() ==> #[trigger] states[i].registries_match(ctx),
    decreases events.len(),
{
    if events.len()==0 {
        assert(states.last()==states[0]);
        prefix.finite_history(c,states[0].placement);
        prefix
    } else {
        trace_prefix(c,ctx,states,events);
        let tracked prefix=extend_source(c,ctx,states.drop_last(),events.drop_last(),prefix);
        let i=events.len() as int-1;
        assert(states.drop_last().last()==states[i]);
        let tracked effect=events[i].certify(c,ctx,states[i]);
        let tracked out=prefix.append(c,states[i].placement,effect);
        assert(states.last()==states[i+1]);
        out.finite_history(c,states.last().placement);
        assert forall|j:int| 0<=j<states.len() implies #[trigger] p::inv(c,states[j].placement) by {
            if j<states.len()-1 { assert(states.drop_last()[j]==states[j]); }
            else { assert(states[j]==states.last()); }
        }
        assert forall|j:int| 0<=j<states.len() implies #[trigger] states[j].registries_match(ctx) by {
            if j<states.len()-1 { assert(states.drop_last()[j]==states[j]); }
            else { assert(states[j]==states.last()); }
        }
        out
    }
}

/// Constructive source-to-model theorem: callers supply source observations,
/// never an already-closed history or a desired placement transition sequence.
pub proof fn finite_source_history(c:p::Constants,ctx:SourceContext,images:EngineImages,
    states:Seq<SourceState>,events:Seq<SourceEvent>) -> (tracked out:Execution)
    requires ctx.wf(c),source_trace(c,ctx,states,events),
        SourceState::nodes_initial(ctx,states[0].nodes),
        states[0]==SourceState::activated(ctx,p::initial(c),images,states[0].nodes),
        engine_observed(p::initial(c),images,ctx.labels),
    ensures out.closed(c,states.last().placement),
        forall|i:int| 0<=i<states.len() ==> #[trigger] p::inv(c,states[i].placement),
        forall|i:int| 0<=i<states.len() ==> #[trigger] states[i].registries_match(ctx),
        exists|behavior:Seq<p::State>| p::behavior(c,behavior) && behavior.last()==states.last().placement,
{
    let tracked prefix=Execution::new(c);
    assert(states[0].registries_match(ctx));
    let tracked out=extend_source(c,ctx,states,events,prefix);
    out.finite_history(c,states.last().placement);
    out
}

/// The live prefix starts at actual post-load bytes, with their established
/// writer/tombstone provenance. Activation itself constructs the initial ledger.
pub proof fn loaded_source_history(c:p::Constants,ctx:SourceContext,tracked loader:Bootstrap,
    lifecycle:BootstrapLifecycle,states:Seq<SourceState>,events:Seq<SourceEvent>) -> (tracked out:Execution)
    requires ctx.wf(c),loader.valid(c,ctx.labels),bootstrap_ready(c,lifecycle),
        bootstrap_progress(loader.lifecycle(),lifecycle),source_trace(c,ctx,states,events),
        SourceState::nodes_initial(ctx,states[0].nodes),
        states[0]==SourceState::activated(ctx,loader.state(),loader.images(),states[0].nodes),
    ensures out.closed(c,states.last().placement),
        forall|i:int| 0<=i<states.len() ==> #[trigger] p::inv(c,states[i].placement),
        forall|i:int| 0<=i<states.len() ==> #[trigger] states[i].registries_match(ctx),
        exists|behavior:Seq<p::State>| p::behavior(c,behavior) && behavior.last()==states.last().placement,
{
    let tracked (prefix,bootstrap)=loader.activate(c,ctx.labels,lifecycle);
    bootstrap.established_prefix(c,states[0].placement,states[0].images,ctx.labels);
    assert(states[0].registries_match(ctx));
    let tracked out=extend_source(c,ctx,states,events,prefix);
    out.finite_history(c,states.last().placement);
    out
}
} // verus!
