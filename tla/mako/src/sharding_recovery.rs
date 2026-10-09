//! Protocol proof against interfaces, NOT a production recovery adapter.
//!
//! Trust boundary: (1) each authority's Raft retains exactly its committed
//! prefix; acceptance/timeout is not commitment; (2) a successful transaction
//! atomically makes its engine writes and terminal outcome durable, and a
//! settled abort fences all old work. Copy/reservation/delete operations are
//! successful local engine transactions through that same interface: exact
//! immutable packet bytes are persisted, and deletion completion reports a
//! finished, generation-scoped deletion, not merely an accepted request.
//! `StartCopy` and `StartCleanup` are the committed reservation/fence points,
//! before asynchronous work starts. An unknown reservation may not start work.
//! Receipt and pending-work dictionaries are durable engine data, queried
//! directly through its recovered transaction interface, not volatile caches.
//! These are interface requirements, not implemented production adapters.
//!
//! There is no atomic multi-authority migration log. Coordinator decisions,
//! participant fences/copy selectors/certificates, and transaction leases have
//! separate checkpoint journals. Transaction success changes ONLY the engine
//! suffixes/outcome, never a sharding journal. Full local checkpoints are a
//! deliberately simple encoding: replay replaces that authority's image, then
//! overlays its durable transaction-engine suffix. `core.placement` is ghost
//! linearization state; it is neither a persisted global log nor recovery input.
//! The induction below derives its invariant and the exact reconstruction
//! equation from construction, rather than assuming either on restart.
use vstd::prelude::*;
use vstd::imap::IMap;
use crate::sharding_placement as p;
#[path = "sharding_recovery_images.rs"]
pub mod images;
pub use images::*;
#[path = "sharding_recovery_proofs.rs"]
pub mod proofs;
pub use proofs::*;
#[path = "sharding_recovery_witnesses.rs"]
pub mod witnesses;

