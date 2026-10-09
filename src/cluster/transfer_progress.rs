//! Source connection for the raw progress protocol. The dense labels enumerate
//! the actual selected physical image union (including retained private bytes),
//! not merely the coordinator's logical keys. These lemmas consume exactly the
//! successful Source/RawStore scan/put/delete contracts used by storage.rs.
use vstd::prelude::*;
use crate::storage::{self,Cell,Image,Identity};
use crate::types::{MigrationPlan,KeyRange,Certificate,Status,Row};
use crate::participant::{Participant,ControlResult};
use crate::transfer_proofs as transfer;
use crate::execution_refinement::{self as execution,ClosedTransfer,EmittedCertificate};
use crate::sharding_placement as p;
use crate::sharding_progress::io;
use crate::sharding_progress::{actual as a,service};
use service::physical;

verus! {

pub open spec fn indices(labels:Seq<Cell>) -> Set<nat> {
    physical::indices(labels)
}
pub open spec fn encode(image:Image,labels:Seq<Cell>) -> Map<nat,Seq<u8>> {
    physical::encode(image,labels)
}
pub open spec fn inventory(image:Image,r:KeyRange,labels:Seq<Cell>) -> bool {
    forall|k:Cell| storage::in_range(r,k) && image.contains_key(k) ==> labels.to_set().contains(k)
}
pub open spec fn ordered(r:KeyRange,labels:Seq<Cell>) -> bool {
    &&& forall|i:int| 0 <= i < labels.len() ==> storage::in_range(r,labels[i])
    &&& forall|i:int,j:int| 0 <= i < labels.len() && 0 <= j < labels.len() ==>
        storage::order(labels[i].1,labels[j].1) == if i < j { -1 } else if i == j { 0 } else { 1 }
    &&& labels.no_duplicates()
}
pub open spec fn cursor(labels:Seq<Cell>,after:Option<Identity>,from:nat) -> bool {
    from <= labels.len() && forall|i:int| 0 <= i < labels.len() ==>
        storage::beyond(after,labels[i].1) == (i >= from)
}
pub open spec fn row_index(r:KeyRange,labels:Seq<Cell>,row:Option<Row>) -> Option<nat> {
    match row {
        None => None,
        Some(row) => Some((choose|i:int| 0 <= i < labels.len() && labels[i] == storage::row_cell(r.table,row)) as nat),
    }
}
pub proof fn index_membership(labels:Seq<Cell>,i:nat)
    ensures indices(labels).contains(i) == (i < labels.len())
{
    if i < labels.len() {
        assert(Seq::new(labels.len(),|j:int| j as nat)[i as int] == i);
    }
}
pub proof fn encoded_lookup(image:Image,labels:Seq<Cell>,i:nat)
    requires i < labels.len()
    ensures io::value(encode(image,labels),i) == storage::value(image,labels[i as int])
{
    index_membership(labels,i);
}
pub proof fn encoded_bounded(image:Image,labels:Seq<Cell>)
    ensures io::bounded(encode(image,labels),labels.len())
{
    assert forall|i:nat| encode(image,labels).contains_key(i) implies i < labels.len() by {
        index_membership(labels,i);
    }
}

/// A successful native cursor result supplies the exact first-row primitive,
/// including EOF absence. Errors are NOT accepted as an empty scan response.
pub proof fn native_scan_step(image:Image,r:KeyRange,labels:Seq<Cell>,after:Option<Identity>,from:nat,row:Option<Row>)
    requires ordered(r,labels),inventory(image,r,labels),cursor(labels,after,from),
        storage::first(image,r,after,row)
    ensures io::first(encode(image,labels),from,row_index(r,labels,row)),
        row is Some ==> row_index(r,labels,row) is Some
            && row_index(r,labels,row).unwrap() < labels.len()
            && encode(image,labels)[row_index(r,labels,row).unwrap()] == row.unwrap().value@
{
    match row {
        None => {
            assert forall|i:nat| i >= from implies !encode(image,labels).contains_key(i) by {
                index_membership(labels,i);
            }
        },
        Some(row) => {
            let cell = storage::row_cell(r.table,row);
            assert(labels.to_set().contains(cell));
            let i = choose|i:int| 0 <= i < labels.len() && labels[i] == cell;
            assert(row_index(r,labels,Some(row)) == Some(i as nat)) by {
                let j = row_index(r,labels,Some(row)).unwrap() as int;
                assert(labels[j] == labels[i]);
                assert(i == j);
            }
            encoded_lookup(image,labels,i as nat);
            assert forall|j:nat| from <= j < i implies !encode(image,labels).contains_key(j) by {
                index_membership(labels,j);
                if encode(image,labels).contains_key(j) {
                    assert(storage::order(cell.1,labels[j as int].1) <= 0);
                    assert(storage::order(cell.1,labels[j as int].1) == 1);
                }
            }
        },
    }
}

pub proof fn native_put_step(image:Image,labels:Seq<Cell>,i:nat,bytes:Seq<u8>)
    requires i < labels.len(),labels.no_duplicates()
    ensures encode(image.insert(labels[i as int],bytes),labels) == encode(image,labels).insert(i,bytes)
{
    assert forall|j:nat| encode(image.insert(labels[i as int],bytes),labels).contains_key(j)
        == encode(image,labels).insert(i,bytes).contains_key(j) by {
        index_membership(labels,j);
        if j < labels.len() && labels[j as int] == labels[i as int] { assert(j == i); }
    }
    assert(encode(image.insert(labels[i as int],bytes),labels) =~= encode(image,labels).insert(i,bytes));
}
pub proof fn native_delete_step(image:Image,labels:Seq<Cell>,i:nat)
    requires i < labels.len(),labels.no_duplicates()
    ensures encode(image.remove(labels[i as int]),labels) == encode(image,labels).remove(i)
{
    assert forall|j:nat| encode(image.remove(labels[i as int]),labels).contains_key(j)
        == encode(image,labels).remove(i).contains_key(j) by {
        index_membership(labels,j);
        if j < labels.len() && labels[j as int] == labels[i as int] { assert(j == i); }
    }
    assert(encode(image.remove(labels[i as int]),labels) =~= encode(image,labels).remove(i));
}

/// Exact raw-image equality discharges COMPLETE, not merely the visible model
/// cells. Private stale rows cannot disappear through the encoding inventory.
pub proof fn raw_complete(current:Image,initial:Image,source:Image,r:KeyRange,labels:Seq<Cell>)
    requires inventory(current,r,labels),inventory(source,r,labels),
        encode(current,labels) == encode(source,labels),storage::preserves_outside(current,initial,r)
    ensures storage::complete(current,initial,source,r)
{
    assert forall|k:Cell| storage::value(current,k) ==
        if storage::in_range(r,k) { storage::value(source,k) } else { storage::value(initial,k) } by {
        if storage::in_range(r,k) && (current.contains_key(k) || source.contains_key(k)) {
            assert(labels.to_set().contains(k));
            let i = choose|i:int| 0 <= i < labels.len() && labels[i] == k;
            encoded_lookup(current,labels,i as nat); encoded_lookup(source,labels,i as nat);
        }
    }
}

/// A source-connected infinite raw trace, whose individual native operations
/// use the three lemmas above, completes without assuming a successful pass.
pub proof fn native_raw_eventual_complete(run:io::Execution,base:nat,stable:nat,
    physical:spec_fn(nat)->Image,source:Image,r:KeyRange,labels:Seq<Cell>) -> (u:nat)
    requires io::runs(run,base),io::primitive_service(run,base),io::stable_raw_io(run,stable),
        run.state(base).source == encode(source,labels),
        inventory(source,r,labels),
        forall|t:nat| t >= base
            && (forall|v:nat| base <= v < t ==> !(run.state(v).stage is Done)) ==>
            inventory(physical(t),r,labels)
                && run.state(t).image == encode(physical(t),labels)
                && storage::preserves_outside(physical(t),physical(base),r)
    ensures u >= base,run.state(u).stage is Done,
        storage::complete(physical(u),physical(base),source,r)
{
    let start = if base > stable { base } else { stable };
    let end = io::eventually_emits(run,base,stable,start);
    let u = io::first_completion(run,base,end);
    io::state_wf(run,base,u);
    raw_complete(physical(u),physical(base),source,r,labels);
    u
}

/// Calls the real cleanup, including its terminating loop and private-capability
/// consumer. No successful result or completed cleanup is a premise.
pub fn cleanup_with_receipt<K:crate::transfer::RawStore>(node:&mut Participant,store:&mut K,plan:&MigrationPlan,
    Ghost(c):Ghost<p::Constants>,Ghost(s):Ghost<p::State>,Ghost(keys):Ghost<Seq<int>>,Ghost(labels):Ghost<Map<int,Cell>>)
    -> (out:(ControlResult,Tracked<(ClosedTransfer,EmittedCertificate)>))
    requires old(node).wf(),old(store).wf(),old(store).available(plan.range),
        old(node).envelope_view(*plan),old(node).transfer_authorized(*plan,true),
        transfer::labels_cover(*plan,s,keys,labels),transfer::local_coupling(old(node),*plan,s,labels),
        s.certificates.contains((plan.generation as nat,
            if old(node).owner_view() == plan.source { p::Certificate::SourceDone } else { p::Certificate::DestinationDone }))
    ensures out.0.status == Status::Ok,out.1@.0.valid(c,s),out.1@.0.after(s) == s,
        out.1@.1.matches(plan.generation,final(node).owner_view(),
            if final(node).owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }),
        storage::complete(final(store).image(plan.range),old(store).image(plan.range),Map::empty(),plan.range)
{
    let ghost before = *node;
    let ghost initial = store.image(plan.range);
    let result = crate::transfer::execute_cleanup(node,store,plan);
    let tracked certified;
    proof {
        certified = execution::cleanup_emitted(c,&before,node,*plan,result,s,keys,labels,initial,store.image(plan.range));
    }
    (result,Tracked(certified))
}

