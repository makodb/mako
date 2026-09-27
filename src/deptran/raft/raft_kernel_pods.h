#pragma once

// The C types of the Raft kernel boundary: what kernels return BY VALUE, and
// the opaque handle through which the core passes itself to a kernel.
//
// Rust owns the definitions (src/server_pods_h.rs, canonical); each is
// `#[repr(C)]` and bound to the name below with `cpp_native_type`, so the
// transpiled C++ lane uses THESE declarations rather than emitting its own,
// and the host (server.h, server.cc, server_seam_cpp.cc) uses them too. They
// are global-scope C declarations on purpose: a kernel's global declaration
// (raft_cpp_lane_kernels.h) can name only global types.
//
// No imports and no namespaces: this header is included from C++20 module
// global fragments.

#include <stdbool.h>
#include <stdint.h>

// The election-timeout configuration, as one value. Every field is an
// environment override with a compiled-in default, read afresh on each call.
struct RaftElectionTimeouts {
  uint64_t grace_period_us_;
  uint64_t preferred_us_;
  uint64_t non_preferred_grace_us_;
  uint64_t non_preferred_steady_us_;
};

// One campaign's vote tally.
struct RaftVoteOutcome {
  int64_t term_;
  bool yes_;
  bool no_;
  int32_t n_voted_yes_;
  int32_t n_voted_no_;
  bool timeouted_;
};

// One AppendEntries reply, read out of the wire response for heartbeat
// phase 2.
struct AppendRespView {
  bool completed_;
  bool status_;
  uint64_t term_;
  uint64_t last_log_index_;
};

// The core's RaftServerBase as a kernel sees it: an address, never a layout.
// The host casts it back to its own RaftServerBase; the Rust lane's seam does
// the same with the Rust type.
struct raft_server_handle;
