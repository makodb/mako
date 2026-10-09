//! Distributed lease accounting: local registries form one logical hold union.
//! A local finish is not permission to release another owner's registrations.
use super::*;
use crate::leases::{self as native,SessionView};
use crate::leases_proofs as expansion;
use crate::types::{TxnId,Grant,Status,KeyRange};
verus! {

pub type Registries = Map<u32,Map<u64,SessionView>>;
pub open spec fn registry_id(client:u64,session:SessionView) -> TxnId {
    TxnId { client,sequence:session.sequence }
}
/// The forward clause covers every retained native session. The reverse clause
/// rules out a logical hold silently omitted from all participant registries.
/// Historical empty model sessions (including bootstrap) need no native slot.
pub open spec fn registries_observed(registries:Registries,s:p::State,addresses:expansion::Addresses) -> bool {
    (forall|owner:u32,client:u64| registries.contains_key(owner) && registries[owner].contains_key(client) ==>
        session_observed(registries[owner],s,registry_id(client,registries[owner][client]),addresses,owner))
    && forall|t:int,k:int| s.sessions.contains_key(t) && s.sessions[t].held.contains_key(k) ==>
        exists|owner:u32,client:u64| registries.contains_key(owner) && registries[owner].contains_key(client)
            && transaction(registry_id(client,registries[owner][client])) == t
            && held_image(registries[owner][client],addresses).contains_key(k)
            && held_image(registries[owner][client],addresses)[k] == s.sessions[t].held[k]
            && s.sessions[t].held[k].owner == owner as int
}

proof fn registration_witness(registries:Registries,s:p::State,addresses:expansion::Addresses,t:int,k:int)
    -> (pair:(u32,u64))
    requires registries_observed(registries,s,addresses),s.sessions.contains_key(t),s.sessions[t].held.contains_key(k),
    ensures registries.contains_key(pair.0),registries[pair.0].contains_key(pair.1),
        transaction(registry_id(pair.1,registries[pair.0][pair.1]))==t,
        held_image(registries[pair.0][pair.1],addresses).contains_key(k),
        held_image(registries[pair.0][pair.1],addresses)[k]==s.sessions[t].held[k],
        s.sessions[t].held[k].owner==pair.0 as int,
{
    let owner=choose|owner:u32| #[trigger] registries.contains_key(owner) && exists|client:u64| registries[owner].contains_key(client)
        && transaction(registry_id(client,registries[owner][client]))==t
        && held_image(registries[owner][client],addresses).contains_key(k)
        && held_image(registries[owner][client],addresses)[k]==s.sessions[t].held[k]
        && s.sessions[t].held[k].owner==owner as int;
    let client=choose|client:u64| registries.contains_key(owner) && registries[owner].contains_key(client)
        && transaction(registry_id(client,registries[owner][client]))==t
        && held_image(registries[owner][client],addresses).contains_key(k)
        && held_image(registries[owner][client],addresses)[k]==s.sessions[t].held[k]
        && s.sessions[t].held[k].owner==owner as int;
    (owner,client)
}

/// Resolution, data writes and metadata transitions do not change registrations.
pub proof fn registries_frame(registries:Registries,before:p::State,after:p::State,addresses:expansion::Addresses)
    requires registries_observed(registries,before,addresses),
        before.sessions.dom()==after.sessions.dom(),
        forall|t:int| before.sessions.contains_key(t) ==> before.sessions[t].held==after.sessions[t].held,
    ensures registries_observed(registries,after,addresses),
{}

/// One owner's one client slot changes. A superseded slot must already be empty;
/// a current transaction's registrations at every other owner are preserved.
pub proof fn registry_replaced(registries:Registries,before:p::State,after:p::State,
    addresses:expansion::Addresses,owner:u32,id:TxnId,local:Map<u64,SessionView>)
    requires registries_observed(registries,before,addresses),registries.contains_key(owner),
        local.contains_key(id.client),local[id.client].sequence==id.sequence,
        local==registries[owner].insert(id.client,local[id.client]),
        session_observed(local,after,id,addresses,owner),
        after.sessions.dom()==before.sessions.dom().insert(transaction(id)),
        forall|t:int| before.sessions.contains_key(t) && t!=transaction(id) ==>
            after.sessions[t].held==before.sessions[t].held,
        forall|other:u32| other!=owner ==> owned_holds(after.sessions[transaction(id)].held,other)==
            if before.sessions.contains_key(transaction(id)) { owned_holds(before.sessions[transaction(id)].held,other) }
            else { Map::<int,p::Grant>::empty() },
        forall|k:int| after.sessions[transaction(id)].held.contains_key(k)
            && after.sessions[transaction(id)].held[k].owner!=owner as int ==>
            before.sessions.contains_key(transaction(id)) && before.sessions[transaction(id)].held.contains_key(k)
                && before.sessions[transaction(id)].held[k]==after.sessions[transaction(id)].held[k],
        registries[owner].contains_key(id.client) && registries[owner][id.client].sequence!=id.sequence ==>
            held_image(registries[owner][id.client],addresses).dom()==Set::<int>::empty(),
    ensures registries_observed(registries.insert(owner,local),after,addresses),
{
    let result=registries.insert(owner,local);
    let target=transaction(id);
    assert forall|o:u32,client:u64| result.contains_key(o) && result[o].contains_key(client) implies
        session_observed(result[o],after,registry_id(client,result[o][client]),addresses,o) by {
        let current=registry_id(client,result[o][client]);
        if o==owner && client==id.client {
            assert(current==id);
        } else {
            assert(result[o][client]==registries[o][client]);
            assert(session_observed(registries[o],before,current,addresses,o));
            if transaction(current)==target {
                crate::migration_refinement::nonce_injective(current,id);
                assert(o!=owner);
            } else {
                assert(after.sessions[transaction(current)].held==before.sessions[transaction(current)].held);
            }
        }
    }
    assert forall|t:int,k:int| after.sessions.contains_key(t) && after.sessions[t].held.contains_key(k) implies
        exists|o:u32,client:u64| result.contains_key(o) && result[o].contains_key(client)
            && transaction(registry_id(client,result[o][client]))==t
            && held_image(result[o][client],addresses).contains_key(k)
            && held_image(result[o][client],addresses)[k]==after.sessions[t].held[k]
            && after.sessions[t].held[k].owner==o as int by {
        if t==target && after.sessions[t].held[k].owner==owner as int {
            assert(owned_holds(after.sessions[t].held,owner).contains_key(k));
            assert(held_image(local[id.client],addresses).contains_key(k));
            assert(result[owner][id.client]==local[id.client]);
        } else {
            assert(before.sessions.contains_key(t) && before.sessions[t].held.contains_key(k));
            assert(before.sessions[t].held[k]==after.sessions[t].held[k]);
            let pair=registration_witness(registries,before,addresses,t,k);
            if pair.0==owner && pair.1==id.client {
                if registries[owner][id.client].sequence==id.sequence {
                    assert(t==target);
                } else {
                    assert(held_image(registries[owner][id.client],addresses).dom()==Set::<int>::empty());
                }
                assert(false);
            }
            assert(result[pair.0][pair.1]==registries[pair.0][pair.1]);
        }
    }
}

/// No finite-coverage premise is needed for the safety direction: every actual
/// selected logical hold would occur in the frozen source's exact registry.
pub proof fn global_drain_from_source(c:p::Constants,s:p::State,registries:Registries,
    addresses:expansion::Addresses,range:KeyRange,keys:Set<int>,owner:u32)
    requires p::inv(c,s),registries_observed(registries,s,addresses),registries.contains_key(owner),
        keys.subset_of(c.keys),addresses.dom() == c.keys,
        forall|k:int| keys.contains(k) ==> crate::bytes::contains_spec(range,addresses[k].0,addresses[k].1)
            && p::directory(s)[k].owner == owner as int,
        forall|client:u64| registries[owner].contains_key(client) ==> registries[owner][client].drained(range),
    ensures p::drained(s,keys),
{
    assert forall|t:int,k:int| s.sessions.contains_key(t) && keys.contains(k)
        implies !s.sessions[t].held.contains_key(k) by {
        if s.sessions[t].held.contains_key(k) {
            let pair=registration_witness(registries,s,addresses,t,k);
            assert(p::session_inv(c,s,t));
            assert(pair.0 == owner);
            expansion::native_drain_no_forgotten_keys(registries[owner][pair.1],addresses,range);
            assert(expansion::expanded(registries[owner][pair.1],addresses).contains_key(k));
        }
    }
}

/// Exact equivalence additionally observes each nonempty native scope/range
/// intersection, including absent rows. Merely enumerating stored rows is not
/// enough. The owner condition is valid before publication, not after activation.
pub proof fn global_drain_equivalence(c:p::Constants,s:p::State,registries:Registries,
    addresses:expansion::Addresses,range:KeyRange,keys:Set<int>,owner:u32)
    requires p::inv(c,s),registries_observed(registries,s,addresses),registries.contains_key(owner),
        keys.subset_of(c.keys),addresses.dom() == c.keys,
        forall|k:int| addresses.contains_key(k) ==> (keys.contains(k)
            == crate::bytes::contains_spec(range,addresses[k].0,addresses[k].1)),
        forall|k:int| keys.contains(k) ==> p::directory(s)[k].owner == owner as int,
        forall|client:u64| registries[owner].contains_key(client) ==>
            expansion::intersection_coverage(registries[owner][client],addresses,range),
    ensures p::drained(s,keys) == (forall|client:u64| registries[owner].contains_key(client)
        ==> registries[owner][client].drained(range)),
{
    if forall|client:u64| registries[owner].contains_key(client) ==> registries[owner][client].drained(range) {
        global_drain_from_source(c,s,registries,addresses,range,keys,owner);
    }
    if p::drained(s,keys) {
        assert forall|client:u64| registries[owner].contains_key(client) implies registries[owner][client].drained(range) by {
            let local = registries[owner][client];
            let id = registry_id(client,local);
            assert(session_observed(registries[owner],s,id,addresses,owner));
            assert forall|k:int| expansion::expanded(local,addresses).contains_key(k) implies
                !crate::bytes::contains_spec(range,addresses[k].0,addresses[k].1) by {
                assert(held_image(local,addresses).contains_key(k));
                assert(s.sessions[transaction(id)].held.contains_key(k));
            }
            expansion::native_drain_equivalence(local,addresses,range);
        }
    }
}

/// The second local registry can join an existing global transaction without
/// reopening it, even while a first owner still retains registrations.
pub proof fn second_owner_admission(c:p::Constants,s:p::State,id:TxnId,
    addresses:expansion::Addresses,owner:u32) -> (tracked out:ClosedTransfer)
    requires s.sessions.contains_key(transaction(id)),!s.sessions[transaction(id)].resolved,
        owned_holds(s.sessions[transaction(id)].held,owner).dom() == Set::<int>::empty(),
    ensures out.valid(c,s),out.after(s) == s,
        session_observed(Map::empty().insert(id.client,SessionView {
            sequence:id.sequence,terminal:false,holds:Seq::empty() }),out.after(s),id,addresses,owner),
{
    let before = Map::<u64,SessionView>::empty();
    let after = before.insert(id.client,SessionView { sequence:id.sequence,terminal:false,holds:Seq::empty() });
    assert(native::begin_effect(before,after,id,Status::Ok));
    native_begin(c,s,before,after,id,Status::Ok,addresses,owner)
}

proof fn two_owner_registry(id:TxnId,addresses:expansion::Addresses,owner:u32) -> (out:Map<u64,SessionView>)
    requires owner<2,addresses.dom()==Set::empty().insert(0int).insert(1int),addresses[0int]!=addresses[1int],
    ensures out==Map::<u64,SessionView>::empty().insert(id.client,SessionView {
        sequence:id.sequence,terminal:false,
        holds:seq![native::point_scope(addresses[owner as int].0,addresses[owner as int].1,Grant { owner,epoch:0 })] }),
        out[id.client].wf(),
        held_image(out[id.client],addresses)==Map::empty().insert(owner as int,p::Grant { owner:owner as int,epoch:0 }),
{
    let key=owner as int;
    let address=addresses[key];
    let grant=Grant { owner,epoch:0 };
    let before=Map::<u64,SessionView>::empty().insert(id.client,SessionView {
        sequence:id.sequence,terminal:false,holds:Seq::empty() });
    let out=before.insert(id.client,SessionView { sequence:id.sequence,terminal:false,
        holds:seq![native::point_scope(address.0,address.1,grant)] });
    assert(native::acquire_effect(before,out,id,address.0,address.1,grant,Status::Ok));
    expansion::acquire_accounting(before,out,id,address.0,address.1,grant,address.0,address.1,grant);
    expansion::expanded_exact(out[id.client],addresses,key,grant);
    assert forall|other:int| addresses.contains_key(other) && other!=key implies
        !out[id.client].scoped(addresses[other].0,addresses[other].1) by {
        assert(addresses[other]!=address);
        assert forall|i:int| 0<=i<out[id.client].holds.len() implies
            !out[id.client].holds[i].contains(addresses[other].0,addresses[other].1) by { assert(i==0); }
    }
    assert(held_image(out[id.client],addresses) =~= Map::empty().insert(key,p::Grant { owner:owner as int,epoch:0 }));
    out
}

proof fn two_owner_release(c:p::Constants,both:p::State,id:TxnId,
    addresses:expansion::Addresses,local0:Map<u64,SessionView>,local1:Map<u64,SessionView>,
    tracked execution:Execution) -> (done:p::State)
    requires p::constants_ok(c),execution.closed(c,both),
        both.sessions.dom()==Set::empty().insert(transaction(id)),!both.sessions[transaction(id)].resolved,
        both.sessions[transaction(id)].held==Map::empty().insert(0int,p::Grant { owner:0,epoch:0 }).insert(1int,p::Grant { owner:1,epoch:0 }),
        session_observed(local0,both,id,addresses,0),session_observed(local1,both,id,addresses,1),
        held_image(local0[id.client],addresses)==Map::empty().insert(0int,p::Grant { owner:0,epoch:0 }),
        held_image(local1[id.client],addresses)==Map::empty().insert(1int,p::Grant { owner:1,epoch:0 }),
    ensures done.sessions.contains_key(transaction(id)),done.sessions[transaction(id)].resolved,
        done.sessions[transaction(id)].held == Map::<int,p::Grant>::empty(),
        p::drained(done,Set::empty().insert(0int).insert(1int)),
{
    hide(p::inv);
    let t=transaction(id);
    let images = Map::empty().insert(0int,Map::empty()).insert(1int,Map::empty());
    let tracked (aborted,completion) = engine_aborted(c,both,id,EngineOutcome::Aborted,images,images);
    let resolved = aborted.after(both);
    let tracked execution = execution.append(c,both,aborted);
    let terminal0=local0.insert(id.client,SessionView { sequence:id.sequence,terminal:true,holds:Seq::empty() });
    let tracked finish0 = native_finish(c,resolved,local0,terminal0,id,Status::Ok,addresses,0,&completion);
    let one_left = finish0.after(resolved);
    let tracked execution = execution.append(c,resolved,finish0);
    assert(one_left.sessions[t].held.contains_key(1));
    assert(!p::drained(one_left,Set::empty().insert(1int)));
    assert(session_observed(local1,one_left,id,addresses,1));
    let terminal1=local1.insert(id.client,SessionView { sequence:id.sequence,terminal:true,holds:Seq::empty() });
    let tracked finish1 = native_finish(c,one_left,local1,terminal1,id,Status::Ok,addresses,1,&completion);
    let done = finish1.after(one_left);
    let tracked execution = execution.append(c,one_left,finish1);
    assert(done.sessions[t].held =~= Map::<int,p::Grant>::empty());
    execution.finite_history(c,done);
    done
}

proof fn singleton_set(key:int)
    ensures seq![key].to_set()==Set::empty().insert(key),
{
    let empty=Seq::<int>::empty();
    empty.lemma_push_to_set_commute(key);
    assert(empty.to_set() =~= Set::<int>::empty());
}

/// An enabled two-owner history. The first local finish leaves the second hold
/// present even after global engine resolution; the final finish drains it.
pub proof fn two_owner_lifecycle() -> (s:p::State)
    ensures s.sessions.contains_key(transaction(TxnId { client:7,sequence:1 })),
        s.sessions[transaction(TxnId { client:7,sequence:1 })].resolved,
        s.sessions[transaction(TxnId { client:7,sequence:1 })].held == Map::<int,p::Grant>::empty(),
        p::drained(s,Set::empty().insert(0int).insert(1int)),
{
    hide(p::inv);
    hide(held_image);
    hide(ClosedTransfer::valid);
    hide(ClosedTransfer::after);
    hide(Execution::closed);
    let c = p::Constants {
        keys:Set::empty().insert(0int).insert(1int),shards:Set::empty().insert(0int).insert(1int),
        table:Map::empty().insert(0int,7int).insert(1int,7int),
        coordinate:Map::empty().insert(0int,0int).insert(1int,1int),
        owners:Map::empty().insert(0int,0int).insert(1int,1int),
    };
    let addresses = Map::empty().insert(0int,(7u64,Seq::<u8>::empty())).insert(1int,(7u64,seq![0u8]));
    assert(addresses[0int].1.len()==0);
    assert(addresses[1int].1.len()==1);
    assert(addresses[0int].1!=addresses[1int].1);
    let id = TxnId { client:7,sequence:1 };
    let t = transaction(id);
    let empty = Map::<u64,SessionView>::empty();
    let opened = empty.insert(id.client,SessionView { sequence:1,terminal:false,holds:Seq::empty() });
    let local0 = two_owner_registry(id,addresses,0);
    let local1 = two_owner_registry(id,addresses,1);
    assert(p::constants_ok(c));
    let tracked execution = Execution::new(c);
    let initial = p::initial(c);
    let tracked begin = native_begin(c,initial,empty,opened,id,Status::Ok,addresses,0);
    let first = begin.after(initial);
    assert(first.sessions == Map::empty().insert(t,p::Session { held:Map::empty(),resolved:false }));
    singleton_set(0int);
    singleton_set(1int);
    let tracked execution = execution.append(c,initial,begin);
    let tracked acquire0 = lease_execution::acquire_keys(c,first,t,seq![0int],held_image(local0[id.client],addresses));
    let held0 = acquire0.after(first);
    assert(held0.sessions[t].held =~= Map::empty().insert(0int,p::Grant { owner:0,epoch:0 }));
    let tracked execution = execution.append(c,first,acquire0);
    assert(owned_holds(held0.sessions[t].held,1) =~= Map::<int,p::Grant>::empty());
    let tracked join1 = second_owner_admission(c,held0,id,addresses,1);
    let joined = join1.after(held0);
    let tracked execution = execution.append(c,held0,join1);
    let tracked acquire1 = lease_execution::acquire_keys(c,joined,t,seq![1int],held_image(local1[id.client],addresses));
    let both = acquire1.after(joined);
    let tracked execution = execution.append(c,joined,acquire1);
    assert(both.sessions.dom()==Set::empty().insert(t));
    assert(both.sessions[t].held =~= Map::empty().insert(0int,p::Grant { owner:0,epoch:0 }).insert(1int,p::Grant { owner:1,epoch:0 }));
    assert(owned_holds(both.sessions[t].held,0) =~= Map::empty().insert(0int,p::Grant { owner:0,epoch:0 }));
    assert(owned_holds(both.sessions[t].held,1) =~= Map::empty().insert(1int,p::Grant { owner:1,epoch:0 }));
    assert(session_observed(local0,both,id,addresses,0));
    assert(session_observed(local1,both,id,addresses,1));
    two_owner_release(c,both,id,addresses,local0,local1,execution)
}
} // verus!