/// The actual successful Final event and its receiver are part of one source
/// chain. Receipt retention is deliberately absent from this interface predicate:
/// it follows from the native event and the intervening participant frames.
pub open spec fn final_source_history(c:p::Constants,ctx:execution::SourceContext,
    states:Seq<execution::SourceState>,events:Seq<execution::SourceEvent>,at:nat,
    record:execution::MetadataRecord,node:&Participant,plan:MigrationPlan)->bool {
    &&& execution::source_trace(c,ctx,states,events)
    &&& states.len() > 0 && at < events.len() && at < states.len()-1
    &&& events[at as int] == execution::SourceEvent::Metadata(record)
    &&& record.command == crate::types::Command::Final && record.result.status == Status::Ok
    &&& record.plan == plan && record.before.owner_view() == plan.destination
    &&& *node == states.last().nodes[record.before.owner_view()]
}

pub proof fn final_receipt_from_source(c:p::Constants,ctx:execution::SourceContext,
    states:Seq<execution::SourceState>,events:Seq<execution::SourceEvent>,at:nat,
    record:execution::MetadataRecord,node:&Participant,plan:MigrationPlan)
    requires final_source_history(c,ctx,states,events,at,record,node,plan)
    ensures node.has_receipt(plan.generation)
{
    let end = (states.len()-1) as nat;
    execution::retained_metadata_receipt(c,ctx,states,events,at,end,record);
}

