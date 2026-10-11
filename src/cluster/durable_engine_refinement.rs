//! Native durable effects, projected into the existing recovery protocol.
//! Completion facts originate in execute_final's actual mirror and row barrier;
//! journal bytes, rather than a desired Ready state, authorize publication.
//! Versions below are physical engine versions, not model transaction writers.
//! Authenticated captures and the finite injective byte embedding remain the
//! explicit source boundary. This is not a production MBTA recovery adapter.
use vstd::prelude::*;
use crate::durable_engine::{self as e,DurableEngine,RowState,RowEffect,TxnRecord,TxnDecision};
use crate::durable_participant::{DurableParticipant,history::CompletionFact};
use crate::durable_participant::codec::{self,Operation};
use crate::durable_kv::Admission;
use crate::storage::{self,Cell};
use crate::types::{TxnId,Status};
use crate::sharding_placement as p;
use crate::sharding_recovery as r;
use crate::execution_refinement as source;
use crate::transfer_proofs as transfer;
use crate::durable_recovery_refinement as bridge;
use crate::durable_participant::history;
use crate::durable_participant::replay_proofs::{ActorHistory,NativeStep};


verus! {
proof fn recovery_plan_facts(c:p::Constants,s:r::State,g:nat)
    requires r::inv(c,s),s.core.placement.plans.contains_key(g),
    ensures p::plan_inv(c,s.core.placement,g),
        c.shards.contains(s.core.placement.plans[g].dst),
        s.core.placement.plans[g].keys!=Set::<int>::empty(),
{
    assert(p::inv(c,s.core.placement));
    assert(p::plan_inv(c,s.core.placement,g));
}

proof fn completion_fields(fact:CompletionFact)
    requires fact.valid(),!fact.cleanup,
    ensures fact.owner==fact.plan.destination,
        fact.before.wf(),fact.after.wf(),
        fact.before.owner_view()==fact.plan.destination,fact.after.owner_view()==fact.plan.destination,
        fact.before.transfer_authorized(fact.plan,false),fact.after.seal_frame(fact.before,fact.plan),
        storage::complete(fact.durable,fact.initial,fact.source,fact.plan.range),
{
    reveal(CompletionFact::valid);
}

proof fn selected_completion(node:&DurableParticipant,index:int)
    requires node.wf(),0<=index<node.records().len(),node.completion_facts().contains_key(index),
        !node.completion_facts()[index].cleanup,
        node.records()[index]==codec::wire(Operation::Complete{plan:node.completion_facts()[index].plan,cleanup:false}),
    ensures node.replay_history().valid(node.records(),node.namespace().owner),
        0<index,index-1<node.replay_history().steps.len(),
        node.completion_facts()[index].valid(),node.completion_facts()[index].owner==node.namespace().owner,
        node.records()[index-1]==codec::wire(Operation::Reserve{plan:node.completion_facts()[index].plan,cleanup:false}),
        node.completion_facts()[index].before.semantic_eq(node.replay_history().steps[index-1].before),
        node.completion_facts()[index].after.semantic_eq(node.replay_history().steps[index-1].after),
{
    hide(NativeStep::valid);hide(codec::wire);
    node.history_facts();
    assert(history::is_completion(node.records()[index]));
    history::completion_at(node.records(),node.namespace().owner,node.completion_facts(),node.replay_history(),index);
}

/// The selected fact is historical evidence, not a recovered runtime argument.
pub proof fn persisted_completion(node:&DurableParticipant,generation:u64)
    -> (fact:CompletionFact)
    requires node.completion_record(generation,false),
    ensures fact.valid(),!fact.cleanup,fact.plan.generation==generation,
        node.completion_facts().values().contains(fact),
        exists|i:int| 0<=i<node.records().len()
            && node.records()[i]==codec::wire(Operation::Complete{plan:fact.plan,cleanup:false}),
{
    let i=history::completion_index(node.records(),node.completion_facts(),generation,false);
    assert(node.completion_facts().values().contains(node.completion_facts()[i]));
    node.completion_facts()[i]
}

pub open spec fn completion_packet(fact:CompletionFact,key:int,labels:Map<int,Cell>,writers:Map<int,int>)->p::Packet {
    transfer::packet(fact.plan.generation as nat,key,transfer::observed(fact.source,labels[key],writers[key]))
}

/// Successful transfer plus the committed barrier produces this fact in the
/// wrapper. Reuse the real loop theorem, including deletion/EOF coverage.
pub proof fn completed_native_copy(c:p::Constants,fact:CompletionFact,s:p::State,
    keys:Seq<int>,labels:Map<int,Cell>,writers:Map<int,int>)->(tracked out:source::ClosedTransfer)
    requires fact.valid(),!fact.cleanup,
        transfer::labels_cover(fact.plan,s,keys,labels),transfer::local_coupling(&fact.before,fact.plan,s,labels),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((fact.plan.destination as int,k))
            && s.packets.contains(completion_packet(fact,k,labels,writers)),
    ensures out.valid(c,s),out.after(s).certificates.contains((fact.plan.generation as nat,p::Certificate::Ready)),
        transfer::local_coupling(&fact.after,fact.plan,out.after(s),labels),
        forall|k:int| labels.contains_key(k) ==> transfer::observed(fact.durable,labels[k],writers[k])
            ==completion_packet(fact,k,labels,writers).cell,
{
    completion_fields(fact);
    transfer::completed_coverage(fact.durable,fact.initial,fact.source,fact.plan,keys,labels,writers);
    source::completed_final(c,&fact.before,&fact.after,fact.plan,s,keys,labels,
        fact.initial,fact.durable,fact.source,writers)
}

pub open spec fn reserved_copy(c:p::Constants,s:r::State,packet:p::Packet)->r::State {
    r::apply(c,s,r::Action::StartCopy{packet})
}
pub open spec fn durable_copy(c:p::Constants,s:r::State,packet:p::Packet)->r::State {
    if s.copied.contains(packet) { reserved_copy(c,s,packet) }
    else { r::apply(c,reserved_copy(c,s,packet),r::Action::DurableCopy{packet}) }
}

/// Structural simulation correspondence, maintained when the metadata bridge
/// emits StartCleanup from an actual committed owner-local reservation. `cut`
/// counts native steps already represented; it is not a backend safety answer.
pub open spec fn cleanup_history_backing(s:r::State,
    trace:crate::durable_participant::replay_proofs::ActorHistory,cut:int,owner:u32)->bool {
    0<=cut<=trace.steps.len()
        && forall|entry:(nat,int,p::Command)| s.cleaning.contains(entry) && entry.1==owner as int ==>
            exists|j:int| 0<=j<cut && match #[trigger] trace.steps[j].operation {
                Operation::Reserve{plan,cleanup}=>cleanup && plan.generation as nat==entry.0,
                _=>false,
            }
}
pub proof fn initial_cleanup_history_backing(c:p::Constants,
    trace:crate::durable_participant::replay_proofs::ActorHistory,owner:u32)
    ensures cleanup_history_backing(r::initial(c),trace,0,owner),
{}

/// Every actual metadata command moves a point's fence monotonically; once a
/// point is terminal it cannot become nonterminal at the same generation.
proof fn native_step_fence(step:crate::durable_participant::replay_proofs::NativeStep,
    table:u64,key:Seq<u8>)
    requires step.valid(),step.before.local_meta(table,key).is_some(),
    ensures step.after.local_meta(table,key).is_some(),
        step.before.local_meta(table,key).unwrap().fence<=step.after.local_meta(table,key).unwrap().fence,
        step.before.local_meta(table,key).unwrap().terminal
            && step.before.local_meta(table,key).unwrap().fence==step.after.local_meta(table,key).unwrap().fence
                ==> step.after.local_meta(table,key).unwrap().terminal,
{
    match step.operation {
        Operation::Deliver{plan,command}=>{
            if step.status!=Status::Ok || step.before.command_seen(plan.generation,command) {
                step.after.metadata_observations(step.before);
            } else if crate::bytes::contains_spec(plan.range,table,key) {
                assert(step.before.command_at(plan,command,step.before.drained_view(plan.range),key));
                let m=step.before.local_meta(table,key).unwrap();
                assert(crate::participant::guard_spec(m,crate::directory::route(plan.old@,key).ownership_version,
                    plan.generation,command,step.before.owner_view()==plan.source,
                    step.before.owner_view()==plan.destination,step.before.drained_view(plan.range)));
                match command {
                    crate::types::Command::Start|crate::types::Command::Freeze=>{},
                    crate::types::Command::Final|crate::types::Command::Retire=>{},
                    crate::types::Command::Commit|crate::types::Command::Abort=>{},
                }
            }
        },
        Operation::Complete{plan,cleanup}=>{
            if !cleanup && step.before.ready_seen(plan.generation) {
                step.after.metadata_observations(step.before);
            }
        },
        Operation::Begin{..}|Operation::Resolve(_)|Operation::Finish(_)|Operation::Release{..}|Operation::Acquire{..}=>{
            step.after.metadata_observations(step.before);
        },
        _=>{},
    }
}

proof fn native_history_fence(trace:crate::durable_participant::replay_proofs::ActorHistory,
    records:Seq<Seq<u8>>,owner:u32,start:int,end:int,table:u64,key:Seq<u8>)
    requires trace.valid(records,owner),0<=start<=end<trace.steps.len(),
        trace.steps[start].before.local_meta(table,key).is_some(),
    ensures trace.steps[end].before.local_meta(table,key).is_some(),
        trace.steps[start].before.local_meta(table,key).unwrap().fence
            <=trace.steps[end].before.local_meta(table,key).unwrap().fence,
        trace.steps[start].before.local_meta(table,key).unwrap().terminal
            && trace.steps[start].before.local_meta(table,key).unwrap().fence
                ==trace.steps[end].before.local_meta(table,key).unwrap().fence
                    ==> trace.steps[end].before.local_meta(table,key).unwrap().terminal,
    decreases end-start,
{
    hide(NativeStep::valid);hide(codec::wire);
    hide(crate::durable_participant::replay_proofs::allowed);
    if start<end {
        trace.entry(records,owner,end-1);
        trace.entry(records,owner,end);
        native_history_fence(trace,records,owner,start,end-1,table,key);
        native_step_fence(trace.steps[end-1],table,key);
        assert(trace.steps[end].before.local_meta(table,key)==trace.steps[end-1].after.local_meta(table,key));
    }
}

proof fn lift_bound_prefix(trace:crate::durable_participant::replay_proofs::ActorHistory,
    cut:int,plan:crate::types::MigrationPlan)
    requires 0<=cut<=trace.steps.len(),crate::durable_participant::replay_proofs::bound(trace.steps.take(cut),plan),
    ensures crate::durable_participant::replay_proofs::bound(trace.steps,plan),
{
    let i=choose|i:int| #![trigger trace.steps.take(cut)[i]] 0<=i<trace.steps.take(cut).len() && match trace.steps.take(cut)[i].operation {
        Operation::Bind(p)=>storage::same_plan(p,plan),_=>false,
    };
    assert(trace.steps[i]==trace.steps.take(cut)[i]);
}

proof fn operation_binding(trace:ActorHistory,records:Seq<Seq<u8>>,owner:u32,index:int)
    requires trace.valid(records,owner),0<=index<trace.steps.len(),
        trace.steps[index].operation is Reserve || trace.steps[index].operation is Complete,
    ensures match trace.steps[index].operation {
        Operation::Reserve{plan,..}|Operation::Complete{plan,..}=>
            crate::durable_participant::replay_proofs::bound(trace.steps,plan),
        _=>false,
    },
{
    hide(NativeStep::valid);hide(codec::wire);
    hide(crate::durable_participant::replay_proofs::pending);
    trace.entry(records,owner,index);
    match trace.steps[index].operation {
        Operation::Reserve{plan,..}|Operation::Complete{plan,..}=>lift_bound_prefix(trace,index,plan),
        _=>{},
    }
}

proof fn reservation_fields(step:NativeStep)
    requires step.valid(),step.operation is Reserve,
    ensures step.before.transfer_authorized(step.operation->Reserve_plan,step.operation->Reserve_cleanup),
{}

