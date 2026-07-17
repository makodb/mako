#pragma once

/**
 * @file channel_transport.hpp
 * @brief In-process Raft transport built on rusty::sync::mpsc. Enables
 *        raft tests to run without sockets, rrr, or PollThreads.
 *
 * Architecture:
 *   - ChannelSwitchboard owns one mpsc channel per site and a small
 *     fault-injection state machine (drop direction, partition two
 *     groups, reset).
 *   - ChannelTransportAdapter satisfies TransportBase. Each
 *     reply-expecting send_* allocates a one-shot reply channel,
 *     pushes an Envelope carrying the deliver-closure + reply sender,
 *     then blocks on the reply receiver. The caller's thread parks in
 *     recv() until the remote worker fills the slot.
 *   - ChannelNodeWorker drains envelopes and invokes each envelope's
 *     deliver closure against the local DispatcherProxy. The closure
 *     synchronously calls the matching handle_* and forwards its
 *     return value through the reply sender.
 *   - Fire-and-forget methods push an envelope whose deliver closure
 *     calls handle_* and discards the return value.
 *
 * Rusty-safety:
 *  - No inheritance, no std::thread, no std::mutex.
 *  - Envelope + every Reply type are explicitly marked Send so they
 *    can cross mpsc boundaries.
 */

#include <cstdint>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/sync/mpsc.hpp>

#include "transport.hpp"
#include "dispatcher.hpp"

#include "../constants.h"