/// The real final traversal and callback succeed from checked authorization,
/// stabilized primitive I/O, and an actual earlier Final delivery. There is no
/// caller-supplied has_receipt, completed pass, or successful callback result.
pub fn final_from_source<K:crate::transfer::RawStore,S:storage::Source>(
    node:&mut Participant,store:&mut K,plan:&MigrationPlan,source:&S,
    Ghost(c):Ghost<p::Constants>,Ghost(ctx):Ghost<execution::SourceContext>,
    Ghost(states):Ghost<Seq<execution::SourceState>>,Ghost(events):Ghost<Seq<execution::SourceEvent>>,
    Ghost(at):Ghost<nat>,Ghost(record):Ghost<execution::MetadataRecord>)->(result:ControlResult)
    requires old(node).wf(),old(store).wf(),source.wf(),
        old(node).envelope_view(*plan),old(node).transfer_authorized(*plan,false),
        old(store).available(plan.range),source.available(*plan),
        states.last().images.contains_key(plan.destination as int),
        states.last().images.contains_key(plan.source as int),
        old(store).image(plan.range) == states.last().images[plan.destination as int],
        source.image() == states.last().images[plan.source as int],
        final_source_history(c,ctx,states,events,at,record,old(node),*plan)
    ensures result.status == Status::Ok,result.certificate == Some(Certificate::Ready),
        final(node).wf(),final(store).wf(),
        storage::complete(final(store).image(plan.range),old(store).image(plan.range),source.image(),plan.range)
{
    proof { final_receipt_from_source(c,ctx,states,events,at,record,node,*plan); }
    crate::transfer::execute_final(node,store,plan,source)
}

