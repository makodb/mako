//! Stable-source, exclusive-destination mirror kernel for range handoff.
//! The put/delete operations follow shard_data.h:101-160. The scan contract is
//! explicit: the worklist covers the union of source and pre-mirror destination
//! keys. Byte ordering, scan RPC completeness and native storage refinement are
//! separate obligations. A short nonempty scan batch is not end-of-range.
//!
//! Ordering and duplicate keys do not matter once the source is stable. The
//! corrected placement protocol must supply freeze/drain and generation/round
//! fencing; this module never assumes a checksum comparison proves equality.
use vstd::prelude::*;
use vstd::arithmetic::div_mod::lemma_add_mod_noop_right;
use super::sharding_partition::in_range;

verus! {

pub open spec fn lookup<V>(m: Map<int, V>, k: int) -> Option<V> {
    if m.dom().contains(k) { Some(m[k]) } else { None }
}
pub open spec fn copy_key<V>(source: Map<int, V>, destination: Map<int, V>, k: int,
    lo: int, hi: Option<int>) -> Map<int, V>
{
    if in_range(k, lo, hi) {
        if source.dom().contains(k) { destination.insert(k, source[k]) }
        else { destination.remove(k) }
    } else { destination }
}
pub open spec fn mirror<V>(source: Map<int, V>, destination: Map<int, V>, work: Seq<int>,
    lo: int, hi: Option<int>) -> Map<int, V>
    decreases work.len()
{
    if work.len() == 0 { destination }
    else { copy_key(source, mirror(source, destination, work.drop_last(), lo, hi), work.last(), lo, hi) }
}

pub proof fn lemma_mirror_point<V>(source: Map<int, V>, destination: Map<int, V>,
    work: Seq<int>, lo: int, hi: Option<int>, k: int)
    ensures lookup(mirror(source, destination, work, lo, hi), k) ==
        if in_range(k, lo, hi) && work.contains(k) { lookup(source, k) } else { lookup(destination, k) }
    decreases work.len()
{
    if work.len() > 0 {
        let prefix = work.drop_last();
        lemma_mirror_point(source, destination, prefix, lo, hi, k);
        if work.contains(k) && work.last() != k {
            let j = choose|j: int| 0 <= j < work.len() && #[trigger] work[j] == k;
            assert(j < work.len() - 1);
            assert(prefix[j] == k);
            assert(prefix.contains(k));
        }
        if prefix.contains(k) {
            let j = choose|j: int| 0 <= j < prefix.len() && #[trigger] prefix[j] == k;
            assert(work[j] == k);
            assert(work.contains(k));
        }
    }
}

pub proof fn theorem_complete_mirror<V>(source: Map<int, V>, destination: Map<int, V>,
    work: Seq<int>, lo: int, hi: Option<int>)
    requires source.dom().union(destination.dom()).subset_of(work.to_set())
    ensures
        forall|k: int| in_range(k, lo, hi) ==>
            #[trigger] lookup(mirror(source, destination, work, lo, hi), k) == lookup(source, k),
        forall|k: int| !in_range(k, lo, hi) ==>
            #[trigger] lookup(mirror(source, destination, work, lo, hi), k) == lookup(destination, k)
{
    assert forall|k: int| in_range(k, lo, hi) implies
        #[trigger] lookup(mirror(source, destination, work, lo, hi), k) == lookup(source, k) by {
        lemma_mirror_point(source, destination, work, lo, hi, k);
        if !work.contains(k) {
            assert(!work.to_set().contains(k));
            assert(!source.dom().contains(k));
            assert(!destination.dom().contains(k));
        }
    }
    assert forall|k: int| !in_range(k, lo, hi) implies
        #[trigger] lookup(mirror(source, destination, work, lo, hi), k) == lookup(destination, k) by {
        lemma_mirror_point(source, destination, work, lo, hi, k);
    }
}

