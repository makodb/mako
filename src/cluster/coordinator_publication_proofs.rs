//! Publication identity follows from the actual coordinator field-effect
//! contracts and its existing inductive migration routing invariant. The history
//! is ghost-only; it includes aborts, retries, receipts and administrative replies.
use vstd::prelude::*;
use crate::migration as n;
use crate::migration_invariants as mi;
use crate::durable_coordinator as d;
use crate::coordinator_publication::{published_tables,publication_wire};
use crate::routing::{Snapshot,same_snapshot};
use crate::directory_proofs::equivalent;
use crate::routing_codec_proofs::{snapshot_wire,word64,tables_wire,word64_injective,tables_unique};
use crate::types::Status;

verus! {

pub struct Step { pub operation: d::Operation, pub result: Result<d::Response,Status> }
pub open spec fn history(states: Seq<n::Native>, steps: Seq<Step>) -> bool {
    states.len() == steps.len()+1
        && d::initialized(states[0],states[0].nodes,states[0].snapshots)
        && (forall|i: int| 0 <= i < states.len() ==> n::layout(states[i]))
        && (forall|i: int| 0 <= i < steps.len() ==>
            d::effect(states[i],states[i+1],steps[i].operation,steps[i].result))
}

pub proof fn begin_publication(b: n::Native,z: n::Native,id: crate::types::TxnId,
    src: u32,dst: u32,range: crate::types::KeyRange,result: Result<u64,Status>)
    requires n::layout(b), mi::routing(b), n::begin_effect(b,z,id,src,dst,range,result)
    ensures mi::routing(z), z.published_version == b.published_version,
        published_tables(z) == published_tables(b)
{
    if let Ok(g) = result {
        mi::begin_preserves(b,z,id,src,dst,range,g);
        assert forall|i: int| 0 <= i < b.current.len() implies
            published_tables(z)[i] == published_tables(b)[i] by {
            assert(b.current[i] < b.snapshots.len());
            assert(z.snapshots[b.current[i] as int] == b.snapshots[b.current[i] as int]);
        }
        assert(published_tables(z) =~= published_tables(b));
    }
}

pub proof fn request_publication(b: n::Native,z: n::Native,r: n::Request,status: Status)
    requires n::layout(b), mi::routing(b), n::transition_effect(b,z,r,status)
    ensures mi::routing(z), b.published_version <= z.published_version,
        b.published_version == z.published_version ==> published_tables(b) == published_tables(z),
        r is Commit && status == Status::Ok ==> b.published_version < z.published_version
{
    mi::request_preserves(b,z,r,status);
    mi::publication_version_monotone(b,z,r,status);
    if r is Commit && status == Status::Ok {
        let i = b.active.unwrap() as int;
        assert(b.controls[i].phase is Retiring);
        assert(b.published_version < b.records[i].plan.generation);
    }
}

pub proof fn native_step(b: n::Native,z: n::Native,step: Step)
    requires n::layout(b), mi::routing(b), d::effect(b,z,step.operation,step.result)
    ensures mi::routing(z), b.published_version <= z.published_version,
        b.published_version == z.published_version ==> published_tables(b) == published_tables(z)
{
    match step.operation {
        d::Operation::Begin { nonce,source,destination,range } => {
            if !(exists|i: int| 0 <= i < b.records.len() && n::same_nonce(b.records[i].plan.nonce,nonce)) {
                match step.result {
                    Ok(d::Response::Begun(g)) => begin_publication(b,z,nonce,source,destination,range,Ok(g)),
                    Err(status) => begin_publication(b,z,nonce,source,destination,range,Err(status)),
                    _ => {},
                }
            }
        },
        d::Operation::Request(r) => match step.result {
            Ok(d::Response::Applied) => request_publication(b,z,r,Status::Ok),
            Err(status) => request_publication(b,z,r,status),
            _ => {},
        },
        d::Operation::Receive { generation,owner,certificate } => match step.result {
            Ok(d::Response::Applied) => mi::received_preserves(b,z,generation,owner,certificate,Status::Ok),
            Err(status) => mi::received_preserves(b,z,generation,owner,certificate,status),
            _ => {},
        },
        d::Operation::Reply(nonce) => match step.result {
            Ok(d::Response::Replied(outcome)) => mi::reply_preserves(b,z,nonce,Some(outcome)),
            Err(Status::Retry) => mi::reply_preserves(b,z,nonce,None),
            _ => {},
        },
    }
}

pub proof fn native_prefix(states: Seq<n::Native>,steps: Seq<Step>,i: int)
    requires history(states,steps), 0 <= i < states.len()
    ensures mi::routing(states[i])
    decreases i
{
    if i == 0 { mi::initialized(states[0]); }
    else {
        native_prefix(states,steps,i-1);
        native_step(states[i-1],states[i],steps[i-1]);
    }
}

pub proof fn publication_interval(states: Seq<n::Native>,steps: Seq<Step>,i: int,j: int)
    requires history(states,steps), 0 <= i <= j < states.len()
    ensures states[i].published_version <= states[j].published_version,
        states[i].published_version == states[j].published_version ==>
            published_tables(states[i]) == published_tables(states[j])
    decreases j-i
{
    if i < j {
        publication_interval(states,steps,i,j-1);
        native_prefix(states,steps,j-1);
        native_step(states[j-1],states[j],steps[j-1]);
    }
}

pub proof fn unique_publication(states: Seq<n::Native>,steps: Seq<Step>,i: int,j: int)
    requires history(states,steps), 0 <= i < states.len(), 0 <= j < states.len(),
        states[i].published_version == states[j].published_version
    ensures published_tables(states[i]) == published_tables(states[j]),
        publication_wire(states[i]) == publication_wire(states[j])
{
    if i <= j { publication_interval(states,steps,i,j); }
    else { publication_interval(states,steps,j,i); }
}

/// Codec equality is semantic, not pointer identity of separately decoded Vecs.
pub open spec fn exported(v: n::Native,s: Snapshot) -> bool {
    s.version == v.published_version && s.tables.len() == v.current.len()
        && forall|i: int| 0 <= i < s.tables.len() ==>
            s.tables@[i].table == published_tables(v)[i].table
                && equivalent(s.tables@[i].boundaries@,published_tables(v)[i].boundaries@)
}

/// Successful decode already supplies snapshot_wire(decoded)==input. Combining
/// it with encode_publication's exact postcondition establishes this association;
/// no route, grant, ownership, or local Serving fact is assumed here.
pub proof fn decoded_export(v: n::Native,s: Snapshot)
    requires v.current.len() <= usize::MAX, snapshot_wire(s) == publication_wire(v)
    ensures exported(v,s)
{
    let data = snapshot_wire(s);
    assert(data.subrange(8,16) =~= word64(s.version));
    assert(data.subrange(8,16) =~= word64(v.published_version));
    word64_injective(s.version,v.published_version);
    assert(data.subrange(16,24) =~= word64(s.tables.len() as u64));
    assert(data.subrange(16,24) =~= word64(v.current.len() as u64));
    word64_injective(s.tables.len() as u64,v.current.len() as u64);
    assert(s.tables.len() == v.current.len());
    assert(data.skip(24) =~= tables_wire(s.tables@));
    assert(data.skip(24) =~= tables_wire(published_tables(v)));
    tables_unique(s.tables@,published_tables(v));
}

pub proof fn export_catalog(v: n::Native,s: Snapshot)
    requires n::layout(v), mi::routing(v), exported(v,s)
    ensures forall|i: int,j: int| 0 <= i < j < s.tables.len() ==> s.tables@[i].table != s.tables@[j].table
{
    assert forall|i: int,j: int| 0 <= i < j < s.tables.len() implies s.tables@[i].table != s.tables@[j].table by {
        assert(0 <= i < v.current.len() && 0 <= j < v.current.len());
        assert(s.tables@[i].table == published_tables(v)[i].table);
        assert(s.tables@[j].table == published_tables(v)[j].table);
        assert(published_tables(v)[i] == v.snapshots[v.current[i] as int]);
        assert(published_tables(v)[j] == v.snapshots[v.current[j] as int]);
        assert(s.tables@[i].table == mi::table(v,i));
        assert(s.tables@[j].table == mi::table(v,j));
        assert(mi::table(v,i) != mi::table(v,j));
    }
}

pub proof fn equal_version_exports(states: Seq<n::Native>,steps: Seq<Step>,i: int,j: int,a: Snapshot,b: Snapshot)
    requires history(states,steps), 0 <= i < states.len(), 0 <= j < states.len(),
        exported(states[i],a), exported(states[j],b), a.version == b.version
    ensures same_snapshot(a,b),
        forall|x: int,y: int| 0 <= x < y < a.tables.len() ==> a.tables@[x].table != a.tables@[y].table
{
    unique_publication(states,steps,i,j);
    native_prefix(states,steps,i);
    export_catalog(states[i],a);
    assert forall|k: int| 0 <= k < a.tables.len() implies
        a.tables@[k].table == b.tables@[k].table && equivalent(a.tables@[k].boundaries@,b.tables@[k].boundaries@) by {
        let t = published_tables(states[i])[k];
        assert(equivalent(a.tables@[k].boundaries@,t.boundaries@));
        assert(equivalent(b.tables@[k].boundaries@,t.boundaries@));
        assert forall|q: int| 0 <= q < a.tables@[k].boundaries.len() implies
            a.tables@[k].boundaries@[q].start@ == b.tables@[k].boundaries@[q].start@
                && a.tables@[k].boundaries@[q].grant == b.tables@[k].boundaries@[q].grant by {
            assert(a.tables@[k].boundaries@[q].start@ == t.boundaries@[q].start@);
            assert(b.tables@[k].boundaries@[q].start@ == t.boundaries@[q].start@);
        }
    }
}

} // verus!
