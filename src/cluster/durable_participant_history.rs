//! Reachable durable prefixes: native semantic execution plus earned raw rows.
//! Neither the byte-KV primitive nor the engine assumes these predicates.
use vstd::prelude::*;
use crate::types::*;
use crate::participant::Participant;
use crate::storage::{self,Image};
use crate::durable_kv::{self,DurableKv,Namespace};
use super::codec::{self,Operation};
use super::replay_proofs::{self,ActorHistory,NativeStep};
verus! {
pub struct CompletionFact {
    pub plan:MigrationPlan,pub owner:u32,pub cleanup:bool,pub before:Participant,pub after:Participant,
    pub initial:Image,pub copied:Image,pub source:Image,pub durable:Image,
}
impl CompletionFact {
    #[verifier::opaque]
    pub open spec fn valid(self)->bool {
        self.before.wf() && self.after.wf() && self.before.owner_view()==self.owner && self.after.owner_view()==self.owner
            && self.before.transfer_authorized(self.plan,self.cleanup) && self.after.lease_view()==self.before.lease_view()
            && self.after.table_ids()==self.before.table_ids()
            && storage::preserves_outside(self.copied,self.initial,self.plan.range)
            && self.durable==self.copied && storage::complete(self.copied,self.initial,self.source,self.plan.range)
            && if self.cleanup{self.source==Map::empty() && self.after.cleanup_done(self.plan.generation) && self.after.local_unchanged(self.before)}
                else{self.owner==self.plan.destination && self.after.seal_frame(self.before,self.plan)}
    }
}
pub open spec fn is_completion(bytes:Seq<u8>)->bool {
    exists|plan:MigrationPlan,cleanup:bool|bytes==codec::wire(Operation::Complete{plan,cleanup})
}
#[verifier::opaque]
pub open spec fn has_completion(records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,generation:u64,cleanup:bool)->bool {
    exists|i:int|0<=i<records.len() && facts.contains_key(i) && facts[i].valid()
        && facts[i].plan.generation==generation && facts[i].cleanup==cleanup
        && records[i]==codec::wire(Operation::Complete{plan:facts[i].plan,cleanup})
}
pub proof fn completion_index(records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,generation:u64,cleanup:bool)->(index:int)
    requires has_completion(records,facts,generation,cleanup),
    ensures 0<=index<records.len(),facts.contains_key(index),facts[index].valid(),
        facts[index].plan.generation==generation,facts[index].cleanup==cleanup,
        records[index]==codec::wire(Operation::Complete{plan:facts[index].plan,cleanup}),
{
    hide(codec::wire);
    reveal(has_completion);
    choose|i:int|0<=i<records.len() && facts.contains_key(i) && facts[i].valid()
        && facts[i].plan.generation==generation && facts[i].cleanup==cleanup
        && records[i]==codec::wire(Operation::Complete{plan:facts[i].plan,cleanup})
}
pub proof fn known_completion(records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,index:int)
    requires 0<=index<records.len(),facts.contains_key(index),facts[index].valid(),
        records[index]==codec::wire(Operation::Complete{plan:facts[index].plan,cleanup:facts[index].cleanup}),
    ensures has_completion(records,facts,facts[index].plan.generation,facts[index].cleanup),
{
    hide(codec::wire);
    reveal(has_completion);
}
pub proof fn completion_prefix(before:Seq<Seq<u8>>,after:Seq<Seq<u8>>,
    old_facts:Map<int,CompletionFact>,new_facts:Map<int,CompletionFact>)
    requires before.len()<=after.len(),after.take(before.len() as int)==before,
        forall|i:int|0<=i<before.len() && old_facts.contains_key(i) ==> new_facts.contains_key(i) && new_facts[i]==old_facts[i],
    ensures forall|generation:u64,cleanup:bool|has_completion(before,old_facts,generation,cleanup)
        ==> has_completion(after,new_facts,generation,cleanup),
{
    hide(codec::wire);
    assert forall|generation:u64,cleanup:bool|has_completion(before,old_facts,generation,cleanup)
        implies has_completion(after,new_facts,generation,cleanup) by{
        let i=completion_index(before,old_facts,generation,cleanup);
        assert(after[i]==before[i]);assert(new_facts[i]==old_facts[i]);
        known_completion(after,new_facts,i);
    }
}
#[verifier::opaque]
pub open spec fn history_valid(records:Seq<Seq<u8>>,owner:u32,facts:Map<int,CompletionFact>)->bool {
    forall|i:int|0<=i<records.len() && #[trigger] is_completion(records[i]) ==> facts.contains_key(i) && facts[i].valid() && facts[i].owner==owner
        && records[i]==codec::wire(Operation::Complete{plan:facts[i].plan,cleanup:facts[i].cleanup})
        && i>0 && records[i-1]==codec::wire(Operation::Reserve{plan:facts[i].plan,cleanup:facts[i].cleanup})
}
#[verifier::opaque]
pub open spec fn facts_match(records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,trace:ActorHistory)->bool {
    forall|i:int|0<=i<records.len() && #[trigger] is_completion(records[i]) ==> 0<i && i-1<trace.steps.len()
        && facts.contains_key(i) && facts[i].before.semantic_eq(trace.steps[i-1].before)
        && facts[i].after.semantic_eq(trace.steps[i-1].after)
}
pub proof fn completion_at(records:Seq<Seq<u8>>,owner:u32,facts:Map<int,CompletionFact>,trace:ActorHistory,index:int)
    requires history_valid(records,owner,facts),facts_match(records,facts,trace),
        0<=index<records.len(),is_completion(records[index]),
    ensures 0<index,index-1<trace.steps.len(),facts.contains_key(index),facts[index].valid(),facts[index].owner==owner,
        records[index]==codec::wire(Operation::Complete{plan:facts[index].plan,cleanup:facts[index].cleanup}),
        records[index-1]==codec::wire(Operation::Reserve{plan:facts[index].plan,cleanup:facts[index].cleanup}),
        facts[index].before.semantic_eq(trace.steps[index-1].before),facts[index].after.semantic_eq(trace.steps[index-1].after),
{
    hide(Participant::semantic_eq);hide(codec::wire);
    reveal(history_valid);reveal(facts_match);
    assert(is_completion(records[index]));
}
pub open spec fn map_history(map:Map<Seq<u8>,Seq<u8>>,ns:Namespace)->bool {
    exists|w:(u64,Seq<Seq<u8>>,Map<int,CompletionFact>,ActorHistory)|
        0<w.1.len()<=u64::MAX && durable_kv::has_prefix(map,ns,w.0,w.1)
            && history_valid(w.1,ns.owner,w.2) && w.3.valid(w.1,ns.owner) && facts_match(w.1,w.2,w.3)
}
pub open spec fn reachable_map(map:Map<Seq<u8>,Seq<u8>>,ns:Namespace)->bool {
    !map.contains_key(durable_kv::key_wire(ns,false,0)) || map_history(map,ns)
}
pub open spec fn durable_history<K:DurableKv>(kv:&K,ns:Namespace)->bool {
    forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(kv.committed(),kv.pending(),candidate) ==> reachable_map(candidate,ns)
}
/// Journal recovery may settle an Unknown batch either way; retain the already
/// established history predicate for every remaining settlement possibility.
pub proof fn settled_history<K:DurableKv>(kv:&K,before:Map<Seq<u8>,Seq<u8>>,
    pending:Option<Seq<durable_kv::WriteOp>>,ns:Namespace)
    requires forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(before,pending,candidate) ==> reachable_map(candidate,ns),
        forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(kv.committed(),kv.pending(),candidate)
            ==> durable_kv::settled_map(before,pending,candidate),
    ensures durable_history(kv,ns),reachable_map(kv.committed(),ns),
{
    hide(map_history);
    assert forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(kv.committed(),kv.pending(),candidate)
        implies reachable_map(candidate,ns) by{
        assert(durable_kv::settled_map(before,pending,candidate));
    }
    assert(durable_kv::settled_map(kv.committed(),kv.pending(),kv.committed()));
}
pub proof fn witnessed_map(map:Map<Seq<u8>,Seq<u8>>,ns:Namespace,incarnation:u64,records:Seq<Seq<u8>>,
    facts:Map<int,CompletionFact>,trace:ActorHistory)
    requires 0<records.len()<=u64::MAX,durable_kv::has_prefix(map,ns,incarnation,records),history_valid(records,ns.owner,facts),trace.valid(records,ns.owner),
        facts_match(records,facts,trace),
    ensures map_history(map,ns),
{
    let witness=(incarnation,records,facts,trace);
    assert(0<witness.1.len()<=u64::MAX && durable_kv::has_prefix(map,ns,witness.0,witness.1)
        && history_valid(witness.1,ns.owner,witness.2) && witness.3.valid(witness.1,ns.owner) && facts_match(witness.1,witness.2,witness.3));
}
pub proof fn created_history<K:DurableKv>(kv:&K,before:Map<Seq<u8>,Seq<u8>>,pending:Option<Seq<durable_kv::WriteOp>>,
    ns:Namespace,trace:ActorHistory)
    requires trace.valid(seq![codec::wire(trace.bootstrap)],ns.owner),
        forall|base:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(before,pending,base) ==> reachable_map(base,ns),
        durable_kv::bootstrap_options(kv,before,pending,ns,codec::wire(trace.bootstrap)),
    ensures durable_history(kv,ns),
        history_valid(seq![codec::wire(trace.bootstrap)],ns.owner,Map::empty()),
        facts_match(seq![codec::wire(trace.bootstrap)],Map::empty(),trace),
{
    reveal(history_valid);reveal(facts_match);
    trace.layout(seq![codec::wire(trace.bootstrap)],ns.owner);
    let bytes=codec::wire(trace.bootstrap);
    not_completion(trace.bootstrap);
    assert(history_valid(seq![bytes],ns.owner,Map::empty()));
    assert forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(kv.committed(),kv.pending(),candidate)
        implies reachable_map(candidate,ns) by{
        let base=choose|base:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(before,pending,base)
            && (candidate==base || candidate==durable_kv::bootstrap_map(base,ns,bytes));
        if candidate!=base{
            durable_kv::bootstrap_prefix(base,ns,bytes);
            witnessed_map(candidate,ns,0,seq![bytes],Map::empty(),trace);
        }
    }
}
pub proof fn appended_history<K:DurableKv>(kv:&K,before:Map<Seq<u8>,Seq<u8>>,ns:Namespace,incarnation:u64,records:Seq<Seq<u8>>,
    bytes:Seq<u8>,old_facts:Map<int,CompletionFact>,new_facts:Map<int,CompletionFact>,trace:ActorHistory,step:NativeStep)
    requires 0<records.len()<u64::MAX,durable_kv::has_prefix(before,ns,incarnation,records),history_valid(records,ns.owner,old_facts),
        history_valid(records.push(bytes),ns.owner,new_facts),trace.valid(records,ns.owner),step.valid(),step.before.semantic_eq(trace.endpoint()),
        step.origins_before==trace.origins(),codec::wire(step.operation)==bytes,
        facts_match(records,old_facts,trace),facts_match(records.push(bytes),new_facts,trace.append(step)),replay_proofs::allowed(trace.steps,step.operation),
        durable_kv::settlement_options(kv,before,durable_kv::append_map(before,ns,incarnation,records,bytes)),
    ensures durable_history(kv,ns),trace.append(step).valid(records.push(bytes),ns.owner),
{
    hide(NativeStep::valid);hide(Participant::semantic_eq);hide(codec::wire);
    replay_proofs::extend(trace,records,ns.owner,step);
    witnessed_map(before,ns,incarnation,records,old_facts,trace);
    durable_kv::append_prefix(before,ns,incarnation,records,bytes);
    witnessed_map(durable_kv::append_map(before,ns,incarnation,records,bytes),ns,incarnation,records.push(bytes),new_facts,trace.append(step));
    assert forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(kv.committed(),kv.pending(),candidate) implies map_history(candidate,ns) by{};
}
pub proof fn activated_history<K:DurableKv>(kv:&K,before:Map<Seq<u8>,Seq<u8>>,ns:Namespace,old_inc:u64,new_inc:u64,
    records:Seq<Seq<u8>>,facts:Map<int,CompletionFact>,trace:ActorHistory)
    requires 0<records.len()<=u64::MAX,durable_kv::has_prefix(before,ns,old_inc,records),history_valid(records,ns.owner,facts),trace.valid(records,ns.owner),
        facts_match(records,facts,trace),
        durable_kv::settlement_options(kv,before,durable_kv::activation_map(before,ns,new_inc,records)),
    ensures durable_history(kv,ns),
{
    hide(codec::wire);
    witnessed_map(before,ns,old_inc,records,facts,trace);durable_kv::activate_prefix(before,ns,old_inc,new_inc,records);
    witnessed_map(durable_kv::activation_map(before,ns,new_inc,records),ns,new_inc,records,facts,trace);
    assert forall|candidate:Map<Seq<u8>,Seq<u8>>|durable_kv::settled_map(kv.committed(),kv.pending(),candidate) implies map_history(candidate,ns) by{};
}
pub proof fn recovered_history<K:DurableKv>(journal:&durable_kv::Journal,kv:&K)->(out:(Map<int,CompletionFact>,ActorHistory))
    requires journal.wf(),journal.consistent(kv),map_history(kv.committed(),journal.namespace_spec()),
    ensures history_valid(journal.records(),journal.namespace_spec().owner,out.0),out.1.valid(journal.records(),journal.namespace_spec().owner),
        facts_match(journal.records(),out.0,out.1),
{
    journal.committed_records(kv);let ns=journal.namespace_spec();
    let w=choose|w:(u64,Seq<Seq<u8>>,Map<int,CompletionFact>,ActorHistory)|
        0<w.1.len()<=u64::MAX && durable_kv::has_prefix(kv.committed(),ns,w.0,w.1)
            && history_valid(w.1,ns.owner,w.2) && w.3.valid(w.1,ns.owner) && facts_match(w.1,w.2,w.3);
    durable_kv::committed_prefix_unique(kv.committed(),ns,w.0,w.1,journal.incarnation_spec(),journal.records());
    (w.2,w.3)
}
pub proof fn completion_unique(p:MigrationPlan,c:bool,q:MigrationPlan,d:bool)
    requires codec::wire(Operation::Complete{plan:p,cleanup:c})==codec::wire(Operation::Complete{plan:q,cleanup:d}),
    ensures storage::same_plan(p,q),c==d,
{codec::operation_unique(Operation::Complete{plan:p,cleanup:c},Operation::Complete{plan:q,cleanup:d});}
pub proof fn not_completion(op:Operation)
    requires !(op is Complete),ensures !is_completion(codec::wire(op)),
{
    assert forall|p:MigrationPlan,c:bool|codec::wire(op)!=codec::wire(Operation::Complete{plan:p,cleanup:c}) by{
        let a=codec::wire(op);let b=codec::wire(Operation::Complete{plan:p,cleanup:c});assert(b[8]==9u8);
        match op{Operation::Bootstrap{..}=>{},Operation::Bind(_)=>{},Operation::Deliver{..}=>{},Operation::Begin{..}=>{},
            Operation::Resolve(_)=>{},Operation::Finish(_)=>{},Operation::Acquire{..}=>{},Operation::Release{..}=>{},
            Operation::Reserve{..}=>{},Operation::Settled=>{},Operation::Complete{..}=>{}}
        assert(a[8]!=b[8]);
    }
}
pub proof fn append_ordinary(records:Seq<Seq<u8>>,owner:u32,facts:Map<int,CompletionFact>,bytes:Seq<u8>,trace:ActorHistory,step:NativeStep)
    requires history_valid(records,owner,facts),!is_completion(bytes),facts_match(records,facts,trace),
    ensures history_valid(records.push(bytes),owner,facts),facts_match(records.push(bytes),facts,trace.append(step)),
{
    hide(Participant::semantic_eq);hide(codec::wire);
    reveal(history_valid);reveal(facts_match);
    assert forall|i:int|0<=i<records.push(bytes).len() && is_completion(records.push(bytes)[i]) implies
        facts.contains_key(i) && facts[i].valid() && facts[i].owner==owner
            && records.push(bytes)[i]==codec::wire(Operation::Complete{plan:facts[i].plan,cleanup:facts[i].cleanup})
            && i>0 && records.push(bytes)[i-1]==codec::wire(Operation::Reserve{plan:facts[i].plan,cleanup:facts[i].cleanup}) by{
        if i==records.len(){assert(records.push(bytes)[i]==bytes);}else{
            assert(records.push(bytes)[i]==records[i]);assert(is_completion(records[i]));
            assert(facts.contains_key(i));assert(i>0);
            assert(records.push(bytes)[i-1]==records[i-1]);
        }
    }
    assert forall|i:int|0<=i<records.push(bytes).len() && is_completion(records.push(bytes)[i]) implies 0<i
        && i-1<trace.append(step).steps.len() && facts.contains_key(i)
        && facts[i].before.semantic_eq(trace.append(step).steps[i-1].before)
        && facts[i].after.semantic_eq(trace.append(step).steps[i-1].after) by{
        if i==records.len(){assert(records.push(bytes)[i]==bytes);}else{
            assert(records.push(bytes)[i]==records[i]);assert(is_completion(records[i]));
            assert(0<i && i-1<trace.steps.len());
            assert(trace.append(step).steps[i-1]==trace.steps[i-1]);
        }
    }
}
pub proof fn append_completion(records:Seq<Seq<u8>>,owner:u32,facts:Map<int,CompletionFact>,fact:CompletionFact,trace:ActorHistory,step:NativeStep)
    requires history_valid(records,owner,facts),fact.valid(),fact.owner==owner,records.len()>0,
        records.last()==codec::wire(Operation::Reserve{plan:fact.plan,cleanup:fact.cleanup}),
        facts_match(records,facts,trace),records.len()==trace.steps.len()+1,
        fact.before.semantic_eq(step.before),fact.after.semantic_eq(step.after),
    ensures history_valid(records.push(codec::wire(Operation::Complete{plan:fact.plan,cleanup:fact.cleanup})),owner,facts.insert(records.len() as int,fact)),
        facts_match(records.push(codec::wire(Operation::Complete{plan:fact.plan,cleanup:fact.cleanup})),facts.insert(records.len() as int,fact),trace.append(step)),
{
    hide(Participant::semantic_eq);hide(codec::wire);
    reveal(history_valid);reveal(facts_match);
    let bytes=codec::wire(Operation::Complete{plan:fact.plan,cleanup:fact.cleanup});let next=facts.insert(records.len() as int,fact);
    assert forall|i:int|0<=i<records.push(bytes).len() && is_completion(records.push(bytes)[i]) implies
        next.contains_key(i) && next[i].valid() && next[i].owner==owner
            && records.push(bytes)[i]==codec::wire(Operation::Complete{plan:next[i].plan,cleanup:next[i].cleanup})
            && i>0 && records.push(bytes)[i-1]==codec::wire(Operation::Reserve{plan:next[i].plan,cleanup:next[i].cleanup}) by{
        if i==records.len(){
            assert(next[i]==fact);assert(records.push(bytes)[i]==bytes);
            assert(records.push(bytes)[i-1]==records.last());
        }else{
            assert(records.push(bytes)[i]==records[i]);assert(is_completion(records[i]));
            assert(facts.contains_key(i));assert(next[i]==facts[i]);assert(i>0);
            assert(records.push(bytes)[i-1]==records[i-1]);
        }
    }
    assert forall|i:int|0<=i<records.push(bytes).len() && is_completion(records.push(bytes)[i]) implies 0<i
        && i-1<trace.append(step).steps.len() && next.contains_key(i)
        && next[i].before.semantic_eq(trace.append(step).steps[i-1].before)
        && next[i].after.semantic_eq(trace.append(step).steps[i-1].after) by{
        if i==records.len(){assert(trace.append(step).steps[i-1]==step);}else{
            assert(records.push(bytes)[i]==records[i]);assert(is_completion(records[i]));
            assert(0<i && i-1<trace.steps.len());
            assert(next[i]==facts[i]);assert(trace.append(step).steps[i-1]==trace.steps[i-1]);
        }
    }
}
} // verus!
