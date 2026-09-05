#pragma once

/**
 * @file dispatcher.hpp
 * @brief Raft inbound-RPC dispatcher abstraction. Mirrors transport.hpp
 *        but for the receiver side.
 *
 * Design intent:
 *  - Every `handle_*` method on the base returns its reply type
 *    directly. The transport (srpc shim or in-memory channel worker)
 *    invokes the dispatcher and uses the returned value as the reply
 *    payload. No srpc::DeferredReply, no callbacks, no rusty::Function.
 *  - Concrete adapters:
 *      RaftServerDispatcher (phase 8.2) — delegates to RaftServer
 *                                         methods synchronously.
 *      A recording adapter in the tests that returns fixed replies.
 *
 * Rusty-safety:
 *  - Polymorphism via an abstract base class with a virtual destructor.
 *  - Reply structs are passed by value.
 */

#include <cstdint>

#include <rusty/box.hpp>

#include "../constants.h"

namespace janus {
namespace raft {

// Forward declarations; full definitions are pulled in at the end of file.
struct VoteReq;
struct VoteReply;
struct VoteDurableReq;
struct VoteDurableReply;
struct AppendEntriesReq;
struct AppendEntriesReply;
struct EmptyAppendEntriesReq;
struct EmptyAppendEntriesReply;
struct AppendEntriesDurableReq;
struct AppendEntriesDurableReply;
struct TimeoutNowReq;
struct TimeoutNowReply;
struct NotifyRestartReq;
struct NotifyRestartReply;
struct InstallSnapshotReq;
struct InstallSnapshotReply;

// ---------------------------------------------------------------------------
// DispatcherBase / DispatcherProxy
// ---------------------------------------------------------------------------

class DispatcherBase {
 public:
  virtual ~DispatcherBase() = default;

  virtual VoteReply                 handle_vote(VoteReq) = 0;
  virtual VoteDurableReply          handle_vote_durable(VoteDurableReq) = 0;
  virtual AppendEntriesReply        handle_append_entries(AppendEntriesReq) = 0;
  virtual EmptyAppendEntriesReply   handle_empty_append_entries(EmptyAppendEntriesReq) = 0;
  virtual AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq) = 0;
  virtual TimeoutNowReply           handle_timeout_now(TimeoutNowReq) = 0;
  virtual NotifyRestartReply        handle_notify_restart(NotifyRestartReq) = 0;
  virtual InstallSnapshotReply      handle_install_snapshot(InstallSnapshotReq) = 0;
};

using DispatcherProxy = rusty::Box<DispatcherBase>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// dispatcher.hpp.
#include "messages.hpp"
