//! Administrative retention, not a production persistence adapter.
//!
//! Each admitted client has one outstanding slot. Its durable incarnation and
//! acknowledged sequence floor are distinct from the coordinator's generation.
//! A full slot, unknown client, or exhausted finite counter fails closed. Client
//! renewal advances an incarnation; deleting namespace high-water marks is not
//! an action. There is deliberately no timeout-based reclamation.
//!
//! This module is the administrative projection used by `protocol::Input`, the
//! external source-connected protocol. Projection labels are NOT admissions:
//! protocol couples Begin/Finish to its own evolving placement state and checks
//! placement geometry before allocating an administrative slot. Its Finish
//! projection checks terminal decision and BOTH receipts.
//! Checkpoint installs the complete materialized image before old recovery
//! dependencies become collectible. Replay below implements reconstruction;
//! neither replay correctness nor preservation of floors is a trusted premise.
//!
//! State.history is proof-only history. Durable is the materialized image:
//! no historical nonce map, terminal-generation tombstone set, or placement
//! trace is hidden in its space accounting. Finish's placement argument is filled
//! only from protocol's current ghost placement state, never from external input.
//! Control/copy gates are prefilters composed with the existing placement guards.
use vstd::prelude::*;
use crate::sharding_placement as p;
#[path = "sharding_retention_protocol.rs"]
pub mod protocol;

