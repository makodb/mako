//! Durable admission around the actual native Participant and transfer loops.
//! No production storage backend is selected here. Recovery reads only committed
//! journal bytes; leases survive recovery until the engine settles their exact
//! transaction identity. One exclusive owner must retain this wrapper, KV and
//! engine borrows through each call (including the native row operations).
use vstd::prelude::*;
use std::collections::HashMap;
use crate::types::*;
use crate::directory::RouteTable;
use crate::participant::{Participant,ControlResult,Cleanup};
use crate::storage::{self,Source};
use crate::durable_engine::DurableEngine;
use crate::durable_kv::{Journal,JournalError,DurableKv,Namespace,Admission,CommitResult};
#[path="durable_participant_codec.rs"]
pub mod codec;
use codec::Operation;
#[path="durable_participant_history.rs"]
pub mod history;
#[cfg(verus_keep_ghost)]
use history::{history_valid,durable_history};
use history::CompletionFact;
#[path="durable_participant_replay.rs"]
pub mod replay_proofs;
use replay_proofs::{ActorHistory,NativeStep};

verus! {
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum ParticipantError { Journal(JournalError), Native(Status), Fenced, PlanConflict, Corrupt, EngineUnknown }

/// Whole-engine preservation for every return path. A pre-existing unresolved
/// row is unchanged when admission fails; any newly pending row is in-range.
pub open spec fn migration_engine_frame<E:DurableEngine>(before:E,after:E,plan:MigrationPlan)->bool {
    after.owner()==before.owner() && after.installed()==before.installed() && after.transactions()==before.transactions()
        && storage::preserves_outside(after.working_state().bytes,before.working_state().bytes,plan.range)
        && storage::preserves_outside(after.durable_state().bytes,before.durable_state().bytes,plan.range)
        && crate::durable_engine::versions_outside(after.working_state().versions,before.working_state().versions,plan.range)
        && crate::durable_engine::versions_outside(after.durable_state().versions,before.durable_state().versions,plan.range)
        && (after.pending_row()==before.pending_row()
            || (after.pending_row() is Some && storage::in_range(plan.range,after.pending_row().unwrap().cell)))
}
proof fn unchanged_engine_frame<E:DurableEngine>(engine:E)
    ensures forall|plan:MigrationPlan|migration_engine_frame(engine,engine,plan),
{}
proof fn matching_engine_frames<E:DurableEngine>(before:E,after:E,selected:MigrationPlan)
    requires migration_engine_frame(before,after,selected),
    ensures forall|plan:MigrationPlan|storage::same_plan(plan,selected) ==> migration_engine_frame(before,after,plan),
{
    assert forall|plan:MigrationPlan|storage::same_plan(plan,selected) implies migration_engine_frame(before,after,plan) by{
        assert forall|cell:storage::Cell|storage::in_range(plan.range,cell)==storage::in_range(selected.range,cell) by{};
    }
}
proof fn completion_evidence(fact:CompletionFact,origins:Map<u64,(u64,u64)>)->(step:NativeStep)
    requires fact.before.wf(),fact.after.wf(),
        fact.before.owner_view()==fact.owner,fact.after.owner_view()==fact.owner,
        fact.before.transfer_authorized(fact.plan,fact.cleanup),
        fact.after.lease_view()==fact.before.lease_view(),fact.after.table_ids()==fact.before.table_ids(),
        storage::preserves_outside(fact.copied,fact.initial,fact.plan.range),
        storage::complete(fact.copied,fact.initial,fact.source,fact.plan.range),fact.durable==fact.copied,
        fact.after.receipt_view()=={
            let r=fact.before.receipt_view()[fact.plan.generation];
            fact.before.receipt_view().insert(fact.plan.generation,if fact.cleanup{(r.0,r.1,true,r.3)}else{(r.0,r.1,r.2,true)})
        },
        if fact.cleanup{fact.source==Map::empty() && fact.after.cleanup_done(fact.plan.generation)
            && fact.after.local_unchanged(fact.before)}else{fact.after.seal_frame(fact.before,fact.plan)},
    ensures fact.valid(),step.valid(),step.before==fact.before,step.after==fact.after,
        step.operation==(Operation::Complete{plan:fact.plan,cleanup:fact.cleanup}),step.status==Status::Ok,
        step.origins_before==origins,step.origins_after==origins,
{
    hide(storage::preserves_outside);hide(storage::complete);hide(Participant::range_role);
    hide(Participant::seal_frame);hide(Participant::local_unchanged);
    reveal(CompletionFact::valid);
    NativeStep{before:fact.before,after:fact.after,operation:Operation::Complete{plan:fact.plan,cleanup:fact.cleanup},
        status:Status::Ok,origins_before:origins,origins_after:origins}
}

/// Not constructible by callers. Only the committed-record replay path creates
/// one, after matching the pending reservation and the retained immutable plan.
/// Its bytes/index refer to an actual Journal::read_record result, not a caller
/// boolean, a physical table handle, or an engine migration-correctness oracle.
pub(crate) struct ReplayCompletion<'a> {
    plan:&'a MigrationPlan,
    owner:u32,
    cleanup:bool,
    index:u64,
    records:Ghost<Seq<Seq<u8>>>,
    fact:Ghost<CompletionFact>,
}
impl<'a> ReplayCompletion<'a> {
    pub closed spec fn wf(&self)->bool {
        self.index<self.records@.len() && self.fact@.valid() && self.fact@.owner==self.owner
            && storage::same_plan(*self.plan,self.fact@.plan) && self.cleanup==self.fact@.cleanup
            && self.records@[self.index as int]==codec::wire(Operation::Complete{plan:*self.plan,cleanup:self.cleanup})
    }
    pub closed spec fn matches_spec(&self,plan:MigrationPlan,owner:u32,cleanup:bool)->bool {
        storage::same_plan(*self.plan,plan) && self.owner==owner && self.cleanup==cleanup
            && self.index<self.records@.len()
            && self.records@[self.index as int]==codec::wire(Operation::Complete{plan:*self.plan,cleanup})
            && self.fact@.valid() && storage::same_plan(self.fact@.plan,plan)
    }
    pub(crate) fn matches(&self,plan:&MigrationPlan,owner:u32,cleanup:bool)->(yes:bool)
        requires self.wf(),
        ensures yes==self.matches_spec(*plan,owner,cleanup),
    {self.owner==owner && self.cleanup==cleanup && storage::plans_match(self.plan,plan)}
}

