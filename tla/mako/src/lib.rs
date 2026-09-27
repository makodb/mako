//! Mako: a tla-rs style (Verus) model of the speculative 2PC protocol.
//! See README.md for scope, assumptions and the theorem index.
#![allow(non_snake_case)]
#![allow(unused_imports)]
#![allow(dead_code)]

pub mod types;
pub mod normal;
pub mod recovery;
pub mod behavior;
pub mod invariants;
pub mod stream_lemmas;
pub mod proofs_basic;
pub mod proofs_normal1;

