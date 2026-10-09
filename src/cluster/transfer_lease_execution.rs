//! Source-certified lease and atomic-engine records. Native sessions retain one
//! current sequence per client; the model retains historical transaction IDs.
//! A successful engine callback supplies an explicit atomic byte effect, not a
//! trusted transaction-handler postcondition. UNKNOWN never mints completion.
use super::*;
use crate::leases::{self as native,SessionView};
use crate::leases_proofs as expansion;
use crate::migration_refinement as master;
use crate::types::{TxnId,Grant,Status};
verus! {
pub open spec fn transaction(id:TxnId) -> int { master::nonce(id) }
pub open spec fn held_image(session:SessionView,addresses:expansion::Addresses) -> Map<int,p::Grant> {
    let held = expansion::expanded(session,addresses);
    Map::new(held.dom(),|k:int| master::grant(held[k]))
}
pub open spec fn session_observed(registry:Map<u64,SessionView>,s:p::State,id:TxnId,addresses:expansion::Addresses) -> bool {
    registry.contains_key(id.client) && registry[id.client].sequence == id.sequence
        && s.sessions.contains_key(transaction(id))
        && s.sessions[transaction(id)].held == held_image(registry[id.client],addresses)
}
pub open spec fn registry_wf(registry:Map<u64,SessionView>) -> bool {
    forall|client:u64| registry.contains_key(client) ==> registry[client].wf()
}

/// Freshness is the retained native sequence frontier coupled to the global
/// allocated-ID history. A repeated begin of the same open session stutters.
pub proof fn native_begin(c:p::Constants,s:p::State,before:Map<u64,SessionView>,after:Map<u64,SessionView>,id:TxnId,status:Status,
    addresses:expansion::Addresses) -> (tracked out:ClosedTransfer)
    requires native::begin_effect(before,after,id,status),
        before != after ==> !s.sessions.contains_key(transaction(id)),
        before == after && status == Status::Ok ==> session_observed(before,s,id,addresses),
    ensures out.valid(c,s),status == Status::Ok ==> session_observed(after,out.after(s),id,addresses),
        status != Status::Ok ==> out.after(s) == s,
{
    if before == after { empty_segment(c,s) } else {
        let t = transaction(id);
        master::nonce_injective(id,id);
        let write = log::Write::Session { txn:t,value:p::Session { held:Map::empty(),resolved:false } };
        log::single_write(s,write);
        let next = log::apply_write(s,write);
        assert(expansion::expanded(after[id.client],addresses).dom() =~= Set::<int>::empty());
        assert(held_image(after[id.client],addresses) =~= Map::<int,p::Grant>::empty());
        log::accepted(c,s,next,p::Action::Open { txn:t });
        ClosedTransfer { segment:log::Segment { writes:seq![write],states:seq![s,next],actions:seq![p::Action::Open { txn:t }] } }
    }
}

pub proof fn native_lease_unchanged(c:p::Constants,s:p::State,before:Map<u64,SessionView>,after:Map<u64,SessionView>)
    -> (tracked out:ClosedTransfer)
    requires before == after,
    ensures out.valid(c,s),out.after(s) == s,
{ empty_segment(c,s) }

pub(super) open spec fn overlay(old:Map<int,p::Grant>,target:Map<int,p::Grant>,keys:Set<int>) -> Map<int,p::Grant> {
    Map::new(old.dom().union(keys),|k:int| if keys.contains(k) { target[k] } else { old[k] })
}

/// Finite lease expansion enumerates each logical key exactly once. The set
/// identity alone does not establish the freshness needed by Acquire/Release.
pub(super) proof fn lease_keys(keys:Set<int>)
    ensures keys.to_seq().to_set() == keys,keys.to_seq().no_duplicates(),
    decreases keys.len(),
{
    keys.lemma_to_seq_to_set_id();
    if keys.len() > 0 {
        let key = keys.choose();
        let rest = keys.remove(key);
        lease_keys(rest);
        let work = keys.to_seq();
        assert(work =~= seq![key] + rest.to_seq());
        assert forall|i:int,j:int| 0 <= i < work.len() && 0 <= j < work.len() && i != j
            implies work[i] != work[j] by {
            if i == 0 {
                assert(rest.to_seq().to_set().contains(rest.to_seq()[j-1]));
            } else if j == 0 {
                assert(rest.to_seq().to_set().contains(rest.to_seq()[i-1]));
            } else {
                assert(rest.to_seq()[i-1] != rest.to_seq()[j-1]);
            }
        }
    }
}

/// Expose membership and index facts at the recursive sequence boundary rather
/// than asking quantified source guards to infer them from set insertion.
proof fn lease_prefix(keys:Seq<int>)
    requires keys.len() > 0,keys.no_duplicates(),
    ensures keys.drop_last().no_duplicates(),
        keys.drop_last().to_set().subset_of(keys.to_set()),
        keys.to_set().contains(keys.last()),
        !keys.drop_last().to_set().contains(keys.last()),
        keys.to_set() == keys.drop_last().to_set().insert(keys.last()),
{
    transfer::split_set(keys);
    assert forall|i:int,j:int| 0 <= i < keys.drop_last().len()
        && 0 <= j < keys.drop_last().len() && i != j
        implies keys.drop_last()[i] != keys.drop_last()[j] by {
        assert(keys[i] != keys[j]);
    }
    if keys.drop_last().to_set().contains(keys.last()) {
        let i = choose|i:int| 0 <= i < keys.drop_last().len() && keys.drop_last()[i] == keys.last();
        assert(keys[i] == keys[keys.len() as int-1]);
        assert(keys[i] != keys[keys.len() as int-1]);
    }
}
pub(super) proof fn acquire_keys(c:p::Constants,s:p::State,t:int,work:Seq<int>,target:Map<int,p::Grant>) -> (tracked out:ClosedTransfer)
    requires s.sessions.contains_key(t),!s.sessions[t].resolved,work.no_duplicates(),
        work.to_set().subset_of(target.dom()),work.to_set().disjoint(s.sessions[t].held.dom()),
        forall|k:int| work.to_set().contains(k) ==> s.physical.contains_key((target[k].owner,k))
            && p::replica(s,target[k].owner,k).role is Serving && p::replica(s,target[k].owner,k).epoch == target[k].epoch,
    ensures out.valid(c,s),out.after(s) == (p::State { sessions:s.sessions.insert(t,p::Session {
        held:overlay(s.sessions[t].held,target,work.to_set()),resolved:false }),..s }),
    decreases work.len(),
{
    hide(log::certificate);
    if work.len() == 0 {
        assert(work.to_set() =~= Set::<int>::empty());
        assert(overlay(s.sessions[t].held,target,work.to_set()) =~= s.sessions[t].held);
        assert(s.sessions.insert(t,s.sessions[t]) =~= s.sessions);
        empty_segment(c,s)
    } else {
        lease_prefix(work);
        assert forall|k:int| work.drop_last().to_set().contains(k) implies
            s.physical.contains_key((target[k].owner,k))
                && p::replica(s,target[k].owner,k).role is Serving
                && p::replica(s,target[k].owner,k).epoch == target[k].epoch by {
            assert(work.to_set().contains(k));
        }
        let tracked prefix = acquire_keys(c,s,t,work.drop_last(),target);
        let mid = prefix.after(s);
        let k = work.last();
        assert(work.to_set().contains(k));
        assert(!mid.sessions[t].held.contains_key(k));
        assert(mid.physical == s.physical);
        assert(p::replica(mid,target[k].owner,k) == p::replica(s,target[k].owner,k));
        let write = log::Write::Held { txn:t,key:k,grant:target[k] };
        log::single_write(mid,write);
        let next = log::apply_write(mid,write);
        let action = p::Action::Acquire { txn:t,key:k,grant:target[k] };
        assert(p::enabled(c,mid,action));
        assert(next == p::apply(c,mid,action));
        log::accepted(c,mid,next,action);
        let tracked step = ClosedTransfer { segment:log::Segment { writes:seq![write],states:seq![mid,next],actions:seq![action] } };
        assert(step.valid(c,mid)) by { reveal(log::certificate); }
        assert(step.after(mid) == next);
        assert(next.sessions[t].held =~= overlay(s.sessions[t].held,target,work.to_set()));
        assert(next.sessions =~= s.sessions.insert(t,p::Session {
            held:overlay(s.sessions[t].held,target,work.to_set()),resolved:false }));
        join(c,s,prefix,step)
    }
}

/// This adapter lemma consumes the exact registration accounting established by
/// the registry source body and the native participant's Serving/epoch check.
proof fn registered(c:p::Constants,s:p::State,before:Map<u64,SessionView>,after:Map<u64,SessionView>,id:TxnId,
    scope:native::ScopeView,node:&Participant,addresses:expansion::Addresses)
    -> (tracked out:ClosedTransfer)
    requires registry_wf(before),registry_wf(after),session_observed(before,s,id,addresses),
        before[id.client].sequence == id.sequence,!before[id.client].terminal,!s.sessions[transaction(id)].resolved,
        after.contains_key(id.client),after[id.client].sequence == id.sequence,
        forall|k:int,g:Grant| addresses.contains_key(k) ==> (after[id.client].has(addresses[k].0,addresses[k].1,g)
            <==> before[id.client].has(addresses[k].0,addresses[k].1,g)
                || scope.contains(addresses[k].0,addresses[k].1) && g == scope.grant),
        forall|k:int| addresses.contains_key(k) && scope.contains(addresses[k].0,addresses[k].1)
            && !expansion::expanded(before[id.client],addresses).contains_key(k) ==>
                node.local_meta(addresses[k].0,addresses[k].1).is_some()
                && node.local_meta(addresses[k].0,addresses[k].1).unwrap().role == crate::types::Role::Serving
                && node.local_meta(addresses[k].0,addresses[k].1).unwrap().epoch == scope.grant.epoch,
        scope.grant.owner == node.owner_view(),
        forall|k:int| addresses.contains_key(k) ==> s.physical.contains_key((node.owner_view() as int,k))
            && node.local_meta(addresses[k].0,addresses[k].1).is_some()
            && crate::participant_proofs::metadata(node.local_meta(addresses[k].0,addresses[k].1).unwrap(),p::replica(s,node.owner_view() as int,k)),
    ensures out.valid(c,s),session_observed(after,out.after(s),id,addresses),
        out.after(s).physical == s.physical,out.after(s).logical == s.logical,
{
    let initial = expansion::expanded(before[id.client],addresses);
    let final_holds = expansion::expanded(after[id.client],addresses);
    let target = held_image(after[id.client],addresses);
    assert(initial.dom().subset_of(final_holds.dom())) by {
        assert forall|k:int| initial.dom().contains(k) implies final_holds.dom().contains(k) by {
            expansion::expanded_exact(before[id.client],addresses,k,initial[k]);
            expansion::expanded_exact(after[id.client],addresses,k,initial[k]);
        }
    }
    assert forall|k:int| initial.dom().contains(k) implies final_holds[k] == initial[k] by {
        expansion::expanded_exact(before[id.client],addresses,k,initial[k]);
        expansion::expanded_exact(after[id.client],addresses,k,initial[k]);
    }
    let added = final_holds.dom().difference(initial.dom());
    lease_keys(added);
    let work = added.to_seq();
    assert forall|k:int| added.contains(k) implies final_holds[k] == scope.grant
        && scope.contains(addresses[k].0,addresses[k].1) by {
        expansion::expanded_exact(after[id.client],addresses,k,final_holds[k]);
        expansion::expanded_exact(before[id.client],addresses,k,final_holds[k]);
    }
    assert forall|k:int| work.to_set().contains(k) implies s.physical.contains_key((target[k].owner,k))
        && p::replica(s,target[k].owner,k).role is Serving && p::replica(s,target[k].owner,k).epoch == target[k].epoch by {
        assert(added.contains(k));
        assert(crate::participant_proofs::metadata(node.local_meta(addresses[k].0,addresses[k].1).unwrap(),p::replica(s,node.owner_view() as int,k)));
    }
    let tracked out = acquire_keys(c,s,transaction(id),work,target);
    assert(s.sessions[transaction(id)].held.dom() == initial.dom());
    assert(target.dom() == final_holds.dom());
    assert(overlay(s.sessions[transaction(id)].held,target,work.to_set()) =~= target) by {
        assert forall|k:int| target.contains_key(k) && !added.contains(k)
            implies s.sessions[transaction(id)].held[k] == target[k] by {
            assert(initial.contains_key(k));
            assert(final_holds[k] == initial[k]);
        }
    }
    out
}

pub proof fn native_point_admission(c:p::Constants,s:p::State,before:&Participant,after:&Participant,
    id:TxnId,table:u64,coordinate:Seq<u8>,grant:Grant,status:Status,addresses:expansion::Addresses)
    -> (tracked out:ClosedTransfer)
    requires before.wf(),after.wf(),registry_wf(before.lease_view()),registry_wf(after.lease_view()),
        session_observed(before.lease_view(),s,id,addresses),
        status != Status::Ok ==> after.lease_view() == before.lease_view(),
        status == Status::Ok ==> after.lease_view() == before.lease_view()
            || native::acquire_effect(before.lease_view(),after.lease_view(),id,table,coordinate,grant,status),
        after.lease_view() != before.lease_view() ==> !before.open_hold(id,table,coordinate,grant),
        status == Status::Ok ==> grant.owner == before.owner_view(),
        status == Status::Ok && !before.open_hold(id,table,coordinate,grant) ==>
            before.local_meta(table,coordinate).is_some() && before.local_meta(table,coordinate).unwrap().role == crate::types::Role::Serving
                && before.local_meta(table,coordinate).unwrap().epoch == grant.epoch,
        after.lease_view() != before.lease_view() ==> !s.sessions[transaction(id)].resolved,
        forall|k:int| addresses.contains_key(k) ==> s.physical.contains_key((before.owner_view() as int,k))
            && before.local_meta(addresses[k].0,addresses[k].1).is_some()
            && crate::participant_proofs::metadata(before.local_meta(addresses[k].0,addresses[k].1).unwrap(),p::replica(s,before.owner_view() as int,k)),
    ensures out.valid(c,s),session_observed(after.lease_view(),out.after(s),id,addresses),
        out.after(s).physical == s.physical,out.after(s).logical == s.logical,
{
    let b = before.lease_view(); let z = after.lease_view();
    if b == z { empty_segment(c,s) } else {
        assert forall|k:int,g:Grant| addresses.contains_key(k) implies (z[id.client].has(addresses[k].0,addresses[k].1,g)
            <==> b[id.client].has(addresses[k].0,addresses[k].1,g)
                || native::point_scope(table,coordinate,grant).contains(addresses[k].0,addresses[k].1) && g == grant) by {
            expansion::acquire_accounting(b,z,id,table,coordinate,grant,addresses[k].0,addresses[k].1,g);
        }
        registered(c,s,b,z,id,native::point_scope(table,coordinate,grant),before,addresses)
    }
}

pub proof fn native_range_admission(c:p::Constants,s:p::State,before:&Participant,after:&Participant,
    id:TxnId,table:u64,lo:Seq<u8>,hi:Option<Seq<u8>>,grant:Grant,status:Status,addresses:expansion::Addresses)
    -> (tracked out:ClosedTransfer)
    requires before.wf(),after.wf(),registry_wf(before.lease_view()),registry_wf(after.lease_view()),
        session_observed(before.lease_view(),s,id,addresses),
        status != Status::Ok ==> after.lease_view() == before.lease_view(),
        status == Status::Ok ==> after.lease_view() == before.lease_view()
            || native::acquire_range_effect(before.lease_view(),after.lease_view(),id,table,lo,hi,grant,status),
        after.lease_view() != before.lease_view() ==> !before.open_range(id,table,lo,hi,grant),
        status == Status::Ok ==> grant.owner == before.owner_view(),
        status == Status::Ok && !before.open_range(id,table,lo,hi,grant) ==>
            forall|coordinate:Seq<u8>| crate::directory::inside(coordinate,lo,hi) ==>
                before.local_meta(table,coordinate).is_some() && before.local_meta(table,coordinate).unwrap().role == crate::types::Role::Serving
                    && before.local_meta(table,coordinate).unwrap().epoch == grant.epoch,
        after.lease_view() != before.lease_view() ==> !s.sessions[transaction(id)].resolved,
        forall|k:int| addresses.contains_key(k) ==> s.physical.contains_key((before.owner_view() as int,k))
            && before.local_meta(addresses[k].0,addresses[k].1).is_some()
            && crate::participant_proofs::metadata(before.local_meta(addresses[k].0,addresses[k].1).unwrap(),p::replica(s,before.owner_view() as int,k)),
    ensures out.valid(c,s),session_observed(after.lease_view(),out.after(s),id,addresses),
        out.after(s).physical == s.physical,out.after(s).logical == s.logical,
{
    let b = before.lease_view(); let z = after.lease_view();
    if b == z { empty_segment(c,s) } else {
        assert forall|k:int,g:Grant| addresses.contains_key(k) implies (z[id.client].has(addresses[k].0,addresses[k].1,g)
            <==> b[id.client].has(addresses[k].0,addresses[k].1,g)
                || native::range_scope(table,lo,hi,grant).contains(addresses[k].0,addresses[k].1) && g == grant) by {
            expansion::acquire_range_expansion(b,z,id,table,lo,hi,grant,addresses,k,g);
            expansion::expanded_exact(b[id.client],addresses,k,g);
            expansion::expanded_exact(z[id.client],addresses,k,g);
        }
        assert forall|k:int| addresses.contains_key(k)
            && native::range_scope(table,lo,hi,grant).contains(addresses[k].0,addresses[k].1)
            && !expansion::expanded(b[id.client],addresses).contains_key(k) implies
                before.local_meta(addresses[k].0,addresses[k].1).is_some()
                && before.local_meta(addresses[k].0,addresses[k].1).unwrap().role == crate::types::Role::Serving
                && before.local_meta(addresses[k].0,addresses[k].1).unwrap().epoch == grant.epoch by {
            let coordinate = addresses[k].1;
            assert(addresses[k].0 == table);
            crate::bytes::cmp_laws(lo,coordinate);
            if let Some(upper) = hi { crate::bytes::cmp_laws(coordinate,upper); }
            assert(crate::directory::inside(coordinate,lo,hi));
            assert(!before.open_range(id,table,lo,hi,grant));
            assert(before.local_meta(table,coordinate).is_some()
                && before.local_meta(table,coordinate).unwrap().role == crate::types::Role::Serving
                && before.local_meta(table,coordinate).unwrap().epoch == grant.epoch);
        }
        registered(c,s,b,z,id,native::range_scope(table,lo,hi,grant),before,addresses)
    }
}

pub(super) proof fn release_keys(c:p::Constants,s:p::State,t:int,keys:Seq<int>) -> (tracked out:ClosedTransfer)
    requires s.sessions.contains_key(t),s.sessions[t].resolved,
        keys.no_duplicates(),keys.to_set().subset_of(s.sessions[t].held.dom()),
    ensures out.valid(c,s),out.after(s) == (p::State { sessions:s.sessions.insert(t,p::Session {
        held:s.sessions[t].held.remove_keys(keys.to_set()),resolved:true }),..s }),
    decreases keys.len(),
{
    hide(log::certificate);
    if keys.len() == 0 {
        assert(keys.to_set() =~= Set::<int>::empty());
        assert(s.sessions[t].held.remove_keys(keys.to_set()) =~= s.sessions[t].held);
        assert(s.sessions.insert(t,s.sessions[t]) =~= s.sessions);
        empty_segment(c,s)
    } else {
        lease_prefix(keys);
        let tracked prefix = release_keys(c,s,t,keys.drop_last());
        let mid = prefix.after(s);
        let key = keys.last();
        assert(keys.to_set().contains(key));
        assert(mid.sessions[t].held.contains_key(key));
        let write = log::Write::Release { txn:t,key };
        log::single_write(mid,write);
        let next = log::apply_write(mid,write);
        let action = p::Action::Release { txn:t,key };
        assert(p::enabled(c,mid,action));
        assert(next == p::apply(c,mid,action));
        log::accepted(c,mid,next,action);
        let tracked step = ClosedTransfer { segment:log::Segment { writes:seq![write],states:seq![mid,next],actions:seq![action] } };
        assert(step.valid(c,mid)) by { reveal(log::certificate); }
        assert(step.after(mid) == next);
        assert(next.sessions[t].held =~= s.sessions[t].held.remove_keys(keys.to_set()));
        assert(next.sessions =~= s.sessions.insert(t,p::Session {
            held:s.sessions[t].held.remove_keys(keys.to_set()),resolved:true }));
        join(c,s,prefix,step)
    }
}

/// Finish is sequenced after the actual terminal engine callback. It resolves
/// the native fence and releases ALL expanded keys; it is not an engine commit.
pub proof fn native_finish(c:p::Constants,s:p::State,before:Map<u64,SessionView>,after:Map<u64,SessionView>,id:TxnId,status:Status,
    addresses:expansion::Addresses,tracked completion:&EngineCompletion) -> (tracked out:ClosedTransfer)
    requires native::finish_effect(before,after,id,status),session_observed(before,s,id,addresses),
        completion.matches(id),s.sessions[transaction(id)].resolved,
    ensures out.valid(c,s),session_observed(after,out.after(s),id,addresses),
        status == Status::Ok ==> out.after(s).sessions[transaction(id)].held.dom() == Set::<int>::empty(),
{
    expansion::finish_accounting(before,after,id,status,addresses);
    if status != Status::Ok { empty_segment(c,s) } else {
        lease_keys(s.sessions[transaction(id)].held.dom());
        let keys = s.sessions[transaction(id)].held.dom().to_seq();
        let tracked out = release_keys(c,s,transaction(id),keys);
        assert(s.sessions[transaction(id)].held.remove_keys(keys.to_set()) =~= Map::<int,p::Grant>::empty());
        assert(held_image(after[id.client],addresses) =~= Map::<int,p::Grant>::empty());
        out
    }
}
} // verus!
