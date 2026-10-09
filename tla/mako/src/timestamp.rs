//! MakoV2's single, totally ordered HLC timestamp. These are mathematical
//! fields of the 16-byte (physical_us, logical, origin) representation, not
//! vector-clock components. Wall time may regress; origin leases are modeled
//! by the protocol, independently of Raft terms and speculative epochs.
use vstd::prelude::*;

verus! {

pub struct Timestamp {
    pub physical: nat,
    pub logical: nat,
    pub origin: nat,
}

pub open spec fn bottom() -> Timestamp {
    Timestamp { physical: 0, logical: 0, origin: 0 }
}

pub open spec fn valid(t: Timestamp) -> bool {
    t.physical <= 18446744073709551615nat
        && t.logical <= 4294967295nat
        && 0 < t.origin <= 4294967295nat
}

pub open spec fn lt(a: Timestamp, b: Timestamp) -> bool {
    a.physical < b.physical
        || (a.physical == b.physical && a.logical < b.logical)
        || (a.physical == b.physical && a.logical == b.logical && a.origin < b.origin)
}

pub open spec fn le(a: Timestamp, b: Timestamp) -> bool { a == b || lt(a, b) }

pub open spec fn max(a: Timestamp, b: Timestamp) -> Timestamp {
    if le(a, b) { b } else { a }
}

pub open spec fn min(a: Timestamp, b: Timestamp) -> Timestamp {
    if le(a, b) { a } else { b }
}

/// The unsigned big-endian 16-byte representation interpreted as one scalar.
pub open spec fn rank(t: Timestamp) -> int {
    (t.physical as int) * 18446744073709551616int
        + (t.logical as int) * 4294967296int + (t.origin as int)
}

pub proof fn scalar_order(a: Timestamp, b: Timestamp)
    requires valid(a) || a == bottom(), valid(b) || b == bottom()
    ensures
        lt(a, b) <==> rank(a) < rank(b),
        a == b <==> rank(a) == rank(b),
        valid(a) ==> rank(a) > 0,
{}

pub proof fn order(a: Timestamp, b: Timestamp, c: Timestamp)
    ensures
        le(a, a),
        le(a, b) || le(b, a),
        (le(a, b) && le(b, a)) ==> a == b,
        (le(a, b) && le(b, c)) ==> le(a, c),
        (lt(a, b) && le(b, c)) ==> lt(a, c),
        (le(a, b) && lt(b, c)) ==> lt(a, c),
        le(a, max(a, b)), le(b, max(a, b)),
        le(min(a, b), a), le(min(a, b), b),
        valid(a) ==> lt(bottom(), a),
{}

/// The full-format allocation contract. The packed local millisecond clock
/// may choose a later representable timestamp; its narrower 44/19-bit layout
/// is not assumed here. None means exhaustion/invalid allocator input, never
/// wrapping. The caller supplies max(last local timestamp, observed bounds).
pub open spec fn allocate(bound: Timestamp, wall: nat, origin: nat) -> Option<Timestamp> {
    if origin == 0 || origin > 4294967295nat || wall > 18446744073709551615nat {
        None
    } else if wall > bound.physical {
        Some(Timestamp { physical: wall, logical: 0, origin })
    } else if bound.physical > 18446744073709551615nat || bound.logical > 4294967295nat {
        None
    } else if origin > bound.origin {
        Some(Timestamp { physical: bound.physical, logical: bound.logical, origin })
    } else if bound.logical < 4294967295nat {
        Some(Timestamp { physical: bound.physical, logical: bound.logical + 1, origin })
    } else if bound.physical < 18446744073709551615nat {
        Some(Timestamp { physical: bound.physical + 1, logical: 0, origin })
    } else {
        None
    }
}

pub proof fn allocation_contract(bound: Timestamp, wall: nat, origin: nat)
    ensures
        allocate(bound, wall, origin) is Some ==> {
            let t = allocate(bound, wall, origin).unwrap();
            valid(t) && lt(bound, t) && t.physical >= wall && t.origin == origin
        },
{}

pub proof fn allocation_follows_observations(local: Timestamp, observed: Timestamp, wall: nat, origin: nat)
    ensures
        allocate(max(local, observed), wall, origin) is Some ==> {
            let t = allocate(max(local, observed), wall, origin).unwrap();
            lt(local, t) && lt(observed, t) && valid(t)
        },
{
    allocation_contract(max(local, observed), wall, origin);
    order(local, observed, max(local, observed));
    if allocate(max(local, observed), wall, origin) is Some {
        let t = allocate(max(local, observed), wall, origin).unwrap();
        order(local, max(local, observed), t);
        order(observed, max(local, observed), t);
    }
}

/// Different leased origins cannot issue an identical full timestamp.
pub proof fn origin_separation(a: Timestamp, b: Timestamp)
    requires a.origin != b.origin
    ensures a != b
{}

} // verus!
