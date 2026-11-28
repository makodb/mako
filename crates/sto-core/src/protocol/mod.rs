//! Distributed STO over explicit storage, transport and durability contracts.
//! Lower implementations belong to mako-runtime and their own component gates.
#![forbid(unsafe_code)]
pub mod admission;
pub mod commit;
pub mod contracts;
pub mod epoch;
pub mod journal;
pub mod participant;
pub mod range_lock;
pub mod redo;
pub mod remote;
pub mod wire;

// Verified entry points take explicit facts; keep their proof signatures intact.
#[allow(clippy::too_many_arguments)]
pub mod verified;
