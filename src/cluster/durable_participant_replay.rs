//! Exact observable replay relation derived from native actor contracts.
use vstd::prelude::*;
use crate::types::*;
use crate::participant::Participant;
#[cfg(verus_keep_ghost)]
use crate::participant::applied_receipt;
use crate::leases;
use super::codec::{self,Operation};
verus! {
pub struct NativeStep {
    pub before:Participant,pub after:Participant,pub operation:Operation,pub status:Status,
    pub origins_before:Map<u64,(u64,u64)>,pub origins_after:Map<u64,(u64,u64)>,
}
pub open spec fn origin_rejected(origins:Map<u64,(u64,u64)>,id:TxnId,incarnation:u64)->bool {
    origins.contains_key(id.client) && origins[id.client].0==id.sequence && origins[id.client].1!=incarnation
}
impl NativeStep {
    pub open spec fn valid(self)->bool {
        self.before.wf() && self.after.wf() && self.after.owner_view()==self.before.owner_view()
        && (match self.operation {
            Operation::Begin{id,incarnation}=>self.after.same_metadata(self.before)
                && if origin_rejected(self.origins_before,id,incarnation){
                    self.status==Status::Retry && self.after.lease_view()==self.before.lease_view()
                        && self.origins_after==self.origins_before
                }else{leases::begin_effect(self.before.lease_view(),self.after.lease_view(),id,self.status)
                    && self.origins_after==if self.status==Status::Ok{
                        self.origins_before.insert(id.client,(id.sequence,incarnation))
                    }else{self.origins_before}},
            _=>self.origins_after==self.origins_before && match self.operation {
                Operation::Bootstrap{..}|Operation::Begin{..}=>false,
                Operation::Bind(_)|Operation::Settled=>self.status==Status::Ok && self.after==self.before,
                Operation::Reserve{plan,cleanup}=>self.status==Status::Ok && self.after==self.before
                    && self.before.transfer_authorized(plan,cleanup),
                Operation::Resolve(id)=>self.after.same_metadata(self.before)
                    && leases::resolve_effect(self.before.lease_view(),self.after.lease_view(),id,self.status),
                Operation::Finish(id)=>self.after.same_metadata(self.before)
                    && leases::finish_effect(self.before.lease_view(),self.after.lease_view(),id,self.status),
                Operation::Release{id,table,coordinate}=>self.after.same_metadata(self.before)
                    && leases::release_effect(self.before.lease_view(),self.after.lease_view(),id,table,coordinate@,self.status),
                Operation::Acquire{id,range,point,grant}=>self.after.same_metadata(self.before)
                    && if point{self.before.point_acquire_effect(self.after,id,range.table,range.lo@,grant,self.status)}else{
                        self.before.range_acquire_effect(self.after,id,range.table,range.lo@,leases::range_hi(range),grant,self.status)},
                Operation::Deliver{plan,command}=>self.after.lease_view()==self.before.lease_view()
                    && self.after.table_ids()==self.before.table_ids()
                    && ((self.status==Status::Ok)==self.before.delivery_authorized(plan,command))
                    && (self.status!=Status::Ok || self.before.command_seen(plan.generation,command) ==> self.after.same_metadata(self.before))
                    && (self.status==Status::Ok && !self.before.command_seen(plan.generation,command) ==> self.after.control_frame(self.before,plan,command))
                    && self.after.receipt_view()==if self.status==Status::Ok && !self.before.command_seen(plan.generation,command){
                        applied_receipt(self.before.receipt_view(),plan.generation,command,self.before.owner_view()==plan.source)
                    }else{self.before.receipt_view()},
                Operation::Complete{plan,cleanup}=>self.status==Status::Ok
                    && self.after.lease_view()==self.before.lease_view() && self.after.table_ids()==self.before.table_ids()
                    && self.after.receipt_view()=={
                        let r=self.before.receipt_view()[plan.generation];
                        self.before.receipt_view().insert(plan.generation,if cleanup{(r.0,r.1,true,r.3)}else{(r.0,r.1,r.2,true)})
                    }
                    && if cleanup{self.after.cleanup_done(plan.generation) && self.after.local_unchanged(self.before)}else{
                        self.before.ready_seen(plan.generation) && self.after.same_metadata(self.before)
                            || !self.before.ready_seen(plan.generation) && self.after.seal_frame(self.before,plan)
                    },
            }
        })
    }
}
pub open spec fn bound(steps:Seq<NativeStep>,plan:MigrationPlan)->bool {
    exists|i:int|0<=i<steps.len() && match #[trigger] steps[i].operation {
        Operation::Bind(p)=>crate::storage::same_plan(p,plan),_=>false,
    }
}
pub open spec fn bound_generation(steps:Seq<NativeStep>,generation:u64)->bool {
    exists|i:int|0<=i<steps.len() && match #[trigger] steps[i].operation {
        Operation::Bind(p)=>p.generation==generation,_=>false,
    }
}
pub open spec fn pending(steps:Seq<NativeStep>)->Option<(MigrationPlan,bool)>
    decreases steps.len(),
{
    if steps.len()==0{None}else{match steps.last().operation {
        Operation::Reserve{plan,cleanup}=>Some((plan,cleanup)),
        Operation::Complete{..}|Operation::Settled=>None,
        _=>pending(steps.drop_last()),
    }}
}
pub open spec fn allowed(steps:Seq<NativeStep>,op:Operation)->bool {
    match pending(steps) {
        Some((p,c))=>match op{
            Operation::Complete{plan,cleanup}=>crate::storage::same_plan(p,plan) && c==cleanup && bound(steps,plan),
            Operation::Settled=>true,_=>false,
        },
        None=>match op {
            Operation::Bootstrap{..}|Operation::Complete{..}|Operation::Settled=>false,
            Operation::Bind(plan)=>plan.generation!=0 && plan.old.len()>0 && !bound_generation(steps,plan.generation),
            Operation::Deliver{plan,..}|Operation::Reserve{plan,..}=>bound(steps,plan),
            _=>true,
        },
    }
}
#[verifier::opaque]
pub open spec fn cache_matches(plans:Seq<MigrationPlan>,reservation:Option<(u64,bool)>,steps:Seq<NativeStep>)->bool {
    (forall|i:int|0<=i<plans.len() ==> bound(steps,plans[i]))
        && (forall|i:int|0<=i<steps.len() ==> match #[trigger] steps[i].operation {
            Operation::Bind(p)=>exists|j:int|0<=j<plans.len() && crate::storage::same_plan(p,plans[j]),_=>true,
        })
        && reservation==match pending(steps){Some((p,c))=>Some((p.generation,c)),None=>None}
}
pub proof fn empty_cache()
    ensures cache_matches(Seq::empty(),None,Seq::empty()),
{reveal(cache_matches);}
pub proof fn cache_facts(plans:Seq<MigrationPlan>,reservation:Option<(u64,bool)>,steps:Seq<NativeStep>)
    requires cache_matches(plans,reservation,steps),
    ensures forall|i:int|0<=i<plans.len() ==> bound(steps,plans[i]),
        forall|i:int|0<=i<steps.len() ==> match #[trigger] steps[i].operation{
            Operation::Bind(p)=>exists|j:int|0<=j<plans.len() && crate::storage::same_plan(p,plans[j]),_=>true},
        reservation==match pending(steps){Some((p,c))=>Some((p.generation,c)),None=>None},
{reveal(cache_matches);}
proof fn cache_operation_shape(a:Operation,b:Operation)
    requires codec::same_operation(a,b),
    ensures (a is Bind)==(b is Bind),(a is Reserve)==(b is Reserve),
        (a is Complete)==(b is Complete),(a is Settled)==(b is Settled),
        a is Bind ==> crate::storage::same_plan(b->Bind_0,a->Bind_0),
        a is Reserve ==> a->Reserve_plan.generation==b->Reserve_plan.generation && a->Reserve_cleanup==b->Reserve_cleanup,
{
    match (a,b){
        (Operation::Bind(p),Operation::Bind(q))=>{
            assert forall|i:int|0<=i<q.old.len() implies q.old@[i].start@==p.old@[i].start@
                && q.old@[i].grant==p.old@[i].grant by{};
        },
        _=>{},
    }
}
proof fn pending_append(steps:Seq<NativeStep>,op:Operation,step:NativeStep)
    requires codec::same_operation(op,step.operation),
    ensures (match pending(steps.push(step)){Some((p,c))=>Some((p.generation,c)),None=>None})
        == match op{
            Operation::Reserve{plan,cleanup}=>Some((plan.generation,cleanup)),
            Operation::Complete{..}|Operation::Settled=>None,
            _=>match pending(steps){Some((p,c))=>Some((p.generation,c)),None=>None},
        },
{
    hide(codec::same_operation);
    cache_operation_shape(op,step.operation);
    assert(steps.push(step).drop_last()==steps);
    assert(steps.push(step).last()==step);
    match op{Operation::Reserve{..}=>{},Operation::Complete{..}=>{},Operation::Settled=>{},_=>{}}
}
pub open spec fn cache_effect(before:Seq<MigrationPlan>,reservation:Option<(u64,bool)>,op:Operation,
    after:Seq<MigrationPlan>,next:Option<(u64,bool)>)->bool {
    match op{
        Operation::Bind(plan)=>after==before.push(plan) && next==reservation,
        Operation::Reserve{plan,cleanup}=>after==before && next==Some((plan.generation,cleanup)),
        Operation::Complete{..}|Operation::Settled=>after==before && next==None,
        _=>after==before && next==reservation,
    }
}
pub proof fn cache_append(before:Seq<MigrationPlan>,reservation:Option<(u64,bool)>,steps:Seq<NativeStep>,
    op:Operation,step:NativeStep,after:Seq<MigrationPlan>,next:Option<(u64,bool)>)
    requires cache_matches(before,reservation,steps),allowed(steps,op),codec::same_operation(op,step.operation),
        cache_effect(before,reservation,op,after,next),
    ensures cache_matches(after,next,steps.push(step)),
{
    hide(codec::same_operation);hide(allowed);hide(pending);
    reveal(cache_matches);
    cache_operation_shape(op,step.operation);pending_append(steps,op,step);
    match op{
        Operation::Bind(plan)=>{
            assert forall|i:int|0<=i<after.len() implies bound(steps.push(step),after[i]) by{
                if i==before.len(){assert(steps.push(step)[steps.len() as int].operation==step.operation);}
                else{let j=choose|j:int|0<=j<steps.len() && match #[trigger] steps[j].operation {
                    Operation::Bind(p)=>crate::storage::same_plan(p,after[i]),_=>false};
                    assert(steps.push(step)[j]==steps[j]);}
            }
        },
        _=>{
            assert forall|i:int|0<=i<after.len() implies bound(steps.push(step),after[i]) by{
                let j=choose|j:int|0<=j<steps.len() && match #[trigger] steps[j].operation{
                    Operation::Bind(p)=>crate::storage::same_plan(p,after[i]),_=>false};
                assert(steps.push(step)[j]==steps[j]);
            }
        }
    }
    assert forall|i:int|0<=i<steps.push(step).len() implies match #[trigger] steps.push(step)[i].operation{
        Operation::Bind(p)=>exists|j:int|0<=j<after.len() && crate::storage::same_plan(p,after[j]),_=>true,
    } by{
        if i==steps.len(){if let Operation::Bind(p)=op{assert(after[before.len() as int]==p);}}
        else if let Operation::Bind(p)=steps[i].operation{
            let j=choose|j:int|0<=j<before.len() && crate::storage::same_plan(p,before[j]);
            assert(after[j]==before[j]);
        }
    }
    assert(steps.push(step).drop_last()==steps);
}
pub proof fn operation_agrees(steps:Seq<NativeStep>,a:Operation,b:Operation)
    requires codec::same_operation(a,b),
    ensures allowed(steps,a)==allowed(steps,b),
{
    match (a,b){
        (Operation::Bind(_),Operation::Bind(_))| (Operation::Deliver{..},Operation::Deliver{..})
        |(Operation::Reserve{..},Operation::Reserve{..})|(Operation::Complete{..},Operation::Complete{..})=>{},
        _=>{},
    }
}
pub open spec fn history_entry(steps:Seq<NativeStep>,initial:Participant,records:Seq<Seq<u8>>,index:int)->bool {
    steps[index].valid() && codec::encodable(steps[index].operation)
        && codec::wire(steps[index].operation)==records[index+1]
        && steps[index].before.semantic_eq(if index==0{initial}else{steps[index-1].after})
        && steps[index].origins_before==(if index==0{Map::empty()}else{steps[index-1].origins_after})
        && allowed(steps.take(index),steps[index].operation)
}
pub struct ActorHistory {pub bootstrap:Operation,pub initial:Participant,pub steps:Seq<NativeStep>}
impl ActorHistory {
    pub open spec fn endpoint(self)->Participant{if self.steps.len()==0{self.initial}else{self.steps.last().after}}
    pub open spec fn origins(self)->Map<u64,(u64,u64)>{if self.steps.len()==0{Map::empty()}else{self.steps.last().origins_after}}
    #[verifier::opaque]
    pub open spec fn valid(self,records:Seq<Seq<u8>>,owner:u32)->bool {
        records.len()==self.steps.len()+1 && records[0]==codec::wire(self.bootstrap)
            && (match self.bootstrap{Operation::Bootstrap{owner:o,tables}=>o==owner && self.initial.bootstrap_view(owner,tables@),_=>false})
            && codec::encodable(self.bootstrap)
            && forall|i:int|0<=i<self.steps.len() ==> #[trigger] history_entry(self.steps,self.initial,records,i)
    }
    pub open spec fn append(self,step:NativeStep)->Self{Self{steps:self.steps.push(step),..self}}
    pub proof fn layout(self,records:Seq<Seq<u8>>,owner:u32)
        requires self.valid(records,owner),
        ensures records.len()==self.steps.len()+1,records[0]==codec::wire(self.bootstrap),codec::encodable(self.bootstrap),
            match self.bootstrap{Operation::Bootstrap{owner:o,tables}=>o==owner && self.initial.bootstrap_view(owner,tables@),_=>false},
    {
        hide(NativeStep::valid);hide(Participant::bootstrap_view);hide(codec::wire);
        reveal(ActorHistory::valid);
    }
    pub proof fn entry(self,records:Seq<Seq<u8>>,owner:u32,index:int)
        requires self.valid(records,owner),0<=index<self.steps.len(),
        ensures self.steps[index].valid(),codec::encodable(self.steps[index].operation),
            codec::wire(self.steps[index].operation)==records[index+1],
            self.steps[index].before.semantic_eq(if index==0{self.initial}else{self.steps[index-1].after}),
            self.steps[index].origins_before==(if index==0{Map::empty()}else{self.steps[index-1].origins_after}),
            allowed(self.steps.take(index),self.steps[index].operation),
    {
        hide(NativeStep::valid);hide(Participant::semantic_eq);hide(Participant::bootstrap_view);hide(codec::wire);
        reveal(ActorHistory::valid);
        assert(history_entry(self.steps,self.initial,records,index));
    }
}
pub proof fn initialize(history:ActorHistory,owner:u32)
    requires history.steps.len()==0,
        match history.bootstrap{Operation::Bootstrap{owner:o,tables}=>o==owner && history.initial.bootstrap_view(owner,tables@),_=>false},
    ensures history.valid(seq![codec::wire(history.bootstrap)],owner),
{
    hide(NativeStep::valid);hide(codec::wire);
    reveal(ActorHistory::valid);
}
pub proof fn prefix(history:ActorHistory,records:Seq<Seq<u8>>,owner:u32,n:int)
    requires history.valid(records,owner),0<=n<=history.steps.len(),
    ensures (ActorHistory{steps:history.steps.take(n),..history}).valid(records.take(n+1),owner),
{
    hide(NativeStep::valid);hide(Participant::semantic_eq);hide(Participant::bootstrap_view);hide(codec::wire);
    hide(allowed);hide(codec::encodable);
    history.layout(records,owner);
    let next=ActorHistory{steps:history.steps.take(n),..history};
    assert(next.steps.len()+1==records.take(n+1).len());
    assert(records.take(n+1)[0]==codec::wire(next.bootstrap));
    assert(codec::encodable(next.bootstrap));
    assert(match next.bootstrap{Operation::Bootstrap{owner:o,tables}=>o==owner && next.initial.bootstrap_view(owner,tables@),_=>false});
    assert forall|i:int|0<=i<next.steps.len() implies
        #[trigger] history_entry(next.steps,next.initial,records.take(n+1),i) by {
        history.entry(records,owner,i);
        assert(next.steps[i]==history.steps[i]);
        assert(next.steps.take(i)==history.steps.take(i));
        if i>0 { assert(next.steps[i-1]==history.steps[i-1]); }
        assert(records.take(n+1)[i+1]==records[i+1]);
    }
    reveal(ActorHistory::valid);
}
pub proof fn immutable_bindings(history:ActorHistory,records:Seq<Seq<u8>>,owner:u32,a:MigrationPlan,b:MigrationPlan)
    requires history.valid(records,owner),bound(history.steps,a),bound(history.steps,b),a.generation==b.generation,
    ensures crate::storage::same_plan(a,b),
{
    let i=choose|i:int|0<=i<history.steps.len() && match #[trigger] history.steps[i].operation{
        Operation::Bind(p)=>crate::storage::same_plan(p,a),_=>false};
    let j=choose|j:int|0<=j<history.steps.len() && match #[trigger] history.steps[j].operation{
        Operation::Bind(p)=>crate::storage::same_plan(p,b),_=>false};
    history.entry(records,owner,i);history.entry(records,owner,j);
    if i<j{assert(history.steps.take(j)[i]==history.steps[i]);assert(bound_generation(history.steps.take(j),a.generation));}
    if j<i{assert(history.steps.take(i)[j]==history.steps[j]);assert(bound_generation(history.steps.take(i),b.generation));}
}
pub proof fn origin_begin(history:ActorHistory,records:Seq<Seq<u8>>,owner:u32,client:u64)
    requires history.valid(records,owner),history.origins().contains_key(client),
    ensures exists|i:int|0<i<records.len() && records[i]==codec::wire(Operation::Begin{
        id:TxnId{client,sequence:history.origins()[client].0},incarnation:history.origins()[client].1}),
    decreases history.steps.len(),
{
    history.layout(records,owner);
    let step=history.steps.last();let n=history.steps.len() as int-1;
    history.entry(records,owner,n);
    if let Operation::Begin{id,incarnation}=step.operation{
        if id.client==client && step.status==Status::Ok{
            assert(records[n+1]==codec::wire(Operation::Begin{id,incarnation}));return;
        }
    }
    prefix(history,records,owner,n);
    let previous=ActorHistory{steps:history.steps.take(n),..history};
    assert(previous.origins().contains_key(client) && previous.origins()[client]==history.origins()[client]);
    origin_begin(previous,records.take(n+1),owner,client);
    let i=choose|i:int|0<i<records.take(n+1).len() && #[trigger] records.take(n+1)[i]==codec::wire(Operation::Begin{
        id:TxnId{client,sequence:history.origins()[client].0},incarnation:history.origins()[client].1});
    assert(records[i]==records.take(n+1)[i]);
}
pub proof fn bound_nonempty(history:ActorHistory,records:Seq<Seq<u8>>,owner:u32,plan:MigrationPlan)
    requires history.valid(records,owner),bound(history.steps,plan),
    ensures plan.old.len()>0,
{
    let i=choose|i:int|0<=i<history.steps.len() && match #[trigger] history.steps[i].operation {
        Operation::Bind(p)=>crate::storage::same_plan(p,plan),_=>false,
    };
    history.entry(records,owner,i);
    assert(codec::encodable(history.steps[i].operation));
}
pub proof fn admitted_encodable(history:ActorHistory,records:Seq<Seq<u8>>,owner:u32,op:Operation)
    requires history.valid(records,owner),allowed(history.steps,op),
    ensures codec::encodable(op),
{
    match op{
        Operation::Deliver{plan,..}|Operation::Reserve{plan,..}|Operation::Complete{plan,..}=>{
            bound_nonempty(history,records,owner,plan);
        },
        _=>{},
    }
}
pub proof fn extend(history:ActorHistory,records:Seq<Seq<u8>>,owner:u32,step:NativeStep)
    requires history.valid(records,owner),step.valid(),step.before.semantic_eq(history.endpoint()),step.origins_before==history.origins(),
        allowed(history.steps,step.operation),
    ensures history.append(step).valid(records.push(codec::wire(step.operation)),owner),
{
    hide(NativeStep::valid);hide(Participant::semantic_eq);hide(Participant::bootstrap_view);hide(codec::wire);
    hide(allowed);hide(codec::encodable);
    history.layout(records,owner);
    admitted_encodable(history,records,owner,step.operation);
    assert(history.steps.push(step).len()+1==records.push(codec::wire(step.operation)).len());
    assert(records.push(codec::wire(step.operation))[0]==codec::wire(history.bootstrap));
    assert(codec::encodable(history.append(step).bootstrap));
    assert(match history.append(step).bootstrap{Operation::Bootstrap{owner:o,tables}=>o==owner
        && history.append(step).initial.bootstrap_view(owner,tables@),_=>false});
    assert forall|i:int|0<=i<history.steps.push(step).len() implies
        #[trigger] history_entry(history.steps.push(step),history.initial,records.push(codec::wire(step.operation)),i) by {
        if i==history.steps.len(){
            assert(history.steps.push(step).take(i)==history.steps);
            assert(history.steps.push(step)[i]==step);
            if i>0 {
                assert(history.steps.last()==history.steps[i-1]);
                assert(history.steps.push(step)[i-1]==history.steps[i-1]);
            }
            assert(records.push(codec::wire(step.operation))[i+1]==codec::wire(step.operation));
        } else {
            history.entry(records,owner,i);
            assert(history.steps.push(step)[i]==history.steps[i]);
            assert(history.steps.push(step).take(i)==history.steps.take(i));
            if i>0 { assert(history.steps.push(step)[i-1]==history.steps[i-1]); }
            assert(records.push(codec::wire(step.operation))[i+1]==records[i+1]);
        }
    }
    reveal(ActorHistory::valid);
}
pub proof fn initial_equal(a:Seq<Boundary>,b:Seq<Boundary>,owner:u32,key:Seq<u8>)
    requires crate::directory_proofs::equivalent(a,b),
    ensures crate::participant::initial_meta(a,owner,key)==crate::participant::initial_meta(b,owner,key),
    decreases a.len(),
{
    if a.len()>0{initial_equal(a.drop_last(),b.drop_last(),owner,key);}
}
pub proof fn release_unique(before:leases::SessionView,a:leases::SessionView,b:leases::SessionView,table:u64,key:Seq<u8>,sa:Status,sb:Status)
    requires before.wf(),leases::point_release(before,a,table,key,sa),leases::point_release(before,b,table,key,sb),
    ensures a==b,sa==sb,
{
    if sa==Status::Ok{
        let i=choose|i:int|0<=i<before.holds.len() && before.holds[i].table==table && before.holds[i].coordinate==key
            && before.holds[i].end is Point && a.holds==before.holds.update(i,before.holds.last()).drop_last();
        let j=choose|j:int|0<=j<before.holds.len() && before.holds[j].table==table && before.holds[j].coordinate==key
            && before.holds[j].end is Point && b.holds==before.holds.update(j,before.holds.last()).drop_last();
        crate::bytes::cmp_laws(key,key);
        leases::common_point_overlaps(before.holds[i],before.holds[j],table,key);
        if j<i{leases::overlap_symmetric(before.holds[i],before.holds[j]);}
        assert(before.holds[i]==before.holds[j]);assert(i==j);
    }
}
pub proof fn register_unique(before:Map<u64,leases::SessionView>,a:Map<u64,leases::SessionView>,b:Map<u64,leases::SessionView>,id:TxnId,scope:leases::ScopeView,sa:Status,sb:Status)
    requires leases::register_effect(before,a,id,scope,sa),leases::register_effect(before,b,id,scope,sb),
    ensures a==b,sa==sb,
{}
proof fn acquire_unique(a:NativeStep,b:NativeStep)
    requires a.valid(),b.valid(),a.before.semantic_eq(b.before),
        codec::same_operation(a.operation,b.operation),a.operation is Acquire,
    ensures a.after.lease_view()==b.after.lease_view(),
{
    hide(crate::bytes::cmp_spec);
    match (a.operation,b.operation){
        (Operation::Acquire{id,range,point,grant},Operation::Acquire{id:other_id,range:other,point:other_point,grant:other_grant})=>{
            assert(id==other_id && point==other_point && grant==other_grant);
            assert(range.table==other.table && range.lo@==other.lo@ && leases::range_hi(range)==leases::range_hi(other));
            assert forall|g:Grant|a.before.open_hold(id,range.table,range.lo@,g)==b.before.open_hold(id,range.table,range.lo@,g)
                && a.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g)
                    ==b.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g) by{
                a.before.semantic_lease_guards(b.before,id,range.table,range.lo@,leases::range_hi(range),g);
            }
            let held_a=exists|g:Grant|a.before.open_hold(id,range.table,range.lo@,g);
            let held_b=exists|g:Grant|b.before.open_hold(id,range.table,range.lo@,g);
            assert(held_a==held_b) by{
                if held_a{
                    let g=choose|g:Grant|a.before.open_hold(id,range.table,range.lo@,g);
                    assert(b.before.open_hold(id,range.table,range.lo@,g));
                }
                if held_b{
                    let g=choose|g:Grant|b.before.open_hold(id,range.table,range.lo@,g);
                    assert(a.before.open_hold(id,range.table,range.lo@,g));
                }
            }
            let ranged_a=exists|g:Grant|a.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g);
            let ranged_b=exists|g:Grant|b.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g);
            assert(ranged_a==ranged_b) by{
                if ranged_a{
                    let g=choose|g:Grant|a.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g);
                    a.before.semantic_lease_guards(b.before,id,range.table,range.lo@,leases::range_hi(range),g);
                    assert(b.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g));
                }
                if ranged_b{
                    let g=choose|g:Grant|b.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g);
                    a.before.semantic_lease_guards(b.before,id,range.table,range.lo@,leases::range_hi(range),g);
                    assert(a.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g));
                }
            }
            assert(a.before.owner_view()==b.before.owner_view() && a.before.table_ids()==b.before.table_ids());
            assert(a.before.local_meta(range.table,range.lo@)==b.before.local_meta(range.table,range.lo@));
            if point{
                assert(a.before.point_acquire_effect(a.after,id,range.table,range.lo@,grant,a.status));
                assert(b.before.point_acquire_effect(b.after,id,range.table,range.lo@,grant,b.status));
            }else{
                assert(a.before.range_acquire_effect(a.after,id,range.table,range.lo@,leases::range_hi(range),grant,a.status));
                assert(b.before.range_acquire_effect(b.after,id,range.table,range.lo@,leases::range_hi(range),grant,b.status));
            }
            let serving_a=forall|key:Seq<u8>|crate::directory::inside(key,range.lo@,leases::range_hi(range)) ==>
                a.before.local_meta(range.table,key).is_some() && a.before.local_meta(range.table,key).unwrap().role==Role::Serving
                    && a.before.local_meta(range.table,key).unwrap().ownership_version==grant.ownership_version;
            let serving_b=forall|key:Seq<u8>|crate::directory::inside(key,range.lo@,leases::range_hi(range)) ==>
                b.before.local_meta(range.table,key).is_some() && b.before.local_meta(range.table,key).unwrap().role==Role::Serving
                    && b.before.local_meta(range.table,key).unwrap().ownership_version==grant.ownership_version;
            assert(serving_a==serving_b) by{
                assert forall|key:Seq<u8>|a.before.local_meta(range.table,key)==b.before.local_meta(range.table,key) by{};
            }
            let admitted=grant.owner==a.before.owner_view() && if point{
                !(exists|g:Grant|a.before.open_hold(id,range.table,range.lo@,g))
                    && a.before.local_meta(range.table,range.lo@).is_some()
                    && a.before.local_meta(range.table,range.lo@).unwrap().role==Role::Serving
                    && a.before.local_meta(range.table,range.lo@).unwrap().ownership_version==grant.ownership_version
            }else{
                crate::directory::proper(range.lo@,leases::range_hi(range))
                    && !(exists|g:Grant|a.before.open_range(id,range.table,range.lo@,leases::range_hi(range),g))
                    && a.before.table_ids().contains(range.table) && serving_a
            };
            let scope=if point{leases::point_scope(range.table,range.lo@,grant)}
                else{leases::range_scope(range.table,range.lo@,leases::range_hi(range),grant)};
            if admitted{
                assert(leases::register_effect(a.before.lease_view(),a.after.lease_view(),id,scope,a.status));
                assert(leases::register_effect(b.before.lease_view(),b.after.lease_view(),id,scope,b.status));
                register_unique(a.before.lease_view(),a.after.lease_view(),b.after.lease_view(),id,scope,a.status,b.status);
            }else{
                assert(a.after.lease_view()==a.before.lease_view());
                assert(b.after.lease_view()==b.before.lease_view());
            }
        },
        _=>{},
    }
}
pub proof fn step_unique(a:NativeStep,b:NativeStep)
    requires a.valid(),b.valid(),a.before.semantic_eq(b.before),a.origins_before==b.origins_before,
        codec::wire(a.operation)==codec::wire(b.operation),
    ensures a.after.semantic_eq(b.after),a.origins_after==b.origins_after,
{
    codec::operation_unique(a.operation,b.operation);
    match (a.operation,b.operation){
        (Operation::Bind(_),Operation::Bind(_))|(Operation::Reserve{..},Operation::Reserve{..})|(Operation::Settled,Operation::Settled)=>{},
        (Operation::Begin{id,incarnation},Operation::Begin{..})=>{
            a.after.metadata_observations(a.before);b.after.metadata_observations(b.before);
        }
        (Operation::Resolve(_),Operation::Resolve(_))|(Operation::Finish(_),Operation::Finish(_))=>{
            a.after.metadata_observations(a.before);b.after.metadata_observations(b.before);
        }
        (Operation::Release{id,table,coordinate},Operation::Release{..})=>{
            a.after.metadata_observations(a.before);b.after.metadata_observations(b.before);
            a.before.lease_facts();
            if a.before.lease_view().contains_key(id.client)
                && a.before.lease_view()[id.client].sequence==id.sequence && a.before.lease_view()[id.client].terminal{
                release_unique(a.before.lease_view()[id.client],a.after.lease_view()[id.client],b.after.lease_view()[id.client],
                    table,coordinate@,a.status,b.status);
            }
        }
        (Operation::Acquire{..},Operation::Acquire{..})=>{
            a.after.metadata_observations(a.before);b.after.metadata_observations(b.before);
            acquire_unique(a,b);
        }
        (Operation::Deliver{plan:p,command:c},Operation::Deliver{plan:q,..})=>{
            a.before.semantic_guards(b.before,p,q,c);
            if a.after.same_metadata(a.before){a.after.metadata_observations(a.before);}
            if b.after.same_metadata(b.before){b.after.metadata_observations(b.before);}
            assert forall|table:u64,key:Seq<u8>|a.after.local_meta(table,key)==b.after.local_meta(table,key) by{};
        }
        (Operation::Complete{plan:p,..},Operation::Complete{plan:q,..})=>{
            a.before.semantic_guards(b.before,p,q,Command::Start);
            if a.after.same_metadata(a.before){a.after.metadata_observations(a.before);}
            if b.after.same_metadata(b.before){b.after.metadata_observations(b.before);}
            assert forall|table:u64,key:Seq<u8>|a.after.local_meta(table,key)==b.after.local_meta(table,key) by{};
        }
        _=>{},
    }
    assert(a.after.owner_view()==b.after.owner_view());
    assert(a.after.table_ids()==b.after.table_ids());
    assert(a.after.receipt_view()==b.after.receipt_view());
    assert(a.after.lease_view()==b.after.lease_view());
    assert forall|table:u64,key:Seq<u8>|a.after.local_meta(table,key)==b.after.local_meta(table,key) by{};
}
pub proof fn bootstrap_unique(a:Operation,b:Operation,x:Participant,y:Participant)
    requires codec::wire(a)==codec::wire(b),
        match a{Operation::Bootstrap{owner,tables}=>x.bootstrap_view(owner,tables@),_=>false},
        match b{Operation::Bootstrap{owner,tables}=>y.bootstrap_view(owner,tables@),_=>false},
    ensures x.semantic_eq(y),
{
    codec::operation_unique(a,b);
    match (a,b){
        (Operation::Bootstrap{owner,tables:p},Operation::Bootstrap{tables:q,..})=>{
            assert(Seq::new(p.len() as nat,|i:int|p@[i].table) =~= Seq::new(q.len() as nat,|i:int|q@[i].table));
            assert(x.table_ids() =~= y.table_ids());
            assert forall|table:u64,key:Seq<u8>|x.local_meta(table,key)==y.local_meta(table,key) by{
                if x.table_ids().contains(table){
                    let i=choose|i:int|0<=i<p.len() && p@[i].table==table;
                    initial_equal(p@[i].boundaries@,q@[i].boundaries@,owner,key);
                }else{x.metadata_absent(table);y.metadata_absent(table);}
            }
        }
        _=>{},
    }
}
} // verus!
