//! Native metadata and durable cleanup publication refine recovery actions.
//! The private cleanup witness retains the actual pre-delivery observation; it
//! is erased evidence, never a recovered-state input. In particular native Empty
//! is not identified with the recovery model's terminal Deliver until deletion
//! and the completion record have both committed.
use vstd::prelude::*;
use crate::types::{Command,MigrationPlan,ReplicaMeta,Role};
use crate::participant::Participant;
use crate::participant_proofs as metadata;
use crate::execution_refinement as source;
use crate::transfer_proofs as transfer;
use crate::sharding_placement as p;
use crate::sharding_recovery as r;
use crate::durable_recovery_refinement as bridge;
use crate::durable_participant::codec::{self,Operation};
use crate::durable_participant::replay_proofs::{ActorHistory,NativeStep};
use crate::durable_participant::history::{self,CompletionFact};
use crate::durable_kv as kv;
use crate::storage;

verus! {
pub open spec fn delivery(record:source::MetadataRecord)->p::Action {
    p::Action::Deliver { generation:record.plan.generation as nat,
        command:metadata::command(record.command),owner:record.before.owner_view() as int }
}
pub open spec fn destructive(record:source::MetadataRecord)->bool {
    record.command==Command::Commit && record.before.owner_view()==record.plan.source
        || record.command==Command::Abort && record.before.owner_view()==record.plan.destination
}
/// The operation and native endpoints come from the executable replay history.
/// Authentication/finite observation remain the existing SourceContext boundary.
pub open spec fn binds_delivery(step:NativeStep,record:source::MetadataRecord)->bool {
    step.before==record.before && step.after==record.after && step.status==record.result.status
        && step.operation==Operation::Deliver{plan:record.plan,command:record.command}
}
pub open spec fn recorded_step(trace:ActorHistory,records:Seq<Seq<u8>>,owner:u32,index:int,step:NativeStep)->bool {
    trace.valid(records,owner) && 0<=index<trace.steps.len() && trace.steps[index]==step
}

pub proof fn recorded_bytes(trace:ActorHistory,records:Seq<Seq<u8>>,owner:u32,index:int,step:NativeStep)
    requires recorded_step(trace,records,owner,index,step),
    ensures records[index+1]==codec::wire(step.operation),
{
    hide(codec::wire); hide(codec::encodable); hide(NativeStep::valid);
    hide(Participant::semantic_eq);
    assert(trace.steps[index]==step);
    trace.entry(records,owner,index);
}
pub proof fn recovery_edge(c:p::Constants,s:r::State,action:r::Action)
    requires r::enabled(c,s,action),
    ensures r::next(c,s,r::apply(c,s,action)),
{
    assert(r::dispatch(c,s,action)==r::apply(c,s,action));
}
pub proof fn recovery_stutter(c:p::Constants,s:r::State)
    ensures r::next(c,s,s),
{
    assert(r::dispatch(c,s,r::Action::Stutter)==s);
}

/// Extract the actual command step from deliver's executable postcondition.
/// Callers construct MetadataRecord from these endpoints and the retained
/// authenticated observation window; no desired recovery successor is supplied.
pub proof fn wrapper_delivery_step(before:&crate::durable_participant::DurableParticipant,
    after:&crate::durable_participant::DurableParticipant,generation:u64,command:Command,
    result:crate::participant::ControlResult)->(out:(MigrationPlan,NativeStep))
    requires after.wf(),after.delivered_result(*before,generation,command,result),
    ensures out.0.generation==generation,
        out.1.operation==(Operation::Deliver{plan:out.0,command}),out.1.status==result.status,
        out.1.before.semantic_eq(before.native()),out.1.after.semantic_eq(after.native()),
        recorded_step(after.replay_history(),after.records(),after.namespace().owner,
            after.replay_history().steps.len() as int-1,out.1),
        after.records()==before.records().push(codec::wire(out.1.operation)),
{
    hide(codec::wire); hide(codec::encodable); hide(NativeStep::valid);
    hide(Participant::semantic_eq);
    after.history_facts();
    let (plan,step)=choose|plan:MigrationPlan,step:NativeStep|plan.generation==generation
        && step.operation==Operation::Deliver{plan,command} && step.status==result.status && step.valid()
        && step.before.semantic_eq(before.native()) && step.after.semantic_eq(after.native())
        && after.records()==before.records().push(codec::wire(step.operation))
        && after.replay_history()==before.replay_history().append(step);
    (plan,step)
}

/// Derive guards from the real command_at interval effects, not from the desired
/// successor or an engine-supplied migration predicate.
pub proof fn metadata_dispatch(c:p::Constants,ctx:source::SourceContext,s:source::SourceState,
    record:source::MetadataRecord)
    requires ctx.wf(c),p::inv(c,s.placement),s.registries_match(ctx),record.observed(ctx,s),record.fresh(),
    ensures p::enabled(c,s.placement,delivery(record)),
        p::local_guard(s.placement,record.plan.generation as nat,metadata::command(record.command),record.before.owner_view() as int),
        record.image(ctx,s).placement==p::apply(c,s.placement,delivery(record)),
        transfer::local_coupling(&record.after,record.plan,record.image(ctx,s).placement,ctx.window(record.keys)),
        r::actor(s.placement,delivery(record))==r::Authority::Participant(record.before.owner_view() as int),
        r::destructive(s.placement,record.plan.generation as nat,metadata::command(record.command),record.before.owner_view() as int)==destructive(record),
{
    let plan=record.plan;
    let labels=ctx.window(record.keys);
    if record.command==Command::Retire { ctx.drained(c,s,&record.before,plan,record.keys); }
    let coordinates=|k:int|labels[k].1.0;
    assert forall|k:int|s.placement.plans[plan.generation as nat].keys.contains(k) implies
        record.before.command_at(plan,record.command,record.before.drained_view(plan.range),coordinates(k))
        && metadata::metadata(record.before.local_meta(plan.range.table,coordinates(k)).unwrap(),
            p::replica(s.placement,record.before.owner_view() as int,k))
        && s.placement.plans[plan.generation as nat].old[k].ownership_version==crate::directory::route(plan.old@,coordinates(k)).ownership_version by {
        assert(storage::in_range(plan.range,labels[k]));
    }
    metadata::native_guard_matches(&record.before,plan,record.command,s.placement,coordinates);
    let tracked result=source::metadata_delivered(c,&record.before,&record.after,plan,record.command,
        &record.result,s.placement,record.keys,labels);
}

pub proof fn native_metadata_commit(c:p::Constants,ctx:source::SourceContext,source:source::SourceState,
    s:r::State,record:source::MetadataRecord,trace:ActorHistory,records:Seq<Seq<u8>>,index:int,
    step:NativeStep,bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,incarnation:u64)
    requires ctx.wf(c),source.registries_match(ctx),r::inv(c,s),bridge::old_slots(s),
        source.placement==s.core.placement,record.observed(ctx,source),record.fresh(),!destructive(record),
        recorded_step(trace,records,ns.owner,index,step),binds_delivery(step,record),
        !ns.coordinator,ns.owner==record.before.owner_view(),kv::has_prefix(bytes,ns,incarnation,records),
        incarnation==r::head(s,r::Authority::Participant(ns.owner as int)).incarnation,
        r::active(s,r::Authority::Participant(ns.owner as int)),
    ensures r::next(c,s,bridge::accepted(c,s,delivery(record))),
        r::next(c,bridge::accepted(c,s,delivery(record)),bridge::committed(c,s,delivery(record))),
        r::inv(c,bridge::committed(c,s,delivery(record))),bridge::old_slots(bridge::committed(c,s,delivery(record))),
        bridge::committed(c,s,delivery(record)).core.placement==record.image(ctx,source).placement,
        records[index+1]==codec::wire(Operation::Deliver{plan:record.plan,command:record.command}),
{
    hide(codec::wire); hide(kv::has_prefix); hide(NativeStep::valid);
    recorded_bytes(trace,records,ns.owner,index,step);
    metadata_dispatch(c,ctx,source,record);
    bridge::commit_authority(c,s,delivery(record));
}

/// This is a projection witness for an actual durable command, not a second
/// transition machine. It stores only immutable source evidence. Native rows
/// remain private while the recovery core still exposes the pre-command replica.
pub struct CleanupProjection {
    pub record:source::MetadataRecord,
    pub context:source::SourceContext,
    pub source:source::SourceState,
}
impl CleanupProjection {
    pub open spec fn valid(self,c:p::Constants)->bool {
        self.context.wf(c) && p::inv(c,self.source.placement) && self.source.registries_match(self.context)
            && self.record.observed(self.context,self.source) && self.record.fresh() && destructive(self.record)
    }
    pub open spec fn pending(self,c:p::Constants,native:Participant,recovery:r::State)->bool {
        self.valid(c) && recovery.core.placement==self.source.placement && native.local_unchanged(self.record.after)
    }
    pub open spec fn projected_meta(self,native:Participant,table:u64,key:Seq<u8>)->Option<ReplicaMeta> {
        if crate::bytes::contains_spec(self.record.plan.range,table,key) {
            self.record.before.local_meta(table,key)
        } else { native.local_meta(table,key) }
    }
    pub open spec fn start(self)->r::Action {
        r::Action::StartCleanup{generation:self.record.plan.generation as nat,
            owner:self.record.before.owner_view() as int,command:metadata::command(self.record.command)}
    }
    pub open spec fn finish(self)->r::Action {
        r::Action::DurableCleanup{generation:self.record.plan.generation as nat,
            owner:self.record.before.owner_view() as int,command:metadata::command(self.record.command)}
    }
}

pub proof fn native_empty_is_private(c:p::Constants,w:CleanupProjection,native:Participant,s:r::State)
    requires w.pending(c,native,s),
    ensures forall|table:u64,key:Seq<u8>|w.projected_meta(native,table,key)==w.record.before.local_meta(table,key),
        forall|key:Seq<u8>|crate::bytes::contains_spec(w.record.plan.range,w.record.plan.range.table,key) ==>
            native.local_meta(w.record.plan.range.table,key).is_some()
            && native.local_meta(w.record.plan.range.table,key).unwrap().role==Role::Empty
            && native.local_meta(w.record.plan.range.table,key).unwrap().terminal
            && native.local_meta(w.record.plan.range.table,key).unwrap().fence==w.record.plan.generation,
{
    let record=w.record;
    assert forall|table:u64,key:Seq<u8>|w.projected_meta(native,table,key)==record.before.local_meta(table,key) by {}
    assert forall|key:Seq<u8>|crate::bytes::contains_spec(record.plan.range,record.plan.range.table,key) implies
        native.local_meta(record.plan.range.table,key).is_some()
        && native.local_meta(record.plan.range.table,key).unwrap().role==Role::Empty
        && native.local_meta(record.plan.range.table,key).unwrap().terminal
        && native.local_meta(record.plan.range.table,key).unwrap().fence==record.plan.generation by {
        assert(record.before.command_at(record.plan,record.command,record.before.drained_view(record.plan.range),key));
    }
}

/// A destructive metadata append is a private stutter, even when the following
/// deletion fails. It must never publish the semantic Done from metadata_image.
pub proof fn native_cleanup_prefix(c:p::Constants,w:CleanupProjection,s:r::State,trace:ActorHistory,
    records:Seq<Seq<u8>>,index:int,step:NativeStep,bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,incarnation:u64)
    requires w.valid(c),s.core.placement==w.source.placement,
        recorded_step(trace,records,ns.owner,index,step),binds_delivery(step,w.record),
        !ns.coordinator,ns.owner==w.record.before.owner_view(),kv::has_prefix(bytes,ns,incarnation,records),
    ensures w.pending(c,step.after,s),r::next(c,s,s),
        records[index+1]==codec::wire(Operation::Deliver{plan:w.record.plan,command:w.record.command}),
        forall|table:u64,key:Seq<u8>|w.projected_meta(step.after,table,key)==step.before.local_meta(table,key),
{
    hide(codec::wire); hide(codec::encodable); hide(kv::has_prefix); hide(NativeStep::valid);
    recorded_bytes(trace,records,ns.owner,index,step);
    native_empty_is_private(c,w,step.after,s);
    recovery_stutter(c,s);
}

/// Replay cannot turn a private native Empty into an acknowledged abstract
/// cleanup. The recovered native endpoint is reconstructed from committed bytes;
/// the old projection witness is used only in this erased proof.
pub proof fn recovered_private_cleanup(c:p::Constants,w:CleanupProjection,s:r::State,
    node:&crate::durable_participant::DurableParticipant,prior:ActorHistory,
    bytes:Map<Seq<u8>,Seq<u8>>,incarnation:u64)
    requires r::inv(c,s),node.wf(),node.replay_equivalent(),
        prior.valid(node.records(),node.namespace().owner),w.pending(c,prior.endpoint(),s),
        kv::has_prefix(bytes,node.namespace(),incarnation,node.records()),
        c.shards.contains(node.namespace().owner as int),s.core.members.contains_key(node.namespace().owner as int),
    ensures w.pending(c,node.native(),s),
        r::witnesses::bootstrap_enabled(c,s,r::Authority::Participant(node.namespace().owner as int)),
        w.pending(c,node.native(),r::witnesses::bootstrap(c,s,r::Authority::Participant(node.namespace().owner as int))),
        forall|table:u64,key:Seq<u8>|w.projected_meta(node.native(),table,key)==w.record.before.local_meta(table,key),
{
    hide(codec::wire); hide(codec::encodable); hide(kv::has_prefix); hide(NativeStep::valid);
    node.replay_refinement();
    bridge::participant_histories_unique(prior,node.replay_history(),node.records(),node.namespace().owner);
    assert(prior.endpoint().semantic_eq(node.native()));
    r::witnesses::bootstrap_summary(c,s,r::Authority::Participant(node.namespace().owner as int));
    native_empty_is_private(c,w,node.native(),s);
}

/// Reservations are admitted only on known commitment. A committed-prefix
/// witness alone cannot grant I/O permission after an Unknown acknowledgement.
pub open spec fn admitted_reservation(result:kv::CommitResult,step:NativeStep,plan:MigrationPlan)->bool {
    result==kv::CommitResult::Committed && step.valid()
        && step.operation==Operation::Reserve{plan,cleanup:true}
}
pub proof fn unknown_reservation_is_not_admission(result:kv::CommitResult,step:NativeStep,plan:MigrationPlan)
    requires result!=kv::CommitResult::Committed,
    ensures !admitted_reservation(result,step,plan),
{}

pub proof fn native_reservation_starts_cleanup(c:p::Constants,w:CleanupProjection,s:r::State,
    trace:ActorHistory,records:Seq<Seq<u8>>,index:int,step:NativeStep,
    bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,incarnation:u64,result:kv::CommitResult)
    requires r::inv(c,s),bridge::old_slots(s),w.pending(c,step.before,s),
        recorded_step(trace,records,ns.owner,index,step),admitted_reservation(result,step,w.record.plan),
        !ns.coordinator,ns.owner==w.record.before.owner_view(),kv::has_prefix(bytes,ns,incarnation,records),
        incarnation==r::head(s,r::Authority::Participant(ns.owner as int)).incarnation,
        r::active(s,r::Authority::Participant(ns.owner as int)),
    ensures r::enabled(c,s,w.start()),r::next(c,s,r::apply(c,s,w.start())),
        r::inv(c,r::apply(c,s,w.start())),bridge::old_slots(r::apply(c,s,w.start())),
        w.pending(c,step.after,r::apply(c,s,w.start())),
        r::apply(c,s,w.start()).cleaning.contains((w.record.plan.generation as nat,ns.owner as int,metadata::command(w.record.command))),
        step.before.transfer_authorized(w.record.plan,true),
        records[index+1]==codec::wire(Operation::Reserve{plan:w.record.plan,cleanup:true}),
{
    hide(codec::wire); hide(codec::encodable); hide(kv::has_prefix); hide(NativeStep::valid);
    reservation_native_frame(step,w.record.plan);
    recorded_bytes(trace,records,ns.owner,index,step);
    metadata_dispatch(c,w.context,w.source,w.record);
    r::lemma_step(c,s,w.start());
    bridge::slots_step(c,s,w.start());
    recovery_edge(c,s,w.start());
}

/// The real durable replay language admits no metadata/lease operation between
/// a cleanup reservation and its completion or engine-settlement fence.
pub proof fn reservation_blocks_generation_reuse(trace:ActorHistory,records:Seq<Seq<u8>>,owner:u32,
    index:int,plan:MigrationPlan)
    requires trace.valid(records,owner),0<=index< trace.steps.len(),
        crate::durable_participant::replay_proofs::pending(trace.steps.take(index))==Some((plan,true)),
    ensures trace.steps[index].after.local_unchanged(trace.steps[index].before),
        match trace.steps[index].operation {
            Operation::Complete{plan:completed,cleanup}=>cleanup && storage::same_plan(plan,completed),
            Operation::Settled=>true,
            _=>false,
        },
{
    hide(codec::wire); hide(codec::encodable); hide(NativeStep::valid);
    trace.entry(records,owner,index);
    reservation_successor_frame(trace.steps.take(index),trace.steps[index],plan);
    assert(crate::durable_participant::replay_proofs::allowed(trace.steps.take(index),trace.steps[index].operation));
}

/// Completed row effects for this owner must be fenced before deletion is
/// acknowledged. This is scheduling/engine evidence, never inferred merely from
/// an empty range: an outstanding old copy could otherwise resurrect a row.
pub open spec fn copies_settled(s:r::State,generation:nat,owner:int)->bool {
    forall|packet:p::Packet|s.copying.contains(packet) && packet.generation==generation
        && s.core.placement.plans[packet.generation].dst==owner ==> s.copied.contains(packet)
}

/// The copy bridge derives this boundary invariant by publishing StartCopy and
/// DurableCopy together only after a concrete durable copy completion. Failed
/// raw prefixes remain private; no engine supplies this invariant as an oracle.
pub proof fn public_copy_boundary_settles_cleanup(s:r::State,generation:nat,owner:int)
    requires crate::durable_engine_refinement::public_copy_boundary(s),
    ensures copies_settled(s,generation,owner),
{ crate::durable_engine_refinement::cleanup_at_public_copy_boundary(s,generation,owner); }

pub proof fn source_cleanup_has_no_copy_writers(c:p::Constants,w:CleanupProjection,s:r::State)
    requires w.valid(c),s.core.placement==w.source.placement,w.record.before.owner_view()==w.record.plan.source,
    ensures copies_settled(s,w.record.plan.generation as nat,w.record.before.owner_view() as int),
{
    assert(w.record.plan.source!=w.record.plan.destination);
    assert(s.core.placement.plans[w.record.plan.generation as nat].dst==w.record.plan.destination);
}

/// Concrete completion bytes select their exact CompletionFact and preceding
/// reservation. Neither a boolean 'cleaned' nor an assumed migration image is
/// accepted as a replacement for that record provenance.
pub proof fn committed_cleanup_rows(records:Seq<Seq<u8>>,owner:u32,facts:Map<int,CompletionFact>,index:int,
    plan:MigrationPlan)
    requires history::history_valid(records,owner,facts),0<=index<records.len(),
        records[index]==codec::wire(Operation::Complete{plan,cleanup:true}),
    ensures facts.contains_key(index),facts[index].valid(),facts[index].cleanup,facts[index].owner==owner,
        storage::same_plan(plan,facts[index].plan),index>0,
        records[index-1]==codec::wire(Operation::Reserve{plan:facts[index].plan,cleanup:true}),
        forall|cell:storage::Cell|storage::in_range(plan.range,cell) ==> storage::value(facts[index].durable,cell)==None,
        facts[index].after.local_unchanged(facts[index].before),
        facts[index].before.owner_view()==owner,facts[index].after.owner_view()==owner,
        history::is_completion(records[index]),
{
    hide(codec::wire);
    reveal(CompletionFact::valid);
    reveal(history::history_valid);
    assert(history::is_completion(records[index]));
    assert(facts.contains_key(index));
    assert(facts[index].valid() && facts[index].owner==owner);
    assert(records[index]==codec::wire(Operation::Complete{plan:facts[index].plan,cleanup:facts[index].cleanup}));
    assert(index>0 && records[index-1]==codec::wire(Operation::Reserve{plan:facts[index].plan,cleanup:facts[index].cleanup}));
    history::completion_unique(plan,true,facts[index].plan,facts[index].cleanup);
    let fact=facts[index];
    assert(fact.source==Map::<storage::Cell,Seq<u8>>::empty());
    assert(storage::complete(fact.durable,fact.initial,fact.source,fact.plan.range));
    assert forall|cell:storage::Cell|storage::in_range(plan.range,cell) implies storage::value(fact.durable,cell)==None by {
        assert(storage::in_range(fact.plan.range,cell));
        assert(storage::value(fact.durable,cell)==storage::value(fact.source,cell));
    }
}

/// execute_cleanup's successful certificate branch supplies completion_record.
/// Its witness is selected from the wrapper's concrete records, never from a
/// caller-provided finished migration or a receipt bit alone.
pub proof fn done_has_durable_empty(node:&crate::durable_participant::DurableParticipant,generation:u64)->(index:int)
    requires node.wf(),node.completion_record(generation,true),
    ensures 0<index<node.records().len(),node.completion_facts().contains_key(index),
        node.completion_facts()[index].valid(),node.completion_facts()[index].cleanup,
        node.completion_facts()[index].plan.generation==generation,
        node.records()[index]==codec::wire(Operation::Complete{plan:node.completion_facts()[index].plan,cleanup:true}),
        node.records()[index-1]==codec::wire(Operation::Reserve{plan:node.completion_facts()[index].plan,cleanup:true}),
        forall|cell:storage::Cell|storage::in_range(node.completion_facts()[index].plan.range,cell) ==>
            storage::value(node.completion_facts()[index].durable,cell)==None,
{
    hide(codec::wire); hide(codec::encodable); hide(NativeStep::valid);
    node.history_facts();
    let i=node.completion_witness(generation,true);
    committed_cleanup_rows(node.records(),node.namespace().owner,node.completion_facts(),i,node.completion_facts()[i].plan);
    i
}

/// This antecedent is the exact engine-frame postcondition of execute_cleanup,
/// not a new backend assumption. No acknowledged append means no engine call,
/// including when a reservation may commit later after an Unknown return.
pub proof fn unacknowledged_reservation_has_no_io<E:crate::durable_engine::DurableEngine>(
    before:crate::durable_participant::DurableParticipant,after:crate::durable_participant::DurableParticipant,
    engine_before:E,engine_after:E,generation:u64)
    requires engine_after!=engine_before ==> after.acknowledged_reservation(before,generation,true),
        after.records().len()<=before.records().len(),
    ensures engine_after==engine_before,!after.acknowledged_reservation(before,generation,true),
{}

pub proof fn emitted_done_has_durable_record(before:crate::durable_participant::DurableParticipant,
    after:&crate::durable_participant::DurableParticipant,generation:u64,
    result:crate::participant::ControlResult)->(index:int)
    requires after.wf(),after.transfer_publication(before,generation,true,result),result.certificate.is_some(),
    ensures 0<index<after.records().len(),after.completion_facts().contains_key(index),
        after.completion_facts()[index].valid(),after.completion_facts()[index].cleanup,
        after.completion_facts()[index].plan.generation==generation,
        result.certificate==Some(if after.namespace().owner==after.completion_facts()[index].plan.source {
            crate::types::Certificate::SourceDone
        }else{crate::types::Certificate::DestinationDone}),
        after.records()[index]==codec::wire(Operation::Complete{plan:after.completion_facts()[index].plan,cleanup:true}),
        after.records()[index-1]==codec::wire(Operation::Reserve{plan:after.completion_facts()[index].plan,cleanup:true}),
        forall|cell:storage::Cell|storage::in_range(after.completion_facts()[index].plan.range,cell) ==>
            storage::value(after.completion_facts()[index].durable,cell)==None,
{
    hide(codec::wire); hide(codec::encodable); hide(NativeStep::valid);
    after.history_facts();
    let i=choose|i:int|0<=i<after.records().len() && after.completion_facts().contains_key(i)
        && after.completion_facts()[i].plan.generation==generation && after.completion_facts()[i].cleanup
        && after.records()[i]==codec::wire(Operation::Complete{plan:after.completion_facts()[i].plan,cleanup:true})
        && result.certificate==Some(if after.namespace().owner==after.completion_facts()[i].plan.source {
            crate::types::Certificate::SourceDone
        }else{crate::types::Certificate::DestinationDone});
    committed_cleanup_rows(after.records(),after.namespace().owner,after.completion_facts(),i,after.completion_facts()[i].plan);
    i
}

/// The hidden Empty interval cannot still serve an abstract lease, and the
/// coordinator cannot start another generation while this command is private.
/// These facts come from the existing recovery invariant and the native local
/// guard, not from treating native Empty as already destructively delivered.
pub proof fn private_cleanup_has_no_authority(c:p::Constants,w:CleanupProjection,s:r::State,
    native:Participant,key:int)
    requires r::inv(c,s),w.pending(c,native,s),
        r::active(s,r::Authority::Participant(w.record.before.owner_view() as int)),
        w.record.keys.to_set().contains(key),
    ensures s.core.placement.active==Some(w.record.plan.generation as nat),
        p::directory(s.core.placement)[key].owner!=native.owner_view() as int,
        forall|txn:int|s.core.placement.sessions.contains_key(txn)
            && s.core.placement.sessions[txn].held.contains_key(key) ==>
                s.core.placement.sessions[txn].held[key].owner!=native.owner_view() as int,
        forall|nonce:int,src:int,dst:int,table:int,lo:int,hi:Option<int>|
            !p::enabled(c,s.core.placement,p::Action::Begin{nonce,src,dst,table,lo,hi}),
{
    metadata_dispatch(c,w.context,w.source,w.record);
    assert(r::enabled(c,s,w.start()));
    r::theorem_cleanup_authorization(c,s,w.record.plan.generation as nat,
        w.record.before.owner_view() as int,metadata::command(w.record.command),key);
}

pub open spec fn cleanup_published(c:p::Constants,s:r::State,w:CleanupProjection)->r::State {
    bridge::committed(c,r::apply(c,s,w.finish()),delivery(w.record))
}

/// The positive terminal path is DurableCleanup, Accept, Commit. Its guard is
/// reconstructed from the retained native command observation and its empty
/// durable image is obtained from the actual indexed completion record.
pub proof fn native_cleanup_published(c:p::Constants,w:CleanupProjection,s:r::State,
    trace:ActorHistory,records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,index:int,
    bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,incarnation:u64)
    requires r::inv(c,s),bridge::old_slots(s),w.valid(c),s.core.placement==w.source.placement,
        !ns.coordinator,ns.owner==w.record.before.owner_view(),kv::has_prefix(bytes,ns,incarnation,records),
        trace.valid(records,ns.owner),history::history_valid(records,ns.owner,facts),
        history::facts_match(records,facts,trace),
        0<=index<records.len(),records[index]==codec::wire(Operation::Complete{plan:w.record.plan,cleanup:true}),
        trace.steps[index-1].before.local_unchanged(w.record.after),
        incarnation==r::head(s,r::Authority::Participant(ns.owner as int)).incarnation,
        r::active(s,r::Authority::Participant(ns.owner as int)),
        s.cleaning.contains((w.record.plan.generation as nat,ns.owner as int,metadata::command(w.record.command))),
        !s.cleaned.contains((w.record.plan.generation as nat,ns.owner as int,metadata::command(w.record.command))),
        ns.owner!=w.record.plan.source ==> crate::durable_engine_refinement::public_copy_boundary(s),
    ensures r::enabled(c,s,w.finish()),r::next(c,s,r::apply(c,s,w.finish())),
        r::next(c,r::apply(c,s,w.finish()),bridge::accepted(c,r::apply(c,s,w.finish()),delivery(w.record))),
        r::next(c,bridge::accepted(c,r::apply(c,s,w.finish()),delivery(w.record)),cleanup_published(c,s,w)),
        r::inv(c,cleanup_published(c,s,w)),bridge::old_slots(cleanup_published(c,s,w)),
        cleanup_published(c,s,w).core.placement==w.record.image(w.context,w.source).placement,
        transfer::local_coupling(&facts[index].after,w.record.plan,cleanup_published(c,s,w).core.placement,w.context.window(w.record.keys)),
        forall|cell:storage::Cell|storage::in_range(w.record.plan.range,cell) ==> storage::value(facts[index].durable,cell)==None,
        cleanup_published(c,s,w).core.placement.certificates.contains((w.record.plan.generation as nat,
            if ns.owner==w.record.plan.source {p::Certificate::SourceDone}else{p::Certificate::DestinationDone})),
{
    hide(codec::wire); hide(kv::has_prefix); hide(NativeStep::valid);
    hide(codec::encodable);
    committed_cleanup_rows(records,ns.owner,facts,index,w.record.plan);
    assert(history::is_completion(records[index]));
    history::completion_at(records,ns.owner,facts,trace,index);
    metadata_dispatch(c,w.context,w.source,w.record);
    if ns.owner==w.record.plan.source { source_cleanup_has_no_copy_writers(c,w,s); }
    else { public_copy_boundary_settles_cleanup(s,w.record.plan.generation as nat,ns.owner as int); }
    assert(r::enabled(c,s,w.finish()));
    r::lemma_step(c,s,w.finish());
    bridge::slots_step(c,s,w.finish());
    recovery_edge(c,s,w.finish());
    let durable=r::apply(c,s,w.finish());
    assert(r::effect_guard(durable,r::envelope(durable,delivery(w.record))));
    bridge::commit_authority(c,durable,delivery(w.record));
    assert(facts[index].before.semantic_eq(trace.steps[index-1].before));
    let labels=w.context.window(w.record.keys);
    assert forall|key:int|s.core.placement.plans[w.record.plan.generation as nat].keys.contains(key) implies
        facts[index].after.local_meta(labels[key].0,labels[key].1.0).is_some()
        && metadata::metadata(facts[index].after.local_meta(labels[key].0,labels[key].1.0).unwrap(),
            p::replica(cleanup_published(c,s,w).core.placement,facts[index].after.owner_view() as int,key)) by {
        assert(facts[index].after.local_meta(labels[key].0,labels[key].1.0)
            ==facts[index].before.local_meta(labels[key].0,labels[key].1.0));
        assert(facts[index].before.local_meta(labels[key].0,labels[key].1.0)
            ==trace.steps[index-1].before.local_meta(labels[key].0,labels[key].1.0));
        assert(facts[index].after.local_meta(labels[key].0,labels[key].1.0)
            ==w.record.after.local_meta(labels[key].0,labels[key].1.0));
    }
}

/// Every recovery edge emitted by the native cleanup bridge preserves the
/// public-boundary invariant. Together with the copy bridge's paired publication
/// theorem this discharges destination Abort's quiescence compositionally.
pub proof fn cleanup_preserves_public_copy_boundary(c:p::Constants,s:r::State,w:CleanupProjection)
    requires crate::durable_engine_refinement::public_copy_boundary(s),
    ensures crate::durable_engine_refinement::public_copy_boundary(r::apply(c,s,w.start())),
        crate::durable_engine_refinement::public_copy_boundary(r::apply(c,s,w.finish())),
        crate::durable_engine_refinement::public_copy_boundary(
            bridge::accepted(c,r::apply(c,s,w.finish()),delivery(w.record))),
        crate::durable_engine_refinement::public_copy_boundary(cleanup_published(c,s,w)),
{
    crate::durable_engine_refinement::public_copy_step(c,s,w.start());
    crate::durable_engine_refinement::public_copy_step(c,s,w.finish());
    let durable=r::apply(c,s,w.finish());
    let envelope=r::envelope(durable,delivery(w.record));
    crate::durable_engine_refinement::public_copy_step(c,durable,r::Action::Accept{envelope});
    crate::durable_engine_refinement::public_copy_step(c,
        bridge::accepted(c,durable,delivery(w.record)),r::Action::Commit{envelope});
}

/// A generation-scoped range check, not an old successful completion token,
/// controls destructive I/O. One overlapping point with a new fence suffices to
/// reject old cleanup; a completion byte for another plan cannot repair this.
pub proof fn stale_generation_cannot_cleanup(node:Participant,old:MigrationPlan,key:Seq<u8>,new_generation:u64)
    requires crate::bytes::contains_spec(old.range,old.range.table,key),
        node.local_meta(old.range.table,key).is_some(),
        node.local_meta(old.range.table,key).unwrap().fence==new_generation,new_generation!=old.generation,
    ensures !node.transfer_authorized(old,true),
{
    if node.transfer_authorized(old,true) {
        assert(crate::participant::role_spec(node.local_meta(old.range.table,key).unwrap(),old.generation,Role::Empty,None,true));
    }
}

pub proof fn stale_reservation_cannot_start(step:NativeStep,plan:MigrationPlan,key:Seq<u8>,new_generation:u64)
    requires step.operation==(Operation::Reserve{plan,cleanup:true}),
        crate::bytes::contains_spec(plan.range,plan.range.table,key),
        step.before.local_meta(plan.range.table,key).is_some(),
        step.before.local_meta(plan.range.table,key).unwrap().fence==new_generation,new_generation!=plan.generation,
    ensures !step.valid(),!admitted_reservation(kv::CommitResult::Committed,step,plan),
{
    stale_generation_cannot_cleanup(step.before,plan,key,new_generation);
}

pub proof fn rejected_or_duplicate_stutters(c:p::Constants,ctx:source::SourceContext,s:source::SourceState,
    recovery:r::State,record:source::MetadataRecord,step:NativeStep)
    requires record.observed(ctx,s),!record.fresh(),binds_delivery(step,record),step.valid(),
    ensures record.image(ctx,s).placement==s.placement,step.after.local_unchanged(step.before),r::next(c,recovery,recovery),
{
    recovery_stutter(c,recovery);
}

/// Long-lived cleanup coupling keeps only this authority's selected replicas
/// fixed. Other replicas, sessions, logical rows, certificates and runtime
/// incarnations may advance. `pending` above is the initial exact-prefix case.
pub open spec fn cleanup_local_frame(w:CleanupProjection,v:p::State)->bool {
    let g=w.record.plan.generation as nat;
    let owner=w.record.before.owner_view() as int;
    v.plans.contains_key(g) && v.plans[g]==w.source.placement.plans[g]
        && v.commands.contains((g,metadata::command(w.record.command)))
        && forall|key:int|w.record.keys.to_set().contains(key) ==>
            v.physical.contains_key((owner,key))
            && p::replica(v,owner,key)==p::replica(w.source.placement,owner,key)
}
pub open spec fn cleanup_native_range(w:CleanupProjection,node:Participant)->bool {
    node.owner_view()==w.record.after.owner_view()
        && forall|table:u64,key:Seq<u8>|crate::bytes::contains_spec(w.record.plan.range,table,key) ==>
            node.local_meta(table,key)==w.record.after.local_meta(table,key)
}
pub open spec fn locally_pending(c:p::Constants,w:CleanupProjection,node:Participant,s:r::State)->bool {
    w.valid(c) && cleanup_local_frame(w,s.core.placement) && cleanup_native_range(w,node)
}
pub proof fn initial_local_cleanup_projection(c:p::Constants,w:CleanupProjection,node:Participant,s:r::State)
    requires w.pending(c,node,s),
    ensures locally_pending(c,w,node,s),
{}

pub proof fn rebased_cleanup_guard(c:p::Constants,w:CleanupProjection,s:r::State)
    requires w.valid(c),cleanup_local_frame(w,s.core.placement),
    ensures p::local_guard(s.core.placement,w.record.plan.generation as nat,
            metadata::command(w.record.command),w.record.before.owner_view() as int),
        p::enabled(c,s.core.placement,delivery(w.record)),
        r::destructive(s.core.placement,w.record.plan.generation as nat,
            metadata::command(w.record.command),w.record.before.owner_view() as int),
{
    metadata_dispatch(c,w.context,w.source,w.record);
    assert forall|key:int|s.core.placement.plans[w.record.plan.generation as nat].keys.contains(key) implies
        p::replica(s.core.placement,w.record.before.owner_view() as int,key)
            ==p::replica(w.source.placement,w.record.before.owner_view() as int,key) by {}
}

/// A physical frame, not global quiescence or a desired successor: the actual
/// interleaving supplies unchanged selected replicas and immutable plan identity.
pub proof fn rebase_cleanup_projection(c:p::Constants,w:CleanupProjection,
    before:Participant,after:Participant,s:r::State,z:r::State)
    requires locally_pending(c,w,before,s),after.owner_view()==before.owner_view(),
        z.core.placement.plans.contains_key(w.record.plan.generation as nat),
        z.core.placement.plans[w.record.plan.generation as nat]==s.core.placement.plans[w.record.plan.generation as nat],
        z.core.placement.commands.contains((w.record.plan.generation as nat,metadata::command(w.record.command))),
        forall|key:int|w.record.keys.to_set().contains(key) ==>
            z.core.placement.physical.contains_key((w.record.before.owner_view() as int,key))
            && p::replica(z.core.placement,w.record.before.owner_view() as int,key)
                ==p::replica(s.core.placement,w.record.before.owner_view() as int,key),
        forall|table:u64,key:Seq<u8>|crate::bytes::contains_spec(w.record.plan.range,table,key) ==>
            after.local_meta(table,key)==before.local_meta(table,key),
    ensures locally_pending(c,w,after,z),
{}

/// Actual other-authority metadata publication may change its own physical
/// cells/certificates while the pending cleanup witness retains only local cells.
pub proof fn other_owner_delivery_rebases(c:p::Constants,w:CleanupProjection,node:Participant,
    s:r::State,envelope:r::Envelope,generation:nat,command:p::Command,owner:int)
    requires locally_pending(c,w,node,s),owner!=w.record.before.owner_view() as int,
        envelope.action==(p::Action::Deliver{generation,command,owner}),
    ensures locally_pending(c,w,node,r::apply(c,s,r::Action::Commit{envelope})),
{
    let z=r::apply(c,s,r::Action::Commit{envelope});
    assert forall|key:int|w.record.keys.to_set().contains(key) implies
        z.core.placement.physical.contains_key((w.record.before.owner_view() as int,key))
        && p::replica(z.core.placement,w.record.before.owner_view() as int,key)
            ==p::replica(s.core.placement,w.record.before.owner_view() as int,key) by {}
    rebase_cleanup_projection(c,w,node,node,s,z);
}

/// The existing cleanup authorization theorem excludes *every* held lease at
/// the hidden owner/range. A real engine settlement may therefore change any
/// unrelated rows and terminal transaction state without moving this witness.
pub proof fn unrelated_transaction_rebases(c:p::Constants,w:CleanupProjection,node:Participant,
    s:r::State,txn:int,committed:bool)
    requires r::inv(c,s),locally_pending(c,w,node,s),
        r::active(s,r::Authority::Participant(node.owner_view() as int)),
        r::enabled(c,s,r::Action::EngineSettle{txn,committed}),
    ensures locally_pending(c,w,node,r::apply(c,s,r::Action::EngineSettle{txn,committed})),
{
    rebased_cleanup_guard(c,w,s);
    assert(r::enabled(c,s,w.start()));
    assert(p::can_resolve(s.core.placement,txn,s.pending[txn]));
    let z=r::apply(c,s,r::Action::EngineSettle{txn,committed});
    assert forall|key:int|w.record.keys.to_set().contains(key) implies
        z.core.placement.physical.contains_key((node.owner_view() as int,key))
        && p::replica(z.core.placement,node.owner_view() as int,key)
            ==p::replica(s.core.placement,node.owner_view() as int,key) by {
        r::theorem_cleanup_authorization(c,s,w.record.plan.generation as nat,node.owner_view() as int,
            metadata::command(w.record.command),key);
    }
    rebase_cleanup_projection(c,w,node,node,s,z);
}

/// Ghost-only byte projection. Retired and covered Stage/Ready model replicas
/// retain the measured pre-delete bytes even while physical deletion proceeds.
/// Outside the private range the projection is always the current real image.
pub open spec fn mask_cleanup_rows(range:crate::types::KeyRange,retained:storage::Image,current:storage::Image)->storage::Image {
    Map::new(retained.dom().filter(|cell:storage::Cell|storage::in_range(range,cell))
        .union(current.dom().filter(|cell:storage::Cell|!storage::in_range(range,cell))),
        |cell:storage::Cell|if storage::in_range(range,cell){retained[cell]}else{current[cell]})
}
pub proof fn mask_cleanup_value(range:crate::types::KeyRange,retained:storage::Image,current:storage::Image,cell:storage::Cell)
    ensures storage::value(mask_cleanup_rows(range,retained,current),cell)
        == (if storage::in_range(range,cell){storage::value(retained,cell)}else{storage::value(current,cell)}),
{}

pub proof fn initialize_cleanup_rows(range:crate::types::KeyRange,initial:storage::Image)
    ensures mask_cleanup_rows(range,initial,initial)==initial,
{
    assert(mask_cleanup_rows(range,initial,initial) =~= initial);
}

/// Exact failed/partial cleanup-loop frame. Prefix supplies the actual deletion
/// cursor; the full-image outside frame ensures serving bytes are not hidden.
pub proof fn private_cleanup_prefix_rows(c:p::Constants,s:r::State,w:CleanupProjection,
    node:Participant,images:source::EngineImages,retained:storage::Image,
    before:storage::Image,after:storage::Image)->(cursor:Option<storage::Identity>)
    requires locally_pending(c,w,node,s),
        images.contains_key(node.owner_view() as int),
        images[node.owner_view() as int]==mask_cleanup_rows(w.record.plan.range,retained,before),
        source::engine_observed(s.core.placement,images,w.context.labels),
        storage::prefix(after,before,Map::empty(),w.record.plan.range),
        storage::preserves_outside(after,before,w.record.plan.range),
    ensures storage::mirrored(after,before,Map::empty(),w.record.plan.range,cursor),
        mask_cleanup_rows(w.record.plan.range,retained,after)==images[node.owner_view() as int],
        source::engine_observed(s.core.placement,
            images.insert(node.owner_view() as int,mask_cleanup_rows(w.record.plan.range,retained,after)),w.context.labels),
        r::next(c,s,s),
{
    let cursor=choose|cursor:Option<storage::Identity>|
        storage::mirrored(after,before,Map::empty(),w.record.plan.range,cursor);
    assert(mask_cleanup_rows(w.record.plan.range,retained,after)
        =~= mask_cleanup_rows(w.record.plan.range,retained,before));
    recovery_stutter(c,s);
    cursor
}

/// A timed-out row delete may resolve old-or-whole after a crash. The retained
/// image does not change in either case; it is not passed to engine recovery.
pub proof fn private_cleanup_row_settlement(range:crate::types::KeyRange,retained:storage::Image,
    before:crate::durable_engine::RowState,pending:Option<crate::durable_engine::RowEffect>,
    after:crate::durable_engine::RowState)
    requires crate::durable_engine::settled_rows(before,pending,after),
        pending is Some ==> storage::in_range(range,pending.unwrap().cell),
    ensures mask_cleanup_rows(range,retained,after.bytes)==mask_cleanup_rows(range,retained,before.bytes),
        storage::preserves_outside(after.bytes,before.bytes,range),
{
    assert(mask_cleanup_rows(range,retained,after.bytes) =~= mask_cleanup_rows(range,retained,before.bytes));
}

/// Outside row effects commute with the mask. The ordinary native transaction
/// byte bridge can therefore run on this projected image without freezing the
/// rest of the owner or treating unrelated changes as stutters.
pub proof fn unrelated_row_commutes(range:crate::types::KeyRange,retained:storage::Image,
    before:crate::durable_engine::RowState,effect:crate::durable_engine::RowEffect)
    requires !storage::in_range(range,effect.cell),
    ensures mask_cleanup_rows(range,retained,crate::durable_engine::row_apply(before,effect).bytes)
        ==crate::durable_engine::row_apply(crate::durable_engine::RowState{
            bytes:mask_cleanup_rows(range,retained,before.bytes),versions:before.versions},effect).bytes,
{
    assert(mask_cleanup_rows(range,retained,crate::durable_engine::row_apply(before,effect).bytes)
        =~=crate::durable_engine::row_apply(crate::durable_engine::RowState{
            bytes:mask_cleanup_rows(range,retained,before.bytes),versions:before.versions},effect).bytes);
}

/// Publication drops the retained byte mask only after the existing durable
/// completion record has earned terminal Deliver. Empty no longer observes the
/// old bytes; every outside observation still comes from the real current image.
pub proof fn completed_cleanup_unmasks_rows(c:p::Constants,w:CleanupProjection,s:r::State,
    images:source::EngineImages,retained:storage::Image,current:storage::Image)
    requires w.valid(c),cleanup_local_frame(w,s.core.placement),
        source::engine_observed(s.core.placement,images,w.context.labels),
        images.contains_key(w.record.before.owner_view() as int),
        images[w.record.before.owner_view() as int]==mask_cleanup_rows(w.record.plan.range,retained,current),
    ensures source::engine_observed(p::apply(c,s.core.placement,delivery(w.record)),
        images.insert(w.record.before.owner_view() as int,current),w.context.labels),
{
    rebased_cleanup_guard(c,w,s);
    let owner=w.record.before.owner_view() as int;
    let z=p::apply(c,s.core.placement,delivery(w.record));
    assert forall|q:(int,int)|z.physical.contains_key(q) && w.context.labels.contains_key(q.1)
        && !(p::replica(z,q.0,q.1).role is Empty)
        && (!(p::replica(z,q.0,q.1).role is Stage)||p::replica(z,q.0,q.1).covered) implies
        images.insert(owner,current).contains_key(q.0)
        && p::replica(z,q.0,q.1).cell.value==
            crate::sharding_bytes::value_option(storage::value(images.insert(owner,current)[q.0],w.context.labels[q.1])) by {
        if q.0==owner {
            assert(!storage::in_range(w.record.plan.range,w.context.labels[q.1]));
            mask_cleanup_value(w.record.plan.range,retained,current,w.context.labels[q.1]);
        }
    }
}

pub proof fn recovered_local_cleanup(c:p::Constants,w:CleanupProjection,s:r::State,
    node:&crate::durable_participant::DurableParticipant,prior:ActorHistory,
    bytes:Map<Seq<u8>,Seq<u8>>,incarnation:u64)
    requires r::inv(c,s),node.wf(),node.replay_equivalent(),
        prior.valid(node.records(),node.namespace().owner),locally_pending(c,w,prior.endpoint(),s),
        kv::has_prefix(bytes,node.namespace(),incarnation,node.records()),
        c.shards.contains(node.namespace().owner as int),s.core.members.contains_key(node.namespace().owner as int),
    ensures locally_pending(c,w,node.native(),s),
        locally_pending(c,w,node.native(),r::witnesses::bootstrap(c,s,r::Authority::Participant(node.namespace().owner as int))),
{
    hide(codec::wire); hide(codec::encodable); hide(kv::has_prefix); hide(NativeStep::valid);
    node.replay_refinement();
    bridge::participant_histories_unique(prior,node.replay_history(),node.records(),node.namespace().owner);
    assert(prior.endpoint().semantic_eq(node.native()));
    r::witnesses::bootstrap_summary(c,s,r::Authority::Participant(node.namespace().owner as int));
}

/// Initialization binds both projections to the acknowledged native reservation
/// and the actual pre-delete engine image. The retained map is an output of this
/// proof construction, not input to executable reservation or replay.
pub proof fn reserved_cleanup_initializes_rows(c:p::Constants,w:CleanupProjection,s:r::State,
    trace:ActorHistory,records:Seq<Seq<u8>>,index:int,step:NativeStep,
    bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,incarnation:u64,result:kv::CommitResult,
    images:source::EngineImages,initial:storage::Image)
    requires r::inv(c,s),bridge::old_slots(s),w.pending(c,step.before,s),
        recorded_step(trace,records,ns.owner,index,step),admitted_reservation(result,step,w.record.plan),
        !ns.coordinator,ns.owner==w.record.before.owner_view(),kv::has_prefix(bytes,ns,incarnation,records),
        incarnation==r::head(s,r::Authority::Participant(ns.owner as int)).incarnation,
        r::active(s,r::Authority::Participant(ns.owner as int)),
        images.contains_key(ns.owner as int),images[ns.owner as int]==initial,
        source::engine_observed(s.core.placement,images,w.context.labels),
    ensures locally_pending(c,w,step.after,r::apply(c,s,w.start())),
        source::engine_observed(r::apply(c,s,w.start()).core.placement,
            images.insert(ns.owner as int,mask_cleanup_rows(w.record.plan.range,initial,initial)),w.context.labels),
        r::enabled(c,s,w.start()),r::next(c,s,r::apply(c,s,w.start())),
{
    hide(codec::wire); hide(codec::encodable); hide(kv::has_prefix); hide(NativeStep::valid);
    native_reservation_starts_cleanup(c,w,s,trace,records,index,step,bytes,ns,incarnation,result);
    initial_local_cleanup_projection(c,w,step.after,r::apply(c,s,w.start()));
    initialize_cleanup_rows(w.record.plan.range,initial);
}

/// Full publication after arbitrary framed interleavings. The old source record
/// authenticates the command; only its local range frame is retained. Concrete
/// completion bytes authorize removal of the row mask and the terminal receipt.
pub proof fn rebased_cleanup_publication(c:p::Constants,w:CleanupProjection,s:r::State,
    trace:ActorHistory,records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,index:int,
    bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,incarnation:u64,
    images:source::EngineImages,retained:storage::Image)
    requires r::inv(c,s),bridge::old_slots(s),w.valid(c),cleanup_local_frame(w,s.core.placement),
        !ns.coordinator,ns.owner==w.record.before.owner_view(),kv::has_prefix(bytes,ns,incarnation,records),
        trace.valid(records,ns.owner),history::history_valid(records,ns.owner,facts),
        history::facts_match(records,facts,trace),
        0<=index<records.len(),records[index]==codec::wire(Operation::Complete{plan:w.record.plan,cleanup:true}),
        cleanup_native_range(w,trace.steps[index-1].before),
        incarnation==r::head(s,r::Authority::Participant(ns.owner as int)).incarnation,
        r::active(s,r::Authority::Participant(ns.owner as int)),
        s.cleaning.contains((w.record.plan.generation as nat,ns.owner as int,metadata::command(w.record.command))),
        !s.cleaned.contains((w.record.plan.generation as nat,ns.owner as int,metadata::command(w.record.command))),
        crate::durable_engine_refinement::public_copy_boundary(s),
        source::engine_observed(s.core.placement,images,w.context.labels),
        images.contains_key(ns.owner as int),
        images[ns.owner as int]==mask_cleanup_rows(w.record.plan.range,retained,facts[index].durable),
    ensures r::next(c,s,r::apply(c,s,w.finish())),
        r::next(c,r::apply(c,s,w.finish()),bridge::accepted(c,r::apply(c,s,w.finish()),delivery(w.record))),
        r::next(c,bridge::accepted(c,r::apply(c,s,w.finish()),delivery(w.record)),cleanup_published(c,s,w)),
        r::inv(c,cleanup_published(c,s,w)),bridge::old_slots(cleanup_published(c,s,w)),
        crate::durable_engine_refinement::public_copy_boundary(cleanup_published(c,s,w)),
        cleanup_published(c,s,w).core.placement==p::apply(c,s.core.placement,delivery(w.record)),
        source::engine_observed(cleanup_published(c,s,w).core.placement,
            images.insert(ns.owner as int,facts[index].durable),w.context.labels),
        transfer::local_coupling(&facts[index].after,w.record.plan,
            cleanup_published(c,s,w).core.placement,w.context.window(w.record.keys)),
        forall|cell:storage::Cell|storage::in_range(w.record.plan.range,cell) ==> storage::value(facts[index].durable,cell)==None,
{
    hide(codec::wire); hide(kv::has_prefix); hide(NativeStep::valid);
    hide(codec::encodable);
    committed_cleanup_rows(records,ns.owner,facts,index,w.record.plan);
    assert(history::is_completion(records[index]));
    history::completion_at(records,ns.owner,facts,trace,index);
    rebased_cleanup_guard(c,w,s);
    public_copy_boundary_settles_cleanup(s,w.record.plan.generation as nat,ns.owner as int);
    r::lemma_step(c,s,w.finish());
    bridge::slots_step(c,s,w.finish());
    recovery_edge(c,s,w.finish());
    let durable=r::apply(c,s,w.finish());
    bridge::commit_authority(c,durable,delivery(w.record));
    cleanup_preserves_public_copy_boundary(c,s,w);
    completed_cleanup_unmasks_rows(c,w,s,images,retained,facts[index].durable);
    let labels=w.context.window(w.record.keys);
    assert forall|key:int|s.core.placement.plans[w.record.plan.generation as nat].keys.contains(key) implies
        facts[index].after.local_meta(labels[key].0,labels[key].1.0).is_some()
        && metadata::metadata(facts[index].after.local_meta(labels[key].0,labels[key].1.0).unwrap(),
            p::replica(cleanup_published(c,s,w).core.placement,ns.owner as int,key)) by {
        assert(storage::in_range(w.record.plan.range,labels[key]));
        metadata::native_effect_matches(&w.record.before,&w.record.after,w.record.plan,w.record.command,
            labels[key].1.0,p::replica(s.core.placement,ns.owner as int,key));
        assert(facts[index].after.local_meta(labels[key].0,labels[key].1.0)
            ==facts[index].before.local_meta(labels[key].0,labels[key].1.0));
        assert(facts[index].before.local_meta(labels[key].0,labels[key].1.0)
            ==trace.steps[index-1].before.local_meta(labels[key].0,labels[key].1.0));
        assert(facts[index].after.local_meta(labels[key].0,labels[key].1.0)
            ==w.record.after.local_meta(labels[key].0,labels[key].1.0));
    }
}

pub proof fn native_registry_preserves_cleanup(c:p::Constants,w:CleanupProjection,s:r::State,step:NativeStep)
    requires locally_pending(c,w,step.before,s),step.valid(),
        match step.operation {
            Operation::Begin{..}|Operation::Resolve(_)|Operation::Finish(_)
                |Operation::Release{..}|Operation::Acquire{..}=>true,
            _=>false,
        },
    ensures locally_pending(c,w,step.after,s),
{
    step.after.metadata_observations(step.before);
}

/// The mask changes no routed or held observation. This is the missing bridge
/// between retained Retired/Ready ghost bytes and destructive native deletion:
/// no participant/client may observe the selected private owner range.
pub proof fn cleanup_mask_preserves_serving_observations(c:p::Constants,w:CleanupProjection,s:r::State,
    node:Participant,retained:storage::Image,current:storage::Image)
    requires r::inv(c,s),locally_pending(c,w,node,s),
        r::active(s,r::Authority::Participant(node.owner_view() as int)),
    ensures forall|key:int|w.context.labels.contains_key(key)
        && p::directory(s.core.placement).contains_key(key)
        && p::directory(s.core.placement)[key].owner==node.owner_view() as int ==>
            storage::value(mask_cleanup_rows(w.record.plan.range,retained,current),w.context.labels[key])
                ==storage::value(current,w.context.labels[key]),
        forall|txn:int,key:int|w.context.labels.contains_key(key)
            && s.core.placement.sessions.contains_key(txn) && s.core.placement.sessions[txn].held.contains_key(key)
            && s.core.placement.sessions[txn].held[key].owner==node.owner_view() as int ==>
                storage::value(mask_cleanup_rows(w.record.plan.range,retained,current),w.context.labels[key])
                    ==storage::value(current,w.context.labels[key]),
{
    assert(w.record.observed(w.context,w.source) && w.record.fresh());
    assert(w.context.plan_observed(w.source,w.record.plan,w.record.keys));
    rebased_cleanup_guard(c,w,s);
    assert(r::enabled(c,s,w.start()));
    assert forall|key:int|w.record.keys.to_set().contains(key) implies
        p::directory(s.core.placement)[key].owner!=node.owner_view() as int
        && (forall|txn:int|s.core.placement.sessions.contains_key(txn)
            && s.core.placement.sessions[txn].held.contains_key(key) ==>
                s.core.placement.sessions[txn].held[key].owner!=node.owner_view() as int) by {
        r::theorem_cleanup_authorization(c,s,w.record.plan.generation as nat,node.owner_view() as int,
            metadata::command(w.record.command),key);
    }
    assert forall|key:int|w.context.labels.contains_key(key)
        && p::directory(s.core.placement).contains_key(key)
        && p::directory(s.core.placement)[key].owner==node.owner_view() as int implies
            storage::value(mask_cleanup_rows(w.record.plan.range,retained,current),w.context.labels[key])
                ==storage::value(current,w.context.labels[key]) by {
        assert(storage::in_range(w.record.plan.range,w.context.labels[key])==w.record.keys.to_set().contains(key));
        if storage::in_range(w.record.plan.range,w.context.labels[key]) {
            r::theorem_cleanup_authorization(c,s,w.record.plan.generation as nat,node.owner_view() as int,
                metadata::command(w.record.command),key);
        }
        assert(!storage::in_range(w.record.plan.range,w.context.labels[key]));
        mask_cleanup_value(w.record.plan.range,retained,current,w.context.labels[key]);
    }
    assert forall|txn:int,key:int|w.context.labels.contains_key(key)
        && s.core.placement.sessions.contains_key(txn) && s.core.placement.sessions[txn].held.contains_key(key)
        && s.core.placement.sessions[txn].held[key].owner==node.owner_view() as int implies
            storage::value(mask_cleanup_rows(w.record.plan.range,retained,current),w.context.labels[key])
                ==storage::value(current,w.context.labels[key]) by {
        assert(storage::in_range(w.record.plan.range,w.context.labels[key])==w.record.keys.to_set().contains(key));
        if storage::in_range(w.record.plan.range,w.context.labels[key]) {
            r::theorem_cleanup_authorization(c,s,w.record.plan.generation as nat,node.owner_view() as int,
                metadata::command(w.record.command),key);
        }
        assert(!storage::in_range(w.record.plan.range,w.context.labels[key]));
        mask_cleanup_value(w.record.plan.range,retained,current,w.context.labels[key]);
    }
}

pub proof fn recovered_private_cleanup_rows(c:p::Constants,w:CleanupProjection,s:r::State,
    node:&crate::durable_participant::DurableParticipant,prior:ActorHistory,
    journal_bytes:Map<Seq<u8>,Seq<u8>>,incarnation:u64,
    retained:storage::Image,images:source::EngineImages,
    before:crate::durable_engine::RowState,pending:Option<crate::durable_engine::RowEffect>,
    after:crate::durable_engine::RowState)
    requires r::inv(c,s),node.wf(),node.replay_equivalent(),
        prior.valid(node.records(),node.namespace().owner),locally_pending(c,w,prior.endpoint(),s),
        kv::has_prefix(journal_bytes,node.namespace(),incarnation,node.records()),
        c.shards.contains(node.namespace().owner as int),s.core.members.contains_key(node.namespace().owner as int),
        crate::durable_engine::settled_rows(before,pending,after),
        pending is Some ==> storage::in_range(w.record.plan.range,pending.unwrap().cell),
        images.contains_key(w.record.before.owner_view() as int),
        images[w.record.before.owner_view() as int]==mask_cleanup_rows(w.record.plan.range,retained,before.bytes),
        source::engine_observed(s.core.placement,images,w.context.labels),
    ensures locally_pending(c,w,node.native(),
            r::witnesses::bootstrap(c,s,r::Authority::Participant(node.namespace().owner as int))),
        source::engine_observed(
            r::witnesses::bootstrap(c,s,r::Authority::Participant(node.namespace().owner as int)).core.placement,
            images.insert(w.record.before.owner_view() as int,
                mask_cleanup_rows(w.record.plan.range,retained,after.bytes)),w.context.labels),
        storage::preserves_outside(after.bytes,before.bytes,w.record.plan.range),
{
    hide(codec::wire); hide(codec::encodable); hide(kv::has_prefix); hide(NativeStep::valid);
    recovered_local_cleanup(c,w,s,node,prior,journal_bytes,incarnation);
    private_cleanup_row_settlement(w.record.plan.range,retained,before,pending,after);
    r::witnesses::bootstrap_summary(c,s,r::Authority::Participant(node.namespace().owner as int));
}

/// The same acknowledged native reservation which enables StartCleanup also
/// supplies the copy bridge's structural history witness. Advancing the native
/// cut cannot erase earlier witnesses, even when a reservation is a duplicate.
pub proof fn reservation_extends_cleanup_backing(c:p::Constants,s:r::State,w:CleanupProjection,
    trace:ActorHistory,records:Seq<Seq<u8>>,index:int,step:NativeStep,owner:u32)
    requires crate::durable_engine_refinement::cleanup_history_backing(s,trace,index,owner),
        recorded_step(trace,records,owner,index,step),
        step.operation==(Operation::Reserve{plan:w.record.plan,cleanup:true}),
        owner==w.record.before.owner_view(),
    ensures crate::durable_engine_refinement::cleanup_history_backing(r::apply(c,s,w.start()),trace,index+1,owner),
{
    hide(codec::wire); hide(codec::encodable); hide(NativeStep::valid);
    hide(Participant::semantic_eq);
    let z=r::apply(c,s,w.start());
    assert forall|entry:(nat,int,p::Command)|z.cleaning.contains(entry) && entry.1==owner as int implies
        exists|j:int|0<=j<index+1 && match #[trigger] trace.steps[j].operation {
            Operation::Reserve{plan,cleanup}=>cleanup && plan.generation as nat==entry.0,_=>false,
        } by {
        if entry==(w.record.plan.generation as nat,owner as int,metadata::command(w.record.command)) {
            assert(trace.steps[index].operation==(Operation::Reserve{plan:w.record.plan,cleanup:true}));
        }else{
            assert(s.cleaning.contains(entry));
            let j=choose|j:int|0<=j<index && match #[trigger] trace.steps[j].operation {
                Operation::Reserve{plan,cleanup}=>cleanup && plan.generation as nat==entry.0,_=>false,
            };
            assert(0<=j<index+1);
        }
    }
}

