//! The argument and cursor rule of `scan_next`.
//!
//! `scan_next` delivers the first live committed row whose key lies in
//! `[lo, hi)` (no upper bound when `has_hi` is 0) and whose identity
//! `(coordinate, key)` is strictly greater than the cursor's when `has_after`
//! is 1. The coordinate of a raw table's row is its key.
use super::order::*;
use vstd::prelude::*;

verus! {

/// Why `scan_next` refuses its arguments. Every refusal is INVALID.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ScanRefusal {
    /// `has_hi` or `has_after` is neither 0 nor 1.
    Flags,
    /// The upper bound is not above the lower bound.
    EmptyRange,
    /// The cursor's coordinate is not its key.
    CursorIdentity,
}

/// Where the engine's scan begins.
#[derive(Clone, Copy)]
pub enum ScanLower<'a> {
    Included(&'a [u8]),
    Excluded(&'a [u8]),
}

/// What `scan_next` does with arguments it accepts.
#[derive(Clone, Copy)]
pub enum ScanStart<'a> {
    /// The cursor is at or beyond the upper bound: no key is left. The verb
    /// answers NOT_FOUND once it knows the table and the idle handle.
    Exhausted,
    /// Ask the engine for the first live row between these bounds.
    Rows { lower: ScanLower<'a>, upper: Option<&'a [u8]> },
}

pub open spec fn flags_valid(has_hi: u32, has_after: u32) -> bool {
    has_hi <= 1 && has_after <= 1
}

pub open spec fn range_empty(has_hi: u32, lo: Seq<u8>, hi: Seq<u8>) -> bool {
    has_hi == 1 && cmp_spec(lo, hi) >= 0
}

pub open spec fn cursor_malformed(has_after: u32, after_coordinate: Seq<u8>, after_key: Seq<u8>) -> bool {
    has_after == 1 && after_coordinate != after_key
}

pub open spec fn arguments_valid(
    has_hi: u32,
    has_after: u32,
    lo: Seq<u8>,
    hi: Seq<u8>,
    after_coordinate: Seq<u8>,
    after_key: Seq<u8>,
) -> bool {
    flags_valid(has_hi, has_after) && !range_empty(has_hi, lo, hi) && !cursor_malformed(
        has_after,
        after_coordinate,
        after_key,
    )
}

/// The identity `(coordinate, key)` is strictly greater than the cursor's:
/// coordinate first, then key, as the sharding crate's `compare_identity`.
pub open spec fn identity_after(
    coordinate: Seq<u8>,
    key: Seq<u8>,
    after_coordinate: Seq<u8>,
    after_key: Seq<u8>,
) -> bool {
    cmp_spec(after_coordinate, coordinate) < 0 || (after_coordinate == coordinate && cmp_spec(
        after_key,
        key,
    ) < 0)
}

/// A key whose row the sharding crate accepts as the answer of this call: in
/// the range, and after the cursor.
pub open spec fn admissible(
    key: Seq<u8>,
    has_hi: u32,
    has_after: u32,
    lo: Seq<u8>,
    hi: Seq<u8>,
    after_coordinate: Seq<u8>,
    after_key: Seq<u8>,
) -> bool {
    &&& cmp_spec(lo, key) <= 0
    &&& has_hi == 1 ==> cmp_spec(key, hi) < 0
    &&& has_after == 1 ==> identity_after(key, key, after_coordinate, after_key)
}

/// A key between the bounds handed to the engine.
pub open spec fn within(key: Seq<u8>, lower: ScanLower, upper: Option<&[u8]>) -> bool {
    &&& match lower {
        ScanLower::Included(bound) => cmp_spec(bound@, key) <= 0,
        ScanLower::Excluded(bound) => cmp_spec(bound@, key) < 0,
    }
    &&& match upper {
        Some(bound) => cmp_spec(key, bound@) < 0,
        None => true,
    }
}

/// What the rule promises.
pub open spec fn scan_contract(
    has_hi: u32,
    has_after: u32,
    lo: Seq<u8>,
    hi: Seq<u8>,
    after_coordinate: Seq<u8>,
    after_key: Seq<u8>,
    decision: Result<ScanStart, ScanRefusal>,
) -> bool {
    let valid = arguments_valid(has_hi, has_after, lo, hi, after_coordinate, after_key);
    let beyond = has_hi == 1 && has_after == 1 && cmp_spec(after_key, hi) >= 0;
    match decision {
        // Exactly the malformed arguments are refused, each for a reason
        // that is true.
        Err(refusal) => !valid && match refusal {
            ScanRefusal::Flags => !flags_valid(has_hi, has_after),
            ScanRefusal::EmptyRange => range_empty(has_hi, lo, hi),
            ScanRefusal::CursorIdentity => cursor_malformed(has_after, after_coordinate, after_key),
        },
        // The end of the range is declared exactly for a cursor at or beyond
        // the upper bound, and then no key is admissible.
        Ok(ScanStart::Exhausted) => valid && beyond && forall|key: Seq<u8>|
            !admissible(key, has_hi, has_after, lo, hi, after_coordinate, after_key),
        // Otherwise the engine is asked for exactly the admissible keys.
        Ok(ScanStart::Rows { lower, upper }) => valid && !beyond && forall|key: Seq<u8>|
            within(key, lower, upper) == admissible(
                key,
                has_hi,
                has_after,
                lo,
                hi,
                after_coordinate,
                after_key,
            ),
    }
}