/// A late global Abort does not fence copy by itself. A concrete prior LOCAL
/// cleanup reservation would have made this same native generation terminal,
/// contradicting the selected completion's actual Stage authorization.
pub proof fn completion_not_cleanup_fenced(c:p::Constants,s:r::State,node:&DurableParticipant,index:int,
    keys:Seq<int>,labels:Map<int,Cell>)
    requires r::inv(c,s),node.wf(),0<index<node.records().len(),node.completion_facts().contains_key(index),
        !node.completion_facts()[index].cleanup,
        node.records()[index]==codec::wire(Operation::Complete{plan:node.completion_facts()[index].plan,cleanup:false}),
        transfer::labels_cover(node.completion_facts()[index].plan,s.core.placement,keys,labels),
        cleanup_history_backing(s,node.replay_history(),index-1,node.namespace().owner),
    ensures !s.cleaning.contains((node.completion_facts()[index].plan.generation as nat,
        node.completion_facts()[index].plan.destination as int,p::Command::Abort)),
{
    hide(r::inv);hide(p::inv);hide(NativeStep::valid);hide(codec::wire);
    hide(crate::durable_participant::replay_proofs::allowed);hide(storage::complete);
    selected_completion(node,index);
    let fact=node.completion_facts()[index];let trace=node.replay_history();let end=index-1;
    completion_fields(fact);
    recovery_plan_facts(c,s,fact.plan.generation as nat);
    trace.entry(node.records(),node.namespace().owner,end);
    assert(fact.valid() && fact.owner==node.namespace().owner && fact.owner==fact.plan.destination);
    assert(fact.before.semantic_eq(trace.steps[end].before));
    codec::operation_unique(trace.steps[end].operation,Operation::Complete{plan:fact.plan,cleanup:false});
    assert(trace.steps[end].operation is Complete);
    let actual=trace.steps[end].operation->Complete_plan;
    assert(storage::same_plan(actual,fact.plan));
    operation_binding(trace,node.records(),node.namespace().owner,end);
    let entry=(fact.plan.generation as nat,fact.plan.destination as int,p::Command::Abort);
    if s.cleaning.contains(entry) {
        let j=choose|j:int| #![trigger trace.steps[j]] 0<=j<end && match trace.steps[j].operation {
            Operation::Reserve{plan,cleanup}=>cleanup && plan.generation as nat==entry.0,_=>false,
        };
        let cleanup=trace.steps[j].operation->Reserve_plan;
        trace.entry(node.records(),node.namespace().owner,j);
        reservation_fields(trace.steps[j]);
        assert(trace.steps[j].before.transfer_authorized(cleanup,true));
        operation_binding(trace,node.records(),node.namespace().owner,j);
        crate::durable_participant::replay_proofs::immutable_bindings(trace,node.records(),node.namespace().owner,cleanup,actual);
        assert(storage::same_plan(cleanup,fact.plan));
        assert(p::plan_inv(c,s.core.placement,fact.plan.generation as nat));
        assert(keys.to_set()!=Set::<int>::empty());
        assert(keys.len()>0) by {
            if keys.len()==0 { assert(keys.to_set() =~= Set::<int>::empty()); }
        }
        let k=keys[0];
        assert(keys.to_set().contains(k));
        assert(labels.dom().contains(k));
        assert(storage::in_range(fact.plan.range,labels[k]));
        assert(crate::bytes::contains_spec(cleanup.range,cleanup.range.table,labels[k].1.0));
        assert(trace.steps[j].before.range_role(cleanup,crate::types::Role::Empty,None,true));
        assert(fact.before.range_role(fact.plan,crate::types::Role::Stage,Some(1),false));
        native_history_fence(trace,node.records(),node.namespace().owner,j,end,fact.plan.range.table,labels[k].1.0);
        assert(trace.steps[end].before.local_meta(fact.plan.range.table,labels[k].1.0).unwrap().terminal);
        assert(!fact.before.local_meta(fact.plan.range.table,labels[k].1.0).unwrap().terminal);
    }
}

/// One key of a COMPLETE durable mirror. Earlier raw destination writes are
/// private Stage bytes: they do not authorize this publication or Seal. The
/// reservation is established before its durable completion; duplicates reuse
/// their exact packet rather than creating a second effect.
proof fn publish_copy_cell(c:p::Constants,s:r::State,fact:CompletionFact,
    keys:Seq<int>,labels:Map<int,Cell>,writers:Map<int,int>,key:int)
    requires r::inv(c,s),bridge::old_slots(s),fact.valid(),!fact.cleanup,
        transfer::labels_cover(fact.plan,s.core.placement,keys,labels),
        transfer::local_coupling(&fact.before,fact.plan,s.core.placement,labels),
        keys.to_set().contains(key),s.core.placement.physical.contains_key((fact.plan.destination as int,key)),
        s.core.placement.packets.contains(completion_packet(fact,key,labels,writers)),
        r::active(s,r::Authority::Participant(fact.plan.destination as int)),
        !s.cleaning.contains((fact.plan.generation as nat,fact.plan.destination as int,p::Command::Abort)),
    ensures ({let packet=completion_packet(fact,key,labels,writers);
        let started=reserved_copy(c,s,packet);let durable=durable_copy(c,s,packet);
        let action=p::Action::DeliverCopy{packet};let z=bridge::committed(c,durable,action);
        r::enabled(c,s,r::Action::StartCopy{packet}) && r::next(c,s,started)
            && r::next(c,started,durable)
            && r::enabled(c,durable,r::Action::Accept{envelope:r::envelope(durable,action)})
            && r::next(c,durable,bridge::accepted(c,durable,action))
            && r::next(c,bridge::accepted(c,durable,action),z)
            && r::inv(c,z) && bridge::old_slots(z)
            && (public_copy_boundary(s) ==> public_copy_boundary(z))
            && r::active(z,r::Authority::Participant(fact.plan.destination as int))
            && z.cleaning==s.cleaning
            && z.core==r::with_placement(s.core,p::State{physical:s.core.placement.physical.insert(
                (fact.plan.destination as int,key),p::Replica{
                    cell:transfer::observed(fact.durable,labels[key],writers[key]),covered:true,
                    ..s.core.placement.physical[(fact.plan.destination as int,key)]}),..s.core.placement})
            && z.core.placement.physical[(fact.plan.destination as int,key)].cell
                ==transfer::observed(fact.durable,labels[key],writers[key])
            && z.core.placement.physical[(fact.plan.destination as int,key)].covered
            && z.core.placement.certificates==s.core.placement.certificates}),
{
    hide(r::next);
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);hide(storage::complete);
    completion_fields(fact);
    recovery_plan_facts(c,s,fact.plan.generation as nat);
    let packet=completion_packet(fact,key,labels,writers);
    assert(!r::copy_fenced(s,packet));
    transfer::completed_coverage(fact.durable,fact.initial,fact.source,fact.plan,keys,labels,writers);
    let segment=transfer::copy_segment(c,&fact.before,fact.plan,s.core.placement,keys,labels,key,packet.cell);
    assert(fact.before.range_role(fact.plan,crate::types::Role::Stage,Some(1),false));
    assert(storage::in_range(fact.plan.range,labels[key]));
    assert(p::copy_guard(s.core.placement,packet));
    assert(r::enabled(c,s,r::Action::StartCopy{packet}));
    r::lemma_step(c,s,r::Action::StartCopy{packet});
    bridge::slots_step(c,s,r::Action::StartCopy{packet});
    let started=reserved_copy(c,s,packet);
    bridge::dispatch_edge(c,s,r::Action::StartCopy{packet});
    if !s.copied.contains(packet) {
        r::lemma_step(c,started,r::Action::DurableCopy{packet});
        bridge::slots_step(c,started,r::Action::DurableCopy{packet});
        bridge::dispatch_edge(c,started,r::Action::DurableCopy{packet});
    } else {
        assert(r::dispatch(c,started,r::Action::Stutter)==started);
        bridge::dispatch_edge(c,started,r::Action::Stutter);
    }
    let durable=durable_copy(c,s,packet);
    let action=p::Action::DeliverCopy{packet};
    assert(p::enabled(c,durable.core.placement,action));
    assert(r::effect_guard(durable,r::envelope(durable,action)));
    bridge::commit_authority(c,durable,action);
    if public_copy_boundary(s) { copy_publication_boundary(c,s,packet); }
}

/// Internal final Seal after the actual full-mirror publication sequence.
/// The public macrostep below selects fact from persisted journal evidence.
proof fn publish_ready(c:p::Constants,s:r::State,fact:CompletionFact,
    keys:Seq<int>,labels:Map<int,Cell>,writers:Map<int,int>)
    requires r::inv(c,s),bridge::old_slots(s),fact.valid(),!fact.cleanup,
        transfer::labels_cover(fact.plan,s.core.placement,keys,labels),
        transfer::local_coupling(&fact.before,fact.plan,s.core.placement,labels),
        r::active(s,r::Authority::Participant(fact.plan.destination as int)),
        forall|k:int| keys.to_set().contains(k) ==>
            s.core.placement.physical.contains_key((fact.plan.destination as int,k))
                && p::replica(s.core.placement,fact.plan.destination as int,k).covered
                && p::replica(s.core.placement,fact.plan.destination as int,k).cell
                    ==transfer::observed(fact.durable,labels[k],writers[k]),
    ensures ({let action=p::Action::Seal{generation:fact.plan.generation as nat};let z=bridge::committed(c,s,action);
        r::next(c,s,bridge::accepted(c,s,action)) && r::next(c,bridge::accepted(c,s,action),z)
            && r::inv(c,z) && bridge::old_slots(z)
            && z.core.placement==p::apply(c,s.core.placement,action)
            && transfer::local_coupling(&fact.after,fact.plan,z.core.placement,labels)
            && z.core.placement.certificates.contains((fact.plan.generation as nat,p::Certificate::Ready))}),
{
    hide(r::next);
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);
    completion_fields(fact);
    recovery_plan_facts(c,s,fact.plan.generation as nat);
    let tracked native=source::sealed(c,&fact.before,&fact.after,fact.plan,s.core.placement,keys,labels,
        fact.durable,fact.initial,fact.source,writers);
    let generation=fact.plan.generation as nat;
    assert forall|k:int| s.core.placement.plans[generation].keys.contains(k) implies {
        let replica=p::replica(s.core.placement,fact.plan.destination as int,k);
        replica.role is Stage && replica.fence==generation && !replica.terminal && replica.round==1 && replica.covered
    } by {
        assert(storage::in_range(fact.plan.range,labels[k]));
        assert(fact.before.range_role(fact.plan,crate::types::Role::Stage,Some(1),false));
    }
    let action=p::Action::Seal{generation};
    bridge::commit_authority(c,s,action);
    let z=bridge::committed(c,s,action);
    assert(z.core.placement.physical =~= native.after(s.core.placement).physical);
    assert(z.core.placement==native.after(s.core.placement));
}

/// An output projection computed from actual durable mirror bytes and captured
/// source writers. It is never a premise specifying a desired successor.
pub open spec fn copied_image(s:p::State,fact:CompletionFact,labels:Map<int,Cell>,
    writers:Map<int,int>,work:Set<int>)->p::State {
    p::State{physical:vstd::imap::IMap::new(|q:(int,int)| s.physical.contains_key(q),|q:(int,int)|
        if q.0==fact.plan.destination as int && work.contains(q.1) {
            p::Replica{cell:transfer::observed(fact.durable,labels[q.1],writers[q.1]),covered:true,..s.physical[q]}
        } else { s.physical[q] }),..s}
}

