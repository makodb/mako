//! Coordinator and cache effects close the same global journal as participant
//! effects. Completion witnesses additionally require an actual successful
//! checked handler result: semantic certificate membership is not emission.
use super::*;
use crate::migration as native;
use crate::migration_refinement as master;
use crate::routing_proofs as cache;
use crate::routing::Snapshot;
use crate::participant::ControlResult;
use crate::types::{Certificate, Status, TxnId, KeyRange, Outcome};

verus! {
/// Admission writes are taken from the retained native record and control.
pub proof fn coordinator_begin(c:p::Constants,b:native::Native,z:native::Native,
    observations:master::Observations,s:p::State,id:TxnId,src:u32,dst:u32,range:KeyRange,generation:u64)
    -> (tracked out:ClosedTransfer)
    requires native::layout(b),native::begin_effect(b,z,id,src,dst,range,Ok(generation)),master::master(b,s),
        master::observes(c,b,observations),master::range_observed(observations,range),
        p::directory(s) == master::directory(c,b,observations),
        forall|owner:u32| b.nodes.contains(owner) ==> c.shards.contains(owner as int),
        master::selected(c,observations,range) != Set::<int>::empty(),
        forall|k:int| c.keys.contains(k) && c.table[k] == range.table ==> observations.slots[k] == z.records.last().slot,
    ensures out.valid(c,s),master::master(z,out.after(s)),
        master::prepared(z,b.records.len() as int),
        out.after(s) == log::apply_writes(s,master::begin_writes(c,b,z,observations)),
{
    master::begin_replay(c,b,z,observations,s,id,src,dst,range,generation);
    master::begin_projection(c,b,z,observations,s,id,src,dst,range,generation);
    master::begin_prepares(b,z,id,src,dst,range,generation);
    let writes = master::begin_writes(c,b,z,observations);
    let segment = master::segment(c,s,writes,master::begin_action(observations,id,src,dst,range));
    ClosedTransfer { segment }
}

/// Native admission rejection, including retained nonce rejection, makes no
/// writes. It does not require the abstract Begin guard to reject too.
pub proof fn rejected_begin(c:p::Constants,b:native::Native,z:native::Native,s:p::State,
    id:TxnId,src:u32,dst:u32,range:KeyRange,error:Status) -> (tracked out:ClosedTransfer)
    requires native::begin_effect(b,z,id,src,dst,range,Err(error)),master::master(b,s),
    ensures out.valid(c,s),out.after(s) == s,z == b,master::master(z,out.after(s)),
{
    master::rejected_admission(b,z,c,s,id,src,dst,range,error);
    empty_segment(c,s)
}

/// All six coordinator phase requests, including rejected requests. Commit's
/// directory append is derived from the actual published snapshot-index write,
/// not from a caller-supplied desired model directory.
pub proof fn coordinator_transition(c:p::Constants,b:native::Native,z:native::Native,
    s:p::State,request:native::Request,status:Status,observations:master::Observations)
    -> (tracked out:ClosedTransfer)
    requires native::layout(b),master::master(b,s),native::transition_effect(b,z,request,status),
        request is Commit && status == Status::Ok ==> {
            b.active is Some
            && master::observes(c,b,observations)
            && master::prepared(b,b.active.unwrap() as int)
            && b.current[b.records[b.active.unwrap() as int].slot as int] == b.records[b.active.unwrap() as int].previous
            && master::range_observed(observations,b.records[b.active.unwrap() as int].plan.range)
            && p::directory(s) == master::directory(c,b,observations)
            && s.plans[s.active.unwrap()].keys == master::selected(c,observations,b.records[b.active.unwrap() as int].plan.range)
            && s.plans[s.active.unwrap()].dst == b.records[b.active.unwrap() as int].plan.destination
            && (forall|k:int| c.keys.contains(k) ==>
                (c.table[k] == b.records[b.active.unwrap() as int].plan.range.table)
                    == (observations.slots[k] == b.records[b.active.unwrap() as int].slot))
        },
    ensures out.valid(c,s),master::master(z,out.after(s)),
        status != Status::Ok ==> out.after(s) == s,
        request is Commit && status == Status::Ok ==>
            p::directory(out.after(s)) == master::directory(c,z,observations),
{
    let snapshot = master::directory(c,z,observations);
    if request is Commit && status == Status::Ok {
        master::publication_observation(c,b,z,observations,s);
    }
    master::transition_replay(b,z,s,c,request,status,snapshot);
    master::transition_projection(c,b,z,s,request,status,snapshot);
    master::transition_fields(b,z,s,request,snapshot,status);
    let writes = master::transition_writes(b,z,request,snapshot,status);
    let segment = master::segment(c,s,writes,master::action(request));
    ClosedTransfer { segment }
}

/// The retained outcome is observed without consuming the nonce or decision.
pub proof fn coordinator_reply(c:p::Constants,b:native::Native,z:native::Native,s:p::State,
    id:TxnId,result:Option<Outcome>) -> (tracked out:ClosedTransfer)
    requires native::layout(b),master::master(b,s),native::reply_effect(b,z,id,result),
    ensures out.valid(c,s),master::master(z,out.after(s)),
        result is None ==> out.after(s) == s,
        result is Some ==> out.after(s).replies.contains_key(master::nonce(id))
            && out.after(s).replies[master::nonce(id)] == master::outcome(result.unwrap()),
{
    let segment = master::reply_segment(c,b,z,s,id,result);
    reveal_with_fuel(log::apply_writes,2);
    ClosedTransfer { segment }
}

/// `emitted` is borrowed, permitting authenticated network retransmission.
/// The transport binding is the equality of its generation, actual sender and
/// certificate to this message. Model membership alone cannot call this API.
pub proof fn coordinator_receive(c:p::Constants,b:native::Native,z:native::Native,s:p::State,
    generation:u64,authenticated_owner:u32,certificate:Certificate,
    tracked emitted:&EmittedCertificate) -> (tracked out:ClosedTransfer)
    requires native::layout(b),master::master(b,s),
        native::receive_effect(b,z,generation,authenticated_owner,certificate,Status::Ok),
        emitted.matches(generation,authenticated_owner,certificate),
        s.certificates.contains((generation as nat,master::certificate(certificate))),
    ensures out.valid(c,s),master::master(z,out.after(s)),
        out.after(s).received.contains((generation as nat,master::certificate(certificate))),
{
    let segment = master::receive_segment(c,b,z,s,generation,authenticated_owner,certificate,Status::Ok);
    reveal_with_fuel(log::apply_writes,2);
    ClosedTransfer { segment }
}

/// Invalid/unknown senders need no issuance witness: the native receipt effect
/// itself proves equality, and no Received write is recorded.
pub proof fn rejected_receive(c:p::Constants,b:native::Native,z:native::Native,s:p::State,
    generation:u64,owner:u32,certificate:Certificate,status:Status) -> (tracked out:ClosedTransfer)
    requires native::layout(b),master::master(b,s),status != Status::Ok,
        native::receive_effect(b,z,generation,owner,certificate,status),
    ensures out.valid(c,s),out.after(s) == s,master::master(z,out.after(s)),
{
    let segment = master::receive_segment(c,b,z,s,generation,owner,certificate,status);
    ClosedTransfer { segment }
}

/// The immutable incoming snapshot is bound to a published dense model index by
/// actual route observations, not by its possibly sparse generation version.
/// Identical installs and failed installs stutter by native snapshot equality.
pub proof fn cache_installed(c:p::Constants,s:p::State,client:int,index:nat,
    before:Option<Snapshot>,after:Option<Snapshot>,incoming:Snapshot,status:Status,
    tables:Map<int,u64>,coordinates:Map<int,Seq<u8>>) -> (tracked out:ClosedTransfer)
    requires cache::install_effect(before,after,incoming,status),
        before is Some ==> s.views.contains_key(client)
            && cache::observed(before.unwrap(),s,s.views[client],tables,coordinates),
        after != before ==> cache::observed(incoming,s,index,tables,coordinates),
    ensures out.valid(c,s),
        after == before ==> out.after(s) == s,
        after != before ==> out.after(s).views.contains_key(client) && out.after(s).views[client] == index,
        after is Some ==> out.after(s).views.contains_key(client)
            && cache::observed(after.unwrap(),out.after(s),out.after(s).views[client],tables,coordinates),
{
    if after == before {
        empty_segment(c,s)
    } else {
        cache::cache_install_refines(c,s,client,index,before,after,incoming,status,tables,coordinates);
        let write = log::Write::View { client,snapshot:index };
        log::single_write(s,write);
        let next = log::apply_write(s,write);
        let action = p::Action::Cache { client,snapshot:index };
        log::accepted(c,s,next,action);
        assert(cache::observed(after.unwrap(),next,index,tables,coordinates));
        ClosedTransfer { segment:log::Segment { writes:seq![write],states:seq![s,next],actions:seq![action] } }
    }
}

/// Mint Ready only from execute_final's actual successful result and checked
/// complete-image/seal postconditions. Captured packets cover every model key,
/// including absent rows. Packet history retains frozen-source provenance even
/// if an authenticated source subsequently processes Abort before local seal.
pub proof fn ready_emitted(c:p::Constants,before:&Participant,after:&Participant,
    plan:MigrationPlan,result:ControlResult,s:p::State,keys:Seq<int>,labels:Map<int,Cell>,
    initial:Image,current:Image,source:Image,writers:Map<int,int>,code:spec_fn(Seq<u8>)->int)
    -> (tracked out:(ClosedTransfer,EmittedCertificate))
    requires result.status == Status::Ok,result.certificate == Some(Certificate::Ready),
        transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(before,plan,s,labels),
        before.transfer_authorized(plan,false),after.seal_frame(*before,plan),
        after.owner_view() == before.owner_view(),
        crate::storage::complete(current,initial,source,plan.range),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((plan.destination as int,k))
            && s.packets.contains(transfer::packet(plan.generation as nat,k,transfer::observed(source,labels[k],writers[k],code))),
    ensures out.0.valid(c,s),out.0.after(s).certificates.contains((plan.generation as nat,p::Certificate::Ready)),
        transfer::local_coupling(after,plan,out.0.after(s),labels),
        out.1.matches(plan.generation,after.owner_view(),Certificate::Ready),
{
    let tracked segment = completed_final(c,before,after,plan,s,keys,labels,initial,current,source,writers,code);
    let tracked emitted = EmittedCertificate {
        generation:plan.generation,owner:after.owner_view(),certificate:Certificate::Ready,
    };
    (segment,emitted)
}

/// Terminal metadata was already delivered and masked retained private bytes
/// as Empty. Cleanup changes no placement field, but a real successful cleanup
/// result, consumed retained capability, and exact empty image are all required
/// before its completion receipt can leave the participant.
pub proof fn cleanup_emitted(c:p::Constants,before:&Participant,after:&Participant,
    plan:MigrationPlan,result:ControlResult,s:p::State,keys:Seq<int>,labels:Map<int,Cell>,
    initial:Image,current:Image) -> (tracked out:(ClosedTransfer,EmittedCertificate))
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(before,plan,s,labels),
        before.transfer_authorized(plan,true),after.local_unchanged(*before),after.cleanup_done(plan.generation),
        result.status == Status::Ok,
        result.certificate == Some(if before.owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }),
        crate::storage::complete(current,initial,Map::empty(),plan.range),
        s.certificates.contains((plan.generation as nat,
            if before.owner_view() == plan.source { p::Certificate::SourceDone } else { p::Certificate::DestinationDone })),
    ensures out.0.valid(c,s),out.0.after(s) == s,
        transfer::local_coupling(after,plan,out.0.after(s),labels),
        out.1.matches(plan.generation,after.owner_view(),
            if after.owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }),
        forall|k:int| keys.to_set().contains(k) ==> !current.dom().contains(labels[k]),
{
    assert forall|k:int| keys.to_set().contains(k) implies !current.dom().contains(labels[k]) by {
        crate::storage_refinement::absent_model_key(current,initial,Map::empty(),plan.range,labels[k]);
    }
    assert forall|k:int| s.plans[plan.generation as nat].keys.contains(k) implies
        crate::participant_proofs::metadata(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),
            p::replica(s,after.owner_view() as int,k)) by {
        assert(after.local_meta(labels[k].0,labels[k].1.0) == before.local_meta(labels[k].0,labels[k].1.0));
    }
    let tracked segment = empty_segment(c,s);
    let tracked emitted = EmittedCertificate {
        generation:plan.generation,owner:after.owner_view(),
        certificate:if before.owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone },
    };
    (segment,emitted)
}
} // verus!
