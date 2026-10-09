//! Explicit legacy-engine atomic operation boundary. These records contain the
//! actual callback outcome, request write bytes and before/after engine images.
//! They do not assume a sharding handler's model transition. Gateway proof binds
//! each record to its exact admitted request and non-reused logical TxnId.
use super::*;
use crate::types::TxnId;
use crate::migration_refinement as master;
verus! {
pub enum EngineOutcome { Committed, Aborted, Unknown }
pub type EngineImages = Map<int,Image>;
pub open spec fn encoded_writes(writes:Map<int,Option<Seq<u8>>>) -> Map<int,Option<int>> {
    Map::new(writes.dom(),|k:int| crate::sharding_bytes::value_option(writes[k]))
}
/// Concrete bytes coupled to the finite model labels, independently of effects.
/// Empty and uncovered Stage replicas retain private bytes, not visible cells.
pub open spec fn engine_observed(s:p::State,images:EngineImages,labels:Map<int,Cell>) -> bool {
    (forall|a:int,b:int| labels.contains_key(a) && labels.contains_key(b) && labels[a] == labels[b] ==> a == b)
        && forall|q:(int,int)| s.physical.contains_key(q) && labels.contains_key(q.1)
            && !(p::replica(s,q.0,q.1).role is Empty)
            && (!(p::replica(s,q.0,q.1).role is Stage) || p::replica(s,q.0,q.1).covered) ==>
            images.contains_key(q.0) && p::replica(s,q.0,q.1).cell.value ==
                crate::sharding_bytes::value_option(crate::storage::value(images[q.0],labels[q.1]))
}
/// Atomic successful engine effects: exactly requested bytes at held owners,
/// and the physical one-key/multi-key frame for every other modeled cell.
pub open spec fn atomic_bytes(s:p::State,t:int,before:EngineImages,after:EngineImages,
    labels:Map<int,Cell>,writes:Map<int,Option<Seq<u8>>>) -> bool {
    writes.dom().subset_of(labels.dom()) && writes.dom().subset_of(s.sessions[t].held.dom())
        && forall|q:(int,int)| s.physical.contains_key(q) && labels.contains_key(q.1) ==>
            before.contains_key(q.0) && after.contains_key(q.0)
            && crate::storage::value(after[q.0],labels[q.1]) ==
                if writes.contains_key(q.1) && s.sessions[t].held[q.1].owner == q.0 { writes[q.1] }
                else { crate::storage::value(before[q.0],labels[q.1]) }
}
spec fn data_writes(s:p::State,t:int,keys:Seq<int>,cells:Map<int,p::Cell>) -> Seq<log::Write>
    decreases keys.len(),
{
    if keys.len() == 0 { Seq::empty() } else {
        data_writes(s,t,keys.drop_last(),cells) + seq![
            log::Write::Logical { key:keys.last(),cell:cells[keys.last()] },
            log::Write::ReplicaCell { owner:s.sessions[t].held[keys.last()].owner,key:keys.last(),cell:cells[keys.last()] }]
    }
}
pub open spec fn data_state(s:p::State,t:int,keys:Set<int>,cells:Map<int,p::Cell>) -> p::State {
    p::State {
        logical:Map::new(s.logical.dom(),|k:int| if keys.contains(k) { cells[k] } else { s.logical[k] }),
        physical:vstd::imap::IMap::new(|q:(int,int)| s.physical.contains_key(q),|q:(int,int)|
            if keys.contains(q.1) && s.sessions[t].held[q.1].owner == q.0 { p::Replica { cell:cells[q.1],..s.physical[q] } } else { s.physical[q] }),..s }
}
/// Exact engine endpoint: only requested data cells and the resolving session
/// change. Physical metadata and the session's held grants are preserved.
pub open spec fn engine_state(s:p::State,t:int,writes:Set<int>,cells:Map<int,p::Cell>) -> p::State {
    p::State {
        sessions:s.sessions.insert(t,p::Session { resolved:true,..s.sessions[t] }),
        ..data_state(s,t,writes,cells)
    }
}
/// One source-certified pair of writes extends the finite updated-key set.
/// Keep map extensionality separate from recursive sequence replay.
proof fn data_step(s:p::State,t:int,keys:Set<int>,k:int,cells:Map<int,p::Cell>)
    requires s.sessions.contains_key(t),s.logical.contains_key(k),
        s.sessions[t].held.contains_key(k),s.physical.contains_key((s.sessions[t].held[k].owner,k)),
        cells.contains_key(k),
    ensures ({
        let mid = data_state(s,t,keys,cells);
        let logical = log::Write::Logical { key:k,cell:cells[k] };
        let physical = log::Write::ReplicaCell { owner:s.sessions[t].held[k].owner,key:k,cell:cells[k] };
        log::writable(mid,logical) && log::writable(log::apply_write(mid,logical),physical)
            && log::apply_write(log::apply_write(mid,logical),physical) == data_state(s,t,keys.insert(k),cells)
    }),
{
    let mid = data_state(s,t,keys,cells);
    let next = log::apply_write(log::apply_write(mid,log::Write::Logical { key:k,cell:cells[k] }),
        log::Write::ReplicaCell { owner:s.sessions[t].held[k].owner,key:k,cell:cells[k] });
    let target = data_state(s,t,keys.insert(k),cells);
    assert(next.logical =~= target.logical) by {
        assert forall|j:int| next.logical.contains_key(j) implies next.logical[j] == target.logical[j] by {
            if j == k {} else if keys.contains(j) {} else {}
        }
    }
    assert(next.physical =~= target.physical) by {
        assert forall|q:(int,int)| next.physical.contains_key(q) implies next.physical[q] == target.physical[q] by {
            if q == (s.sessions[t].held[k].owner,k) {
                assert(mid.physical[q] == if keys.contains(k) {
                    p::Replica { cell:cells[k],..s.physical[q] }
                } else { s.physical[q] });
            } else if q.1 == k {
                assert(q.0 != s.sessions[t].held[k].owner);
            }
        }
    }
}

proof fn data_replay(s:p::State,t:int,keys:Seq<int>,cells:Map<int,p::Cell>)
    requires s.sessions.contains_key(t),keys.to_set().subset_of(cells.dom()),
        forall|k:int| keys.to_set().contains(k) ==> s.logical.contains_key(k) && s.sessions[t].held.contains_key(k)
            && s.physical.contains_key((s.sessions[t].held[k].owner,k)),
    ensures log::writes_ok(s,data_writes(s,t,keys,cells)),
        log::apply_writes(s,data_writes(s,t,keys,cells)) == data_state(s,t,keys.to_set(),cells),
    decreases keys.len(),
{
    if keys.len() == 0 {
        assert(keys.to_set() =~= Set::<int>::empty());
        assert(data_state(s,t,keys.to_set(),cells).logical =~= s.logical);
        assert(data_state(s,t,keys.to_set(),cells).physical =~= s.physical);
    } else {
        transfer::split_set(keys);
        let prior = keys.drop_last();
        assert(prior.to_set().subset_of(keys.to_set()));
        assert forall|k:int| prior.to_set().contains(k) implies s.logical.contains_key(k)
            && s.sessions[t].held.contains_key(k)
            && s.physical.contains_key((s.sessions[t].held[k].owner,k)) by {
            assert(keys.to_set().contains(k));
        }
        data_replay(s,t,prior,cells);
        let prefix = data_writes(s,t,prior,cells);
        let k = keys.last();
        assert(keys.to_set().contains(k));
        let logical = log::Write::Logical { key:k,cell:cells[k] };
        let physical = log::Write::ReplicaCell { owner:s.sessions[t].held[k].owner,key:k,cell:cells[k] };
        data_step(s,t,prior.to_set(),k,cells);
        log::append_write(s,prefix,logical);
        log::append_write(s,prefix.push(logical),physical);
        assert(data_writes(s,t,keys,cells) =~= prefix.push(logical).push(physical));
    }
}

/// Relate the replayed field updates to Resolve only after bytes and writer
/// provenance have independently determined every updated cell.
proof fn data_resolve(c:p::Constants,s:p::State,t:int,cells:Map<int,p::Cell>,values:Map<int,Option<int>>)
    requires cells.dom() == values.dom(),
        forall|k:int| values.contains_key(k) ==> cells[k] == (p::Cell { value:values[k],writer:t }),
    ensures log::apply_write(data_state(s,t,values.dom(),cells),log::Write::Resolved { txn:t,resolved:true })
        == p::apply(c,s,p::Action::Resolve { txn:t,writes:values }),
{
    let next = log::apply_write(data_state(s,t,values.dom(),cells),log::Write::Resolved { txn:t,resolved:true });
    let target = p::apply(c,s,p::Action::Resolve { txn:t,writes:values });
    assert(next.logical =~= target.logical) by {
        assert forall|k:int| next.logical.contains_key(k) implies next.logical[k] == target.logical[k] by {
            if values.contains_key(k) {
                assert(cells[k] == (p::Cell { value:values[k],writer:t }));
            }
        }
    }
    assert(next.physical =~= target.physical) by {
        assert forall|q:(int,int)| next.physical.contains_key(q) implies next.physical[q] == target.physical[q] by {
            if values.contains_key(q.1) && s.sessions[t].held[q.1].owner == q.0 {
                assert(cells[q.1] == (p::Cell { value:values[q.1],writer:t }));
            }
        }
    }
}

/// Successful engine effects derive every cell from actual post-commit bytes.
/// The occurrence may be a runtime nonce or an exclusive bootstrap occurrence;
/// neither case invents native lease activity or assumes model effects.
pub(super) proof fn engine_effect(c:p::Constants,s:p::State,t:int,outcome:EngineOutcome,
    before:EngineImages,after:EngineImages,labels:Map<int,Cell>,writes:Map<int,Option<Seq<u8>>>)
    -> (tracked out:ClosedTransfer)
    requires outcome is Committed,p::can_resolve(s,t,encoded_writes(writes)),
        atomic_bytes(s,t,before,after,labels,writes),
        engine_observed(s,before,labels),
        forall|k:int| writes.contains_key(k) ==> s.logical.contains_key(k)
            && s.physical.contains_key((s.sessions[t].held[k].owner,k)),
    ensures out.valid(c,s),out.after(s).sessions[t].resolved,
        engine_observed(out.after(s),after,labels),
        out.after(s) == engine_state(s,t,writes.dom(),Map::new(writes.dom(),|k:int|
            transfer::observed(after[s.sessions[t].held[k].owner],labels[k],t))),
        forall|k:int| writes.contains_key(k) ==> out.after(s).logical[k] == transfer::observed(
            after[s.sessions[t].held[k].owner],labels[k],t),
{
    let work = writes.dom().to_seq();
    writes.dom().lemma_to_seq_to_set_id();
    assert(work.to_set() == writes.dom());
    let cells = Map::new(writes.dom(),|k:int| transfer::observed(after[s.sessions[t].held[k].owner],labels[k],t));
    assert forall|k:int| writes.contains_key(k) implies cells[k] == (p::Cell { value:encoded_writes(writes)[k],writer:t }) by {
        assert(crate::storage::value(after[s.sessions[t].held[k].owner],labels[k]) == writes[k]);
    }
    assert forall|k:int| work.to_set().contains(k) implies s.logical.contains_key(k)
        && s.sessions[t].held.contains_key(k)
        && s.physical.contains_key((s.sessions[t].held[k].owner,k)) by {
        assert(writes.contains_key(k));
    }
    data_replay(s,t,work,cells);
    let raw = data_writes(s,t,work,cells);
    let resolved = log::Write::Resolved { txn:t,resolved:true };
    log::append_write(s,raw,resolved);
    let raw = raw.push(resolved);
    let next = log::apply_writes(s,raw);
    assert(next == engine_state(s,t,writes.dom(),cells));
    let action = p::Action::Resolve { txn:t,writes:encoded_writes(writes) };
    data_resolve(c,s,t,cells,encoded_writes(writes));
    assert(next == p::apply(c,s,action));
    log::accepted(c,s,next,action);
    assert forall|q:(int,int)| next.physical.contains_key(q) && labels.contains_key(q.1)
        && !(p::replica(next,q.0,q.1).role is Empty)
        && (!(p::replica(next,q.0,q.1).role is Stage) || p::replica(next,q.0,q.1).covered) implies
        after.contains_key(q.0) && p::replica(next,q.0,q.1).cell.value ==
            crate::sharding_bytes::value_option(crate::storage::value(after[q.0],labels[q.1])) by {
        assert(s.physical.contains_key(q));
        assert(p::replica(next,q.0,q.1).role == p::replica(s,q.0,q.1).role);
        assert(p::replica(next,q.0,q.1).covered == p::replica(s,q.0,q.1).covered);
        assert(after.contains_key(q.0));
        if writes.contains_key(q.1) && s.sessions[t].held[q.1].owner == q.0 {
            assert(cells[q.1] == transfer::observed(after[q.0],labels[q.1],t));
        } else {
            assert(p::replica(next,q.0,q.1).cell == p::replica(s,q.0,q.1).cell);
            assert(p::replica(s,q.0,q.1).cell.value ==
                crate::sharding_bytes::value_option(crate::storage::value(before[q.0],labels[q.1])));
            assert(crate::storage::value(after[q.0],labels[q.1]) == crate::storage::value(before[q.0],labels[q.1]));
        }
    }
    ClosedTransfer { segment:log::Segment { writes:raw,states:seq![s,next],actions:seq![action] } }
}

/// Runtime completion additionally binds the proved engine effects to the
/// admitted logical transaction identity, never an engine TID or model oracle.
pub proof fn engine_committed(c:p::Constants,s:p::State,id:TxnId,outcome:EngineOutcome,
    before:EngineImages,after:EngineImages,labels:Map<int,Cell>,writes:Map<int,Option<Seq<u8>>>)
    -> (tracked out:(ClosedTransfer,EngineCompletion))
    requires outcome is Committed,p::can_resolve(s,master::nonce(id),encoded_writes(writes)),
        atomic_bytes(s,master::nonce(id),before,after,labels,writes),
        engine_observed(s,before,labels),
        forall|k:int| writes.contains_key(k) ==> s.logical.contains_key(k)
            && s.physical.contains_key((s.sessions[master::nonce(id)].held[k].owner,k)),
    ensures out.0.valid(c,s),out.1.matches(id),out.0.after(s).sessions[master::nonce(id)].resolved,
        engine_observed(out.0.after(s),after,labels),
        forall|k:int| writes.contains_key(k) ==> out.0.after(s).logical[k] == transfer::observed(
            after[s.sessions[master::nonce(id)].held[k].owner],labels[k],master::nonce(id)),
        out.0.after(s) == engine_state(s,master::nonce(id),writes.dom(),Map::new(writes.dom(),|k:int|
            transfer::observed(after[s.sessions[master::nonce(id)].held[k].owner],labels[k],master::nonce(id)))),
{
    let tracked effect = engine_effect(c,s,master::nonce(id),outcome,before,after,labels,writes);
    (effect,EngineCompletion { id })
}

/// Known abort/failed validation has a proved unchanged engine image. UNKNOWN
/// is deliberately not accepted and cannot release leases or mint completion.
pub proof fn engine_aborted(c:p::Constants,s:p::State,id:TxnId,outcome:EngineOutcome,
    before:EngineImages,after:EngineImages) -> (tracked out:(ClosedTransfer,EngineCompletion))
    requires outcome is Aborted,before == after,s.sessions.contains_key(master::nonce(id)),!s.sessions[master::nonce(id)].resolved,
    ensures out.0.valid(c,s),out.1.matches(id),out.0.after(s).sessions[master::nonce(id)].resolved,
        out.0.after(s).physical == s.physical,out.0.after(s).logical == s.logical,
        out.0.after(s) == (p::State { sessions:s.sessions.insert(master::nonce(id),
            p::Session { resolved:true,..s.sessions[master::nonce(id)] }),..s }),
{
    let t = master::nonce(id);
    let write = log::Write::Resolved { txn:t,resolved:true };
    log::single_write(s,write);
    let next = log::apply_write(s,write);
    let action = p::Action::Resolve { txn:t,writes:Map::empty() };
    assert(p::apply(c,s,action).logical =~= s.logical);
    assert(p::apply(c,s,action).physical =~= s.physical);
    log::accepted(c,s,next,action);
    (ClosedTransfer { segment:log::Segment { writes:seq![write],states:seq![s,next],actions:seq![action] } },EngineCompletion { id })
}

/// Read result agreement uses the real returned byte option, preserving absence.
/// Read-only effects stutter because the actual atomic read frames its image.
pub proof fn engine_read(c:p::Constants,s:p::State,id:TxnId,key:int,label:Cell,before:Image,after:Image,
    returned:Option<Seq<u8>>) -> (tracked out:ClosedTransfer)
    requires p::live_lease(s,master::nonce(id),key),before == after,returned == crate::storage::value(before,label),
        p::replica(s,s.sessions[master::nonce(id)].held[key].owner,key).cell.value ==
            crate::sharding_bytes::value_option(crate::storage::value(before,label)),
    ensures out.valid(c,s),out.after(s) == s,p::read(s,master::nonce(id),key).is_some(),
        p::read(s,master::nonce(id),key).unwrap().value == crate::sharding_bytes::value_option(returned),
        forall|expected:Option<Seq<u8>>| crate::sharding_bytes::value_option(expected)
            == p::read(s,master::nonce(id),key).unwrap().value ==> expected == returned,
{
    assert forall|expected:Option<Seq<u8>>| crate::sharding_bytes::value_option(expected)
        == p::read(s,master::nonce(id),key).unwrap().value implies expected == returned by {
        crate::sharding_bytes::value_option_injective(expected,returned);
    }
    empty_segment(c,s)
}

pub open spec fn request_writes(request:crate::gateway::RequestView,reply:crate::gateway::ReplyView,key:int)
    -> Map<int,Option<Seq<u8>>> {
    if request.kind == crate::gateway::GET
        || ((request.kind == crate::gateway::INSERT || request.kind == crate::gateway::DELETE) && !reply.op_result) { Map::empty() }
    else { Map::empty().insert(key,if request.kind == crate::gateway::DELETE { None } else { Some(request.value) }) }
}

/// End-to-end external point-operation binding: checked Gateway admission
/// selects this exact request; the retained callback reply binds its actual
/// bytes/outcome. Only the explicit atomic engine image relation is trusted.
pub proof fn gateway_engine_committed(c:p::Constants,s:p::State,participant:u32,
    admission_before:Map<u64,crate::gateway::StreamView>,admission_after:Map<u64,crate::gateway::StreamView>,
    callback_before:Map<u64,crate::gateway::StreamView>,callback_after:Map<u64,crate::gateway::StreamView>,
    request:crate::gateway::RequestView,reply:crate::gateway::ReplyView,
    before:EngineImages,after:EngineImages,labels:Map<int,Cell>,key:int)
    -> (tracked out:(ClosedTransfer,EngineCompletion))
    requires
        forall|client:u64| admission_before.contains_key(client) ==> crate::gateway::stream_wf(admission_before[client]),
        crate::gateway::admission_effect(admission_before,admission_after,participant,request,crate::gateway::AdmissionView::Execute),
        callback_before.contains_key(request.client),callback_before[request.client] == admission_after[request.client],
        crate::gateway::finish_effect(callback_before,callback_after,request.client,request.sequence,reply),
        request.owner == participant,reply.status == 0,reply.outcome == crate::gateway::COMMITTED,
        labels.contains_key(key),labels[key] == (request.table,(request.coordinate,request.key)),
        p::live_lease(s,master::nonce(TxnId { client:request.client,sequence:request.sequence }),key),
        s.sessions[master::nonce(TxnId { client:request.client,sequence:request.sequence })].held[key] ==
            (p::Grant { owner:request.owner as int,epoch:request.epoch as nat }),
        atomic_bytes(s,master::nonce(TxnId { client:request.client,sequence:request.sequence }),before,after,labels,request_writes(request,reply,key)),
        s.logical.contains_key(key),s.physical.contains_key((request.owner as int,key)),
        engine_observed(s,before,labels),
        before.contains_key(request.owner as int),
        request.kind == crate::gateway::GET ==> (if reply.op_result { Some(reply.value) } else { None })
            == crate::storage::value(before[request.owner as int],labels[key]),
        request.kind == crate::gateway::GET ==> p::replica(s,request.owner as int,key).cell.value ==
            crate::sharding_bytes::value_option(crate::storage::value(before[request.owner as int],labels[key])),
        request.kind == crate::gateway::INSERT ==> reply.op_result == !before[request.owner as int].dom().contains(labels[key]),
        request.kind == crate::gateway::DELETE ==> reply.op_result == before[request.owner as int].dom().contains(labels[key]),
    ensures out.0.valid(c,s),out.1.matches(TxnId { client:request.client,sequence:request.sequence }),
        callback_after[request.client].request == request,callback_after[request.client].reply == Some(reply),
        out.0.after(s).sessions[master::nonce(TxnId { client:request.client,sequence:request.sequence })].resolved,
        request.kind == crate::gateway::GET ==> p::read(s,master::nonce(TxnId { client:request.client,sequence:request.sequence }),key).unwrap().value
            == if reply.op_result { Some(crate::sharding_bytes::value_code(reply.value)) } else { None },
{
    crate::gateway::admission_safety(admission_before,admission_after,participant,request,crate::gateway::AdmissionView::Execute);
    crate::gateway::retained_callback_result(callback_before,callback_after,request,reply);
    let id = TxnId { client:request.client,sequence:request.sequence };
    let writes = request_writes(request,reply,key);
    assert forall|k:int| writes.contains_key(k) implies k == key by {}
    assert(encoded_writes(writes).dom().subset_of(s.sessions[master::nonce(id)].held.dom())) by {
        assert forall|k:int| encoded_writes(writes).contains_key(k) implies
            s.sessions[master::nonce(id)].held.contains_key(k) by {
            assert(k == key);
        }
    }
    assert(p::can_resolve(s,master::nonce(id),encoded_writes(writes)));
    assert forall|k:int| writes.contains_key(k) implies s.logical.contains_key(k)
        && s.physical.contains_key((s.sessions[master::nonce(id)].held[k].owner,k)) by {
        assert(k == key);
    }
    if request.kind == crate::gateway::GET {
        let returned = if reply.op_result { Some(reply.value) } else { None };
        let tracked read = engine_read(c,s,id,key,labels[key],before[request.owner as int],
            before[request.owner as int],returned);
    }
    engine_committed(c,s,id,EngineOutcome::Committed,before,after,labels,request_writes(request,reply,key))
}

pub proof fn gateway_engine_aborted(c:p::Constants,s:p::State,participant:u32,
    admission_before:Map<u64,crate::gateway::StreamView>,admission_after:Map<u64,crate::gateway::StreamView>,
    callback_before:Map<u64,crate::gateway::StreamView>,callback_after:Map<u64,crate::gateway::StreamView>,
    request:crate::gateway::RequestView,reply:crate::gateway::ReplyView,before:EngineImages,after:EngineImages)
    -> (tracked out:(ClosedTransfer,EngineCompletion))
    requires
        forall|client:u64| admission_before.contains_key(client) ==> crate::gateway::stream_wf(admission_before[client]),
        crate::gateway::admission_effect(admission_before,admission_after,participant,request,crate::gateway::AdmissionView::Execute),
        callback_before.contains_key(request.client),callback_before[request.client] == admission_after[request.client],
        crate::gateway::finish_effect(callback_before,callback_after,request.client,request.sequence,reply),
        request.owner == participant,reply.outcome == crate::gateway::ABORTED,before == after,
        s.sessions.contains_key(master::nonce(TxnId { client:request.client,sequence:request.sequence })),
        !s.sessions[master::nonce(TxnId { client:request.client,sequence:request.sequence })].resolved,
    ensures out.0.valid(c,s),out.1.matches(TxnId { client:request.client,sequence:request.sequence }),
        callback_after[request.client].request == request,callback_after[request.client].reply == Some(reply),
        out.0.after(s).sessions[master::nonce(TxnId { client:request.client,sequence:request.sequence })].resolved,
{
    crate::gateway::admission_safety(admission_before,admission_after,participant,request,crate::gateway::AdmissionView::Execute);
    crate::gateway::retained_callback_result(callback_before,callback_after,request,reply);
    engine_aborted(c,s,TxnId { client:request.client,sequence:request.sequence },EngineOutcome::Aborted,before,after)
}
} // verus!
