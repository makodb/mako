//! The new admission guard refines, rather than replaces, placement::admission.
use vstd::prelude::*;
use super::*;

verus! {

pub proof fn capture_preserves_pair(c: p::Constants, s: State, client: int, scope: Scope, representative: int)
    requires inv(c,s), can_capture(c,s,client,scope,representative)
    ensures route_matches(c,s.placement,captured(s,client,scope,representative)),
        captured(s,client,scope,representative).snapshot == Some(s.placement.views[client]),
        forall|k: int| keys(c,scope).contains(k) ==>
            s.placement.directory[s.placement.views[client] as int][k]
                == captured(s,client,scope,representative).grant
{}

/// Equality is an identity in ONE publication history, not a premise that two
/// unrelated snapshots with the same user-supplied number have equal contents.
pub proof fn same_publication_identity(s: p::State, a: nat, b: nat, key: int)
    requires a < s.directory.len(), b < s.directory.len(), a == b
    ensures s.directory[a as int] == s.directory[b as int],
        s.directory[a as int][key] == s.directory[b as int][key]
{}

pub proof fn publication_extends(c: p::Constants, s: p::State, a: p::Action)
    requires p::inv(c,s), p::enabled(c,s,a)
    ensures p::apply(c,s,a).directory.len() >= s.directory.len(),
        forall|v: int| 0 <= v < s.directory.len() ==>
            p::apply(c,s,a).directory[v] == s.directory[v],
        a is Commit ==> p::apply(c,s,a).directory.len() == s.directory.len() + 1,
        !(a is Commit) ==> p::apply(c,s,a).directory == s.directory
{
    p::lemma_snapshots_immutable(c,s,a);
    match a { p::Action::Commit => {}, _ => {} }
}

pub proof fn published_request_survives(c: p::Constants, s: p::State, a: p::Action, r: Request)
    requires p::inv(c,s), p::enabled(c,s,a), route_matches(c,s,r)
    ensures route_matches(c,p::apply(c,s,a),r)
{
    publication_extends(c,s,a);
    if r.snapshot is Some {
        let v = r.snapshot.unwrap();
        assert(p::apply(c,s,a).directory[v as int] == s.directory[v as int]);
    }
}

pub proof fn fast_establishes_exact(c: p::Constants, s: State, r: Request)
    requires inv(c,s), route_matches(c,s.placement,r), fast(s,r)
    ensures exact(c,s.placement,r)
{
    let v = r.snapshot.unwrap();
    assert(certifiable(c,s.placement,r.grant.owner,v));
    assert forall|k: int| keys(c,r.scope).contains(k) implies
        s.placement.physical.contains_key((r.grant.owner,k))
            && p::replica(s.placement,r.grant.owner,k).role is Serving
            && p::replica(s.placement,r.grant.owner,k).ownership_version == r.grant.ownership_version by {
        assert(c.keys.contains(k));
        same_publication_identity(s.placement,v,s.certified[r.grant.owner],k);
        assert(s.placement.directory[v as int][k] == r.grant);
    }
}

/// Covers point access AND every modeled key of the full requested scan scope.
pub proof fn range_authority(c: p::Constants, s: State, id: int)
    requires inv(c,s), s.requests.contains_key(id), range_authorized(c,s,s.requests[id])
    ensures exact(c,s.placement,s.requests[id]),
        forall|k: int| keys(c,s.requests[id].scope).contains(k) ==>
            s.requests[id].grant == p::directory(s.placement)[k]
{
    let r = s.requests[id];
    assert(route_matches(c,s.placement,r));
    if fast(s,r) { fast_establishes_exact(c,s,r); }
    assert forall|k: int| keys(c,r.scope).contains(k) implies
        r.grant == p::directory(s.placement)[k] by {
        assert(c.keys.contains(k));
        assert(s.placement.physical.contains_key((r.grant.owner,k)));
        assert(c.shards.contains(r.grant.owner));
        assert(p::key_inv(c,s.placement,k));
    }
}

/// Existing session/terminal checks are unconditional. Only the local range
/// metadata work is elided; successful admission still has EXACTLY its old guard.
pub proof fn admission_refines_placement(c: p::Constants, s: State, id: int, txn: int, key: int)
    requires inv(c,s), can_admit(c,s,id,txn,key)
    ensures p::admission(s.placement,txn,key,s.requests[id].grant),
        s.requests[id].grant == p::directory(s.placement)[key],
        p::replica(s.placement,s.requests[id].grant.owner,key).role is Serving,
        p::replica(s.placement,s.requests[id].grant.owner,key).ownership_version
            == s.requests[id].grant.ownership_version
{
    range_authority(c,s,id);
}

pub proof fn fallback_is_exact(c: p::Constants, s: State, r: Request)
    requires !fast(s,r)
    ensures range_authorized(c,s,r) == exact(c,s.placement,r),
        r.snapshot is None ==> !fast(s,r)
{}

pub proof fn untrusted_never_fast(c: p::Constants, s: State, id: int, scope: Scope, grant: p::Grant)
    ensures !fast(apply(c,s,Action::Untrusted { id,scope,grant }),
        apply(c,s,Action::Untrusted { id,scope,grant }).requests[id])
{}

/// Every non-authority transition may modify bytes, sessions, packet delivery,
/// or publication, but not the role/incarnation against which we certified.
pub proof fn authority_frame(c: p::Constants, s: p::State, a: p::Action, owner: int, key: int)
    requires p::inv(c,s), p::enabled(c,s,a), !changes_authority(a)
    ensures p::apply(c,s,a).physical.contains_key((owner,key)) == s.physical.contains_key((owner,key)),
        s.physical.contains_key((owner,key)) ==>
            p::replica(p::apply(c,s,a),owner,key).role == p::replica(s,owner,key).role
            && p::replica(p::apply(c,s,a),owner,key).ownership_version == p::replica(s,owner,key).ownership_version
{
    match a {
        p::Action::Resolve { txn,writes } => {},
        p::Action::DeliverCopy { packet } => {
            assert(p::packet_inv(s,packet));
            assert(p::plan_inv(c,s,packet.generation));
            assert(c.keys.contains(packet.key));
            assert(c.shards.contains(s.plans[packet.generation].dst));
            assert(s.physical.contains_key((s.plans[packet.generation].dst,packet.key)));
        },
        _ => {},
    }
}

pub proof fn placement_preserves_certificate(c: p::Constants, s: State, a: p::Action)
    requires inv(c,s), p::enabled(c,s.placement,a)
    ensures forall|owner: int| placement_step(c,s,a).certified.contains_key(owner) ==>
        certifiable(c,placement_step(c,s,a).placement,owner,placement_step(c,s,a).certified[owner])
{
    let z = placement_step(c,s,a);
    publication_extends(c,s.placement,a);
    if !changes_authority(a) {
        assert forall|owner: int| z.certified.contains_key(owner) implies
            certifiable(c,z.placement,owner,z.certified[owner]) by {
            let v = s.certified[owner];
            assert(certifiable(c,s.placement,owner,v));
            assert(z.placement.directory[v as int] == s.placement.directory[v as int]);
            assert forall|k: int| c.keys.contains(k) && z.placement.directory[v as int][k].owner == owner implies {
                let replica = p::replica(z.placement,owner,k);
                z.placement.physical.contains_key((owner,k)) && replica.role is Serving
                    && replica.ownership_version == z.placement.directory[v as int][k].ownership_version
            } by {
                authority_frame(c,s.placement,a,owner,k);
            }
        }
    }
}

pub proof fn certify_preserves_authority(c: p::Constants, s: State, owner: int, snapshot: nat)
    requires inv(c,s), enabled(c,s,Action::Certify { owner,snapshot })
    ensures inv(c,apply(c,s,Action::Certify { owner,snapshot })),
        apply(c,s,Action::Certify { owner,snapshot }).certified.contains_key(owner) ==>
            certifiable(c,s.placement,owner,snapshot)
{
    let z = apply(c,s,Action::Certify { owner,snapshot });
    assert forall|n: int| z.certified.contains_key(n) implies
        certifiable(c,z.placement,n,z.certified[n]) by {
        if n != owner { assert(s.certified.contains_key(n)); }
    }
}

pub proof fn placement_step_preserves(c: p::Constants, s: State, a: p::Action)
    requires inv(c,s), p::enabled(c,s.placement,a)
    ensures inv(c,placement_step(c,s,a))
{
    p::lemma_step(c,s.placement,a);
    placement_preserves_certificate(c,s,a);
    assert forall|id: int| s.requests.contains_key(id) implies
        route_matches(c,p::apply(c,s.placement,a),s.requests[id]) by {
        published_request_survives(c,s.placement,a,s.requests[id]);
    }
}

pub proof fn lemma_init(c: p::Constants)
    requires p::constants_ok(c)
    ensures inv(c,initial(c))
{ p::lemma_init(c); }

pub proof fn lemma_step(c: p::Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a)
    ensures inv(c,apply(c,s,a))
{
    match a {
        Action::Placement { action } => { placement_step_preserves(c,s,action); },
        Action::Capture { id,client,scope,representative } => {
            capture_preserves_pair(c,s,client,scope,representative);
        },
        Action::Untrusted { .. } => {},
        Action::Certify { owner,snapshot } => { certify_preserves_authority(c,s,owner,snapshot); },
        Action::Invalidate => {},
        Action::Admit { id,txn,key } => {
            admission_refines_placement(c,s,id,txn,key);
            placement_step_preserves(c,s,p::Action::Acquire { txn,key,grant: s.requests[id].grant });
        },
    }
}

/// Erasing capture/certification steps yields the old placement transition
/// system. In particular no migration, drain, or held-lease invariant is changed.
pub proof fn step_refines_placement(c: p::Constants, s: State, a: Action)
    requires inv(c,s), enabled(c,s,a)
    ensures p::next(c,s.placement,apply(c,s,a).placement)
{
    let old = match a {
        Action::Placement { action } => action,
        Action::Admit { id,txn,key } => {
            admission_refines_placement(c,s,id,txn,key);
            p::Action::Acquire { txn,key,grant: s.requests[id].grant }
        },
        _ => p::Action::Stutter,
    };
    assert(p::enabled(c,s.placement,old));
    assert(apply(c,s,a).placement == p::dispatch(c,s.placement,old));
}

pub proof fn theorem_invariant(c: p::Constants, states: Seq<State>, actions: Seq<Action>, i: int)
    requires p::constants_ok(c), behavior(c,states,actions), 0 <= i < states.len()
    ensures inv(c,states[i])
    decreases i
{
    if i == 0 { lemma_init(c); }
    else {
        theorem_invariant(c,states,actions,i-1);
        lemma_step(c,states[i-1],actions[i-1]);
    }
}

pub proof fn admitted_lease_preserves_safety(c: p::Constants, s: State, id: int, txn: int, key: int)
    requires inv(c,s), can_admit(c,s,id,txn,key)
    ensures p::inv(c,apply(c,s,Action::Admit { id,txn,key }).placement),
        p::live_lease(apply(c,s,Action::Admit { id,txn,key }).placement,txn,key),
        p::read(apply(c,s,Action::Admit { id,txn,key }).placement,txn,key)
            == Some(s.placement.logical[key])
{
    lemma_step(c,s,Action::Admit { id,txn,key });
    let z = apply(c,s,Action::Admit { id,txn,key }).placement;
    p::lemma_read_matches_logical(c,z,txn,key);
}

pub proof fn frozen_rejects_matching_snapshot(c: p::Constants, s: State, id: int, txn: int, key: int)
    requires inv(c,s), s.requests.contains_key(id), keys(c,s.requests[id].scope).contains(key),
        p::replica(s.placement,s.requests[id].grant.owner,key).role is Frozen
    ensures !fast(s,s.requests[id]), !can_admit(c,s,id,txn,key)
{
    let r = s.requests[id];
    if fast(s,r) { fast_establishes_exact(c,s,r); }
}

/// A delivered freeze with outstanding leases still cannot drain; the new
/// certificate is irrelevant to physical lease lifetime and completion rules.
pub proof fn held_lease_still_blocks_drain(c: p::Constants, s: State, generation: nat, txn: int, key: int)
    requires inv(c,s), s.placement.plans.contains_key(generation),
        s.placement.plans[generation].keys.contains(key), s.placement.sessions.contains_key(txn),
        s.placement.sessions[txn].held.contains_key(key)
    ensures !enabled(c,s,Action::Placement { action: p::Action::Drain { generation } })
{ p::lemma_exact_drain_observes_every_session(c,s.placement,generation,txn,key); }

/// Returning to the same physical owner does not revive an old incarnation.
/// The conclusion covers either branch and does not require a version mismatch.
pub proof fn return_migration_rejects_old_grant(c: p::Constants,s: State,id: int,txn: int,key: int)
    requires inv(c,s), s.requests.contains_key(id), keys(c,s.requests[id].scope).contains(key),
        s.requests[id].grant.owner == p::directory(s.placement)[key].owner,
        s.requests[id].grant.ownership_version != p::directory(s.placement)[key].ownership_version
    ensures !can_admit(c,s,id,txn,key)
{
    if can_admit(c,s,id,txn,key) { admission_refines_placement(c,s,id,txn,key); }
}

pub proof fn scan_covers_every_selected_key(c: p::Constants,s: State,id: int,table: int,lo: int,hi: Option<int>)
    requires inv(c,s), s.requests.contains_key(id),
        s.requests[id].scope == (Scope::Scan { table,lo,hi }),
        range_authorized(c,s,s.requests[id])
    ensures forall|k: int| c.keys.contains(k) && c.table[k] == table
        && partition::in_range(c.coordinate[k],lo,hi) ==>
            p::replica(s.placement,s.requests[id].grant.owner,k).role is Serving
                && p::replica(s.placement,s.requests[id].grant.owner,k).ownership_version
                    == s.requests[id].grant.ownership_version
{
    range_authority(c,s,id);
    assert forall|k: int| c.keys.contains(k) && c.table[k] == table
        && partition::in_range(c.coordinate[k],lo,hi) implies
            p::replica(s.placement,s.requests[id].grant.owner,k).role is Serving
                && p::replica(s.placement,s.requests[id].grant.owner,k).ownership_version
                    == s.requests[id].grant.ownership_version by {
        assert(keys(c,s.requests[id].scope).contains(k));
    }
}

/// Queued transport requests are immutable through publication, authority
/// changes, completion, retries, and admission. Capture never overwrites an ID.
pub proof fn retry_preserves_pair(c: p::Constants, s: State, a: Action, id: int)
    requires enabled(c,s,a), s.requests.contains_key(id)
    ensures apply(c,s,a).requests.contains_key(id), apply(c,s,a).requests[id] == s.requests[id]
{ match a { Action::Capture { .. } => {}, Action::Untrusted { .. } => {}, _ => {} } }

} // verus!
