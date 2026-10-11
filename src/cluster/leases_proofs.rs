//! Refinement of exact coordinate and interval leases to every logical key.
//! Addresses describe the logical universe, not the set of existing storage rows.
//! Native drain implies logical drain without a coverage premise; the converse
//! needs a represented key in every relevant scope/range INTERSECTION.
use vstd::prelude::*;
use crate::bytes;
use crate::types::{Grant, KeyRange, Status, TxnId};
use crate::leases::*;

verus! {

/// Distinct logical keys may share one warehouse coordinate. The map may include
/// absent rows; no storage enumeration or currently materialized-row premise is used.
pub type Addresses = Map<int, (u64, Seq<u8>)>;

pub open spec fn scope_grant(s: SessionView, table: u64, coordinate: Seq<u8>) -> Grant {
    s.holds[choose|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(table,coordinate)].grant
}

pub open spec fn expanded(s: SessionView, addresses: Addresses) -> Map<int, Grant> {
    Map::new(addresses.dom().filter(|k: int| s.scoped(addresses[k].0,addresses[k].1)),
        |k: int| scope_grant(s,addresses[k].0,addresses[k].1))
}

pub proof fn unique_grant(s: SessionView, table: u64, coordinate: Seq<u8>, grant: Grant)
    requires s.wf(), s.has(table,coordinate,grant),
    ensures s.scoped(table,coordinate), scope_grant(s,table,coordinate) == grant,
{
    let i = choose|i: int| 0 <= i < s.holds.len()
        && s.holds[i].contains(table,coordinate) && s.holds[i].grant == grant;
    let j = choose|j: int| 0 <= j < s.holds.len() && s.holds[j].contains(table,coordinate);
    common_point_overlaps(s.holds[i],s.holds[j],table,coordinate);
    if j < i { overlap_symmetric(s.holds[i],s.holds[j]); }
}

pub proof fn expanded_exact(s: SessionView, addresses: Addresses, key: int, grant: Grant)
    requires s.wf(), addresses.contains_key(key),
    ensures (expanded(s,addresses).contains_key(key) && expanded(s,addresses)[key] == grant)
        <==> s.has(addresses[key].0,addresses[key].1,grant),
{
    let t = addresses[key].0;
    let c = addresses[key].1;
    if s.has(t,c,grant) { unique_grant(s,t,c,grant); }
    if expanded(s,addresses).contains_key(key) {
        let i = choose|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(t,c);
        assert(s.has(t,c,s.holds[i].grant));
        unique_grant(s,t,c,s.holds[i].grant);
    }
}

pub proof fn aliases_share_grant(s: SessionView, addresses: Addresses, first: int, second: int)
    requires s.wf(), addresses.contains_key(first), addresses.contains_key(second),
        addresses[first] == addresses[second], expanded(s,addresses).contains_key(first),
    ensures expanded(s,addresses).contains_key(second),
        expanded(s,addresses)[first] == expanded(s,addresses)[second],
{}

pub proof fn begin_fence(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId, status: Status)
    requires begin_effect(before,after,id,status), before.contains_key(id.client),
    ensures after.contains_key(id.client), after[id.client].sequence >= before[id.client].sequence,
        (id.sequence < before[id.client].sequence
            || id.sequence == before[id.client].sequence && before[id.client].terminal)
            ==> status == Status::Invalid && after == before,
        after[id.client].sequence != before[id.client].sequence ==>
            before[id.client].terminal && before[id.client].holds.len() == 0
                && after[id.client].sequence == id.sequence && id.sequence > before[id.client].sequence,
{}

pub proof fn registration_accounting(before: SessionView, after: SessionView, scope: ScopeView,
    table: u64, coordinate: Seq<u8>, grant: Grant)
    requires registration(before,after,scope,Status::Ok),
    ensures after.has(table,coordinate,grant) <==> before.has(table,coordinate,grant)
        || (scope.contains(table,coordinate) && scope.grant == grant),
{
    if before.has(table,coordinate,grant) {
        let i = choose|i: int| 0 <= i < before.holds.len()
            && before.holds[i].contains(table,coordinate) && before.holds[i].grant == grant;
        assert(after.holds[i] == before.holds[i]);
    }
    if after.has(table,coordinate,grant) && after.holds != before.holds {
        let i = choose|i: int| 0 <= i < after.holds.len()
            && after.holds[i].contains(table,coordinate) && after.holds[i].grant == grant;
        if i < before.holds.len() { assert(before.has(table,coordinate,grant)); }
    }
    if scope.contains(table,coordinate) && scope.grant == grant {
        registration_success(before,after,scope);
        let i = choose|i: int| 0 <= i < after.holds.len()
            && after.holds[i].covers(scope) && after.holds[i].grant == scope.grant;
        covered_point(after.holds[i],scope,table,coordinate);
        assert(after.has(table,coordinate,grant));
    }
}

pub proof fn acquire_stability(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, grant: Grant, status: Status,
    held_table: u64, held_coordinate: Seq<u8>, held_grant: Grant)
    requires acquire_effect(before,after,id,table,coordinate,grant,status),
        before.contains_key(id.client), before[id.client].has(held_table,held_coordinate,held_grant),
    ensures after[id.client].has(held_table,held_coordinate,held_grant),
        before[id.client].terminal ==> after == before && status == Status::Invalid,
        before[id.client].scoped(table,coordinate) ==> after == before,
{
    let i = choose|i: int| 0 <= i < before[id.client].holds.len()
        && before[id.client].holds[i].contains(held_table,held_coordinate)
        && before[id.client].holds[i].grant == held_grant;
    assert(after[id.client].holds[i] == before[id.client].holds[i]);
    if before[id.client].scoped(table,coordinate) { assert(after =~= before); }
}

pub proof fn acquire_accounting(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, grant: Grant, held_table: u64, held_coordinate: Seq<u8>, held_grant: Grant)
    requires acquire_effect(before,after,id,table,coordinate,grant,Status::Ok),
    ensures after[id.client].has(held_table,held_coordinate,held_grant) <==>
        before[id.client].has(held_table,held_coordinate,held_grant)
        || (held_table == table && held_coordinate == coordinate && held_grant == grant),
{
    registration_accounting(before[id.client],after[id.client],point_scope(table,coordinate,grant),
        held_table,held_coordinate,held_grant);
}

pub proof fn acquire_expansion(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, grant: Grant, addresses: Addresses, key: int, held_grant: Grant)
    requires acquire_effect(before,after,id,table,coordinate,grant,Status::Ok),
        before[id.client].wf(), after[id.client].wf(), addresses.contains_key(key),
    ensures (expanded(after[id.client],addresses).contains_key(key)
            && expanded(after[id.client],addresses)[key] == held_grant) <==>
        (expanded(before[id.client],addresses).contains_key(key)
            && expanded(before[id.client],addresses)[key] == held_grant)
        || (addresses[key] == (table,coordinate) && held_grant == grant),
{
    acquire_accounting(before,after,id,table,coordinate,grant,addresses[key].0,addresses[key].1,held_grant);
    expanded_exact(before[id.client],addresses,key,held_grant);
    expanded_exact(after[id.client],addresses,key,held_grant);
}

/// Every logical key inside the interval acquires the full grant, including
/// absent rows and all warehouse aliases. Every key outside is unchanged.
pub proof fn acquire_range_expansion(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>, grant: Grant,
    addresses: Addresses, key: int, held_grant: Grant)
    requires acquire_range_effect(before,after,id,table,lo,hi,grant,Status::Ok),
        before[id.client].wf(), after[id.client].wf(), addresses.contains_key(key),
    ensures (expanded(after[id.client],addresses).contains_key(key)
            && expanded(after[id.client],addresses)[key] == held_grant) <==>
        (expanded(before[id.client],addresses).contains_key(key)
            && expanded(before[id.client],addresses)[key] == held_grant)
        || (addresses[key].0 == table && inside(lo,hi,addresses[key].1) && held_grant == grant),
{
    registration_accounting(before[id.client],after[id.client],range_scope(table,lo,hi,grant),
        addresses[key].0,addresses[key].1,held_grant);
    expanded_exact(before[id.client],addresses,key,held_grant);
    expanded_exact(after[id.client],addresses,key,held_grant);
}

pub proof fn register_guard_and_frame(before: Map<u64, SessionView>, after: Map<u64, SessionView>,
    id: TxnId, scope: ScopeView, status: Status)
    requires register_effect(before,after,id,scope,status),
    ensures frame(before,after,id.client),
        status != Status::Ok ==> after == before,
        status == Status::Ok ==> scope.valid() && before.contains_key(id.client)
            && before[id.client].sequence == id.sequence && !before[id.client].terminal
            && before[id.client].compatible(scope),
        before.contains_key(id.client) ==> after[id.client].sequence == before[id.client].sequence
            && after[id.client].terminal == before[id.client].terminal,
{
    if status != Status::Ok { assert(after =~= before); }
    assert(before.dom() =~= after.dom());
    assert forall|c: u64| before.contains_key(c) && c != id.client implies after[c] == before[c] by {};
}

pub proof fn conflicting_overlap_rejected(before: Map<u64, SessionView>, after: Map<u64, SessionView>,
    id: TxnId, scope: ScopeView, status: Status, i: int)
    requires register_effect(before,after,id,scope,status), before.contains_key(id.client),
        0 <= i < before[id.client].holds.len(),
        before[id.client].holds[i].overlaps(scope), before[id.client].holds[i].grant != scope.grant,
    ensures status == Status::Invalid, after == before,
{
    assert(!before[id.client].compatible(scope));
    assert(after =~= before);
}

pub proof fn contained_idempotence(before: Map<u64, SessionView>, after: Map<u64, SessionView>,
    id: TxnId, scope: ScopeView, status: Status)
    requires register_effect(before,after,id,scope,status), before.contains_key(id.client),
        before[id.client].sequence == id.sequence, !before[id.client].terminal,
        scope.valid(), before[id.client].compatible(scope), before[id.client].covering(scope),
    ensures status == Status::Ok, after == before,
{
    assert(after =~= before);
}

pub proof fn held_range_expansion(s: SessionView, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>,
    grant: Grant, addresses: Addresses, key: int)
    requires s.wf(), s.has_range(table,lo,hi,grant), addresses.contains_key(key),
        addresses[key].0 == table, inside(lo,hi,addresses[key].1),
    ensures expanded(s,addresses).contains_key(key), expanded(s,addresses)[key] == grant,
{
    let i = choose|i: int| 0 <= i < s.holds.len()
        && s.holds[i].covers_range(table,lo,hi) && s.holds[i].grant == grant;
    covered_point(s.holds[i],range_scope(table,lo,hi,grant),table,addresses[key].1);
    assert(s.has(table,addresses[key].1,grant));
    expanded_exact(s,addresses,key,grant);
}

pub proof fn resolve_accounting(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    status: Status, client: u64, range: KeyRange)
    requires resolve_effect(before,after,id,status), before.contains_key(client),
    ensures after.contains_key(client), after[client].sequence == before[client].sequence,
        after[client].holds == before[client].holds,
        after[client].drained(range) == before[client].drained(range),
        before[client].terminal ==> after[client].terminal,
{}

/// Finish atomically records terminal and clears all registrations. It never
/// deletes/reopens the session, loses the sequence fence, or touches another client.
pub proof fn finish_accounting(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    status: Status, addresses: Addresses)
    requires finish_effect(before,after,id,status),
    ensures frame(before,after,id.client), status != Status::Ok ==> after == before,
        status == Status::Ok ==> after[id.client].terminal && after[id.client].sequence == id.sequence
            && after[id.client].sequence == before[id.client].sequence
            && after[id.client].holds.len() == 0
            && expanded(after[id.client],addresses) == Map::<int,Grant>::empty(),
{
    if status == Status::Ok { assert(expanded(after[id.client],addresses) =~= Map::<int,Grant>::empty()); }
    assert(before.dom() =~= after.dom());
    assert forall|c: u64| before.contains_key(c) && c != id.client implies after[c] == before[c] by {};
}

pub proof fn release_is_terminal(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, status: Status)
    requires release_effect(before,after,id,table,coordinate,status),
    ensures after != before ==> before.contains_key(id.client)
        && before[id.client].sequence == id.sequence && before[id.client].terminal,
        before.contains_key(id.client) && !before[id.client].terminal ==> after == before,
{}

pub open spec fn surviving_hold(s: SessionView, removed_table: u64, removed_coordinate: Seq<u8>,
    table: u64, coordinate: Seq<u8>, grant: Grant) -> bool {
    exists|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(table,coordinate)
        && s.holds[i].grant == grant && !(s.holds[i].table == removed_table
            && s.holds[i].coordinate == removed_coordinate && s.holds[i].end is Point)
}

/// Releasing an explicit point does not release an independently registered scan.
/// The logical effect is exactly the union of surviving registrations.
pub proof fn release_surviving_expansion(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, addresses: Addresses, key: int, grant: Grant)
    requires release_effect(before,after,id,table,coordinate,Status::Ok),
        before[id.client].wf(), after[id.client].wf(), addresses.contains_key(key),
    ensures (expanded(after[id.client],addresses).contains_key(key)
            && expanded(after[id.client],addresses)[key] == grant) <==>
        surviving_hold(before[id.client],table,coordinate,addresses[key].0,addresses[key].1,grant),
{
    let a = after[id.client]; let b = before[id.client];
    let t = addresses[key].0; let c = addresses[key].1;
    expanded_exact(a,addresses,key,grant);
    if a.has(t,c,grant) {
        let i = choose|i: int| 0 <= i < a.holds.len() && a.holds[i].contains(t,c) && a.holds[i].grant == grant;
        assert(a.holds.contains(a.holds[i]));
        assert(b.holds.contains(a.holds[i]));
        let j = choose|j: int| 0 <= j < b.holds.len() && b.holds[j] == a.holds[i];
        assert(surviving_hold(b,table,coordinate,t,c,grant));
    }
    if surviving_hold(b,table,coordinate,t,c,grant) {
        let j = choose|j: int| 0 <= j < b.holds.len() && b.holds[j].contains(t,c)
            && b.holds[j].grant == grant && !(b.holds[j].table == table
                && b.holds[j].coordinate == coordinate && b.holds[j].end is Point);
        assert(b.holds.contains(b.holds[j]));
        assert(a.holds.contains(b.holds[j]));
        let i = choose|i: int| 0 <= i < a.holds.len() && a.holds[i] == b.holds[j];
        assert(a.has(t,c,grant));
    }
}

proof fn point_only_survivor(s: SessionView, table: u64, coordinate: Seq<u8>, t: u64, c: Seq<u8>, g: Grant)
    requires forall|i: int| 0 <= i < s.holds.len() ==> s.holds[i].end is Point,
    ensures surviving_hold(s,table,coordinate,t,c,g) <==>
        s.has(t,c,g) && (t != table || c != coordinate),
{
    if s.has(t,c,g) && (t != table || c != coordinate) {
        let i = choose|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(t,c) && s.holds[i].grant == g;
        assert(surviving_hold(s,table,coordinate,t,c,g));
    }
    if surviving_hold(s,table,coordinate,t,c,g) {
        let i = choose|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(t,c)
            && s.holds[i].grant == g && !(s.holds[i].table == table
                && s.holds[i].coordinate == coordinate && s.holds[i].end is Point);
        assert(s.has(t,c,g));
    }
}

/// The original exact alias-class release theorem is retained for point-only
/// sessions. With range registrations, the stronger surviving-union theorem applies.
pub proof fn release_expansion(before: Map<u64, SessionView>, after: Map<u64, SessionView>, id: TxnId,
    table: u64, coordinate: Seq<u8>, addresses: Addresses)
    requires release_effect(before,after,id,table,coordinate,Status::Ok),
        before[id.client].wf(), after[id.client].wf(),
        forall|i: int| 0 <= i < before[id.client].holds.len() ==> before[id.client].holds[i].end is Point,
    ensures expanded(after[id.client],addresses) == Map::new(
        expanded(before[id.client],addresses).dom().filter(|k: int|
            addresses[k].0 != table || addresses[k].1 != coordinate),
        |k: int| expanded(before[id.client],addresses)[k]),
{
    let a = expanded(after[id.client],addresses); let b = expanded(before[id.client],addresses);
    assert forall|k: int| #[trigger] a.contains_key(k) implies
        b.contains_key(k) && b[k] == a[k] && (addresses[k].0 != table || addresses[k].1 != coordinate) by {
        release_surviving_expansion(before,after,id,table,coordinate,addresses,k,a[k]);
        point_only_survivor(before[id.client],table,coordinate,addresses[k].0,addresses[k].1,a[k]);
        expanded_exact(before[id.client],addresses,k,a[k]);
    }
    assert forall|k: int| #[trigger] b.contains_key(k) && (addresses[k].0 != table || addresses[k].1 != coordinate)
        implies a.contains_key(k) && a[k] == b[k] by {
        release_surviving_expansion(before,after,id,table,coordinate,addresses,k,b[k]);
        point_only_survivor(before[id.client],table,coordinate,addresses[k].0,addresses[k].1,b[k]);
        expanded_exact(before[id.client],addresses,k,b[k]);
    }
    let expected = Map::new(b.dom().filter(|k: int| addresses[k].0 != table || addresses[k].1 != coordinate),|k: int| b[k]);
    assert forall|k: int| a.contains_key(k) <==> expected.contains_key(k) by {
        if a.contains_key(k) { assert(b.contains_key(k) && b[k] == a[k]); }
        if expected.contains_key(k) { assert(a.contains_key(k) && a[k] == b[k]); }
    }
    assert(a =~= expected);
}

/// Unlike finite-universe equivalence, exact native geometry is unconditional:
/// drain is exactly emptiness of intersection over ALL byte coordinates.
pub proof fn drain_exact_geometry(s: SessionView, range: KeyRange)
    ensures s.drained(range) <==> (forall|c: Seq<u8>| s.scoped(range.table,c) ==>
        !bytes::contains_spec(range,range.table,c)),
{
    if s.drained(range) {
        assert forall|c: Seq<u8>| s.scoped(range.table,c) implies
            !bytes::contains_spec(range,range.table,c) by {
            if bytes::contains_spec(range,range.table,c) {
                let i = choose|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(range.table,c);
                common_point_overlaps(s.holds[i],
                    range_scope(range.table,range.lo@,range_hi(range),s.holds[i].grant),range.table,c);
            }
        }
    } else {
        let i = choose|i: int| 0 <= i < s.holds.len()
            && s.holds[i].overlaps_range(range.table,range.lo@,range_hi(range));
        let query = range_scope(range.table,range.lo@,range_hi(range),s.holds[i].grant);
        overlap_has_point(s.holds[i],query);
        let c = choose|c: Seq<u8>| s.holds[i].contains(s.holds[i].table,c) && query.contains(s.holds[i].table,c);
        assert(s.scoped(range.table,c));
        assert(bytes::contains_spec(range,range.table,c));
    }
}

pub open spec fn logical_drained(s: SessionView, addresses: Addresses, range: KeyRange) -> bool {
    forall|k: int| expanded(s,addresses).contains_key(k) ==>
        !bytes::contains_spec(range,addresses[k].0,addresses[k].1)
}

pub proof fn native_drain_no_forgotten_keys(s: SessionView, addresses: Addresses, range: KeyRange)
    requires s.drained(range),
    ensures logical_drained(s,addresses,range),
{
    assert forall|k: int| expanded(s,addresses).contains_key(k) implies
        !bytes::contains_spec(range,addresses[k].0,addresses[k].1) by {
        if bytes::contains_spec(range,addresses[k].0,addresses[k].1) {
            let i = choose|i: int| 0 <= i < s.holds.len() && s.holds[i].contains(addresses[k].0,addresses[k].1);
            let query = range_scope(range.table,range.lo@,range_hi(range),s.holds[i].grant);
            common_point_overlaps(s.holds[i],query,addresses[k].0,addresses[k].1);
        }
    }
}

/// A represented point somewhere in the scope is insufficient: it might lie
/// outside the drained subrange. Every nonempty relevant intersection needs a key.
pub open spec fn intersection_coverage(s: SessionView, addresses: Addresses, range: KeyRange) -> bool {
    forall|i: int| 0 <= i < s.holds.len()
        && s.holds[i].overlaps_range(range.table,range.lo@,range_hi(range)) ==> exists|k: int|
            addresses.contains_key(k) && s.holds[i].contains(addresses[k].0,addresses[k].1)
                && bytes::contains_spec(range,addresses[k].0,addresses[k].1)
}

pub proof fn native_drain_equivalence(s: SessionView, addresses: Addresses, range: KeyRange)
    requires intersection_coverage(s,addresses,range),
    ensures s.drained(range) <==> logical_drained(s,addresses,range),
{
    if s.drained(range) { native_drain_no_forgotten_keys(s,addresses,range); }
    if logical_drained(s,addresses,range) {
        assert forall|i: int| 0 <= i < s.holds.len() implies
            !s.holds[i].overlaps_range(range.table,range.lo@,range_hi(range)) by {
            if s.holds[i].overlaps_range(range.table,range.lo@,range_hi(range)) {
                let k = choose|k: int| addresses.contains_key(k)
                    && s.holds[i].contains(addresses[k].0,addresses[k].1)
                    && bytes::contains_spec(range,addresses[k].0,addresses[k].1);
                assert(s.scoped(addresses[k].0,addresses[k].1));
                assert(expanded(s,addresses).contains_key(k));
            }
        }
    }
}

} // verus!
