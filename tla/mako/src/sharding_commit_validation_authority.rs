//! Freeze-aware authority, independent of the directory publication number.
//! Histories enumerate coherent metadata publications; repeated loads can select
//! the same entry. Native atomics/RCU correspondence is a foreign obligation.
use vstd::prelude::*;
// Infinite maps/sets retain every coordinate, including unbounded empty tails.
use vstd::imap::IMap as Map;
use vstd::iset::ISet as Set;
use super::Grant;

verus! {

pub type Coordinate = (int, int); // logical table, ordered ownership coordinate
pub type Key = (int, int, int); // table, ownership coordinate, physical row identity
/// Warehouse-style ownership coordinates contain arbitrarily many row aliases.
/// This lift is ghost state; it does not enumerate or allocate those rows.
pub open spec fn lift_authority(owned: Map<Coordinate,Grant>) -> Map<Key,Grant> {
    Map::new(|k: Key| owned.contains_key((k.0,k.1)), |k: Key| owned[(k.0,k.1)])
}
pub proof fn every_row_alias_is_covered(owned: Map<Coordinate,Grant>, table: int, coordinate: int, row: int)
    requires owned.contains_key((table,coordinate))
    ensures lift_authority(owned).contains_key((table,coordinate,row)),
        lift_authority(owned)[(table,coordinate,row)] == owned[(table,coordinate)]
{}
pub struct Snapshot {
    pub incarnation: nat,
    pub epoch: nat,
    pub fenced: bool,
    pub serving: Map<Key, Grant>,
}
pub open spec fn earlier(a: Snapshot, b: Snapshot) -> bool {
    a.incarnation < b.incarnation || (a.incarnation == b.incarnation && a.epoch < b.epoch)
}
pub open spec fn change(s: Snapshot, serving: Map<Key, Grant>, fenced: bool) -> Snapshot {
    Snapshot { epoch: s.epoch + 1, serving, fenced, ..s }
}
pub open spec fn restart(s: Snapshot) -> Snapshot {
    Snapshot { incarnation: s.incarnation + 1, epoch: 0, fenced: true, serving: Map::empty() }
}
pub open spec fn history(h: Seq<Snapshot>) -> bool {
    h.len() > 0 && forall|i: int| 0 <= i < h.len() - 1 ==> earlier(#[trigger] h[i],h[i+1])
}
pub open spec fn exact(s: Snapshot, owner: int, footprint: Map<Key, Grant>) -> bool {
    forall|key: Key| footprint.contains_key(key) && footprint[key].owner == owner ==>
        s.serving.contains_key(key) && s.serving[key] == footprint[key]
}
/// Differential check; the fast branch needs an independently valid baseline.
pub open spec fn accepted(old: Snapshot, now: Snapshot, owner: int, footprint: Map<Key, Grant>) -> bool {
    !now.fenced && old.incarnation == now.incarnation
        && (old.epoch == now.epoch || exact(now,owner,footprint))
}
pub open spec fn interval(table: int, lo: int, hi: Option<int>) -> Set<Key> {
    Set::new(|k: Key| k.0 == table && lo <= k.1 && (hi is None || k.1 < hi.unwrap()))
}
pub open spec fn scan_footprint(routes: Map<Key, Grant>, table: int, lo: int, hi: Option<int>) -> Map<Key, Grant> {
    Map::new(|k: Key| interval(table,lo,hi).contains(k), |k: Key| routes[k])
}

/// A certificate for the captured immutable routing view covers later footprint
/// discovery without mutable per-operation admission. A first-key certificate
/// is not enough: every footprint grant must come from this certified view.
pub proof fn certificate_covers_future_footprint(baseline: Snapshot, owner: int,
    routes: Map<Key,Grant>, footprint: Map<Key,Grant>)
    requires exact(baseline,owner,routes), footprint.dom().subset_of(routes.dom()),
        forall|k: Key| footprint.contains_key(k) ==> footprint[k] == routes[k]
    ensures exact(baseline,owner,footprint)
{
    assert forall|k: Key| footprint.contains_key(k) && footprint[k].owner == owner
        implies baseline.serving.contains_key(k) && baseline.serving[k] == footprint[k] by {
        assert(routes.contains_key(k) && routes[k] == footprint[k]);
    }
}

pub proof fn mutation_invalidates(s: Snapshot, serving: Map<Key, Grant>, fenced: bool)
    ensures earlier(s,change(s,serving,fenced)), change(s,serving,fenced).epoch != s.epoch
{}
pub proof fn restart_rejects_old_incarnation(s: Snapshot, owner: int, footprint: Map<Key, Grant>)
    ensures earlier(s,restart(s)), !accepted(s,restart(s),owner,footprint)
{}
pub proof fn chronological(h: Seq<Snapshot>, i: int, j: int)
    requires history(h), 0 <= i < j < h.len()
    ensures earlier(h[i],h[j])
    decreases j - i
{
    if i + 1 < j { chronological(h,i,j-1); }
}
pub proof fn same_stamp_same_snapshot(h: Seq<Snapshot>, i: int, j: int)
    requires history(h), 0 <= i <= j < h.len(),
        h[i].incarnation == h[j].incarnation, h[i].epoch == h[j].epoch
    ensures i == j, h[i] == h[j]
{
    if i < j { chronological(h,i,j); }
}
/// Both the hot branch and exact fallback establish the same authority fact.
pub proof fn validation_establishes_authority(h: Seq<Snapshot>, i: int, j: int,
    owner: int, footprint: Map<Key, Grant>)
    requires history(h), 0 <= i <= j < h.len(), !h[i].fenced,
        exact(h[i],owner,footprint), accepted(h[i],h[j],owner,footprint)
    ensures !h[j].fenced, exact(h[j],owner,footprint)
{
    if h[i].epoch == h[j].epoch { same_stamp_same_snapshot(h,i,j); }
}
/// Scopes include absence, internal gaps, and an unbounded tail. This is not a
/// quantification over only rows returned by the storage engine.
pub proof fn scan_checks_every_coordinate(h: Seq<Snapshot>, i: int, j: int,
    owner: int, routes: Map<Key, Grant>, table: int, lo: int, hi: Option<int>)
    requires history(h), 0 <= i <= j < h.len(), !h[i].fenced,
        interval(table,lo,hi).subset_of(routes.dom()),
        exact(h[i],owner,scan_footprint(routes,table,lo,hi)),
        accepted(h[i],h[j],owner,scan_footprint(routes,table,lo,hi))
    ensures forall|k: Key| interval(table,lo,hi).contains(k) && routes[k].owner == owner ==>
        h[j].serving.contains_key(k) && h[j].serving[k] == routes[k]
{
    validation_establishes_authority(h,i,j,owner,scan_footprint(routes,table,lo,hi));
    let footprint = scan_footprint(routes,table,lo,hi);
    assert forall|k: Key| interval(table,lo,hi).contains(k) && routes[k].owner == owner
        implies h[j].serving.contains_key(k) && h[j].serving[k] == routes[k] by {
        assert(footprint.contains_key(k) && footprint[k] == routes[k]);
    }
}
/// No participant's success is silently used for a different participant.
pub proof fn all_participants_cover_scope(now: Map<int,Snapshot>, footprint: Map<Key,Grant>)
    requires forall|k: Key| footprint.contains_key(k) ==>
        now.contains_key(footprint[k].owner)
        && exact(now[footprint[k].owner],footprint[k].owner,footprint)
    ensures forall|k: Key| footprint.contains_key(k) ==>
        now[footprint[k].owner].serving.contains_key(k)
        && now[footprint[k].owner].serving[k] == footprint[k]
{}
/// The global routing number is deliberately absent from accepted(): an
/// unchanged routing publication cannot turn a Frozen range into Serving.
pub proof fn frozen_rejected(old: Snapshot, owner: int, key: Key, grant: Grant)
    requires grant.owner == owner
    ensures !accepted(old,change(old,Map::empty(),false),owner,Map::empty().insert(key,grant))
{
    let footprint = Map::empty().insert(key,grant);
    assert(footprint.contains_key(key) && footprint[key].owner == owner);
    assert(!change(old,Map::empty(),false).serving.contains_key(key));
}

/// Checked counterexample to treating any captured epoch as a certificate.
/// This is excluded by validation_establishes_authority's baseline premise.
pub proof fn unchanged_epoch_without_authorized_baseline_is_unsound()
{
    let key: Key = (0,0,7);
    let grant = Grant { owner: 0, version: 0 };
    let baseline = Snapshot { incarnation: 0, epoch: 5, fenced: false, serving: Map::empty() };
    let footprint = Map::empty().insert(key,grant);
    assert(accepted(baseline,baseline,0,footprint));
    assert(footprint.contains_key(key) && footprint[key].owner == 0);
    assert(!exact(baseline,0,footprint));
}

} // verus!
