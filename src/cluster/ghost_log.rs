//! Erased raw-effect instrumentation for native sharding, following the TiKV
//! application-certificate and raft-rs/libp2p open-segment pattern.
//!
//! This module is proof infrastructure, not a production refinement claim.
//! A caller supplies its concrete view (including retained ghost provenance),
//! proves each observed native mutation equals its raw write, and proves an
//! independent placement path before closing. Intermediate writes need not
//! satisfy placement invariants. Neither replay nor close executes the model.
//! The range lock/storage boundary must justify the observation and exclusion;
//! an ambiguous engine result cannot be certified as rejection/stutter.
use vstd::prelude::*;
use crate::sharding_placement as p;
#[path = "ghost_log_proofs.rs"]
mod proofs;
pub use proofs::*;

verus! {

/// Entry/field effects, not desired placement actions. Entry insertion is useful
/// for initialization and immutable records; in-place replica/session changes
/// have individual field writes. Directory snapshots can only be appended.
pub enum Write {
    Logical { key: int, cell: p::Cell },
    Replica { owner: int, key: int, value: p::Replica },
    ReplicaCell { owner: int, key: int, cell: p::Cell },
    ReplicaRole { owner: int, key: int, role: p::Role },
    ReplicaEpoch { owner: int, key: int, epoch: nat },
    ReplicaFence { owner: int, key: int, fence: nat },
    ReplicaTerminal { owner: int, key: int, terminal: bool },
    ReplicaRound { owner: int, key: int, round: nat },
    ReplicaCovered { owner: int, key: int, covered: bool },
    Session { txn: int, value: p::Session },
    Held { txn: int, key: int, grant: p::Grant },
    Release { txn: int, key: int },
    Resolved { txn: int, resolved: bool },
    DirectoryAppend { snapshot: Map<int, p::Grant> },
    View { client: int, snapshot: nat },
    NextGeneration { generation: nat },
    Active { generation: Option<nat> },
    Plan { generation: nat, value: p::Plan },
    Phase { generation: nat, value: p::Phase },
    Command { generation: nat, value: p::Command },
    Certificate { generation: nat, value: p::Certificate },
    Received { generation: nat, value: p::Certificate },
    Packet { value: p::Packet },
    Outcome { nonce: int, value: p::Outcome },
    Reply { nonce: int, value: p::Outcome },
}

/// An address denotes one top-level entry. Nested field writes conservatively
/// own their containing entry; this avoids assuming independence of two fields
/// while one native operation replaces the entire entry.
pub enum Address {
    Logical(int), Replica(int, int), Session(int), Directory,
    View(int), NextGeneration, Active, Plan(nat), Phase(nat),
    Command(nat, p::Command), Certificate(nat, p::Certificate),
    Received(nat, p::Certificate), Packet(p::Packet), Outcome(int), Reply(int),
}
pub open spec fn address(w: Write) -> Address {
    match w {
        Write::Logical { key, .. } => Address::Logical(key),
        Write::Replica { owner, key, .. } | Write::ReplicaCell { owner, key, .. }
        | Write::ReplicaRole { owner, key, .. } | Write::ReplicaEpoch { owner, key, .. }
        | Write::ReplicaFence { owner, key, .. } | Write::ReplicaTerminal { owner, key, .. }
        | Write::ReplicaRound { owner, key, .. } | Write::ReplicaCovered { owner, key, .. }
            => Address::Replica(owner, key),
        Write::Session { txn, .. } | Write::Held { txn, .. }
        | Write::Release { txn, .. } | Write::Resolved { txn, .. } => Address::Session(txn),
        Write::DirectoryAppend { .. } => Address::Directory,
        Write::View { client, .. } => Address::View(client),
        Write::NextGeneration { .. } => Address::NextGeneration,
        Write::Active { .. } => Address::Active,
        Write::Plan { generation, .. } => Address::Plan(generation),
        Write::Phase { generation, .. } => Address::Phase(generation),
        Write::Command { generation, value } => Address::Command(generation, value),
        Write::Certificate { generation, value } => Address::Certificate(generation, value),
        Write::Received { generation, value } => Address::Received(generation, value),
        Write::Packet { value } => Address::Packet(value),
        Write::Outcome { nonce, .. } => Address::Outcome(nonce),
        Write::Reply { nonce, .. } => Address::Reply(nonce),
    }
}

pub open spec fn same_at(s: p::State, z: p::State, a: Address) -> bool {
    match a {
        Address::Logical(k) => s.logical.contains_key(k) == z.logical.contains_key(k)
            && (s.logical.contains_key(k) ==> s.logical[k] == z.logical[k]),
        Address::Replica(n,k) => s.physical.contains_key((n,k)) == z.physical.contains_key((n,k))
            && (s.physical.contains_key((n,k)) ==> s.physical[(n,k)] == z.physical[(n,k)]),
        Address::Session(t) => s.sessions.contains_key(t) == z.sessions.contains_key(t)
            && (s.sessions.contains_key(t) ==> s.sessions[t] == z.sessions[t]),
        Address::Directory => s.directory == z.directory,
        Address::View(k) => s.views.contains_key(k) == z.views.contains_key(k)
            && (s.views.contains_key(k) ==> s.views[k] == z.views[k]),
        Address::NextGeneration => s.next_generation == z.next_generation,
        Address::Active => s.active == z.active,
        Address::Plan(g) => s.plans.contains_key(g) == z.plans.contains_key(g)
            && (s.plans.contains_key(g) ==> s.plans[g] == z.plans[g]),
        Address::Phase(g) => s.phases.contains_key(g) == z.phases.contains_key(g)
            && (s.phases.contains_key(g) ==> s.phases[g] == z.phases[g]),
        Address::Command(g,v) => s.commands.contains((g,v)) == z.commands.contains((g,v)),
        Address::Certificate(g,v) => s.certificates.contains((g,v)) == z.certificates.contains((g,v)),
        Address::Received(g,v) => s.received.contains((g,v)) == z.received.contains((g,v)),
        Address::Packet(v) => s.packets.contains(v) == z.packets.contains(v),
        Address::Outcome(k) => s.outcomes.contains_key(k) == z.outcomes.contains_key(k)
            && (s.outcomes.contains_key(k) ==> s.outcomes[k] == z.outcomes[k]),
        Address::Reply(k) => s.replies.contains_key(k) == z.replies.contains_key(k)
            && (s.replies.contains_key(k) ==> s.replies[k] == z.replies[k]),
    }
}

/// Partial field updates require the entry to exist. Raw insertion remains an
/// insertion even when it would be illegal as a placement transition.
pub open spec fn writable(s: p::State, w: Write) -> bool {
    match w {
        Write::ReplicaCell { owner, key, .. } | Write::ReplicaRole { owner, key, .. }
        | Write::ReplicaEpoch { owner, key, .. } | Write::ReplicaFence { owner, key, .. }
        | Write::ReplicaTerminal { owner, key, .. } | Write::ReplicaRound { owner, key, .. }
        | Write::ReplicaCovered { owner, key, .. } => s.physical.contains_key((owner,key)),
        Write::Held { txn, .. } | Write::Release { txn, .. }
        | Write::Resolved { txn, .. } => s.sessions.contains_key(txn),
        _ => true,
    }
}

pub open spec fn apply_write(s: p::State, w: Write) -> p::State {
    match w {
        Write::Logical { key, cell } => p::State { logical: s.logical.insert(key,cell), ..s },
        Write::Replica { owner, key, value } => p::State { physical: s.physical.insert((owner,key),value), ..s },
        Write::ReplicaCell { owner, key, cell } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { cell, ..s.physical[(owner,key)] }), ..s },
        Write::ReplicaRole { owner, key, role } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { role, ..s.physical[(owner,key)] }), ..s },
        Write::ReplicaEpoch { owner, key, epoch } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { epoch, ..s.physical[(owner,key)] }), ..s },
        Write::ReplicaFence { owner, key, fence } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { fence, ..s.physical[(owner,key)] }), ..s },
        Write::ReplicaTerminal { owner, key, terminal } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { terminal, ..s.physical[(owner,key)] }), ..s },
        Write::ReplicaRound { owner, key, round } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { round, ..s.physical[(owner,key)] }), ..s },
        Write::ReplicaCovered { owner, key, covered } => p::State { physical: s.physical.insert((owner,key),
            p::Replica { covered, ..s.physical[(owner,key)] }), ..s },
        Write::Session { txn, value } => p::State { sessions: s.sessions.insert(txn,value), ..s },
        Write::Held { txn, key, grant } => p::State { sessions: s.sessions.insert(txn,
            p::Session { held: s.sessions[txn].held.insert(key,grant), ..s.sessions[txn] }), ..s },
        Write::Release { txn, key } => p::State { sessions: s.sessions.insert(txn,
            p::Session { held: s.sessions[txn].held.remove(key), ..s.sessions[txn] }), ..s },
        Write::Resolved { txn, resolved } => p::State { sessions: s.sessions.insert(txn,
            p::Session { resolved, ..s.sessions[txn] }), ..s },
        Write::DirectoryAppend { snapshot } => p::State { directory: s.directory.push(snapshot), ..s },
        Write::View { client, snapshot } => p::State { views: s.views.insert(client,snapshot), ..s },
        Write::NextGeneration { generation } => p::State { next_generation: generation, ..s },
        Write::Active { generation } => p::State { active: generation, ..s },
        Write::Plan { generation, value } => p::State { plans: s.plans.insert(generation,value), ..s },
        Write::Phase { generation, value } => p::State { phases: s.phases.insert(generation,value), ..s },
        Write::Command { generation, value } => p::State { commands: s.commands.insert((generation,value)), ..s },
        Write::Certificate { generation, value } => p::State { certificates: s.certificates.insert((generation,value)), ..s },
        Write::Received { generation, value } => p::State { received: s.received.insert((generation,value)), ..s },
        Write::Packet { value } => p::State { packets: s.packets.insert(value), ..s },
        Write::Outcome { nonce, value } => p::State { outcomes: s.outcomes.insert(nonce,value), ..s },
        Write::Reply { nonce, value } => p::State { replies: s.replies.insert(nonce,value), ..s },
    }
}

