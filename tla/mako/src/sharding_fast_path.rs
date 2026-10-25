//! Snapshot admission as an extension of the existing placement state machine.
//! Publication versions are indices in placement::State::directory: Commit is
//! the only append, and old entries are immutable. This is the corrected atomic
//! publication model, not the deliberately torn legacy publication experiment.
//!
//! Capture is the genuine client/transport boundary: it reads one cached
//! immutable publication and transports the resulting scope/grant/version tuple
//! unchanged, including retries. An arbitrary foreign request enters Untrusted,
//! which has no snapshot provenance and always uses exact local validation.
//! Neither a caller-supplied scalar nor a well-formed grant authenticates origin.
//! The finite-key projection covers EVERY modeled key in a scan interval; native
//! full-range capture must additionally establish continuous segment coverage.
use vstd::prelude::*;
use crate::sharding_partition as partition;
use crate::sharding_placement as p;
#[path = "sharding_fast_path_proofs.rs"]
pub mod proofs;
#[path = "sharding_fast_path_witnesses.rs"]
pub mod witnesses;

verus! {

pub enum Scope { Point { key: int }, Scan { table: int, lo: int, hi: Option<int> } }
pub struct Request { pub scope: Scope, pub grant: p::Grant, pub snapshot: Option<nat> }
pub struct State {
    pub placement: p::State,
    pub requests: Map<int, Request>,
    /// Volatile derived authority, never recovered from durable metadata.
    pub certified: Map<int, nat>,
}
pub enum Action {
    Placement { action: p::Action },
    Capture { id: int, client: int, scope: Scope, representative: int },
    Untrusted { id: int, scope: Scope, grant: p::Grant },
    Certify { owner: int, snapshot: nat },
    Invalidate,
    Admit { id: int, txn: int, key: int },
}
pub open spec fn keys(c: p::Constants, scope: Scope) -> Set<int> {
    match scope {
        Scope::Point { key } => Set::empty().insert(key),
        Scope::Scan { table, lo, hi } => p::selected(c,table,lo,hi),
    }
}
pub open spec fn scope_ok(c: p::Constants, scope: Scope) -> bool {
    keys(c,scope).subset_of(c.keys) && keys(c,scope) != Set::<int>::empty()
        && match scope { Scope::Point { .. } => true,
            Scope::Scan { lo,hi,.. } => partition::valid_range(lo,hi) }
}
pub open spec fn route_matches(c: p::Constants, s: p::State, r: Request) -> bool {
    scope_ok(c,r.scope) && match r.snapshot {
        None => true,
        Some(v) => v < s.directory.len()
            && forall|k: int| keys(c,r.scope).contains(k) ==> s.directory[v as int][k] == r.grant,
    }
}
/// This is the actual receiver check, not an assumed authority fact. Every
/// published route assigned to this receiver must match its local metadata.
pub open spec fn certifiable(c: p::Constants, s: p::State, owner: int, v: nat) -> bool {
    v < s.directory.len() && c.shards.contains(owner)
        && forall|k: int| c.keys.contains(k) && s.directory[v as int][k].owner == owner ==> {
            let replica = p::replica(s,owner,k);
            s.physical.contains_key((owner,k))
                && replica.role is Serving
                && replica.ownership_version == s.directory[v as int][k].ownership_version
        }
}
pub open spec fn inv(c: p::Constants, s: State) -> bool {
    &&& p::inv(c,s.placement)
    &&& forall|id: int| s.requests.contains_key(id) ==> route_matches(c,s.placement,s.requests[id])
    &&& forall|owner: int| s.certified.contains_key(owner) ==>
        certifiable(c,s.placement,owner,s.certified[owner])
}
pub open spec fn initial(c: p::Constants) -> State {
    State { placement: p::initial(c), requests: Map::empty(), certified: Map::empty() }
}
pub open spec fn captured(s: State, client: int, scope: Scope, representative: int) -> Request {
    let version = s.placement.views[client];
    Request { scope, grant: s.placement.directory[version as int][representative], snapshot: Some(version) }
}
pub open spec fn can_capture(c: p::Constants, s: State, client: int, scope: Scope, representative: int) -> bool {
    scope_ok(c,scope) && s.placement.views.contains_key(client)
        && s.placement.views[client] < s.placement.directory.len()
        && keys(c,scope).contains(representative)
        && forall|k: int| keys(c,scope).contains(k) ==>
            s.placement.directory[s.placement.views[client] as int][k]
                == s.placement.directory[s.placement.views[client] as int][representative]
}
/// The hot branch deliberately does not look at range-local metadata.
pub open spec fn fast(s: State, r: Request) -> bool {
    r.snapshot is Some && s.certified.contains_key(r.grant.owner)
        && s.certified[r.grant.owner] == r.snapshot.unwrap()
}
pub open spec fn exact(c: p::Constants, s: p::State, r: Request) -> bool {
    forall|k: int| keys(c,r.scope).contains(k) ==>
        s.physical.contains_key((r.grant.owner,k))
            && p::replica(s,r.grant.owner,k).role is Serving
            && p::replica(s,r.grant.owner,k).ownership_version == r.grant.ownership_version
}
pub open spec fn range_authorized(c: p::Constants, s: State, r: Request) -> bool {
    if fast(s,r) { true } else { exact(c,s.placement,r) }
}
pub open spec fn can_admit(c: p::Constants, s: State, id: int, txn: int, key: int) -> bool {
    s.requests.contains_key(id) && keys(c,s.requests[id].scope).contains(key)
        && s.placement.sessions.contains_key(txn) && !s.placement.sessions[txn].resolved
        && !s.placement.sessions[txn].held.contains_key(key)
        && range_authorized(c,s,s.requests[id])
}
/// Deliver covers freeze, retirement, commit, abort, and all replayed controls;
/// Seal also changes a local role. Invalidate BEFORE executing those mutations.
/// This conservative model clears every receiver's certificate together.
pub open spec fn changes_authority(a: p::Action) -> bool { a is Deliver || a is Seal }
pub open spec fn placement_step(c: p::Constants, s: State, a: p::Action) -> State {
    let certified = if changes_authority(a) { Map::empty() } else { s.certified };
    State { placement: p::apply(c,s.placement,a), certified, ..s }
}
pub open spec fn enabled(c: p::Constants, s: State, a: Action) -> bool {
    match a {
        Action::Placement { action } => p::enabled(c,s.placement,action),
        Action::Capture { id,client,scope,representative } => !s.requests.contains_key(id)
            && can_capture(c,s,client,scope,representative),
        Action::Untrusted { id,scope,.. } => !s.requests.contains_key(id) && scope_ok(c,scope),
        Action::Certify { owner,snapshot } => snapshot < s.placement.directory.len() && c.shards.contains(owner),
        Action::Invalidate => true,
        Action::Admit { id,txn,key } => can_admit(c,s,id,txn,key),
    }
}
pub open spec fn apply(c: p::Constants, s: State, a: Action) -> State {
    match a {
        Action::Placement { action } => placement_step(c,s,action),
        Action::Capture { id,client,scope,representative } => State {
            requests: s.requests.insert(id,captured(s,client,scope,representative)), ..s },
        Action::Untrusted { id,scope,grant } => State {
            requests: s.requests.insert(id,Request { scope,grant,snapshot: None }), ..s },
        Action::Certify { owner,snapshot } => State {
            certified: if certifiable(c,s.placement,owner,snapshot) {
                s.certified.insert(owner,snapshot)
            } else { s.certified.remove(owner) }, ..s },
        Action::Invalidate => State { certified: Map::empty(), ..s },
        Action::Admit { id,txn,key } => placement_step(c,s,
            p::Action::Acquire { txn,key,grant: s.requests[id].grant }),
    }
}
pub open spec fn behavior(c: p::Constants, states: Seq<State>, actions: Seq<Action>) -> bool {
    states.len() == actions.len() + 1 && states[0] == initial(c)
        && forall|i: int| 0 <= i < actions.len() ==>
            enabled(c,states[i],actions[i]) && states[i+1] == apply(c,states[i],actions[i])
}

} // verus!
