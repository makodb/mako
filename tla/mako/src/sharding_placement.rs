//! Corrected live-process handoff. Logical keys are canonical across physical owners.
//! All range-local effects are atomic under a participant range lock; delivery and
//! master receipt are separate. No crashes, fairness, checksum axiom, or C++/Raft
//! refinement is assumed. Cell.writer is ghost application provenance, not a STO TID.
use vstd::prelude::*;
use vstd::imap::IMap;
use vstd::iset::ISet;
use crate::sharding_partition as partition;
#[path = "sharding_placement_proofs.rs"]
pub mod proofs;
pub use proofs::*;

verus! {

pub struct Cell { pub value: Option<int>, pub writer: int }
pub struct Grant { pub owner: int, pub epoch: nat }
pub struct Session { pub held: Map<int, Grant>, pub resolved: bool }
pub struct Constants {
    pub keys: Set<int>, pub shards: Set<int>,
    pub table: Map<int, int>, pub coordinate: Map<int, int>,
    pub owners: Map<int, int>,
}
pub enum Role { Empty, Serving, Frozen, Retired, Stage, Ready }
pub struct Replica {
    pub cell: Cell, pub role: Role, pub epoch: nat, pub fence: nat,
    pub terminal: bool, pub round: nat, pub covered: bool,
}
pub struct Plan {
    pub nonce: int, pub src: int, pub dst: int,
    pub table: int, pub lo: int, pub hi: Option<int>,
    pub keys: Set<int>, pub old: Map<int, Grant>,
}
pub enum Phase { Copy, Freezing, Final, Retiring, Committed, Aborted }
pub enum Command { Start, Freeze, Final, Retire, Commit, Abort }
pub enum Certificate { Drained, Ready, Retired, SourceDone, DestinationDone }
pub struct Packet { pub generation: nat, pub round: nat, pub key: int, pub cell: Cell }
pub struct Outcome { pub generation: nat, pub committed: bool }
pub struct State {
    pub logical: Map<int, Cell>, pub sessions: Map<int, Session>,
    pub physical: IMap<(int, int), Replica>,
    pub directory: Seq<Map<int, Grant>>, pub views: Map<int, nat>,
    pub next_generation: nat, pub active: Option<nat>,
    pub plans: Map<nat, Plan>, pub phases: Map<nat, Phase>,
    pub commands: Set<(nat, Command)>, pub certificates: Set<(nat, Certificate)>,
    pub received: Set<(nat, Certificate)>, pub packets: Set<Packet>,
    pub outcomes: Map<int, Outcome>, pub replies: Map<int, Outcome>,
}
pub enum Action {
    Open { txn: int }, Acquire { txn: int, key: int, grant: Grant },
    Resolve { txn: int, writes: Map<int, Option<int>> }, Release { txn: int, key: int },
    Begin { nonce: int, src: int, dst: int, table: int, lo: int, hi: Option<int> },
    RequestFreeze, RequestFinal, RequestRetire, Commit, Abort, Finish,
    Deliver { generation: nat, command: Command, owner: int },
    Drain { generation: nat }, Seal { generation: nat },
    Receive { generation: nat, certificate: Certificate },
    Capture { generation: nat, round: nat, key: int }, DeliverCopy { packet: Packet },
    Cache { client: int, snapshot: nat }, Reply { nonce: int }, Stutter,
}
pub open spec fn empty_cell() -> Cell { Cell { value: None, writer: -1 } }
pub open spec fn constants_ok(c: Constants) -> bool {
    c.shards.len() >= 2
    && c.table.dom() == c.keys && c.coordinate.dom() == c.keys && c.owners.dom() == c.keys
    && forall|k: int| c.keys.contains(k) ==> c.coordinate[k] >= 0 && c.shards.contains(c.owners[k])
    && forall|n: int| c.shards.contains(n) ==> n >= 0
}
pub open spec fn selected(c: Constants, table: int, lo: int, hi: Option<int>) -> Set<int> {
    c.keys.filter(|k: int| c.table[k] == table
        && partition::in_range(c.coordinate[k], lo, hi))
}
pub open spec fn directory(s: State) -> Map<int, Grant> { s.directory.last() }
pub open spec fn replica(s: State, owner: int, key: int) -> Replica { s.physical[(owner, key)] }
pub open spec fn initial(c: Constants) -> State {
    State {
        logical: Map::new(c.keys, |k: int| empty_cell()),
        sessions: Map::empty(),
        physical: IMap::new(|p: (int,int)| c.shards.contains(p.0) && c.keys.contains(p.1),
            |p: (int,int)| Replica { cell: empty_cell(),
                role: if c.owners[p.1] == p.0 { Role::Serving } else { Role::Empty },
                epoch: 0, fence: 0, terminal: true, round: 0, covered: false }),
        directory: seq![Map::new(c.keys, |k: int| Grant { owner: c.owners[k], epoch: 0 })],
        views: Map::empty(), next_generation: 1, active: None,
        plans: Map::empty(), phases: Map::empty(), commands: Set::empty(),
        certificates: Set::empty(), received: Set::empty(), packets: Set::empty(),
        outcomes: Map::empty(), replies: Map::empty(),
    }
}
pub open spec fn current(s: State, g: nat) -> bool { s.active == Some(g) }
pub open spec fn terminal(p: Phase) -> bool { p is Committed || p is Aborted }
pub open spec fn before_decision(s: State, g: nat) -> bool { !terminal(s.phases[g]) }
pub open spec fn drained(s: State, keys: Set<int>) -> bool {
    forall|t: int, k: int| s.sessions.contains_key(t) && keys.contains(k)
        ==> !s.sessions[t].held.contains_key(k)
}
pub open spec fn live_lease(s: State, txn: int, key: int) -> bool {
    s.sessions.contains_key(txn) && !s.sessions[txn].resolved && s.sessions[txn].held.contains_key(key)
}
pub open spec fn read(s: State, txn: int, key: int) -> Option<Cell> {
    if live_lease(s, txn, key) { Some(replica(s, s.sessions[txn].held[key].owner, key).cell) } else { None }
}
pub open spec fn can_resolve(s: State, txn: int, writes: Map<int, Option<int>>) -> bool {
    s.sessions.contains_key(txn) && !s.sessions[txn].resolved
        && writes.dom().subset_of(s.sessions[txn].held.dom())
}
pub open spec fn write_logical(logical: Map<int, Cell>, txn: int, writes: Map<int, Option<int>>) -> Map<int, Cell> {
    Map::new(logical.dom(), |k: int|
        if writes.contains_key(k) { Cell { value: writes[k], writer: txn } } else { logical[k] })
}
pub open spec fn admission(s: State, txn: int, key: int, grant: Grant) -> bool {
    s.sessions.contains_key(txn) && !s.sessions[txn].resolved
        && s.physical.contains_key((grant.owner,key))
        && replica(s,grant.owner,key).role is Serving
        && replica(s,grant.owner,key).epoch == grant.epoch
        && !s.sessions[txn].held.contains_key(key)
}
/// Local checks only: no participant consults the master's current decision.
pub open spec fn local_guard(s: State, g: nat, cmd: Command, owner: int) -> bool {
    let p = s.plans[g];
    forall|k: int| p.keys.contains(k) ==> {
        let r = replica(s,owner,k);
        match cmd {
            Command::Start => owner == p.dst && r.role is Empty && r.fence < g,
            Command::Freeze => owner == p.src && r.role is Serving && r.epoch == p.old[k].epoch && r.fence < g,
            Command::Final => owner == p.dst && r.role is Stage && r.fence == g && !r.terminal && r.round == 0,
            Command::Retire => owner == p.src && r.role is Frozen && r.fence == g && !r.terminal
                && drained(s,p.keys),
            Command::Commit => (owner == p.dst && r.role is Ready || owner == p.src && r.role is Retired)
                && r.fence == g && !r.terminal,
            Command::Abort => (owner == p.src || owner == p.dst)
                && (r.fence < g || r.fence == g && !r.terminal),
        }
    }
}
pub open spec fn command_replica(r: Replica, g: nat, cmd: Command, source: bool) -> Replica {
    match cmd {
        Command::Start => Replica { cell: empty_cell(), role: Role::Stage, epoch: g,
            fence: g, terminal: false, round: 0, covered: false },
        Command::Freeze => Replica { role: Role::Frozen, fence: g, terminal: false, ..r },
        Command::Final => Replica { round: 1, covered: false, ..r },
        Command::Retire => Replica { role: Role::Retired, ..r },
        Command::Commit => if source { Replica { cell: empty_cell(), role: Role::Empty, terminal: true, ..r } }
            else { Replica { role: Role::Serving, terminal: true, ..r } },
        Command::Abort => if source { Replica { role: Role::Serving, fence: g, terminal: true, ..r } }
            else { Replica { cell: empty_cell(), role: Role::Empty, fence: g, terminal: true, ..r } },
    }
}
pub open spec fn delivered(s: State, g: nat, cmd: Command, owner: int) -> State {
    let p = s.plans[g];
    let source = (cmd is Freeze || cmd is Retire || cmd is Commit || cmd is Abort)
        && owner == p.src && local_guard(s,g,cmd,p.src);
    let dest = (cmd is Start || cmd is Final || cmd is Commit || cmd is Abort)
        && owner == p.dst && local_guard(s,g,cmd,p.dst);
    State { physical: IMap::new(|q: (int,int)| s.physical.contains_key(q), |q: (int,int)|
        if p.keys.contains(q.1) && (q.0 == p.src && source || q.0 == p.dst && dest) {
            command_replica(s.physical[q],g,cmd,q.0 == p.src)
        } else { s.physical[q] }),
        certificates: if cmd is Retire && source { s.certificates.insert((g,Certificate::Retired)) }
            else if (cmd is Commit || cmd is Abort) && source { s.certificates.insert((g,Certificate::SourceDone)) }
            else if (cmd is Commit || cmd is Abort) && dest { s.certificates.insert((g,Certificate::DestinationDone)) }
            else { s.certificates }, ..s }
}
pub open spec fn copy_guard(s: State, packet: Packet) -> bool {
    let r = replica(s,s.plans[packet.generation].dst,packet.key);
    r.role is Stage && r.fence == packet.generation && !r.terminal && r.round == packet.round
}
pub open spec fn enabled(c: Constants, s: State, a: Action) -> bool {
    match a {
        Action::Open { txn } => txn >= 0 && !s.sessions.contains_key(txn),
        Action::Acquire { txn, key, grant } => admission(s,txn,key,grant),
        Action::Resolve { txn, writes } => can_resolve(s,txn,writes),
        Action::Release { txn, key } => s.sessions.contains_key(txn) && s.sessions[txn].resolved && s.sessions[txn].held.contains_key(key),
        Action::Begin { nonce, src, dst, table, lo, hi } => s.active is None && !s.outcomes.contains_key(nonce)
            && src != dst && c.shards.contains(src) && c.shards.contains(dst)
            && partition::valid_range(lo,hi) && selected(c,table,lo,hi) != Set::<int>::empty()
            && forall|k: int| selected(c,table,lo,hi).contains(k) ==> directory(s)[k].owner == src,
        Action::RequestFreeze => s.active is Some && s.phases[s.active.unwrap()] is Copy,
        Action::RequestFinal => s.active is Some && s.phases[s.active.unwrap()] is Freezing
            && s.received.contains((s.active.unwrap(),Certificate::Drained)),
        Action::RequestRetire => s.active is Some && s.phases[s.active.unwrap()] is Final
            && s.received.contains((s.active.unwrap(),Certificate::Ready)),
        Action::Commit => s.active is Some && s.phases[s.active.unwrap()] is Retiring
            && s.received.contains((s.active.unwrap(),Certificate::Retired)),
        Action::Abort => s.active is Some && before_decision(s,s.active.unwrap()),
        Action::Finish => s.active is Some && terminal(s.phases[s.active.unwrap()])
            && s.received.contains((s.active.unwrap(),Certificate::SourceDone))
            && s.received.contains((s.active.unwrap(),Certificate::DestinationDone)),
        Action::Deliver { generation, command, owner } => s.commands.contains((generation,command)),
        Action::Drain { generation } => s.plans.contains_key(generation)
            && drained(s,s.plans[generation].keys)
            && forall|k: int| s.plans[generation].keys.contains(k) ==> {
                let r = replica(s,s.plans[generation].src,k);
                r.role is Frozen && r.fence == generation && !r.terminal
            },
        Action::Seal { generation } => s.plans.contains_key(generation)
            && forall|k: int| s.plans[generation].keys.contains(k) ==> {
                let r = replica(s,s.plans[generation].dst,k);
                r.role is Stage && r.fence == generation && !r.terminal && r.round == 1 && r.covered
            },
        Action::Receive { generation, certificate } => s.certificates.contains((generation,certificate)),
        Action::Capture { generation, round, key } => s.plans.contains_key(generation)
            && s.plans[generation].keys.contains(key) && round <= 1
            && replica(s,s.plans[generation].src,key).fence <= generation
            && (replica(s,s.plans[generation].src,key).role is Serving
                || replica(s,s.plans[generation].src,key).role is Frozen)
            && (round == 0 || source_frozen(s,generation) && drained(s,s.plans[generation].keys)),
        Action::DeliverCopy { packet } => s.packets.contains(packet),
        Action::Cache { client, snapshot } => snapshot < s.directory.len(),
        Action::Reply { nonce } => s.outcomes.contains_key(nonce),
        Action::Stutter => true,
    }
}
pub open spec fn apply(c: Constants, s: State, a: Action) -> State {
    match a {
        Action::Open { txn } => State { sessions: s.sessions.insert(txn,Session { held: Map::empty(), resolved: false }), ..s },
        Action::Acquire { txn, key, grant } => State { sessions: s.sessions.insert(txn,
            Session { held: s.sessions[txn].held.insert(key,grant), ..s.sessions[txn] }), ..s },
        Action::Resolve { txn, writes } => State {
            logical: write_logical(s.logical,txn,writes),
            physical: IMap::new(|q: (int,int)| s.physical.contains_key(q), |q: (int,int)|
                if writes.contains_key(q.1) && s.sessions[txn].held[q.1].owner == q.0 {
                    Replica { cell: Cell { value: writes[q.1], writer: txn }, ..s.physical[q] }
                } else { s.physical[q] }),
            sessions: s.sessions.insert(txn,Session { resolved: true, ..s.sessions[txn] }), ..s },
        Action::Release { txn, key } => State { sessions: s.sessions.insert(txn,
            Session { held: s.sessions[txn].held.remove(key), ..s.sessions[txn] }), ..s },
        Action::Begin { nonce, src, dst, table, lo, hi } => {
            let g = s.next_generation;
            State { active: Some(g), next_generation: g+1,
                plans: s.plans.insert(g,Plan { nonce,src,dst,table,lo,hi,
                    keys: selected(c,table,lo,hi),old: directory(s) }),
                phases: s.phases.insert(g,Phase::Copy), commands: s.commands.insert((g,Command::Start)), ..s }
        },
        Action::RequestFreeze => State { phases: s.phases.insert(s.active.unwrap(),Phase::Freezing),
            commands: s.commands.insert((s.active.unwrap(),Command::Freeze)), ..s },
        Action::RequestFinal => State { phases: s.phases.insert(s.active.unwrap(),Phase::Final),
            commands: s.commands.insert((s.active.unwrap(),Command::Final)), ..s },
        Action::RequestRetire => State { phases: s.phases.insert(s.active.unwrap(),Phase::Retiring),
            commands: s.commands.insert((s.active.unwrap(),Command::Retire)), ..s },
        Action::Commit => {
            let g = s.active.unwrap(); let p = s.plans[g];
            State { phases: s.phases.insert(g,Phase::Committed), commands: s.commands.insert((g,Command::Commit)),
                outcomes: s.outcomes.insert(p.nonce,Outcome { generation: g, committed: true }),
                directory: s.directory.push(Map::new(directory(s).dom(), |k: int|
                    if p.keys.contains(k) { Grant { owner: p.dst, epoch: g } } else { directory(s)[k] })), ..s }
        },
        Action::Abort => { let g = s.active.unwrap(); State {
            phases: s.phases.insert(g,Phase::Aborted), commands: s.commands.insert((g,Command::Abort)),
            outcomes: s.outcomes.insert(s.plans[g].nonce,Outcome { generation: g, committed: false }), ..s } },
        Action::Finish => State { active: None, ..s },
        Action::Deliver { generation, command, owner } => delivered(s,generation,command,owner),
        Action::Drain { generation } => State { certificates: s.certificates.insert((generation,Certificate::Drained)), ..s },
        Action::Seal { generation } => { let p = s.plans[generation]; State {
            physical: IMap::new(|q: (int,int)| s.physical.contains_key(q), |q: (int,int)|
                if q.0 == p.dst && p.keys.contains(q.1) { Replica { role: Role::Ready, ..s.physical[q] } }
                else { s.physical[q] }), certificates: s.certificates.insert((generation,Certificate::Ready)), ..s } },
        Action::Receive { generation, certificate } => State { received: s.received.insert((generation,certificate)), ..s },
        Action::Capture { generation, round, key } => State { packets: s.packets.insert(Packet {
            generation,round,key,cell: replica(s,s.plans[generation].src,key).cell }), ..s },
        Action::DeliverCopy { packet } => if copy_guard(s,packet) {
            let q = (s.plans[packet.generation].dst,packet.key);
            State { physical: s.physical.insert(q,Replica { cell: packet.cell, covered: true, ..s.physical[q] }), ..s }
        } else { s },
        Action::Cache { client, snapshot } => State { views: s.views.insert(client,snapshot), ..s },
        Action::Reply { nonce } => State { replies: s.replies.insert(nonce,s.outcomes[nonce]), ..s },
        Action::Stutter => s,
    }
}

/// Total request delivery: rejected, stale, and duplicate requests stutter.
/// A terminal administrative retry retrieves its retained result via Reply.
pub open spec fn dispatch(c: Constants, s: State, a: Action) -> State {
    if enabled(c,s,a) { apply(c,s,a) } else { s }
}
pub open spec fn next(c: Constants, s: State, z: State) -> bool {
    exists|a: Action| z == #[trigger] dispatch(c,s,a)
}
pub open spec fn behavior(c: Constants, states: Seq<State>) -> bool {
    states.len() > 0 && states[0] == initial(c)
        && forall|i: int| 0 <= i < states.len()-1 ==> #[trigger] next(c,states[i],states[i+1])
}

} // verus!
