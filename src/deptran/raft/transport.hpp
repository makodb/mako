#pragma once

/**
 * @file transport.hpp
 * @brief Raft network transport abstraction.
 *
 * Polymorphism via an abstract base class. Concrete adapters inherit
 * from TransportBase and override the virtual `send_*` /
 * `self_site_id` methods.
 *
 * Design intent:
 *  - Every reply-expecting `send_*` method returns its reply type
 *    directly. The calling fiber yields (via rrr::IntEvent::Wait on the
 *    rrr side, or a reply-slot signaled by the channel worker on the
 *    in-memory side) until the reply arrives. No callbacks, no
 *    rusty::Function in the base signatures.
 *  - Fire-and-forget methods (vote-durable, append-durable,
 *    notify-restart) stay `void`.
 *  - No per-partition broadcast on the base. Leaders that want to
 *    vote a quorum spawn N fibers, each issuing a per-peer send_vote.
 *
 * Concrete adapters:
 *    RrrTransportAdapter      — wraps RaftCommo / rrr::Future.
 *    ChannelTransportAdapter  — wraps rusty::sync::mpsc.
 *
 * Rusty-safety:
 *  - Polymorphism via virtual dispatch with a virtual destructor.
 *  - std::shared_ptr appears only at the rrr-module boundary
 *    (MarshallDeputy carries std::shared_ptr<Marshallable>). Every
 *    such boundary is annotated `@unsafe`.
 */

#include <cstdint>

#include <rusty/box.hpp>

#include "../constants.h"

namespace janus {
namespace raft {

// Forward declarations to keep base construction independent of
// messages.hpp's import of the rrr module.
struct VoteReq;
struct VoteReply;
struct VoteDurableReq;
struct TimeoutNowReq;
struct TimeoutNowReply;
struct AppendEntriesReq;
struct AppendEntriesReply;
struct EmptyAppendEntriesReq;
struct EmptyAppendEntriesReply;
struct AppendEntriesDurableReq;
struct InstallSnapshotReq;
struct InstallSnapshotReply;

// ---------------------------------------------------------------------------
// TransportBase / TransportProxy
// ---------------------------------------------------------------------------

class TransportBase {
 public:
  virtual ~TransportBase() = default;

  // Reply-expecting RPCs: return reply by value. Caller blocks on a
  // fiber-yielding primitive inside the adapter.
  virtual AppendEntriesReply       send_append_entries(siteid_t /*dst*/, AppendEntriesReq) = 0;
  virtual EmptyAppendEntriesReply  send_empty_append_entries(siteid_t /*dst*/, EmptyAppendEntriesReq) = 0;
  virtual VoteReply                send_vote(siteid_t /*dst*/, VoteReq) = 0;
  virtual TimeoutNowReply          send_timeout_now(siteid_t /*dst*/, TimeoutNowReq) = 0;
  virtual InstallSnapshotReply     send_install_snapshot(siteid_t /*dst*/, InstallSnapshotReq) = 0;

  // Durable-ack and restart notifications are fire-and-forget by design.
  virtual void                     send_vote_durable(siteid_t /*candidate*/, VoteDurableReq) = 0;
  virtual void                     send_append_entries_durable(siteid_t /*leader*/, AppendEntriesDurableReq) = 0;
  virtual void                     send_notify_restart(siteid_t /*self*/, parid_t) = 0;

  // Identity — useful for adapters that need to know their own site
  // for logging / loopback suppression.
  virtual siteid_t                 self_site_id() const = 0;
};

// @safe - owning transport handle. The pointed-to implementation may still
// contain unsafe RPC/channel boundaries.
using TransportProxy = rusty::Box<TransportBase>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// transport.hpp.
#include "messages.hpp"
