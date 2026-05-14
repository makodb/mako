#pragma once

/**
 * @file transport.hpp
 * @brief Raft network transport abstraction.
 *
 * Polymorphism via pro::proxy (no inheritance). Concrete adapters are
 * plain C++ classes with matching method names; the proxy binds to them
 * via PRO_DEF_MEM_DISPATCH + facade_builder.
 *
 * Design intent:
 *  - Every reply-expecting `send_*` method returns its reply type
 *    directly. The calling fiber yields (via rrr::IntEvent::Wait on the
 *    rrr side, or a reply-slot signaled by the channel worker on the
 *    in-memory side) until the reply arrives. No callbacks, no
 *    rusty::Function in the facade signatures.
 *  - Fire-and-forget methods (vote-durable, append-durable,
 *    notify-restart) stay `void`.
 *  - No per-partition broadcast on the facade. Leaders that want to
 *    vote a quorum spawn N fibers, each issuing a per-peer send_vote.
 *
 * Concrete adapters:
 *    RrrTransportAdapter      — wraps RaftCommo / rrr::Future.
 *    ChannelTransportAdapter  — wraps rusty::sync::mpsc.
 *
 * Rusty-safety:
 *  - No inheritance; polymorphism via pro::proxy.
 *  - std::shared_ptr appears only at the rrr-module boundary
 *    (MarshallDeputy carries std::shared_ptr<Marshallable>). Every
 *    such boundary is annotated `@unsafe`.
 */

#include <cstdint>

// deptran/constants.h defines macro RR, which collides with template
// parameter names in proxy headers (e.g. class RR in proxy/v4/proxy.h).
// Keep proxy includes insulated from that macro.
#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RAFT_TRANSPORT_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RAFT_TRANSPORT_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RAFT_TRANSPORT_RESTORE_RR_MACRO
#endif

#include "../constants.h"

namespace janus {
namespace raft {

// Forward declarations to keep facade construction independent of
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
// Per-method dispatch tags.
// ---------------------------------------------------------------------------

PRO_DEF_MEM_DISPATCH(TrSendAppendEntries,       send_append_entries);
PRO_DEF_MEM_DISPATCH(TrSendEmptyAppendEntries,  send_empty_append_entries);
PRO_DEF_MEM_DISPATCH(TrSendVote,                send_vote);
PRO_DEF_MEM_DISPATCH(TrSendTimeoutNow,          send_timeout_now);
PRO_DEF_MEM_DISPATCH(TrSendVoteDurable,         send_vote_durable);
PRO_DEF_MEM_DISPATCH(TrSendAppendEntriesDurable,send_append_entries_durable);
PRO_DEF_MEM_DISPATCH(TrSendNotifyRestart,       send_notify_restart);
PRO_DEF_MEM_DISPATCH(TrSendInstallSnapshot,     send_install_snapshot);
PRO_DEF_MEM_DISPATCH(TrSelfSiteId,              self_site_id);

// ---------------------------------------------------------------------------
// TransportFacade / TransportProxy
// ---------------------------------------------------------------------------

struct TransportFacade : pro::facade_builder
    // Reply-expecting RPCs: return reply by value. Caller blocks on a
    // fiber-yielding primitive inside the adapter.
    ::add_convention<TrSendAppendEntries,
        AppendEntriesReply(siteid_t /* dst */, AppendEntriesReq)>
    ::add_convention<TrSendEmptyAppendEntries,
        EmptyAppendEntriesReply(siteid_t /* dst */, EmptyAppendEntriesReq)>
    ::add_convention<TrSendVote,
        VoteReply(siteid_t /* dst */, VoteReq)>
    ::add_convention<TrSendTimeoutNow,
        TimeoutNowReply(siteid_t /* dst */, TimeoutNowReq)>
    ::add_convention<TrSendInstallSnapshot,
        InstallSnapshotReply(siteid_t /* dst */, InstallSnapshotReq)>
    // Durable-ack and restart notifications are fire-and-forget by design.
    ::add_convention<TrSendVoteDurable,
        void(siteid_t /* candidate */, VoteDurableReq)>
    ::add_convention<TrSendAppendEntriesDurable,
        void(siteid_t /* leader */, AppendEntriesDurableReq)>
    ::add_convention<TrSendNotifyRestart,
        void(siteid_t /* self */, parid_t)>
    // Identity — useful for adapters that need to know their own site
    // for logging / loopback suppression.
    ::add_convention<TrSelfSiteId, siteid_t() const>
    ::build {};

using TransportProxy = pro::proxy<TransportFacade>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// transport.hpp.
#include "messages.hpp"