/// All selected bytes have already passed the actual mirror and durability
/// barrier. Expand their authenticated packets in finite key order, retaining
/// native Stage metadata until the final persisted Ready publication.
proof fn publish_copies(c:p::Constants,s:r::State,fact:CompletionFact,
    keys:Seq<int>,labels:Map<int,Cell>,writers:Map<int,int>,work:Seq<int>)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),fact.valid(),!fact.cleanup,
        transfer::labels_cover(fact.plan,s.core.placement,keys,labels),
        transfer::local_coupling(&fact.before,fact.plan,s.core.placement,labels),
        r::active(s,r::Authority::Participant(fact.plan.destination as int)),
        !s.cleaning.contains((fact.plan.generation as nat,fact.plan.destination as int,p::Command::Abort)),
        work.to_set().subset_of(keys.to_set()),
        forall|k:int| keys.to_set().contains(k) ==>
            s.core.placement.physical.contains_key((fact.plan.destination as int,k))
                && s.core.placement.packets.contains(completion_packet(fact,k,labels,writers)),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        out.0.cleaning==s.cleaning,
        out.0.core.placement==copied_image(s.core.placement,fact,labels,writers,work.to_set()),
        transfer::local_coupling(&fact.before,fact.plan,out.0.core.placement,labels),
        r::active(out.0,r::Authority::Participant(fact.plan.destination as int)),
    decreases work.len(),
{
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);hide(r::next);
    if work.len()==0 {
        assert(work.to_set() =~= Set::<int>::empty());
        assert(copied_image(s.core.placement,fact,labels,writers,work.to_set()).physical
            =~=s.core.placement.physical);
        (s,seq![s])
    } else {
        transfer::split_set(work);
        let prefix=work.drop_last();
        assert(prefix.to_set().subset_of(keys.to_set()));
        let (mid,path)=publish_copies(c,s,fact,keys,labels,writers,prefix);
        let key=work.last();
        assert(keys.to_set().contains(key));
        publish_copy_cell(c,mid,fact,keys,labels,writers,key);
        let packet=completion_packet(fact,key,labels,writers);
        let started=reserved_copy(c,mid,packet);
        let durable=durable_copy(c,mid,packet);
        let action=p::Action::DeliverCopy{packet};
        let accepted=bridge::accepted(c,durable,action);
        let z=bridge::committed(c,durable,action);
        let states=path.push(started).push(durable).push(accepted).push(z);
        assert forall|i:int| 0<=i<states.len()-1 implies
            #[trigger] r::next(c,states[i],states[i+1]) by {
            if i<path.len()-1 {}
            else if i==path.len()-1 { assert(path.last()==mid); }
            else if i==path.len() {}
            else if i==path.len()+1 {}
            else { assert(i==path.len()+2); }
        }
        assert(z.core.placement.physical
            =~=copied_image(s.core.placement,fact,labels,writers,work.to_set()).physical) by {
            assert forall|q:(int,int)| s.core.placement.physical.contains_key(q) implies
                z.core.placement.physical[q]
                    ==copied_image(s.core.placement,fact,labels,writers,work.to_set()).physical[q] by {
                if q==(fact.plan.destination as int,key) {
                    assert(z.core.placement.physical[q].cell==transfer::observed(fact.durable,labels[key],writers[key]));
                }
            }
        }
        assert forall|k:int| z.core.placement.plans[fact.plan.generation as nat].keys.contains(k) implies
            fact.before.local_meta(labels[k].0,labels[k].1.0).is_some()
                && crate::participant_proofs::metadata(fact.before.local_meta(labels[k].0,labels[k].1.0).unwrap(),
                    p::replica(z.core.placement,fact.before.owner_view() as int,k)) by {}
        (z,states)
    }
}

/// Freshness is selected from existing history, not a backend safety callback.
/// A late coordinator Abort still permits local copy/Seal while the native
/// destination is Stage and no local cleanup reservation has preceded it.
/// Ready retransmissions and replaced generations have no new abstract effect.
pub open spec fn fresh_completion(s:r::State,fact:CompletionFact)->bool {
    let g=fact.plan.generation as nat;
    s.core.placement.active==Some(g) && s.core.placement.plans.contains_key(g)
        && !s.core.placement.certificates.contains((g,p::Certificate::Ready))
}

/// Select one exact persisted Complete record. Native history_facts proves its
/// full-mirror/barrier fact and immediately preceding authorized reservation.
/// Ghost evidence is not recovery input; the actual journal selects the index.
pub proof fn completed_copy_path(c:p::Constants,s:r::State,node:&DurableParticipant,index:int,
    keys:Seq<int>,labels:Map<int,Cell>,writers:Map<int,int>)->(out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),node.wf(),
        0<=index<node.records().len(),node.completion_facts().contains_key(index),
        !node.completion_facts()[index].cleanup,
        node.records()[index]==codec::wire(Operation::Complete{plan:node.completion_facts()[index].plan,cleanup:false}),
        ({let fact=node.completion_facts()[index];fresh_completion(s,fact) ==> {
            transfer::labels_cover(fact.plan,s.core.placement,keys,labels)
                && transfer::local_coupling(&fact.before,fact.plan,s.core.placement,labels)
                && r::active(s,r::Authority::Participant(fact.plan.destination as int))
                && cleanup_history_backing(s,node.replay_history(),index-1,node.namespace().owner)
                && forall|k:int| keys.to_set().contains(k) ==>
                    s.core.placement.physical.contains_key((fact.plan.destination as int,k))
                        && s.core.placement.packets.contains(completion_packet(fact,k,labels,writers))
        }}),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        0<index,node.records()[index-1]==codec::wire(Operation::Reserve{
            plan:node.completion_facts()[index].plan,cleanup:false}),
        out.0.core.placement.plans==s.core.placement.plans,
        out.0.core.placement.packets==s.core.placement.packets,
        out.0.cleaning==s.cleaning,
        ({let fact=node.completion_facts()[index];
            if fresh_completion(s,fact) {
                out.0.core.placement.certificates.contains((fact.plan.generation as nat,p::Certificate::Ready))
                    && transfer::local_coupling(&fact.after,fact.plan,out.0.core.placement,labels)
                    && out.0.core.placement==p::apply(c,
                        copied_image(s.core.placement,fact,labels,writers,keys.to_set()),
                        p::Action::Seal{generation:fact.plan.generation as nat})
            } else { out.0==s && out.1==seq![s] }}),
{
    hide(r::next);
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);hide(NativeStep::valid);hide(codec::wire);
    selected_completion(node,index);
    let fact=node.completion_facts()[index];
    if !fresh_completion(s,fact) {
        (s,seq![s])
    } else {
        // The source loop theorem independently checks complete coverage from
        // real mirror bytes and authenticated source packets, including absence.
        completion_not_cleanup_fenced(c,s,node,index,keys,labels);
        let tracked native=completed_native_copy(c,fact,s.core.placement,keys,labels,writers);
        let (copied,path)=publish_copies(c,s,fact,keys,labels,writers,keys);
        assert forall|k:int| keys.to_set().contains(k) implies
            p::replica(copied.core.placement,fact.plan.destination as int,k).covered
                && p::replica(copied.core.placement,fact.plan.destination as int,k).cell
                    ==transfer::observed(fact.durable,labels[k],writers[k]) by {}
        publish_ready(c,copied,fact,keys,labels,writers);
        let action=p::Action::Seal{generation:fact.plan.generation as nat};
        let accepted=bridge::accepted(c,copied,action);
        let z=bridge::committed(c,copied,action);
        public_copy_step(c,copied,r::Action::Accept{envelope:r::envelope(copied,action)});
        public_copy_step(c,accepted,r::Action::Commit{envelope:r::envelope(copied,action)});
        let states=path.push(accepted).push(z);
        assert forall|i:int| 0<=i<states.len()-1 implies
            #[trigger] r::next(c,states[i],states[i+1]) by {
            if i<path.len()-1 {} else if i==path.len()-1 { assert(path.last()==copied); }
        }
        (z,states)
    }
}

/// Byte and version framing for the ONE accepted raw effect; a failed response
/// is not permission to choose an unrelated row or a new version on recovery.
pub proof fn row_settlement_frame(before:RowState,pending:Option<RowEffect>,after:RowState)
    requires e::settled_rows(before,pending,after),
    ensures forall|k:Cell| pending is None || k!=pending.unwrap().cell ==>
        storage::value(after.bytes,k)==storage::value(before.bytes,k)
            && after.versions.contains_key(k)==before.versions.contains_key(k)
            && (before.versions.contains_key(k) ==> after.versions[k]==before.versions[k]),
{
    if after!=before { let effect=pending.unwrap();assert(after==e::row_apply(before,effect)); }
}

pub proof fn row_apply_idempotent(before:RowState,effect:RowEffect)
    ensures e::row_apply(e::row_apply(before,effect),effect)==e::row_apply(before,effect),
{
    assert(e::row_apply(e::row_apply(before,effect),effect).bytes =~= e::row_apply(before,effect).bytes);
    assert(e::row_apply(e::row_apply(before,effect),effect).versions =~= e::row_apply(before,effect).versions);
}

/// Unknown may already have persisted the effect. Replaying that exact effect
/// cannot increment twice: its accepted version is retained in pending_row.
pub proof fn unknown_then_recover(before:RowState,effect:RowEffect,unknown:RowState,recovered:RowState)
    requires e::settled_rows(before,Some(effect),unknown),e::settled_rows(unknown,Some(effect),recovered),
    ensures e::settled_rows(before,Some(effect),recovered),
        forall|k:Cell| k!=effect.cell ==> storage::value(recovered.bytes,k)==storage::value(before.bytes,k)
            && recovered.versions.contains_key(k)==before.versions.contains_key(k)
            && (before.versions.contains_key(k) ==> recovered.versions[k]==before.versions[k]),
{
    row_apply_idempotent(before,effect);
    row_settlement_frame(before,Some(effect),recovered);
}

/// Pending raw work fences every admission token, including a formerly valid
/// incarnation. Successful recover_rows clears the installed token and requires
/// a separate installation before application can pass check again.
pub proof fn pending_admission_fence<E:DurableEngine>(engine:&E,a:Admission)
    requires engine.pending_row() is Some,
    ensures !(engine.installed()==Some(a) && engine.pending_row()==None
        && engine.owner()==a.namespace.owner && !a.namespace.coordinator),
{}

pub proof fn recovered_admission_fence<E:DurableEngine>(before:&E,after:&E,status:Status)
    requires e::settled_rows(before.durable_state(),before.pending_row(),after.durable_state()),
        status==Status::Ok ==> after.recovered() && after.installed()==None && after.pending_row()==None
            && after.working_state()==after.durable_state(),
    ensures status==Status::Ok ==> after.installed()==None && after.working_state()==after.durable_state(),
        forall|k:Cell| before.pending_row() is None || k!=before.pending_row().unwrap().cell ==>
            storage::value(after.durable_state().bytes,k)==storage::value(before.durable_state().bytes,k)
                && after.durable_state().versions.contains_key(k)==before.durable_state().versions.contains_key(k)
                && (before.durable_state().versions.contains_key(k) ==>
                    after.durable_state().versions[k]==before.durable_state().versions[k]),
{
    row_settlement_frame(before.durable_state(),before.pending_row(),after.durable_state());
}

/// This is exactly the primitive's accepted transaction effect, not an engine
/// callback asserting migration correctness. Unknown is deliberately independent
/// of whether this same effect became durable.
pub open spec fn primitive_settlement(before:RowState,after:RowState,
    records:Map<TxnId,TxnRecord>,terminal:Map<TxnId,TxnRecord>,id:TxnId)->bool {
    (before==after && records==terminal)
        || (records.contains_key(id) && terminal.contains_key(id)
            && terminal==records.insert(id,terminal[id]) && e::txn_effect(before,records[id],after,terminal[id]))
        || (!records.contains_key(id) && before==after
            && terminal==records.insert(id,TxnRecord{writes:Seq::empty(),decision:TxnDecision::Abort,settled:true}))
}

pub proof fn exact_transaction_settlement(before:RowState,after:RowState,
    records:Map<TxnId,TxnRecord>,terminal:Map<TxnId,TxnRecord>,id:TxnId)
    requires primitive_settlement(before,after,records,terminal,id),terminal.contains_key(id),terminal[id].settled,
    ensures forall|other:TxnId| other!=id ==> terminal.contains_key(other)==records.contains_key(other)
        && (records.contains_key(other) ==> terminal[other]==records[other]),
        !records.contains_key(id) ==> before==after && terminal[id].writes==Seq::<RowEffect>::empty()
            && terminal[id].decision==TxnDecision::Abort,
        records.contains_key(id) && !records[id].settled ==> terminal[id].writes==records[id].writes
            && ((terminal[id].decision==TxnDecision::Commit && after==e::batch_apply(before,records[id].writes))
                || (terminal[id].decision==TxnDecision::Abort && after==before)),
        records.contains_key(id) && records[id].settled ==> terminal[id]==records[id] && after==before,
{}

/// Exact empty Abort is meaningful only after the primitive's global fence. A
/// missing local record alone cannot invoke this lemma: a terminal tombstone and
/// the exact primitive effect are required. The model session can still hold
/// leases for an admission whose engine registration never reached the owner.
pub proof fn absent_transaction_abort(c:p::Constants,s:r::State,id:TxnId,before:RowState,after:RowState,
    records:Map<TxnId,TxnRecord>,terminal:Map<TxnId,TxnRecord>)
    requires r::inv(c,s),primitive_settlement(before,after,records,terminal,id),
        !records.contains_key(id),terminal.contains_key(id),terminal[id].settled,
        s.pending.contains_key(source::transaction(id)),
    ensures before==after,terminal[id].writes==Seq::<RowEffect>::empty(),terminal[id].decision==TxnDecision::Abort,
        r::enabled(c,s,r::Action::EngineSettle{txn:source::transaction(id),committed:false}),
        r::next(c,s,r::apply(c,s,r::Action::EngineSettle{txn:source::transaction(id),committed:false})),
        r::inv(c,r::apply(c,s,r::Action::EngineSettle{txn:source::transaction(id),committed:false})),
        r::apply(c,s,r::Action::EngineSettle{txn:source::transaction(id),committed:false}).core.placement
            ==p::apply(c,s.core.placement,p::Action::Resolve{txn:source::transaction(id),writes:Map::empty()}),
{
    exact_transaction_settlement(before,after,records,terminal,id);
    let action=r::Action::EngineSettle{txn:source::transaction(id),committed:false};
    r::lemma_step(c,s,action);
    bridge::dispatch_edge(c,s,action);
}

