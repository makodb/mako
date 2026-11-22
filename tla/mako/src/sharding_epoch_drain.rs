//! Epoch-drained handoff with optional private asynchronous precopy, composed
//! with immutable epoch admission. Only joined, drained exact final copy can
//! establish the Copied/Ready phase; precopy never establishes authority.
//! Images cover one arbitrary nonempty ownership scope, including every physical
//! alias and absent coordinate; no independent per-row executions are chosen.
//! EngineCommit is the ordinary OCC/distributed atomic-settlement interface:
//! it validates one full-scope snapshot and atomically installs its replacement.
//! This conservative engine interface is NOT a native STO/2PC refinement proof.
//! Publication/announcement memory ordering, crash recovery, and engine opacity
//! remain separate obligations. No epoch-equality check occurs at settlement.
//! `local` supports the independent-process specialization. Colocated owners
//! must share physical epoch labels and establish projection::shared_domain;
//! its physical-cut theorem supplies owner-local Complete, not independent
//! per-owner clocks. The relation is an explicit native correspondence premise.
use vstd::prelude::*;
use vstd::imap::IMap;
use vstd::iset::ISet;
use crate::sharding_epoch_admission as admission;
pub use admission::{Grant, Key};
#[path = "sharding_epoch_drain_proofs.rs"]
pub mod proofs;
#[path = "sharding_epoch_drain_witnesses.rs"]
pub mod witnesses;
#[path = "sharding_epoch_drain_precopy.rs"]
pub mod precopy;
#[path = "sharding_epoch_drain_projection.rs"]
pub mod projection;

