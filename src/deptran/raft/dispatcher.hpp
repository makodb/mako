#pragma once

/**
 * @file dispatcher.hpp
 * @brief Raft inbound-RPC dispatcher abstraction. Mirrors transport.hpp
 *        but for the receiver side.
 *
 * Design intent:
 *  - Every `handle_*` method on the base returns its reply type
 *    directly. The transport (rrr shim or in-memory channel worker)
 *    invokes the dispatcher and uses the returned value as the reply
 *    payload. No rrr::DeferredReply, no callbacks, no rusty::Function.
 *  - Concrete adapters:
 *      RaftServerDispatcher — delegates to RaftServer methods synchronously.
 *      A recording adapter in the tests that returns fixed replies.
 *
 * Rusty-safety:
 *  - Polymorphism via an abstract base class with a virtual destructor.
 *  - DispatcherBase is a DSL-owned trait; concrete dispatchers keep ownership
 *    and server lifetimes in C++.
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

#if RUSTYCPP_RUST
pub trait DispatcherBase {
    fn handle_vote(&mut self, req: VoteReq) -> VoteReply;
    fn handle_vote_durable(&mut self, req: VoteDurableReq) -> VoteDurableReply;
    fn handle_append_entries(&mut self, req: AppendEntriesReq) -> AppendEntriesReply;
    fn handle_empty_append_entries(&mut self, req: EmptyAppendEntriesReq) -> EmptyAppendEntriesReply;
    fn handle_append_entries_durable(&mut self, req: AppendEntriesDurableReq) -> AppendEntriesDurableReply;
    fn handle_timeout_now(&mut self, req: TimeoutNowReq) -> TimeoutNowReply;
    fn handle_notify_restart(&mut self, req: NotifyRestartReq) -> NotifyRestartReply;
    fn handle_install_snapshot(&mut self, req: InstallSnapshotReq) -> InstallSnapshotReply;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=dispatcher.1 version=1 rust_sha256=f744d708debc2cdeb8bae874ca2b85401771572e6327b2c78b120cfc561ac365*/
namespace {
class DispatcherBase {
public:
    virtual ~DispatcherBase() noexcept(false) {}
    virtual VoteReply handle_vote(VoteReq req) = 0;
    virtual VoteDurableReply handle_vote_durable(VoteDurableReq req) = 0;
    virtual AppendEntriesReply handle_append_entries(AppendEntriesReq req) = 0;
    virtual EmptyAppendEntriesReply handle_empty_append_entries(EmptyAppendEntriesReq req) = 0;
    virtual AppendEntriesDurableReply handle_append_entries_durable(AppendEntriesDurableReq req) = 0;
    virtual TimeoutNowReply handle_timeout_now(TimeoutNowReq req) = 0;
    virtual NotifyRestartReply handle_notify_restart(NotifyRestartReq req) = 0;
    virtual InstallSnapshotReply handle_install_snapshot(InstallSnapshotReq req) = 0;
    DispatcherBase(const DispatcherBase&) = delete;
    DispatcherBase& operator=(const DispatcherBase&) = delete;
    DispatcherBase(DispatcherBase&&) = delete;
    DispatcherBase& operator=(DispatcherBase&&) = delete;
protected:
    DispatcherBase() = default;
};
}

template <class U> class DispatcherBaseAdapter;
template <class U> class DispatcherBaseAdapterRef;
template <class U> class DispatcherBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=dispatcher.1*/

using DispatcherProxy = rusty::Box<DispatcherBase>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// dispatcher.hpp.
#include "messages.hpp"