/// Select physical keys at one fixed warehouse coordinate. Stop ends this
/// warehouse and every later coordinate in an ascending scan.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FixedScan {
    Skip,
    Stop,
    AllKeys,
    AfterKey,
}

pub open spec fn fixed_row_allowed(
    coordinate: Seq<u8>, key: Seq<u8>, lo: Seq<u8>, hi: Seq<u8>, bounded: bool,
    after_coordinate: Seq<u8>, after_key: Seq<u8>, has_after: bool,
) -> bool {
    &&& cmp_spec(lo, coordinate) <= 0
    &&& bounded ==> cmp_spec(coordinate, hi) < 0
    &&& has_after ==> identity_after(coordinate, key, after_coordinate, after_key)
}

/// All physical keys, the strict suffix after the cursor key, or no keys.
/// Bounds and cursors may have arbitrary lengths.
pub fn fixed_scan(coordinate: &[u8], lo: &[u8], hi: &[u8], bounded: bool,
                  after_coordinate: &[u8], has_after: bool) -> (decision: FixedScan)
    ensures
        (decision == FixedScan::Stop) == (bounded && cmp_spec(coordinate@, hi@) >= 0),
        forall|key: Seq<u8>, after_key: Seq<u8>|
            fixed_row_allowed(coordinate@, key, lo@, hi@, bounded,
                                         after_coordinate@, after_key, has_after)
                == match decision {
                    FixedScan::Skip | FixedScan::Stop => false,
                    FixedScan::AllKeys => true,
                    FixedScan::AfterKey => cmp_spec(after_key, key) < 0,
                },
{
    proof {
        cmp_laws(after_coordinate@, coordinate@);
        cmp_laws(coordinate@, lo@);
    }
    if bounded && compare(coordinate, hi) >= 0 {
        FixedScan::Stop
    } else if compare(coordinate, lo) < 0 {
        FixedScan::Skip
    } else if !has_after {
        FixedScan::AllKeys
    } else {
        let cursor_order = compare(after_coordinate, coordinate);
        if cursor_order > 0 {
            FixedScan::Skip
        } else if cursor_order == 0 {
            FixedScan::AfterKey
        } else {
            FixedScan::AllKeys
        }
    }
}

/// Whether both flags are 0 or 1. The shell asks before it looks at a span.
pub fn scan_flags_valid(has_hi: u32, has_after: u32) -> (valid: bool)
    ensures
        valid == flags_valid(has_hi, has_after),
{
    has_hi <= 1 && has_after <= 1
}

/// Decides what `scan_next` does with its arguments: refuse them, declare the
/// range exhausted, or ask the engine between two bounds.
pub fn scan_arguments<'a>(
    has_hi: u32,
    has_after: u32,
    lo: &'a [u8],
    hi: &'a [u8],
    after_coordinate: &[u8],
    after_key: &'a [u8],
) -> (decision: Result<ScanStart<'a>, ScanRefusal>)
    ensures
        scan_contract(has_hi, has_after, lo@, hi@, after_coordinate@, after_key@, decision),
{
    if !scan_flags_valid(has_hi, has_after) {
        return Err(ScanRefusal::Flags);
    }
    if has_hi == 1 && compare(lo, hi) >= 0 {
        return Err(ScanRefusal::EmptyRange);
    }
    // The coordinate of a raw table's row is its key.
    if has_after == 1 && compare(after_coordinate, after_key) != 0 {
        proof {
            cmp_laws(after_coordinate@, after_key@);
        }
        return Err(ScanRefusal::CursorIdentity);
    }
    // The cursor is exclusive on the key bytes themselves. A cursor below the
    // range starts at its lower bound, which is inclusive.
    let exclusive = has_after == 1 && compare(after_key, lo) >= 0;
    let start = if exclusive {
        after_key
    } else {
        lo
    };
    proof {
        cmp_laws(after_coordinate@, after_key@);
        cmp_laws(after_key@, lo@);
    }
    if has_hi == 1 && compare(start, hi) >= 0 {
        proof {
            // The range is not empty, so only a cursor can be this far.
            assert(exclusive);
            assert forall|key: Seq<u8>|
                !admissible(
                    key,
                    has_hi,
                    has_after,
                    lo@,
                    hi@,
                    after_coordinate@,
                    after_key@,
                ) by {
                if cmp_spec(after_key@, key) < 0 && cmp_spec(key, hi@) < 0 {
                    cmp_trans(after_key@, key, hi@);
                }
            }
        }
        return Ok(ScanStart::Exhausted);
    }
    let lower = if exclusive {
        ScanLower::Excluded(after_key)
    } else {
        ScanLower::Included(lo)
    };
    let upper = if has_hi == 1 {
        Some(hi)
    } else {
        None
    };
    proof {
        assert forall|key: Seq<u8>|
            within(key, lower, upper) == admissible(
                key,
                has_hi,
                has_after,
                lo@,
                hi@,
                after_coordinate@,
                after_key@,
            ) by {
            if exclusive {
                // After a cursor inside the range is at or after the lower
                // bound.
                if cmp_spec(after_key@, key) < 0 {
                    cmp_trans(lo@, after_key@, key);
                }
            } else if has_after == 1 {
                // Everything from the lower bound on is after a cursor below
                // the range.
                if cmp_spec(lo@, key) <= 0 {
                    cmp_trans(after_key@, lo@, key);
                }
            }
        }
        if !exclusive && has_after == 1 && has_hi == 1 {
            cmp_trans(after_key@, lo@, hi@);
        }
    }
    Ok(ScanStart::Rows { lower, upper })
}

} // verus!