pub struct DurableParticipant {
    actor:Participant,
    journal:Journal,
    plans:Vec<MigrationPlan>,
    pending:Option<(u64,bool)>,
    completed:Vec<(u64,bool)>,
    origins:HashMap<u64,(u64,u64)>,
    facts:Ghost<Map<int,CompletionFact>>,
    trace:Ghost<ActorHistory>,
    candidate:bool,
}
fn rejected(status:Status)->(out:ControlResult)
    ensures out.status==status,out.certificate==None,out.cleanup==Cleanup::None,
{ControlResult{status,certificate:None,cleanup:Cleanup::None}}
proof fn participant_reflexive(actor:Participant)
    ensures actor.semantic_eq(actor),
{}
impl DurableParticipant {
    pub closed spec fn wf(&self)->bool {
        self.actor.wf() && self.journal.wf() && !self.journal.namespace_spec().coordinator
            && self.actor.owner_view()==self.journal.namespace_spec().owner
            && history_valid(self.journal.records(),self.actor.owner_view(),self.facts@)
            && self.trace@.valid(self.records(),self.namespace().owner)
            && history::facts_match(self.records(),self.facts@,self.trace@)
            && (!self.candidate ==> replay_proofs::cache_matches(self.plans@,self.pending,self.trace@.steps))
            && (!self.candidate ==> self.replay_equivalent())
            && (forall|i:int| 0<=i<self.completed.len() ==> self.completion_record(self.completed@[i].0,self.completed@[i].1))
    }
    pub closed spec fn consistent<K:DurableKv>(&self,kv:&K)->bool {self.journal.consistent(kv)}
    pub closed spec fn active(&self)->bool {self.journal.active() && !self.candidate}
    pub closed spec fn records(&self)->Seq<Seq<u8>>{self.journal.records()}
    pub closed spec fn native(&self)->Participant{self.actor}
    pub closed spec fn namespace(&self)->Namespace{self.journal.namespace_spec()}
    pub closed spec fn completion_facts(&self)->Map<int,CompletionFact>{self.facts@}
    pub closed spec fn replay_equivalent(&self)->bool {
        self.actor.semantic_eq(self.trace@.endpoint()) && self.origins@==self.trace@.origins()
    }
    pub closed spec fn replay_history(&self)->ActorHistory{self.trace@}
    pub closed spec fn origin_view(&self)->Map<u64,(u64,u64)>{self.origins@}
    pub proof fn replay_refinement(&self)
        requires self.wf(),self.replay_equivalent(),
        ensures self.replay_history().valid(self.records(),self.namespace().owner),
            self.native().semantic_eq(self.replay_history().endpoint()),
            self.origin_view()==self.replay_history().origins(),
    {}
    pub proof fn history_facts(&self)
        requires self.wf(),
        ensures history_valid(self.records(),self.namespace().owner,self.completion_facts()),
            self.replay_history().valid(self.records(),self.namespace().owner),
            history::facts_match(self.records(),self.completion_facts(),self.replay_history()),
    {}
    pub closed spec fn pending_phase(&self)->Option<(MigrationPlan,bool)>{replay_proofs::pending(self.trace@.steps)}
    pub closed spec fn bound_plan(&self,plan:MigrationPlan)->bool{replay_proofs::bound(self.trace@.steps,plan)}
    proof fn selected_binding(&self,index:int)
        requires self.wf(),!self.candidate,0<=index<self.plans.len(),
        ensures self.bound_plan(self.plans@[index]),
            forall|plan:MigrationPlan|self.bound_plan(plan) && plan.generation==self.plans@[index].generation ==>
                storage::same_plan(plan,self.plans@[index]),
    {
        replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);
        assert forall|plan:MigrationPlan|self.bound_plan(plan) && plan.generation==self.plans@[index].generation
            implies storage::same_plan(plan,self.plans@[index]) by{
            replay_proofs::immutable_bindings(self.trace@,self.records(),self.namespace().owner,plan,self.plans@[index]);
        }
    }
    proof fn completion_cache(&self,index:int,cleanup:bool,step:NativeStep)
        requires self.wf(),0<=index<self.plans.len(),1<self.records().len(),
            replay_proofs::cache_matches(self.plans@,self.pending,self.trace@.steps),
            self.records().last()==codec::wire(Operation::Reserve{plan:self.plans@[index],cleanup}),
            step.operation==(Operation::Complete{plan:self.plans@[index],cleanup}),
        ensures replay_proofs::allowed(self.trace@.steps,step.operation),
            replay_proofs::cache_matches(self.plans@,None,self.trace@.steps.push(step)),
    {
        hide(codec::wire);hide(NativeStep::valid);hide(Participant::semantic_eq);
        hide(Participant::bootstrap_view);hide(Participant::transfer_authorized);
        self.trace@.layout(self.records(),self.namespace().owner);
        self.trace@.entry(self.records(),self.namespace().owner,self.trace@.steps.len() as int-1);
        let last=self.trace@.steps.last();
        codec::operation_unique(last.operation,Operation::Reserve{plan:self.plans@[index],cleanup});
        replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);
        replay_proofs::cache_append(self.plans@,self.pending,self.trace@.steps,step.operation,step,self.plans@,None);
    }
    pub proof fn completion_witness(&self,generation:u64,cleanup:bool)->(index:int)
        requires self.wf(),self.completion_record(generation,cleanup),
        ensures 0<index<self.records().len(),self.completion_facts().contains_key(index),
            self.completion_facts()[index].valid(),self.completion_facts()[index].owner==self.namespace().owner,
            self.completion_facts()[index].plan.generation==generation,self.completion_facts()[index].cleanup==cleanup,
            self.records()[index]==codec::wire(Operation::Complete{plan:self.completion_facts()[index].plan,cleanup}),
            self.records()[index-1]==codec::wire(Operation::Reserve{plan:self.completion_facts()[index].plan,cleanup}),
            self.completion_facts()[index].before.semantic_eq(self.replay_history().steps[index-1].before),
            self.completion_facts()[index].after.semantic_eq(self.replay_history().steps[index-1].after),
    {
        hide(codec::wire);
        let i=history::completion_index(self.records(),self.facts@,generation,cleanup);
        assert(history::is_completion(self.records()[i]));
        history::completion_at(self.records(),self.namespace().owner,self.facts@,self.trace@,i);
        i
    }
    pub proof fn completion_plan(&self,plan:MigrationPlan,cleanup:bool)->(index:int)
        requires self.wf(),self.bound_plan(plan),self.completion_record(plan.generation,cleanup),
        ensures 0<index<self.records().len(),self.completion_facts().contains_key(index),
            storage::same_plan(self.completion_facts()[index].plan,plan),self.completion_facts()[index].cleanup==cleanup,
            self.records()[index]==codec::wire(Operation::Complete{plan:self.completion_facts()[index].plan,cleanup}),
    {
        hide(codec::wire);
        let index=self.completion_witness(plan.generation,cleanup);let fact=self.facts@[index];
        self.trace@.layout(self.records(),self.namespace().owner);
        self.trace@.entry(self.records(),self.namespace().owner,index-1);
        let step=self.trace@.steps[index-1];
        codec::operation_unique(step.operation,Operation::Complete{plan:fact.plan,cleanup});
        assert(replay_proofs::bound(self.trace@.steps.take(index-1),fact.plan));
        let j=choose|j:int|0<=j<index-1 && match #[trigger] self.trace@.steps.take(index-1)[j].operation{
            Operation::Bind(p)=>storage::same_plan(p,fact.plan),_=>false};
        assert(self.trace@.steps[j]==self.trace@.steps.take(index-1)[j]);
        replay_proofs::immutable_bindings(self.trace@,self.records(),self.namespace().owner,plan,fact.plan);
        index
    }
    proof fn reservation_record(&self,before:Self,index:int,plan:MigrationPlan,cleanup:bool)
        requires self.wf(),before.records().len()<=index<self.records().len(),0<index,
            self.records()[index]==codec::wire(Operation::Reserve{plan,cleanup}),
        ensures self.acknowledged_reservation(before,plan.generation,cleanup),
    {
        hide(codec::wire);
        self.trace@.layout(self.records(),self.namespace().owner);
        self.trace@.entry(self.records(),self.namespace().owner,index-1);
        let step=self.trace@.steps[index-1];
        codec::operation_unique(step.operation,Operation::Reserve{plan,cleanup});
        if let Operation::Reserve{plan:recorded,cleanup:c}=step.operation{
            assert(self.records()[index]==codec::wire(Operation::Reserve{plan:recorded,cleanup:c}));
            assert(step.before.transfer_authorized(recorded,c));
            assert(self.acknowledged_reservation(before,plan.generation,cleanup));
        }
    }
    pub open spec fn delivered_result(&self,before:Self,generation:u64,command:Command,result:ControlResult)->bool {
        exists|plan:MigrationPlan,step:NativeStep|self.delivery_evidence(before,generation,command,result,plan,step)
    }
    pub open spec fn delivery_evidence(&self,before:Self,generation:u64,command:Command,result:ControlResult,
        plan:MigrationPlan,step:NativeStep)->bool {
        plan.generation==generation
            && step.operation==Operation::Deliver{plan,command} && step.status==result.status && step.valid()
            && step.before.semantic_eq(before.native()) && step.after.semantic_eq(self.native())
            && self.records()==before.records().push(codec::wire(step.operation))
            && self.replay_history()==before.replay_history().append(step)
            && (result.status==Status::Ok ==> self.native().command_seen(generation,command) && self.native().has_receipt(generation))
            && (result.certificate.is_some() ==> result.status==Status::Ok && result.cleanup==Cleanup::None)
            && (result.certificate==Some(Certificate::Retired) ==> command==Command::Retire)
            && (result.certificate==Some(Certificate::SourceDone) ==> (command==Command::Commit || command==Command::Abort) && before.native().owner_view()==plan.source)
            && (result.certificate==Some(Certificate::DestinationDone) ==> (command==Command::Commit || command==Command::Abort) && before.native().owner_view()==plan.destination)
            && (result.status==Status::Ok && command==Command::Retire && !before.native().command_seen(generation,command) ==> before.native().capture_authorized(plan))
            && (result.status==Status::Ok && !before.native().command_seen(generation,command) ==>
                forall|k:Seq<u8>|crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
                    before.native().command_at(plan,command,before.native().drained_view(plan.range),k))
    }
    pub open spec fn acknowledged_reservation(&self,before:Self,generation:u64,cleanup:bool)->bool {
        exists|i:int,plan:MigrationPlan|before.records().len()<=i<self.records().len() && 0<i
            && plan.generation==generation && self.records()[i]==codec::wire(Operation::Reserve{plan,cleanup})
            && i-1<self.replay_history().steps.len() && self.replay_history().steps[i-1].valid()
            && self.replay_history().steps[i-1].operation==Operation::Reserve{plan,cleanup}
            && self.replay_history().steps[i-1].before.transfer_authorized(plan,cleanup)
    }
    pub open spec fn transfer_publication(&self,before:Self,generation:u64,cleanup:bool,result:ControlResult)->bool {
        result.certificate.is_some() ==> result.status==Status::Ok && result.cleanup==Cleanup::None
            && self.completion_record(generation,cleanup)
            && (exists|i:int|0<=i<self.records().len() && self.completion_facts().contains_key(i)
                && self.completion_facts()[i].plan.generation==generation && self.completion_facts()[i].cleanup==cleanup
                && self.records()[i]==codec::wire(Operation::Complete{plan:self.completion_facts()[i].plan,cleanup})
                && result.certificate==Some(if !cleanup{Certificate::Ready}
                    else if self.namespace().owner==self.completion_facts()[i].plan.source{Certificate::SourceDone}else{Certificate::DestinationDone}))
            && (self.records()==before.records() && self.native().semantic_eq(before.native())
                || self.records().len()==before.records().len()+2 && self.acknowledged_reservation(before,generation,cleanup)
                    && self.records().last()==codec::wire(Operation::Complete{plan:self.completion_facts()[self.records().len() as int-1].plan,cleanup}))
    }
    pub open spec fn completion_record(&self,generation:u64,cleanup:bool)->bool {
        history::has_completion(self.records(),self.completion_facts(),generation,cleanup)
    }
    pub proof fn durable_reachable<K:DurableKv>(&self,kv:&K)
        requires self.wf(),self.consistent(kv),
        ensures durable_history(kv,self.namespace()),
    {
        self.journal.committed_records(kv);
        history::witnessed_map(kv.committed(),self.namespace(),self.journal.incarnation_spec(),self.records(),self.facts@,self.trace@);
    }
    pub closed spec fn accepts(&self,a:Admission)->bool {
        self.active() && a.namespace==self.journal.namespace_spec() && a.incarnation==self.journal.incarnation_spec()
    }
    fn check(&self,a:&Admission)->(yes:bool)
        ensures yes==self.accepts(*a),
    {!self.candidate && self.journal.check(a)}
    /// Fencing only changes journal admission flags, not the replay/cache view.
    fn fence(&mut self)
        requires old(self).wf(),
        ensures final(self).wf(),!final(self).active(),
            final(self).records()==old(self).records(),final(self).namespace()==old(self).namespace(),
            final(self).journal.incarnation_spec()==old(self).journal.incarnation_spec(),
            final(self).actor==old(self).actor,final(self).plans@==old(self).plans@,
            final(self).pending==old(self).pending,final(self).origins@==old(self).origins@,
            final(self).facts==old(self).facts,final(self).trace==old(self).trace,
            final(self).completed@==old(self).completed@,final(self).candidate==old(self).candidate,
    {
        hide(codec::wire);hide(Participant::semantic_eq);
        self.journal.fence();
        proof{
            assert(self.replay_equivalent()==old(self).replay_equivalent());
            assert forall|i:int|0<=i<self.completed.len() implies self.completion_record(self.completed@[i].0,self.completed@[i].1) by{
                assert(old(self).completion_record(self.completed@[i].0,self.completed@[i].1));
            }
        }
    }
    fn plan_index(&self,generation:u64)->(out:Option<usize>)
        ensures out is Some ==> out->Some_0<self.plans.len() && self.plans@[out->Some_0 as int].generation==generation,
            out is None ==> forall|i:int|0<=i<self.plans.len() ==> self.plans@[i].generation!=generation,
    {
        let mut i=0usize;
        while i<self.plans.len()
            invariant i<=self.plans.len(),forall|j:int|0<=j<i ==> self.plans@[j].generation!=generation,
            decreases self.plans.len()-i,
        {if self.plans[i].generation==generation{return Some(i);}i+=1;}
        None
    }
    fn earned(&self,generation:u64,cleanup:bool)->(yes:bool)
        requires self.wf(),
        ensures yes ==> self.completion_record(generation,cleanup),
    {
        hide(codec::wire);hide(Participant::semantic_eq);
        let mut i=0usize;
        while i<self.completed.len()
            invariant i<=self.completed.len(),self.wf(),
            decreases self.completed.len()-i,
        {if self.completed[i].0==generation && self.completed[i].1==cleanup{return true;}i+=1;}
        false
    }
    fn origin_matches(&self,id:TxnId,incarnation:u64)->(yes:bool)
        ensures yes==(self.origins@.contains_key(id.client) && self.origins@[id.client]==(id.sequence,incarnation)),
    {
        match self.origins.get(&id.client){Some(pair)=>pair.0==id.sequence && pair.1==incarnation,None=>false}
    }
    pub open spec fn recovery_work(&self,id:TxnId,incarnation:u64)->bool {
        self.origin_view().contains_key(id.client) && self.origin_view()[id.client].0==id.sequence
            && self.origin_view()[id.client].1!=incarnation && self.native().recovery_pending(id)
    }
    pub open spec fn recovery_record(&self,index:int,incarnation:u64)->bool {
        0<=index<self.records().len() && exists|pair:(TxnId,u64)|self.recovery_work(pair.0,incarnation)
            && self.origin_view()[pair.0.client].1==pair.1
            && self.records()[index]==codec::wire(Operation::Begin{id:pair.0,incarnation:pair.1})
    }
    /// Start with cursor 0 after activate. Advance using the returned cursor;
    /// finish each returned identity through the durable engine fence. Repeated
    /// Begin records are harmless: finished identities are always excluded.
    pub fn next_recovery_transaction<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,cursor:u64)
        ->(out:Result<Option<(TxnId,u64)>,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),final(self).consistent(final(kv)),
            final(kv).committed()==old(kv).committed(),final(kv).pending()==old(kv).pending(),
            out is Ok ==> *final(self)==*old(self) && final(self).accepts(*a),
            out is Ok && out->Ok_0 is Some ==> {
                let work=out->Ok_0->Some_0;
                cursor<work.1<=final(self).records().len() && final(self).recovery_work(work.0,a.incarnation)
            },
            out is Ok && out->Ok_0 is None ==> forall|i:int|cursor<=i<final(self).records().len() ==> !final(self).recovery_record(i,a.incarnation),
            out is Ok && out->Ok_0 is None && cursor==0 ==> forall|id:TxnId|!final(self).recovery_work(id,a.incarnation),
    {
        hide(NativeStep::valid);hide(codec::wire);
        if !self.check(a){return Err(ParticipantError::Fenced);}
        let count=self.journal.count();
        if cursor>count{return Err(ParticipantError::Native(Status::Invalid));}
        let mut i=cursor;
        while i<count
            invariant cursor<=i<=count,count==self.records().len(),self.wf(),kv.wf(),self.consistent(kv),
                *self==*old(self),self.accepts(*a),kv.committed()==old(kv).committed(),kv.pending()==old(kv).pending(),
                forall|j:int|cursor<=j<i ==> !self.recovery_record(j,a.incarnation),
            decreases count-i,
        {
            let bytes=match self.journal.read_record(kv,i){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Journal(e))};
            let record=match codec::decode_begin(&bytes){Ok(v)=>v,Err(_)=>{
                proof{self.journal.committed_records(kv);}
                self.fence();
                proof{self.journal.prefix_consistent(kv);}
                return Err(ParticipantError::Corrupt);
            }};
            if let Some((id,origin))=record{
                if origin!=a.incarnation && self.origin_matches(id,origin) && self.actor.needs_settlement(id){
                    return Ok(Some((id,i+1)));
                }
            }
            proof{
                if self.recovery_record(i as int,a.incarnation){
                    let (id,origin)=choose|pair:(TxnId,u64)|self.recovery_work(pair.0,a.incarnation)
                        && self.origin_view()[pair.0.client].1==pair.1
                        && self.records()[i as int]==codec::wire(Operation::Begin{id:pair.0,incarnation:pair.1});
                    if let Some((decoded,inc))=record{
                        codec::operation_unique(Operation::Begin{id:decoded,incarnation:inc},Operation::Begin{id,incarnation:origin});
                    }
                }
                assert(!self.recovery_record(i as int,a.incarnation));
                assert forall|j:int|cursor<=j<i+1 implies !self.recovery_record(j,a.incarnation) by{
                    if j==i{}else{assert(j<i);}
                }
            }
            i+=1;
        }
        proof{
            if cursor==0{
                assert forall|id:TxnId|!self.recovery_work(id,a.incarnation) by{
                    if self.recovery_work(id,a.incarnation){
                        replay_proofs::origin_begin(self.trace@,self.records(),self.namespace().owner,id.client);
                        let origin=self.origins@[id.client].1;
                        let j=choose|j:int|0<j<self.records().len() && self.records()[j]==codec::wire(Operation::Begin{id,incarnation:origin});
                        let pair=(id,origin);
                        assert(self.recovery_work(pair.0,a.incarnation)
                            && self.origin_view()[pair.0.client].1==pair.1
                            && self.records()[j]==codec::wire(Operation::Begin{id:pair.0,incarnation:pair.1}));
                        assert(self.recovery_record(j,a.incarnation));
                    }
                }
            }
        }
        Ok(None)
    }
    fn begin_origin(&mut self,id:TxnId,incarnation:u64)->(s:Status)
        requires old(self).wf(),old(self).candidate,
        ensures final(self).wf(),final(self).journal==old(self).journal,
            final(self).plans@==old(self).plans@,final(self).facts==old(self).facts,
            final(self).completed@==old(self).completed@,final(self).pending==old(self).pending,
            final(self).trace==old(self).trace,final(self).candidate,
            (NativeStep{before:old(self).actor,after:final(self).actor,operation:Operation::Begin{id,incarnation},status:s,
                origins_before:old(self).origins@,origins_after:final(self).origins@}).valid(),
    {
        hide(codec::wire);
        let ghost before=self.actor;
        if let Some(pair)=self.origins.get(&id.client){
            if pair.0==id.sequence && pair.1!=incarnation{
                proof{
                    self.actor.metadata_reflexive();
                    assert(replay_proofs::origin_rejected(self.origins@,id,incarnation));
                }
                return Status::Retry;
            }
        }
        proof{assert(!replay_proofs::origin_rejected(self.origins@,id,incarnation));}
        let status=self.actor.begin(id);
        if matches!(status,Status::Ok){self.origins.insert(id.client,(id.sequence,incarnation));}
        proof{self.actor.metadata_observations(before);}
        status
    }
    fn append<K:DurableKv>(&mut self,kv:&mut K,bytes:Vec<u8>,Ghost(step):Ghost<NativeStep>)->(out:Result<(),ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),!history::is_completion(bytes@),
            step.valid(),codec::wire(step.operation)==bytes@,step.after==old(self).actor,step.origins_after==old(self).origins@,
            step.before.semantic_eq(old(self).trace@.endpoint()),step.origins_before==old(self).trace@.origins(),
            replay_proofs::allowed(old(self).trace@.steps,step.operation),
            replay_proofs::cache_matches(old(self).plans@,old(self).pending,old(self).trace@.steps.push(step)),
        ensures final(self).wf(),final(kv).wf(),durable_history(final(kv),old(self).namespace()),
            final(self).namespace()==old(self).namespace(),
            final(self).journal.incarnation_spec()==old(self).journal.incarnation_spec(),
            final(self).actor==old(self).actor,final(self).plans@==old(self).plans@,
            final(self).pending==old(self).pending,final(self).origins@==old(self).origins@,
            final(self).completed@==old(self).completed@,final(self).facts==old(self).facts,
            out is Ok ==> final(self).active() && final(self).consistent(final(kv))
                && final(self).records()==old(self).records().push(bytes@) && final(self).trace@==old(self).trace@.append(step),
            out is Err ==> final(self).records()==old(self).records() && final(self).trace==old(self).trace,
            out is Err ==> !final(self).active(),
    {
        hide(codec::wire);hide(NativeStep::valid);hide(Participant::semantic_eq);
        proof{assert(self.actor.semantic_eq(self.actor)) by{reveal(Participant::semantic_eq);}}
        proof{self.durable_reachable(kv);}
        if self.journal.count()==u64::MAX{self.fence();return Err(ParticipantError::Journal(JournalError::Exhausted));}
        let ghost before=kv.committed();let ghost records=self.records();
        let ghost ns=self.namespace();let ghost incarnation=self.journal.incarnation_spec();let ghost payload=bytes@;
        proof{self.journal.committed_records(kv);history::append_ordinary(records,ns.owner,self.facts@,payload,self.trace@,step);}
        let result=self.journal.append(kv,bytes);
        proof{history::appended_history(kv,before,ns,incarnation,records,payload,self.facts@,self.facts@,self.trace@,step);}
        match result{
            Ok(())=>{
                proof{
                    assert(self.records()==records.push(payload));
                    assert(self.records().take(records.len() as int) =~= records);
                    history::completion_prefix(records,self.records(),self.facts@,self.facts@);
                }
                self.trace=Ghost(self.trace@.append(step));self.candidate=false;Ok(())
            },
            Err(e)=>Err(ParticipantError::Journal(e)),
        }
    }
    fn append_identity<K:DurableKv>(&mut self,kv:&mut K,bytes:Vec<u8>,Ghost(op):Ghost<Operation>,
        Ghost(cache):Ghost<(Seq<MigrationPlan>,Option<(u64,bool)>)>)->(out:Result<(),ParticipantError>)
        requires old(self).wf(),old(self).replay_equivalent(),old(kv).wf(),old(self).consistent(old(kv)),
            bytes@==codec::wire(op),op is Bind || op is Reserve || op is Settled,
            replay_proofs::allowed(old(self).trace@.steps,op),
            op is Reserve ==> old(self).actor.transfer_authorized(op->Reserve_plan,op->Reserve_cleanup),
            replay_proofs::cache_matches(cache.0,cache.1,old(self).trace@.steps),
            replay_proofs::cache_effect(cache.0,cache.1,op,old(self).plans@,old(self).pending),
        ensures final(self).wf(),final(kv).wf(),durable_history(final(kv),old(self).namespace()),
            final(self).actor==old(self).actor,final(self).plans@==old(self).plans@,final(self).origins@==old(self).origins@,
            final(self).namespace()==old(self).namespace(),final(self).journal.incarnation_spec()==old(self).journal.incarnation_spec(),
            out is Ok ==> final(self).active() && final(self).consistent(final(kv))
                && final(self).records()==old(self).records().push(bytes@),
            out is Err ==> !final(self).active(),
    {
        hide(codec::wire);hide(NativeStep::valid);
        let ghost step=NativeStep{before:self.actor,after:self.actor,operation:op,status:Status::Ok,
            origins_before:self.origins@,origins_after:self.origins@};
        proof{assert(step.valid()) by{reveal(NativeStep::valid);}}
        proof{history::not_completion(op);}
        proof{replay_proofs::cache_append(cache.0,cache.1,self.trace@.steps,op,step,self.plans@,self.pending);}
        self.append(kv,bytes,Ghost(step))
    }

    pub fn create<K:DurableKv>(kv:&mut K,owner:u32,tables:Vec<RouteTable>)->(out:Result<Self,ParticipantError>)
        requires old(kv).wf(),durable_history(old(kv),Namespace{owner,coordinator:false}),
        ensures final(kv).wf(),durable_history(final(kv),Namespace{owner,coordinator:false}),
            out is Ok ==> out->Ok_0.wf() && !out->Ok_0.active() && out->Ok_0.consistent(final(kv)),
    {
        let op=Operation::Bootstrap{owner,tables};
        let ghost bootstrap=op;
        let bytes=match codec::encode(&op){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        let tables=match op{Operation::Bootstrap{tables,..}=>tables,_=>return Err(ParticipantError::Corrupt)};
        let actor=match Participant::new(owner,tables){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        let ghost trace=ActorHistory{bootstrap,initial:actor,steps:Seq::empty()};
        proof{replay_proofs::initialize(trace,owner);replay_proofs::empty_cache();}
        let ghost before=kv.committed();let ghost pending=kv.pending();
        let submitted=Journal::create(kv,Namespace{owner,coordinator:false},bytes);
        proof{history::created_history(kv,before,pending,Namespace{owner,coordinator:false},trace);}
        let journal=match submitted{Ok(v)=>v,Err(e)=>return Err(ParticipantError::Journal(e))};
        let result=Self{actor,journal,plans:Vec::new(),pending:None,completed:Vec::new(),origins:HashMap::new(),
            facts:Ghost(Map::empty()),trace:Ghost(trace),candidate:false};
        proof{result.durable_reachable(kv);}
        Ok(result)
    }

    /// Drop the old wrapper before recovery. The row barrier settles *all* old
    /// copy/delete submissions before examining the definitive journal head.
    /// It deliberately does not finish a single transaction lease.
    pub fn recover<K:DurableKv,E:DurableEngine>(kv:&mut K,engine:&mut E,owner:u32)->(out:Result<Self,ParticipantError>)
        requires old(kv).wf(),old(engine).wf(),old(engine).owner()==owner,
            durable_history(old(kv),Namespace{owner,coordinator:false}),
        ensures final(kv).wf(),final(engine).wf(),
            durable_history(final(kv),Namespace{owner,coordinator:false}),
            out is Ok ==> out->Ok_0.wf() && !out->Ok_0.active() && out->Ok_0.consistent(final(kv)),
            out is Ok ==> out->Ok_0.replay_equivalent(),
    {
        hide(NativeStep::valid);hide(codec::wire);hide(history::map_history);
        let ghost initial=kv.committed();let ghost pending=kv.pending();
        let status=engine.recover_rows();if !matches!(status,Status::Ok){return Err(ParticipantError::Native(status));}
        let recovered=Journal::recover(kv,Namespace{owner,coordinator:false});
        proof{history::settled_history(kv,initial,pending,Namespace{owner,coordinator:false});}
        let mut journal=match recovered{Ok(v)=>v,Err(e)=>return Err(ParticipantError::Journal(e))};
        proof{
            journal.committed_records(kv);
            assert(kv.committed().contains_key(crate::durable_kv::key_wire(journal.namespace_spec(),false,0)));
            assert(history::map_history(kv.committed(),journal.namespace_spec()));
        }
        let ghost (facts,trace)=history::recovered_history(&journal,kv);
        proof{trace.layout(journal.records(),owner);}
        let bytes=match journal.read_record(kv,0){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Journal(e))};
        let op=match codec::decode(&bytes){Ok(v)=>v,Err(_)=>return Err(ParticipantError::Corrupt)};
        let ghost bootstrap=op;
        let tables=match op{Operation::Bootstrap{owner:record_owner,tables}=>{
            if record_owner!=owner{return Err(ParticipantError::Corrupt);}tables
        },_=>return Err(ParticipantError::Corrupt)};
        let actor=match Participant::new(owner,tables){Ok(v)=>v,Err(_)=>return Err(ParticipantError::Corrupt)};
        proof{replay_proofs::bootstrap_unique(bootstrap,trace.bootstrap,actor,trace.initial);}
        let mut result=Self{actor,journal,plans:Vec::new(),pending:None,completed:Vec::new(),origins:HashMap::new(),
            facts:Ghost(facts),trace:Ghost(trace),candidate:true};
        proof{
            replay_proofs::empty_cache();
            assert(result.plans@ =~= Seq::<MigrationPlan>::empty());
            assert(trace.steps.take(0) =~= Seq::<NativeStep>::empty());
        }
        let count=result.journal.count();let mut i=1u64;
        while i<count
            invariant result.wf(),!result.active(),kv.wf(),result.consistent(kv),1<=i<=count,
                engine.wf(),
                result.candidate,result.trace@==trace,!result.journal.active(),
                result.actor.semantic_eq(if i==1{trace.initial}else{trace.steps[i as int-2].after}),
                replay_proofs::cache_matches(result.plans@,result.pending,trace.steps.take(i as int-1)),
                result.origins@==if i==1{Map::empty()}else{trace.steps[i as int-2].origins_after},
                count==result.journal.records().len(),
                count==trace.steps.len()+1,durable_history(kv,Namespace{owner,coordinator:false}),
                result.namespace()==(Namespace{owner,coordinator:false}),
            decreases count-i,
        {
            let bytes=match result.journal.read_record(kv,i){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Journal(e))};
            let op=match codec::decode(&bytes){Ok(v)=>v,Err(_)=>return Err(ParticipantError::Corrupt)};
            let ghost before=result.actor;let ghost origins=result.origins@;
            if !result.replay(op,i){return Err(ParticipantError::Corrupt);}
            proof{
                trace.entry(result.records(),owner,i as int-1);
                let step=choose|step:NativeStep|step.valid() && step.before==before && step.after==result.actor
                    && step.origins_before==origins && step.origins_after==result.origins@
                    && codec::wire(step.operation)==result.records()[i as int];
                replay_proofs::step_unique(step,trace.steps[i as int-1]);
            }
            i+=1;
        }
        proof{
            assert(trace.steps.take(i as int-1)==trace.steps);
            assert(replay_proofs::cache_matches(result.plans@,result.pending,trace.steps));
            assert(result.actor.semantic_eq(trace.endpoint()));
            assert(result.origins@==trace.origins());
            assert(result.replay_equivalent());
        }
        result.candidate=false;
        Ok(result)
    }

    /// Only startup hands out an admission. There is no getter that upgrades an
    /// old queued request. Durable incarnation installation precedes publication.
    pub fn activate<K:DurableKv,E:DurableEngine>(&mut self,kv:&mut K,engine:&mut E)->(out:Result<Admission,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(engine).wf(),old(self).consistent(old(kv)),
            old(engine).owner()==old(self).native().owner_view(),
        ensures final(self).wf(),final(kv).wf(),final(engine).wf(),
            durable_history(final(kv),old(self).namespace()),
            out is Ok ==> final(self).accepts(out->Ok_0) && final(self).consistent(final(kv)),
            out is Err ==> !final(self).active(),
    {
        proof{self.durable_reachable(kv);}
        if self.candidate{self.fence();return Err(ParticipantError::Fenced);}
        proof{replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);}
        let status=engine.recover_rows();
        if !matches!(status,Status::Ok){self.fence();return Err(ParticipantError::Native(status));}
        let ghost before=kv.committed();let ghost records=self.records();let ghost ns=self.namespace();
        let ghost old_inc=self.journal.incarnation_spec();let ghost next_inc=(old_inc+1) as u64;
        proof{self.journal.committed_records(kv);}
        let activated=self.journal.activate(kv);
        proof{history::activated_history(kv,before,ns,old_inc,next_inc,records,self.facts@,self.trace@);}
        let admission=match activated{Ok(v)=>v,Err(e)=>return Err(ParticipantError::Journal(e))};
        let status=engine.install(admission);
        if !matches!(status,Status::Ok){self.fence();return Err(ParticipantError::Native(status));}
        // A pre-crash reservation may have applied a durable prefix, but no old
        // operation can apply after recover_rows. Record that settlement before
        // permitting a new terminal command or destructive cleanup.
        if self.pending.is_some(){
            let bytes=match codec::encode(&Operation::Settled){Ok(v)=>v,Err(e)=>{self.fence();return Err(ParticipantError::Native(e));}};
            let ghost cache=(self.plans@,self.pending);
            self.candidate=true;self.pending=None;
            if let Err(e)=self.append_identity(kv,bytes,Ghost(Operation::Settled),Ghost(cache)){return Err(e);}
        }
        Ok(admission)
    }

    pub fn bind_plan<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,plan:MigrationPlan)->(out:Result<(),ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)),
            durable_history(final(kv),old(self).namespace()),
    {
        proof{self.durable_reachable(kv);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        proof{replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);}
        if self.pending.is_some(){return Err(ParticipantError::Native(Status::Busy));}
        if let Some(i)=self.plan_index(plan.generation){
            return if storage::plans_match(&self.plans[i],&plan){Ok(())}else{Err(ParticipantError::PlanConflict)};
        }
        if plan.generation==0 || plan.old.len()==0{return Err(ParticipantError::Native(Status::Invalid));}
        let op=Operation::Bind(plan);
        let bytes=match codec::encode(&op){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        let ghost operation=op;
        let ghost cache=(self.plans@,self.pending);
        self.candidate=true;
        if let Operation::Bind(plan)=op{self.plans.push(plan);}
        if let Err(e)=self.append_identity(kv,bytes,Ghost(operation),Ghost(cache)){return Err(e);}
        Ok(())
    }

    pub fn deliver<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,generation:u64,command:Command)->(out:Result<ControlResult,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)) && final(self).accepts(*a),
            durable_history(final(kv),old(self).namespace()),
            out is Ok ==> final(self).delivered_result(*old(self),generation,command,out->Ok_0),
    {
        hide(codec::wire);hide(NativeStep::valid);
        hide(DurableParticipant::delivery_evidence);
        proof{self.durable_reachable(kv);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        proof{replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);}
        if self.pending.is_some(){return Err(ParticipantError::Native(Status::Busy));}
        let i=match self.plan_index(generation){Some(i)=>i,None=>return Err(ParticipantError::Native(Status::NotFound))};
        let bytes=match codec::encode_delivery(&self.plans[i],command){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        let ghost before=self.actor;let ghost origins=self.origins@;
        let ghost operation=Operation::Deliver{plan:self.plans@[i as int],command};
        self.candidate=true;
        let result=self.actor.deliver(&self.plans[i],command);
        let ghost step=NativeStep{before,after:self.actor,operation,status:result.status,origins_before:origins,origins_after:self.origins@};
        proof{assert(step.valid()) by{reveal(NativeStep::valid);}}
        proof{replay_proofs::cache_append(self.plans@,self.pending,self.trace@.steps,operation,step,self.plans@,self.pending);}
        if let Err(e)=self.append(kv,bytes,Ghost(step)){return Err(e);}
        proof{
            let plan=self.plans@[i as int];
            assert(plan.generation==generation && step.operation==Operation::Deliver{plan,command});
            assert(step.before.semantic_eq(old(self).native()) && step.after.semantic_eq(self.native()));
            assert(self.records()==old(self).records().push(codec::wire(step.operation)));
            assert(self.replay_history()==old(self).replay_history().append(step));
            assert(self.delivery_evidence(*old(self),generation,command,result,plan,step)) by {
                reveal(DurableParticipant::delivery_evidence);
            }
            assert(self.delivered_result(*old(self),generation,command,result));
        }
        Ok(result)
    }

    fn lease<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,op:Operation)->(out:Result<Status,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)) && final(self).accepts(*a),
            durable_history(final(kv),old(self).namespace()),
    {
        hide(codec::wire);
        proof{self.durable_reachable(kv);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        proof{replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);}
        if self.pending.is_some(){return Err(ParticipantError::Native(Status::Busy));}
        let bytes=match codec::encode(&op){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        match &op{
            Operation::Begin{incarnation,..}=>{if *incarnation!=a.incarnation{return Err(ParticipantError::Fenced);}}
            Operation::Resolve(id)|Operation::Acquire{id,..}=>{
                if !self.origin_matches(*id,a.incarnation){return Err(ParticipantError::Fenced);}
            }
            Operation::Finish(_)|Operation::Release{..}=>{},
            _=>return Err(ParticipantError::Corrupt),
        }
        let ghost before=self.actor;let ghost origins=self.origins@;let ghost operation=op;
        self.candidate=true;
        let status=match op{
            Operation::Begin{id,incarnation}=>{
                self.begin_origin(id,incarnation)
            }
            Operation::Resolve(id)=>{
                self.actor.resolve(id)
            }
            Operation::Finish(id)=>self.actor.finish(id),
            Operation::Acquire{id,range,point,grant}=>{
                if point{self.actor.acquire(id,range.table,&range.lo,grant)}else{
                    self.actor.acquire_range(id,range.table,&range.lo,match &range.hi{Some(h)=>Some(h.as_slice()),None=>None},grant)}
            }
            Operation::Release{id,table,coordinate}=>self.actor.release(id,table,&coordinate),
            _=>return Err(ParticipantError::Corrupt),
        };
        let ghost step=NativeStep{before,after:self.actor,operation,status,origins_before:origins,origins_after:self.origins@};
        proof{if self.actor.same_metadata(before){self.actor.metadata_observations(before);}}
        proof{replay_proofs::cache_append(self.plans@,self.pending,self.trace@.steps,operation,step,self.plans@,self.pending);}
        if let Err(e)=self.append(kv,bytes,Ghost(step)){return Err(e);}Ok(status)
    }
    pub fn begin<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,id:TxnId)->(out:Result<Status,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)) && final(self).accepts(*a),
            durable_history(final(kv),old(self).namespace()),
    {self.lease(kv,a,Operation::Begin{id,incarnation:a.incarnation})}
    pub fn resolve<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,id:TxnId)->(out:Result<Status,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)) && final(self).accepts(*a),
            durable_history(final(kv),old(self).namespace()),
    {self.lease(kv,a,Operation::Resolve(id))}
    /// Point acquisitions use range.lo and require no upper bound. A scan uses
    /// the actual native acquire_range path, including cross-grant validation.
    pub fn acquire<K:DurableKv>(&mut self,kv:&mut K,a:&Admission,id:TxnId,range:KeyRange,point:bool,grant:Grant)->(out:Result<Status,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)),
            durable_history(final(kv),old(self).namespace()),
    {
        proof{self.durable_reachable(kv);}
        if point && range.hi.is_some(){return Err(ParticipantError::Native(Status::Invalid));}
        self.lease(kv,a,Operation::Acquire{id,range,point,grant})
    }
    pub fn finish<K:DurableKv,E:DurableEngine>(&mut self,kv:&mut K,engine:&mut E,a:&Admission,id:TxnId)->(out:Result<Status,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(engine).wf(),old(self).consistent(old(kv)),
            old(engine).installed()==Some(*a),
        ensures final(self).wf(),final(kv).wf(),final(engine).wf(),
            out is Ok ==> final(self).consistent(final(kv)) && crate::durable_engine::settled(final(engine).transactions(),id),
            durable_history(final(kv),old(self).namespace()),
            crate::durable_engine::transaction_settlement(*old(engine),*final(engine),*a,id),
            !crate::durable_engine::settled(final(engine).transactions(),id) ==> final(self).native().lease_view()==old(self).native().lease_view(),
    {
        proof{self.durable_reachable(kv);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        let s=engine.settle_transaction(a,id);
        if !matches!(s,Status::Ok){self.fence();return Err(ParticipantError::Native(s));}
        self.lease(kv,a,Operation::Finish(id))
    }
    pub fn release<K:DurableKv,E:DurableEngine>(&mut self,kv:&mut K,engine:&mut E,a:&Admission,id:TxnId,table:u64,coordinate:Vec<u8>)->(out:Result<Status,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(engine).wf(),old(self).consistent(old(kv)),old(engine).installed()==Some(*a),
        ensures final(self).wf(),final(kv).wf(),final(engine).wf(),out is Ok ==> final(self).consistent(final(kv)) && crate::durable_engine::settled(final(engine).transactions(),id),
            durable_history(final(kv),old(self).namespace()),
            crate::durable_engine::transaction_settlement(*old(engine),*final(engine),*a,id),
            !crate::durable_engine::settled(final(engine).transactions(),id) ==> final(self).native().lease_view()==old(self).native().lease_view(),
    {
        proof{self.durable_reachable(kv);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        let s=engine.settle_transaction(a,id);
        if !matches!(s,Status::Ok){self.fence();return Err(ParticipantError::Native(s));}
        self.lease(kv,a,Operation::Release{id,table,coordinate})
    }

    fn reserve<K:DurableKv>(&mut self,kv:&mut K,index:usize,cleanup:bool)->(out:Result<(),ParticipantError>)
        requires old(self).wf(),!old(self).candidate,old(kv).wf(),old(self).consistent(old(kv)),index<old(self).plans.len(),
        ensures final(self).wf(),final(kv).wf(),out is Ok ==> final(self).consistent(final(kv)) && final(self).active()
            && final(self).records()==old(self).records().push(codec::wire(Operation::Reserve{plan:old(self).plans@[index as int],cleanup}))
            && final(self).actor.transfer_authorized(old(self).plans@[index as int],cleanup),
            final(self).actor==old(self).actor,final(self).plans@==old(self).plans@,
            final(self).namespace()==old(self).namespace(),
            durable_history(final(kv),old(self).namespace()),
    {
        hide(codec::wire);
        proof{self.durable_reachable(kv);}
        proof{replay_proofs::cache_facts(self.plans@,self.pending,self.trace@.steps);}
        if self.pending.is_some(){return Err(ParticipantError::Native(Status::Busy));}
        let i=index;let generation=self.plans[i].generation;
        let status=self.actor.authorize_transfer(&self.plans[i],cleanup);
        if !matches!(status,Status::Ok){return Err(ParticipantError::Native(status));}
        let bytes=match codec::encode_reservation(&self.plans[i],cleanup){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        let ghost operation=Operation::Reserve{plan:self.plans@[i as int],cleanup};
        let ghost cache=(self.plans@,self.pending);
        self.candidate=true;self.pending=Some((generation,cleanup));
        if let Err(e)=self.append_identity(kv,bytes,Ghost(operation),Ghost(cache)){return Err(e);}Ok(())
    }
    fn publish_completion<K:DurableKv>(&mut self,kv:&mut K,generation:u64,cleanup:bool,bytes:Vec<u8>,
        Ghost(fact):Ghost<CompletionFact>,Ghost(step):Ghost<NativeStep>)->(out:Result<(),ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(self).consistent(old(kv)),
            fact.valid(),fact.owner==old(self).native().owner_view(),fact.plan.generation==generation,fact.cleanup==cleanup,
            bytes@==codec::wire(Operation::Complete{plan:fact.plan,cleanup}),
            old(self).records().last()==codec::wire(Operation::Reserve{plan:fact.plan,cleanup}),
            step.valid(),codec::wire(step.operation)==bytes@,step.after==old(self).actor,step.origins_after==old(self).origins@,
            step.before.semantic_eq(old(self).trace@.endpoint()),step.origins_before==old(self).trace@.origins(),
            fact.before.semantic_eq(step.before),fact.after.semantic_eq(step.after),
            replay_proofs::allowed(old(self).trace@.steps,step.operation),
            replay_proofs::cache_matches(old(self).plans@,None,old(self).trace@.steps.push(step)),
        ensures final(self).wf(),final(kv).wf(),durable_history(final(kv),old(self).namespace()),
            out is Ok ==> final(self).consistent(final(kv)),
            out is Ok ==> final(self).completion_record(generation,cleanup),
            out is Ok ==> final(self).trace@==old(self).trace@.append(step)
                && final(self).records()==old(self).records().push(bytes@)
                && final(self).completion_facts()==old(self).completion_facts().insert(old(self).records().len() as int,fact),
            out is Err ==> final(self).records()==old(self).records() && final(self).trace==old(self).trace,
            final(self).plans@==old(self).plans@,final(self).actor==old(self).actor,
    {
        hide(codec::wire);hide(NativeStep::valid);
        proof{self.trace@.layout(self.records(),self.namespace().owner);}
        proof{self.durable_reachable(kv);}
        if self.journal.count()==u64::MAX{self.fence();return Err(ParticipantError::Journal(JournalError::Exhausted));}
        let ghost before=kv.committed();let ghost records=self.records();
        let ghost ns=self.namespace();let ghost incarnation=self.journal.incarnation_spec();let ghost payload=bytes@;
        let ghost previous=self.facts@;let ghost next=previous.insert(records.len() as int,fact);
        proof{self.journal.committed_records(kv);history::append_completion(records,ns.owner,previous,fact,self.trace@,step);}
        let result=self.journal.append(kv,bytes);
        proof{history::appended_history(kv,before,ns,incarnation,records,payload,previous,next,self.trace@,step);}
        if let Err(e)=result{return Err(ParticipantError::Journal(e));}
        proof{
            assert(self.records()==records.push(payload));
            assert(self.records().take(records.len() as int) =~= records);
            assert forall|i:int|0<=i<records.len() && previous.contains_key(i) implies next.contains_key(i) && next[i]==previous[i] by{};
            history::completion_prefix(records,self.records(),previous,next);
        }
        self.facts=Ghost(next);
        self.trace=Ghost(self.trace@.append(step));self.candidate=false;
        self.completed.push((generation,cleanup));self.pending=None;
        proof{
            history::known_completion(self.records(),self.facts@,records.len() as int);
            assert forall|i:int|0<=i<self.completed.len() implies self.completion_record(self.completed@[i].0,self.completed@[i].1) by{
                if i==old(self).completed.len(){assert(self.completed@[i]==(generation,cleanup));}
                else{assert(self.completed@[i]==old(self).completed@[i]);assert(old(self).completion_record(self.completed@[i].0,self.completed@[i].1));}
            }
        }
        Ok(())
    }

    pub fn execute_final<K:DurableKv,E:DurableEngine,S:Source>(&mut self,kv:&mut K,engine:&mut E,a:&Admission,generation:u64,source:&S)->(out:Result<ControlResult,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(engine).wf(),source.wf(),old(self).consistent(old(kv)),
            old(engine).installed()==Some(*a),
        ensures final(self).wf(),final(kv).wf(),final(engine).wf(),out is Ok ==> final(self).consistent(final(kv)),
            durable_history(final(kv),old(self).namespace()),
            out is Ok && out->Ok_0.certificate==Some(Certificate::Ready) ==> final(self).completion_record(generation,false),
            out is Ok ==> final(self).transfer_publication(*old(self),generation,false,out->Ok_0),
            *final(engine)!=*old(engine) ==> final(self).acknowledged_reservation(*old(self),generation,false),
            final(engine).owner()==old(engine).owner(),final(engine).installed()==old(engine).installed(),
            final(engine).transactions()==old(engine).transactions(),
            forall|plan:MigrationPlan|old(self).bound_plan(plan) && plan.generation==generation ==>
                migration_engine_frame(*old(engine),*final(engine),plan),
    {
        hide(codec::wire);hide(NativeStep::valid);hide(Participant::semantic_eq);
        hide(Participant::bootstrap_view);hide(Participant::transfer_authorized);
        hide(storage::preserves_outside);hide(storage::complete);hide(storage::prefix);
        hide(crate::durable_engine::versions_outside);hide(Participant::seal_frame);hide(Participant::local_unchanged);
        let ghost before_engine=*engine;
        proof{unchanged_engine_frame(before_engine);participant_reflexive(self.actor);}
        let ghost before_call=*self;
        proof{self.durable_reachable(kv);}
        proof{self.trace@.layout(self.records(),self.namespace().owner);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        if !engine.check(a){self.fence();return Err(ParticipantError::Fenced);}
        let i=match self.plan_index(generation){Some(i)=>i,None=>return Err(ParticipantError::Native(Status::NotFound))};
        proof{self.selected_binding(i as int);}
        if self.earned(generation,false){
            proof{self.completion_plan(self.plans@[i as int],false);}
            return Ok(ControlResult{status:Status::Ok,certificate:Some(Certificate::Ready),cleanup:Cleanup::None});
        }
        let status=self.actor.authorize_transfer(&self.plans[i],false);
        if !matches!(status,Status::Ok){return Ok(rejected(status));}
        let bytes=match codec::encode_completion(&self.plans[i],false){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        if let Err(e)=self.reserve(kv,i,false){return Err(e);}
        proof{self.reservation_record(before_call,before_call.records().len() as int,self.plans@[i as int],false);}
        let ghost before=self.actor;let ghost initial=engine.working_state().bytes;
        proof{participant_reflexive(before);}
        let ghost origins=self.origins@;self.candidate=true;
        let result=crate::durable_engine::copy_rows(&mut self.actor,engine,*a,&self.plans[i],source);
        proof{matching_engine_frames(before_engine,*engine,self.plans@[i as int]);}
        if !matches!(result.certificate,Some(Certificate::Ready)){self.fence();return Err(ParticipantError::Native(result.status));}
        match engine.commit_rows(a,&self.plans[i].range){
            CommitResult::Committed=>{},
            CommitResult::NotCommitted=>{self.fence();return Err(ParticipantError::Journal(JournalError::NotCommitted));}
            CommitResult::Unknown=>{self.fence();return Err(ParticipantError::EngineUnknown);}
        }
        let ghost fact=CompletionFact{plan:self.plans@[i as int],owner:self.actor.owner_view(),cleanup:false,
            before,after:self.actor,initial,copied:engine.working_state().bytes,source:source.image(),durable:engine.durable_state().bytes};
        proof{participant_reflexive(self.actor);}
        let ghost step;
        proof{step=completion_evidence(fact,origins);self.completion_cache(i as int,false,step);}
        let ghost completion_index=self.records().len() as int;
        let publication=self.publish_completion(kv,generation,false,bytes,Ghost(fact),Ghost(step));
        proof{
            self.reservation_record(before_call,before_call.records().len() as int,self.plans@[i as int],false);
            if publication is Ok {
                assert(self.records().len()==before_call.records().len()+2);
                assert(self.completion_facts().contains_key(completion_index));
                assert(self.completion_facts()[completion_index]==fact);
                assert(self.records()[completion_index]==codec::wire(Operation::Complete{plan:fact.plan,cleanup:false}));
                assert(result.status==Status::Ok && result.cleanup==Cleanup::None && result.certificate==Some(Certificate::Ready));
                assert(fact.plan.generation==generation);
                assert(self.transfer_publication(before_call,generation,false,result));
            }
        }
        if let Err(e)=publication{return Err(e);}Ok(result)
    }
    pub fn execute_cleanup<K:DurableKv,E:DurableEngine>(&mut self,kv:&mut K,engine:&mut E,a:&Admission,generation:u64)->(out:Result<ControlResult,ParticipantError>)
        requires old(self).wf(),old(kv).wf(),old(engine).wf(),old(self).consistent(old(kv)),old(engine).installed()==Some(*a),
        ensures final(self).wf(),final(kv).wf(),final(engine).wf(),out is Ok ==> final(self).consistent(final(kv)),
            durable_history(final(kv),old(self).namespace()),
            out is Ok && out->Ok_0.certificate.is_some() ==> final(self).completion_record(generation,true),
            out is Ok ==> final(self).transfer_publication(*old(self),generation,true,out->Ok_0),
            *final(engine)!=*old(engine) ==> final(self).acknowledged_reservation(*old(self),generation,true),
            final(engine).owner()==old(engine).owner(),final(engine).installed()==old(engine).installed(),
            final(engine).transactions()==old(engine).transactions(),
            forall|plan:MigrationPlan|old(self).bound_plan(plan) && plan.generation==generation ==>
                migration_engine_frame(*old(engine),*final(engine),plan),
    {
        hide(codec::wire);hide(NativeStep::valid);hide(Participant::semantic_eq);
        hide(Participant::bootstrap_view);hide(Participant::transfer_authorized);
        hide(storage::preserves_outside);hide(storage::complete);hide(storage::prefix);
        hide(crate::durable_engine::versions_outside);hide(Participant::seal_frame);hide(Participant::local_unchanged);
        let ghost before_engine=*engine;
        proof{unchanged_engine_frame(before_engine);participant_reflexive(self.actor);}
        let ghost before_call=*self;
        proof{self.durable_reachable(kv);}
        proof{self.trace@.layout(self.records(),self.namespace().owner);}
        if !self.check(a){return Err(ParticipantError::Fenced);}
        if !engine.check(a){self.fence();return Err(ParticipantError::Fenced);}
        let i=match self.plan_index(generation){Some(i)=>i,None=>return Err(ParticipantError::Native(Status::NotFound))};
        proof{self.selected_binding(i as int);}
        if self.earned(generation,true){
            proof{self.completion_plan(self.plans@[i as int],true);}
            return Ok(ControlResult{status:Status::Ok,
                certificate:Some(if self.actor.owner()==self.plans[i].source{Certificate::SourceDone}else{Certificate::DestinationDone}),cleanup:Cleanup::None});
        }
        let status=self.actor.authorize_transfer(&self.plans[i],true);
        if !matches!(status,Status::Ok){return Ok(rejected(status));}
        let bytes=match codec::encode_completion(&self.plans[i],true){Ok(v)=>v,Err(e)=>return Err(ParticipantError::Native(e))};
        if let Err(e)=self.reserve(kv,i,true){return Err(e);}
        proof{self.reservation_record(before_call,before_call.records().len() as int,self.plans@[i as int],true);}
        let ghost before=self.actor;let ghost initial=engine.working_state().bytes;
        proof{participant_reflexive(before);}
        let ghost origins=self.origins@;self.candidate=true;
        let result=crate::durable_engine::cleanup_rows(&mut self.actor,engine,*a,&self.plans[i]);
        proof{matching_engine_frames(before_engine,*engine,self.plans@[i as int]);}
        if result.certificate.is_none(){self.fence();return Err(ParticipantError::Native(result.status));}
        match engine.commit_rows(a,&self.plans[i].range){
            CommitResult::Committed=>{},
            CommitResult::NotCommitted=>{self.fence();return Err(ParticipantError::Journal(JournalError::NotCommitted));}
            CommitResult::Unknown=>{self.fence();return Err(ParticipantError::EngineUnknown);}
        }
        let ghost fact=CompletionFact{plan:self.plans@[i as int],owner:self.actor.owner_view(),cleanup:true,
            before,after:self.actor,initial,copied:engine.working_state().bytes,source:Map::empty(),durable:engine.durable_state().bytes};
        proof{participant_reflexive(self.actor);}
        let ghost step;
        proof{step=completion_evidence(fact,origins);self.completion_cache(i as int,true,step);}
        let ghost completion_index=self.records().len() as int;
        let publication=self.publish_completion(kv,generation,true,bytes,Ghost(fact),Ghost(step));
        proof{
            self.reservation_record(before_call,before_call.records().len() as int,self.plans@[i as int],true);
            if publication is Ok {
                assert(self.records().len()==before_call.records().len()+2);
                assert(self.completion_facts().contains_key(completion_index));
                assert(self.completion_facts()[completion_index]==fact);
                assert(self.records()[completion_index]==codec::wire(Operation::Complete{plan:fact.plan,cleanup:true}));
                assert(result.status==Status::Ok && result.cleanup==Cleanup::None);
                assert(result.certificate==Some(if self.namespace().owner==fact.plan.source{
                    Certificate::SourceDone}else{Certificate::DestinationDone}));
                assert(fact.plan.generation==generation);
                assert(self.transfer_publication(before_call,generation,true,result));
            }
        }
        if let Err(e)=publication{return Err(e);}Ok(result)
    }

    pub fn drain(&self,a:&Admission,generation:u64)->Result<ControlResult,ParticipantError>{
        if !self.check(a){return Err(ParticipantError::Fenced);}
        let i=match self.plan_index(generation){Some(i)=>i,None=>return Err(ParticipantError::Native(Status::NotFound))};
        Ok(self.actor.drain(&self.plans[i]))
    }
    pub fn meta(&self,a:&Admission,table:u64,coordinate:&[u8])->Result<Option<ReplicaMeta>,ParticipantError>{
        if !self.check(a){return Err(ParticipantError::Fenced);}Ok(self.actor.meta(table,coordinate))
    }
    pub fn capture<E:DurableEngine>(&self,engine:&E,a:&Admission,generation:u64,after:Option<&Row>)->(out:Result<Option<Row>,ParticipantError>)
        requires self.wf(),engine.wf(),engine.installed()==Some(*a),
    {
        if !self.check(a){return Err(ParticipantError::Fenced);}
        let i=match self.plan_index(generation){Some(i)=>i,None=>return Err(ParticipantError::Native(Status::NotFound))};
        let status=self.actor.authorize_capture(&self.plans[i]);
        if !matches!(status,Status::Ok){return Err(ParticipantError::Native(status));}
        match engine.scan(a,&self.plans[i].range,after){Ok(v)=>Ok(v),Err(e)=>Err(ParticipantError::Native(e))}
    }

    fn replay(&mut self,op:Operation,index:u64)->(ok:bool)
        requires old(self).wf(),old(self).candidate,!old(self).journal.active(),index<old(self).records().len(),
            old(self).records()[index as int]==codec::wire(op),
            0<index,replay_proofs::cache_matches(old(self).plans@,old(self).pending,old(self).trace@.steps.take(index as int-1)),
        ensures final(self).wf(),final(self).candidate,final(self).journal==old(self).journal,final(self).trace==old(self).trace,
            ok ==> replay_proofs::cache_matches(final(self).plans@,final(self).pending,final(self).trace@.steps.take(index as int)),
            ok ==> exists|step:NativeStep|step.valid() && step.before==old(self).actor && step.after==final(self).actor
                && step.origins_before==old(self).origins@ && step.origins_after==final(self).origins@
                && codec::wire(step.operation)==old(self).records()[index as int],
    {
        hide(codec::wire);hide(NativeStep::valid);hide(Participant::semantic_eq);
        hide(Participant::bootstrap_view);hide(Participant::transfer_authorized);
        hide(codec::same_operation);hide(replay_proofs::allowed);hide(replay_proofs::pending);
        proof{
            self.trace@.layout(self.records(),self.namespace().owner);
            self.trace@.entry(self.records(),self.namespace().owner,index as int-1);
        }
        let ghost before=self.actor;let ghost origins=self.origins@;let ghost operation=op;
        let ghost cache=(self.plans@,self.pending);let ghost prior_steps=self.trace@.steps.take(index as int-1);
        proof{codec::operation_unique(op,self.trace@.steps[index as int-1].operation);}
        proof{replay_proofs::operation_agrees(prior_steps,op,self.trace@.steps[index as int-1].operation);}
        let mut status=Status::Ok;
        match op{
            Operation::Bootstrap{..}=>return false,
            Operation::Bind(plan)=>{
                if self.plan_index(plan.generation).is_some() || plan.generation==0{return false;}
                self.plans.push(plan);
            }
            Operation::Deliver{plan,command}=>{
                if self.pending.is_some(){return false;}
                let i=match self.plan_index(plan.generation){Some(i)=>i,None=>return false};
                if !storage::plans_match(&self.plans[i],&plan){return false;}
                status=self.actor.deliver(&plan,command).status;
            }
            Operation::Begin{id,incarnation}=>{status=self.begin_origin(id,incarnation);}
            Operation::Resolve(id)=>{status=self.actor.resolve(id);}
            Operation::Finish(id)=>{status=self.actor.finish(id);}
            Operation::Acquire{id,range,point,grant}=>{
                status=if point{
                    if range.hi.is_some(){return false;}self.actor.acquire(id,range.table,&range.lo,grant)
                }else{self.actor.acquire_range(id,range.table,&range.lo,match &range.hi{Some(h)=>Some(h.as_slice()),None=>None},grant)};
            }
            Operation::Release{id,table,coordinate}=>{status=self.actor.release(id,table,&coordinate);}
            Operation::Reserve{plan,cleanup}=>{
                let generation=plan.generation;
                if self.pending.is_some(){return false;}
                let i=match self.plan_index(generation){Some(i)=>i,None=>return false};
                if !storage::plans_match(&self.plans[i],&plan){return false;}
                if !matches!(self.actor.authorize_transfer(&self.plans[i],cleanup),Status::Ok){return false;}
                proof{self.actor.matching_transfer(self.plans@[i as int],plan,cleanup);}
                self.pending=Some((generation,cleanup));
            }
            Operation::Settled=>{if self.pending.is_none(){return false;}self.pending=None;}
            Operation::Complete{plan:recorded_plan,cleanup}=>{
                let generation=recorded_plan.generation;
                if self.pending!=Some((generation,cleanup)){return false;}
                let i=match self.plan_index(generation){Some(i)=>i,None=>return false};
                if !storage::plans_match(&self.plans[i],&recorded_plan){return false;}
                proof{
                    assert(history::is_completion(self.records()[index as int]));
                    history::completion_at(self.records(),self.namespace().owner,self.facts@,self.trace@,index as int);
                    history::completion_unique(recorded_plan,cleanup,self.facts@[index as int].plan,self.facts@[index as int].cleanup);
                    history::known_completion(self.records(),self.facts@,index as int);
                }
                let plan=&recorded_plan;
                let receipt=ReplayCompletion{plan,owner:self.actor.owner(),cleanup,index,records:Ghost(self.records()),fact:Ghost(self.facts@[index as int])};
                let result=if cleanup{
                    let token=match storage::recover_cleanup(plan,self.actor.owner(),&receipt){Some(v)=>v,None=>return false};
                    self.actor.complete_cleanup(plan,token)
                }else{
                    let token=match storage::recover_copy(plan,self.actor.owner(),&receipt){Some(v)=>v,None=>return false};
                    self.actor.seal(plan,&token)
                };
                if result.certificate.is_none(){return false;}status=result.status;
                self.completed.push((generation,cleanup));self.pending=None;
                proof{
                    assert forall|j:int|0<=j<self.completed.len() implies
                        self.completion_record(self.completed@[j].0,self.completed@[j].1) by{
                        if j==self.completed.len()-1{
                            assert(self.completed@[j]==(generation,cleanup));
                        }
                    }
                }
            }
        }
        proof{
            if self.actor.same_metadata(before){self.actor.metadata_observations(before);}
            let step=NativeStep{before,after:self.actor,operation,status,origins_before:origins,origins_after:self.origins@};
            assert(step.valid()) by{reveal(NativeStep::valid);}
            replay_proofs::cache_append(cache.0,cache.1,prior_steps,operation,self.trace@.steps[index as int-1],self.plans@,self.pending);
            assert(prior_steps.push(self.trace@.steps[index as int-1])==self.trace@.steps.take(index as int));
        }
        true
    }
    /// Local authenticated frozen-source implementation; an RPC implementation
    /// must provide the same existing Source contract. Both native borrows stay
    /// held through the destination transfer, so no queued work can swap ownership versions.
    pub fn source<'a,E:DurableEngine>(&'a self,engine:&'a E,a:Admission,generation:u64)
        ->(out:Result<DurableSource<'a,E>,ParticipantError>)
        requires self.wf(),engine.wf(),
        ensures out is Ok ==> out->Ok_0.wf(),
    {
        hide(codec::wire);hide(Participant::semantic_eq);hide(Participant::capture_authorized);
        if !self.check(&a) || !engine.check(&a){return Err(ParticipantError::Fenced);}
        let index=match self.plan_index(generation){Some(i)=>i,None=>return Err(ParticipantError::Native(Status::NotFound))};
        let status=self.actor.authorize_capture(&self.plans[index]);
        if !matches!(status,Status::Ok){return Err(ParticipantError::Native(status));}
        Ok(DurableSource{participant:self,engine,admission:a,index})
    }
}

pub struct DurableSource<'a,E:DurableEngine>{
    participant:&'a DurableParticipant,
    engine:&'a E,
    admission:Admission,
    index:usize,
}
impl<'a,E:DurableEngine> Source for DurableSource<'a,E>{
    closed spec fn image(&self)->storage::Image{self.engine.working_state().bytes}
    closed spec fn wf(&self)->bool{self.participant.wf() && self.engine.wf() && self.index<self.participant.plans.len()}
    closed spec fn available(&self,plan:MigrationPlan)->bool{false}
    fn scan(&self,plan:&MigrationPlan,after:Option<&Row>)->(out:Result<Option<Row>,Status>){
        hide(codec::wire);hide(Participant::semantic_eq);hide(Participant::capture_authorized);hide(storage::same_plan);
        let retained=&self.participant.plans[self.index];
        if !self.participant.check(&self.admission) || !storage::plans_match(retained,plan){return Err(Status::Retry);}
        let status=self.participant.actor.authorize_capture(retained);
        if !matches!(status,Status::Ok){return Err(status);}
        self.engine.scan(&self.admission,&plan.range,after)
    }
}
} // verus!