/// A lost settlement response never implies a release. Even when settlement
/// actually committed, both EngineUnknown and EngineSettle retain all held
/// grants; a separately persisted Release must follow the global fence.
pub proof fn settlement_preserves_leases(c:p::Constants,s:r::State,txn:int,committed:bool)
    requires s.pending.contains_key(txn),s.core.placement.sessions.contains_key(txn),
    ensures r::enabled(c,s,r::Action::EngineUnknown{txn}),
        r::apply(c,s,r::Action::EngineUnknown{txn})==s,
        r::apply(c,s,r::Action::EngineSettle{txn,committed}).core.placement.sessions[txn].held
            ==s.core.placement.sessions[txn].held,
        r::apply(c,s,r::Action::EngineSettle{txn,committed}).core.placement.sessions[txn].resolved,
{}

/// The last accepted effect is the byte/version provenance for a cell. Duplicate
/// cells in a primitive write set are handled in order, never assumed unique.
pub open spec fn last_effect(writes:Seq<RowEffect>,key:Cell)->Option<RowEffect>
    decreases writes.len(),
{
    if writes.len()==0 { None } else {
        let rest=last_effect(writes.skip(1),key);
        if rest is Some { rest } else if writes[0].cell==key { Some(writes[0]) } else { None }
    }
}
pub proof fn batch_cell(before:RowState,writes:Seq<RowEffect>,key:Cell)
    ensures storage::value(e::batch_apply(before,writes).bytes,key)==match last_effect(writes,key) {
        Some(effect)=>effect.value,None=>storage::value(before.bytes,key),
    },
        last_effect(writes,key) is None ==> e::batch_apply(before,writes).versions.contains_key(key)
            ==before.versions.contains_key(key)
            && (before.versions.contains_key(key) ==> e::batch_apply(before,writes).versions[key]==before.versions[key]),
        last_effect(writes,key) is Some ==> e::batch_apply(before,writes).versions.contains_key(key)
            && e::batch_apply(before,writes).versions[key]==last_effect(writes,key).unwrap().version,
    decreases writes.len(),
{
    if writes.len()>0 {
        batch_cell(e::row_apply(before,writes[0]),writes.skip(1),key);
    }
}

pub open spec fn byte_images(rows:Map<int,RowState>)->source::EngineImages {
    Map::new(rows.dom(),|owner:int| rows[owner].bytes)
}

/// Byte write-set decoding, not a successor-state condition. Each model write
/// names exactly the last accepted native effect at its held owner; every other
/// observed cell is absent from that owner's accepted write set.
pub open spec fn write_set_embedding(s:p::State,txn:int,labels:Map<int,Cell>,
    writes:Map<int,Option<Seq<u8>>>,records:Map<int,TxnRecord>)->bool {
    writes.dom().subset_of(labels.dom()) && writes.dom().subset_of(s.sessions[txn].held.dom())
        && forall|q:(int,int)| s.physical.contains_key(q) && labels.contains_key(q.1) ==>
            records.contains_key(q.0) && if writes.contains_key(q.1) && s.sessions[txn].held[q.1].owner==q.0 {
                last_effect(records[q.0].writes,labels[q.1]) is Some
                    && last_effect(records[q.0].writes,labels[q.1]).unwrap().value==writes[q.1]
            } else { last_effect(records[q.0].writes,labels[q.1]) is None }
}

/// Distributed settlement composes per-owner primitive records with the SAME
/// committed decision. No primitive is asked whether migration is correct.
/// The invocation layer supplies the coordinated transaction's real write
/// records, including empty records at framed owners in the finite observation.
pub proof fn committed_engine_record(c:p::Constants,ctx:source::SourceContext,s:source::SourceState,id:TxnId,
    before:Map<int,RowState>,after:Map<int,RowState>,records:Map<int,TxnRecord>,terminal:Map<int,TxnRecord>,
    writes:Map<int,Option<Seq<u8>>>)->(record:source::EngineRecord)
    requires s.images==byte_images(before),!s.settled.contains_key(id),
        s.placement.sessions.contains_key(source::transaction(id)),
        !s.placement.sessions[source::transaction(id)].resolved,
        before.dom()==after.dom(),before.dom()==records.dom(),records.dom()==terminal.dom(),
        forall|owner:int| before.contains_key(owner) ==> !records[owner].settled
            && e::txn_effect(before[owner],records[owner],after[owner],terminal[owner])
            && terminal[owner].decision==TxnDecision::Commit,
        write_set_embedding(s.placement,source::transaction(id),ctx.labels,writes,records),
        source::engine_observed(s.placement,s.images,ctx.labels),
        forall|q:(int,int)| s.placement.physical.contains_key(q) && ctx.labels.contains_key(q.1) ==>
            before.contains_key(q.0),
        forall|k:int| writes.contains_key(k) ==> s.placement.logical.contains_key(k)
            && s.placement.physical.contains_key((s.placement.sessions[source::transaction(id)].held[k].owner,k)),
    ensures record==(source::EngineRecord::Settle{id,outcome:source::EngineOutcome::Committed,
            before:byte_images(before),after:byte_images(after),writes}),
        record.observed(ctx,s),
{
    let t=source::transaction(id);
    assert forall|q:(int,int)| s.placement.physical.contains_key(q) && ctx.labels.contains_key(q.1) implies
        storage::value(after[q.0].bytes,ctx.labels[q.1])==
            if writes.contains_key(q.1) && s.placement.sessions[t].held[q.1].owner==q.0 { writes[q.1] }
            else { storage::value(before[q.0].bytes,ctx.labels[q.1]) } by {
        batch_cell(before[q.0],records[q.0].writes,ctx.labels[q.1]);
    }
    assert(source::atomic_bytes(s.placement,t,byte_images(before),byte_images(after),ctx.labels,writes));
    assert(source::encoded_writes(writes).dom()==writes.dom());
    source::EngineRecord::Settle{id,outcome:source::EngineOutcome::Committed,
        before:byte_images(before),after:byte_images(after),writes}
}

/// The source-certified atomic byte event determines Resolve's values and
/// writer tags. The model's pending table is the projection of accepted writes.
pub proof fn settle_engine_record(c:p::Constants,ctx:source::SourceContext,src:source::SourceState,
    s:r::State,record:source::EngineRecord)
    requires r::inv(c,s),src.placement==s.core.placement,record.observed(ctx,src),
        record is Settle,
        s.pending.contains_key(source::transaction(record->Settle_id)),
        record->Settle_outcome is Committed ==>
            s.pending[source::transaction(record->Settle_id)]==source::encoded_writes(record->Settle_writes),
    ensures ({let txn=source::transaction(record->Settle_id);let committed=record->Settle_outcome is Committed;
        let action=r::Action::EngineSettle{txn,committed};let z=r::apply(c,s,action);
        r::enabled(c,s,action) && r::next(c,s,z) && r::inv(c,z)
            && z.core.placement==record.image(ctx,src).placement
            && z.settled.contains(txn) && !z.pending.contains_key(txn)
            && z.core.placement.sessions[txn].held==s.core.placement.sessions[txn].held}),
{
    let tracked native=record.certify(c,ctx,src);
    let txn=source::transaction(record->Settle_id);
    let committed=record->Settle_outcome is Committed;
    let action=r::Action::EngineSettle{txn,committed};
    let z=r::apply(c,s,action);
    if committed {
        let writes=record->Settle_writes;
        let after=record->Settle_after;
        assert forall|k:int| writes.contains_key(k) implies
            transfer::observed(after[s.core.placement.sessions[txn].held[k].owner],ctx.labels[k],txn)
                ==(p::Cell{value:source::encoded_writes(writes)[k],writer:txn}) by {}
        assert(z.core.placement.logical =~= record.image(ctx,src).placement.logical);
        assert(z.core.placement.physical =~= record.image(ctx,src).placement.physical);
    } else {
        assert(record->Settle_outcome is Aborted);
        assert(z.core.placement.logical =~= s.core.placement.logical);
        assert(z.core.placement.physical =~= s.core.placement.physical);
    }
    r::lemma_step(c,s,action);
    bridge::dispatch_edge(c,s,action);
}

pub open spec fn recovery_path(c:p::Constants,states:Seq<r::State>)->bool {
    states.len()>0 && forall|i:int| 0<=i<states.len()-1 ==> #[trigger] r::next(c,states[i],states[i+1])
}

/// Expand a finite set of actual dropped holds into durable Release entries.
/// Settlement itself never drops a hold, and no metadata journal is rewritten
/// by the preceding engine transaction.
pub proof fn release_sequence(c:p::Constants,s:r::State,txn:int,keys:Seq<int>)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),r::valid_authority(c,r::Authority::Transaction(txn)),
        r::active(s,r::Authority::Transaction(txn)),
        s.core.placement.sessions.contains_key(txn),s.core.placement.sessions[txn].resolved,
        keys.no_duplicates(),keys.to_set().subset_of(s.core.placement.sessions[txn].held.dom()),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),r::active(out.0,r::Authority::Transaction(txn)),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        out.0.core.placement==(p::State{sessions:s.core.placement.sessions.insert(txn,p::Session{
            held:s.core.placement.sessions[txn].held.remove_keys(keys.to_set()),resolved:true}),..s.core.placement}),
    decreases keys.len(),
{
    if keys.len()==0 {
        assert(keys.to_set() =~= Set::<int>::empty());
        assert(s.core.placement.sessions[txn].held.remove_keys(keys.to_set()) =~= s.core.placement.sessions[txn].held);
        assert(s.core.placement.sessions.insert(txn,s.core.placement.sessions[txn]) =~= s.core.placement.sessions);
        (s,seq![s])
    } else {
        let prefix=keys.drop_last();
        transfer::split_set(keys);
        assert(prefix.no_duplicates());
        assert(prefix.to_set().subset_of(keys.to_set()));
        assert(!prefix.to_set().contains(keys.last())) by {
            if prefix.to_set().contains(keys.last()) {
                let i=choose|i:int| 0<=i<prefix.len() && prefix[i]==keys.last();
                assert(keys[i]==keys[keys.len()-1]);
            }
        }
        let (mid,path)=release_sequence(c,s,txn,prefix);
        let action=p::Action::Release{txn,key:keys.last()};
        bridge::commit_authority(c,mid,action);
        let accepted=bridge::accepted(c,mid,action);
        let z=bridge::committed(c,mid,action);
        let states=path.push(accepted).push(z);
        assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
            if i<path.len()-1 {} else if i==path.len()-1 { assert(path.last()==mid); }
        }
        assert(z.core.placement.sessions[txn].held =~= s.core.placement.sessions[txn].held.remove_keys(keys.to_set()));
        assert(z.core.placement.sessions =~= s.core.placement.sessions.insert(txn,p::Session{
            held:s.core.placement.sessions[txn].held.remove_keys(keys.to_set()),resolved:true}));
        (z,states)
    }
}

/// Native finish's owner-local registry deletion supplies ALL release keys.
/// The actual EngineCompletion precedes this record in the source history; the
/// recovery settled marker comes from the exact primitive settlement above.
pub proof fn finish_release_path(c:p::Constants,ctx:source::SourceContext,src:source::SourceState,
    s:r::State,record:source::RegistryRecord,keys:Seq<int>)->(out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),src.placement==s.core.placement,
        src.registries_match(ctx),record.observed(ctx,src),record.call is Finish,record.status==Status::Ok,
        s.settled.contains(source::transaction(record.id)),
        r::active(s,r::Authority::Transaction(source::transaction(record.id))),
        keys.no_duplicates(),keys.to_set()==source::held_image(record.before.lease_view()[record.id.client],ctx.addresses()).dom(),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        out.0.core.placement==record.image(ctx,src).placement,
{
    let tracked native=record.certify(c,ctx,src);
    let txn=source::transaction(record.id);
    assert(s.core.placement.sessions[txn].resolved);
    assert(keys.to_set().subset_of(s.core.placement.sessions[txn].held.dom()));
    release_sequence(c,s,txn,keys)
}