verus! {

pub enum Mode { Down, Replaying, Recovered, Admitted, Active }
pub struct Runtime {
    pub mode: Mode, pub cursor: nat, pub engine_cursor: nat,
    pub cache: Option<Image>, pub admission_floor: nat,
}
pub struct State {
    pub core: Core,
    pub journals: IMap<Authority, Seq<Checkpoint>>,
    // Physical transaction-journal key enumeration, not a coordinator log.
    // IMap entries outside this finite catalogue are virtual genesis images.
    // Creating/appending a transaction authority's own journal makes its key
    // discoverable in the same store operation; Raft recovery must enumerate
    // those committed namespaces and must not forget them after a crash.
    pub lease_names: Set<int>,
    pub engines: IMap<Authority, Seq<EngineWrite>>,
    pub runtime: IMap<Authority, Runtime>,
    pub accepted: Set<Envelope>,
    pub pending: Map<int, Map<int, Option<int>>>,
    pub settled: Set<int>,
    pub copying: Set<p::Packet>, pub copied: Set<p::Packet>,
    pub cleaning: Set<(nat,int,p::Command)>, pub cleaned: Set<(nat,int,p::Command)>,
}
pub struct EngineAuthorityImage {
    pub writes: Seq<EngineWrite>,pub pending: Option<Map<int,Option<int>>>,pub settled: bool,
    pub copying: Set<p::Packet>,pub copied: Set<p::Packet>,
    pub cleaning: Set<(nat,int,p::Command)>,pub cleaned: Set<(nat,int,p::Command)>,
}
// The sets in State are ghost unions of disjoint authority-owned engine tables.
// This projection makes their persistence/read ownership explicit.
pub open spec fn engine_image(s: State, a: Authority) -> EngineAuthorityImage {
    match a {
        Authority::Participant(n) => EngineAuthorityImage {
            writes: s.engines[a],pending: None,settled: false,
            copying: s.copying.filter(|packet: p::Packet| s.core.placement.plans[packet.generation].dst == n),
            copied: s.copied.filter(|packet: p::Packet| s.core.placement.plans[packet.generation].dst == n),
            cleaning: s.cleaning.filter(|entry: (nat,int,p::Command)| entry.1 == n),
            cleaned: s.cleaned.filter(|entry: (nat,int,p::Command)| entry.1 == n),
        },
        Authority::Transaction(t) => EngineAuthorityImage {
            writes: s.engines[a],pending: if s.pending.contains_key(t) { Some(s.pending[t]) } else { None },
            settled: s.settled.contains(t),copying: Set::empty(),copied: Set::empty(),
            cleaning: Set::empty(),cleaned: Set::empty(),
        },
        Authority::Coordinator => EngineAuthorityImage {
            writes: s.engines[a],pending: None,settled: false,copying: Set::empty(),copied: Set::empty(),
            cleaning: Set::empty(),cleaned: Set::empty(),
        },
    }
}
pub enum Action {
    Boot { authority: Authority }, Crash { authority: Authority },
    ReplayCheckpoint { authority: Authority }, ReplayEngine { authority: Authority },
    InstallSnapshot { authority: Authority, index: nat },
    Complete { authority: Authority }, Admit { authority: Authority }, Activate { authority: Authority },
    Accept { envelope: Envelope }, AppendUnknown { envelope: Envelope }, Commit { envelope: Envelope },
    EngineStart { txn: int, writes: Map<int,Option<int>>, incarnation: nat, targets: Map<int,(nat,nat)> },
    EngineUnknown { txn: int }, EngineSettle { txn: int, committed: bool },
    StartCopy { packet: p::Packet }, DurableCopy { packet: p::Packet },
    StartCleanup { generation: nat, owner: int, command: p::Command },
    DurableCleanup { generation: nat, owner: int, command: p::Command },
    Remove { owner: int }, Join { owner: int }, Stutter,
}
pub open spec fn valid_authority(c: p::Constants, a: Authority) -> bool {
    match a { Authority::Coordinator => true, Authority::Participant(n) => c.shards.contains(n), Authority::Transaction(t) => t >= 0 }
}
pub open spec fn head(s: State, a: Authority) -> Checkpoint { s.journals[a].last() }
pub open spec fn active(s: State, a: Authority) -> bool { s.runtime[a].mode is Active }
pub open spec fn registered(s: State, a: Authority) -> bool {
    match a { Authority::Participant(n) => s.core.members.contains_key(n), _ => true }
}
pub open spec fn member(s: State, a: Authority) -> nat {
    match a { Authority::Participant(n) => s.core.members[n], _ => 0 }
}
pub open spec fn down() -> Runtime {
    Runtime { mode: Mode::Down,cursor: 0,engine_cursor: 0,cache: None,admission_floor: 0 }
}
pub open spec fn initial_core(c: p::Constants) -> Core {
    Core { placement: p::initial(c),members: Map::new(c.shards,|_owner: int| 0),owner_floor: Map::new(c.shards,|_owner: int| 0) }
}
pub open spec fn initial(c: p::Constants) -> State {
    let core = initial_core(c);
    State { core,lease_names: Set::empty(),
        journals: IMap::new(|_authority: Authority| true,|a: Authority| seq![Checkpoint {
            image: project(core,a),engine_offset: 0,incarnation: 0,membership: 0,processed: Set::empty(),
        }]),
        engines: IMap::new(|_authority: Authority| true,|_authority: Authority| Seq::empty()),
        runtime: IMap::new(|_authority: Authority| true,|_authority: Authority| down()),
        accepted: Set::empty(),pending: Map::empty(),settled: Set::empty(),
        copying: Set::empty(),copied: Set::empty(),cleaning: Set::empty(),cleaned: Set::empty(),
    }
}
pub open spec fn caught_up(s: State, a: Authority) -> bool {
    s.runtime[a].cursor == s.journals[a].len() && s.runtime[a].engine_cursor == s.engines[a].len()
}
pub open spec fn credential(s: State, a: Authority, incarnation: nat, membership: nat) -> bool {
    registered(s,a) && head(s,a).incarnation == incarnation
        && head(s,a).membership == membership && membership == member(s,a)
}
pub open spec fn target(s: State, action: p::Action) -> Option<(int,nat,nat)> {
    match action {
        p::Action::Acquire { grant, .. } => Some((grant.owner,s.core.members[grant.owner],head(s,Authority::Participant(grant.owner)).incarnation)),
        p::Action::Begin { src,dst,.. } => Some((src,s.core.members[src],s.core.members[dst])),
        _ => None,
    }
}
pub open spec fn envelope(s: State, action: p::Action) -> Envelope {
    let a = actor(s.core.placement,action);
    Envelope { action,slot: s.journals[a].len(),incarnation: head(s,a).incarnation,membership: member(s,a),target: target(s,action) }
}
pub open spec fn admission_guard(s: State, e: Envelope) -> bool {
    match e.action {
        p::Action::Acquire { grant, .. } => {
            let a = Authority::Participant(grant.owner);
            active(s,a) && credential(s,a,head(s,a).incarnation,member(s,a)) && e.target == target(s,e.action)
        },
        p::Action::Begin { src,dst,.. } => s.core.members.contains_key(src) && s.core.members.contains_key(dst)
            && e.target == target(s,e.action),
        _ => true,
    }
}
pub open spec fn destructive(s: p::State, g: nat, cmd: p::Command, n: int) -> bool {
    cmd is Commit && n == s.plans[g].src || cmd is Abort && n == s.plans[g].dst
}
pub open spec fn copy_fenced(s: State, packet: p::Packet) -> bool {
    s.cleaning.contains((packet.generation,s.core.placement.plans[packet.generation].dst,p::Command::Abort))
}
pub open spec fn effect_guard(s: State, e: Envelope) -> bool {
    admission_guard(s,e) && match e.action {
        p::Action::Resolve { .. } => false,
        p::Action::DeliverCopy { packet } => s.copied.contains(packet) && !copy_fenced(s,packet),
        p::Action::Deliver { generation,command,owner } =>
            !destructive(s.core.placement,generation,command,owner) || s.cleaned.contains((generation,owner,command)),
        _ => true,
    }
}
pub open spec fn owner_idle(s: State, owner: int) -> bool {
    (forall|k: int| p::directory(s.core.placement).contains_key(k) ==> p::directory(s.core.placement)[k].owner != owner)
    && (s.core.placement.active is Some ==> {
        let plan = s.core.placement.plans[s.core.placement.active.unwrap()]; plan.src != owner && plan.dst != owner
    })
    && (forall|t: int,k: int| s.core.placement.sessions.contains_key(t) && s.core.placement.sessions[t].held.contains_key(k)
        ==> s.core.placement.sessions[t].held[k].owner != owner)
}
pub open spec fn enabled(c: p::Constants, s: State, action: Action) -> bool {
    match action {
        Action::Boot { authority: a } => valid_authority(c,a) && s.runtime[a].mode is Down,
        Action::Crash { authority: a } => valid_authority(c,a),
        Action::ReplayCheckpoint { authority: a } => valid_authority(c,a) && !(s.runtime[a].mode is Down || s.runtime[a].mode is Active)
            && s.runtime[a].cursor < s.journals[a].len(),
        Action::InstallSnapshot { authority: a,index } => valid_authority(c,a)
            && !(s.runtime[a].mode is Down || s.runtime[a].mode is Active) && index < s.journals[a].len(),
        Action::ReplayEngine { authority: a } => valid_authority(c,a) && !(s.runtime[a].mode is Down || s.runtime[a].mode is Active)
            && s.runtime[a].cursor == s.journals[a].len() && s.runtime[a].engine_cursor < s.engines[a].len(),
        Action::Complete { authority: a } => valid_authority(c,a) && s.runtime[a].mode is Replaying && caught_up(s,a),
        Action::Admit { authority: a } => valid_authority(c,a) && s.runtime[a].mode is Recovered && caught_up(s,a) && registered(s,a),
        Action::Activate { authority: a } => valid_authority(c,a) && s.runtime[a].mode is Admitted && caught_up(s,a)
            && registered(s,a) && head(s,a).membership == member(s,a),
        Action::Accept { envelope: e } => {
            let a = actor(s.core.placement,e.action);
            valid_authority(c,a) && active(s,a) && credential(s,a,e.incarnation,e.membership)
                && e.slot == s.journals[a].len() && effect_guard(s,e) && p::enabled(c,s.core.placement,e.action)
        },
        Action::AppendUnknown { envelope: e } => s.accepted.contains(e),
        Action::Commit { envelope: e } => {
            let a = actor(s.core.placement,e.action);
            valid_authority(c,a) && s.accepted.contains(e) && !head(s,a).processed.contains(e)
                && e.slot == s.journals[a].len() && credential(s,a,e.incarnation,e.membership) && effect_guard(s,e)
        },
        Action::EngineStart { txn,writes,incarnation,targets } => active(s,Authority::Transaction(txn))
            && head(s,Authority::Transaction(txn)).incarnation == incarnation
            && !s.pending.contains_key(txn) && p::can_resolve(s.core.placement,txn,writes)
            && forall|k: int| s.core.placement.sessions[txn].held.contains_key(k) ==> {
                let n = s.core.placement.sessions[txn].held[k].owner;
                let a = Authority::Participant(n);
                active(s,a) && targets.contains_key(n)
                    && credential(s,a,targets[n].1,targets[n].0)
            },
        Action::EngineUnknown { txn } => s.pending.contains_key(txn),
        Action::EngineSettle { txn,.. } => s.pending.contains_key(txn),
        Action::StartCopy { packet } => s.core.placement.packets.contains(packet)
            && p::copy_guard(s.core.placement,packet) && !copy_fenced(s,packet)
            && active(s,Authority::Participant(s.core.placement.plans[packet.generation].dst)),
        Action::DurableCopy { packet } => s.copying.contains(packet) && !s.copied.contains(packet),
        Action::StartCleanup { generation,owner,command } => active(s,Authority::Participant(owner))
            && s.core.placement.commands.contains((generation,command))
            && p::local_guard(s.core.placement,generation,command,owner)
            && destructive(s.core.placement,generation,command,owner),
        Action::DurableCleanup { generation,owner,command } => s.cleaning.contains((generation,owner,command))
            && !s.cleaned.contains((generation,owner,command))
            && forall|packet: p::Packet| s.copying.contains(packet) && packet.generation == generation
                && s.core.placement.plans[packet.generation].dst == owner ==> s.copied.contains(packet),
        Action::Remove { owner } => active(s,Authority::Coordinator) && s.core.members.contains_key(owner)
            && !(s.runtime[Authority::Participant(owner)].mode is Active) && owner_idle(s,owner),
        Action::Join { owner } => active(s,Authority::Coordinator) && c.shards.contains(owner) && !s.core.members.contains_key(owner),
        Action::Stutter => true,
    }
}
// Active processes apply their own committed entry/engine effect. Offline or
// replaying processes do not receive magical cache updates.
pub open spec fn refresh(s: State) -> State {
    State { runtime: IMap::new(|_authority: Authority| true,|a: Authority|
        if active(s,a) { Runtime { cursor: s.journals[a].len(),engine_cursor: s.engines[a].len(),
            cache: Some(project(s.core,a)),..s.runtime[a] } } else { s.runtime[a] }),..s }
}
pub open spec fn recorded_names(names: Set<int>, a: Authority) -> Set<int> {
    match a { Authority::Transaction(t) => names.insert(t), _ => names }
}
pub open spec fn append_checkpoint(s: State, a: Authority, core: Core, processed: Set<Envelope>) -> State {
    refresh(State { core,lease_names: recorded_names(s.lease_names,a),
        journals: s.journals.insert(a,s.journals[a].push(Checkpoint {
        image: project(core,a),engine_offset: s.engines[a].len(),processed,..head(s,a)
    })),..s })
}
pub open spec fn engine_settle(c: p::Constants, s: State, t: int, committed: bool) -> State {
    let writes = if committed { s.pending[t] } else { Map::empty() };
    let old = s.core.placement;
    let placement = p::apply(c,old,p::Action::Resolve { txn: t,writes });
    refresh(State {
        core: with_placement(s.core,placement),
        engines: IMap::new(|_authority: Authority| true,|a: Authority| match a {
            Authority::Participant(n) => if transaction_write(old,t,writes,n).cells.dom() != Set::<int>::empty() {
                s.engines[a].push(transaction_write(old,t,writes,n))
            } else { s.engines[a] },
            _ => s.engines[a],
        }),
        pending: s.pending.remove(t),settled: s.settled.insert(t),..s
    })
}
pub open spec fn apply(c: p::Constants, s: State, action: Action) -> State {
    match action {
        Action::Boot { authority: a } => State { runtime: s.runtime.insert(a,Runtime { mode: Mode::Replaying,..down() }),..s },
        Action::Crash { authority: a } => State { runtime: s.runtime.insert(a,down()),..s },
        Action::ReplayCheckpoint { authority: a } => {
            let r = s.runtime[a]; let checkpoint = s.journals[a][r.cursor as int];
            State { runtime: s.runtime.insert(a,Runtime { mode: Mode::Replaying,cursor: r.cursor+1,
                engine_cursor: checkpoint.engine_offset,cache: Some(checkpoint.image),..r }),..s }
        },
        Action::InstallSnapshot { authority: a,index } => {
            let checkpoint = s.journals[a][index as int];
            State { runtime: s.runtime.insert(a,Runtime { mode: Mode::Replaying,cursor: index+1,
                engine_cursor: checkpoint.engine_offset,cache: Some(checkpoint.image),..s.runtime[a] }),..s }
        },
        Action::ReplayEngine { authority: a } => {
            let r = s.runtime[a];
            State { runtime: s.runtime.insert(a,Runtime { mode: Mode::Replaying,engine_cursor: r.engine_cursor+1,
                cache: Some(overlay(r.cache.unwrap(),s.engines[a][r.engine_cursor as int])),..r }),..s }
        },
        Action::Complete { authority: a } => State { runtime: s.runtime.insert(a,Runtime {
            mode: Mode::Recovered,admission_floor: head(s,a).incarnation,..s.runtime[a] }),..s },
        Action::Admit { authority: a } => {
            let checkpoint = Checkpoint { image: s.runtime[a].cache.unwrap(),engine_offset: s.engines[a].len(),
                incarnation: head(s,a).incarnation+1,membership: member(s,a),..head(s,a) };
            State { lease_names: recorded_names(s.lease_names,a),
                journals: s.journals.insert(a,s.journals[a].push(checkpoint)),
                runtime: s.runtime.insert(a,Runtime { mode: Mode::Admitted,cursor: s.journals[a].len()+1,..s.runtime[a] }),..s }
        },
        Action::Activate { authority: a } => State { runtime: s.runtime.insert(a,Runtime { mode: Mode::Active,..s.runtime[a] }),..s },
        Action::Accept { envelope: e } => State { accepted: s.accepted.insert(e),..s },
        Action::Commit { envelope: e } => {
            let a = actor(s.core.placement,e.action);
            append_checkpoint(s,a,with_placement(s.core,p::dispatch(c,s.core.placement,e.action)),head(s,a).processed.insert(e))
        },
        Action::EngineStart { txn,writes,.. } => State { pending: s.pending.insert(txn,writes),..s },
        Action::EngineSettle { txn,committed } => engine_settle(c,s,txn,committed),
        Action::StartCopy { packet } => State { copying: s.copying.insert(packet),..s },
        Action::DurableCopy { packet } => State { copied: s.copied.insert(packet),..s },
        Action::StartCleanup { generation,owner,command } => State { cleaning: s.cleaning.insert((generation,owner,command)),..s },
        Action::DurableCleanup { generation,owner,command } => State { cleaned: s.cleaned.insert((generation,owner,command)),..s },
        Action::Remove { owner } => append_checkpoint(s,Authority::Coordinator,
            Core { members: s.core.members.remove(owner),..s.core },head(s,Authority::Coordinator).processed),
        Action::Join { owner } => append_checkpoint(s,Authority::Coordinator,Core {
            members: s.core.members.insert(owner,s.core.owner_floor[owner]+1),
            owner_floor: s.core.owner_floor.insert(owner,s.core.owner_floor[owner]+1),..s.core },head(s,Authority::Coordinator).processed),
        _ => s,
    }
}
pub open spec fn dispatch(c: p::Constants, s: State, a: Action) -> State { if enabled(c,s,a) { apply(c,s,a) } else { s } }
pub open spec fn next(c: p::Constants, s: State, z: State) -> bool { exists|a: Action| z == #[trigger] dispatch(c,s,a) }
pub open spec fn behavior(c: p::Constants, states: Seq<State>) -> bool {
    states.len() > 0 && states[0] == initial(c)
        && forall|i: int| 0 <= i < states.len()-1 ==> #[trigger] next(c,states[i],states[i+1])
}

// A retained lease is not itself a post-restart serving capability. Both the
// transaction authority and participant must present their current admissions.
pub open spec fn read(s: State, txn: int, key: int, transaction_incarnation: nat,
    participant_incarnation: nat, membership: nat) -> Option<p::Cell>
{
    if p::live_lease(s.core.placement,txn,key)
        && active(s,Authority::Transaction(txn))
        && head(s,Authority::Transaction(txn)).incarnation == transaction_incarnation
    {
        let a = Authority::Participant(s.core.placement.sessions[txn].held[key].owner);
        if active(s,a) && credential(s,a,participant_incarnation,membership) {
            p::read(s.core.placement,txn,key)
        } else { None }
    } else { None }
}

} // verus!
