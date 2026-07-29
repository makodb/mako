//! # srpc — the rrr RPC framework as a real Rust library
//!
//! This crate is the Rust source of truth for sRPC, ported layer by
//! layer from the C++ implementation in `src/rrr` (which stays the
//! production code until each layer is replaced). Two consumers:
//!
//! * **Rust programs** use it as a normal crate.
//! * **C++ (mako)** consumes it through rusty-cpp's Rust→C++
//!   transpilation — the same pipeline that produces the
//!   `third-party/rusty-cpp/transpiled/*_port` modules today. No FFI.
//!
//! That second consumer imposes the *transpiler-first* style rules
//! used throughout: zero external dependencies, plain control flow
//! (`while` loops over iterator chains), no proc macros, small and
//! isolated `unsafe` kernels (none in the wire layer), and byte-exact
//! wire compatibility with the C++ implementation — including its
//! quirks, which are documented where they are reproduced.
//!
//! Current scope (milestone 1): the wire layer — SparseInt varints,
//! binary archives, and the serialize/deserialize impls for scalars,
//! strings, and containers, golden-tested against bytes produced by
//! the C++ implementation.

pub mod wire;

pub mod base;
