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
// Rust DSL owns scalar-only wire values. `cpp_value_init` preserves each
// incumbent C++ default member initializer without adding constructors, and
// `cpp_no_auto_traits` avoids introducing C++-only marker members.
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub last_log_idx: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub last_log_term: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub candidate_site_id: u16,
    #[cfg_attr(any(), cpp_value_init)]
    pub current_term: i64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub max_ballot: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub vote_granted: bool,
}

// VoteDurable — sent by a voter once its vote has been persisted.
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteDurableReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub term: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub voter_id: u16,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteDurableReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub acknowledged: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.vote version=1 rust_sha256=06daa0599a6464f54eb30a5d892a2365b069e7a09f67900274002af3555f82fe*/
struct VoteReq;
struct VoteReply;
struct VoteDurableReq;
struct VoteDurableReply;

struct VoteReq {
    uint64_t last_log_idx{};
    int64_t last_log_term{};
    uint16_t candidate_site_id{};
    int64_t current_term{};
};

struct VoteReply {
    int64_t max_ballot{};
    bool vote_granted{};
};

struct VoteDurableReq {
    int64_t term{};
    uint16_t voter_id{};
};

struct VoteDurableReply {
    bool acknowledged{};
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
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_append_ok: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_current_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_last_log_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_ack_type: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.append_entries_reply version=1 rust_sha256=e5500a47a858b0edbf96bdb631736d0df240ed4461f0110ad22fff7001c28f7c*/
struct AppendEntriesReply;

struct AppendEntriesReply {
    uint64_t follower_append_ok{};
    uint64_t follower_current_term{};
    uint64_t follower_last_log_index{};
    uint64_t follower_ack_type{};
};
/*RUSTYCPP:GEN-END id=raft_messages.append_entries_reply*/

// ---------------------------------------------------------------------------
// EmptyAppendEntries (heartbeat / election trigger)
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub slot: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub ballot: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_current_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_site_id: u16,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_prev_log_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_prev_log_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_commit_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub trigger_election_now: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_append_ok: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_current_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_last_log_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_ack_type: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.heartbeat version=1 rust_sha256=116e7c9c4b2067115c90f2f1b46984f94bc9cc6cdc64cdeff465b0cbbb396f56*/
struct EmptyAppendEntriesReq;
struct EmptyAppendEntriesReply;

struct EmptyAppendEntriesReq {
    uint64_t slot{};
    int64_t ballot{};
    uint64_t leader_current_term{};
    uint16_t leader_site_id{};
    uint64_t leader_prev_log_index{};
    uint64_t leader_prev_log_term{};
    uint64_t leader_commit_index{};
    bool trigger_election_now{};
};

struct EmptyAppendEntriesReply {
    uint64_t follower_append_ok{};
    uint64_t follower_current_term{};
    uint64_t follower_last_log_index{};
    uint64_t follower_ack_type{};
};
/*RUSTYCPP:GEN-END id=raft_messages.heartbeat*/

// ---------------------------------------------------------------------------
// AppendEntriesDurable — follower acks that its log has been fsync'd.
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesDurableReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub term: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_id: u16,
    #[cfg_attr(any(), cpp_value_init)]
    pub last_log_index: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesDurableReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub acknowledged: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.durable version=1 rust_sha256=2c2d53634a2498010ad6d2f68cec2e7ababc54e598cf8326e188ccf1668a6b5e*/
struct AppendEntriesDurableReq;
struct AppendEntriesDurableReply;

struct AppendEntriesDurableReq {
    int64_t term{};
    uint16_t follower_id{};
    uint64_t last_log_index{};
};

struct AppendEntriesDurableReply {
    bool acknowledged{};
};
/*RUSTYCPP:GEN-END id=raft_messages.durable*/

// ---------------------------------------------------------------------------
// TimeoutNow — leader asks a follower to immediately start election
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct TimeoutNowReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_site_id: u16,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct TimeoutNowReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub success: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.timeout_now version=1 rust_sha256=906e5a4fc6ac262e0d5875fc3b18c65d74d79612504b1270d972dda099908d28*/
struct TimeoutNowReq;
struct TimeoutNowReply;

struct TimeoutNowReq {
    uint64_t leader_term{};
    uint16_t leader_site_id{};
};

struct TimeoutNowReply {
    uint64_t follower_term{};
    bool success{};
};
/*RUSTYCPP:GEN-END id=raft_messages.timeout_now*/

// ---------------------------------------------------------------------------
// NotifyRestart — after crash recovery, tell peers to reconnect.
// ---------------------------------------------------------------------------
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct NotifyRestartReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub restarted_site_id: u16,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct NotifyRestartReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub acknowledged: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.notify_restart version=1 rust_sha256=04dfe88fec699a31c6224d0b505624b43e32018c8ff40728b78f2e9c7b62559a*/
struct NotifyRestartReq;
struct NotifyRestartReply;

struct NotifyRestartReq {
    uint16_t restarted_site_id{};
};

struct NotifyRestartReply {
    bool acknowledged{};
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
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct InstallSnapshotReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub term_out: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.install_snapshot_reply version=1 rust_sha256=1750f8f5cd49db43490faabae9daa7597a7e0a316882e89f9067415b70c18a39*/
struct InstallSnapshotReply;

struct InstallSnapshotReply {
    uint64_t term_out{};
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
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct RemoveServerReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub server_id: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_messages.remove_server_req version=1 rust_sha256=9025ec0bc7cc43741651db96865a16260bcd09f58fa0c56f59a259c2596d62f6*/
struct RemoveServerReq;

struct RemoveServerReq {
    uint64_t term{};
    uint64_t server_id{};
};
/*RUSTYCPP:GEN-END id=raft_messages.remove_server_req*/

struct RemoveServerReply {
  bool        success{false};
  std::string error_msg;
  uint64_t    leader_hint{0};
};

}  // namespace raft
}  // namespace janus