/// Point release removes registrations, not all logical keys at the coordinate:
/// overlapping range registrations continue to hold their covered aliases.
pub proof fn point_release_subset(before:Map<u64,crate::leases::SessionView>,
    after:Map<u64,crate::leases::SessionView>,id:TxnId,table:u64,coordinate:Seq<u8>,status:Status,
    addresses:crate::leases_proofs::Addresses)
    requires crate::leases::release_effect(before,after,id,table,coordinate,status),
        before.contains_key(id.client),before[id.client].sequence==id.sequence,before[id.client].wf(),
        after.contains_key(id.client),after[id.client].wf(),
    ensures after[id.client].sequence==id.sequence,
        source::held_image(after[id.client],addresses).dom().subset_of(source::held_image(before[id.client],addresses).dom()),
        forall|k:int| source::held_image(after[id.client],addresses).contains_key(k) ==>
            source::held_image(after[id.client],addresses)[k]==source::held_image(before[id.client],addresses)[k],
{
    let b=before[id.client];let z=after[id.client];
    assert forall|k:int| source::held_image(z,addresses).contains_key(k) implies
        source::held_image(b,addresses).contains_key(k)
            && source::held_image(z,addresses)[k]==source::held_image(b,addresses)[k] by {
        let address=addresses[k];
        let i=choose|i:int| 0<=i<z.holds.len() && z.holds[i].contains(address.0,address.1);
        assert(z.holds.contains(z.holds[i]));
        assert(b.holds.contains(z.holds[i]));
        let j=choose|j:int| 0<=j<b.holds.len() && b.holds[j]==z.holds[i];
        assert(b.has(address.0,address.1,z.holds[i].grant));
        assert(z.has(address.0,address.1,z.holds[i].grant));
        crate::leases_proofs::unique_grant(b,address.0,address.1,z.holds[i].grant);
        crate::leases_proofs::unique_grant(z,address.0,address.1,z.holds[i].grant);
    }
}

/// The release keys are decoded from the actual native registry difference.
/// The globally fenced settlement is an earlier event, never inferred from a
/// local missing transaction record or from release's return status.
pub proof fn point_release_path(c:p::Constants,s:r::State,id:TxnId,owner:u32,
    before:Map<u64,crate::leases::SessionView>,after:Map<u64,crate::leases::SessionView>,
    table:u64,coordinate:Seq<u8>,status:Status,addresses:crate::leases_proofs::Addresses,keys:Seq<int>)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),r::active(s,r::Authority::Transaction(source::transaction(id))),
        s.settled.contains(source::transaction(id)),s.core.placement.sessions[source::transaction(id)].resolved,
        source::session_observed(before,s.core.placement,id,addresses,owner),
        crate::leases::release_effect(before,after,id,table,coordinate,status),
        before[id.client].wf(),after.contains_key(id.client),after[id.client].wf(),
        keys.no_duplicates(),keys.to_set()==source::held_image(before[id.client],addresses).dom()
            .difference(source::held_image(after[id.client],addresses).dom()),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),recovery_path(c,out.1),
        out.1.first()==s,out.1.last()==out.0,
        source::session_observed(after,out.0.core.placement,id,addresses,owner),
        forall|other:u32| other!=owner ==> source::owned_holds(out.0.core.placement.sessions[source::transaction(id)].held,other)
            ==source::owned_holds(s.core.placement.sessions[source::transaction(id)].held,other),
{
    point_release_subset(before,after,id,table,coordinate,status,addresses);
    let txn=source::transaction(id);
    assert(keys.to_set().subset_of(s.core.placement.sessions[txn].held.dom()));
    let out=release_sequence(c,s,txn,keys);
    assert(source::owned_holds(out.0.core.placement.sessions[txn].held,owner)
        =~=source::held_image(after[id.client],addresses));
    assert forall|other:u32| other!=owner implies
        source::owned_holds(out.0.core.placement.sessions[txn].held,other)
            ==source::owned_holds(s.core.placement.sessions[txn].held,other) by {
        assert(source::owned_holds(out.0.core.placement.sessions[txn].held,other)
            =~=source::owned_holds(s.core.placement.sessions[txn].held,other));
    }
    out
}

/// A raw effect in an uncovered Stage/Empty cell is private. Its old-or-whole
/// crash settlement changes neither placement nor any visible byte observation.
/// This is also the projection used while a complete mirror awaits Ready's
/// durable journal append. No rollback of a possibly durable row is assumed.
pub proof fn private_row_recovery(c:p::Constants,s:r::State,owner:int,
    images:source::EngineImages,labels:Map<int,Cell>,before:RowState,pending:Option<RowEffect>,after:RowState)
    requires source::engine_observed(s.core.placement,images,labels),
        images.contains_key(owner),images[owner]==before.bytes,
        e::settled_rows(before,pending,after),
        forall|k:int| labels.contains_key(k) && s.core.placement.physical.contains_key((owner,k))
            && pending is Some && labels[k]==pending.unwrap().cell ==>
            p::replica(s.core.placement,owner,k).role is Empty
                || (p::replica(s.core.placement,owner,k).role is Stage
                    && !p::replica(s.core.placement,owner,k).covered),
    ensures source::engine_observed(s.core.placement,images.insert(owner,after.bytes),labels),
        r::next(c,s,s),
        forall|k:Cell| pending is None || k!=pending.unwrap().cell ==>
            after.versions.contains_key(k)==before.versions.contains_key(k)
                && (before.versions.contains_key(k) ==> after.versions[k]==before.versions[k]),
{
    row_settlement_frame(before,pending,after);
    assert forall|q:(int,int)| s.core.placement.physical.contains_key(q) && labels.contains_key(q.1)
        && !(p::replica(s.core.placement,q.0,q.1).role is Empty)
        && (!(p::replica(s.core.placement,q.0,q.1).role is Stage)
            || p::replica(s.core.placement,q.0,q.1).covered) implies
        images.insert(owner,after.bytes).contains_key(q.0)
            && p::replica(s.core.placement,q.0,q.1).cell.value==
                crate::sharding_bytes::value_option(storage::value(images.insert(owner,after.bytes)[q.0],labels[q.1])) by {
        if q.0==owner {
            assert(pending is None || labels[q.1]!=pending.unwrap().cell);
        }
    }
    assert(r::dispatch(c,s,r::Action::Stutter)==s);
    assert(r::next(c,s,s));
}

/// Whole failed copy_rows calls now export execute_final's real ordered-prefix
/// and metadata frames. Preserve that witness while projecting private Stage
/// bytes to a recovery stutter: neither a partial handler nor Unknown creates
/// an unresolved abstract StartCopy. A pending primitive row is handled
/// separately by unknown_then_recover before any new admission.
pub proof fn failed_copy_private_prefix(c:p::Constants,s:r::State,
    before:&crate::participant::Participant,after:&crate::participant::Participant,
    plan:crate::types::MigrationPlan,images:source::EngineImages,labels:Map<int,Cell>,
    initial:storage::Image,current:storage::Image,source_image:storage::Image,
    result:crate::participant::ControlResult)->(cursor:Option<storage::Identity>)
    requires result.certificate.is_none(),after.local_unchanged(*before),after.same_metadata(*before),
        storage::prefix(current,initial,source_image,plan.range),
        storage::preserves_outside(current,initial,plan.range),
        images.contains_key(plan.destination as int),images[plan.destination as int]==initial,
        source::engine_observed(s.core.placement,images,labels),public_copy_boundary(s),
        forall|k:int| labels.contains_key(k) && storage::in_range(plan.range,labels[k])
            && s.core.placement.physical.contains_key((plan.destination as int,k)) ==>
            p::replica(s.core.placement,plan.destination as int,k).role is Stage
                && !p::replica(s.core.placement,plan.destination as int,k).covered,
    ensures storage::mirrored(current,initial,source_image,plan.range,cursor),
        source::engine_observed(s.core.placement,images.insert(plan.destination as int,current),labels),
        r::next(c,s,s),public_copy_boundary(r::apply(c,s,r::Action::Stutter)),
{
    let cursor=choose|cursor:Option<storage::Identity>| storage::mirrored(current,initial,source_image,plan.range,cursor);
    let owner=plan.destination as int;
    assert forall|q:(int,int)| s.core.placement.physical.contains_key(q) && labels.contains_key(q.1)
        && !(p::replica(s.core.placement,q.0,q.1).role is Empty)
        && (!(p::replica(s.core.placement,q.0,q.1).role is Stage)
            || p::replica(s.core.placement,q.0,q.1).covered) implies
        images.insert(owner,current).contains_key(q.0)
            && p::replica(s.core.placement,q.0,q.1).cell.value==
                crate::sharding_bytes::value_option(storage::value(images.insert(owner,current)[q.0],labels[q.1])) by {
        if q.0==owner { assert(!storage::in_range(plan.range,labels[q.1])); }
    }
    public_copy_step(c,s,r::Action::Stutter);
    assert(r::dispatch(c,s,r::Action::Stutter)==s);
    assert(r::next(c,s,s));
    cursor
}

/// Project the accepted write-set before settling it. The credential premises
/// are the independently installed admission fences, not a migration oracle;
/// can_resolve follows from the actual unresolved session and held write keys.
pub proof fn start_engine_record(c:p::Constants,s:r::State,id:TxnId,
    writes:Map<int,Option<Seq<u8>>>,targets:Map<int,(nat,nat)>)
    requires r::inv(c,s),r::active(s,r::Authority::Transaction(source::transaction(id))),
        !s.pending.contains_key(source::transaction(id)),
        s.core.placement.sessions.contains_key(source::transaction(id)),
        !s.core.placement.sessions[source::transaction(id)].resolved,
        writes.dom().subset_of(s.core.placement.sessions[source::transaction(id)].held.dom()),
        forall|k:int| s.core.placement.sessions[source::transaction(id)].held.contains_key(k) ==> {
            let owner=s.core.placement.sessions[source::transaction(id)].held[k].owner;
            r::active(s,r::Authority::Participant(owner)) && targets.contains_key(owner)
                && r::credential(s,r::Authority::Participant(owner),targets[owner].1,targets[owner].0)
        },
    ensures ({let txn=source::transaction(id);
        let action=r::Action::EngineStart{txn,writes:source::encoded_writes(writes),
            incarnation:r::head(s,r::Authority::Transaction(txn)).incarnation,targets};
        r::enabled(c,s,action) && r::next(c,s,r::apply(c,s,action))
            && r::inv(c,r::apply(c,s,action))
            && r::apply(c,s,action).pending[txn]==source::encoded_writes(writes)
            && r::apply(c,s,action).core==s.core}),
{
    let txn=source::transaction(id);
    assert(source::encoded_writes(writes).dom()==writes.dom());
    let action=r::Action::EngineStart{txn,writes:source::encoded_writes(writes),
        incarnation:r::head(s,r::Authority::Transaction(txn)).incarnation,targets};
    r::lemma_step(c,s,action);
    bridge::dispatch_edge(c,s,action);
}

/// Public crash/replay cuts occur outside the short StartCopy/DurableCopy
/// expansion. Failed raw mirrors are private-row stutters, not accepted
/// abstract asynchronous packets. Thus cleanup never needs to invent durable
/// completion for an abandoned, partially copied packet.
pub open spec fn public_copy_boundary(s:r::State)->bool {
    s.copying==s.copied
}
pub proof fn initial_public_copy_boundary(c:p::Constants)
    ensures public_copy_boundary(r::initial(c)),
{}
pub proof fn public_copy_step(c:p::Constants,s:r::State,action:r::Action)
    requires public_copy_boundary(s),!(action is StartCopy),!(action is DurableCopy),
    ensures public_copy_boundary(r::apply(c,s,action)),
{}
pub proof fn copy_publication_boundary(c:p::Constants,s:r::State,packet:p::Packet)
    requires public_copy_boundary(s),
    ensures public_copy_boundary(durable_copy(c,s,packet)),
        public_copy_boundary(bridge::committed(c,durable_copy(c,s,packet),p::Action::DeliverCopy{packet})),
{
    assert(durable_copy(c,s,packet).copying =~= durable_copy(c,s,packet).copied);
}
pub proof fn cleanup_at_public_copy_boundary(s:r::State,generation:nat,owner:int)
    requires public_copy_boundary(s),
    ensures forall|packet:p::Packet| s.copying.contains(packet) && packet.generation==generation
        && s.core.placement.plans[packet.generation].dst==owner ==> s.copied.contains(packet),
{}

