//! Closed, source-certified execution history. No API accepts a logger-declared
//! transition: coordinator, metadata, cache, lease, engine and transfer records
//! discharge paths from their actual source effects. Private receipt tokens
//! distinguish semantic Empty masking from a response actually emitted after
//! cleanup; errors close their real metadata/copy prefix rather than stuttering.
//!
//! Exclusive loader commits establish the initial prefix before activation.
//! Runtime records then reuse that journal and its real writer/tombstone history.
//! Named boundaries remain authentic command/capture RPCs, canonical finite-key
//! embedding, successful atomic engine bytes/frames, and selected-range transfer
//! exclusion. Unrelated admitted writes are not excluded by a metadata mutex.
//! This is the fixed-live, non-replicated handoff envelope, not crash/Raft safety.
use vstd::prelude::*;
use crate::sharding_placement as p;
use crate::ghost_log as log;
use crate::transfer_proofs as transfer;
use crate::participant::Participant;
use crate::types::MigrationPlan;
use crate::storage::{Image,Cell};
#[path = "transfer_control_proofs.rs"]
mod control;
pub use control::*;
#[path = "transfer_metadata_proofs.rs"]
mod metadata_execution;
pub use metadata_execution::*;
#[path = "transfer_lease_execution.rs"]
mod lease_execution;
pub use lease_execution::*;
#[path = "transfer_engine_proofs.rs"]
mod engine;
pub use engine::*;
#[path = "transfer_bootstrap.rs"]
mod bootstrap;
pub use bootstrap::*;
verus! {
/// Evidence of an actual emitted participant response. Semantic certificates
/// from Empty masking alone cannot construct this token or authorize receipt.
pub tracked struct EmittedCertificate {
    ghost generation:u64,
    ghost owner:u32,
    ghost certificate:crate::types::Certificate,
}
impl EmittedCertificate {
    pub closed spec fn matches(&self,generation:u64,owner:u32,certificate:crate::types::Certificate) -> bool {
        self.generation == generation && self.owner == owner && self.certificate == certificate
    }
}
pub tracked struct EngineCompletion { ghost id:crate::types::TxnId }
impl EngineCompletion {
    pub closed spec fn matches(&self,id:crate::types::TxnId) -> bool { self.id == id }
}
pub tracked struct ClosedTransfer { ghost segment: log::Segment }
impl ClosedTransfer {
    pub closed spec fn valid(&self,c: p::Constants,before: p::State) -> bool {
        log::certificate(c,before,self.segment)
    }
    pub closed spec fn after(&self,before: p::State) -> p::State { log::apply_writes(before,self.segment.writes) }
    /// Integrate with the global actor journal without exposing a constructor
    /// from arbitrary raw writes or an asserted model transition.
    pub proof fn append_to_journal(tracked self,c:p::Constants,initial:p::State,
        journal:log::Journal,before:p::State) -> (out:log::Journal)
        requires log::closed(c,initial,journal,before),self.valid(c,before),
        ensures log::closed(c,initial,out,self.after(before)),
    {
        let journal = log::open_segment(c,initial,journal,before);
        let next = self.after(before);
        let journal = log::record_batch(c,initial,journal,before,next,self.segment.writes);
        log::close_segment(c,initial,journal,next,self.segment.states,self.segment.actions)
    }
}

pub proof fn captured(c: p::Constants,node: &Participant,plan: MigrationPlan,
    s: p::State,keys: Seq<int>,labels: Map<int,Cell>,source: Image,k: int,writer: int,code: spec_fn(Seq<u8>)->int,
    cursor:Option<crate::storage::Identity>,row:Option<crate::types::Row>)
    -> (tracked out: ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(node,plan,s,labels),
        node.capture_authorized(plan),keys.to_set().contains(k),
        crate::storage::first(source,plan.range,cursor,row),transfer::scan_covers(plan.range,cursor,row,labels[k]),
        p::drained(s,s.plans[plan.generation as nat].keys) == node.drained_view(plan.range),
        transfer::observed(source,labels[k],writer,code) == p::replica(s,plan.source as int,k).cell,
    ensures out.valid(c,s),out.after(s).packets.contains(transfer::packet(plan.generation as nat,k,transfer::observed(source,labels[k],writer,code))),
{
    let segment = transfer::capture_segment(c,node,plan,s,keys,labels,source,k,writer,code,cursor,row);
    ClosedTransfer { segment }
}

pub proof fn copied(c: p::Constants,node: &Participant,plan: MigrationPlan,
    s: p::State,keys: Seq<int>,labels: Map<int,Cell>,key: int,
    before: Image,after: Image,row_value: Option<Seq<u8>>,writers_before: Map<int,int>,writers_after: Map<int,int>,
    source_cell: p::Cell,code: spec_fn(Seq<u8>)->int)
    -> (tracked out: ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(node,plan,s,labels),
        node.transfer_authorized(plan,false),keys.to_set().contains(key),
        s.physical.contains_key((plan.destination as int,key)),s.packets.contains(transfer::packet(plan.generation as nat,key,source_cell)),
        after == match row_value { Some(v) => before.insert(labels[key],v), None => before.remove(labels[key]) },
        source_cell.value == match row_value { Some(v) => Some(code(v)), None => None },
        writers_after == writers_before.insert(key,source_cell.writer),
    ensures out.valid(c,s),
        out.after(s).physical[(plan.destination as int,key)].cell == transfer::observed(after,labels[key],writers_after[key],code),
        out.after(s).physical[(plan.destination as int,key)].covered,
{
    transfer::native_copy_effect(before,after,labels[key],row_value,labels,key,writers_before,writers_after,source_cell,code);
    let segment = transfer::copy_segment(c,node,plan,s,keys,labels,key,source_cell);
    reveal_with_fuel(log::apply_writes,3);
    ClosedTransfer { segment }
}

/// A scan-certified absent label delivers a tombstone with its captured writer,
/// even when no physical delete was needed. Real ordered EOF/gaps prove absence.
pub proof fn absent_copy(c: p::Constants,node: &Participant,plan: MigrationPlan,
    s: p::State,keys: Seq<int>,labels: Map<int,Cell>,key: int,image: Image,
    writers: Map<int,int>,cell: p::Cell,code: spec_fn(Seq<u8>)->int)
    -> (tracked out: ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(node,plan,s,labels),
        node.transfer_authorized(plan,false),keys.to_set().contains(key),
        s.physical.contains_key((plan.destination as int,key)),s.packets.contains(transfer::packet(plan.generation as nat,key,cell)),
        !image.dom().contains(labels[key]),cell.value == None,
    ensures out.valid(c,s),out.after(s).physical[(plan.destination as int,key)].covered,
        out.after(s).physical[(plan.destination as int,key)].cell == cell,
{
    assert(image.remove(labels[key]) =~= image);
    copied(c,node,plan,s,keys,labels,key,image,image,None,writers,writers.insert(key,cell.writer),cell,code)
}

pub open spec fn ready_writes(owner: int,keys: Seq<int>) -> Seq<log::Write>
    decreases keys.len(),
{
    if keys.len() == 0 { Seq::empty() }
    else { ready_writes(owner,keys.drop_last()).push(log::Write::ReplicaRole { owner,key:keys.last(),role:p::Role::Ready }) }
}
pub open spec fn ready_state(s:p::State,owner:int,keys:Seq<int>) -> p::State {
    p::State { physical:vstd::imap::IMap::new(|q:(int,int)| s.physical.contains_key(q),|q:(int,int)|
        if q.0 == owner && keys.to_set().contains(q.1) { p::Replica { role:p::Role::Ready,..s.physical[q] } } else { s.physical[q] }),..s }
}
pub proof fn ready_replay(s: p::State,owner: int,keys: Seq<int>)
    requires forall|k: int| keys.to_set().contains(k) ==> s.physical.contains_key((owner,k)),
    ensures log::writes_ok(s,ready_writes(owner,keys)),
        log::apply_writes(s,ready_writes(owner,keys)) == ready_state(s,owner,keys),
    decreases keys.len(),
{
    if keys.len() > 0 {
        assert(keys.drop_last().to_set().subset_of(keys.to_set()));
        assert(keys.to_set().contains(keys.last()));
        ready_replay(s,owner,keys.drop_last());
        let writes = ready_writes(owner,keys.drop_last());
        log::append_write(s,writes,log::Write::ReplicaRole { owner,key:keys.last(),role:p::Role::Ready });
        transfer::split_set(keys);
        let actual = log::apply_writes(s,ready_writes(owner,keys));
        assert(actual.physical =~= ready_state(s,owner,keys).physical);
    } else {
        assert(keys.to_set() =~= Set::<int>::empty());
        assert(s.physical =~= ready_state(s,owner,keys).physical);
    }
}

/// Called from execute_final's successful branch: native after.seal_frame is
/// the actual participant effect, and complete comes from the streaming mirror.
/// ALL selected model keys must have source-certified delivery coverage first.
pub proof fn sealed(c: p::Constants,before: &Participant,after: &Participant,plan: MigrationPlan,
    s: p::State,keys: Seq<int>,labels: Map<int,Cell>,current: Image,initial: Image,source: Image,
    writers: Map<int,int>,code: spec_fn(Seq<u8>)->int) -> (tracked out: ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(before,plan,s,labels),
        before.transfer_authorized(plan,false),after.seal_frame(*before,plan),
        after.owner_view() == before.owner_view(),
        crate::storage::complete(current,initial,source,plan.range),
        forall|k: int| keys.to_set().contains(k) ==> s.physical.contains_key((plan.destination as int,k))
            && s.physical[(plan.destination as int,k)].covered
            && s.physical[(plan.destination as int,k)].cell == transfer::observed(current,labels[k],writers[k],code),
    ensures out.valid(c,s),out.after(s).certificates.contains((plan.generation as nat,p::Certificate::Ready)),
        transfer::local_coupling(after,plan,out.after(s),labels),
{
    transfer::completed_coverage(current,initial,source,plan,keys,labels,writers,code);
    let g = plan.generation as nat;
    let owner = plan.destination as int;
    assert forall|k:int| s.plans[g].keys.contains(k) implies {
        let r = p::replica(s,owner,k);
        r.role is Stage && r.fence == g && !r.terminal && r.round == 1 && r.covered
    } by {
        assert(before.range_role(plan,crate::types::Role::Stage,Some(1),false));
        assert(crate::storage::in_range(plan.range,labels[k]));
        assert(crate::participant_proofs::metadata(before.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(s,owner,k)));
        // Reuse the metadata field replay theorem on the actual native result.
        crate::participant_proofs::metadata_replay(s,owner,k,after.local_meta(labels[k].0,labels[k].1.0).unwrap());
    }
    let writes = ready_writes(owner,keys);
    ready_replay(s,owner,keys);
    let certificate = log::Write::Certificate { generation:g,value:p::Certificate::Ready };
    log::append_write(s,writes,certificate);
    let writes = writes.push(certificate);
    let next = log::apply_writes(s,writes);
    assert(next.physical =~= p::apply(c,s,p::Action::Seal { generation:g }).physical);
    log::accepted(c,s,next,p::Action::Seal { generation:g });
    assert forall|k:int| s.plans[g].keys.contains(k) implies
        crate::participant_proofs::metadata(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(next,owner,k)) by {
        assert(crate::storage::in_range(plan.range,labels[k]));
    }
    ClosedTransfer { segment:log::Segment { writes,states:seq![s,next],actions:seq![p::Action::Seal { generation:g }] } }
}

pub proof fn cleaned(c:p::Constants,s:p::State,node:&Participant,plan:MigrationPlan,key:int,label:Cell,before:Image,after:Image)
    -> (tracked out:ClosedTransfer)
    requires node.transfer_authorized(plan,true),crate::storage::in_range(plan.range,label),
        node.local_meta(label.0,label.1.0).is_some(),
        crate::participant_proofs::metadata(node.local_meta(label.0,label.1.0).unwrap(),p::replica(s,node.owner_view() as int,key)),
        after == before.remove(label),
    ensures out.valid(c,s),out.after(s) == s,
{
    let segment = transfer::cleanup_stutter(c,s,node,plan,key,label,before,after);
    ClosedTransfer { segment }
}

/// Compose certificates produced above, never a caller-provided action path.
proof fn join(c:p::Constants,s:p::State,tracked first:ClosedTransfer,tracked second:ClosedTransfer)
    -> (tracked out:ClosedTransfer)
    requires first.valid(c,s),second.valid(c,first.after(s)),
    ensures out.valid(c,s),out.after(s) == second.after(first.after(s)),
{
    log::concat_writes(s,first.segment.writes,second.segment.writes);
    log::concat_path(c,first.segment.states,first.segment.actions,second.segment.states,second.segment.actions);
    ClosedTransfer { segment:log::Segment {
        writes:first.segment.writes + second.segment.writes,
        states:first.segment.states + second.segment.states.drop_first(),
        actions:first.segment.actions + second.segment.actions,
    } }
}

proof fn empty_segment(c:p::Constants,s:p::State) -> (tracked out:ClosedTransfer)
    ensures out.valid(c,s),out.after(s) == s,
{
    ClosedTransfer { segment:log::Segment { writes:Seq::empty(),states:seq![s],actions:Seq::empty() } }
}
proof fn copy_sequence(c:p::Constants,node:&Participant,plan:MigrationPlan,s:p::State,
    keys:Seq<int>,labels:Map<int,Cell>,work:Seq<int>,cells:Map<int,p::Cell>)
    -> (tracked out:ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(node,plan,s,labels),
        node.transfer_authorized(plan,false),work.to_set().subset_of(keys.to_set()),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((plan.destination as int,k)),
        forall|k:int| work.to_set().contains(k) ==> s.packets.contains(transfer::packet(plan.generation as nat,k,cells[k])),
    ensures out.valid(c,s),out.after(s).plans == s.plans,out.after(s).packets == s.packets,
        transfer::local_coupling(node,plan,out.after(s),labels),
        out.after(s).certificates == s.certificates,
        forall|q:(int,int)| q.0 != plan.destination || !work.to_set().contains(q.1) ==>
            out.after(s).physical[q] == s.physical[q],
        forall|k:int| keys.to_set().contains(k) ==> out.after(s).physical.contains_key((plan.destination as int,k)),
        forall|k:int| work.to_set().contains(k) ==> out.after(s).physical[(plan.destination as int,k)].cell == cells[k]
            && out.after(s).physical[(plan.destination as int,k)].covered,
    decreases work.len(),
{
    hide(log::certificate);
    if work.len() == 0 {
        empty_segment(c,s)
    } else {
        transfer::split_set(work);
        assert(work.drop_last().to_set().subset_of(work.to_set()));
        let tracked prefix = copy_sequence(c,node,plan,s,keys,labels,work.drop_last(),cells);
        let mid = prefix.after(s);
        let key = work.last();
        assert(work.to_set().contains(key));
        let step = transfer::copy_segment(c,node,plan,mid,keys,labels,key,cells[key]);
        let next = log::apply_writes(mid,step.writes);
        assert forall|k:int| keys.to_set().contains(k) implies
            crate::participant_proofs::metadata(node.local_meta(labels[k].0,labels[k].1.0).unwrap(),
                p::replica(next,plan.destination as int,k)) by {}
        assert forall|k:int| work.to_set().contains(k) implies next.physical[(plan.destination as int,k)].cell == cells[k]
            && next.physical[(plan.destination as int,k)].covered by {
            if k != key { assert(work.drop_last().to_set().contains(k)); }
        }
        join(c,s,prefix,ClosedTransfer { segment:step })
    }
}

/// Macrostep closure for the actual successful execute_final postcondition.
/// Source packets are authenticated captures (including EOF/gap absences).
/// No cell value is taken from a desired model endpoint: raw cells below are
/// the final native bytes plus the captured source writers. The complete cursor
/// theorem proves these agree, and the sequence covers EVERY model key.
pub proof fn completed_final(c:p::Constants,before:&Participant,after:&Participant,
    plan:MigrationPlan,s:p::State,keys:Seq<int>,labels:Map<int,Cell>,initial:Image,current:Image,source:Image,
    writers:Map<int,int>,code:spec_fn(Seq<u8>)->int) -> (tracked out:ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(before,plan,s,labels),
        before.transfer_authorized(plan,false),after.seal_frame(*before,plan),
        after.owner_view() == before.owner_view(),
        crate::storage::complete(current,initial,source,plan.range),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((plan.destination as int,k))
            && s.packets.contains(transfer::packet(plan.generation as nat,k,transfer::observed(source,labels[k],writers[k],code))),
    ensures out.valid(c,s),out.after(s).certificates.contains((plan.generation as nat,p::Certificate::Ready)),
        transfer::local_coupling(after,plan,out.after(s),labels),
{
    transfer::completed_coverage(current,initial,source,plan,keys,labels,writers,code);
    let cells = Map::new(keys.to_set(),|k:int| transfer::observed(current,labels[k],writers[k],code));
    let tracked copied = copy_sequence(c,before,plan,s,keys,labels,keys,cells);
    let mid = copied.after(s);
    assert forall|k:int| keys.to_set().contains(k) implies mid.physical.contains_key((plan.destination as int,k))
        && mid.physical[(plan.destination as int,k)].covered
        && mid.physical[(plan.destination as int,k)].cell == transfer::observed(current,labels[k],writers[k],code) by {
        assert(cells[k] == transfer::observed(current,labels[k],writers[k],code));
        assert(copied.after(s).physical[(plan.destination as int,k)].covered);
    }
    let tracked ready = sealed(c,before,after,plan,mid,keys,labels,current,initial,source,writers,code);
    join(c,s,copied,ready)
}

/// Close the actually processed prefix after a failed final-copy call.
/// `cursor` witnesses storage::mirror's executable prefix contract.
/// Captured packets cover precisely the processed model keys, including gaps.
/// Uncovered Stage cells remain private: Start may have masked retained bytes.
/// Only a later successful completed_final may append Seal/emit Ready.
pub proof fn partial_final(c:p::Constants,node:&Participant,after_node:&Participant,plan:MigrationPlan,
    s:p::State,keys:Seq<int>,labels:Map<int,Cell>,initial:Image,current:Image,source:Image,
    cursor:Option<crate::storage::Identity>,source_writers:Map<int,int>,result:crate::participant::ControlResult,
    code:spec_fn(Seq<u8>)->int) -> (tracked out:ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(node,plan,s,labels),
        node.transfer_authorized(plan,false),after_node.local_unchanged(*node),
        result.status != crate::types::Status::Ok,result.certificate.is_none(),after_node.same_metadata(*node),
        crate::storage::mirrored(current,initial,source,plan.range,cursor),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((plan.destination as int,k))
            && (p::replica(s,plan.destination as int,k).covered ==>
                p::replica(s,plan.destination as int,k).cell.value == transfer::observed(initial,labels[k],0,code).value),
        forall|k:int| keys.to_set().contains(k) && !crate::storage::beyond(cursor,labels[k].1) ==>
            source_writers.contains_key(k)
            && s.packets.contains(transfer::packet(plan.generation as nat,k,transfer::observed(source,labels[k],source_writers[k],code))),
    ensures out.valid(c,s),transfer::local_coupling(after_node,plan,out.after(s),labels),
        out.after(s).certificates == s.certificates,
        forall|k:int| keys.to_set().contains(k) && p::replica(out.after(s),plan.destination as int,k).covered ==>
            p::replica(out.after(s),plan.destination as int,k).cell.value == transfer::observed(current,labels[k],0,code).value,
        forall|k:int| keys.to_set().contains(k) && !crate::storage::beyond(cursor,labels[k].1) ==>
            p::replica(out.after(s),plan.destination as int,k).cell == transfer::observed(current,labels[k],source_writers[k],code),
{
    let processed = keys.to_set().filter(|k:int| !crate::storage::beyond(cursor,labels[k].1));
    processed.lemma_to_seq_to_set_id();
    let work = processed.to_seq();
    let cells = Map::new(processed,|k:int| transfer::observed(current,labels[k],source_writers[k],code));
    assert forall|k:int| processed.contains(k) implies cells[k] ==
        transfer::observed(source,labels[k],source_writers[k],code) by {
        assert(crate::storage::in_range(plan.range,labels[k]));
    }
    let tracked out = copy_sequence(c,node,plan,s,keys,labels,work,cells);
    assert forall|k:int| keys.to_set().contains(k) && p::replica(out.after(s),plan.destination as int,k).covered implies
        p::replica(out.after(s),plan.destination as int,k).cell.value == transfer::observed(current,labels[k],0,code).value by {
        assert(crate::storage::in_range(plan.range,labels[k]));
        if processed.contains(k) {
            assert(cells[k] == transfer::observed(current,labels[k],source_writers[k],code));
        } else {
            assert(out.after(s).physical[(plan.destination as int,k)] == s.physical[(plan.destination as int,k)]);
            assert(crate::storage::value(current,labels[k]) == crate::storage::value(initial,labels[k]));
        }
    }
    assert forall|k:int| keys.to_set().contains(k) && !crate::storage::beyond(cursor,labels[k].1) implies
        p::replica(out.after(s),plan.destination as int,k).cell == transfer::observed(current,labels[k],source_writers[k],code) by {
        assert(processed.contains(k));
        assert(work.to_set().contains(k));
        assert(out.after(s).physical[(plan.destination as int,k)].cell == cells[k]);
    }
    out
}

/// Rejected admission or I/O before any byte effect is a genuine stutter.
/// In contrast, terminal metadata delivery and processed copy prefixes use
/// their dedicated constructors even if the enclosing RPC ultimately fails.
pub proof fn transfer_unchanged(c:p::Constants,node:&Participant,after_node:&Participant,plan:MigrationPlan,
    s:p::State,labels:Map<int,Cell>,initial:Image,current:Image,result:crate::participant::ControlResult)
    -> (tracked out:ClosedTransfer)
    requires result.status != crate::types::Status::Ok,result.certificate.is_none(),
        current == initial,after_node.same_metadata(*node),after_node.local_unchanged(*node),
        after_node.lease_view() == node.lease_view(),transfer::local_coupling(node,plan,s,labels),
    ensures out.valid(c,s),out.after(s) == s,transfer::local_coupling(after_node,plan,s,labels),
{ empty_segment(c,s) }

/// Empty metadata masks private storage, so a partial cleanup prefix stutters
/// without inventing completion. The pending receipt and all metadata remain
/// unchanged; retries continue deletion under the retained incarnation guard.
pub proof fn partial_cleanup(c:p::Constants,node:&Participant,after_node:&Participant,plan:MigrationPlan,
    s:p::State,keys:Seq<int>,labels:Map<int,Cell>,initial:Image,current:Image,result:crate::participant::ControlResult)
    -> (tracked out:ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(node,plan,s,labels),
        node.transfer_authorized(plan,true),after_node.local_unchanged(*node),
        result.status != crate::types::Status::Ok,result.certificate.is_none(),after_node.same_metadata(*node),
        crate::storage::prefix(current,initial,Map::empty(),plan.range),
    ensures out.valid(c,s),out.after(s) == s,transfer::local_coupling(after_node,plan,s,labels),
        forall|k:int| keys.to_set().contains(k) ==> p::replica(s,node.owner_view() as int,k).role is Empty,
{
    assert forall|k:int| keys.to_set().contains(k) implies p::replica(s,node.owner_view() as int,k).role is Empty by {
        assert(crate::storage::in_range(plan.range,labels[k]));
        assert(node.range_role(plan,crate::types::Role::Empty,None,true));
    }
    empty_segment(c,s)
}

/// Delayed terminal delivery closes only after actual byte deletion. The native
/// deliver guard/effect, exact cleanup image, and retained cleanup receipt are
/// separate inputs from the executable contracts, not a supplied model action.
pub proof fn completed_terminal(c:p::Constants,before:&Participant,after:&Participant,
    plan:MigrationPlan,command:crate::types::Command,s:p::State,keys:Seq<int>,labels:Map<int,Cell>,
    initial:Image,current:Image) -> (tracked out:ClosedTransfer)
    requires transfer::labels_cover(plan,s,keys,labels),transfer::local_coupling(before,plan,s,labels),
        (command == crate::types::Command::Commit && before.owner_view() == plan.source)
            || (command == crate::types::Command::Abort && before.owner_view() == plan.destination),
        s.commands.contains((plan.generation as nat,crate::participant_proofs::command(command))),
        p::drained(s,s.plans[plan.generation as nat].keys) == before.drained_view(plan.range),
        after.control_frame(*before,plan,command),after.owner_view() == before.owner_view(),after.cleanup_done(plan.generation),
        crate::storage::complete(current,initial,Map::empty(),plan.range),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((before.owner_view() as int,k))
            && before.command_at(plan,command,before.drained_view(plan.range),labels[k].1.0)
            && s.plans[plan.generation as nat].old[k].epoch == crate::directory::route(plan.old@,labels[k].1.0).epoch,
    ensures out.valid(c,s),out.after(s).certificates.contains((plan.generation as nat,
        if before.owner_view() == plan.source { p::Certificate::SourceDone } else { p::Certificate::DestinationDone })),
        transfer::local_coupling(after,plan,out.after(s),labels),
{
    let g = plan.generation as nat;
    let owner = before.owner_view() as int;
    let coordinates = |k:int| labels[k].1.0;
    crate::participant_proofs::native_guard_matches(before,plan,command,s,coordinates);
    let records = Map::new(keys.to_set(),|k:int| transfer::terminal_record(
        after.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(s,owner,k)));
    assert forall|k:int| keys.to_set().contains(k) implies records[k] ==
        p::command_replica(p::replica(s,owner,k),g,crate::participant_proofs::command(command),owner == plan.source) by {
        crate::participant_proofs::native_effect_matches(before,after,plan,command,labels[k].1.0,p::replica(s,owner,k));
        assert(crate::storage::in_range(plan.range,labels[k]));
        crate::storage_refinement::absent_model_key(current,initial,Map::empty(),plan.range,labels[k]);
    }
    let writes = transfer::terminal_writes(owner,keys,records);
    transfer::terminal_replay(s,owner,keys,records);
    let certificate = if owner == plan.source { p::Certificate::SourceDone } else { p::Certificate::DestinationDone };
    let w = log::Write::Certificate { generation:g,value:certificate };
    log::append_write(s,writes,w);
    let writes = writes.push(w);
    let next = log::apply_writes(s,writes);
    let action = p::Action::Deliver { generation:g,command:crate::participant_proofs::command(command),owner };
    assert(next.physical =~= p::delivered(s,g,crate::participant_proofs::command(command),owner).physical);
    log::accepted(c,s,next,action);
    assert forall|k:int| keys.to_set().contains(k) implies
        crate::participant_proofs::metadata(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(next,owner,k)) by {}
    ClosedTransfer { segment:log::Segment { writes,states:seq![s,next],actions:seq![action] } }
}

/// This ledger can only append a source-certified token; no arbitrary sequence
/// of model actions or asserted handler outcome is accepted by its public API.
pub tracked struct Execution { ghost journal: log::Journal }
impl Execution {
    pub closed spec fn closed(&self,c:p::Constants,actual:p::State) -> bool {
        log::closed(c,p::initial(c),self.journal,actual)
    }
    pub proof fn new(c:p::Constants) -> (tracked out:Self)
        requires p::constants_ok(c),ensures out.closed(c,p::initial(c)),
    {
        let journal = log::initialize(c,p::initial(c));
        Self { journal }
    }
    pub proof fn append(tracked self,c:p::Constants,before:p::State,tracked segment:ClosedTransfer) -> (tracked out:Self)
        requires self.closed(c,before),segment.valid(c,before),
        ensures out.closed(c,segment.after(before)),
    {
        let journal = log::open_segment(c,p::initial(c),self.journal,before);
        let next = segment.after(before);
        let journal = log::record_batch(c,p::initial(c),journal,before,next,segment.segment.writes);
        let journal = log::close_segment(c,p::initial(c),journal,next,segment.segment.states,segment.segment.actions);
        Self { journal }
    }
    pub proof fn finite_history(tracked &self,c:p::Constants,actual:p::State)
        requires p::constants_ok(c),self.closed(c,actual),
        ensures p::inv(c,actual),exists|states:Seq<p::State>| p::behavior(c,states) && states.last() == actual,
    {
        log::finite_behavior(c,self.journal.closed);
        log::flatten(c,p::initial(c),self.journal.closed);
        let states = log::behavior_states(p::initial(c),self.journal.closed);
        assert(p::behavior(c,states) && states.last() == actual);
    }
}
} // verus!