/// The real mirror supplies CompletedCopy through storage::mirror's availability
/// postcondition. At this endpoint its real source-scan segments have already
/// produced the authentic packet history; seal's return is derived by executing
/// the checked callback, not supplied as an Ok premise.
pub fn seal_with_receipt(node:&mut Participant,plan:&MigrationPlan,completed:&storage::CompletedCopy,
    Ghost(c):Ghost<p::Constants>,Ghost(s):Ghost<p::State>,Ghost(keys):Ghost<Seq<int>>,Ghost(labels):Ghost<Map<int,Cell>>,
    Ghost(initial):Ghost<Image>,Ghost(current):Ghost<Image>,Ghost(source):Ghost<Image>,Ghost(writers):Ghost<Map<int,int>>,
    Ghost(ctx):Ghost<execution::SourceContext>,Ghost(states):Ghost<Seq<execution::SourceState>>,
    Ghost(events):Ghost<Seq<execution::SourceEvent>>,Ghost(at):Ghost<nat>,Ghost(record):Ghost<execution::MetadataRecord>)
    -> (out:(ControlResult,Tracked<(ClosedTransfer,EmittedCertificate)>))
    requires old(node).wf(),old(node).envelope_view(*plan),
        final_source_history(c,ctx,states,events,at,record,old(node),*plan),
        s == states.last().placement,labels == ctx.window(keys),
        states.last().images.contains_key(plan.destination as int),
        current == states.last().images[plan.destination as int],
        old(node).transfer_authorized(*plan,false),completed.matches_spec(*plan,old(node).owner_view()),
        storage::complete(current,initial,source,plan.range),
        transfer::labels_cover(*plan,s,keys,labels),transfer::local_coupling(old(node),*plan,s,labels),
        forall|k:int| keys.to_set().contains(k) ==> s.physical.contains_key((plan.destination as int,k))
            && s.packets.contains(transfer::packet(plan.generation as nat,k,transfer::observed(source,labels[k],writers[k])))
    ensures out.0.status == Status::Ok,out.0.certificate == Some(Certificate::Ready),
        out.1@.0.valid(c,s),out.1@.1.matches(plan.generation,final(node).owner_view(),Certificate::Ready)
{
    proof { final_receipt_from_source(c,ctx,states,events,at,record,node,*plan); }
    let ghost before = *node;
    let result = node.seal(plan,completed);
    let tracked certified;
    proof {
        certified = execution::ready_emitted(c,&before,node,*plan,result,s,keys,labels,initial,current,source,writers);
    }
    (result,Tracked(certified))
}

