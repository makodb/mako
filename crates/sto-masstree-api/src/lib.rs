//! Safe Rust application ownership for native Masstree rows and distributed STO.
//! The only lifetime erasure is the private affine worker owner.
#![deny(unsafe_code)]
pub mod native;
#[allow(unsafe_code)]
mod owner;
