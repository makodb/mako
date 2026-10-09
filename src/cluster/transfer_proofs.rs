//! Source-certified transfer segments. Byte values and ghost writer provenance
//! come from the actual frozen source image; neither is computed by model.apply.
use vstd::prelude::*;
use crate::storage::{self, Image, Cell, Identity};
use crate::types::{MigrationPlan, Role};
use crate::participant::Participant;
use crate::participant_proofs as metadata;
use crate::sharding_placement as p;
use crate::ghost_log as log;
verus! {
pub proof fn split_set<T>(keys:Seq<T>)
    requires keys.len() > 0,
    ensures keys.to_set() == keys.drop_last().to_set().insert(keys.last()),
{
    assert(keys.drop_last().push(keys.last()) =~= keys);
    keys.drop_last().lemma_push_to_set_commute(keys.last());
}
/// One canonical injective representation is shared by every native event.
/// Writers are historical application provenance, including deleted keys;
/// absence does NOT reset writer to -1.
pub open spec fn observed(image: Image,label: Cell,writer: int) -> p::Cell {
    p::Cell { value: crate::sharding_bytes::value_option(storage::value(image,label)), writer }
}
pub proof fn observed_bytes(a:Image,b:Image,label_a:Cell,label_b:Cell,writer_a:int,writer_b:int)
    requires observed(a,label_a,writer_a).value == observed(b,label_b,writer_b).value,
    ensures storage::value(a,label_a) == storage::value(b,label_b),
{
    crate::sharding_bytes::value_option_injective(storage::value(a,label_a),storage::value(b,label_b));
}
pub open spec fn packet(g: nat,k: int,cell: p::Cell) -> p::Packet {
    p::Packet { generation:g,round:1,key:k,cell }
}
pub open spec fn labels_cover(plan: MigrationPlan,s: p::State,keys: Seq<int>,labels: Map<int,Cell>) -> bool {
    plan.source != plan.destination && s.plans.contains_key(plan.generation as nat)
        && s.plans[plan.generation as nat].src == plan.source
        && s.plans[plan.generation as nat].dst == plan.destination
        && keys.to_set() == s.plans[plan.generation as nat].keys
        && keys.no_duplicates() && labels.dom() == keys.to_set()
        && (forall|k: int| labels.dom().contains(k) ==> storage::in_range(plan.range,labels[k]))
        && (forall|a: int,b: int| labels.dom().contains(a) && labels.dom().contains(b)
            && labels[a] == labels[b] ==> a == b)
}
pub open spec fn local_coupling(node: &Participant,plan: MigrationPlan,s: p::State,labels: Map<int,Cell>) -> bool {
    forall|k: int| s.plans[plan.generation as nat].keys.contains(k) ==>
        node.local_meta(labels[k].0,labels[k].1.0).is_some()
        && metadata::metadata(node.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(s,node.owner_view() as int,k))
}

/// Both EOF and an ordered gap establish absence for model keys never returned
/// by the physical cursor. These ghost captures do not perform native deletes.
pub proof fn scan_absence(image: Image,range: crate::types::KeyRange,after: Option<Identity>,
    row: Option<crate::types::Row>,key: Cell)
    requires storage::first(image,range,after,row), storage::in_range(range,key),storage::beyond(after,key.1),
        match row { None => true, Some(r) => storage::order(key.1,storage::identity(r)) < 0 },
    ensures !image.dom().contains(key),
{
    if let Some(r) = row {
        if key.1.0 == r.coordinate@ { crate::bytes::cmp_laws(key.1.1,r.key@); }
        else { crate::bytes::cmp_laws(key.1.0,r.coordinate@); }
    }
}
pub open spec fn scan_covers(range:crate::types::KeyRange,after:Option<Identity>,row:Option<crate::types::Row>,key:Cell) -> bool {
    storage::in_range(range,key) && storage::beyond(after,key.1)
        && match row { None => true,Some(r) => key == storage::row_cell(range.table,r)
            || storage::order(key.1,storage::identity(r)) < 0 }
}
/// The local role/drain guard is derived from checked capture's postcondition.
/// Remaining premises are the global lease expansion and authentic source
/// byte/provenance embedding, not an assumed handler transition.
pub proof fn capture_segment(c: p::Constants,node: &Participant,plan: MigrationPlan,
    s: p::State,keys: Seq<int>,labels: Map<int,Cell>,source: Image,k: int,writer: int,
    cursor:Option<Identity>,row:Option<crate::types::Row>)
    -> (segment: log::Segment)
    requires labels_cover(plan,s,keys,labels),local_coupling(node,plan,s,labels),
        node.capture_authorized(plan),keys.to_set().contains(k),
        storage::first(source,plan.range,cursor,row),scan_covers(plan.range,cursor,row,labels[k]),
        p::drained(s,s.plans[plan.generation as nat].keys) == node.drained_view(plan.range),
        observed(source,labels[k],writer) == p::replica(s,plan.source as int,k).cell,
    ensures log::certificate(c,s,segment),
        segment.writes == seq![log::Write::Packet { value: packet(plan.generation as nat,k,observed(source,labels[k],writer)) }],
        segment.actions == seq![p::Action::Capture { generation:plan.generation as nat,round:1,key:k }],
{
    if row is None || labels[k] != storage::row_cell(plan.range.table,row.unwrap()) {
        scan_absence(source,plan.range,cursor,row,labels[k]);
    }
    let g = plan.generation as nat;
    assert forall|key: int| s.plans[g].keys.contains(key) implies {
        let r = p::replica(s,plan.source as int,key);
        r.role is Frozen && r.fence == g && !r.terminal
    } by {
        assert(storage::in_range(plan.range,labels[key]));
        assert(node.range_role(plan,Role::Frozen,None,false));
        assert(metadata::metadata(node.local_meta(labels[key].0,labels[key].1.0).unwrap(),p::replica(s,plan.source as int,key)));
    }
    let action = p::Action::Capture { generation:g,round:1,key:k };
    let write = log::Write::Packet { value: packet(g,k,observed(source,labels[k],writer)) };
    log::single_write(s,write);
    let after = log::apply_write(s,write);
    log::accepted(c,s,after,action);
    log::Segment { writes:seq![write],states:seq![s,after],actions:seq![action] }
}

pub open spec fn copy_writes(owner: int,key: int,cell: p::Cell) -> Seq<log::Write> {
    seq![log::Write::ReplicaCell { owner,key,cell },log::Write::ReplicaCovered { owner,key,covered:true }]
}
pub proof fn copy_segment(c: p::Constants,node: &Participant,plan: MigrationPlan,
    s: p::State,keys: Seq<int>,labels: Map<int,Cell>,key: int,cell: p::Cell)
    -> (segment: log::Segment)
    requires labels_cover(plan,s,keys,labels),local_coupling(node,plan,s,labels),
        node.transfer_authorized(plan,false),keys.to_set().contains(key),
        s.physical.contains_key((plan.destination as int,key)),s.packets.contains(packet(plan.generation as nat,key,cell)),
    ensures log::certificate(c,s,segment),segment.writes == copy_writes(plan.destination as int,key,cell),
        segment.actions == seq![p::Action::DeliverCopy { packet:packet(plan.generation as nat,key,cell) }],
        log::apply_writes(s,segment.writes) == (p::State { physical:s.physical.insert((plan.destination as int,key),
            p::Replica { cell,covered:true,..p::replica(s,plan.destination as int,key) }),..s }),
{
    let g = plan.generation as nat;
    let owner = plan.destination as int;
    let p = packet(g,key,cell);
    assert(node.range_role(plan,Role::Stage,Some(1),false));
    assert(storage::in_range(plan.range,labels[key]));
    assert(metadata::metadata(node.local_meta(labels[key].0,labels[key].1.0).unwrap(),p::replica(s,owner,key)));
    reveal_with_fuel(log::apply_writes,3);
    reveal_with_fuel(log::writes_ok,3);
    let writes = copy_writes(owner,key,cell);
    let after = log::apply_writes(s,writes);
    assert(p::copy_guard(s,p));
    assert(after.physical =~= p::apply(c,s,p::Action::DeliverCopy { packet:p }).physical);
    log::accepted(c,s,after,p::Action::DeliverCopy { packet:p });
    log::Segment { writes,states:seq![s,after],actions:seq![p::Action::DeliverCopy { packet:p }] }
}

/// One native point effect refines one copy delivery. The effect equality is
/// supplied by RawStore put/delete, not by the logger. Source packet provenance
/// is retained even for tombstones. Unrelated labels are framed by injectivity.
pub proof fn native_copy_effect(before: Image,after: Image,label: Cell,row_value: Option<Seq<u8>>,
    labels: Map<int,Cell>,key: int,writers_before: Map<int,int>,writers_after: Map<int,int>,
    source_cell: p::Cell)
    requires labels.dom().contains(key),labels[key] == label,
        forall|a: int,b: int| labels.dom().contains(a) && labels.dom().contains(b) && labels[a] == labels[b] ==> a == b,
        after == match row_value { Some(v) => before.insert(label,v), None => before.remove(label) },
        source_cell.value == crate::sharding_bytes::value_option(row_value),
        writers_after == writers_before.insert(key,source_cell.writer),
    ensures observed(after,label,writers_after[key]) == source_cell,
        forall|k: int| labels.dom().contains(k) && k != key ==>
            observed(after,labels[k],writers_after[k]) == observed(before,labels[k],writers_before[k]),
{}

/// End-to-end exact mirror coverage includes all finite model labels, including
/// absent-in-both keys. The source writers are carried from actual captures.
pub proof fn completed_coverage(current: Image,initial: Image,source: Image,plan: MigrationPlan,
    keys: Seq<int>,labels: Map<int,Cell>,writers: Map<int,int>)
    requires storage::complete(current,initial,source,plan.range),labels.dom() == keys.to_set(),
        forall|k: int| labels.dom().contains(k) ==> storage::in_range(plan.range,labels[k]),
    ensures forall|k: int| labels.dom().contains(k) ==>
        observed(current,labels[k],writers[k]) == observed(source,labels[k],writers[k]),
{
    let dense = Seq::new(keys.len(),|i: int| labels[keys[i]]);
    assert forall|i:int| 0 <= i < dense.len() implies storage::in_range(plan.range,dense[i]) by {
        assert(keys.to_set().contains(keys[i]));
    }
    crate::storage_refinement::native_to_scalar(current,initial,source,plan.range,dense);
}

/// Empty-role private bytes are deliberately hidden from the placement view.
/// Cleanup's physical delete is therefore a genuine abstraction stutter, while
/// complete_cleanup controls when the already justified terminal receipt leaves.
pub proof fn cleanup_stutter(c: p::Constants,s: p::State,node: &Participant,plan: MigrationPlan,
    key: int,label: Cell,before: Image,after: Image)
    -> (segment: log::Segment)
    requires node.transfer_authorized(plan,true),storage::in_range(plan.range,label),
        node.local_meta(label.0,label.1.0).is_some(),
        metadata::metadata(node.local_meta(label.0,label.1.0).unwrap(),p::replica(s,node.owner_view() as int,key)),
        after == before.remove(label),
    ensures log::certificate(c,s,segment),segment.writes.len() == 0,
        segment.actions == seq![p::Action::Stutter],p::replica(s,node.owner_view() as int,key).role is Empty,
{
    assert(node.range_role(plan,Role::Empty,None,true));
    log::stutter(c,s,s);
    log::Segment { writes:Seq::empty(),states:seq![s,s],actions:seq![p::Action::Stutter] }
}

/// Raw terminal record uses actual native metadata, not command_replica.
pub open spec fn terminal_record(m:crate::types::ReplicaMeta,old:p::Replica) -> p::Replica {
    p::Replica { cell:p::empty_cell(),role:metadata::role(m.role),epoch:m.epoch as nat,
        fence:m.fence as nat,terminal:m.terminal,round:m.round as nat,covered:old.covered }
}
pub open spec fn terminal_writes(owner:int,keys:Seq<int>,records:Map<int,p::Replica>) -> Seq<log::Write>
    decreases keys.len(),
{
    if keys.len() == 0 { Seq::empty() } else {
        terminal_writes(owner,keys.drop_last(),records).push(log::Write::Replica { owner,key:keys.last(),value:records[keys.last()] })
    }
}
pub open spec fn terminal_state(s:p::State,owner:int,keys:Seq<int>,records:Map<int,p::Replica>) -> p::State {
    p::State { physical:vstd::imap::IMap::new(|q:(int,int)| s.physical.contains_key(q),|q:(int,int)|
        if q.0 == owner && keys.to_set().contains(q.1) { records[q.1] } else { s.physical[q] }),..s }
}
pub proof fn terminal_replay(s:p::State,owner:int,keys:Seq<int>,records:Map<int,p::Replica>)
    requires forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((owner,k)),
    ensures log::writes_ok(s,terminal_writes(owner,keys,records)),
        log::apply_writes(s,terminal_writes(owner,keys,records)) == terminal_state(s,owner,keys,records),
    decreases keys.len(),
{
    if keys.len() > 0 {
        assert(keys.drop_last().to_set().subset_of(keys.to_set()));
        assert(keys.to_set().contains(keys.last()));
        terminal_replay(s,owner,keys.drop_last(),records);
        log::append_write(s,terminal_writes(owner,keys.drop_last(),records),
            log::Write::Replica { owner,key:keys.last(),value:records[keys.last()] });
        split_set(keys);
        let next = log::apply_writes(s,terminal_writes(owner,keys,records));
        assert(next.physical =~= terminal_state(s,owner,keys,records).physical);
    } else {
        assert(keys.to_set() =~= Set::<int>::empty());
        assert(s.physical =~= terminal_state(s,owner,keys,records).physical);
    }
}
} // verus!
