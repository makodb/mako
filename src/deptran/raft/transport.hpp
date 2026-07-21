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

#if RUSTYCPP_RUST
pub trait TransportBase {
    // @unsafe - May block on RPC/channel reply path.
    fn send_append_entries(&mut self, dst: u16, req: AppendEntriesReq)
        -> AppendEntriesReply;
    // @unsafe - May block on RPC/channel reply path.
    fn send_empty_append_entries(&mut self, dst: u16, req: EmptyAppendEntriesReq)
        -> EmptyAppendEntriesReply;
    // @unsafe - May block on RPC/channel reply path.
    fn send_vote(&mut self, dst: u16, req: VoteReq) -> VoteReply;
    // @unsafe - May block on RPC/channel reply path.
    fn send_timeout_now(&mut self, dst: u16, req: TimeoutNowReq) -> TimeoutNowReply;
    // @unsafe - May block on RPC/channel reply path.
    fn send_install_snapshot(&mut self, dst: u16, req: InstallSnapshotReq)
        -> InstallSnapshotReply;

    // @unsafe - Fire-and-forget RPC/channel send.
    fn send_vote_durable(&mut self, candidate: u16, req: VoteDurableReq);
    // @unsafe - Fire-and-forget RPC/channel send.
    fn send_append_entries_durable(&mut self, leader: u16,
                                   req: AppendEntriesDurableReq);
    // @unsafe - Fire-and-forget RPC/channel send.
    fn send_notify_restart(&mut self, self_site: u16, par: u32);

    // @safe
    fn self_site_id(&self) -> u16;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=transport.1 version=1 rust_sha256=f5f3b10fc05ead3090b5aff9a03ca2791fd3660942e55501e1da88162220fd15*/
namespace {
class TransportBase {
public:
    virtual ~TransportBase() noexcept(false) {}
    virtual AppendEntriesReply send_append_entries(uint16_t dst, AppendEntriesReq req) = 0;
    virtual EmptyAppendEntriesReply send_empty_append_entries(uint16_t dst, EmptyAppendEntriesReq req) = 0;
    virtual VoteReply send_vote(uint16_t dst, VoteReq req) = 0;
    virtual TimeoutNowReply send_timeout_now(uint16_t dst, TimeoutNowReq req) = 0;
    virtual InstallSnapshotReply send_install_snapshot(uint16_t dst, InstallSnapshotReq req) = 0;
    virtual void send_vote_durable(uint16_t candidate, VoteDurableReq req) = 0;
    virtual void send_append_entries_durable(uint16_t leader, AppendEntriesDurableReq req) = 0;
    virtual void send_notify_restart(uint16_t self_site, uint32_t par) = 0;
    virtual uint16_t self_site_id() const = 0;
    TransportBase(const TransportBase&) = delete;
    TransportBase& operator=(const TransportBase&) = delete;
    TransportBase(TransportBase&&) = delete;
    TransportBase& operator=(TransportBase&&) = delete;
protected:
    TransportBase() = default;
};
}

template <class U> class TransportBaseAdapter;
template <class U> class TransportBaseAdapterRef;
template <class U> class TransportBaseAdapterRefMut;
/*RUSTYCPP:GEN-END id=transport.1*/

// @safe - owning transport handle. The pointed-to implementation may still
// contain unsafe RPC/channel boundaries.
using TransportProxy = rusty::Box<TransportBase>;

}  // namespace raft
}  // namespace janus

// Provide complete RPC payload definitions to callers that include only
// transport.hpp.
#include "messages.hpp"