/// Consume the exact postcondition of the real settle_transaction call (also
/// preserved by DurableParticipant::finish/release). No specification of a
/// correct migration or a desired post-state enters this primitive bridge.
pub proof fn native_settlement_projection<E:DurableEngine>(before:&E,after:&E,a:Admission,id:TxnId)
    requires e::transaction_settlement(*before,*after,a,id),
    ensures primitive_settlement(before.durable_state(),after.durable_state(),before.transactions(),after.transactions(),id),
        primitive_settlement(before.working_state(),after.working_state(),before.transactions(),after.transactions(),id),
        after.owner()==before.owner(),after.installed()==before.installed(),after.pending_row()==before.pending_row(),
        !before.transactions().contains_key(id) && e::settled(after.transactions(),id) ==>
            after.transactions()[id].decision==TxnDecision::Abort
                && after.transactions()[id].writes==Seq::<RowEffect>::empty()
                && after.durable_state()==before.durable_state()
                && after.working_state()==before.working_state(),
{
    if !before.transactions().contains_key(id) && e::settled(after.transactions(),id) {
        exact_transaction_settlement(before.durable_state(),after.durable_state(),before.transactions(),after.transactions(),id);
    }
}

/// The lease-frame premise is exported by BOTH actual wrapper bodies, not by a
/// backend trait. An unresolved settlement cannot change even one point/range
/// registration; consequently its finite held-key projection also stays exact.
pub proof fn unresolved_native_leases<E:DurableEngine>(before:&DurableParticipant,after:&DurableParticipant,
    engine:&E,id:TxnId,addresses:crate::leases_proofs::Addresses)
    requires !e::settled(engine.transactions(),id),
        !e::settled(engine.transactions(),id) ==> after.native().lease_view()==before.native().lease_view(),
        before.native().lease_view().contains_key(id.client),
    ensures after.native().lease_view()==before.native().lease_view(),
        source::held_image(after.native().lease_view()[id.client],addresses)
            ==source::held_image(before.native().lease_view()[id.client],addresses),
{}

/// External TP lifecycle evidence at the metadata operation's serialization
/// point. Dispatch/application must hold this authorization through the call;
/// an earlier Boolean check or one owner's absent record is insufficient.
/// Native begin/acquire do NOT themselves consult the global TP catalogue.
pub enum TpPhase { Enrolling, Settled }
pub struct TpEnrollment { pub id:TxnId,pub phase:TpPhase }
pub open spec fn tp_correspondence(s:r::State,permit:TpEnrollment)->bool {
    (permit.phase is Settled)==s.settled.contains(source::transaction(permit.id))
}
pub proof fn tp_enrollment_unresolved(c:p::Constants,s:r::State,permit:TpEnrollment)
    requires r::inv(c,s),tp_correspondence(s,permit),permit.phase is Enrolling,
    ensures s.core.placement.sessions.contains_key(source::transaction(permit.id)) ==>
        !s.core.placement.sessions[source::transaction(permit.id)].resolved,
{}

/// An authority-local committed byte index, never an arbitrary logger event.
pub proof fn committed_lease_step<K:crate::durable_kv::DurableKv>(node:&DurableParticipant,kv:&K,index:int)
    -> (step:crate::durable_participant::replay_proofs::NativeStep)
    requires node.wf(),node.consistent(kv),kv.wf(),0<index<node.records().len(),
    ensures step==node.replay_history().steps[index-1],step.valid(),
        codec::wire(step.operation)==node.records()[index],
{
    hide(NativeStep::valid);hide(codec::wire);
    node.history_facts();
    node.replay_history().layout(node.records(),node.namespace().owner);
    node.replay_history().entry(node.records(),node.namespace().owner,index-1);
    node.replay_history().steps[index-1]
}

proof fn registry_hold_witness(registries:Map<u32,Map<u64,crate::leases::SessionView>>,
    s:p::State,addresses:crate::leases_proofs::Addresses,t:int,k:int)->(pair:(u32,u64))
    requires source::registries_observed(registries,s,addresses),
        s.sessions.contains_key(t),s.sessions[t].held.contains_key(k),
    ensures registries.contains_key(pair.0),registries[pair.0].contains_key(pair.1),
        source::transaction(source::registry_id(pair.1,registries[pair.0][pair.1]))==t,
        source::held_image(registries[pair.0][pair.1],addresses).contains_key(k),
        source::held_image(registries[pair.0][pair.1],addresses)[k]==s.sessions[t].held[k],
        s.sessions[t].held[k].owner==pair.0 as int,
{
    let owner=choose|owner:u32| #[trigger] registries.contains_key(owner) && exists|client:u64| registries[owner].contains_key(client)
        && source::transaction(source::registry_id(client,registries[owner][client]))==t
        && source::held_image(registries[owner][client],addresses).contains_key(k)
        && source::held_image(registries[owner][client],addresses)[k]==s.sessions[t].held[k]
        && s.sessions[t].held[k].owner==owner as int;
    let client=choose|client:u64| registries.contains_key(owner) && registries[owner].contains_key(client)
        && source::transaction(source::registry_id(client,registries[owner][client]))==t
        && source::held_image(registries[owner][client],addresses).contains_key(k)
        && source::held_image(registries[owner][client],addresses)[k]==s.sessions[t].held[k]
        && s.sessions[t].held[k].owner==owner as int;
    (owner,client)
}

/// Transaction authorities are ghost projections of the union of owner-local
/// lease journals. They are NOT a third kind of native Namespace. This theorem
/// exposes their exact reconstructed image through the existing bidirectional
/// owner-registry relation; the global TxnId is supplied separately by TP.
pub proof fn virtual_lease_image(c:p::Constants,s:r::State,ctx:source::SourceContext,
    src:source::SourceState,id:TxnId)
    requires r::inv(c,s),src.placement==s.core.placement,src.registries_match(ctx),
        s.core.placement.sessions.contains_key(source::transaction(id)),
    ensures r::recovered_leases(s,source::transaction(id))==Some(s.core.placement.sessions[source::transaction(id)].held),
        forall|k:int| s.core.placement.sessions[source::transaction(id)].held.contains_key(k) ==>
            exists|owner:u32,client:u64| src.registries.contains_key(owner) && src.registries[owner].contains_key(client)
                && source::registry_id(client,src.registries[owner][client])==id
                && source::held_image(src.registries[owner][client],ctx.addresses()).contains_key(k)
                && source::held_image(src.registries[owner][client],ctx.addresses())[k]
                    ==s.core.placement.sessions[source::transaction(id)].held[k],
{
    let txn=source::transaction(id);
    assert(r::durable_inv(s,r::Authority::Transaction(txn)));
    assert forall|k:int| s.core.placement.sessions[txn].held.contains_key(k) implies
        exists|owner:u32,client:u64| src.registries.contains_key(owner) && src.registries[owner].contains_key(client)
            && source::registry_id(client,src.registries[owner][client])==id
            && source::held_image(src.registries[owner][client],ctx.addresses()).contains_key(k)
            && source::held_image(src.registries[owner][client],ctx.addresses())[k]
                ==s.core.placement.sessions[txn].held[k] by {
        let pair=registry_hold_witness(src.registries,s.core.placement,ctx.addresses(),txn,k);
        crate::migration_refinement::nonce_injective(source::registry_id(pair.1,src.registries[pair.0][pair.1]),id);
        assert(source::registry_id(pair.1,src.registries[pair.0][pair.1])==id);
    }
}

proof fn transaction_snapshot_path(c:p::Constants,s:r::State,id:TxnId)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        out.0==r::witnesses::snapshot_start(c,s,r::Authority::Transaction(source::transaction(id))),
{
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);hide(r::next);
    crate::migration_refinement::nonce_injective(id,id);
    let a=r::Authority::Transaction(source::transaction(id));
    r::witnesses::snapshot_witness(c,s,a);
    let crash=r::Action::Crash{authority:a};
    let crashed=r::apply(c,s,crash);
    r::lemma_step(c,s,crash);bridge::slots_step(c,s,crash);public_copy_step(c,s,crash);
    let boot=r::Action::Boot{authority:a};
    let booted=r::apply(c,crashed,boot);
    r::lemma_step(c,crashed,boot);bridge::slots_step(c,crashed,boot);public_copy_step(c,crashed,boot);
    let snapshot=r::Action::InstallSnapshot{authority:a,index:(s.journals[a].len()-1) as nat};
    let loaded=r::apply(c,booted,snapshot);
    r::lemma_step(c,booted,snapshot);bridge::slots_step(c,booted,snapshot);public_copy_step(c,booted,snapshot);
    bridge::dispatch_edge(c,s,crash);bridge::dispatch_edge(c,crashed,boot);
    bridge::dispatch_edge(c,booted,snapshot);
    let states=seq![s,crashed,booted,loaded];
    assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
        if i==0 {} else if i==1 {} else { assert(i==2); }
    }
    (loaded,states)
}

proof fn transaction_reconstructed_path(c:p::Constants,s:r::State,id:TxnId)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        out.0==r::witnesses::reconstructed_runtime(c,s,r::Authority::Transaction(source::transaction(id))),
{
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);hide(r::next);
    hide(r::witnesses::snapshot_start);hide(r::witnesses::replay_n);
    crate::migration_refinement::nonce_injective(id,id);
    let a=r::Authority::Transaction(source::transaction(id));
    r::authority_facts(c,s,a);
    assert(s.engines[a].len()==0);
    assert(r::witnesses::suffix_length(s,a)==0);
    r::witnesses::reconstruction_witness(c,s,a);
    let (loaded,path)=transaction_snapshot_path(c,s,id);
    assert(r::witnesses::replay_n(c,loaded,a,0)==loaded) by { reveal(r::witnesses::replay_n); }
    let complete=r::Action::Complete{authority:a};
    let recovered=r::apply(c,loaded,complete);
    r::lemma_step(c,loaded,complete);bridge::slots_step(c,loaded,complete);public_copy_step(c,loaded,complete);
    bridge::dispatch_edge(c,loaded,complete);
    let states=path.push(recovered);
    assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
        if i<path.len()-1 {} else { assert(i==path.len()-1 && path.last()==loaded); }
    }
    (recovered,states)
}

/// Materialize/reconstruct the virtual transaction authority rather than
/// assuming it was independently admitted by owner KV. Its engine suffix is
/// empty by recovery::durable_inv. Only the proof journal/cache is changed;
/// participant admission, bytes, and all owner-local registries are framed.
pub proof fn transaction_authority_path(c:p::Constants,s:r::State,id:TxnId)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        out.0==r::witnesses::bootstrap(c,s,r::Authority::Transaction(source::transaction(id))),
        out.0.core==s.core,r::active(out.0,r::Authority::Transaction(source::transaction(id))),
        forall|owner:int| r::active(out.0,r::Authority::Participant(owner))==r::active(s,r::Authority::Participant(owner)),
{
    hide(r::next);
    hide(r::inv);hide(p::inv);hide(bridge::old_slots);
    hide(r::witnesses::snapshot_start);hide(r::witnesses::reconstructed_runtime);hide(r::witnesses::replay_n);
    crate::migration_refinement::nonce_injective(id,id);
    let a=r::Authority::Transaction(source::transaction(id));
    r::witnesses::bootstrap_witness(c,s,a);
    let (recovered,path)=transaction_reconstructed_path(c,s,id);
    let admit=r::Action::Admit{authority:a};
    let admitted=r::apply(c,recovered,admit);
    r::lemma_step(c,recovered,admit);bridge::slots_step(c,recovered,admit);public_copy_step(c,recovered,admit);
    let activate=r::Action::Activate{authority:a};
    let active=r::apply(c,admitted,activate);
    r::lemma_step(c,admitted,activate);bridge::slots_step(c,admitted,activate);public_copy_step(c,admitted,activate);
    bridge::dispatch_edge(c,recovered,admit);bridge::dispatch_edge(c,admitted,activate);
    let states=path.push(admitted).push(active);
    assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
        if i<path.len()-1 {} else if i==path.len()-1 { assert(path.last()==recovered); }
    }
    (active,states)
}

