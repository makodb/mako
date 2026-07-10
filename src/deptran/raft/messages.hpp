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
#if RUSTYCPP_RUST
pub struct VoteReq {
    last_log_idx: u64,
    last_log_term: i64,
    candidate_site_id: u16,
    current_term: i64,
}

pub struct VoteReply {
    max_ballot: i64,
    vote_granted: bool,
}

pub struct VoteDurableReq {
    term: i64,
    voter_id: u16,
}

pub struct VoteDurableReply {
    acknowledged: bool,
}

pub struct AppendEntriesDurableReq {
    term: i64,
    follower_id: u16,
    last_log_index: u64,
}

pub struct AppendEntriesDurableReply {
    acknowledged: bool,
}

pub struct TimeoutNowReq {
    leader_term: u64,
    leader_site_id: u16,
}

pub struct TimeoutNowReply {
    follower_term: u64,
    success: bool,
}

pub struct NotifyRestartReq {
    restarted_site_id: u16,
}

pub struct NotifyRestartReply {
    acknowledged: bool,
}

pub struct InstallSnapshotReply {
    term_out: u64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=messages.1 version=1 rust_sha256=146a5de5df5a25274e7d90735cd3a752da7e183ceb8f8f8a7fbe1e36b7e21c8d*/
struct VoteReq;
struct VoteReply;
struct VoteDurableReq;
struct VoteDurableReply;
struct AppendEntriesDurableReq;
struct AppendEntriesDurableReply;
struct TimeoutNowReq;
struct TimeoutNowReply;
struct NotifyRestartReq;
struct NotifyRestartReply;
struct InstallSnapshotReply;

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

struct VoteDurableReq {
    int64_t term;
    uint16_t voter_id;
};

struct VoteDurableReply {
    bool acknowledged;
};

struct AppendEntriesDurableReq {
    int64_t term;
    uint16_t follower_id;
    uint64_t last_log_index;
};

struct AppendEntriesDurableReply {
    bool acknowledged;
};

struct TimeoutNowReq {
    uint64_t leader_term;
    uint16_t leader_site_id;
};

struct TimeoutNowReply {
    uint64_t follower_term;
    bool success;
};

struct NotifyRestartReq {
    uint16_t restarted_site_id;
};

struct NotifyRestartReply {
    bool acknowledged;
};

struct InstallSnapshotReply {
    uint64_t term_out;
};
/*RUSTYCPP:GEN-END id=messages.1*/

// ---------------------------------------------------------------------------
// VoteDurable — sent by a voter once its vote has been persisted.
// ---------------------------------------------------------------------------
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

struct AppendEntriesReply {
  uint64_t follower_append_ok{0};
  uint64_t follower_current_term{0};
  uint64_t follower_last_log_index{0};
  uint64_t follower_ack_type{0};
};

// ---------------------------------------------------------------------------
// EmptyAppendEntries (heartbeat / election trigger)
// ---------------------------------------------------------------------------
struct EmptyAppendEntriesReq {
  uint64_t slot{0};
  ballot_t ballot{0};
  uint64_t leader_current_term{0};
  siteid_t leader_site_id{0};
  uint64_t leader_prev_log_index{0};
  uint64_t leader_prev_log_term{0};
  uint64_t leader_commit_index{0};
  bool     trigger_election_now{false};
};

struct EmptyAppendEntriesReply {
  uint64_t follower_append_ok{0};
  uint64_t follower_current_term{0};
  uint64_t follower_last_log_index{0};
  uint64_t follower_ack_type{0};
};





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

struct RemoveServerReq {
  uint64_t term{0};
  uint64_t server_id{0};
};

struct RemoveServerReply {
  bool        success{false};
  std::string error_msg;
  uint64_t    leader_hint{0};
};

}  // namespace raft
}  // namespace janus
