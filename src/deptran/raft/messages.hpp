#pragma once

/**
 * @file messages.hpp
 * @brief Plain C++ Raft RPC payload structs.
 *
 * These structs are the abstract representation of every Raft RPC carried
 * over the wire today. They do not contain any rrr-specific machinery
 * (Future, DeferredReply, Proxy) so they can be used by either the
 * production rrr transport or an in-memory channel transport for tests.
 *
 * The field layout matches the current `RaftProxy::Rpc*` structs in
 * src/deptran/rcc_rpc.h, so the rrr adapter is a trivial memberwise copy.
 *
 * Rusty-safety:
 *  - No virtual functions; no inheritance.
 *  - No std smart pointers in new fields. The one exception is the
 *    existing `rrr::MarshallDeputy` command payload, which is still the
 *    on-the-wire format for AppendEntries. When the LogEntry / command
 *    representation itself is moved off rrr (later plan phase), these
 *    structs will follow.
 */

#include <cstdint>
#include <string>

#include <rusty/option.hpp>
#include <rusty/vec.hpp>

#include "rrr/rrr.hpp"

#include "../constants.h"
#include "../mako_commands.h"  // janus::Command

namespace janus {
namespace raft {

// MarshallDeputy retired;
// production wire path uses janus::Command directly.

// ---------------------------------------------------------------------------
// RequestVote
// ---------------------------------------------------------------------------
// Rust DSL is the source of truth for this leaf POD family. The adjacent
// generated block is the C++ production compiler sees. Keep construction
// value-initialized (`T{}`) to preserve positional aggregate call sites.
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReq {
    pub last_log_idx: u64,
    pub last_log_term: i64,
    pub candidate_site_id: u16,
    pub current_term: i64,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReply {
    pub max_ballot: i64,
    pub vote_granted: bool,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteDurableReq {
    pub term: i64,
    pub voter_id: u16,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteDurableReply {
    pub acknowledged: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.vote version=1 rust_sha256=2eac51939a3d2a05a9f87ac846a9cb1231d47def474957bc2891864796805e32*/
struct VoteReq;
struct VoteReply;
struct VoteDurableReq;
struct VoteDurableReply;

struct VoteReq {
    uint64_t last_log_idx;
    int64_t last_log_term;
    uint16_t candidate_site_id;
    int64_t current_term;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct VoteReply {
    int64_t max_ballot;
    bool vote_granted;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct VoteDurableReq {
    int64_t term;
    uint16_t voter_id;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct VoteDurableReply {
    bool acknowledged;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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

// Rust DSL is the source of truth for this scalar response. The command-
// bearing request remains C++-owned until Command has a real Rust facade.
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesReply {
    pub follower_append_ok: u64,
    pub follower_current_term: u64,
    pub follower_last_log_index: u64,
    pub follower_ack_type: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.append_entries_reply version=1 rust_sha256=1fdefb29c0e7ab50b7f2cf4394b0250d5e5ef9a3a8bd213f33e66214bf35c6f2*/
struct AppendEntriesReply;

struct AppendEntriesReply {
    uint64_t follower_append_ok;
    uint64_t follower_current_term;
    uint64_t follower_last_log_index;
    uint64_t follower_ack_type;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.append_entries_reply*/

// ---------------------------------------------------------------------------
// EmptyAppendEntries (heartbeat / election trigger)
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
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
    pub trigger_election_now: bool,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReply {
    pub follower_append_ok: u64,
    pub follower_current_term: u64,
    pub follower_last_log_index: u64,
    pub follower_ack_type: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.heartbeat version=1 rust_sha256=645839bf14cb8cf07ff4150a6726df745e5f1f97b1b802876f4034d4a5fee591*/
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
    bool trigger_election_now;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct EmptyAppendEntriesReply {
    uint64_t follower_append_ok;
    uint64_t follower_current_term;
    uint64_t follower_last_log_index;
    uint64_t follower_ack_type;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.heartbeat*/

// ---------------------------------------------------------------------------
// AppendEntriesDurable — follower acks that its log has been fsync'd.
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesDurableReq {
    pub term: i64,
    pub follower_id: u16,
    pub last_log_index: u64,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesDurableReply {
    pub acknowledged: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.durable version=1 rust_sha256=54706669da00a9551cc2e30f930ea4ff7a94adb9e35c9eb94667a7d2d1f325dd*/
struct AppendEntriesDurableReq;
struct AppendEntriesDurableReply;

struct AppendEntriesDurableReq {
    int64_t term;
    uint16_t follower_id;
    uint64_t last_log_index;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct AppendEntriesDurableReply {
    bool acknowledged;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.durable*/

// ---------------------------------------------------------------------------
// TimeoutNow — leader asks a follower to immediately start election
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct TimeoutNowReq {
    pub leader_term: u64,
    pub leader_site_id: u16,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct TimeoutNowReply {
    pub follower_term: u64,
    pub success: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.timeout_now version=1 rust_sha256=662327b815f3f56c97cfcff20258f8f4ba6b6e0dd7de9ea527a9d43147d6fe93*/
struct TimeoutNowReq;
struct TimeoutNowReply;

struct TimeoutNowReq {
    uint64_t leader_term;
    uint16_t leader_site_id;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct TimeoutNowReply {
    uint64_t follower_term;
    bool success;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.timeout_now*/

// ---------------------------------------------------------------------------
// NotifyRestart — after crash recovery, tell peers to reconnect.
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct NotifyRestartReq {
    pub restarted_site_id: u16,
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct NotifyRestartReply {
    pub acknowledged: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.notify_restart version=1 rust_sha256=2e360e2c5f77d087938c19dd197bb7bc235176b72484089df695cbd8e25e50f9*/
struct NotifyRestartReq;
struct NotifyRestartReply;

struct NotifyRestartReq {
    uint16_t restarted_site_id;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct NotifyRestartReply {
    bool acknowledged;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.notify_restart*/

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
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct InstallSnapshotReply {
    pub term_out: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.install_snapshot_reply version=1 rust_sha256=fe1d3751271e9a49b91ef4c07c086e458399e02380ed4394b8d12202bf59248c*/
struct InstallSnapshotReply;

struct InstallSnapshotReply {
    uint64_t term_out;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.install_snapshot_reply*/

// ---------------------------------------------------------------------------
// AddServer / RemoveServer (membership change)
// ---------------------------------------------------------------------------
struct AddServerReq {
  uint64_t    term{0};
  uint64_t    new_server_id{0};
  std::string new_server_addr;
};

struct AddServerReply {
  bool        success{false};
  std::string error_msg;
  uint64_t    leader_hint{0};
};

#if RUSTYCPP_RUST
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct RemoveServerReq {
    pub term: u64,
    pub server_id: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.remove_server_req version=1 rust_sha256=65a46ed6936859c4a9fb711970f94b68b572fd74a8aa42d3e67bb2e05371611a*/
struct RemoveServerReq;

struct RemoveServerReq {
    uint64_t term;
    uint64_t server_id;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=raft_messages.remove_server_req*/

struct RemoveServerReply {
  bool        success{false};
  std::string error_msg;
  uint64_t    leader_hint{0};
};

}  // namespace raft
}  // namespace janus
