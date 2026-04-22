#pragma once

/**
 * @file transport.hpp
 * @brief Raft network transport abstraction (Phase 1 of decouple plan).
 *
 * Polymorphism via pro::proxy (no inheritance). Concrete adapters are
 * plain C++ classes with matching method names; the proxy binds to them
 * via PRO_DEF_MEM_DISPATCH + facade_builder. See the MarshallableFacade
 * / MarshallableProxy pattern in src/rrr/misc/marshal.hpp for the
 * canonical in-tree example.
 *
 * Design intent:
 *  - Every outbound Raft RPC has a fire-and-forget send_* method on the
 *    facade. Replies come back via a rusty::Function<void(Reply)> the
 *    caller supplies at the call site. No rrr::Future, no
 *    rrr::DeferredReply, no Fiber yield in the facade signature.
 *  - Concrete adapters in scope:
 *      RrrTransportAdapter    (phase 2) — wraps RaftProxy / rrr::Future.
 *      ChannelTransportAdapter(phase 4) — wraps rusty::sync::mpsc.
 *
 * Rusty-safety:
 *  - All std::shared_ptr / std::function occurrences below come from
 *    the marshal.hpp rrr-module boundary (it's where our proxy library
 *    lives). Rusty facades live inside that module too, so the
 *    signatures below use rusty::Function to keep the contract clean.
 *  - The facade's send_install_snapshot and send_append_entries take
 *    ownership of the request by value; adapters move it into whatever
 *    downstream machinery they use.
 */

#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>

#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>

#include "messages.hpp"

import rrr;

#include "../constants.h"

namespace janus {
namespace raft {

// ---------------------------------------------------------------------------
// Callback aliases — reply types land on a rusty::Function delivered by the
// adapter when the RPC (or its quorum) completes.
// ---------------------------------------------------------------------------

using OnAppendEntriesReply =
    rusty::Function<void(siteid_t /* from */, AppendEntriesReply)>;
using OnVoteReply =
    rusty::Function<void(siteid_t /* from */, VoteReply)>;
using OnTimeoutNowReply =
    rusty::Function<void(siteid_t /* from */, TimeoutNowReply)>;
using OnInstallSnapshotReply =
    rusty::Function<void(siteid_t /* from */, InstallSnapshotReply)>;

// ---------------------------------------------------------------------------
// Per-method dispatch tags. The names chosen here are the method names a
// concrete adapter must define; the proxy library binds by name.
// ---------------------------------------------------------------------------

PRO_DEF_MEM_DISPATCH(TrSendAppendEntries,       send_append_entries);
PRO_DEF_MEM_DISPATCH(TrSendEmptyAppendEntries,  send_empty_append_entries);
PRO_DEF_MEM_DISPATCH(TrBroadcastVote,           broadcast_vote);
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
    ::add_convention<TrSendAppendEntries,
        void(siteid_t /* dst */,
             AppendEntriesReq,
             OnAppendEntriesReply)>
    ::add_convention<TrSendEmptyAppendEntries,
        void(siteid_t /* dst */,
             EmptyAppendEntriesReq,
             OnAppendEntriesReply)>
    // BroadcastVote sends to every peer of the partition; adapter
    // fans out and invokes the callback once per reply received.
    ::add_convention<TrBroadcastVote,
        void(parid_t,
             VoteReq,
             OnVoteReply)>
    ::add_convention<TrSendTimeoutNow,
        void(siteid_t /* dst */,
             TimeoutNowReq,
             OnTimeoutNowReply)>
    // Durable-ack RPCs are fire-and-forget on purpose.
    ::add_convention<TrSendVoteDurable,
        void(siteid_t /* candidate */, VoteDurableReq)>
    ::add_convention<TrSendAppendEntriesDurable,
        void(siteid_t /* leader */, AppendEntriesDurableReq)>
    ::add_convention<TrSendNotifyRestart,
        void(siteid_t /* self */, parid_t)>
    ::add_convention<TrSendInstallSnapshot,
        void(siteid_t /* dst */,
             InstallSnapshotReq,
             OnInstallSnapshotReply)>
    // Identity — useful for adapters that need to know their own site
    // for logging / loopback suppression.
    ::add_convention<TrSelfSiteId, siteid_t() const>
    ::build {};

using TransportProxy = pro::proxy<TransportFacade>;

}  // namespace raft
}  // namespace janus