proof fn native_begin_record(c:p::Constants,s:r::State,ctx:source::SourceContext,src:source::SourceState,
    step:crate::durable_participant::replay_proofs::NativeStep,permit:TpEnrollment)
    -> (record:source::RegistryRecord)
    requires r::inv(c,s),src.placement==s.core.placement,src.registries_match(ctx),step.valid(),
        step.operation is Begin,permit.id==step.operation->Begin_id,
        tp_correspondence(s,permit),permit.phase is Enrolling,
        src.registries.contains_key(step.before.owner_view()),
        src.registries[step.before.owner_view()]==step.before.lease_view(),
        !crate::durable_participant::replay_proofs::origin_rejected(
            step.origins_before,step.operation->Begin_id,step.operation->Begin_incarnation),
    ensures record==(source::RegistryRecord{before:step.before,after:step.after,id:permit.id,
            status:step.status,call:source::RegistryCall::Begin}),
        record.observed(ctx,src),
{
    tp_enrollment_unresolved(c,s,permit);
    let id=permit.id;let owner=step.before.owner_view();let b=step.before.lease_view();let z=step.after.lease_view();
    assert(crate::leases::begin_effect(b,z,id,step.status));
    if b!=z && s.core.placement.sessions.contains_key(source::transaction(id)) {
        assert forall|k:int| source::owned_holds(s.core.placement.sessions[source::transaction(id)].held,owner).contains_key(k)
            implies false by {
            let pair=registry_hold_witness(src.registries,s.core.placement,ctx.addresses(),source::transaction(id),k);
            assert(pair.0==owner);
            crate::migration_refinement::nonce_injective(source::registry_id(pair.1,src.registries[pair.0][pair.1]),id);
            assert(pair.1==id.client && b[id.client].sequence==id.sequence);
            assert(b==z);
        }
        assert(source::owned_holds(s.core.placement.sessions[source::transaction(id)].held,owner).dom()
            =~=Set::<int>::empty());
    }
    if b==z && step.status==Status::Ok {
        assert(b.contains_key(id.client) && b[id.client].sequence==id.sequence);
        assert(source::session_observed(b,src.placement,id,ctx.addresses(),owner));
    }
    source::RegistryRecord{before:step.before,after:step.after,id,status:step.status,call:source::RegistryCall::Begin}
}

/// First-owner Begin registers Open through the virtual authority's real model
/// recovery path. Later owners only update their local registry projection.
/// Unknown owner journal replies do not enter this function until their exact
/// committed prefix has been selected; no lease is dropped by this bridge.
pub proof fn native_begin_path<K:crate::durable_kv::DurableKv>(c:p::Constants,s:r::State,
    ctx:source::SourceContext,src:source::SourceState,node:&DurableParticipant,kv:&K,index:int,permit:TpEnrollment)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
        src.placement==s.core.placement,src.registries_match(ctx),
        node.wf(),kv.wf(),node.consistent(kv),0<index<node.records().len(),
        ({let step=node.replay_history().steps[index-1];
            step.operation is Begin && permit.id==step.operation->Begin_id
                && src.registries.contains_key(step.before.owner_view())
                && src.registries[step.before.owner_view()]==step.before.lease_view()}),
        tp_correspondence(s,permit),permit.phase is Enrolling,
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        ({let step=node.replay_history().steps[index-1];
            source::registries_observed(src.registries.insert(step.before.owner_view(),step.after.lease_view()),
                out.0.core.placement,ctx.addresses())}),
        forall|txn:int| s.core.placement.sessions.contains_key(txn) ==>
            out.0.core.placement.sessions[txn].held==s.core.placement.sessions[txn].held,
{
    let step=committed_lease_step(node,kv,index);
    let id=permit.id;
    if crate::durable_participant::replay_proofs::origin_rejected(step.origins_before,id,step.operation->Begin_incarnation) {
        assert(step.after.lease_view()==step.before.lease_view());
        assert(src.registries.insert(step.before.owner_view(),step.after.lease_view()) =~=src.registries);
        (s,seq![s])
    } else {
        let record=native_begin_record(c,s,ctx,src,step,permit);
        let tracked native=record.certify(c,ctx,src);
        if step.status!=Status::Ok || s.core.placement.sessions.contains_key(source::transaction(id)) {
            assert(record.image(ctx,src).placement==s.core.placement);
            (s,seq![s])
        } else {
            let (active,path)=transaction_authority_path(c,s,id);
            let action=p::Action::Open{txn:source::transaction(id)};
            crate::migration_refinement::nonce_injective(id,id);
            bridge::commit_authority(c,active,action);
            let accepted=bridge::accepted(c,active,action);
            let z=bridge::committed(c,active,action);
            public_copy_step(c,active,r::Action::Accept{envelope:r::envelope(active,action)});
            public_copy_step(c,accepted,r::Action::Commit{envelope:r::envelope(active,action)});
            assert(z.core.placement==record.image(ctx,src).placement);
            let states=path.push(accepted).push(z);
            assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
                if i<path.len()-1 {} else if i==path.len()-1 { assert(path.last()==active); }
            }
            (z,states)
        }
    }
}

proof fn native_admission_record(c:p::Constants,s:r::State,ctx:source::SourceContext,src:source::SourceState,
    step:crate::durable_participant::replay_proofs::NativeStep,permit:TpEnrollment)
    -> (record:source::AdmissionRecord)
    requires r::inv(c,s),src.placement==s.core.placement,src.registries_match(ctx),step.valid(),
        step.operation is Acquire,permit.id==step.operation->Acquire_id,
        tp_correspondence(s,permit),permit.phase is Enrolling,
        step.after.lease_view()!=step.before.lease_view(),
        src.registries.contains_key(step.before.owner_view()),
        src.registries[step.before.owner_view()]==step.before.lease_view(),
        forall|k:int| ctx.addresses().contains_key(k) ==>
            s.core.placement.physical.contains_key((step.before.owner_view() as int,k))
                && step.before.local_meta(ctx.addresses()[k].0,ctx.addresses()[k].1).is_some()
                && crate::participant_proofs::metadata(step.before.local_meta(ctx.addresses()[k].0,ctx.addresses()[k].1).unwrap(),
                    p::replica(s.core.placement,step.before.owner_view() as int,k)),
    ensures record.before==step.before,record.after==step.after,record.id==permit.id,record.status==step.status,
        record.grant==step.operation->Acquire_grant,record.observed(ctx,src),
{
    step.before.lease_facts();step.after.lease_facts();
    tp_enrollment_unresolved(c,s,permit);
    let id=permit.id;let range=step.operation->Acquire_range;let grant=step.operation->Acquire_grant;
    let call=if step.operation->Acquire_point {
        source::AdmissionCall::Point{table:range.table,coordinate:range.lo@}
    } else { source::AdmissionCall::Range{table:range.table,lo:range.lo@,hi:crate::leases::range_hi(range)} };
    let b=step.before.lease_view();let z=step.after.lease_view();
    let scope=if step.operation->Acquire_point {
        assert(crate::leases::acquire_effect(b,z,id,range.table,range.lo@,grant,step.status));
        crate::leases::point_scope(range.table,range.lo@,grant)
    } else {
        assert(crate::leases::acquire_range_effect(b,z,id,range.table,range.lo@,crate::leases::range_hi(range),grant,step.status));
        crate::leases::range_scope(range.table,range.lo@,crate::leases::range_hi(range),grant)
    };
    crate::leases_proofs::register_guard_and_frame(b,z,id,scope,step.status);
    assert(step.status==Status::Ok);
    assert(step.before.lease_view().contains_key(id.client) && step.before.lease_view()[id.client].sequence==id.sequence);
    assert(source::session_observed(step.before.lease_view(),src.placement,id,ctx.addresses(),step.before.owner_view()));
    source::AdmissionRecord{before:step.before,after:step.after,id,grant,status:step.status,call}
}

pub open spec fn admission_scope(record:source::AdmissionRecord)->crate::leases::ScopeView {
    match record.call {
        source::AdmissionCall::Point{table,coordinate}=>crate::leases::point_scope(table,coordinate,record.grant),
        source::AdmissionCall::Range{table,lo,hi}=>crate::leases::range_scope(table,lo,hi,record.grant),
    }
}
proof fn admission_key_accounting(ctx:source::SourceContext,src:source::SourceState,
    record:source::AdmissionRecord,key:int,grant:crate::types::Grant)
    requires record.observed(ctx,src),record.after.lease_view()!=record.before.lease_view(),
        ctx.addresses().contains_key(key),
    ensures ({
        let b=crate::leases_proofs::expanded(record.before.lease_view()[record.id.client],ctx.addresses());
        let z=crate::leases_proofs::expanded(record.after.lease_view()[record.id.client],ctx.addresses());
        (z.contains_key(key) && z[key]==grant) <==> (b.contains_key(key) && b[key]==grant)
            || (admission_scope(record).contains(ctx.addresses()[key].0,ctx.addresses()[key].1) && grant==record.grant)
    }),
{
    let b=record.before.lease_view();let z=record.after.lease_view();
    assert(record.status==Status::Ok);
    match record.call {
        source::AdmissionCall::Point{table,coordinate}=>{
            crate::leases_proofs::acquire_expansion(b,z,record.id,table,coordinate,record.grant,ctx.addresses(),key,grant);
        },
        source::AdmissionCall::Range{table,lo,hi}=>{
            crate::leases_proofs::acquire_range_expansion(b,z,record.id,table,lo,hi,record.grant,ctx.addresses(),key,grant);
        },
    }
}

/// Decode native point/range registrations. Existing aliases keep their exact
/// grants; newly covered logical keys derive Serving/ownership_version from actual native
/// checks, including absent rows and all coordinate aliases.
proof fn admission_projection(c:p::Constants,ctx:source::SourceContext,src:source::SourceState,
    record:source::AdmissionRecord)
    requires p::inv(c,src.placement),record.observed(ctx,src),
        record.after.lease_view()!=record.before.lease_view(),
    ensures ({let target=source::held_image(record.after.lease_view()[record.id.client],ctx.addresses());
        let held=src.placement.sessions[source::transaction(record.id)].held;
        (forall|k:int| target.contains_key(k) && held.contains_key(k) ==> target[k]==held[k])
            && (forall|k:int| target.contains_key(k) && !held.contains_key(k) ==>
                target[k].owner==record.before.owner_view() as int
                    && src.placement.physical.contains_key((target[k].owner,k))
                    && p::replica(src.placement,target[k].owner,k).role is Serving
                    && p::replica(src.placement,target[k].owner,k).ownership_version==target[k].ownership_version)
    }),
{
    let addresses=ctx.addresses();let owner=record.before.owner_view();let t=source::transaction(record.id);
    let before=crate::leases_proofs::expanded(record.before.lease_view()[record.id.client],addresses);
    let after=crate::leases_proofs::expanded(record.after.lease_view()[record.id.client],addresses);
    let target=source::held_image(record.after.lease_view()[record.id.client],addresses);
    let held=src.placement.sessions[t].held;
    assert(record.status==Status::Ok && record.grant.owner==owner);
    assert forall|k:int| target.contains_key(k) implies
        (held.contains_key(k) ==> target[k]==held[k])
            && (!held.contains_key(k) ==> target[k].owner==owner as int
                && src.placement.physical.contains_key((target[k].owner,k))
                && p::replica(src.placement,target[k].owner,k).role is Serving
                && p::replica(src.placement,target[k].owner,k).ownership_version==target[k].ownership_version) by {
        assert(addresses.contains_key(k) && after.contains_key(k));
        assert(target[k]==crate::migration_refinement::grant(after[k]));
        assert(src.placement.physical.contains_key((owner as int,k)));
        assert(c.keys.contains(k) && c.shards.contains(owner as int));
        admission_key_accounting(ctx,src,record,k,after[k]);
        if before.contains_key(k) {
            admission_key_accounting(ctx,src,record,k,before[k]);
            assert(after[k]==before[k]);
            assert(source::held_image(record.before.lease_view()[record.id.client],addresses).contains_key(k));
            assert(source::owned_holds(held,owner).contains_key(k));
            assert(held.contains_key(k));
            assert(target[k]==source::held_image(record.before.lease_view()[record.id.client],addresses)[k]);
            assert(target[k]==held[k]);
        } else {
            assert(after[k]==record.grant);
            assert(admission_scope(record).contains(addresses[k].0,addresses[k].1));
            match record.call {
                source::AdmissionCall::Point{table,coordinate}=>{
                    assert(addresses[k]==(table,coordinate));
                    assert(!record.before.open_hold(record.id,table,coordinate,record.grant));
                },
                source::AdmissionCall::Range{table,lo,hi}=>{
                    assert(!record.before.open_range(record.id,table,lo,hi,record.grant));
                    assert(crate::directory::inside(addresses[k].1,lo,hi));
                },
            }
            assert(record.before.local_meta(addresses[k].0,addresses[k].1).unwrap().role==crate::types::Role::Serving);
            assert(p::replica(src.placement,owner as int,k).role is Serving);
            assert(target[k].owner==owner as int);
            assert(record.before.local_meta(addresses[k].0,addresses[k].1).unwrap().ownership_version==record.grant.ownership_version);
            assert(p::replica(src.placement,owner as int,k).ownership_version==target[k].ownership_version);
            if held.contains_key(k) {
                assert(p::key_inv(c,src.placement,k));
                assert(p::session_inv(c,src.placement,t));
                assert(p::directory(src.placement)[k].owner==owner as int);
                assert(held[k].owner==owner as int);
                assert(source::owned_holds(held,owner).contains_key(k));
                assert(before.contains_key(k));
            }
        }
    }
}

