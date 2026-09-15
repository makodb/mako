#pragma once

// Inline-mode counterpart of src/rrr/rust-type-map.toml.
//
// WHY THIS FILE EXISTS
//
// The rusty-cpp transpiler has two modes. Crate mode (which src/rrr uses)
// takes `--type-map`, so a canonical Rust name such as `rusty::ReactorIntEvent`
// is rewritten to its real C++ spelling `IntEvent` on the way out. Inline mode
// -- `rusty-cpp-transpiler inline-rust`, which every src/deptran/raft carrier
// uses -- has no such flag: run `inline-rust --help` and there is no
// `--type-map`. A foreign type therefore reaches C++ under the EXACT path the
// Rust spells.
//
// So the two languages have to agree on one name. Rust's side is fixed: the
// rustc facade crate (src/rrr/rusty-rustc/src/lib.rs:337-338) publishes these
// reactor types as `rusty::ReactorPollThread` and `rusty::ReactorIntEvent`,
// and a DSL block must use those names or rustc cannot resolve them at all
// (gate G1 in docs/migration/raft/cpp-refactor-plan.md). This header supplies
// the other side: the same two names, in C++, aliased to the real types.
//
// The pairs below are the same pairs src/rrr/rust-type-map.toml already
// declares:
//
//     ReactorIntEvent   = "IntEvent"
//     ReactorPollThread = "PollThread"
//     ReactorFiber      = "Fiber"
//
// so the two modes agree on the mapping and only differ in where it is
// written down. If raft ever moves to crate mode (schema_version = 2 in
// src/deptran/raft/rust-modules.toml), this file is deleted and a type map
// replaces it.
//
// SCOPE: aliases only, no new entities. They live in `namespace rusty`
// because that is the namespace the emitted path names; nothing inside
// third-party/rusty-cpp refers to either identifier, so nothing existing can
// bind to them by accident.
//
// ORDERING: include this AFTER the header that imports the rrr reactor module
// (server.h does). Both targets are owned by the C++20 named module
// `rrr.reactor`, so they cannot be forward-declared from here -- an alias is
// all this file may contain.

namespace rusty {

using ReactorPollThread = ::rrr::PollThread;
using ReactorIntEvent = ::rrr::IntEvent;
using ReactorFiber = ::rrr::Fiber;

}  // namespace rusty
