//! Native byte-journal / recovery-protocol composition.
//! The logical checkpoint is a projection of decoded native operations, not a
//! second executable state machine or a recovered-state input. Source records
//! retain the existing authenticated-RPC and finite-byte-observation boundary.
use vstd::prelude::*;
use crate::sharding_placement as p;
use crate::sharding_recovery as r;
use crate::migration as n;
use crate::migration_refinement as m;
use crate::durable_coordinator as d;
use crate::durable_coordinator_codec::{Operation,operation_wire};
use crate::durable_coordinator_proofs as replay;
use crate::durable_kv as kv;
use crate::execution_refinement as source;
use crate::ghost_log as log;
use crate::types::Status;

verus! {
/// Fresh envelope slots follow from committed-prefix growth. This auxiliary
/// invariant says nothing about whether any migration transition is correct.
pub open spec fn old_slots(s:r::State)->bool {
    forall|a:r::Authority,e:r::Envelope| r::head(s,a).processed.contains(e)
        ==> e.slot < s.journals[a].len()
}
pub proof fn initial_slots(c:p::Constants)
    ensures old_slots(r::initial(c)),
{}
pub proof fn slots_step(c:p::Constants,s:r::State,action:r::Action)
    requires r::inv(c,s),old_slots(s),r::enabled(c,s,action),
    ensures old_slots(r::apply(c,s,action)),
{
    let z=r::apply(c,s,action);
    assert forall|a:r::Authority,e:r::Envelope| r::head(z,a).processed.contains(e)
        implies e.slot < z.journals[a].len() by {
        match action {
            r::Action::Commit{envelope}=>{
                if a==r::actor(s.core.placement,envelope.action) {
                    if e==envelope { assert(e.slot==s.journals[a].len()); }
                    else { assert(r::head(s,a).processed.contains(e)); }
                }
            },
            r::Action::Admit{authority}=>{},
            r::Action::Remove{..}|r::Action::Join{..}=>{},
            _=>{},
        }
    }
}

/// Auxiliary journal freshness is derived from genesis over every finite
/// recovery path emitted by the native bridges, including unknown/crash cuts.
/// It is not a backend promise about a migration handler.
pub proof fn finite_slots(c:p::Constants,states:Seq<r::State>,index:nat)
    requires p::constants_ok(c),r::behavior(c,states),index<states.len(),
    ensures r::inv(c,states[index as int]),old_slots(states[index as int]),
        r::rebuild(c,states[index as int])==states[index as int].core.placement,
    decreases index,
{
    if index==0 {
        assert(states[0]==r::initial(c));
        r::lemma_init(c);
        initial_slots(c);
    } else {
        let previous=(index-1) as nat;
        finite_slots(c,states,previous);
        let before=states[previous as int];
        let after=states[index as int];
        assert(previous as int+1==index as int);
        assert(r::next(c,before,after));
        let action=choose|action:r::Action|after==r::dispatch(c,before,action);
        if r::enabled(c,before,action) {
            slots_step(c,before,action);
            r::lemma_step(c,before,action);
            assert(after==r::apply(c,before,action));
        } else {
            assert(after==before);
        }
    }
    r::theorem_reconstruction(c,states[index as int]);
}

/// Copy publication expands an already completed native operation. All other
/// native journal edges preserve the public copy boundary without a row oracle.
pub proof fn journal_preserves_copy_boundary(c:p::Constants,s:r::State,action:p::Action)
    requires crate::durable_engine_refinement::public_copy_boundary(s),
    ensures crate::durable_engine_refinement::public_copy_boundary(accepted(c,s,action)),
        crate::durable_engine_refinement::public_copy_boundary(committed(c,s,action)),
{
    let envelope=r::envelope(s,action);
    crate::durable_engine_refinement::public_copy_step(c,s,r::Action::Accept{envelope});
    crate::durable_engine_refinement::public_copy_step(c,accepted(c,s,action),r::Action::Commit{envelope});
}

pub open spec fn accepted(c:p::Constants,s:r::State,action:p::Action)->r::State {
    r::apply(c,s,r::Action::Accept{envelope:r::envelope(s,action)})
}
pub open spec fn committed(c:p::Constants,s:r::State,action:p::Action)->r::State {
    r::apply(c,accepted(c,s,action),r::Action::Commit{envelope:r::envelope(s,action)})
}

pub proof fn dispatch_edge(c:p::Constants,before:r::State,action:r::Action)
    ensures r::next(c,before,r::dispatch(c,before,action)),
{
    hide(r::dispatch);
    assert(exists|a:r::Action| r::dispatch(c,before,action)==r::dispatch(c,before,a));
}
/// Callers derive these guards from native effects. This helper only lifts an
/// enabled placement action through the authority-local accept/commit journal.
pub proof fn commit_authority(c:p::Constants,s:r::State,action:p::Action)
    requires r::inv(c,s),old_slots(s),
        r::valid_authority(c,r::actor(s.core.placement,action)),
        r::active(s,r::actor(s.core.placement,action)),
        p::enabled(c,s.core.placement,action),r::effect_guard(s,r::envelope(s,action)),
    ensures r::enabled(c,s,r::Action::Accept{envelope:r::envelope(s,action)}),
        r::enabled(c,accepted(c,s,action),r::Action::Commit{envelope:r::envelope(s,action)}),
        r::inv(c,committed(c,s,action)),old_slots(committed(c,s,action)),
        r::next(c,s,accepted(c,s,action)),r::next(c,accepted(c,s,action),committed(c,s,action)),
        committed(c,s,action).core==r::with_placement(s.core,p::apply(c,s.core.placement,action)),
        r::head(committed(c,s,action),r::actor(s.core.placement,action)).image
            ==r::project(committed(c,s,action).core,r::actor(s.core.placement,action)),
        forall|a:r::Authority|r::active(committed(c,s,action),a)==r::active(s,a),
        forall|a:r::Authority|r::head(committed(c,s,action),a).incarnation==r::head(s,a).incarnation,
        forall|a:r::Authority|r::head(committed(c,s,action),a).membership==r::head(s,a).membership,
{
    hide(r::inv);
    let a=r::actor(s.core.placement,action);let e=r::envelope(s,action);
    r::authority_facts(c,s,a);
    assert(!r::head(s,a).processed.contains(e)) by {
        if r::head(s,a).processed.contains(e){assert(e.slot<s.journals[a].len());}
    }
    assert(r::enabled(c,s,r::Action::Accept{envelope:e}));
    r::witnesses::commit_pair_enabled(c,s,action);
    r::lemma_step(c,s,r::Action::Accept{envelope:e});
    slots_step(c,s,r::Action::Accept{envelope:e});
    slots_step(c,accepted(c,s,action),r::Action::Commit{envelope:e});
    dispatch_edge(c,s,r::Action::Accept{envelope:e});
    dispatch_edge(c,accepted(c,s,action),r::Action::Commit{envelope:e});
}
/// Pure interface composition: the action guard is supplied by an actual native
/// source-effect theorem below, not an assumed successful migration handler.
pub proof fn commit_coordinator(c:p::Constants,s:r::State,action:p::Action)
    requires r::inv(c,s),old_slots(s),r::active(s,r::Authority::Coordinator),
        s.core.members.dom()==c.shards,p::enabled(c,s.core.placement,action),
        r::actor(s.core.placement,action)==r::Authority::Coordinator,
    ensures r::enabled(c,s,r::Action::Accept{envelope:r::envelope(s,action)}),
        r::enabled(c,accepted(c,s,action),r::Action::Commit{envelope:r::envelope(s,action)}),
        r::inv(c,committed(c,s,action)),old_slots(committed(c,s,action)),
        r::next(c,s,accepted(c,s,action)),r::next(c,accepted(c,s,action),committed(c,s,action)),
        committed(c,s,action).core==r::with_placement(s.core,p::apply(c,s.core.placement,action)),
        r::head(committed(c,s,action),r::Authority::Coordinator).image
            ==r::project(committed(c,s,action).core,r::Authority::Coordinator),
{
    hide(r::inv);
    let a=r::Authority::Coordinator;
    let e=r::envelope(s,action);
    r::authority_facts(c,s,a);
    assert(r::credential(s,a,e.incarnation,e.membership));
    match action {
        p::Action::Begin{src,dst,..}=>{
            assert(s.core.members.contains_key(src));
            assert(s.core.members.contains_key(dst));
        },
        _=>{},
    }
    assert(r::effect_guard(s,e));
    assert(r::enabled(c,s,r::Action::Accept{envelope:e}));
    r::lemma_step(c,s,r::Action::Accept{envelope:e});
    slots_step(c,s,r::Action::Accept{envelope:e});
    let taken=accepted(c,s,action);
    assert(!r::head(taken,a).processed.contains(e)) by {
        if r::head(taken,a).processed.contains(e) {
            assert(e.slot<taken.journals[a].len());
        }
    }
    assert(r::enabled(c,taken,r::Action::Commit{envelope:e}));
    r::lemma_step(c,taken,r::Action::Commit{envelope:e});
    slots_step(c,taken,r::Action::Commit{envelope:e});
    dispatch_edge(c,s,r::Action::Accept{envelope:e});
    dispatch_edge(c,taken,r::Action::Commit{envelope:e});
}

pub open spec fn call_action(ctx:source::SourceContext,call:source::ControlCall)->p::Action {
    match call {
        source::ControlCall::Begin{id,source,destination,range,result}=>match result {
            Ok(_)=>m::begin_action(ctx.observations,id,source,destination,range),
            _=>p::Action::Stutter,
        },
        source::ControlCall::Transition{request,status}=>
            if status==Status::Ok {m::action(request)}else{p::Action::Stutter},
        source::ControlCall::Receive{generation,certificate,status,..}=>
            if status==Status::Ok {p::Action::Receive{generation:generation as nat,certificate:m::certificate(certificate)}}
            else{p::Action::Stutter},
        source::ControlCall::Reply{id,result}=>
            if result is Some {p::Action::Reply{nonce:m::nonce(id)}}else{p::Action::Stutter},
    }
}
/// The existing source-certified native field writes determine both the model
/// action and its image. Neither a desired successor nor a guard is an input.
pub proof fn control_dispatch(c:p::Constants,ctx:source::SourceContext,s:source::SourceState,
    record:source::ControlRecord)
    requires record.observed(c,ctx,s),
    ensures p::enabled(c,s.placement,call_action(ctx,record.call)),
        record.image(c,ctx,s).placement==p::apply(c,s.placement,call_action(ctx,record.call)),
        r::actor(s.placement,call_action(ctx,record.call))==r::Authority::Coordinator,
{
    let b=record.before;let z=record.after;let v=s.placement;
    match record.call {
        source::ControlCall::Begin{id,source,destination,range,result}=>match result {
            Ok(g)=>{m::begin_replay(c,b,z,ctx.observations,v,id,source,destination,range,g);},
            Err(_)=>{},
        },
        source::ControlCall::Transition{request,status}=>{
            if status==Status::Ok {
                if request is Commit {m::publication_observation(c,b,z,ctx.observations,v);}
                m::transition_guard(b,z,v,c,request,status);
                m::transition_replay(b,z,v,c,request,status,m::directory(c,z,ctx.observations));
                m::transition_fields(b,z,v,request,m::directory(c,z,ctx.observations),status);
            }
        },
        source::ControlCall::Receive{generation,owner,certificate,status}=>{
            m::receive_replay(b,z,v,c,generation,owner,certificate,status);
            if status==Status::Ok {
                log::single_write(v,log::Write::Received{generation:generation as nat,value:m::certificate(certificate)});
            }
        },
        source::ControlCall::Reply{id,result}=>{
            m::reply_replay(b,z,v,c,id,result);
            if result is Some {
                let i=choose|i:int| 0<=i<b.records.len() && n::same_nonce(b.records[i].plan.nonce,id);
                assert(m::controls_at(b,v,i));
                assert(v.outcomes.contains_key(m::nonce(id)));
                log::single_write(v,log::Write::Reply{nonce:m::nonce(id),value:m::outcome(result.unwrap())});
            }
        },
    }
}

pub open spec fn binds_operation(record:source::ControlRecord,op:Operation,response:d::Response)->bool {
    match (op,response,record.call) {
        (Operation::Begin{nonce,source:src,destination:dst,range},d::Response::Begun(g),
            source::ControlCall::Begin{id,source,destination,range:rng,result})=>
            nonce==id && src==source && dst==destination && range==rng && result==Ok(g),
        (Operation::Request(request),d::Response::Applied,
            source::ControlCall::Transition{request:r,status})=>request==r && status==Status::Ok,
        (Operation::Receive{generation:g,owner:n,certificate:cert},d::Response::Applied,
            source::ControlCall::Receive{generation,owner,certificate,status})=>
            g==generation && n==owner && cert==certificate && status==Status::Ok,
        (Operation::Reply(nonce),d::Response::Replied(outcome),
            source::ControlCall::Reply{id,result})=>nonce==id && result==Some(outcome),
        _=>false,
    }
}
/// Successful durable execute supplies `effect`, the appended operation bytes,
/// and both replay facts. `observed` is the unchanged finite-byte/authenticated
/// source contract, constructed by the existing source-history induction.
pub proof fn native_control_commit(c:p::Constants,ctx:source::SourceContext,
    state:source::SourceState,recovery:r::State,record:source::ControlRecord,
    op:Operation,response:d::Response,namespace:kv::Namespace,incarnation:u64,
    bytes:Map<Seq<u8>,Seq<u8>>,records:Seq<Seq<u8>>)
    requires r::inv(c,recovery),old_slots(recovery),recovery.core.placement==state.placement,
        r::active(recovery,r::Authority::Coordinator),recovery.core.members.dom()==c.shards,
        record.observed(c,ctx,state),binds_operation(record,op,response),
        d::effect(record.before,record.after,op,Ok(response)),
        d::replays(records,record.before),n::layout(record.after),
        namespace.coordinator,kv::has_prefix(bytes,namespace,incarnation,records.push(operation_wire(op))),
        incarnation==r::head(recovery,r::Authority::Coordinator).incarnation,
    ensures r::inv(c,committed(c,recovery,call_action(ctx,record.call))),
        old_slots(committed(c,recovery,call_action(ctx,record.call))),
        committed(c,recovery,call_action(ctx,record.call)).core.placement==record.image(c,ctx,state).placement,
        r::next(c,recovery,accepted(c,recovery,call_action(ctx,record.call))),
        r::next(c,accepted(c,recovery,call_action(ctx,record.call)),committed(c,recovery,call_action(ctx,record.call))),
        d::replays(records.push(operation_wire(op)),record.after),
{
    d::extend_replay(records,record.before,record.after,op,response);
    control_dispatch(c,ctx,state,record);
    commit_coordinator(c,recovery,call_action(ctx,record.call));
}

pub proof fn equivalent_master(before:n::Native,after:n::Native,state:p::State)
    requires n::layout(before),replay::same_state(before,after),m::master(before,state),
    ensures m::master(after,state),
{
    if before.active is Some {
        let i=before.active.unwrap() as int;
        assert(replay::same_plan(before.records[i].plan,after.records[i].plan));
    }
    assert forall|i:int| 0<=i<after.records.len() implies m::controls_at(after,state,i) by {
        assert(m::controls_at(before,state,i));
        assert(replay::same_plan(before.records[i].plan,after.records[i].plan));
    }
    assert forall|g:nat| state.plans.contains_key(g) <==>
        (exists|i:int| 0<=i<after.records.len() && after.records[i].plan.generation==g) by {
        if state.plans.contains_key(g) {
            let i=choose|i:int| 0<=i<before.records.len() && before.records[i].plan.generation==g;
            assert(replay::same_plan(before.records[i].plan,after.records[i].plan));
            assert(exists|j:int| 0<=j<after.records.len() && after.records[j].plan.generation==g);
        }else if exists|i:int| 0<=i<after.records.len() && after.records[i].plan.generation==g {
            let i=choose|i:int| 0<=i<after.records.len() && after.records[i].plan.generation==g;
            assert(replay::same_plan(before.records[i].plan,after.records[i].plan));
            assert(exists|j:int| 0<=j<before.records.len() && before.records[j].plan.generation==g);
        }
    }
    assert forall|id:int| state.outcomes.contains_key(id) implies
        exists|i:int| 0<=i<after.records.len() && m::nonce(after.records[i].plan.nonce)==id by {
        let i=choose|i:int| 0<=i<before.records.len() && m::nonce(before.records[i].plan.nonce)==id;
        assert(replay::same_plan(before.records[i].plan,after.records[i].plan));
    }
    assert forall|id:int| state.replies.contains_key(id) implies
        exists|i:int| 0<=i<after.records.len() && m::nonce(after.records[i].plan.nonce)==id by {
        let i=choose|i:int| 0<=i<before.records.len() && m::nonce(before.records[i].plan.nonce)==id;
        assert(replay::same_plan(before.records[i].plan,after.records[i].plan));
    }
}
/// Both native states are obtained by the checked replay relation over the SAME
/// concrete committed bytes. The earlier state is a ghost proof witness only;
/// DurableCoordinator::recover neither receives nor consults it.
pub proof fn recovered_coordinator(c:p::Constants,state:r::State,records:Seq<Seq<u8>>,
    before:n::Native,after:n::Native,bytes:Map<Seq<u8>,Seq<u8>>,ns:kv::Namespace,inc:u64)
    requires r::inv(c,state),d::replays(records,before),d::replays(records,after),
        m::master(before,state.core.placement),kv::has_prefix(bytes,ns,inc,records),ns.coordinator,
        inc==r::head(state,r::Authority::Coordinator).incarnation,
    ensures m::master(after,state.core.placement),replay::same_state(before,after),
        r::witnesses::bootstrap_enabled(c,state,r::Authority::Coordinator),
        r::inv(c,r::witnesses::bootstrap(c,state,r::Authority::Coordinator)),
        r::witnesses::bootstrap(c,state,r::Authority::Coordinator).core==state.core,
        r::head(r::witnesses::bootstrap(c,state,r::Authority::Coordinator),r::Authority::Coordinator).incarnation==inc+1,
{
    replay::replay_equivalent(records,before,after);
    equivalent_master(before,after,state.core.placement);
    r::witnesses::bootstrap_summary(c,state,r::Authority::Coordinator);
}
/// A timed-out append can still commit after the process dies. Crash discards
/// only runtime admission; settlement chooses the old or whole appended prefix.
pub proof fn coordinator_commit_after_crash(c:p::Constants,s:r::State,action:p::Action)
    requires r::inv(c,s),old_slots(s),r::active(s,r::Authority::Coordinator),
        s.core.members.dom()==c.shards,p::enabled(c,s.core.placement,action),
        r::actor(s.core.placement,action)==r::Authority::Coordinator,
    ensures ({
        let taken=accepted(c,s,action);
        let crashed=r::apply(c,taken,r::Action::Crash{authority:r::Authority::Coordinator});
        let settled=r::apply(c,crashed,r::Action::Commit{envelope:r::envelope(s,action)});
        r::inv(c,crashed) && r::inv(c,settled)
            && r::enabled(c,crashed,r::Action::Commit{envelope:r::envelope(s,action)})
            && crashed.core==s.core
            && settled.core==committed(c,s,action).core
            && !r::active(crashed,r::Authority::Coordinator)
            && !r::active(settled,r::Authority::Coordinator)
    }),
{
    commit_coordinator(c,s,action);
    let a=r::Authority::Coordinator;
    let e=r::envelope(s,action);
    let taken=accepted(c,s,action);
    r::lemma_step(c,s,r::Action::Accept{envelope:e});
    r::lemma_step(c,taken,r::Action::Crash{authority:a});
    let crashed=r::apply(c,taken,r::Action::Crash{authority:a});
    assert(r::effect_guard(crashed,e));
    assert(r::enabled(c,crashed,r::Action::Commit{envelope:e}));
    r::lemma_step(c,crashed,r::Action::Commit{envelope:e});
}

/// Recovery's actor is selected by the settled bytes, never by the caller's
/// pre-crash return status. This includes both immediate and delayed commit.
pub proof fn uncertain_coordinator_prefix(bytes:Map<Seq<u8>,Seq<u8>>,
    ns:kv::Namespace,inc:u64,records:Seq<Seq<u8>>,op:Operation,
    before:n::Native,candidate:n::Native,recovered:n::Native,decoded:Seq<Seq<u8>>)
    requires records.len()<u64::MAX,decoded.len()<=u64::MAX,
        d::replays(records,before),d::replays(records.push(operation_wire(op)),candidate),
        d::replays(decoded,recovered),kv::has_prefix(bytes,ns,inc,decoded),
        kv::has_prefix(bytes,ns,inc,records)
            || kv::has_prefix(bytes,ns,inc,records.push(operation_wire(op))),
    ensures (decoded==records && replay::same_state(before,recovered))
        || (decoded==records.push(operation_wire(op)) && replay::same_state(candidate,recovered)),
{
    if kv::has_prefix(bytes,ns,inc,records) {
        kv::committed_prefix_unique(bytes,ns,inc,records,inc,decoded);
        replay::replay_equivalent(records,before,recovered);
    } else {
        kv::committed_prefix_unique(bytes,ns,inc,records.push(operation_wire(op)),inc,decoded);
        replay::replay_equivalent(decoded,candidate,recovered);
    }
}
/// An acknowledged retry of the same immutable intent appends provenance but
/// cannot allocate a generation, alter a decision, or republish routing.
pub proof fn duplicate_begin_stutters(before:n::Native,after:n::Native,
    nonce:crate::types::TxnId,src:u32,dst:u32,range:crate::types::KeyRange,g:u64)
    requires d::effect(before,after,Operation::Begin{nonce,source:src,destination:dst,range},Ok(d::Response::Begun(g))),
        exists|i:int|0<=i<before.records.len() && n::same_nonce(before.records[i].plan.nonce,nonce),
    ensures before==after,
{}

proof fn semantic_reverse(a:crate::participant::Participant,b:crate::participant::Participant)
    requires a.semantic_eq(b),
    ensures b.semantic_eq(a),
{
    assert forall|table:u64,key:Seq<u8>| b.local_meta(table,key)==a.local_meta(table,key) by {};
}
proof fn semantic_chain(a:crate::participant::Participant,b:crate::participant::Participant,
    c:crate::participant::Participant)
    requires a.semantic_eq(b),b.semantic_eq(c),
    ensures a.semantic_eq(c),
{
    assert forall|table:u64,key:Seq<u8>| a.local_meta(table,key)==c.local_meta(table,key) by {};
}

pub proof fn participant_histories_unique(
    a:crate::durable_participant::replay_proofs::ActorHistory,
    b:crate::durable_participant::replay_proofs::ActorHistory,
    records:Seq<Seq<u8>>,owner:u32)
    requires a.valid(records,owner),b.valid(records,owner),
    ensures a.endpoint().semantic_eq(b.endpoint()),a.origins()==b.origins(),
    decreases records.len(),
{
    hide(crate::participant::Participant::semantic_eq);
    hide(crate::participant::Participant::bootstrap_view);
    hide(crate::durable_participant::codec::encodable);
    hide(crate::durable_participant::replay_proofs::NativeStep::valid);
    hide(crate::durable_participant::replay_proofs::allowed);
    hide(crate::durable_participant::codec::wire);
    a.layout(records,owner);
    b.layout(records,owner);
    if a.steps.len()==0 {
        crate::durable_participant::replay_proofs::bootstrap_unique(a.bootstrap,b.bootstrap,a.initial,b.initial);
    } else {
        crate::durable_participant::replay_proofs::prefix(a,records,owner,a.steps.len() as int-1);
        crate::durable_participant::replay_proofs::prefix(b,records,owner,b.steps.len() as int-1);
        let ap=crate::durable_participant::replay_proofs::ActorHistory{steps:a.steps.drop_last(),..a};
        let bp=crate::durable_participant::replay_proofs::ActorHistory{steps:b.steps.drop_last(),..b};
        assert(ap.steps =~= a.steps.take(a.steps.len() as int-1));
        assert(bp.steps =~= b.steps.take(b.steps.len() as int-1));
        assert(records.drop_last() =~= records.take(records.len() as int-1));
        assert(ap.valid(records.drop_last(),owner));
        assert(bp.valid(records.drop_last(),owner));
        participant_histories_unique(ap,bp,records.drop_last(),owner);
        let last=a.steps.len() as int-1;
        a.entry(records,owner,last);
        b.entry(records,owner,last);
        let x=a.steps.last();let y=b.steps.last();
        assert(x.valid() && y.valid());
        assert(crate::durable_participant::codec::wire(x.operation)==records.last());
        assert(crate::durable_participant::codec::wire(y.operation)==records.last());
        semantic_reverse(y.before,bp.endpoint());
        semantic_chain(x.before,ap.endpoint(),bp.endpoint());
        semantic_chain(x.before,bp.endpoint(),y.before);
        assert(x.before.semantic_eq(y.before));
        crate::durable_participant::replay_proofs::step_unique(x,y);
    }
}

/// The prior trace is erased evidence of how the bytes were produced. Executable
/// recovery accepts only KV/engine handles and the authority name, not this trace.
/// Row recovery is a separate engine operation; this lemma preserves the native
/// metadata, receipts, leases and transaction-origin projection exactly.
pub proof fn recovered_participant_metadata(c:p::Constants,state:r::State,
    q:&crate::durable_participant::DurableParticipant,
    prior:crate::durable_participant::replay_proofs::ActorHistory,
    plan:crate::types::MigrationPlan,labels:Map<int,crate::storage::Cell>,
    bytes:Map<Seq<u8>,Seq<u8>>,inc:u64)
    requires r::inv(c,state),q.wf(),q.replay_equivalent(),
        prior.valid(q.records(),q.namespace().owner),
        c.shards.contains(q.namespace().owner as int),
        state.core.members.contains_key(q.namespace().owner as int),
        kv::has_prefix(bytes,q.namespace(),inc,q.records()),
        inc==r::head(state,r::Authority::Participant(q.namespace().owner as int)).incarnation,
        crate::transfer_proofs::local_coupling(&prior.endpoint(),plan,state.core.placement,labels),
    ensures prior.endpoint().semantic_eq(q.native()),prior.origins()==q.origin_view(),
        crate::transfer_proofs::local_coupling(&q.native(),plan,state.core.placement,labels),
        r::witnesses::bootstrap_enabled(c,state,r::Authority::Participant(q.namespace().owner as int)),
        r::witnesses::bootstrap(c,state,r::Authority::Participant(q.namespace().owner as int)).core==state.core,
{
    hide(r::inv);
    hide(crate::durable_participant::replay_proofs::NativeStep::valid);
    hide(crate::durable_participant::codec::wire);
    hide(crate::durable_participant::codec::encodable);
    hide(crate::participant::Participant::bootstrap_view);
    q.replay_refinement();
    participant_histories_unique(prior,q.replay_history(),q.records(),q.namespace().owner);
    semantic_reverse(q.native(),q.replay_history().endpoint());
    semantic_chain(prior.endpoint(),q.replay_history().endpoint(),q.native());
    assert(prior.endpoint().semantic_eq(q.native()));
    assert forall|k:int|state.core.placement.plans[plan.generation as nat].keys.contains(k) implies
        q.native().local_meta(labels[k].0,labels[k].1.0).is_some()
        && crate::participant_proofs::metadata(q.native().local_meta(labels[k].0,labels[k].1.0).unwrap(),
            p::replica(state.core.placement,q.native().owner_view() as int,k)) by {
        assert(prior.endpoint().local_meta(labels[k].0,labels[k].1.0)
            ==q.native().local_meta(labels[k].0,labels[k].1.0));
    }
    r::witnesses::bootstrap_summary(c,state,r::Authority::Participant(q.namespace().owner as int));
}
} // verus!