proof fn acquire_sequence(c:p::Constants,s:r::State,id:TxnId,owner:u32,target:Map<int,p::Grant>,work:Seq<int>)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
        r::active(s,r::Authority::Transaction(source::transaction(id))),r::active(s,r::Authority::Participant(owner as int)),
        s.core.placement.sessions.contains_key(source::transaction(id)),!s.core.placement.sessions[source::transaction(id)].resolved,
        work.no_duplicates(),work.to_set().subset_of(target.dom()),
        work.to_set().disjoint(s.core.placement.sessions[source::transaction(id)].held.dom()),
        forall|k:int| work.to_set().contains(k) ==> target[k].owner==owner as int
            && s.core.placement.physical.contains_key((target[k].owner,k))
            && p::replica(s.core.placement,target[k].owner,k).role is Serving
            && p::replica(s.core.placement,target[k].owner,k).ownership_version==target[k].ownership_version,
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        r::active(out.0,r::Authority::Transaction(source::transaction(id))),
        r::active(out.0,r::Authority::Participant(owner as int)),
        out.0.core.placement==(p::State{sessions:s.core.placement.sessions.insert(source::transaction(id),p::Session{
            held:source::overlay(s.core.placement.sessions[source::transaction(id)].held,target,work.to_set()),resolved:false}),
            ..s.core.placement}),
    decreases work.len(),
{
    hide(r::next);
    let t=source::transaction(id);
    if work.len()==0 {
        assert(work.to_set() =~= Set::<int>::empty());
        assert(source::overlay(s.core.placement.sessions[t].held,target,work.to_set()) =~=s.core.placement.sessions[t].held);
        assert(s.core.placement.sessions.insert(t,s.core.placement.sessions[t]) =~=s.core.placement.sessions);
        (s,seq![s])
    } else {
        transfer::split_set(work);let prefix=work.drop_last();let key=work.last();
        assert(prefix.no_duplicates());
        assert(prefix.to_set().subset_of(work.to_set()));
        assert(!prefix.to_set().contains(key)) by {
            if prefix.to_set().contains(key) {
                let i=choose|i:int| 0<=i<prefix.len() && prefix[i]==key;
                assert(work[i]==work[work.len()-1]);
            }
        }
        let (mid,path)=acquire_sequence(c,s,id,owner,target,prefix);
        assert(work.to_set().contains(key));
        assert(target.contains_key(key) && target[key].owner==owner as int);
        assert(!s.core.placement.sessions[t].held.contains_key(key));
        assert(!mid.core.placement.sessions[t].held.contains_key(key));
        assert(mid.core.placement.physical==s.core.placement.physical);
        assert(!mid.core.placement.sessions[t].resolved);
        let action=p::Action::Acquire{txn:t,key,grant:target[key]};
        r::authority_facts(c,mid,r::Authority::Participant(owner as int));
        assert(r::credential(mid,r::Authority::Participant(owner as int),
            r::head(mid,r::Authority::Participant(owner as int)).incarnation,
            r::member(mid,r::Authority::Participant(owner as int))));
        assert(p::admission(mid.core.placement,t,key,target[key]));
        assert(p::enabled(c,mid.core.placement,action));
        assert(r::admission_guard(mid,r::envelope(mid,action)));
        bridge::commit_authority(c,mid,action);
        let accepted=bridge::accepted(c,mid,action);let z=bridge::committed(c,mid,action);
        public_copy_step(c,mid,r::Action::Accept{envelope:r::envelope(mid,action)});
        public_copy_step(c,accepted,r::Action::Commit{envelope:r::envelope(mid,action)});
        assert(z.core.placement.sessions[t].held
            =~=source::overlay(s.core.placement.sessions[t].held,target,work.to_set()));
        assert(z.core.placement.sessions =~=s.core.placement.sessions.insert(t,p::Session{
            held:source::overlay(s.core.placement.sessions[t].held,target,work.to_set()),resolved:false}));
        let states=path.push(accepted).push(z);
        assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
            if i<path.len()-1 {} else if i==path.len()-1 { assert(path.last()==mid); }
        }
        (z,states)
    }
}

/// Canonical point/range acquisition from an actual committed participant
/// NativeStep. TP authorization is held through this native call; owner KV
/// supplies only committed byte provenance and its own incarnation fence.
pub proof fn native_acquire_path<K:crate::durable_kv::DurableKv>(c:p::Constants,s:r::State,
    ctx:source::SourceContext,src:source::SourceState,node:&DurableParticipant,kv:&K,index:int,
    permit:TpEnrollment,keys:Seq<int>)->(out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
        src.placement==s.core.placement,src.registries_match(ctx),
        node.wf(),kv.wf(),node.consistent(kv),0<index<node.records().len(),
        tp_correspondence(s,permit),permit.phase is Enrolling,
        ({let step=node.replay_history().steps[index-1];let owner=step.before.owner_view();
            step.operation is Acquire && permit.id==step.operation->Acquire_id
                && src.registries.contains_key(owner) && src.registries[owner]==step.before.lease_view()
                && r::active(s,r::Authority::Participant(owner as int))
                && (forall|k:int| ctx.addresses().contains_key(k) ==> s.core.placement.physical.contains_key((owner as int,k))
                    && step.before.local_meta(ctx.addresses()[k].0,ctx.addresses()[k].1).is_some()
                    && crate::participant_proofs::metadata(step.before.local_meta(ctx.addresses()[k].0,ctx.addresses()[k].1).unwrap(),
                        p::replica(s.core.placement,owner as int,k)))
                && (step.after.lease_view()!=step.before.lease_view() ==> keys.no_duplicates()
                    && keys.to_set()==source::held_image(step.after.lease_view()[permit.id.client],ctx.addresses()).dom()
                        .difference(s.core.placement.sessions[source::transaction(permit.id)].held.dom()))
        }),
    ensures r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        recovery_path(c,out.1),out.1.first()==s,out.1.last()==out.0,
        ({let step=node.replay_history().steps[index-1];
            source::registries_observed(src.registries.insert(step.before.owner_view(),step.after.lease_view()),
                out.0.core.placement,ctx.addresses())}),
{
    let step=committed_lease_step(node,kv,index);
    if step.before.lease_view()==step.after.lease_view() {
        assert(src.registries.insert(step.before.owner_view(),step.after.lease_view()) =~=src.registries);
        (s,seq![s])
    } else {
        let record=native_admission_record(c,s,ctx,src,step,permit);
        let tracked native=record.certify(c,ctx,src);
        admission_projection(c,ctx,src,record);
        let target=source::held_image(step.after.lease_view()[permit.id.client],ctx.addresses());
        let (active,prefix)=transaction_authority_path(c,s,permit.id);
        let (z,suffix)=acquire_sequence(c,active,permit.id,step.before.owner_view(),target,keys);
        assert(source::overlay(s.core.placement.sessions[source::transaction(permit.id)].held,target,keys.to_set())
            =~=source::overlay(s.core.placement.sessions[source::transaction(permit.id)].held,target,target.dom()));
        assert(z.core.placement==record.image(ctx,src).placement);
        let states=prefix+suffix.drop_first();
        assert forall|i:int| 0<=i<states.len()-1 implies #[trigger] r::next(c,states[i],states[i+1]) by {
            if i<prefix.len()-1 {} else if i==prefix.len()-1 {
                assert(prefix.last()==active && suffix.first()==active);
            } else { assert(states[i]==suffix[i-prefix.len()+1]); }
        }
        (z,states)
    }
}

proof fn held_image_frame(before:crate::leases::SessionView,after:crate::leases::SessionView,
    addresses:crate::leases_proofs::Addresses)
    requires before.wf(),after.wf(),before.holds==after.holds,
    ensures source::held_image(before,addresses)==source::held_image(after,addresses),
{
    let b=crate::leases_proofs::expanded(before,addresses);
    let z=crate::leases_proofs::expanded(after,addresses);
    assert forall|k:int| b.contains_key(k) || z.contains_key(k) implies
        b.contains_key(k) && z.contains_key(k) && b[k]==z[k] by {
        assert(addresses.contains_key(k));
        let grant=if b.contains_key(k){b[k]}else{z[k]};
        crate::leases_proofs::expanded_exact(before,addresses,k,grant);
        crate::leases_proofs::expanded_exact(after,addresses,k,grant);
        assert(before.has(addresses[k].0,addresses[k].1,grant)==after.has(addresses[k].0,addresses[k].1,grant));
    }
    assert(b =~= z);
    assert(source::held_image(before,addresses) =~=source::held_image(after,addresses));
}

/// The native operation named Resolve closes this owner's enrollment bit. It
/// does not report engine settlement, does not release a hold, and therefore
/// cannot map to placement::Resolve or recovery::EngineSettle.
pub proof fn native_resolve_path<K:crate::durable_kv::DurableKv>(c:p::Constants,s:r::State,
    ctx:source::SourceContext,src:source::SourceState,node:&DurableParticipant,kv:&K,index:int)
    -> (out:(r::State,Seq<r::State>))
    requires r::inv(c,s),bridge::old_slots(s),public_copy_boundary(s),
        src.placement==s.core.placement,src.registries_match(ctx),
        node.wf(),kv.wf(),node.consistent(kv),0<index<node.records().len(),
        ({let step=node.replay_history().steps[index-1];
            step.operation is Resolve && src.registries.contains_key(step.before.owner_view())
                && src.registries[step.before.owner_view()]==step.before.lease_view()}),
    ensures out.0==s,out.1==seq![s,s],recovery_path(c,out.1),
        r::inv(c,out.0),bridge::old_slots(out.0),public_copy_boundary(out.0),
        ({let step=node.replay_history().steps[index-1];
            source::registries_observed(src.registries.insert(step.before.owner_view(),step.after.lease_view()),
                s.core.placement,ctx.addresses())}),
{
    let step=committed_lease_step(node,kv,index);
    step.before.lease_facts();step.after.lease_facts();
    let owner=step.before.owner_view();let b=step.before.lease_view();let z=step.after.lease_view();
    assert(crate::leases::resolve_effect(b,z,step.operation->Resolve_0,step.status));
    assert(b.dom()==z.dom());
    assert forall|client:u64| b.contains_key(client) implies
        b[client].sequence==z[client].sequence && b[client].holds==z[client].holds
            && source::held_image(b[client],ctx.addresses())==source::held_image(z[client],ctx.addresses()) by {
        assert(b[client].holds==z[client].holds);
        held_image_frame(b[client],z[client],ctx.addresses());
    }
    let registries=src.registries.insert(owner,z);
    assert forall|o:u32,client:u64| registries.contains_key(o) && registries[o].contains_key(client) implies
        source::session_observed(registries[o],s.core.placement,
            source::registry_id(client,registries[o][client]),ctx.addresses(),o) by {
        if o==owner {
            assert(source::registry_id(client,z[client])==source::registry_id(client,b[client]));
            assert(source::session_observed(b,s.core.placement,source::registry_id(client,b[client]),ctx.addresses(),owner));
        }
    }
    assert forall|t:int,k:int| s.core.placement.sessions.contains_key(t) && s.core.placement.sessions[t].held.contains_key(k) implies
        exists|o:u32,client:u64| registries.contains_key(o) && registries[o].contains_key(client)
            && source::transaction(source::registry_id(client,registries[o][client]))==t
            && source::held_image(registries[o][client],ctx.addresses()).contains_key(k)
            && source::held_image(registries[o][client],ctx.addresses())[k]==s.core.placement.sessions[t].held[k]
            && s.core.placement.sessions[t].held[k].owner==o as int by {
        let pair=registry_hold_witness(src.registries,s.core.placement,ctx.addresses(),t,k);
        if pair.0==owner {
            assert(source::registry_id(pair.1,z[pair.1])==source::registry_id(pair.1,b[pair.1]));
            assert(source::held_image(z[pair.1],ctx.addresses())==source::held_image(b[pair.1],ctx.addresses()));
        }
        assert(registries.contains_key(pair.0) && registries[pair.0].contains_key(pair.1));
    }
    assert(r::dispatch(c,s,r::Action::Stutter)==s);
    bridge::dispatch_edge(c,s,r::Action::Stutter);
    (s,seq![s,s])
}

} // verus!
