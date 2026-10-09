//! Extensional linkage for the real streaming driver's complete image.
//!
//! This is not placement-history refinement. The parent must attach each source
//! image to authentic Capture in Frozen+drained state, each successful destination
//! effect to DeliverCopy under its Stage/generation/round guard, and retain all
//! finite model-key coverage (including keys absent from BOTH physical images).
//! The scan proves those absences; it does not emit a physical delete for them.
//! A packet captured before abort is authentic old data, but cannot authorize
//! effects in a newer incarnation or make an aborted coordinator commit.
use vstd::prelude::*;
use crate::storage::*;
use crate::types::KeyRange;
use crate::sharding_mirror as scalar;
verus! {
broadcast use vstd::set_lib::range_set_properties;
/// Finite model keys are given dense nonnegative labels. Different rows sharing
/// a warehouse coordinate remain different Cells/labels. Labels may include
/// absent keys: omission from an engine cursor is NOT omission from the model.
pub open spec fn encode(image: Image,labels: Seq<Cell>) -> Map<int,Seq<u8>> {
    Map::new(Set::<int>::range(0,labels.len() as int).filter(|i: int| image.dom().contains(labels[i])),
        |i: int| image[labels[i]])
}
pub open spec fn label_work(labels: Seq<Cell>) -> Seq<int> {
    Seq::new(labels.len(),|i: int| i)
}
pub proof fn encoded_lookup(image: Image,labels: Seq<Cell>,i: int)
    requires 0 <= i < labels.len(),
    ensures scalar::lookup(encode(image,labels),i) == value(image,labels[i]),
{}
/// The actual driver's all-key postcondition discharges this premise. The
/// scalar worklist is a ghost expansion over finite model keys, including no-op
/// absent deliveries. No runtime model or magic scan coverage premise is used.
pub proof fn native_to_scalar(current: Image,initial: Image,source: Image,r: KeyRange,labels: Seq<Cell>)
    requires complete(current,initial,source,r),
        forall|i: int| 0 <= i < labels.len() ==> in_range(r,labels[i]),
    ensures encode(current,labels) == scalar::mirror(encode(source,labels),encode(initial,labels),label_work(labels),0,None),
        forall|i: int| 0 <= i < labels.len() ==>
            scalar::lookup(encode(current,labels),i) == value(source,labels[i]),
        forall|k: Cell| !in_range(r,k) ==> value(current,k) == value(initial,k),
{
    let work = label_work(labels);
    assert forall|i: int| encode(source,labels).dom().union(encode(initial,labels).dom()).contains(i)
        implies work.to_set().contains(i) by {
        assert(0 <= i < labels.len());
        assert(work[i] == i);
        assert(work.contains(i));
    }
    scalar::theorem_complete_mirror(encode(source,labels),encode(initial,labels),work,0,None);
    assert forall|i: int| scalar::lookup(encode(current,labels),i) ==
        scalar::lookup(scalar::mirror(encode(source,labels),encode(initial,labels),work,0,None),i) by {
        if 0 <= i < labels.len() {
            encoded_lookup(current,labels,i); encoded_lookup(source,labels,i);
        }
    }
    let encoded = encode(current,labels);
    let mirrored = scalar::mirror(encode(source,labels),encode(initial,labels),work,0,None);
    assert forall|i: int| encoded.dom().contains(i) == mirrored.dom().contains(i) by {
        assert(scalar::lookup(encoded,i) == scalar::lookup(mirrored,i));
    }
    assert forall|i: int| encoded.dom().contains(i) implies encoded[i] == mirrored[i] by {
        assert(scalar::lookup(encoded,i) == scalar::lookup(mirrored,i));
    }
    assert(encode(current,labels) =~= scalar::mirror(encode(source,labels),encode(initial,labels),work,0,None));
}
/// Coverage for an absent model key follows from cursor completeness and the
/// driver's frame invariant, even when that key never appeared in either scan.
pub proof fn absent_model_key(current: Image,initial: Image,source: Image,r: KeyRange,k: Cell)
    requires complete(current,initial,source,r),in_range(r,k),!source.dom().contains(k),
    ensures !current.dom().contains(k),
{ assert(value(current,k) == value(source,k)); }
} // verus!
