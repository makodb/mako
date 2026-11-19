//! Verus proofs about the REAL `rpc/errors.rs` classification functions.
//!
//! Lives in the verify/ harness, not in the srpc crate, so it is never compiled
//! into production and never transpiled to C++ -- which is what lets it use
//! in-body `proof!`. It reasons from the definitional `#[cfg(verus)]` contracts
//! those functions carry (the numeric ranges) to two consistency theorems. See
//! docs/verification.md.
use vstd::prelude::*;

use crate::errors::{
    get_error_category, is_connection_error, is_timeout_error, RpcError, RpcErrorCategory,
};

// `RpcError` / `RpcErrorCategory` are plain `#[repr(i32)]` enums declared
// outside `verus!`, so Verus needs an external type specification to admit them
// into spec reasoning.
#[verifier::external_type_specification]
#[allow(dead_code)]
pub struct ExRpcError(RpcError);

#[verifier::external_type_specification]
#[allow(dead_code)]
pub struct ExRpcErrorCategory(RpcErrorCategory);

// Theorem 1: the connection and timeout error classes are disjoint -- no error
// code is classified as both. The predicates recompute their ranges [100,200)
// and [400,500) independently, so this is a real (not tautological) check.
#[verus_spec]
#[allow(dead_code)]
pub fn prove_connection_and_timeout_are_disjoint(err: RpcError) {
    let c = is_connection_error(err);
    let t = is_timeout_error(err);
    proof! {
        assert(!(c && t));
    }
}

// Theorem 2: the boolean predicates agree with the six-way categorizer. This is
// the load-bearing one: is_connection_error / is_timeout_error hard-code their
// ranges separately from get_error_category, so an edit to one range that
// forgets the other would break this proof. errors_rust.rs only samples this by
// example; here it holds for every RpcError.
#[verus_spec]
#[allow(dead_code)]
pub fn prove_predicates_match_the_categorizer(err: RpcError) {
    let ic = is_connection_error(err);
    let it = is_timeout_error(err);
    let cat = get_error_category(err);
    proof! {
        assert(ic == (cat == RpcErrorCategory::CONNECTION));
        assert(it == (cat == RpcErrorCategory::TIMEOUT));
    }
}
