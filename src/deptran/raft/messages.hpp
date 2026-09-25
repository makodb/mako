#pragma once

/**
 * @file messages.hpp
 * @brief Plain C++ Raft RPC payload structs.
 *
 * These structs are the abstract representation of every Raft RPC carried
 * over the wire today. They do not contain any srpc-specific machinery
 * (Future, DeferredReply, Proxy) so they can be used by either the
 * production srpc transport or an in-memory channel transport for tests.
 *
 * The field layout matches the current `RaftProxy::Rpc*` structs in
 * src/deptran/rcc_rpc.h, so the srpc adapter is a trivial memberwise copy.
 *
 * Rusty-safety:
 *  - No virtual functions; no inheritance.
 *  - No std smart pointers in new fields. The one exception is the
 *    existing `srpc::MarshallDeputy` command payload, which is still the
 *    on-the-wire format for AppendEntries. When the LogEntry / command
 *    representation itself is moved off srpc (later plan phase), these
 *    structs will follow.
 */

#include <cstdint>
#include <string>

#include <rusty/option.hpp>
#include <rusty/vec.hpp>

#include "srpc/srpc.hpp"

#include "../constants.h"
#include "../mako_commands.h"  // janus::Command

namespace janus {
namespace raft {

// MarshallDeputy retired;
// production wire path uses janus::Command directly.

// ---------------------------------------------------------------------------
// RequestVote
// ---------------------------------------------------------------------------
// Rust DSL owns scalar-only wire values. `cpp_no_auto_traits` avoids
// introducing C++-only marker members.
//
// These fields carry NO per-member `{}` initializer. Every C++ site that makes
// one of these uses the brace form -- `return VoteReply{}`,
// `send_vote(2, VoteReq{})` -- which value-initializes every member of an
// aggregate whether or not the members have their own initializers, so the
// zeroing is already guaranteed at each site. A BARE declaration
// (`VoteReply reply;`) would leave them indeterminate, so
// scripts/raft_field_census.py refuses one; the error paths above are exactly
// where a garbage reply would be read as a real vote or append result.
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReq {
    pub last_log_idx: u64,
    pub last_log_term: i64,
    pub candidate_site_id: u16,
    pub current_term: i64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReply {
    pub max_ballot: i64,
    pub vote_granted: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.vote version=1 rust_sha256=1d5e1587b33379a356bc4cab45155ef031e06ba8d443180b4a4777aa5a8fc9e3*/
struct VoteReq;
struct VoteReply;

struct VoteReq {
    uint64_t last_log_idx;
    int64_t last_log_term;
    uint16_t candidate_site_id;
    int64_t current_term;
};

struct VoteReply {
    int64_t max_ballot;
    bool vote_granted;
};
/*RUSTYCPP:GEN-END id=raft_messages.vote*/

// ---------------------------------------------------------------------------
// AppendEntries (with command payload)
// ---------------------------------------------------------------------------
struct AppendEntriesReq {
  uint64_t       slot{0};
  ballot_t       ballot{0};
  uint64_t       leader_current_term{0};
  siteid_t       leader_site_id{0};
  uint64_t       leader_prev_log_index{0};
  uint64_t       leader_prev_log_term{0};
  uint64_t       leader_commit_index{0};
  // 2 step 5 (2026-05-05): `cmd` migrated from `MarshallDeputy`
  // to `janus::Command` (= `SerializableEnvelope<MakoCommands>`)
  // alongside the Marshallable/MarshallDeputy retirement.  Wire format
  // is identical (`[v32 kind][payload]` for both, post-L9 alignment).
  ::janus::Command cmd{};
  uint64_t       leader_next_log_term{0};
};

#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesReply {
    pub follower_append_ok: u64,
    pub follower_current_term: u64,
    pub follower_last_log_index: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.append_entries_reply version=1 rust_sha256=ffcbffce24b181685836aeccd2ebd421703ae5225e714f68c8f692509495b14c*/
struct AppendEntriesReply;

struct AppendEntriesReply {
    uint64_t follower_append_ok;
    uint64_t follower_current_term;
    uint64_t follower_last_log_index;
};
/*RUSTYCPP:GEN-END id=raft_messages.append_entries_reply*/

// ---------------------------------------------------------------------------
// EmptyAppendEntries (heartbeat)
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReq {
    pub slot: u64,
    pub ballot: i64,
    pub leader_current_term: u64,
    pub leader_site_id: u16,
    pub leader_prev_log_index: u64,
    pub leader_prev_log_term: u64,
    pub leader_commit_index: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReply {
    pub follower_append_ok: u64,
    pub follower_current_term: u64,
    pub follower_last_log_index: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.heartbeat version=1 rust_sha256=9486ebc57768ca09270b74276279c8ae8a16612c603da5eeb4055966cac005b1*/
struct EmptyAppendEntriesReq;
struct EmptyAppendEntriesReply;

struct EmptyAppendEntriesReq {
    uint64_t slot;
    int64_t ballot;
    uint64_t leader_current_term;
    uint16_t leader_site_id;
    uint64_t leader_prev_log_index;
    uint64_t leader_prev_log_term;
    uint64_t leader_commit_index;
};

struct EmptyAppendEntriesReply {
    uint64_t follower_append_ok;
    uint64_t follower_current_term;
    uint64_t follower_last_log_index;
};
/*RUSTYCPP:GEN-END id=raft_messages.heartbeat*/

// ---------------------------------------------------------------------------
// InstallSnapshot
// ---------------------------------------------------------------------------
struct InstallSnapshotReq {
  uint64_t    term{0};
  uint64_t    leader_id{0};
  uint64_t    last_included_index{0};
  uint64_t    last_included_term{0};
  std::string data;  // raw snapshot bytes; LZ4-compressed in RocksDB impl
};

#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct InstallSnapshotReply {
    pub term_out: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.install_snapshot_reply version=1 rust_sha256=715ebd5405f75969c371f1dc5e546ae46035358190eeb8a7a2a785b60a287854*/
struct InstallSnapshotReply;

struct InstallSnapshotReply {
    uint64_t term_out;
};
/*RUSTYCPP:GEN-END id=raft_messages.install_snapshot_reply*/

}  // namespace raft
}  // namespace janus