pub open spec fn native_window(plan:MigrationPlan,labels:Map<int,Cell>)->physical::Window<Cell> {
    physical::Window { contains:|k:Cell| storage::in_range(plan.range,k),logical:labels }
}

pub open spec fn native_world(w:physical::World<Cell>,plan:MigrationPlan,labels:Map<int,Cell>)->bool {
    &&& w.window(plan.generation as nat) == native_window(plan,labels)
    // These are RawStore's selected-range views, not a quiescent whole engine.
    &&& forall|t:nat,owner:int,k:Cell| w.image(t,owner).contains_key(k) ==> storage::in_range(plan.range,k)
    &&& forall|id:nat| w.job(id).generation == plan.generation
        && w.issued_at(w.job(id).origin).contains(id) ==> ordered(plan.range,w.job(id).labels)
}

pub proof fn native_window_completion(current:Image,initial:Image,source:Image,plan:MigrationPlan,labels:Map<int,Cell>)
    requires physical::complete(current,initial,source,native_window(plan,labels))
    ensures storage::complete(current,initial,source,plan.range)
{
    assert forall|k:Cell| storage::value(current,k) ==
        if storage::in_range(plan.range,k) { storage::value(source,k) } else { storage::value(initial,k) } by {
        assert(physical::value(current,k) ==
            if native_window(plan,labels).includes(k) { physical::value(source,k) } else { physical::value(initial,k) });
    }
}

/// Native instantiation consumes the SAME physical World in every service
/// binding. Its Cell is storage::Cell and its values use sharding_bytes, so
/// private rows and arbitrary bytes cannot be replaced by placement masks or an
/// unrelated empty driver. Actual snapshot/I/O responses supply this timeline;
/// production construction of that adapter remains outside this proof round.
pub proof fn native_service_eventual_completion(c:p::Constants,b:a::Execution,w:physical::World<Cell>,
    net:service::Network,plan:MigrationPlan,labels:Map<int,Cell>,base:nat,keys:Seq<int>,prefix:Seq<p::State>)->(t:nat)
    requires p::constants_ok(c),p::behavior(c,prefix),prefix.last() == b.state(base),
        a::stable_steps(c,b,base),transfer::labels_cover(plan,b.state(base),keys,labels),
        a::finite_image(b.state(base),plan.generation as nat,keys),
        a::engines_settle(b,base,plan.generation as nat),a::finite_admission(b,base,plan.generation as nat),
        native_world(w,plan,labels),
        service::submitted_work(c,b,w,base,plan.generation as nat),
        service::submitted_receipts(c,b,w,net,base,plan.generation as nat),
        service::emission_registry(b,w,net,base,plan.generation as nat),
        service::wire_fair(c,b,net,base,plan.generation as nat)
    ensures t >= base,crate::sharding_progress::completed(b.state(t),plan.generation as nat),
        exists|id:nat,u:nat| w.job(id).generation == plan.generation
            && w.job(id).origin <= u <= t
            && w.job(id).owner == if b.state(t).phases[plan.generation as nat] is Committed { plan.source as int } else { plan.destination as int }
            && storage::complete(w.image(u,w.job(id).owner),w.image(w.job(id).origin,w.job(id).owner),Map::empty(),plan.range)
{
    let g = plan.generation as nat;
    let t = service::theorem_service_eventual_completion(c,b,w,net,base,g,keys,prefix);
    a::reachable_progress_shape(c,prefix); a::metadata_span(c,b,base,g,base,t);
    let owner = if b.state(t).phases[g] is Committed { b.state(t).plans[g].src } else { b.state(t).plans[g].dst };
    let witness = choose|witness:service::CleanupWitness| witness.valid(b,w,g,t);
    let id = witness.job;
    let u = witness.time;
    native_window_completion(w.image(u,owner),w.image(w.job(id).origin,owner),Map::empty(),plan,labels);
    t
}

} // verus!
