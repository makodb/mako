//! Proposed commit-time admission, NOT a refinement of the current native path.
//! An arbitrary physical row under an ownership coordinate (including absence)
//! is projected from one common execution. IDs and handoffs are unbounded.
//! Copy conflicts with unsettled logical write intents; this guard is an engine
//! requirement, not a fact established about MassTrans by this model.
//! Ordinary OCC history/opacity, continuous scan discovery, machine memory
//! ordering and C++/FFI refinement are separate obligations.
use vstd::prelude::*;
#[path = "sharding_commit_validation_proofs.rs"]
pub mod proofs;
#[path = "sharding_commit_validation_authority.rs"]
pub mod authority;
#[path = "sharding_commit_validation_witnesses.rs"]
pub mod witnesses;

verus! {

pub struct Grant { pub owner: int, pub version: nat }
/// Writer is semantic provenance, NOT a native STO timestamp.
pub struct Cell { pub value: Option<int>, pub writer: Option<int> }
pub struct Constants { pub nodes: Set<int>, pub initial: Grant, pub value: Option<int> }
pub struct Txn {
    pub grant: Grant,
    pub epoch: nat,
    pub writes: bool,
    pub value: Option<int>,
    pub observed: Option<Cell>,
    pub locked: bool,
    pub sealed: bool,
    pub authority_ok: bool,
    pub read_ok: bool,
    pub installed: bool,
    pub aborted: bool,
    pub finished: bool,
}
pub enum Phase { Prepared, Frozen, Retired, Published, Active }
pub struct Move {
    pub source: Grant,
    pub destination: int,
    pub generation: nat,
    pub phase: Phase,
    pub copied: bool,
}
pub struct State {
    pub route: Grant,
    pub serving: Map<int, Option<nat>>,
    pub epochs: Map<int, nat>,
    pub data: Map<int, Cell>,
    pub available: Set<int>,
    pub logical: Cell,
    pub txns: Map<int, Txn>,
    pub locks: Map<int, int>,
    pub movement: Option<Move>,
    pub next_generation: nat,
}
pub enum Action {
    Capture { id: int, writes: bool, value: Option<int> },
    Observe { id: int },
    Lock { id: int },
    Seal { id: int },
    ValidateAuthority { id: int },
    ValidateData { id: int },
    Install { id: int },
    Abort { id: int },
    Release { id: int },
    Finish { id: int },
    PrepareMove { destination: int, generation: nat },
    Freeze { generation: nat },
    Copy { generation: nat },
    Retire { generation: nat },
    Publish { generation: nat },
    Activate { generation: nat },
    Reclaim { generation: nat },
    Cancel { generation: nat },
    EpochBump { owner: int },
    Stutter,
}
pub open spec fn constants(c: Constants) -> bool { c.nodes.contains(c.initial.owner) }
pub open spec fn empty() -> Cell { Cell { value: None, writer: None } }
pub open spec fn target(m: Move) -> Grant { Grant { owner: m.destination, version: m.generation } }
pub open spec fn initial(c: Constants) -> State {
    let cell = Cell { value: c.value, writer: None };
    State {
        route: c.initial,
        serving: Map::new(c.nodes, |n: int|
            if n == c.initial.owner { Some(c.initial.version) } else { None }),
        epochs: Map::new(c.nodes, |n: int| 0nat),
        data: Map::new(c.nodes, |n: int|
            if n == c.initial.owner { cell } else { empty() }),
        available: c.nodes, logical: cell, txns: Map::empty(), locks: Map::empty(),
        movement: None, next_generation: c.initial.version + 1,
    }
}
pub open spec fn live(s: State, id: int) -> bool {
    s.txns.contains_key(id) && !s.txns[id].finished && !s.txns[id].aborted
}
pub open spec fn pending(t: Txn) -> bool {
    t.writes && t.authority_ok && !t.installed && !t.aborted && !t.finished
}
pub open spec fn effects_settled(t: Txn) -> bool { t.installed || t.aborted }
/// A successful engine read must exclude an UNSETTLED intent. A physically
/// unlinked committed deletion can already expose final absence while its
/// removed value remains locked; cleanup still retains its lifetime pin.
pub open spec fn copy_conflict_free(s: State) -> bool {
    let owner = s.movement.unwrap().source.owner;
    !s.locks.contains_key(owner) || effects_settled(s.txns[s.locks[owner]])
}
pub open spec fn pins_clear(s: State, owner: int) -> bool {
    forall|id: int| #[trigger] s.txns.contains_key(id) && !s.txns[id].finished ==>
        s.txns[id].grant.owner != owner
}
pub open spec fn current_move(s: State, generation: nat) -> bool {
    s.movement is Some && s.movement.unwrap().generation == generation
}
pub open spec fn before_publication(m: Move) -> bool {
    m.phase is Prepared || m.phase is Frozen || m.phase is Retired
}
pub open spec fn can_validate(s: State, id: int) -> bool {
    live(s,id) && s.txns[id].sealed
        && (s.epochs[s.txns[id].grant.owner] == s.txns[id].epoch
            || s.serving[s.txns[id].grant.owner] == Some(s.txns[id].grant.version))
}
pub open spec fn enabled(c: Constants, s: State, a: Action) -> bool {
    match a {
        Action::Capture { id,.. } => !s.txns.contains_key(id)
            && s.serving[s.route.owner] == Some(s.route.version),
        Action::Observe { id } => live(s,id) && !s.txns[id].sealed,
        Action::Lock { id } => live(s,id) && s.txns[id].writes && !s.txns[id].locked
            && !s.txns[id].sealed && !s.locks.contains_key(s.txns[id].grant.owner),
        Action::Seal { id } => live(s,id) && !s.txns[id].sealed
            && (!s.txns[id].writes || s.txns[id].locked),
        Action::ValidateAuthority { id } => can_validate(s,id),
        Action::ValidateData { id } => live(s,id) && s.txns[id].sealed
            && (s.txns[id].observed is None || (
                s.data[s.txns[id].grant.owner] == s.txns[id].observed.unwrap()
                && (!s.locks.contains_key(s.txns[id].grant.owner)
                    || s.locks[s.txns[id].grant.owner] == id))),
        Action::Install { id } => live(s,id) && s.txns[id].writes
            && s.txns[id].authority_ok && s.txns[id].read_ok && !s.txns[id].installed,
        Action::Abort { id } => live(s,id) && !s.txns[id].installed,
        Action::Release { id } => s.txns.contains_key(id) && !s.txns[id].finished
            && s.txns[id].locked && effects_settled(s.txns[id]),
        Action::Finish { id } => s.txns.contains_key(id) && !s.txns[id].finished
            && (s.txns[id].aborted || s.txns[id].installed || (
                !s.txns[id].writes && s.txns[id].authority_ok && s.txns[id].read_ok)),
        Action::PrepareMove { destination,generation } => s.movement is None
            && generation >= s.next_generation
            && c.nodes.contains(destination) && destination != s.route.owner
            && pins_clear(s,destination),
        Action::Freeze { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Prepared,
        Action::Copy { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Frozen
            && copy_conflict_free(s),
        Action::Retire { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Frozen && s.movement.unwrap().copied,
        Action::Publish { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Retired,
        Action::Activate { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Published,
        Action::Reclaim { generation } => current_move(s,generation)
            && s.movement.unwrap().phase is Active
            && pins_clear(s,s.movement.unwrap().source.owner),
        Action::Cancel { generation } => current_move(s,generation)
            && before_publication(s.movement.unwrap()),
        Action::EpochBump { owner } => c.nodes.contains(owner),
        Action::Stutter => true,
    }
}
pub open spec fn put_tx(s: State, id: int, t: Txn) -> State {
    State { txns: s.txns.insert(id,t), ..s }
}
pub open spec fn move_phase(s: State, phase: Phase) -> State {
    State { movement: Some(Move { phase, ..s.movement.unwrap() }), ..s }
}
pub open spec fn apply(s: State, a: Action) -> State {
    match a {
        Action::Capture { id,writes,value } => put_tx(s,id,Txn {
            grant: s.route, epoch: s.epochs[s.route.owner], writes, value, observed: None, locked: false,
            sealed: false, authority_ok: false, read_ok: false, installed: false,
            aborted: false, finished: false,
        }),
        Action::Observe { id } => put_tx(s,id,Txn {
            observed: if s.txns[id].observed is None {
                Some(s.data[s.txns[id].grant.owner])
            } else { s.txns[id].observed }, ..s.txns[id] }),
        Action::Lock { id } => State {
            txns: s.txns.insert(id,Txn { locked: true, ..s.txns[id] }),
            locks: s.locks.insert(s.txns[id].grant.owner,id), ..s },
        Action::Seal { id } => put_tx(s,id,Txn { sealed: true, ..s.txns[id] }),
        Action::ValidateAuthority { id } => put_tx(s,id,Txn { authority_ok: true, ..s.txns[id] }),
        Action::ValidateData { id } => put_tx(s,id,Txn { read_ok: true, ..s.txns[id] }),
        Action::Install { id } => {
            let cell = Cell { value: s.txns[id].value, writer: Some(id) };
            State { txns: s.txns.insert(id,Txn { installed: true, ..s.txns[id] }),
                data: s.data.insert(s.txns[id].grant.owner,cell), logical: cell, ..s }
        },
        Action::Abort { id } => put_tx(s,id,Txn { aborted: true, ..s.txns[id] }),
        // Logical settlement permits unlock; it does NOT end the storage pin.
        Action::Release { id } => State {
            txns: s.txns.insert(id,Txn { locked: false, ..s.txns[id] }),
            locks: s.locks.remove(s.txns[id].grant.owner), ..s },
        // Finish denotes actual completed cleanup, never a timeout or unknown
        // outcome. Retaining locks until Finish is allowed but not required.
        Action::Finish { id } => State {
            txns: s.txns.insert(id,Txn { finished: true, locked: false, ..s.txns[id] }),
            locks: if s.txns[id].locked { s.locks.remove(s.txns[id].grant.owner) } else { s.locks }, ..s },
        // The common coordinator supplies a fresh generation. Other ranges can
        // have consumed intervening numbers; the local projection permits gaps.
        Action::PrepareMove { destination,generation } => State {
            movement: Some(Move { source: s.route, destination,
                generation, phase: Phase::Prepared, copied: false }),
            next_generation: generation + 1,
            available: s.available.insert(destination), ..s },
        Action::Freeze { .. } => State {
            serving: s.serving.insert(s.movement.unwrap().source.owner,None),
            epochs: s.epochs.insert(s.movement.unwrap().source.owner,s.epochs[s.movement.unwrap().source.owner] + 1),
            ..move_phase(s,Phase::Frozen) },
        Action::Copy { .. } => {
            let m = s.movement.unwrap();
            State { data: s.data.insert(m.destination,s.data[m.source.owner]),
                movement: Some(Move { copied: true, ..m }), ..s }
        },
        Action::Retire { .. } => move_phase(s,Phase::Retired),
        Action::Publish { .. } => State {
            route: target(s.movement.unwrap()), ..move_phase(s,Phase::Published) },
        Action::Activate { .. } => State {
            serving: s.serving.insert(s.movement.unwrap().destination,Some(s.movement.unwrap().generation)),
            epochs: s.epochs.insert(s.movement.unwrap().destination,s.epochs[s.movement.unwrap().destination] + 1),
            ..move_phase(s,Phase::Active) },
        Action::Reclaim { .. } => State {
            data: s.data.insert(s.movement.unwrap().source.owner,empty()),
            available: s.available.remove(s.movement.unwrap().source.owner), movement: None, ..s },
        Action::Cancel { .. } => State {
            serving: s.serving.insert(s.movement.unwrap().source.owner,Some(s.movement.unwrap().source.version)),
            epochs: s.epochs.insert(s.movement.unwrap().source.owner,s.epochs[s.movement.unwrap().source.owner] + 1),
            movement: None, ..s },
        Action::EpochBump { owner } => State { epochs: s.epochs.insert(owner,s.epochs[owner] + 1), ..s },
        Action::Stutter => s,
    }
}
pub open spec fn txn_inv(c: Constants, s: State, id: int) -> bool {
    let t = s.txns[id];
    &&& c.nodes.contains(t.grant.owner) && t.grant.version < s.next_generation
    &&& t.epoch <= s.epochs[t.grant.owner]
    &&& (t.epoch == s.epochs[t.grant.owner] ==> s.serving[t.grant.owner] == Some(t.grant.version))
    &&& (!t.finished ==> s.available.contains(t.grant.owner))
    &&& (t.locked ==> t.writes && !t.finished && s.locks.contains_key(t.grant.owner)
        && s.locks[t.grant.owner] == id)
    &&& (t.sealed && t.writes && !t.finished && !effects_settled(t) ==> t.locked)
    &&& ((t.authority_ok || t.read_ok) ==> t.sealed)
    &&& (t.installed ==> t.writes && t.authority_ok && t.read_ok && !t.aborted)
    &&& (pending(t) ==> t.grant == s.route)
    &&& (t.finished ==> effects_settled(t) || (!t.writes && t.authority_ok && t.read_ok))
}
pub open spec fn move_inv(c: Constants, s: State, m: Move) -> bool {
    &&& c.nodes.contains(m.source.owner) && c.nodes.contains(m.destination)
    &&& m.source.owner != m.destination
    &&& m.source.version < m.generation && m.generation < s.next_generation
    &&& s.available.contains(m.source.owner) && s.available.contains(m.destination)
    &&& s.route == if before_publication(m) { m.source } else { target(m) }
    &&& s.serving[m.source.owner] == if m.phase is Prepared { Some(m.source.version) } else { None }
    &&& s.serving[m.destination] == if m.phase is Active { Some(m.generation) } else { None }
    &&& (m.phase is Prepared ==> !m.copied)
    &&& ((m.phase is Retired || m.phase is Published || m.phase is Active) ==> m.copied)
    &&& (m.copied && !(m.phase is Active) ==> s.data[m.destination] == s.data[m.source.owner])
    &&& (m.copied && !(m.phase is Active) ==> forall|id: int| #[trigger] s.txns.contains_key(id) ==>
        !pending(s.txns[id]))
    &&& (m.copied ==> forall|id: int| #[trigger] s.txns.contains_key(id) && pending(s.txns[id]) ==>
        s.txns[id].grant != m.source)
    &&& (!(m.phase is Active) ==> pins_clear(s,m.destination))
}
pub open spec fn inv(c: Constants, s: State) -> bool {
    &&& s.data.dom() == c.nodes && s.serving.dom() == c.nodes && s.epochs.dom() == c.nodes
    &&& c.nodes.contains(s.route.owner) && s.route.version < s.next_generation
    &&& s.available.subset_of(c.nodes) && s.available.contains(s.route.owner)
    &&& s.data[s.route.owner] == s.logical
    &&& (forall|n: int| c.nodes.contains(n) && #[trigger] s.serving[n] is Some ==>
        s.route == (Grant { owner: n, version: s.serving[n].unwrap() }) && s.available.contains(n))
    &&& (forall|id: int| #[trigger] s.txns.contains_key(id) ==> txn_inv(c,s,id))
    &&& (forall|n: int| #[trigger] s.locks.contains_key(n) ==> c.nodes.contains(n)
        && s.txns.contains_key(s.locks[n]) && s.txns[s.locks[n]].locked
        && s.txns[s.locks[n]].grant.owner == n)
    &&& (s.movement is Some ==> move_inv(c,s,s.movement.unwrap()))
    &&& (s.movement is None ==> s.serving[s.route.owner] == Some(s.route.version))
}
pub open spec fn behavior(c: Constants, states: Seq<State>, actions: Seq<Action>) -> bool {
    states.len() == actions.len() + 1 && states[0] == initial(c)
        && forall|i: int| 0 <= i < actions.len() ==>
            enabled(c,states[i],actions[i]) && states[i+1] == apply(states[i],actions[i])
}

} // verus!