namespace janus {
namespace raft {

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

// Carries one outbound RPC across the switchboard. `deliver` is a
// closure bound by the sender: when the receiving worker invokes it
// with its local dispatcher, it calls the matching handle_* and
// forwards the return value through whatever reply channel the
// closure captured.
using EnvelopeDeliverFn = rusty::Function<void(DispatcherProxy&)>;

#if RUSTYCPP_RUST
pub struct Envelope {
    from: siteid_t,
    to: siteid_t,
    deliver: EnvelopeDeliverFn,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel_transport.1 version=1 rust_sha256=fb1f7f720fe2acb8a1d137369c772851558644ca1b43c78129e51b1de8b6806c*/
struct Envelope;

struct Envelope {
    siteid_t from;
    siteid_t to;
    EnvelopeDeliverFn deliver;
};
/*RUSTYCPP:GEN-END id=channel_transport.1*/

}  // namespace raft
}  // namespace janus

// ---------------------------------------------------------------------------
// Send-trait registrations.
// Every type crossing an mpsc boundary (Envelope itself + each Reply
// type carried in a one-shot reply channel) must be marked Send.
// ---------------------------------------------------------------------------
namespace rusty {
template <> struct is_send<janus::raft::Envelope>                 : std::true_type {};
template <> struct is_send<janus::raft::VoteReply>                : std::true_type {};
template <> struct is_send<janus::raft::VoteDurableReply>         : std::true_type {};
template <> struct is_send<janus::raft::AppendEntriesReply>       : std::true_type {};
template <> struct is_send<janus::raft::EmptyAppendEntriesReply>  : std::true_type {};
template <> struct is_send<janus::raft::AppendEntriesDurableReply>: std::true_type {};
template <> struct is_send<janus::raft::TimeoutNowReply>          : std::true_type {};
template <> struct is_send<janus::raft::NotifyRestartReply>       : std::true_type {};
template <> struct is_send<janus::raft::InstallSnapshotReply>     : std::true_type {};
}  // namespace rusty

namespace janus {
namespace raft {

// ---------------------------------------------------------------------------
// Fault-injection configuration
// ---------------------------------------------------------------------------

struct ChannelFaults;

inline bool channel_faults_is_dropped(const ChannelFaults& faults,
                                      siteid_t from,
                                      siteid_t to);
inline int channel_faults_find_partition(const ChannelFaults& faults,
                                         siteid_t site);

#if RUSTYCPP_RUST
pub struct ChannelFaults {
    dropped: std::vector<std::pair<u16, u16>>,
    partitions: std::vector<std::vector<u16>>,
}

impl ChannelFaults {
    fn is_dropped(&self, from: u16, to: u16) -> bool {
        channel_faults_is_dropped(self, from, to)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel_transport.faults version=1 rust_sha256=d72e873ce88c41732e9b246ee3a1e5ca0e550d9d286e4405f6c45e41a1faba83*/
struct ChannelFaults;

struct ChannelFaults {
    std::vector<std::pair<uint16_t, uint16_t>> dropped;
    std::vector<std::vector<uint16_t>> partitions;

    bool is_dropped(uint16_t from, uint16_t to) const;
};


bool ChannelFaults::is_dropped(uint16_t from, uint16_t to) const {
    return channel_faults_is_dropped((*this), std::move(from), std::move(to));
}
/*RUSTYCPP:GEN-END id=channel_transport.faults*/

// @safe - stateless predicates used by the in-process transport. Channel
// ownership, closure delivery, blocking recv loops, and switchboard mutation
// stay in C++; these helpers only compare copied site/partition ids.
#if RUSTYCPP_RUST
pub fn channel_faults_drop_matches(drop_from: u16,
                                   drop_to: u16,
                                   from: u16,
                                   to: u16) -> bool {
    drop_from == from && drop_to == to
}

pub fn channel_faults_partitions_block(from_partition: i32,
                                       to_partition: i32) -> bool {
    from_partition != to_partition
}

pub fn channel_envelope_matches_destination(envelope_to: u16,
                                            site: u16) -> bool {
    envelope_to == site
}
#endif
/*RUSTYCPP:GEN-BEGIN id=channel_transport.small_helpers version=1 rust_sha256=79f5eaf4cbb3e6137d5ee633b66a7a7ee99d2db35066a1c67b0df7e0cfc45173*/
bool channel_faults_drop_matches(uint16_t drop_from, uint16_t drop_to, uint16_t from, uint16_t to);
bool channel_faults_partitions_block(int32_t from_partition, int32_t to_partition);
bool channel_envelope_matches_destination(uint16_t envelope_to, uint16_t site);

bool channel_faults_drop_matches(uint16_t drop_from, uint16_t drop_to, uint16_t from, uint16_t to) {
    return (rusty::detail::deref_if_pointer_like(drop_from) == rusty::detail::deref_if_pointer_like(from)) && (rusty::detail::deref_if_pointer_like(drop_to) == rusty::detail::deref_if_pointer_like(to));
}

bool channel_faults_partitions_block(int32_t from_partition, int32_t to_partition) {
    return rusty::detail::deref_if_pointer_like(from_partition) != rusty::detail::deref_if_pointer_like(to_partition);
}

bool channel_envelope_matches_destination(uint16_t envelope_to, uint16_t site) {
    return rusty::detail::deref_if_pointer_like(envelope_to) == rusty::detail::deref_if_pointer_like(site);
}
/*RUSTYCPP:GEN-END id=channel_transport.small_helpers*/

inline bool channel_faults_is_dropped(const ChannelFaults& faults,
                                      siteid_t from,
                                      siteid_t to) {
  for (auto& p : faults.dropped) {
    if (channel_faults_drop_matches(p.first, p.second, from, to)) return true;
  }
  if (!faults.partitions.empty()) {
    auto pf = channel_faults_find_partition(faults, from);
    auto pt = channel_faults_find_partition(faults, to);
    if (channel_faults_partitions_block(pf, pt)) return true;
  }
  return false;
}

inline int channel_faults_find_partition(const ChannelFaults& faults,
                                         siteid_t site) {
  for (size_t i = 0; i < faults.partitions.size(); ++i) {
    for (auto x : faults.partitions[i]) {
      if (x == site) return static_cast<int>(i);
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// ChannelSwitchboard
// ---------------------------------------------------------------------------

class ChannelSwitchboard {
 public:
  // @safe
  ChannelSwitchboard() = default;

  // @safe - registers a new site. Returns the receiver side.
  rusty::sync::mpsc::Receiver<Envelope> register_site(siteid_t s) {
    auto [tx, rx] = rusty::sync::mpsc::channel<Envelope>();
    senders_.push_back({s, std::move(tx)});
    return std::move(rx);
  }

  // @unsafe { pushes into mpsc; drops silently if the dest is gone }
  void send(Envelope env) {
    if (faults_.is_dropped(env.from, env.to)) return;
    for (auto& pair : senders_) {
      if (channel_envelope_matches_destination(env.to, pair.first)) {
        (void)pair.second.send(std::move(env));
        return;
      }
    }
  }

  // @safe - fault-injection accessors
  void drop_direction(siteid_t from, siteid_t to) {
    faults_.dropped.emplace_back(from, to);
  }
  void partition(std::vector<std::vector<siteid_t>> groups) {
    faults_.partitions = std::move(groups);
  }
  void reset_faults() {
    faults_ = ChannelFaults{};
  }

 private:
  std::vector<std::pair<siteid_t, rusty::sync::mpsc::Sender<Envelope>>> senders_;
  ChannelFaults faults_{};
};

// ---------------------------------------------------------------------------
// Helper: build a fire-and-forget deliver closure.
// ---------------------------------------------------------------------------
namespace detail {

template <typename Handle>
// @safe
inline rusty::Function<void(DispatcherProxy&)>
make_fire_and_forget(Handle handle) {
  return rusty::Function<void(DispatcherProxy&)>(
      [handle = std::move(handle)](DispatcherProxy& disp) mutable {
        handle(disp);
      });
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ChannelTransportAdapter — satisfies TransportBase (fiber-sync).
// ---------------------------------------------------------------------------

class ChannelTransportAdapter : public TransportBase {
 public:
  // @unsafe { non-owning switchboard pointer }
  ChannelTransportAdapter(ChannelSwitchboard* sw, siteid_t self, parid_t par)
      : sw_(sw), self_(self), par_(par) {}

  // @safe
  siteid_t self_site_id() const override { return self_; }

  // ------------------------------------------------------------------
  // Reply-expecting RPCs.
  //
  // Each method:
  //   1. Allocates a one-shot rusty::sync::mpsc reply channel for the
  //      matching Reply type.
  //   2. Builds an Envelope whose deliver closure captures the req and
  //      the reply sender.
  //   3. Pushes the envelope through the switchboard.
  //   4. Blocks on the reply receiver until the remote worker fills it.
  //
  // If the switchboard drops the envelope (fault injection / unknown
  // dest), the reply channel's sender is destroyed, recv() returns
  // Err(Disconnected), and we fall back to a default-constructed Reply
  // (which matches the "server down" shape of the old OnDisconnected
  // path).
  // ------------------------------------------------------------------

  // @unsafe { mpsc bridge }
  AppendEntriesReply send_append_entries(siteid_t dst, AppendEntriesReq req) override {
    auto [tx, rx] = rusty::sync::mpsc::channel<AppendEntriesReply>();
    Envelope env{self_, dst,
        rusty::Function<void(DispatcherProxy&)>(
            [req = std::move(req), tx = std::move(tx)](DispatcherProxy& disp) mutable {
              (void)tx.send(disp->handle_append_entries(std::move(req)));
            })};
    sw_->send(std::move(env));
    auto r = rx.recv();
    if (r.is_err()) return AppendEntriesReply{};
    return r.unwrap();
  }

  // @unsafe { mpsc bridge }
  EmptyAppendEntriesReply send_empty_append_entries(siteid_t dst,
                                                    EmptyAppendEntriesReq req) override {
    auto [tx, rx] = rusty::sync::mpsc::channel<EmptyAppendEntriesReply>();
    Envelope env{self_, dst,
        rusty::Function<void(DispatcherProxy&)>(
            [req = std::move(req), tx = std::move(tx)](DispatcherProxy& disp) mutable {
              (void)tx.send(disp->handle_empty_append_entries(std::move(req)));
            })};
    sw_->send(std::move(env));
    auto r = rx.recv();
    if (r.is_err()) return EmptyAppendEntriesReply{};
    return r.unwrap();
  }

  // @unsafe { mpsc bridge }
  VoteReply send_vote(siteid_t dst, VoteReq req) override {
    auto [tx, rx] = rusty::sync::mpsc::channel<VoteReply>();
    Envelope env{self_, dst,
        rusty::Function<void(DispatcherProxy&)>(
            [req = std::move(req), tx = std::move(tx)](DispatcherProxy& disp) mutable {
              (void)tx.send(disp->handle_vote(std::move(req)));
            })};
    sw_->send(std::move(env));
    auto r = rx.recv();
    if (r.is_err()) return VoteReply{};
    return r.unwrap();
  }

  // @unsafe { mpsc bridge }
  TimeoutNowReply send_timeout_now(siteid_t dst, TimeoutNowReq req) override {
    auto [tx, rx] = rusty::sync::mpsc::channel<TimeoutNowReply>();
    Envelope env{self_, dst,
        rusty::Function<void(DispatcherProxy&)>(
            [req = std::move(req), tx = std::move(tx)](DispatcherProxy& disp) mutable {
              (void)tx.send(disp->handle_timeout_now(std::move(req)));
            })};
    sw_->send(std::move(env));
    auto r = rx.recv();
    if (r.is_err()) return TimeoutNowReply{};
    return r.unwrap();
  }

  // @unsafe { mpsc bridge }
  InstallSnapshotReply send_install_snapshot(siteid_t dst, InstallSnapshotReq req) override {
    auto [tx, rx] = rusty::sync::mpsc::channel<InstallSnapshotReply>();
    Envelope env{self_, dst,
        rusty::Function<void(DispatcherProxy&)>(
            [req = std::move(req), tx = std::move(tx)](DispatcherProxy& disp) mutable {
              (void)tx.send(disp->handle_install_snapshot(std::move(req)));
            })};
    sw_->send(std::move(env));
    auto r = rx.recv();
    if (r.is_err()) return InstallSnapshotReply{};
    return r.unwrap();
  }

  // ------------------------------------------------------------------
  // Fire-and-forget RPCs. Reply is discarded.
  // ------------------------------------------------------------------

  // @safe
  void send_vote_durable(siteid_t candidate, VoteDurableReq req) override {
    Envelope env{self_, candidate,
        rusty::Function<void(DispatcherProxy&)>(
            [req](DispatcherProxy& disp) mutable {
              (void)disp->handle_vote_durable(req);
            })};
    sw_->send(std::move(env));
  }

  // @safe
  void send_append_entries_durable(siteid_t leader, AppendEntriesDurableReq req) override {
    Envelope env{self_, leader,
        rusty::Function<void(DispatcherProxy&)>(
            [req](DispatcherProxy& disp) mutable {
              (void)disp->handle_append_entries_durable(req);
            })};
    sw_->send(std::move(env));
  }

  // @safe
  void send_notify_restart(siteid_t dst, parid_t /*par*/) override {
    NotifyRestartReq req{};
    req.restarted_site_id = self_;
    Envelope env{self_, dst,
        rusty::Function<void(DispatcherProxy&)>(
            [req](DispatcherProxy& disp) mutable {
              (void)disp->handle_notify_restart(req);
            })};
    sw_->send(std::move(env));
  }

 private:
  ChannelSwitchboard* sw_{nullptr};
  siteid_t            self_{0};
  parid_t             par_{0};
};

// @safe - factory produces a TransportProxy backed by the channel adapter.
inline TransportProxy make_channel_transport(ChannelSwitchboard* sw,
                                             siteid_t self,
                                             parid_t  par) {
  return rusty::make_box<ChannelTransportAdapter>(sw, self, par);
}

// ---------------------------------------------------------------------------
// ChannelNodeWorker — drains a receiver and invokes a DispatcherProxy.
// ---------------------------------------------------------------------------

class ChannelNodeWorker {
 public:
  // @safe
  ChannelNodeWorker(rusty::sync::mpsc::Receiver<Envelope> rx,
                    DispatcherProxy dispatcher)
      : rx_(std::move(rx)), dispatcher_(std::move(dispatcher)) {}

  // @unsafe { try_recv / Result unwrap on rusty boundary }
  bool step() {
    auto r = rx_.try_recv();
    if (r.is_err()) return false;
    auto env = r.unwrap();
    env.deliver(dispatcher_);
    return true;
  }

  // @unsafe { blocking recv; returns false on channel close }
  bool step_blocking() {
    auto r = rx_.recv();
    if (r.is_err()) return false;
    auto env = r.unwrap();
    env.deliver(dispatcher_);
    return true;
  }

  // @safe - drain everything currently buffered; returns count.
  size_t run_until_empty() {
    size_t n = 0;
    while (step()) ++n;
    return n;
  }

 private:
  rusty::sync::mpsc::Receiver<Envelope> rx_;
  DispatcherProxy                       dispatcher_;
};

}  // namespace raft
}  // namespace janus