/// Completion publication never invents another cleanup reservation. This
/// also covers unrelated actors and replay/row steps whose cleaning set frames.
pub proof fn framed_cleanup_backing(s:r::State,z:r::State,trace:ActorHistory,cut:int,next_cut:int,owner:u32)
    requires crate::durable_engine_refinement::cleanup_history_backing(s,trace,cut,owner),
        z.cleaning==s.cleaning,cut<=next_cut<=trace.steps.len(),
    ensures crate::durable_engine_refinement::cleanup_history_backing(z,trace,next_cut,owner),
{
    assert forall|entry:(nat,int,p::Command)|z.cleaning.contains(entry) && entry.1==owner as int implies
        exists|j:int|0<=j<next_cut && match #[trigger] trace.steps[j].operation {
            Operation::Reserve{plan,cleanup}=>cleanup && plan.generation as nat==entry.0,_=>false,
        } by {
        let j=choose|j:int|0<=j<cut && match #[trigger] trace.steps[j].operation {
            Operation::Reserve{plan,cleanup}=>cleanup && plan.generation as nat==entry.0,_=>false,
        };
        assert(0<=j<next_cut);
    }
}

pub proof fn publication_preserves_cleanup_backing(c:p::Constants,s:r::State,w:CleanupProjection,
    trace:ActorHistory,cut:int,next_cut:int,owner:u32)
    requires crate::durable_engine_refinement::cleanup_history_backing(s,trace,cut,owner),
        cut<=next_cut<=trace.steps.len(),
    ensures crate::durable_engine_refinement::cleanup_history_backing(cleanup_published(c,s,w),trace,next_cut,owner),
{
    framed_cleanup_backing(s,cleanup_published(c,s,w),trace,cut,next_cut,owner);
}

/// Selected-step access avoids expanding every operation in the enclosing
/// trace while retaining the full executable reservation contract.
pub proof fn reservation_native_frame(step:NativeStep,plan:MigrationPlan)
    requires step.valid(),step.operation==(Operation::Reserve{plan,cleanup:true}),
    ensures step.after==step.before,step.before.transfer_authorized(plan,true),
{}

pub proof fn reservation_successor_frame(prefix:Seq<NativeStep>,step:NativeStep,plan:MigrationPlan)
    requires step.valid(),crate::durable_participant::replay_proofs::allowed(prefix,step.operation),
        crate::durable_participant::replay_proofs::pending(prefix)==Some((plan,true)),
    ensures step.after.local_unchanged(step.before),
        match step.operation {
            Operation::Complete{plan:completed,cleanup}=>cleanup && storage::same_plan(plan,completed),
            Operation::Settled=>true,_=>false,
        },
{}
} // verus!
