//! Shared-process drain projection, not an independent-owner clock refinement.
//! `physical.readers` is a ghost enumeration of ALL retained engine/logical
//! guards in one process (including guards with no sharding owner). The native
//! representation is still one oldest announcement and thread-local depth per
//! worker. `embedding` relates owner-local proof readers to those guards.
//! Publication/recheck ordering and establishing this embedding at native entry
//! remain the explicit correspondence boundary; this theorem proves the drain
//! implication once that relation holds. A different process has a different
//! physical state and embedding: no cross-process counter comparison is used.
use vstd::prelude::*;
use super::admission;

verus! {

pub open spec fn retained(s: admission::State, id: int) -> bool {
    s.readers.contains_key(id) && s.readers[id].announced && !s.readers[id].finished
}
pub open spec fn shared_domain(physical: admission::State,
    owners: Map<int,admission::State>, embedding: Map<(int,int),int>) -> bool {
    &&& (forall|owner: int| #[trigger] owners.contains_key(owner) ==>
        owners[owner].epoch == physical.epoch)
    &&& (forall|owner: int, id: int| owners.contains_key(owner) && retained(owners[owner],id) ==>
        embedding.contains_key((owner,id)) && retained(physical,embedding[(owner,id)])
        && physical.readers[embedding[(owner,id)]].worker == owners[owner].readers[id].worker
        // Retaining an older enclosing guard is conservative, never younger.
        && physical.readers[embedding[(owner,id)]].epoch <= owners[owner].readers[id].epoch)
}

/// The slot is the minimum of every physical guard, not merely one owner's
/// guards. Thus an older colocated owner can only delay, never accelerate, cut.
pub proof fn every_colocated_guard_bounds_slot(physical: admission::State,
    owners: Map<int,admission::State>, embedding: Map<(int,int),int>, owner: int, id: int)
    requires shared_domain(physical,owners,embedding), owners.contains_key(owner),
        retained(owners[owner],id)
    ensures admission::worker_slot(physical,owners[owner].readers[id].worker) is Some,
        admission::worker_slot(physical,owners[owner].readers[id].worker).unwrap()
            <= owners[owner].readers[id].epoch
{
    admission::proofs::retained_guard_bounds_worker_slot(physical,embedding[(owner,id)]);
}

pub proof fn physical_cut_entails_each_owner_drain(physical: admission::State,
    owners: Map<int,admission::State>, embedding: Map<(int,int),int>, cut: nat)
    requires shared_domain(physical,owners,embedding), admission::slots_drained(physical,cut)
    ensures forall|owner: int| owners.contains_key(owner) ==> admission::drained(owners[owner],cut)
{
    assert forall|owner: int| owners.contains_key(owner)
        implies admission::drained(owners[owner],cut) by {
        assert forall|id: int| owners[owner].readers.contains_key(id)
            && owners[owner].readers[id].announced && !owners[owner].readers[id].finished
            implies cut <= owners[owner].readers[id].epoch by {
            every_colocated_guard_bounds_slot(physical,owners,embedding,owner,id);
        }
    }
}

/// The single physical scan justifies each owner's abstract Complete step.
/// Histories remain owner-local; epoch labels/cuts are from this ONE domain.
pub proof fn completed_physical_cut_enables_local_completion(physical: admission::State,
    owners: Map<int,admission::State>, embedding: Map<(int,int),int>, cut: nat, owner: int)
    requires shared_domain(physical,owners,embedding), owners.contains_key(owner),
        admission::slots_drained(physical,cut), 0 < cut <= physical.epoch,
        owners[owner].completed <= cut
    ensures admission::enabled(owners[owner],admission::Action::Complete { cut })
{
    physical_cut_entails_each_owner_drain(physical,owners,embedding,cut);
}

} // verus!