/// Concrete obsolete-destination-key deletion, overwrite, insertion, outside
/// preservation, and duplicate delivery in one execution of the kernel.
pub proof fn witness_mirror_deletes_extras() -> (result: Map<int, int>)
    ensures lookup(result, 1) == Some(10int), lookup(result, 2) == Some(20int),
        lookup(result, 3) == None::<int>, lookup(result, 9) == Some(90int)
{
    let source = Map::empty().insert(1int, 10int).insert(2int, 20int);
    let destination = Map::empty().insert(1int, 5int).insert(3int, 30int).insert(9int, 90int);
    let work = seq![1int, 3int, 2int, 1int, 9int];
    let result = mirror(source, destination, work, 0, Some(4int));
    lemma_mirror_point(source, destination, work, 0, Some(4int), 1);
    lemma_mirror_point(source, destination, work, 0, Some(4int), 2);
    lemma_mirror_point(source, destination, work, 0, Some(4int), 3);
    lemma_mirror_point(source, destination, work, 0, Some(4int), 9);
    result
}

pub open spec fn modulus() -> int { 18446744073709551616int }
pub open spec fn contribution(row: (int, int), hash_key: spec_fn(int) -> int,
    hash_value: spec_fn(int) -> int) -> int
{ 1000003 * hash_key(row.0) + hash_value(row.1) }
/// Unsigned-u64 fold, including wrap after each row contribution.
pub open spec fn checksum(rows: Seq<(int, int)>, hash_key: spec_fn(int) -> int,
    hash_value: spec_fn(int) -> int) -> int
    decreases rows.len()
{
    if rows.len() == 0 { 0 }
    else { (checksum(rows.drop_last(), hash_key, hash_value)
        + contribution(rows.last(), hash_key, hash_value)) % modulus() }
}
pub open spec fn raw_sum(rows: Seq<(int, int)>, hash_key: spec_fn(int) -> int,
    hash_value: spec_fn(int) -> int) -> int
    decreases rows.len()
{
    if rows.len() == 0 { 0 }
    else { raw_sum(rows.drop_last(), hash_key, hash_value) + contribution(rows.last(), hash_key, hash_value) }
}
pub proof fn lemma_checksum_is_modular_sum(rows: Seq<(int, int)>, hash_key: spec_fn(int) -> int,
    hash_value: spec_fn(int) -> int)
    ensures checksum(rows, hash_key, hash_value) == raw_sum(rows, hash_key, hash_value) % modulus()
    decreases rows.len()
{
    if rows.len() > 0 {
        lemma_checksum_is_modular_sum(rows.drop_last(), hash_key, hash_value);
        lemma_add_mod_noop_right(contribution(rows.last(), hash_key, hash_value),
            raw_sum(rows.drop_last(), hash_key, hash_value), modulus());
    }
}

/// Applies to the actual FNV fold for ANY FNV outputs: exchanging values between
/// distinct keys preserves its sum. No hash collision or cryptographic premise
/// is needed. Therefore verify_range's byte-equality comment is not justified.
pub proof fn theorem_swapped_values_collide(a: int, b: int, x: int, y: int,
    hash_key: spec_fn(int) -> int, hash_value: spec_fn(int) -> int)
    requires a != b, x != y
    ensures
        checksum(seq![(a, x), (b, y)], hash_key, hash_value)
            == checksum(seq![(a, y), (b, x)], hash_key, hash_value),
        Map::<int, int>::empty().insert(a, x).insert(b, y)
            != Map::<int, int>::empty().insert(a, y).insert(b, x)
{
    let left = seq![(a, x), (b, y)];
    let right = seq![(a, y), (b, x)];
    lemma_checksum_is_modular_sum(left, hash_key, hash_value);
    lemma_checksum_is_modular_sum(right, hash_key, hash_value);
    reveal_with_fuel(raw_sum, 3);
    assert(raw_sum(left, hash_key, hash_value) == raw_sum(right, hash_key, hash_value));
    assert(Map::<int, int>::empty().insert(a, x).insert(b, y)[a] == x);
    assert(Map::<int, int>::empty().insert(a, y).insert(b, x)[a] == y);
}

} // verus!
