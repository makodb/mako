#pragma once

/**
 * @file dispatcher.hpp
 * @brief Raft inbound-RPC dispatcher abstraction (Phase 3 of decouple
 *        plan). Mirrors transport.hpp but for the receiver side.
 *
 * Design intent:
 *  - Every inbound Raft RPC has a `handle_*` method on the facade taking
 *    the request struct and a reply callback. The transport (rrr shim or
 *    in-memory channel worker) drains incoming messages and calls the
 *    dispatcher. No rrr::DeferredReply, no Fiber yield, no rrr::Service
 *    base class in the facade signature.
 *  - Concrete adapters:
 *      RaftServerDispatcher (phase 3.5) — delegates to RaftServer
 *                                         methods synchronously (or via
 *                                         a Fiber on the rrr side).
 *      ChannelDispatcher    (phase 4)   — drives the same RaftServer
 *                                         from the in-memory worker
 *                                         thread.
 *
 * Rusty-safety:
 *  - Polymorphism via pro::proxy; no inheritance.
 *  - Callbacks are rusty::Function; the reply payload is handed back
 *    by value.
 */

#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/function.hpp>
#include <rusty/sync/atomic.hpp>

// deptran/constants.h defines macro RR, which can collide with template
// parameter names inside proxy headers. Protect proxy includes.
#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RAFT_DISPATCHER_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RAFT_DISPATCHER_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RAFT_DISPATCHER_RESTORE_RR_MACRO
#endif

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
// Per-handler reply callbacks. The dispatcher invokes these when a
// handler finishes; the transport then marshals the reply back to the
// caller.
// ---------------------------------------------------------------------------

using OnVoteReplyDispatch           = rusty::Function<void(VoteReply)>;
using OnVoteDurableReplyDispatch    = rusty::Function<void(VoteDurableReply)>;
using OnAppendEntriesReplyDispatch  = rusty::Function<void(AppendEntriesReply)>;
using OnEmptyAppendEntriesReplyDispatch =
    rusty::Function<void(EmptyAppendEntriesReply)>;
using OnAppendEntriesDurableReplyDispatch =
    rusty::Function<void(AppendEntriesDurableReply)>;
using OnTimeoutNowReplyDispatch     = rusty::Function<void(TimeoutNowReply)>;
using OnNotifyRestartReplyDispatch  = rusty::Function<void(NotifyRestartReply)>;
using OnInstallSnapshotReplyDispatch =
    rusty::Function<void(InstallSnapshotReply)>;

// ---------------------------------------------------------------------------
// Per-method dispatch tags.
// ---------------------------------------------------------------------------

PRO_DEF_MEM_DISPATCH(DpHandleVote,                  handle_vote);
PRO_DEF_MEM_DISPATCH(DpHandleVoteDurable,           handle_vote_durable);
PRO_DEF_MEM_DISPATCH(DpHandleAppendEntries,         handle_append_entries);
PRO_DEF_MEM_DISPATCH(DpHandleEmptyAppendEntries,    handle_empty_append_entries);
PRO_DEF_MEM_DISPATCH(DpHandleAppendEntriesDurable,  handle_append_entries_durable);
PRO_DEF_MEM_DISPATCH(DpHandleTimeoutNow,            handle_timeout_now);
PRO_DEF_MEM_DISPATCH(DpHandleNotifyRestart,         handle_notify_restart);
PRO_DEF_MEM_DISPATCH(DpHandleInstallSnapshot,       handle_install_snapshot);

// ---------------------------------------------------------------------------
// DispatcherFacade / DispatcherProxy
// ---------------------------------------------------------------------------

struct DispatcherFacade : pro::facade_builder
    ::add_convention<DpHandleVote,
        void(VoteReq, OnVoteReplyDispatch)>
    ::add_convention<DpHandleVoteDurable,
        void(VoteDurableReq, OnVoteDurableReplyDispatch)>
    ::add_convention<DpHandleAppendEntries,
        void(AppendEntriesReq, OnAppendEntriesReplyDispatch)>
    ::add_convention<DpHandleEmptyAppendEntries,
        void(EmptyAppendEntriesReq, OnEmptyAppendEntriesReplyDispatch)>
    ::add_convention<DpHandleAppendEntriesDurable,
        void(AppendEntriesDurableReq, OnAppendEntriesDurableReplyDispatch)>
    ::add_convention<DpHandleTimeoutNow,
        void(TimeoutNowReq, OnTimeoutNowReplyDispatch)>
    ::add_convention<DpHandleNotifyRestart,
        void(NotifyRestartReq, OnNotifyRestartReplyDispatch)>
    ::add_convention<DpHandleInstallSnapshot,
        void(InstallSnapshotReq, OnInstallSnapshotReplyDispatch)>
    ::build {};

using DispatcherProxy = pro::proxy<DispatcherFacade>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// dispatcher.hpp.
#include "messages.hpp"