verus! {

pub struct RequestId { pub client: nat, pub incarnation: nat, pub sequence: nat }
pub struct Payload {
    pub src: int, pub dst: int, pub table: int, pub lo: int, pub hi: Option<int>,
}
pub struct Namespace { pub incarnation: nat, pub floor: nat }
pub struct Record {
    pub id: RequestId, pub payload: Payload, pub generation: nat,
    pub outcome: Option<p::Outcome>, pub acknowledged: bool,
}
pub struct Config {
    pub placement: p::Constants,
    pub clients: Set<nat>, pub counter_limit: nat, pub record_capacity: nat,
}
pub struct Durable {
    pub namespaces: Map<nat, Namespace>,
    pub records: Map<nat, Record>,
    pub next_generation: nat,
    pub control_floor: nat,
    pub recovery_floor: nat,
    pub active: Option<nat>,
    // A Grant includes BOTH owner and incarnation. No owner-only coalescing.
    pub routes: Map<int, p::Grant>,
}
pub struct State { pub disk: Durable, pub history: Map<RequestId, Record> }
pub enum Disposition {
    Fresh, Pending, Result { outcome: p::Outcome }, Conflict, Expired,
    UnknownNamespace, Busy, Exhausted,
}
/// Internal administrative projection labels. The external input type is
/// protocol::Input, which has no caller-supplied placement state.
pub enum Action {
    Begin { id: RequestId, payload: Payload },
    Finish { id: RequestId, placement: p::State },
    Acknowledge { id: RequestId, outcome: p::Outcome },
    Checkpoint,
    Compact { client: nat },
    Renew { client: nat },
}

pub open spec fn config_ok(c: Config) -> bool {
    p::constants_ok(c.placement) && c.counter_limit > 0
}
pub open spec fn initial(c: Config) -> State {
    State { disk: Durable {
        namespaces: Map::new(c.clients, |_client: nat| Namespace { incarnation: 0, floor: 0 }),
        records: Map::empty(), next_generation: 1, control_floor: 0,
        recovery_floor: 0, active: None, routes: p::directory(p::initial(c.placement)),
    }, history: Map::empty() }
}
pub open spec fn expired(d: Durable, id: RequestId) -> bool {
    d.namespaces.contains_key(id.client) && (
        id.incarnation < d.namespaces[id.client].incarnation
        || id.incarnation == d.namespaces[id.client].incarnation
            && id.sequence <= d.namespaces[id.client].floor)
}
pub open spec fn exact_slot(d: Durable, id: RequestId) -> bool {
    d.records.contains_key(id.client) && d.records[id.client].id == id
}
pub open spec fn disposition(c: Config, d: Durable, id: RequestId, payload: Payload) -> Disposition {
    if expired(d,id) { Disposition::Expired }
    else if !d.namespaces.contains_key(id.client)
        || id.incarnation != d.namespaces[id.client].incarnation { Disposition::UnknownNamespace }
    else if exact_slot(d,id) {
        let r = d.records[id.client];
        if r.payload != payload { Disposition::Conflict }
        else { match r.outcome {
            Some(outcome) => Disposition::Result { outcome },
            None => Disposition::Pending,
        } }
    } else if d.records.contains_key(id.client)
        || id.sequence != d.namespaces[id.client].floor + 1 || d.active is Some {
        Disposition::Busy
    } else if id.sequence > c.counter_limit || d.next_generation > c.counter_limit
        || d.records.len() >= c.record_capacity { Disposition::Exhausted }
    else { Disposition::Fresh }
}

/// Interface binding: request identity lives in the retained slot; the placement
/// nonce is the authority-allocated generation, never the user's reusable nonce.
/// Only the already-verified placement machine produces this authenticated
/// Finish input. Its exact payload, generation, outcome and receipts are checked.
pub open spec fn finish_ready(c: Config, d: Durable, id: RequestId, ps: p::State) -> bool {
    exact_slot(d,id) && d.active == Some(id.client)
    && d.records[id.client].outcome is None
    && ps.active == Some(d.records[id.client].generation)
    && ps.plans.contains_key(d.records[id.client].generation)
    && ps.plans[d.records[id.client].generation].nonce == d.records[id.client].generation as int
    && ps.plans[d.records[id.client].generation].src == d.records[id.client].payload.src
    && ps.plans[d.records[id.client].generation].dst == d.records[id.client].payload.dst
    && ps.plans[d.records[id.client].generation].table == d.records[id.client].payload.table
    && ps.plans[d.records[id.client].generation].lo == d.records[id.client].payload.lo
    && ps.plans[d.records[id.client].generation].hi == d.records[id.client].payload.hi
    && p::enabled(c.placement,ps,p::Action::Finish)
    && ps.outcomes.contains_key(d.records[id.client].generation as int)
    && ps.outcomes[d.records[id.client].generation as int] == (p::Outcome {
        generation: d.records[id.client].generation,
        committed: ps.phases[d.records[id.client].generation] is Committed,
    })
    && p::directory(ps).dom() == c.placement.keys
}
pub open spec fn collectible(d: Durable, client: nat) -> bool {
    d.records.contains_key(client) && d.records[client].outcome is Some
        && d.records[client].acknowledged
        && d.records[client].generation <= d.recovery_floor
        && d.active != Some(client)
}
pub open spec fn recovery_needed(d: Durable, client: nat) -> bool {
    d.records.contains_key(client) && d.records[client].generation > d.recovery_floor
}
pub open spec fn accepts_generation(d: Durable, generation: nat) -> bool {
    generation > d.control_floor && d.active is Some
        && d.records.contains_key(d.active.unwrap())
        && d.records[d.active.unwrap()].generation == generation
}
pub open spec fn accepts_control(d: Durable, generation: nat, _command: p::Command) -> bool {
    accepts_generation(d,generation)
}
pub open spec fn accepts_copy(d: Durable, packet: p::Packet) -> bool {
    accepts_generation(d,packet.generation)
}
pub open spec fn enabled(c: Config, d: Durable, a: Action) -> bool {
    match a {
        Action::Begin { id,payload } => disposition(c,d,id,payload) is Fresh,
        Action::Finish { id,placement } => finish_ready(c,d,id,placement),
        Action::Acknowledge { id,outcome } => exact_slot(d,id)
            && d.records[id.client].outcome == Some(outcome),
        Action::Checkpoint => true,
        Action::Compact { client } => collectible(d,client),
        Action::Renew { client } => d.namespaces.contains_key(client)
            && !d.records.contains_key(client)
            && d.namespaces[client].incarnation < c.counter_limit,
    }
}
pub open spec fn new_record(d: Durable, id: RequestId, payload: Payload) -> Record {
    Record { id,payload,generation: d.next_generation,outcome: None,acknowledged: false }
}
pub open spec fn finished_record(d: Durable, id: RequestId, ps: p::State) -> Record {
    Record { outcome: Some(ps.outcomes[d.records[id.client].generation as int]),
        ..d.records[id.client] }
}
pub open spec fn apply_disk(d: Durable, a: Action) -> Durable {
    match a {
        Action::Begin { id,payload } => Durable {
            records: d.records.insert(id.client,new_record(d,id,payload)),
            next_generation: d.next_generation + 1,active: Some(id.client),..d },
        Action::Finish { id,placement } => Durable {
            records: d.records.insert(id.client,finished_record(d,id,placement)),
            active: None,control_floor: d.records[id.client].generation,
            routes: p::directory(placement),..d },
        Action::Acknowledge { id,outcome: _ } => Durable {
            records: d.records.insert(id.client,Record { acknowledged: true,..d.records[id.client] }),..d },
        // This successful commit installs this COMPLETE image as the new recovery
        // checkpoint. No old log/checkpoint can be discarded before installation.
        Action::Checkpoint => Durable { recovery_floor: d.control_floor,..d },
        Action::Compact { client } => Durable {
            namespaces: d.namespaces.insert(client,Namespace {
                floor: d.records[client].id.sequence,..d.namespaces[client] }),
            records: d.records.remove(client),..d },
        Action::Renew { client } => Durable {
            namespaces: d.namespaces.insert(client,Namespace {
                incarnation: d.namespaces[client].incarnation + 1,floor: 0 }),..d },
    }
}
pub open spec fn apply(s: State, a: Action) -> State {
    State { disk: apply_disk(s.disk,a), history: match a {
        Action::Begin { id,payload } => s.history.insert(id,new_record(s.disk,id,payload)),
        Action::Finish { id,placement } => s.history.insert(id,finished_record(s.disk,id,placement)),
        Action::Acknowledge { id,outcome: _ } =>
            s.history.insert(id,Record { acknowledged: true,..s.disk.records[id.client] }),
        _ => s.history,
    } }
}
pub open spec fn dispatch(c: Config, s: State, a: Action) -> State {
    if enabled(c,s.disk,a) { apply(s,a) } else { s }
}

pub open spec fn disk_inv(c: Config, d: Durable) -> bool {
    &&& config_ok(c)
    &&& d.namespaces.dom() == c.clients
    &&& d.records.dom().subset_of(c.clients)
    &&& d.records.len() <= c.record_capacity
    &&& 0 < d.next_generation <= c.counter_limit + 1
    &&& d.recovery_floor <= d.control_floor < d.next_generation
    &&& d.routes.dom() == c.placement.keys
    &&& forall|client: nat| d.namespaces.contains_key(client) ==>
        d.namespaces[client].incarnation <= c.counter_limit
        && d.namespaces[client].floor <= c.counter_limit
    &&& forall|client: nat| d.records.contains_key(client) ==> {
        let r = d.records[client];
        &&& r.id.client == client
        &&& r.id.incarnation == d.namespaces[client].incarnation
        &&& r.id.sequence == d.namespaces[client].floor + 1
        &&& r.id.sequence <= c.counter_limit
        &&& 0 < r.generation < d.next_generation
        &&& (r.outcome is None <==> d.active == Some(client))
        &&& (r.outcome is None ==> r.generation == d.control_floor + 1)
        &&& (r.outcome is Some ==> r.generation <= d.control_floor
            && r.outcome.unwrap().generation == r.generation)
        &&& (r.acknowledged ==> r.outcome is Some)
    }
    &&& match d.active {
        Some(client) => d.records.contains_key(client)
            && d.records[client].outcome is None
            && d.next_generation == d.control_floor + 2,
        None => d.next_generation == d.control_floor + 1,
    }
}
pub open spec fn history_record_ok(d: Durable, id: RequestId, r: Record) -> bool {
    &&& r.id == id
    &&& d.namespaces.contains_key(id.client)
    &&& 0 < r.generation < d.next_generation
    &&& 0 < id.sequence
    &&& id.incarnation <= d.namespaces[id.client].incarnation
    &&& (r.outcome is Some ==> r.outcome.unwrap().generation == r.generation
        && r.generation <= d.control_floor)
    &&& (r.acknowledged ==> r.outcome is Some)
    &&& if exact_slot(d,id) { d.records[id.client] == r }
        else { expired(d,id) && r.outcome is Some && r.acknowledged }
}
pub open spec fn inv(c: Config, s: State) -> bool {
    &&& disk_inv(c,s.disk)
    &&& forall|id: RequestId| s.history.contains_key(id) ==>
        history_record_ok(s.disk,id,s.history[id])
    &&& forall|client: nat| s.disk.records.contains_key(client) ==>
        s.history.contains_key(s.disk.records[client].id)
        && s.history[s.disk.records[client].id] == s.disk.records[client]
}

pub proof fn lemma_initial(c: Config)
    requires config_ok(c)
    ensures inv(c,initial(c))
{
    let d = initial(c).disk;
    assert(d.namespaces.dom() =~= c.clients);
    assert(d.routes.dom() =~= c.placement.keys);
    assert forall|client: nat| d.namespaces.contains_key(client) implies
        d.namespaces[client].incarnation <= c.counter_limit
        && d.namespaces[client].floor <= c.counter_limit by {}
}

pub proof fn lemma_expired_monotone(c: Config, d: Durable, a: Action, id: RequestId)
    requires disk_inv(c,d), enabled(c,d,a), expired(d,id)
    ensures expired(apply_disk(d,a),id)
{
    match a {
        Action::Compact { client } => {
            assert(d.records.contains_key(client));
            assert(d.records[client].id.sequence == d.namespaces[client].floor + 1);
        },
        Action::Renew { client } => {},
        _ => {},
    }
}

pub proof fn lemma_disk_step(c: Config, d: Durable, a: Action)
    requires disk_inv(c,d), enabled(c,d,a)
    ensures disk_inv(c,apply_disk(d,a)),
        apply_disk(d,a).next_generation >= d.next_generation,
        apply_disk(d,a).control_floor >= d.control_floor,
        apply_disk(d,a).recovery_floor >= d.recovery_floor
{
    let z = apply_disk(d,a);
    match a {
        Action::Begin { id,payload } => {
            assert(!d.records.contains_key(id.client));
            assert(d.active is None);
            assert(z.records.len() == d.records.len() + 1);
            assert forall|client: nat| z.records.contains_key(client) implies {
                let r = z.records[client];
                &&& r.id.client == client
                &&& r.id.incarnation == z.namespaces[client].incarnation
                &&& r.id.sequence == z.namespaces[client].floor + 1
                &&& r.id.sequence <= c.counter_limit
                &&& 0 < r.generation < z.next_generation
                &&& (r.outcome is None <==> z.active == Some(client))
                &&& (r.outcome is None ==> r.generation == z.control_floor + 1)
                &&& (r.outcome is Some ==> r.generation <= z.control_floor
                    && r.outcome.unwrap().generation == r.generation)
                &&& (r.acknowledged ==> r.outcome is Some)
            } by { if client != id.client { assert(d.records.contains_key(client)); } }
        },
        Action::Finish { id,placement } => {
            assert(d.records[id.client].generation == d.control_floor + 1);
            assert forall|client: nat| z.records.contains_key(client) implies {
                let r = z.records[client];
                &&& r.id.client == client
                &&& r.id.incarnation == z.namespaces[client].incarnation
                &&& r.id.sequence == z.namespaces[client].floor + 1
                &&& r.id.sequence <= c.counter_limit
                &&& 0 < r.generation < z.next_generation
                &&& (r.outcome is None <==> z.active == Some(client))
                &&& (r.outcome is None ==> r.generation == z.control_floor + 1)
                &&& (r.outcome is Some ==> r.generation <= z.control_floor
                    && r.outcome.unwrap().generation == r.generation)
                &&& (r.acknowledged ==> r.outcome is Some)
            } by { assert(d.records.contains_key(client)); }
        },
        Action::Acknowledge { id,outcome } => {
            assert forall|client: nat| z.records.contains_key(client) implies {
                let r = z.records[client];
                &&& r.id.client == client
                &&& r.id.incarnation == z.namespaces[client].incarnation
                &&& r.id.sequence == z.namespaces[client].floor + 1
                &&& r.id.sequence <= c.counter_limit
                &&& 0 < r.generation < z.next_generation
                &&& (r.outcome is None <==> z.active == Some(client))
                &&& (r.outcome is None ==> r.generation == z.control_floor + 1)
                &&& (r.outcome is Some ==> r.generation <= z.control_floor
                    && r.outcome.unwrap().generation == r.generation)
                &&& (r.acknowledged ==> r.outcome is Some)
            } by { assert(d.records.contains_key(client)); }
        },
        Action::Compact { client } => {
            assert(z.records.len() + 1 == d.records.len());
            assert forall|other: nat| z.records.contains_key(other) implies
                z.namespaces[other] == d.namespaces[other]
                && z.records[other] == d.records[other] by { assert(other != client); }
        },
        Action::Renew { client } => {
            assert forall|other: nat| z.records.contains_key(other) implies
                z.namespaces[other] == d.namespaces[other]
                && z.records[other] == d.records[other] by { assert(other != client); }
        },
        Action::Checkpoint => {},
    }
    assert(z.records.dom().subset_of(c.clients));
    assert forall|client: nat| z.namespaces.contains_key(client) implies
        z.namespaces[client].incarnation <= c.counter_limit
        && z.namespaces[client].floor <= c.counter_limit by {
        assert(d.namespaces.contains_key(client));
    }
    assert(z.namespaces.dom() == c.clients);
    assert(z.records.len() <= c.record_capacity);
    assert(0 < z.next_generation <= c.counter_limit + 1);
    assert(z.recovery_floor <= z.control_floor < z.next_generation);
    assert(z.routes.dom() == c.placement.keys);
    assert forall|client: nat| z.records.contains_key(client) implies {
        let r = z.records[client];
        &&& r.id.client == client
        &&& r.id.incarnation == z.namespaces[client].incarnation
        &&& r.id.sequence == z.namespaces[client].floor + 1
        &&& r.id.sequence <= c.counter_limit
        &&& 0 < r.generation < z.next_generation
        &&& (r.outcome is None <==> z.active == Some(client))
        &&& (r.outcome is None ==> r.generation == z.control_floor + 1)
        &&& (r.outcome is Some ==> r.generation <= z.control_floor
            && r.outcome.unwrap().generation == r.generation)
        &&& (r.acknowledged ==> r.outcome is Some)
    } by {
        match a {
            Action::Compact { client: removed } => {
                assert(client != removed);
                assert(d.records.contains_key(client));
            },
            Action::Renew { client: renewed } => {
                assert(client != renewed);
                assert(d.records.contains_key(client));
            },
            Action::Checkpoint => { assert(d.records.contains_key(client)); },
            _ => {},
        }
    }
    assert(match z.active {
        Some(client) => z.records.contains_key(client) && z.records[client].outcome is None
            && z.next_generation == z.control_floor + 2,
        None => z.next_generation == z.control_floor + 1,
    }) by {
        if d.active is Some {
            let client = d.active.unwrap();
            assert(d.records.contains_key(client));
            assert(d.records[client].outcome is None);
            match a {
                Action::Compact { client: removed } => { assert(client != removed); },
                Action::Renew { client: renewed } => { assert(client != renewed); },
                Action::Acknowledge { id,outcome } => { assert(client != id.client); },
                _ => {},
            }
        }
    }
}

pub proof fn lemma_step(c: Config, s: State, a: Action)
    requires inv(c,s), enabled(c,s.disk,a)
    ensures inv(c,apply(s,a)),
        s.history.dom().subset_of(apply(s,a).history.dom()),
        forall|id: RequestId| s.history.contains_key(id) ==>
            apply(s,a).history[id].payload == s.history[id].payload
            && apply(s,a).history[id].generation == s.history[id].generation
            && (s.history[id].outcome is Some ==>
                apply(s,a).history[id].outcome == s.history[id].outcome)
            && (s.history[id].acknowledged ==> apply(s,a).history[id].acknowledged)
{
    lemma_disk_step(c,s.disk,a);
    let z = apply(s,a);
    match a {
        Action::Begin { id,payload } => {
            if s.history.contains_key(id) {
                assert(history_record_ok(s.disk,id,s.history[id]));
                assert(!exact_slot(s.disk,id));
                assert(expired(s.disk,id));
                assert(false);
            }
        },
        Action::Finish { id,placement } => {
            assert(s.history.contains_key(id));
            assert(s.history[id] == s.disk.records[id.client]);
        },
        Action::Acknowledge { id,outcome } => {
            assert(s.history.contains_key(id));
            assert(s.history[id] == s.disk.records[id.client]);
        },
        _ => {},
    }
    assert forall|id: RequestId| z.history.contains_key(id) implies
        history_record_ok(z.disk,id,z.history[id]) by {
        if s.history.contains_key(id) {
            assert(history_record_ok(s.disk,id,s.history[id]));
            if !exact_slot(s.disk,id) { lemma_expired_monotone(c,s.disk,a,id); }
        }
        match a {
            Action::Begin { id: fresh,payload } => {
                if id != fresh && s.history.contains_key(id) {
                    if id.client == fresh.client {
                        assert(!exact_slot(s.disk,id));
                        lemma_expired_monotone(c,s.disk,a,id);
                    }
                }
            },
            Action::Compact { client } => {
                if id.client == client && exact_slot(s.disk,id) {
                    assert(id == s.disk.records[client].id);
                    assert(s.disk.records[client].acknowledged);
                }
            },
            Action::Renew { client } => {
                if id.client == client { assert(!exact_slot(s.disk,id)); }
            },
            _ => {},
        }
    }
    assert forall|client: nat| z.disk.records.contains_key(client) implies
        z.history.contains_key(z.disk.records[client].id)
        && z.history[z.disk.records[client].id] == z.disk.records[client] by {
        match a {
            Action::Begin { id,payload } => {
                if client != id.client { assert(s.disk.records.contains_key(client)); }
            },
            _ => { assert(s.disk.records.contains_key(client)); },
        }
    }
    assert forall|id: RequestId| s.history.contains_key(id) implies
        z.history[id].payload == s.history[id].payload
        && z.history[id].generation == s.history[id].generation
        && (s.history[id].outcome is Some ==> z.history[id].outcome == s.history[id].outcome)
        && (s.history[id].acknowledged ==> z.history[id].acknowledged) by {
        assert(history_record_ok(s.disk,id,s.history[id]));
        match a {
            Action::Begin { id: fresh,payload } => { assert(id != fresh); },
            Action::Finish { id: done,placement } => {
                if id == done { assert(s.history[id].outcome is None); }
            },
            Action::Acknowledge { id: ack,outcome } => {
                if id == ack { assert(s.history[id] == s.disk.records[id.client]); }
            },
            _ => {},
        }
    }
}

pub open spec fn behavior(c: Config, states: Seq<State>, actions: Seq<Action>) -> bool {
    states.len() == actions.len() + 1 && states[0] == initial(c)
        && forall|i: int| 0 <= i < actions.len() ==>
            states[i+1] == dispatch(c,states[i],actions[i])
}
pub proof fn theorem_history(c: Config, states: Seq<State>, actions: Seq<Action>, i: nat)
    requires config_ok(c), behavior(c,states,actions), i < states.len()
    ensures inv(c,states[i as int])
    decreases i
{
    if i == 0 { lemma_initial(c); }
    else {
        theorem_history(c,states,actions,(i-1) as nat);
        assert(states[i as int] == dispatch(c,states[i-1],actions[i-1]));
        if enabled(c,states[i-1].disk,actions[i-1]) {
            lemma_step(c,states[i-1],actions[i-1]);
        }
    }
}
pub proof fn theorem_outcome_immutable(c: Config, states: Seq<State>, actions: Seq<Action>,
    i: nat, j: nat, id: RequestId)
    requires config_ok(c), behavior(c,states,actions), i <= j < states.len(),
        states[i as int].history.contains_key(id), states[i as int].history[id].outcome is Some
    ensures states[j as int].history.contains_key(id),
        states[j as int].history[id].outcome == states[i as int].history[id].outcome,
        states[j as int].history[id].payload == states[i as int].history[id].payload,
        states[j as int].history[id].generation == states[i as int].history[id].generation
    decreases j-i
{
    if i < j {
        theorem_outcome_immutable(c,states,actions,i,(j-1) as nat,id);
        theorem_history(c,states,actions,(j-1) as nat);
        assert(states[j as int] == dispatch(c,states[j-1],actions[j-1]));
        if enabled(c,states[j-1].disk,actions[j-1]) {
            lemma_step(c,states[j-1],actions[j-1]);
        }
    }
}

pub proof fn theorem_retained_result(c: Config, s: State, id: RequestId)
    requires inv(c,s), s.history.contains_key(id), s.history[id].outcome is Some,
        !s.history[id].acknowledged
    ensures exact_slot(s.disk,id),
        disposition(c,s.disk,id,s.history[id].payload) == (Disposition::Result {
            outcome: s.history[id].outcome.unwrap() }),
        forall|payload: Payload| payload != s.history[id].payload ==>
            disposition(c,s.disk,id,payload) is Conflict
{
    assert(history_record_ok(s.disk,id,s.history[id]));
    assert(s.disk.records.contains_key(id.client));
    assert(!expired(s.disk,id));
}
pub proof fn theorem_forgotten(c: Config, s: State, id: RequestId, payload: Payload,
    command: p::Command, packet: p::Packet)
    requires inv(c,s), s.history.contains_key(id), !exact_slot(s.disk,id),
        packet.generation == s.history[id].generation
    ensures s.history[id].acknowledged, s.history[id].outcome is Some,
        disposition(c,s.disk,id,payload) is Expired,
        !enabled(c,s.disk,Action::Begin { id,payload }),
        !accepts_control(s.disk,s.history[id].generation,command),
        !accepts_copy(s.disk,packet)
{
    assert(history_record_ok(s.disk,id,s.history[id]));
}
pub proof fn theorem_collection(c: Config, s: State, client: nat)
    requires inv(c,s), enabled(c,s.disk,Action::Compact { client })
    ensures inv(c,apply(s,Action::Compact { client })),
        apply(s,Action::Compact { client }).disk.records.len() + 1 == s.disk.records.len(),
        !apply(s,Action::Compact { client }).disk.records.contains_key(client),
        apply(s,Action::Compact { client }).disk.routes == s.disk.routes,
        apply(s,Action::Compact { client }).disk.control_floor == s.disk.control_floor,
        apply(s,Action::Compact { client }).disk.recovery_floor == s.disk.recovery_floor,
        expired(apply(s,Action::Compact { client }).disk,s.disk.records[client].id),
        s.disk.active != Some(client), !recovery_needed(s.disk,client)
{
    lemma_step(c,s,Action::Compact { client });
}
pub proof fn theorem_no_active_or_checkpoint_collection(c: Config, d: Durable, client: nat)
    requires disk_inv(c,d), d.active == Some(client) || recovery_needed(d,client)
    ensures !enabled(c,d,Action::Compact { client })
{}

/// Exact slot accounting: one retained record per admitted namespace, not per
/// historical generation. Silent clients keep their slot and block further
/// requests; unknown clients are not silently registered. Routing/lifecycle
/// metadata persists even when no requests remain. No fixed global bound is
/// claimed if the configured namespace/key sets themselves grow without bound.
pub open spec fn materialized_cells(d: Durable) -> nat {
    d.records.len() + d.namespaces.len() + d.routes.len() + 4
}
pub proof fn theorem_space(c: Config, s: State)
    requires inv(c,s)
    ensures s.disk.records.len() <= c.record_capacity,
        s.disk.records.dom().subset_of(c.clients),
        materialized_cells(s.disk) <= c.record_capacity + c.clients.len() + c.placement.keys.len() + 4,
        forall|client: nat| s.disk.records.contains_key(client) ==>
            s.disk.active == Some(client)
            || !s.disk.records[client].acknowledged
            || recovery_needed(s.disk,client) || collectible(s.disk,client)
{
    assert(s.disk.namespaces.dom() == c.clients);
    assert(s.disk.routes.dom() == c.placement.keys);
}
pub proof fn theorem_collector_drained(c: Config, s: State)
    requires inv(c,s), forall|client: nat| !collectible(s.disk,client)
    ensures forall|client: nat| s.disk.records.contains_key(client) ==>
        s.disk.active == Some(client) || !s.disk.records[client].acknowledged
        || recovery_needed(s.disk,client)
{
    theorem_space(c,s);
}
pub proof fn theorem_capacity_fail_closed(c: Config, s: State, id: RequestId, payload: Payload)
    requires inv(c,s), !exact_slot(s.disk,id),
        s.disk.next_generation > c.counter_limit || id.sequence > c.counter_limit
        || s.disk.records.len() >= c.record_capacity
    ensures !enabled(c,s.disk,Action::Begin { id,payload }),
        dispatch(c,s,Action::Begin { id,payload }) == s
{}
pub proof fn theorem_incarnation_boundary(d: Durable, client: nat, key: int, other: int)
    requires d.routes.contains_key(key), d.routes.contains_key(other),
        d.routes[key].epoch != d.routes[other].epoch
    ensures apply_disk(d,Action::Compact { client }).routes[key] !=
        apply_disk(d,Action::Compact { client }).routes[other],
        apply_disk(d,Action::Renew { client }).routes[key] == d.routes[key],
        apply_disk(d,Action::Checkpoint).routes[key] == d.routes[key]
{}

/// The recovery algorithm uses ONLY the snapshot image and committed suffix.
/// It does not consult proof history or resurrect a discarded nonce map.
pub open spec fn replay(c: Config, snapshot: Durable, suffix: Seq<Action>) -> Durable
    decreases suffix.len()
{
    if suffix.len() == 0 { snapshot }
    else {
        let before = replay(c,snapshot,suffix.drop_last());
        if enabled(c,before,suffix.last()) { apply_disk(before,suffix.last()) } else { before }
    }
}
pub proof fn theorem_replay(c: Config, snapshot: Durable, suffix: Seq<Action>)
    requires disk_inv(c,snapshot)
    ensures disk_inv(c,replay(c,snapshot,suffix)),
        replay(c,snapshot,suffix).next_generation >= snapshot.next_generation,
        replay(c,snapshot,suffix).control_floor >= snapshot.control_floor,
        replay(c,snapshot,suffix).recovery_floor >= snapshot.recovery_floor,
        forall|id: RequestId| expired(snapshot,id) ==> expired(replay(c,snapshot,suffix),id)
    decreases suffix.len()
{
    if suffix.len() > 0 {
        theorem_replay(c,snapshot,suffix.drop_last());
        let d = replay(c,snapshot,suffix.drop_last());
        if enabled(c,d,suffix.last()) {
            lemma_disk_step(c,d,suffix.last());
            assert forall|id: RequestId| expired(snapshot,id) implies
                expired(replay(c,snapshot,suffix),id) by {
                lemma_expired_monotone(c,d,suffix.last(),id);
            }
        }
    }
}
pub proof fn theorem_snapshot_suffix(c: Config, states: Seq<State>, actions: Seq<Action>,
    cut: nat, end: nat)
    requires config_ok(c), behavior(c,states,actions), cut <= end < states.len()
    ensures replay(c,states[cut as int].disk,actions.subrange(cut as int,end as int)) == states[end as int].disk
    decreases end-cut
{
    if cut == end {
        assert(actions.subrange(cut as int,end as int).len() == 0);
    } else {
        theorem_snapshot_suffix(c,states,actions,cut,(end-1) as nat);
        let suffix = actions.subrange(cut as int,end as int);
        assert(suffix.drop_last() =~= actions.subrange(cut as int,end as int-1));
        assert(suffix.last() == actions[end-1]);
        assert(states[end as int] == dispatch(c,states[end-1],actions[end-1]));
    }
}
pub proof fn theorem_restart_rejects(c: Config, snapshot: Durable, suffix: Seq<Action>,
    id: RequestId, payload: Payload, generation: nat, command: p::Command, packet: p::Packet)
    requires disk_inv(c,snapshot), expired(snapshot,id), generation <= snapshot.control_floor,
        packet.generation == generation
    ensures disposition(c,replay(c,snapshot,suffix),id,payload) is Expired,
        !enabled(c,replay(c,snapshot,suffix),Action::Begin { id,payload }),
        !accepts_control(replay(c,snapshot,suffix),generation,command),
        !accepts_copy(replay(c,snapshot,suffix),packet)
{
    theorem_replay(c,snapshot,suffix);
}

/// Connect the materialized completion transition to the placement proof, rather
/// than accepting an arbitrary engine completion bit as administrative cleanup.
pub proof fn theorem_finish_interface(c: Config, d: Durable, id: RequestId, ps: p::State)
    requires disk_inv(c,d), finish_ready(c,d,id,ps), p::inv(c.placement,ps)
    ensures p::enabled(c.placement,ps,p::Action::Finish),
        p::terminal(ps.phases[d.records[id.client].generation]),
        ps.received.contains((d.records[id.client].generation,p::Certificate::SourceDone)),
        ps.received.contains((d.records[id.client].generation,p::Certificate::DestinationDone)),
        p::apply(c.placement,ps,p::Action::Finish).active is None,
        apply_disk(d,Action::Finish { id,placement: ps }).routes ==
            p::directory(p::apply(c.placement,ps,p::Action::Finish)),
        apply_disk(d,Action::Finish { id,placement: ps }).records[id.client].outcome ==
            Some(ps.outcomes[d.records[id.client].generation as int])
{
    p::lemma_step(c.placement,ps,p::Action::Finish);
}

pub proof fn theorem_forgotten_history(c: Config, states: Seq<State>, actions: Seq<Action>,
    cut: nat, end: nat, id: RequestId, payload: Payload, command: p::Command, packet: p::Packet)
    requires config_ok(c), behavior(c,states,actions), cut <= end < states.len(),
        states[cut as int].history.contains_key(id), !exact_slot(states[cut as int].disk,id),
        packet.generation == states[cut as int].history[id].generation
    ensures disposition(c,states[end as int].disk,id,payload) is Expired,
        !enabled(c,states[end as int].disk,Action::Begin { id,payload }),
        !accepts_control(states[end as int].disk,states[cut as int].history[id].generation,command),
        !accepts_copy(states[end as int].disk,packet)
{
    theorem_history(c,states,actions,cut);
    assert(history_record_ok(states[cut as int].disk,id,states[cut as int].history[id]));
    theorem_snapshot_suffix(c,states,actions,cut,end);
    theorem_restart_rejects(c,states[cut as int].disk,actions.subrange(cut as int,end as int),
        id,payload,states[cut as int].history[id].generation,command,packet);
}

pub open spec fn witness_config() -> Config {
    Config {
        placement: p::Constants {
            keys: Set::empty().insert(0int),shards: Set::empty().insert(0int).insert(1int),
            table: Map::empty().insert(0int,0int),
            coordinate: Map::empty().insert(0int,0int),
            owners: Map::empty().insert(0int,0int),
        },
        clients: Set::empty().insert(7nat),counter_limit: 3,record_capacity: 1,
    }
}
pub open spec fn witness_id(sequence: nat) -> RequestId {
    RequestId { client: 7,incarnation: 0,sequence }
}
pub open spec fn witness_payload() -> Payload {
    Payload { src: 0,dst: 1,table: 0,lo: 0,hi: None }
}
pub open spec fn witness_placement_actions() -> Seq<p::Action> {
    seq![
        p::Action::Begin { nonce: 1,src: 0,dst: 1,table: 0,lo: 0,hi: None },
        p::Action::Abort,
        p::Action::Deliver { generation: 1,command: p::Command::Abort,owner: 0 },
        p::Action::Deliver { generation: 1,command: p::Command::Abort,owner: 1 },
        p::Action::Receive { generation: 1,certificate: p::Certificate::SourceDone },
        p::Action::Receive { generation: 1,certificate: p::Certificate::DestinationDone },
    ]
}
pub open spec fn witness_placement_at(i: nat) -> p::State
    decreases i
{
    if i == 0 { p::initial(witness_config().placement) }
    else { p::apply(witness_config().placement,witness_placement_at((i-1) as nat),
        witness_placement_actions()[i-1]) }
}
pub proof fn lemma_witness_placement()
    ensures p::inv(witness_config().placement,witness_placement_at(6)),
        p::enabled(witness_config().placement,witness_placement_at(6),p::Action::Finish),
        witness_placement_at(6).outcomes[1int] == (p::Outcome { generation: 1,committed: false }),
        witness_placement_at(6).plans[1nat].nonce == 1,
        witness_placement_at(6).next_generation == 2,
        p::enabled(witness_config().placement,
            p::apply(witness_config().placement,witness_placement_at(6),p::Action::Finish),
            p::Action::Begin { nonce: 2,src: 0,dst: 1,table: 0,lo: 0,hi: None }),
        witness_placement_at(0) == p::initial(witness_config().placement),
        forall|i: int| 0 <= i < 6 ==> p::enabled(witness_config().placement,
            witness_placement_at(i as nat),witness_placement_actions()[i])
{
    let c = witness_config().placement;
    assert(p::constants_ok(c));
    p::lemma_init(c);
    reveal_with_fuel(witness_placement_at,7);
    assert(p::enabled(c,witness_placement_at(0),witness_placement_actions()[0]));
    p::lemma_step(c,witness_placement_at(0),witness_placement_actions()[0]);
    assert(p::enabled(c,witness_placement_at(1),witness_placement_actions()[1]));
    p::lemma_step(c,witness_placement_at(1),witness_placement_actions()[1]);
    assert(p::local_guard(witness_placement_at(2),1,p::Command::Abort,0));
    assert(p::enabled(c,witness_placement_at(2),witness_placement_actions()[2]));
    p::lemma_step(c,witness_placement_at(2),witness_placement_actions()[2]);
    assert(p::local_guard(witness_placement_at(3),1,p::Command::Abort,1));
    assert(p::enabled(c,witness_placement_at(3),witness_placement_actions()[3]));
    p::lemma_step(c,witness_placement_at(3),witness_placement_actions()[3]);
    assert(p::enabled(c,witness_placement_at(4),witness_placement_actions()[4]));
    p::lemma_step(c,witness_placement_at(4),witness_placement_actions()[4]);
    assert(p::enabled(c,witness_placement_at(5),witness_placement_actions()[5]));
    p::lemma_step(c,witness_placement_at(5),witness_placement_actions()[5]);
    assert forall|i: int| 0 <= i < 6 implies
        p::enabled(c,witness_placement_at(i as nat),witness_placement_actions()[i]) by {
        if i == 0 {} else if i == 1 {} else if i == 2 {} else if i == 3 {}
        else if i == 4 {} else { assert(i == 5); }
    }
}
pub proof fn lemma_witness_placement_step(i: nat)
    requires i < 6
    ensures p::enabled(witness_config().placement,witness_placement_at(i),
            witness_placement_actions()[i as int]),
        witness_placement_at(i+1) == p::apply(witness_config().placement,
            witness_placement_at(i),witness_placement_actions()[i as int])
{
    lemma_witness_placement();
    reveal_with_fuel(witness_placement_at,1);
}
pub open spec fn witness_actions() -> Seq<Action> {
    seq![
        Action::Begin { id: witness_id(1),payload: witness_payload() },
        Action::Finish { id: witness_id(1),placement: witness_placement_at(6) },
        Action::Acknowledge { id: witness_id(1),outcome: p::Outcome { generation: 1,committed: false } },
        Action::Checkpoint,
        Action::Compact { client: 7 },
        // A delayed administrative retry, deliberately NOT enabled.
        Action::Begin { id: witness_id(1),payload: witness_payload() },
        Action::Begin { id: witness_id(2),payload: witness_payload() },
    ]
}
pub proof fn theorem_enabled_witness() -> (states: Seq<State>)
    ensures behavior(witness_config(),states,witness_actions()), states.len() == 8,
        forall|i: int| 0 <= i < 7 && i != 5 ==>
            enabled(witness_config(),states[i].disk,witness_actions()[i]),
        states[5].disk.records.len() + 1 == states[4].disk.records.len(),
        states[5].disk.records.len() == 0,
        disposition(witness_config(),states[5].disk,witness_id(1),witness_payload()) is Expired,
        !enabled(witness_config(),states[5].disk,witness_actions()[5]),
        states[6] == states[5],
        !accepts_control(states[7].disk,1,p::Command::Abort),
        !accepts_copy(states[7].disk,p::Packet {
            generation: 1,round: 0,key: 0,cell: p::empty_cell() }),
        states[7].disk.records[7nat].id == witness_id(2),
        states[7].disk.records[7nat].generation == 2,
        states[7].disk.active == Some(7nat),
        inv(witness_config(),states[7])
{
    hide(witness_placement_at);
    hide(inv);
    hide(disk_inv);
    hide(enabled);
    hide(apply);
    hide(p::inv);
    hide(protocol::step);
    hide(protocol::apply);
    hide(protocol::enabled);
    let c = witness_config();
    assert(config_ok(c));
    let full = protocol::theorem_enabled_witness();
    assert(full[6].administrative == full[1].administrative);
    let states = seq![full[0].administrative,full[1].administrative,
        full[7].administrative,full[8].administrative,full[9].administrative,
        full[10].administrative,full[11].administrative,full[14].administrative];
    protocol::theorem_history(c,full,protocol::witness_inputs(),14);
    protocol::theorem_history(c,full,protocol::witness_inputs(),9);
    protocol::theorem_history(c,full,protocol::witness_inputs(),10);
    protocol::lemma_admitted_projection(c,full[0],protocol::witness_inputs()[0]);
    protocol::lemma_admitted_projection(c,full[6],protocol::witness_inputs()[6]);
    protocol::lemma_admitted_projection(c,full[7],protocol::witness_inputs()[7]);
    protocol::lemma_admitted_projection(c,full[8],protocol::witness_inputs()[8]);
    protocol::lemma_admitted_projection(c,full[9],protocol::witness_inputs()[9]);
    protocol::lemma_admitted_projection(c,full[13],protocol::witness_inputs()[13]);
    assert(full[10].administrative.disk.records.len() + 1
        == full[9].administrative.disk.records.len()) by {
        theorem_collection(c,full[9].administrative,7);
    }
    theorem_forgotten(c,full[10].administrative,witness_id(1),witness_payload(),
        p::Command::Abort,p::Packet { generation: 1,round: 0,key: 0,cell: p::empty_cell() });
    theorem_forgotten(c,full[14].administrative,witness_id(1),witness_payload(),
        p::Command::Abort,p::Packet { generation: 1,round: 0,key: 0,cell: p::empty_cell() });
    assert forall|i: int| 0 <= i < 7 implies
        states[i+1] == dispatch(c,states[i],witness_actions()[i]) by {
        if i == 0 {
            assert(full[1] == protocol::step(c,full[0],protocol::witness_inputs()[0]));
        } else if i == 1 {
            assert(full[7] == protocol::step(c,full[6],protocol::witness_inputs()[6]));
        } else if i == 2 {
            assert(full[8] == protocol::step(c,full[7],protocol::witness_inputs()[7]));
        } else if i == 3 {
            assert(full[9] == protocol::step(c,full[8],protocol::witness_inputs()[8]));
        } else if i == 4 {
            assert(full[10] == protocol::step(c,full[9],protocol::witness_inputs()[9]));
        } else if i == 5 {} else {
            assert(i == 6);
            assert(full[14] == protocol::step(c,full[13],protocol::witness_inputs()[13]));
        }
    }
    assert forall|i: int| 0 <= i < 7 && i != 5 implies
        enabled(c,states[i].disk,witness_actions()[i]) by {
        if i == 0 {
            assert(protocol::enabled(c,full[0],protocol::witness_inputs()[0]));
        } else if i == 1 {
            assert(protocol::enabled(c,full[6],protocol::witness_inputs()[6]));
        } else if i == 2 {
            assert(protocol::enabled(c,full[7],protocol::witness_inputs()[7]));
        } else if i == 3 {
            assert(protocol::enabled(c,full[8],protocol::witness_inputs()[8]));
        } else if i == 4 {
            assert(protocol::enabled(c,full[9],protocol::witness_inputs()[9]));
        } else {
            assert(i == 6);
            assert(protocol::enabled(c,full[13],protocol::witness_inputs()[13]));
        }
    }
    states
}

/// Physical log entries erase the placement proof witness. In particular, a
/// completed operation does NOT serialize p::State (and its unbounded history).
/// The committed-prefix interface supplies entries produced by enabled actions;
/// arbitrary unauthenticated byte strings are not a Raft committed prefix.
pub enum JournalEntry {
    Begun { record: Record },
    Finished { client: nat, outcome: p::Outcome, routes: Map<int,p::Grant> },
    Acknowledged { client: nat },
    Checkpointed,
    Compacted { client: nat },
    Renewed { client: nat },
}
pub open spec fn journal_entry(d: Durable, a: Action) -> JournalEntry {
    match a {
        Action::Begin { id,payload } => JournalEntry::Begun { record: new_record(d,id,payload) },
        Action::Finish { id,placement } => JournalEntry::Finished { client: id.client,
            outcome: placement.outcomes[d.records[id.client].generation as int],
            routes: p::directory(placement) },
        Action::Acknowledge { id,outcome: _ } => JournalEntry::Acknowledged { client: id.client },
        Action::Checkpoint => JournalEntry::Checkpointed,
        Action::Compact { client } => JournalEntry::Compacted { client },
        Action::Renew { client } => JournalEntry::Renewed { client },
    }
}
pub open spec fn replay_entry(d: Durable, entry: JournalEntry) -> Durable {
    match entry {
        JournalEntry::Begun { record } => Durable {
            records: d.records.insert(record.id.client,record),
            next_generation: d.next_generation + 1,active: Some(record.id.client),..d },
        JournalEntry::Finished { client,outcome,routes } => Durable {
            records: d.records.insert(client,Record { outcome: Some(outcome),..d.records[client] }),
            active: None,control_floor: d.records[client].generation,routes,..d },
        JournalEntry::Acknowledged { client } => Durable {
            records: d.records.insert(client,Record { acknowledged: true,..d.records[client] }),..d },
        JournalEntry::Checkpointed => Durable { recovery_floor: d.control_floor,..d },
        JournalEntry::Compacted { client } => Durable {
            namespaces: d.namespaces.insert(client,Namespace {
                floor: d.records[client].id.sequence,..d.namespaces[client] }),
            records: d.records.remove(client),..d },
        JournalEntry::Renewed { client } => Durable {
            namespaces: d.namespaces.insert(client,Namespace {
                incarnation: d.namespaces[client].incarnation + 1,floor: 0 }),..d },
    }
}
pub proof fn lemma_journal_encoding(d: Durable, a: Action)
    ensures replay_entry(d,journal_entry(d,a)) == apply_disk(d,a)
{
    match a {
        Action::Begin { .. } => {}, Action::Finish { .. } => {},
        Action::Acknowledge { .. } => {}, Action::Checkpoint => {},
        Action::Compact { .. } => {}, Action::Renew { .. } => {},
    }
}
pub open spec fn reconstruct(snapshot: Durable, suffix: Seq<JournalEntry>) -> Durable
    decreases suffix.len()
{
    if suffix.len() == 0 { snapshot }
    else { replay_entry(reconstruct(snapshot,suffix.drop_last()),suffix.last()) }
}
pub open spec fn encoded_suffix(states: Seq<State>, actions: Seq<Action>,
    cut: nat, end: nat) -> Seq<JournalEntry> {
    Seq::new((end-cut) as nat, |i: int| journal_entry(states[cut+i].disk,actions[cut+i]))
}
pub proof fn theorem_materialized_reconstruction(c: Config, states: Seq<State>, actions: Seq<Action>,
    cut: nat, end: nat)
    requires config_ok(c), behavior(c,states,actions), cut <= end < states.len(),
        forall|i: int| cut <= i < end ==> enabled(c,states[i].disk,actions[i])
    ensures reconstruct(states[cut as int].disk,encoded_suffix(states,actions,cut,end)) == states[end as int].disk
    decreases end-cut
{
    if cut < end {
        theorem_materialized_reconstruction(c,states,actions,cut,(end-1) as nat);
        let suffix = encoded_suffix(states,actions,cut,end);
        assert(suffix.drop_last() =~= encoded_suffix(states,actions,cut,(end-1) as nat));
        assert(suffix.last() == journal_entry(states[end-1].disk,actions[end-1]));
        lemma_journal_encoding(states[end-1].disk,actions[end-1]);
        assert(states[end as int] == apply(states[end-1],actions[end-1]));
    }
}
pub proof fn theorem_materialized_restart_fencing(c: Config, states: Seq<State>, actions: Seq<Action>,
    cut: nat, end: nat, id: RequestId, payload: Payload, command: p::Command, packet: p::Packet)
    requires config_ok(c), behavior(c,states,actions), cut <= end < states.len(),
        forall|i: int| cut <= i < end ==> enabled(c,states[i].disk,actions[i]),
        states[cut as int].history.contains_key(id), !exact_slot(states[cut as int].disk,id),
        packet.generation == states[cut as int].history[id].generation
    ensures disposition(c,reconstruct(states[cut as int].disk,
            encoded_suffix(states,actions,cut,end)),id,payload) is Expired,
        !accepts_control(reconstruct(states[cut as int].disk,encoded_suffix(states,actions,cut,end)),
            packet.generation,command),
        !accepts_copy(reconstruct(states[cut as int].disk,encoded_suffix(states,actions,cut,end)),packet)
{
    theorem_materialized_reconstruction(c,states,actions,cut,end);
    theorem_forgotten_history(c,states,actions,cut,end,id,payload,command,packet);
}

pub proof fn theorem_retained_retry(c: Config, s: State, id: RequestId, payload: Payload)
    requires inv(c,s), exact_slot(s.disk,id), s.disk.records[id.client].outcome is Some
    ensures payload == s.disk.records[id.client].payload ==>
        disposition(c,s.disk,id,payload) == (Disposition::Result {
            outcome: s.disk.records[id.client].outcome.unwrap() }),
        payload != s.disk.records[id.client].payload ==>
            disposition(c,s.disk,id,payload) is Conflict,
        !enabled(c,s.disk,Action::Begin { id,payload })
{
    assert(!expired(s.disk,id));
}
pub proof fn theorem_namespace_renewal(c: Config, s: State, client: nat, sequence: nat)
    requires inv(c,s), enabled(c,s.disk,Action::Renew { client })
    ensures inv(c,apply(s,Action::Renew { client })),
        apply(s,Action::Renew { client }).disk.namespaces[client].incarnation
            == s.disk.namespaces[client].incarnation + 1,
        expired(apply(s,Action::Renew { client }).disk,RequestId {
            client,incarnation: s.disk.namespaces[client].incarnation,sequence }),
        apply(s,Action::Renew { client }).disk.next_generation == s.disk.next_generation
{
    lemma_step(c,s,Action::Renew { client });
}
pub proof fn theorem_incarnation_exhaustion(c: Config, d: Durable, client: nat)
    requires disk_inv(c,d), d.namespaces.contains_key(client),
        d.namespaces[client].incarnation == c.counter_limit
    ensures !enabled(c,d,Action::Renew { client })
{}
pub proof fn theorem_no_generation_reuse(c: Config, s: State, id: RequestId, payload: Payload)
    requires inv(c,s), enabled(c,s.disk,Action::Begin { id,payload })
    ensures !s.history.contains_key(id),
        forall|old: RequestId| s.history.contains_key(old) ==>
            s.history[old].generation < apply(s,Action::Begin { id,payload }).disk.records[id.client].generation
{
    if s.history.contains_key(id) {
        assert(history_record_ok(s.disk,id,s.history[id]));
        assert(!exact_slot(s.disk,id));
        assert(expired(s.disk,id));
        assert(false);
    }
    assert forall|old: RequestId| s.history.contains_key(old) implies
        s.history[old].generation < apply(s,Action::Begin { id,payload }).disk.records[id.client].generation by {
        assert(history_record_ok(s.disk,old,s.history[old]));
    }
}
pub proof fn theorem_materialized_shrink(c: Config, s: State, client: nat)
    requires inv(c,s), enabled(c,s.disk,Action::Compact { client })
    ensures materialized_cells(apply(s,Action::Compact { client }).disk) + 1
        == materialized_cells(s.disk)
{
    theorem_collection(c,s,client);
    assert(apply(s,Action::Compact { client }).disk.namespaces.dom() == s.disk.namespaces.dom());
}

} // verus!
