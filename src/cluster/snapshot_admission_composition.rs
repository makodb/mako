//! Native checked capture -> immutable coordinator publication -> receiver
//! certification -> unchanged AdmissionRecord -> existing placement refinement.
//!
//! Named foreign boundary: requests really came from the checked immutable
//! capture, transport preserves scope/grant/version, and snapshot bytes came
//! from the one fixed master through encode_publication/decode. Raw callers
//! cannot assert these origins by supplying a number; they use NO_SNAPSHOT.
//! History here is erased proof state over actual native effect contracts, not
//! a second runtime log or an assumed local-authority conclusion.
#![cfg(verus_keep_ghost)]
use vstd::prelude::*;
use crate::participant::Participant;
use crate::routing::{Snapshot,captured,captured_range,snapshot_lookup};
use crate::routing_proofs::identical_observations;
use crate::migration::Native;
use crate::types::{TxnId,RoutedGrant,Status,Role,NO_SNAPSHOT};
use crate::directory::{inside,hi_view};
use crate::execution_refinement as source;
use crate::sharding_placement as p;
#[path = "coordinator_publication_proofs.rs"]
pub mod publication;

verus! {

pub open spec fn certificate_origin(node: Participant,states: Seq<Native>) -> bool {
    node.certified_snapshot() is Some ==> exists|i: int| 0 <= i < states.len()
        && publication::exported(states[i],node.certified_snapshot().unwrap())
}
pub open spec fn point_origin(states: Seq<Native>,table: u64,key: Seq<u8>,r: RoutedGrant) -> bool {
    exists|i: int,snapshot: Snapshot| 0 <= i < states.len()
        && publication::exported(states[i],snapshot) && captured(snapshot,table,key,r)
}
pub open spec fn range_origin(states: Seq<Native>,table: u64,lo: Seq<u8>,hi: Option<Seq<u8>>,r: RoutedGrant) -> bool {
    exists|i: int,snapshot: Snapshot| 0 <= i < states.len()
        && publication::exported(states[i],snapshot) && captured_range(snapshot,table,lo,hi,r)
}

pub proof fn genuine_point_provenance(node: Participant,states: Seq<Native>,steps: Seq<publication::Step>,
    table: u64,key: Seq<u8>,r: RoutedGrant)
    requires publication::history(states,steps), certificate_origin(node,states), point_origin(states,table,key,r)
    ensures node.routed_point_provenance(table,key,r)
{
    if node.certified_snapshot() is Some && node.certified_snapshot().unwrap().version == r.snapshot_version {
        let cert = node.certified_snapshot().unwrap();
        let j = choose|j: int| 0 <= j < states.len() && publication::exported(states[j],cert);
        let (i,snapshot) = choose|i: int,snapshot: Snapshot| 0 <= i < states.len()
            && publication::exported(states[i],snapshot) && captured(snapshot,table,key,r);
        publication::equal_version_exports(states,steps,i,j,snapshot,cert);
        identical_observations(snapshot,cert,table,key);
    }
}

pub proof fn genuine_range_provenance(node: Participant,states: Seq<Native>,steps: Seq<publication::Step>,
    table: u64,lo: Seq<u8>,hi: Option<Seq<u8>>,r: RoutedGrant)
    requires publication::history(states,steps), certificate_origin(node,states), range_origin(states,table,lo,hi,r)
    ensures node.routed_range_provenance(table,lo,hi,r)
{
    if node.certified_snapshot() is Some && node.certified_snapshot().unwrap().version == r.snapshot_version {
        let cert = node.certified_snapshot().unwrap();
        let j = choose|j: int| 0 <= j < states.len() && publication::exported(states[j],cert);
        let (i,snapshot) = choose|i: int,snapshot: Snapshot| 0 <= i < states.len()
            && publication::exported(states[i],snapshot) && captured_range(snapshot,table,lo,hi,r);
        publication::equal_version_exports(states,steps,i,j,snapshot,cert);
        assert forall|key: Seq<u8>| inside(key,lo,hi) implies snapshot_lookup(cert,table,key) == Some(r.grant) by {
            identical_observations(snapshot,cert,table,key);
        }
    }
}

/// Even with a certificate, absence of genuine provenance cannot accidentally
/// collide with a published version: the certifier rejects the sentinel.
pub proof fn no_snapshot_uses_unconditional_admission(node: Participant,table: u64,key: Seq<u8>,hi: Option<Seq<u8>>,r: RoutedGrant)
    requires node.wf(), r.snapshot_version == NO_SNAPSHOT
    ensures node.routed_point_provenance(table,key,r), node.routed_range_provenance(table,key,hi,r)
{
    node.certified_authority();
}

/// Existing source/refinement coupling, not a promise that the local role is
/// Serving. It permits Frozen/Retired/Empty states and stale captured grants.
pub open spec fn source_context(node: Participant,id: TxnId,ctx: source::SourceContext,s: source::SourceState) -> bool {
    s.registries.contains_key(node.owner_view()) && s.registries[node.owner_view()] == node.lease_view()
        && source::session_observed(node.lease_view(),s.placement,id,ctx.addresses(),node.owner_view())
        && !s.placement.sessions[source::transaction(id)].resolved
        && forall|k: int| ctx.addresses().contains_key(k) ==>
            s.placement.physical.contains_key((node.owner_view() as int,k))
                && node.local_meta(ctx.addresses()[k].0,ctx.addresses()[k].1).is_some()
                && crate::participant_proofs::metadata(node.local_meta(ctx.addresses()[k].0,ctx.addresses()[k].1).unwrap(),
                    p::replica(s.placement,node.owner_view() as int,k))
}

/// This checked call actually invokes acquire_routed; its AdmissionRecord has
/// precisely the old unconditional authority/lease obligations. In particular
/// neither a local Serving premise nor a grant-authority premise appears above
/// the call. These Ghost parameters incur no production history allocation.
pub fn point_from_publication(node: &mut Participant,id: TxnId,table: u64,key: &[u8],r: RoutedGrant,
    Ghost(states): Ghost<Seq<Native>>,Ghost(steps): Ghost<Seq<publication::Step>>,
    Ghost(ctx): Ghost<source::SourceContext>,Ghost(s): Ghost<source::SourceState>) -> (out: Status)
    requires old(node).wf(), old(node).table_wf(table),
        publication::history(states,steps), certificate_origin(*old(node),states), point_origin(states,table,key@,r),
        source_context(*old(node),id,ctx,s)
    ensures final(node).wf(), final(node).same_metadata(*old(node)),
        old(node).point_acquire_effect(*final(node),id,table,key@,r.grant,out),
        (source::AdmissionRecord { before: *old(node),after: *final(node),id,grant: r.grant,status: out,
            call: source::AdmissionCall::Point { table,coordinate: key@ } }).observed(ctx,s)
{
    let ghost before = *node;
    proof { genuine_point_provenance(*node,states,steps,table,key@,r); }
    let out = node.acquire_routed(id,table,key,r);
    proof {
        before.lease_facts();
        node.lease_facts();
        node.metadata_observations(before);
    }
    out
}

pub fn range_from_publication(node: &mut Participant,id: TxnId,table: u64,lo: &[u8],hi: Option<&[u8]>,r: RoutedGrant,
    Ghost(states): Ghost<Seq<Native>>,Ghost(steps): Ghost<Seq<publication::Step>>,
    Ghost(ctx): Ghost<source::SourceContext>,Ghost(s): Ghost<source::SourceState>) -> (out: Status)
    requires old(node).wf(), publication::history(states,steps), certificate_origin(*old(node),states),
        range_origin(states,table,lo@,hi_view(hi),r), source_context(*old(node),id,ctx,s)
    ensures final(node).wf(), final(node).same_metadata(*old(node)),
        old(node).range_acquire_effect(*final(node),id,table,lo@,hi_view(hi),r.grant,out),
        (source::AdmissionRecord { before: *old(node),after: *final(node),id,grant: r.grant,status: out,
            call: source::AdmissionCall::Range { table,lo: lo@,hi: hi_view(hi) } }).observed(ctx,s)
{
    let ghost before = *node;
    proof { genuine_range_provenance(*node,states,steps,table,lo@,hi_view(hi),r); }
    let out = node.acquire_range_routed(id,table,lo,hi,r);
    proof {
        before.lease_facts();
        node.lease_facts();
        node.metadata_observations(before);
    }
    out
}

/// Reuse the original native-to-placement transfer theorem, including complete
/// owner-local registration, other-owner lease framing, and physical drain.
pub proof fn admitted_record_refines_placement(c: p::Constants,ctx: source::SourceContext,s: source::SourceState,
    record: source::AdmissionRecord) -> (tracked out: source::ClosedTransfer)
    requires p::inv(c,s.placement), s.registries_match(ctx), record.observed(ctx,s)
    ensures out.valid(c,s.placement), out.after(s.placement) == record.image(ctx,s).placement,
        record.image(ctx,s).registries_match(ctx)
{ record.certify(c,ctx,s) }

} // verus!