pub open spec fn apply_writes(s: p::State, writes: Seq<Write>) -> p::State
    decreases writes.len(),
{
    if writes.len() == 0 { s }
    else { apply_write(apply_writes(s,writes.drop_last()),writes.last()) }
}
pub open spec fn writes_ok(s: p::State, writes: Seq<Write>) -> bool
    decreases writes.len(),
{
    writes.len() == 0 || (writes_ok(s,writes.drop_last())
        && writable(apply_writes(s,writes.drop_last()),writes.last()))
}

/// Dispatch is used ONLY in certificates, never in raw replay. This includes
/// enabled local deliveries whose own fenced guard rejects them, as well as
/// globally disabled actions. A path records every step in a batched handler.
pub open spec fn path(c: p::Constants, states: Seq<p::State>, actions: Seq<p::Action>) -> bool {
    states.len() == actions.len() + 1
        && forall|i: int| 0 <= i < actions.len() ==>
            states[i+1] == p::dispatch(c,states[i],actions[i])
}
pub struct Segment {
    pub writes: Seq<Write>,
    pub states: Seq<p::State>,
    pub actions: Seq<p::Action>,
}
pub open spec fn certificate(c: p::Constants, before: p::State, segment: Segment) -> bool {
    writes_ok(before,segment.writes)
        && path(c,segment.states,segment.actions)
        && segment.states[0] == before
        && segment.states.last() == apply_writes(before,segment.writes)
}
pub open spec fn replay(initial: p::State, log: Seq<Segment>) -> p::State
    decreases log.len(),
{
    if log.len() == 0 { initial }
    else { apply_writes(replay(initial,log.drop_last()),log.last().writes) }
}
pub open spec fn log_ok(c: p::Constants, initial: p::State, log: Seq<Segment>) -> bool
    decreases log.len(),
{
    log.len() == 0 || (log_ok(c,initial,log.drop_last())
        && certificate(c,replay(initial,log.drop_last()),log.last()))
}
/// None is a closed boundary; Some(empty) is an open, not yet mutated segment.
pub struct Journal { pub closed: Seq<Segment>, pub pending: Option<Seq<Write>> }
pub open spec fn journal_state(initial: p::State, j: Journal) -> p::State {
    let before = replay(initial,j.closed);
    match j.pending { None => before, Some(writes) => apply_writes(before,writes) }
}
pub open spec fn mid(c: p::Constants, initial: p::State, j: Journal, actual: p::State) -> bool {
    log_ok(c,initial,j.closed) && actual == journal_state(initial,j)
        && match j.pending { None => true, Some(writes) => writes_ok(replay(initial,j.closed),writes) }
}
pub open spec fn closed(c: p::Constants, initial: p::State, j: Journal, actual: p::State) -> bool {
    mid(c,initial,j,actual) && j.pending is None
}
pub open spec fn coupled<C>(initial: p::State, j: Journal, native: C,
    view: spec_fn(C) -> p::State) -> bool { view(native) == journal_state(initial,j) }

/// Flatten the certified states, not raw intermediate states. A zero-action
/// segment contributes no transition and must have an exactly equal endpoint.
pub open spec fn behavior_states(initial: p::State, log: Seq<Segment>) -> Seq<p::State>
    decreases log.len(),
{
    if log.len() == 0 { seq![initial] }
    else { behavior_states(initial,log.drop_last()) + log.last().states.drop_first() }
}
pub open spec fn behavior_actions(log: Seq<Segment>) -> Seq<p::Action>
    decreases log.len(),
{
    if log.len() == 0 { Seq::empty() }
    else { behavior_actions(log.drop_last()) + log.last().actions }
}

} // verus!
