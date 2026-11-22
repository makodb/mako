//! Operational composition for retention. The parent module's Action is only
//! an internal administrative projection; Input below is the external protocol.
//! Placement state is a ghost execution of the existing proved machine, started
//! at p::initial and changed only by p::apply on enabled placement actions. No
//! caller can supply a placement completion witness. The physical journal erases
//! this ghost state and records only successful materialized administrative edits.
use vstd::prelude::*;
use super as r;
use crate::sharding_placement as p;

verus! {

pub struct ProtocolState { pub administrative: r::State, pub placement: p::State }
pub enum Input {
    Begin { id: r::RequestId, payload: r::Payload },
    Finish { id: r::RequestId },
    Placement { action: p::Action },
    Acknowledge { id: r::RequestId, outcome: p::Outcome },
    Checkpoint,
    Compact { client: nat },
    Renew { client: nat },
}
pub enum RequestDisposition {
    Administrative { status: r::Disposition }, InvalidPlacement,
}
pub open spec fn initial(c: r::Config) -> ProtocolState {
    ProtocolState { administrative: r::initial(c),placement: p::initial(c.placement) }
}
pub open spec fn begin_action(d: r::Durable, payload: r::Payload) -> p::Action {
    p::Action::Begin { nonce: d.next_generation as int,src: payload.src,dst: payload.dst,
        table: payload.table,lo: payload.lo,hi: payload.hi }
}
pub open spec fn administrative_action(s: ProtocolState, input: Input) -> Option<r::Action> {
    match input {
        Input::Begin { id,payload } => Some(r::Action::Begin { id,payload }),
        Input::Finish { id } => Some(r::Action::Finish { id,placement: s.placement }),
        Input::Placement { .. } => None,
        Input::Acknowledge { id,outcome } => Some(r::Action::Acknowledge { id,outcome }),
        Input::Checkpoint => Some(r::Action::Checkpoint),
        Input::Compact { client } => Some(r::Action::Compact { client }),
        Input::Renew { client } => Some(r::Action::Renew { client }),
    }
}
pub open spec fn placement_action(s: ProtocolState, input: Input) -> Option<p::Action> {
    match input {
        Input::Begin { id: _,payload } => Some(begin_action(s.administrative.disk,payload)),
        Input::Finish { .. } => Some(p::Action::Finish),
        Input::Placement { action } => Some(action),
        _ => None,
    }
}
/// The generation check is a PREFILTER, not a replacement for p's local phase,
/// round, role, membership and authenticity checks. A rejected packet never
/// reaches p::apply or a reclaimed plan lookup. Replies use the administrative
/// disposition, never p's proof-history outcome map.
pub open spec fn placement_prefilter(d: r::Durable, action: p::Action) -> bool {
    match action {
        p::Action::Begin { .. } | p::Action::Finish | p::Action::Reply { .. } => false,
        p::Action::Deliver { generation,command,owner: _ } => r::accepts_control(d,generation,command),
        p::Action::DeliverCopy { packet } => r::accepts_copy(d,packet),
        _ => true,
    }
}
pub open spec fn enabled(c: r::Config, s: ProtocolState, input: Input) -> bool {
    (match administrative_action(s,input) {
        Some(action) => r::enabled(c,s.administrative.disk,action),None => true,
    })
    && (match placement_action(s,input) {
        Some(action) => p::enabled(c.placement,s.placement,action),None => true,
    })
    && (match input {
        Input::Placement { action } => placement_prefilter(s.administrative.disk,action),
        _ => true,
    })
}
pub open spec fn request_disposition(c: r::Config, s: ProtocolState,
    id: r::RequestId, payload: r::Payload) -> RequestDisposition {
    let status = r::disposition(c,s.administrative.disk,id,payload);
    if status is Fresh && !p::enabled(c.placement,s.placement,begin_action(s.administrative.disk,payload)) {
        RequestDisposition::InvalidPlacement
    } else { RequestDisposition::Administrative { status } }
}
pub open spec fn apply(c: r::Config, s: ProtocolState, input: Input) -> ProtocolState {
    ProtocolState {
        administrative: match administrative_action(s,input) {
            Some(action) => r::apply(s.administrative,action),None => s.administrative,
        },
        placement: match placement_action(s,input) {
            Some(action) => p::apply(c.placement,s.placement,action),None => s.placement,
        },
    }
}
pub open spec fn step(c: r::Config, s: ProtocolState, input: Input) -> ProtocolState {
    if enabled(c,s,input) { apply(c,s,input) } else { s }
}
pub open spec fn active_binding(s: ProtocolState) -> bool {
    match s.administrative.disk.active {
        None => s.placement.active is None
            && s.administrative.disk.routes == p::directory(s.placement),
        Some(client) => {
            let record = s.administrative.disk.records[client];
            let g = record.generation;
            &&& s.placement.active == Some(g)
            &&& s.placement.plans.contains_key(g)
            &&& s.placement.plans[g].nonce == g as int
            &&& s.placement.plans[g].src == record.payload.src
            &&& s.placement.plans[g].dst == record.payload.dst
            &&& s.placement.plans[g].table == record.payload.table
            &&& s.placement.plans[g].lo == record.payload.lo
            &&& s.placement.plans[g].hi == record.payload.hi
        },
    }
}
pub open spec fn inv(c: r::Config, s: ProtocolState) -> bool {
    r::inv(c,s.administrative) && p::inv(c.placement,s.placement)
        && s.administrative.disk.next_generation == s.placement.next_generation
        && active_binding(s)
}
pub proof fn lemma_initial(c: r::Config)
    requires r::config_ok(c)
    ensures inv(c,initial(c))
{
    r::lemma_initial(c);
    p::lemma_init(c.placement);
}
pub proof fn lemma_internal_placement(c: r::Config, s: p::State, action: p::Action)
    requires p::inv(c.placement,s), p::enabled(c.placement,s,action),
        !(action is Begin || action is Finish)
    ensures p::apply(c.placement,s,action).active == s.active,
        p::apply(c.placement,s,action).next_generation == s.next_generation,
        p::apply(c.placement,s,action).plans == s.plans,
        s.active is None ==> p::directory(p::apply(c.placement,s,action)) == p::directory(s)
{
    match action {
        p::Action::Commit => { assert(s.active is Some); },
        _ => {},
    }
}
pub proof fn lemma_step(c: r::Config, s: ProtocolState, input: Input)
    requires inv(c,s), enabled(c,s,input)
    ensures inv(c,apply(c,s,input))
{
    if administrative_action(s,input) is Some {
        r::lemma_step(c,s.administrative,administrative_action(s,input).unwrap());
    }
    if placement_action(s,input) is Some {
        p::lemma_step(c.placement,s.placement,placement_action(s,input).unwrap());
    }
    match input {
        Input::Begin { id,payload } => {
            assert(s.administrative.disk.active is None);
            assert(s.placement.active is None);
            assert(apply(c,s,input).placement.plans[s.placement.next_generation].nonce
                == s.placement.next_generation as int);
        },
        Input::Finish { id } => {
            r::theorem_finish_interface(c,s.administrative.disk,id,s.placement);
        },
        Input::Placement { action } => {
            lemma_internal_placement(c,s.placement,action);
        },
        Input::Compact { client } => {
            assert(s.administrative.disk.active != Some(client));
        },
        Input::Renew { client } => {
            if s.administrative.disk.active is Some {
                assert(s.administrative.disk.active.unwrap() != client);
            }
        },
        _ => {},
    }
}
pub open spec fn behavior(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>) -> bool {
    states.len() == inputs.len() + 1 && states[0] == initial(c)
        && forall|i: int| 0 <= i < inputs.len() ==> states[i+1] == step(c,states[i],inputs[i])
}
pub proof fn theorem_history(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>, i: nat)
    requires r::config_ok(c), behavior(c,states,inputs), i < states.len()
    ensures inv(c,states[i as int])
    decreases i
{
    if i == 0 { lemma_initial(c); }
    else {
        theorem_history(c,states,inputs,(i-1) as nat);
        assert(states[i as int] == step(c,states[i-1],inputs[i-1]));
        if enabled(c,states[i-1],inputs[i-1]) { lemma_step(c,states[i-1],inputs[i-1]); }
    }
}
/// This readiness theorem derives the projection's completion guard from the
/// CURRENT source-connected placement state, not from a caller-supplied witness.
pub proof fn theorem_finish_ready(c: r::Config, s: ProtocolState, id: r::RequestId)
    requires inv(c,s), r::exact_slot(s.administrative.disk,id),
        s.administrative.disk.active == Some(id.client),
        p::enabled(c.placement,s.placement,p::Action::Finish)
    ensures enabled(c,s,Input::Finish { id })
{
    let g = s.administrative.disk.records[id.client].generation;
    assert(s.placement.plans.contains_key(g));
    assert(p::plan_inv(c.placement,s.placement,g));
    assert(p::terminal(s.placement.phases[g]));
    assert(s.placement.outcomes.contains_key(g as int));
    assert(p::directory(s.placement).dom() == c.placement.keys);
}
pub proof fn theorem_completion_origin(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>,
    i: nat, id: r::RequestId)
    requires r::config_ok(c), behavior(c,states,inputs), i < inputs.len(),
        inputs[i as int] == (Input::Finish { id }), enabled(c,states[i as int],inputs[i as int])
    ensures p::inv(c.placement,states[i as int].placement),
        p::enabled(c.placement,states[i as int].placement,p::Action::Finish),
        states[i as int].placement.active == Some(states[i as int].administrative.disk.records[id.client].generation),
        states[i as int].placement.received.contains((states[i as int].administrative.disk.records[id.client].generation,
            p::Certificate::SourceDone)),
        states[i as int].placement.received.contains((states[i as int].administrative.disk.records[id.client].generation,
            p::Certificate::DestinationDone)),
        states[i as int+1].placement == p::apply(c.placement,states[i as int].placement,p::Action::Finish),
        states[i as int+1].administrative.disk.records[id.client].outcome == Some(
            states[i as int].placement.outcomes[states[i as int].administrative.disk.records[id.client].generation as int])
{
    theorem_history(c,states,inputs,i);
    r::theorem_finish_interface(c,states[i as int].administrative.disk,id,states[i as int].placement);
    assert(states[i as int+1] == apply(c,states[i as int],inputs[i as int]));
}
pub proof fn theorem_invalid_begin(c: r::Config, s: ProtocolState, id: r::RequestId, payload: r::Payload)
    requires !p::enabled(c.placement,s.placement,begin_action(s.administrative.disk,payload))
    ensures !enabled(c,s,Input::Begin { id,payload }), step(c,s,Input::Begin { id,payload }) == s,
        r::disposition(c,s.administrative.disk,id,payload) is Fresh ==>
            request_disposition(c,s,id,payload) is InvalidPlacement
{}
pub proof fn theorem_wrapper_rejects(c: r::Config, s: ProtocolState, generation: nat,
    command: p::Command, owner: int, packet: p::Packet)
    requires generation <= s.administrative.disk.control_floor, packet.generation == generation
    ensures !enabled(c,s,Input::Placement { action: p::Action::Deliver { generation,command,owner } }),
        step(c,s,Input::Placement { action: p::Action::Deliver { generation,command,owner } }) == s,
        !enabled(c,s,Input::Placement { action: p::Action::DeliverCopy { packet } }),
        step(c,s,Input::Placement { action: p::Action::DeliverCopy { packet } }) == s
{}
pub proof fn theorem_wrapper_delegates(c: r::Config, s: ProtocolState, action: p::Action)
    requires enabled(c,s,Input::Placement { action })
    ensures p::enabled(c.placement,s.placement,action),
        step(c,s,Input::Placement { action }).placement == p::apply(c.placement,s.placement,action),
        step(c,s,Input::Placement { action }).administrative == s.administrative,
        placement_prefilter(s.administrative.disk,action)
{}
pub proof fn theorem_collection_preserves_live_routes(c: r::Config, s: ProtocolState, client: nat)
    requires inv(c,s), enabled(c,s,Input::Compact { client })
    ensures step(c,s,Input::Compact { client }).placement == s.placement,
        p::directory(step(c,s,Input::Compact { client }).placement) == p::directory(s.placement),
        r::materialized_cells(step(c,s,Input::Compact { client }).administrative.disk) + 1
            == r::materialized_cells(s.administrative.disk)
{
    r::theorem_materialized_shrink(c,s.administrative,client);
}

/// None means no administrative log entry: placement-only work and rejected
/// retries cannot smuggle a placement history into the administrative journal.
pub open spec fn emitted(c: r::Config, s: ProtocolState, input: Input) -> Option<r::JournalEntry> {
    if enabled(c,s,input) && administrative_action(s,input) is Some {
        Some(r::journal_entry(s.administrative.disk,administrative_action(s,input).unwrap()))
    } else { None }
}
pub open spec fn reconstruct(snapshot: r::Durable, suffix: Seq<Option<r::JournalEntry>>) -> r::Durable
    decreases suffix.len()
{
    if suffix.len() == 0 { snapshot }
    else {
        let before = reconstruct(snapshot,suffix.drop_last());
        match suffix.last() { Some(entry) => r::replay_entry(before,entry),None => before }
    }
}
pub open spec fn suffix(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>,
    cut: nat, end: nat) -> Seq<Option<r::JournalEntry>> {
    Seq::new((end-cut) as nat, |i: int| emitted(c,states[cut+i],inputs[cut+i]))
}
pub proof fn lemma_emitted(c: r::Config, s: ProtocolState, input: Input)
    ensures match emitted(c,s,input) {
        Some(entry) => r::replay_entry(s.administrative.disk,entry) == step(c,s,input).administrative.disk,
        None => s.administrative.disk == step(c,s,input).administrative.disk,
    }
{
    if enabled(c,s,input) && administrative_action(s,input) is Some {
        r::lemma_journal_encoding(s.administrative.disk,administrative_action(s,input).unwrap());
    }
}
pub proof fn theorem_snapshot_suffix(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>,
    cut: nat, end: nat)
    requires r::config_ok(c), behavior(c,states,inputs), cut <= end < states.len()
    ensures reconstruct(states[cut as int].administrative.disk,suffix(c,states,inputs,cut,end))
        == states[end as int].administrative.disk
    decreases end-cut
{
    if cut < end {
        theorem_snapshot_suffix(c,states,inputs,cut,(end-1) as nat);
        let entries = suffix(c,states,inputs,cut,end);
        assert(entries.drop_last() =~= suffix(c,states,inputs,cut,(end-1) as nat));
        assert(entries.last() == emitted(c,states[end-1],inputs[end-1]));
        lemma_emitted(c,states[end-1],inputs[end-1]);
        assert(states[end as int] == step(c,states[end-1],inputs[end-1]));
    }
}
pub proof fn theorem_floors_monotone(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>,
    cut: nat, end: nat, id: r::RequestId)
    requires r::config_ok(c), behavior(c,states,inputs), cut <= end < states.len(),
        r::expired(states[cut as int].administrative.disk,id)
    ensures r::expired(states[end as int].administrative.disk,id),
        states[end as int].administrative.disk.control_floor >= states[cut as int].administrative.disk.control_floor
    decreases end-cut
{
    if cut < end {
        theorem_floors_monotone(c,states,inputs,cut,(end-1) as nat,id);
        theorem_history(c,states,inputs,(end-1) as nat);
        let s = states[end-1]; let input = inputs[end-1];
        assert(states[end as int] == step(c,s,input));
        if enabled(c,s,input) && administrative_action(s,input) is Some {
            let a = administrative_action(s,input).unwrap();
            r::lemma_expired_monotone(c,s.administrative.disk,a,id);
            r::lemma_disk_step(c,s.administrative.disk,a);
        }
    }
}
pub proof fn theorem_forgotten_after_restart(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>,
    cut: nat, end: nat, id: r::RequestId, payload: r::Payload, packet: p::Packet, command: p::Command)
    requires r::config_ok(c), behavior(c,states,inputs), cut <= end < states.len(),
        states[cut as int].administrative.history.contains_key(id),
        !r::exact_slot(states[cut as int].administrative.disk,id),
        packet.generation == states[cut as int].administrative.history[id].generation
    ensures r::disposition(c,reconstruct(states[cut as int].administrative.disk,
            suffix(c,states,inputs,cut,end)),id,payload) is Expired,
        !enabled(c,states[end as int],Input::Begin { id,payload }),
        !enabled(c,states[end as int],Input::Placement { action: p::Action::DeliverCopy { packet } }),
        forall|owner: int| !enabled(c,states[end as int],Input::Placement { action: p::Action::Deliver {
            generation: packet.generation,command,owner } })
{
    theorem_history(c,states,inputs,cut);
    assert(r::history_record_ok(states[cut as int].administrative.disk,id,states[cut as int].administrative.history[id]));
    theorem_floors_monotone(c,states,inputs,cut,end,id);
    theorem_snapshot_suffix(c,states,inputs,cut,end);
    assert forall|owner: int| !enabled(c,states[end as int],Input::Placement { action: p::Action::Deliver {
        generation: packet.generation,command,owner } }) by {
        theorem_wrapper_rejects(c,states[end as int],packet.generation,command,owner,packet);
    }
}

pub open spec fn witness_inputs() -> Seq<Input> {
    seq![
        Input::Begin { id: r::witness_id(1),payload: r::witness_payload() },
        Input::Placement { action: r::witness_placement_actions()[1] },
        Input::Placement { action: r::witness_placement_actions()[2] },
        Input::Placement { action: r::witness_placement_actions()[3] },
        Input::Placement { action: r::witness_placement_actions()[4] },
        Input::Placement { action: r::witness_placement_actions()[5] },
        Input::Finish { id: r::witness_id(1) },
        Input::Acknowledge { id: r::witness_id(1),outcome: p::Outcome { generation: 1,committed: false } },
        Input::Checkpoint,
        Input::Compact { client: 7 },
        Input::Begin { id: r::witness_id(1),payload: r::witness_payload() },
        Input::Placement { action: p::Action::Deliver { generation: 1,command: p::Command::Abort,owner: 0 } },
        Input::Placement { action: p::Action::DeliverCopy { packet: p::Packet {
            generation: 1,round: 0,key: 0,cell: p::empty_cell() } } },
        Input::Begin { id: r::witness_id(2),payload: r::witness_payload() },
    ]
}
pub proof fn advance(c: r::Config, s: ProtocolState, input: Input) -> (z: ProtocolState)
    requires inv(c,s), enabled(c,s,input)
    ensures z == step(c,s,input), z == apply(c,s,input), inv(c,z)
{
    lemma_step(c,s,input);
    apply(c,s,input)
}
pub proof fn lemma_admitted_projection(c: r::Config, s: ProtocolState, input: Input)
    requires enabled(c,s,input), administrative_action(s,input) is Some
    ensures r::enabled(c,s.administrative.disk,administrative_action(s,input).unwrap()),
        step(c,s,input).administrative ==
            r::apply(s.administrative,administrative_action(s,input).unwrap())
{}
pub proof fn lemma_invariant_projection(c: r::Config, s: ProtocolState)
    requires inv(c,s)
    ensures r::inv(c,s.administrative)
{}
pub open spec fn witness_prefix_ok(states: Seq<ProtocolState>) -> bool {
    &&& states.len() == 7
    &&& behavior(r::witness_config(),states,witness_inputs().subrange(0,6))
    &&& inv(r::witness_config(),states[6])
    &&& states[6].placement == r::witness_placement_at(6)
    &&& states[6].administrative == r::apply(r::initial(r::witness_config()),
        r::Action::Begin { id: r::witness_id(1),payload: r::witness_payload() })
    &&& states[6].administrative == states[1].administrative
    &&& forall|i: int| 0 <= i < 6 ==> enabled(r::witness_config(),states[i],witness_inputs()[i])
}
pub proof fn witness_prefix() -> (states: Seq<ProtocolState>)
    ensures witness_prefix_ok(states)
{
    hide(r::inv);
    hide(r::disk_inv);
    hide(p::inv);
    hide(p::apply);
    hide(p::enabled);
    hide(r::witness_placement_at);
    let c = r::witness_config();
    let inputs = witness_inputs();
    let s0 = initial(c);
    assert(r::config_ok(c));
    lemma_initial(c);
    r::lemma_witness_placement();
    r::lemma_witness_placement_step(0);
    assert(s0.placement == r::witness_placement_at(0));
    assert(enabled(c,s0,inputs[0]));
    let s1 = advance(c,s0,inputs[0]);
    assert(s1.placement == r::witness_placement_at(1));
    r::lemma_witness_placement_step(1);
    assert(enabled(c,s1,inputs[1]));
    let s2 = advance(c,s1,inputs[1]);
    assert(s2.placement == r::witness_placement_at(2));
    r::lemma_witness_placement_step(2);
    assert(enabled(c,s2,inputs[2]));
    let s3 = advance(c,s2,inputs[2]);
    assert(s3.placement == r::witness_placement_at(3));
    r::lemma_witness_placement_step(3);
    assert(enabled(c,s3,inputs[3]));
    let s4 = advance(c,s3,inputs[3]);
    assert(s4.placement == r::witness_placement_at(4));
    r::lemma_witness_placement_step(4);
    assert(enabled(c,s4,inputs[4]));
    let s5 = advance(c,s4,inputs[4]);
    assert(s5.placement == r::witness_placement_at(5));
    r::lemma_witness_placement_step(5);
    assert(enabled(c,s5,inputs[5]));
    let s6 = advance(c,s5,inputs[5]);
    assert(s6.placement == r::witness_placement_at(6));
    let states = seq![s0,s1,s2,s3,s4,s5,s6];
    assert forall|i: int| 0 <= i < 6 implies
        states[i+1] == step(c,states[i],inputs[i]) by {
        if i == 0 {} else if i == 1 {} else if i == 2 {} else if i == 3 {}
        else if i == 4 {} else { assert(i == 5); }
    }
    assert forall|i: int| 0 <= i < 6 implies enabled(c,states[i],inputs[i]) by {
        if i == 0 {} else if i == 1 {} else if i == 2 {} else if i == 3 {}
        else if i == 4 {} else { assert(i == 5); }
    }
    states
}

pub open spec fn witness_result(states: Seq<ProtocolState>) -> bool {
    &&& behavior(r::witness_config(),states,witness_inputs()) && states.len() == 15
    &&& forall|i: int| 0 <= i < 14 && (i < 10 || i == 13) ==>
        enabled(r::witness_config(),states[i],witness_inputs()[i])
    &&& states[6].placement == r::witness_placement_at(6)
    &&& states[6].administrative == states[1].administrative
    &&& states[7].placement.active is None
    &&& r::materialized_cells(states[10].administrative.disk) + 1
        == r::materialized_cells(states[9].administrative.disk)
    &&& states[10].administrative.disk.records.len() == 0
    &&& states[11] == states[10] && states[12] == states[11] && states[13] == states[12]
    &&& r::disposition(r::witness_config(),states[14].administrative.disk,
        r::witness_id(1),r::witness_payload()) is Expired
    &&& states[14].administrative.disk.records[7nat].id == r::witness_id(2)
    &&& states[14].administrative.disk.records[7nat].generation == 2
    &&& states[14].placement.active == Some(2nat) && inv(r::witness_config(),states[14])
    &&& states[14].administrative.disk.active == Some(7nat)
    &&& states[10].administrative.history.contains_key(r::witness_id(1))
    &&& states[10].administrative.history[r::witness_id(1)].generation == 1
    &&& states[14].administrative.history.contains_key(r::witness_id(1))
    &&& states[14].administrative.history[r::witness_id(1)].generation == 1
}
pub proof fn witness_suffix(prefix: Seq<ProtocolState>) -> (states: Seq<ProtocolState>)
    requires witness_prefix_ok(prefix)
    ensures witness_result(states)
{
    hide(r::witness_placement_at);
    hide(r::inv);
    hide(r::disk_inv);
    hide(p::inv);
    hide(p::apply);
    hide(p::enabled);
    let c = r::witness_config();
    let inputs = witness_inputs();
    let s6 = prefix[6];
    r::lemma_witness_placement();
    theorem_finish_ready(c,s6,r::witness_id(1));
    let s7 = advance(c,s6,inputs[6]);
    r::theorem_retained_result(c,s7.administrative,r::witness_id(1));
    assert(enabled(c,s7,inputs[7]));
    let s8 = advance(c,s7,inputs[7]);
    assert(!enabled(c,s8,Input::Compact { client: 7 }));
    let s9 = advance(c,s8,inputs[8]);
    assert(enabled(c,s9,inputs[9]));
    theorem_collection_preserves_live_routes(c,s9,7);
    let s10 = advance(c,s9,inputs[9]);
    assert(!enabled(c,s10,inputs[10]));
    let s11 = step(c,s10,inputs[10]);
    theorem_wrapper_rejects(c,s11,1,p::Command::Abort,0,p::Packet {
        generation: 1,round: 0,key: 0,cell: p::empty_cell() });
    let s12 = step(c,s11,inputs[11]);
    let s13 = step(c,s12,inputs[12]);
    assert(p::enabled(c.placement,s13.placement,begin_action(s13.administrative.disk,r::witness_payload())));
    assert(enabled(c,s13,inputs[13]));
    let s14 = advance(c,s13,inputs[13]);
    r::theorem_forgotten(c,s14.administrative,r::witness_id(1),r::witness_payload(),
        p::Command::Abort,p::Packet { generation: 1,round: 0,key: 0,cell: p::empty_cell() });
    let states = prefix + seq![s7,s8,s9,s10,s11,s12,s13,s14];
    assert forall|i: int| 0 <= i < 14 implies
        states[i+1] == step(c,states[i],inputs[i]) by {
        if i < 6 {
            let prefix_inputs = witness_inputs().subrange(0,6);
            assert(behavior(c,prefix,prefix_inputs));
            assert(0 <= i < prefix_inputs.len());
            assert(prefix[i+1] == step(c,prefix[i],prefix_inputs[i]));
            assert(prefix_inputs[i] == inputs[i]);
            assert(prefix[i+1] == step(c,prefix[i],inputs[i]));
        } else if i == 6 {} else if i == 7 {} else if i == 8 {} else if i == 9 {}
        else if i == 10 {} else if i == 11 {} else if i == 12 {} else { assert(i == 13); }
    }
    assert forall|i: int| 0 <= i < 14 && (i < 10 || i == 13) implies
        enabled(c,states[i],inputs[i]) by {
        if i < 6 { assert(enabled(c,prefix[i],inputs[i])); }
        else if i == 6 {} else if i == 7 {} else if i == 8 {} else if i == 9 {}
        else { assert(i == 13); }
    }
    states
}
pub proof fn theorem_enabled_witness() -> (states: Seq<ProtocolState>)
    ensures witness_result(states)
{
    let prefix = witness_prefix();
    witness_suffix(prefix)
}

pub proof fn theorem_outcome_immutable(c: r::Config, states: Seq<ProtocolState>, inputs: Seq<Input>,
    cut: nat, end: nat, id: r::RequestId)
    requires r::config_ok(c), behavior(c,states,inputs), cut <= end < states.len(),
        states[cut as int].administrative.history.contains_key(id),
        states[cut as int].administrative.history[id].outcome is Some
    ensures states[end as int].administrative.history.contains_key(id),
        states[end as int].administrative.history[id].outcome == states[cut as int].administrative.history[id].outcome,
        states[end as int].administrative.history[id].payload == states[cut as int].administrative.history[id].payload,
        states[end as int].administrative.history[id].generation == states[cut as int].administrative.history[id].generation,
        !states[end as int].administrative.history[id].acknowledged ==>
            r::exact_slot(states[end as int].administrative.disk,id)
            && r::disposition(c,states[end as int].administrative.disk,id,
                states[cut as int].administrative.history[id].payload) == (r::Disposition::Result {
                    outcome: states[cut as int].administrative.history[id].outcome.unwrap() })
    decreases end-cut
{
    if cut < end {
        theorem_outcome_immutable(c,states,inputs,cut,(end-1) as nat,id);
        theorem_history(c,states,inputs,(end-1) as nat);
        let s = states[end-1]; let input = inputs[end-1];
        assert(states[end as int] == step(c,s,input));
        if enabled(c,s,input) && administrative_action(s,input) is Some {
            r::lemma_step(c,s.administrative,administrative_action(s,input).unwrap());
        }
    }
    theorem_history(c,states,inputs,end);
    if !states[end as int].administrative.history[id].acknowledged {
        r::theorem_retained_result(c,states[end as int].administrative,id);
    }
}

} // verus!
