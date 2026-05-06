#pragma once

/**
 * @file dispatcher.hpp
 * @brief Raft inbound-RPC dispatcher abstraction. Mirrors transport.hpp
 *        but for the receiver side.
 *
 * Design intent:
 *  - Every `handle_*` method on the facade returns its reply type
 *    directly. The transport (rrr shim or in-memory channel worker)
 *    invokes the dispatcher and uses the returned value as the reply
 *    payload. No rrr::DeferredReply, no callbacks, no rusty::Function.
 *  - Concrete adapters:
 *      RaftServerDispatcher (phase 8.2) — delegates to RaftServer
 *                                         methods synchronously.
 *      A recording adapter in the tests that returns fixed replies.
 *
 * Rusty-safety:
 *  - Polymorphism via pro::proxy; no inheritance.
 *  - Reply structs are passed by value.
 */

#include <cstdint>

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
        VoteReply(VoteReq)>
    ::add_convention<DpHandleVoteDurable,
        VoteDurableReply(VoteDurableReq)>
    ::add_convention<DpHandleAppendEntries,
        AppendEntriesReply(AppendEntriesReq)>
    ::add_convention<DpHandleEmptyAppendEntries,
        EmptyAppendEntriesReply(EmptyAppendEntriesReq)>
    ::add_convention<DpHandleAppendEntriesDurable,
        AppendEntriesDurableReply(AppendEntriesDurableReq)>
    ::add_convention<DpHandleTimeoutNow,
        TimeoutNowReply(TimeoutNowReq)>
    ::add_convention<DpHandleNotifyRestart,
        NotifyRestartReply(NotifyRestartReq)>
    ::add_convention<DpHandleInstallSnapshot,
        InstallSnapshotReply(InstallSnapshotReq)>
    ::build {};

using DispatcherProxy = pro::proxy<DispatcherFacade>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// dispatcher.hpp.
#include "messages.hpp"
