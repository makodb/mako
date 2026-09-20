#![deny(unsafe_code)]

//! Pure Rust reference datatypes for exercising the STO adapter contract.
//!
//! These collections favor explicit conflict domains and auditable commit
//! behavior over production tuning. They complement the fine-grained
//! Masstree adapter with small, deterministic test surfaces.

mod hash_map;
mod queue;
mod snapshot;
mod vector;

pub use hash_map::{HashMapCreateError, TxnHashMap};
pub use queue::TxnQueue;
pub use vector::{TxnVec, VecBoundsError};