verus! {

pub type Image = IMap<Key,Option<int>>;
pub struct Constants {
    pub nodes: Set<int>,
    pub scope: ISet<Key>,
    pub anchor: Key,
    pub initial: Grant,
    pub image: Image,
}
pub enum Outcome { Pending, Committed, Aborted }
pub struct Txn {
    pub grant: Grant,
    pub captured: nat,
    pub observed: Image,
    pub replacement: Image,
    pub writes: bool,
    pub outcome: Outcome,
    pub cleaned: bool,
}
pub enum Phase { Prepared, Frozen, Copied, Published, Active }
/// A row observation is immutable, but need not be current when delivered.
pub struct Packet {
    pub generation: nat,
    pub round: nat,
    pub destination: int,
    pub key: Key,
    pub value: Option<int>,
}
pub struct Move {
    pub source: Grant,
    pub destination: int,
    pub generation: nat,
    pub phase: Phase,
    pub frozen_index: Option<nat>,
    pub cut: Option<nat>,
    pub round: nat,
    pub precopy_open: bool,
    pub precopy_joined: bool,
}
pub struct State {
    pub route: Grant,
    pub local: Map<int,admission::State>,
    pub data: Map<int,Image>,
    pub logical: Image,
    pub txns: Map<int,Txn>,
    pub movement: Option<Move>,
    pub next_generation: nat,
    pub packets: Map<int,Packet>,
    pub effects: Set<int>,
}
pub enum Action {
    Local { node: int, action: admission::Action },
    Enroll { node: int, id: int, grant: Grant, writes: bool, replacement: Image },
    EngineCommit { id: int },
    EngineAbort { id: int },
    Cleanup { id: int },
    Prepare { destination: int, generation: nat },
    StartPrecopy { generation: nat },
    Observe { generation: nat, id: int, key: Key },
    AcceptPrecopy { id: int },
    FinishPrecopy { id: int },
    ClosePrecopy { generation: nat },
    JoinPrecopy { generation: nat },
    Freeze { generation: nat },
    EstablishCut { generation: nat },
    Drain { generation: nat },
    Copy { generation: nat },
    Publish { generation: nat },
    Activate { generation: nat },
    Reclaim { generation: nat },
    Cancel { generation: nat },
    Stutter,
}
pub open spec fn constants(c: Constants) -> bool {
    c.nodes.contains(c.initial.owner) && c.scope.contains(c.anchor)
        && c.image.dom() == c.scope
}
pub open spec fn blank(c: Constants) -> Image {
    IMap::new(|k: Key| c.scope.contains(k), |k: Key| None)
}
pub open spec fn serving(c: Constants, grant: Grant) -> admission::View {
    IMap::new(|k: Key| c.scope.contains(k), |k: Key| grant)
}
pub open spec fn initial(c: Constants) -> State {
    State { route: c.initial,
        local: Map::new(c.nodes, |n: int| admission::initial(
            if n == c.initial.owner { serving(c,c.initial) } else { IMap::empty() })),
        data: Map::new(c.nodes, |n: int| if n == c.initial.owner { c.image } else { blank(c) }),
        logical: c.image, txns: Map::empty(), movement: None,
        next_generation: c.initial.version + 1, packets: Map::empty(), effects: Set::empty() }
}
pub open spec fn target(m: Move) -> Grant {
    Grant { owner: m.destination, version: m.generation }
}
pub open spec fn current_move(s: State, generation: nat) -> bool {
    s.movement is Some && s.movement.unwrap().generation == generation
}
pub open spec fn before_publication(m: Move) -> bool {
    m.phase is Prepared || m.phase is Frozen || m.phase is Copied
}
pub open spec fn copied(m: Move) -> bool {
    m.phase is Copied || m.phase is Published || m.phase is Active
}
pub open spec fn pending(s: State, id: int) -> bool {
    s.txns.contains_key(id) && s.txns[id].outcome is Pending
}
pub open spec fn local_exit_ready(s: State, node: int, id: int) -> bool {
    !s.txns.contains_key(id) || s.txns[id].grant.owner != node || s.txns[id].cleaned
}
/// The certificate is over the entire immutable scope, not returned rows or its
/// first key. Empty/non-target read sections use Local without Enroll.
pub open spec fn scope_authorized(c: Constants, s: State, node: int, id: int, grant: Grant) -> bool {
    forall|k: Key| #[trigger] c.scope.contains(k) ==> admission::authorized(s.local[node],id,k,grant)
}
pub open spec fn local_allowed(s: State, node: int, action: admission::Action) -> bool {
    match action {
        admission::Action::Publish { .. } => false,
        admission::Action::Exit { id } => local_exit_ready(s,node,id),
        admission::Action::Retry { id } => !s.txns.contains_key(id)
            || s.txns[id].grant.owner != node || pending(s,id),
        _ => true,
    }
}
pub open spec fn packet_current(s: State, p: Packet) -> bool {
    s.movement is Some && p.generation == s.movement.unwrap().generation
        && p.round == s.movement.unwrap().round
        && p.destination == s.movement.unwrap().destination
}
pub open spec fn precopy_phase(m: Move) -> bool {
    m.phase is Prepared || m.phase is Frozen
}
pub open spec fn enabled(c: Constants, s: State, a: Action) -> bool {
    match a {
        Action::Local { node,action } => c.nodes.contains(node)
            && admission::enabled(s.local[node],action) && local_allowed(s,node,action),
        Action::Enroll { node,id,grant,replacement,.. } => c.nodes.contains(node)
            && node == grant.owner && !s.txns.contains_key(id)
            && replacement.dom() == c.scope
            && scope_authorized(c,s,node,id,grant),
        // Captured authority is retained by the invariant, NOT rechecked here.
        // In particular an erroneous early Exit must not silently disable commit.
        Action::EngineCommit { id } => pending(s,id)
            && s.data[s.txns[id].grant.owner] == s.txns[id].observed,
        Action::EngineAbort { id } => pending(s,id),
        Action::Cleanup { id } => s.txns.contains_key(id)
            && !(s.txns[id].outcome is Pending) && !s.txns[id].cleaned,
        Action::Prepare { destination,generation } => s.movement is None
            && c.nodes.contains(destination) && destination != s.route.owner
            && generation >= s.next_generation,
        Action::StartPrecopy { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Prepared && s.movement.unwrap().precopy_joined,
        Action::Observe { generation,id,key } => current_move(s,generation)
            && s.movement.unwrap().precopy_open && precopy_phase(s.movement.unwrap())
            && !s.packets.contains_key(id) && c.scope.contains(key),
        Action::AcceptPrecopy { id } => s.packets.contains_key(id)
            && packet_current(s,s.packets[id])
            && s.movement.unwrap().precopy_open && precopy_phase(s.movement.unwrap())
            && !s.effects.contains(id),
        Action::FinishPrecopy { id } => s.effects.contains(id),
        Action::ClosePrecopy { generation } => current_move(s,generation)
            && s.movement.unwrap().precopy_open,
        Action::JoinPrecopy { generation } => current_move(s,generation)
            && !s.movement.unwrap().precopy_open && s.effects.is_empty(),
        Action::Freeze { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Prepared,
        Action::EstablishCut { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Frozen && s.movement.unwrap().cut is None,
        Action::Drain { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Frozen && s.movement.unwrap().cut is Some
            && admission::enabled(s.local[s.movement.unwrap().source.owner],
                admission::Action::Complete { cut: s.movement.unwrap().cut.unwrap() }),
        Action::Copy { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Frozen && s.movement.unwrap().cut is Some
            && s.movement.unwrap().precopy_joined
            && s.local[s.movement.unwrap().source.owner].completed >= s.movement.unwrap().cut.unwrap(),
        Action::Publish { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Copied,
        Action::Activate { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Published,
        Action::Reclaim { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Active,
        Action::Cancel { generation } => current_move(s,generation)
            && before_publication(s.movement.unwrap()) && s.movement.unwrap().precopy_joined,
        Action::Stutter => true,
    }
}
pub open spec fn local_step(s: State, node: int, action: admission::Action) -> State {
    State { local: s.local.insert(node,admission::apply(s.local[node],action)), ..s }
}
pub open spec fn set_phase(s: State, phase: Phase) -> State {
    State { movement: Some(Move { phase, ..s.movement.unwrap() }), ..s }
}
pub open spec fn apply(c: Constants, s: State, a: Action) -> State {
    match a {
        Action::Local { node,action } => {
            let out = local_step(s,node,action);
            match action {
                admission::Action::Retry { id } => if s.txns.contains_key(id)
                    && s.txns[id].grant.owner == node {
                    State { txns: s.txns.insert(id,Txn { observed: s.data[node], ..s.txns[id] }), ..out }
                } else { out },
                _ => out,
            }
        },
        Action::Enroll { node,id,grant,writes,replacement } => State {
            txns: s.txns.insert(id,Txn { grant,
                captured: s.local[node].readers[id].view.unwrap(), observed: s.data[node],
                replacement, writes, outcome: Outcome::Pending, cleaned: false }), ..s },
        Action::EngineCommit { id } => {
            let t = s.txns[id];
            State { txns: s.txns.insert(id,Txn { outcome: Outcome::Committed, ..t }),
                data: if t.writes { s.data.insert(t.grant.owner,t.replacement) } else { s.data },
                logical: if t.writes { t.replacement } else { s.logical }, ..s }
        },
        Action::EngineAbort { id } => State {
            txns: s.txns.insert(id,Txn { outcome: Outcome::Aborted, ..s.txns[id] }), ..s },
        Action::Cleanup { id } => State {
            txns: s.txns.insert(id,Txn { cleaned: true, ..s.txns[id] }), ..s },
        Action::Prepare { destination,generation } => State {
            movement: Some(Move { source: s.route, destination, generation,
                phase: Phase::Prepared, frozen_index: None, cut: None,
                round: 0, precopy_open: false, precopy_joined: true }),
            next_generation: generation + 1, ..s },
        Action::StartPrecopy { .. } => State {
            movement: Some(Move { round: s.movement.unwrap().round + 1,
                precopy_open: true, precopy_joined: false, ..s.movement.unwrap() }), ..s },
        Action::Observe { id,key,.. } => {
            let m = s.movement.unwrap();
            State { packets: s.packets.insert(id,Packet { generation: m.generation,
                round: m.round, destination: m.destination, key,
                value: s.data[m.source.owner][key] }), ..s }
        },
        Action::AcceptPrecopy { id } => State { effects: s.effects.insert(id), ..s },
        Action::FinishPrecopy { id } => {
            let p = s.packets[id];
            State { effects: s.effects.remove(id),
                data: s.data.insert(p.destination,s.data[p.destination].insert(p.key,p.value)), ..s }
        },
        Action::ClosePrecopy { .. } => State {
            movement: Some(Move { precopy_open: false, ..s.movement.unwrap() }), ..s },
        Action::JoinPrecopy { .. } => State {
            movement: Some(Move { precopy_joined: true, ..s.movement.unwrap() }), ..s },
        Action::Freeze { .. } => {
            let m = s.movement.unwrap();
            State { movement: Some(Move { phase: Phase::Frozen,
                frozen_index: Some(s.local[m.source.owner].history.len()), ..m }),
                ..local_step(s,m.source.owner,admission::Action::Publish { view: IMap::empty() }) }
        },
        Action::EstablishCut { .. } => {
            let m = s.movement.unwrap();
            State { movement: Some(Move { cut: Some(s.local[m.source.owner].epoch + 1), ..m }),
                ..local_step(s,m.source.owner,admission::Action::Tick) }
        },
        Action::Drain { .. } => {
            let m = s.movement.unwrap();
            local_step(s,m.source.owner,admission::Action::Complete { cut: m.cut.unwrap() })
        },
        Action::Copy { .. } => {
            let m = s.movement.unwrap();
            // Exact replacement includes absence and removes destination-only bytes.
            State { data: s.data.insert(m.destination,s.data[m.source.owner]),
                ..set_phase(s,Phase::Copied) }
        },
        Action::Publish { .. } => State {
            route: target(s.movement.unwrap()), ..set_phase(s,Phase::Published) },
        Action::Activate { .. } => {
            let m = s.movement.unwrap();
            set_phase(local_step(s,m.destination,
                admission::Action::Publish { view: serving(c,target(m)) }),Phase::Active)
        },
        Action::Reclaim { .. } => State {
            data: s.data.insert(s.movement.unwrap().source.owner,blank(c)), movement: None, ..s },
        Action::Cancel { .. } => {
            let m = s.movement.unwrap();
            State { movement: None,
                ..local_step(s,m.source.owner,admission::Action::Publish { view: serving(c,m.source) }) }
        },
        Action::Stutter => s,
    }
}
/// Retired history is excluded by the *local engine* completed barrier. This
/// publication index is ghost provenance, never an independently sampled clock.
pub open spec fn suffix_start(local: admission::State) -> nat {
    if local.completed == 0 { 0 } else { local.floor[local.completed] }
}
pub open spec fn suffix_inv(c: Constants, s: State, node: int) -> bool {
    forall|i: int| suffix_start(s.local[node]) <= i < s.local[node].history.len() ==>
        s.local[node].history[i] == IMap::<Key,Grant>::empty()
        || (node == s.route.owner && s.local[node].history[i] == serving(c,s.route))
}
pub open spec fn current_serving(s: State) -> bool {
    s.movement is None || s.movement.unwrap().phase is Prepared || s.movement.unwrap().phase is Active
}
pub open spec fn txn_inv(c: Constants, s: State, id: int) -> bool {
    let t = s.txns[id];
    let l = s.local[t.grant.owner];
    &&& c.nodes.contains(t.grant.owner) && t.grant.version < s.next_generation
    &&& t.observed.dom() == c.scope && t.replacement.dom() == c.scope
    &&& l.readers.contains_key(id) && l.readers[id].entered
    &&& l.readers[id].view == Some(t.captured) && t.captured < l.history.len()
    &&& l.history[t.captured as int] == serving(c,t.grant)
    &&& (t.cleaned ==> !(t.outcome is Pending))
    &&& (l.readers[id].finished ==> t.cleaned)
    &&& (!l.readers[id].finished ==> t.grant == s.route)
}
pub open spec fn move_inv(c: Constants, s: State, m: Move) -> bool {
    &&& c.nodes.contains(m.source.owner) && c.nodes.contains(m.destination)
    &&& m.source.owner != m.destination
    &&& (m.precopy_open ==> precopy_phase(m) && !m.precopy_joined)
    &&& (m.precopy_joined ==> !m.precopy_open && s.effects.is_empty())
    &&& (copied(m) ==> m.precopy_joined)
    &&& m.source.version < m.generation && m.generation < s.next_generation
    &&& s.route == if before_publication(m) { m.source } else { target(m) }
    &&& (m.phase is Prepared ==> m.frozen_index is None && m.cut is None)
    &&& (!(m.phase is Prepared) ==> m.frozen_index is Some
        && m.frozen_index.unwrap() < s.local[m.source.owner].history.len()
        && forall|i: int| m.frozen_index.unwrap() <= i < s.local[m.source.owner].history.len()
            ==> s.local[m.source.owner].history[i] == IMap::<Key,Grant>::empty())
    &&& (m.cut is Some ==> m.frozen_index is Some
        && 0 < m.cut.unwrap() <= s.local[m.source.owner].epoch
        && m.frozen_index.unwrap() <= s.local[m.source.owner].floor[m.cut.unwrap()])
    &&& (copied(m) ==> m.cut is Some
        && m.cut.unwrap() <= s.local[m.source.owner].completed
        && s.data[m.destination] == s.logical)
}
pub open spec fn inv(c: Constants, s: State) -> bool {
    &&& (forall|id: int| #[trigger] s.packets.contains_key(id) ==>
        c.scope.contains(s.packets[id].key) && c.nodes.contains(s.packets[id].destination)
            && s.packets[id].generation < s.next_generation)
    &&& (forall|id: int| #[trigger] s.effects.contains(id) ==>
        s.packets.contains_key(id) && packet_current(s,s.packets[id])
            && precopy_phase(s.movement.unwrap()) && !s.movement.unwrap().precopy_joined)
    &&& s.local.dom() == c.nodes && s.data.dom() == c.nodes
    &&& c.nodes.contains(s.route.owner) && s.route.version < s.next_generation
    &&& s.logical.dom() == c.scope && s.data[s.route.owner] == s.logical
    &&& (forall|n: int| c.nodes.contains(n) ==> admission::inv(#[trigger] s.local[n])
        && s.data[n].dom() == c.scope && suffix_inv(c,s,n)
        && admission::current_view(s.local[n]) ==
            if current_serving(s) && n == s.route.owner { serving(c,s.route) } else { IMap::empty() })
    &&& (forall|id: int| #[trigger] s.txns.contains_key(id) ==> txn_inv(c,s,id))
    &&& (s.movement is Some ==> move_inv(c,s,s.movement.unwrap()))
}
pub open spec fn behavior(c: Constants, states: Seq<State>, actions: Seq<Action>) -> bool {
    states.len() == actions.len() + 1 && states[0] == initial(c)
        && forall|i: int| 0 <= i < actions.len() ==> enabled(c,states[i],actions[i])
            && states[i+1] == apply(c,states[i],actions[i])
}

} // verus!
