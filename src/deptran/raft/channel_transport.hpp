#pragma once

/**
 * @file channel_transport.hpp
 * @brief In-process Raft transport built on rusty::sync::mpsc (Phase 4
 *        of the decouple plan). Enables raft tests to run without
 *        sockets, rrr, or PollThreads.
 *
 * Architecture:
 *   - ChannelSwitchboard owns one mpsc channel per site and a small
 *     fault-injection state machine (drop direction, partition two
 *     groups, reset).
 *   - ChannelTransportAdapter satisfies TransportFacade and pushes
 *     Envelopes onto the destination site's channel via the
 *     switchboard. Outbound RPCs are captured as opaque "deliver"
 *     closures that are invoked by the destination's worker thread.
 *   - ChannelNodeWorker pulls envelopes off its receiver and invokes
 *     each envelope's deliver closure against its local DispatcherProxy.
 *     Reply callbacks fire from the worker thread of whoever *received*
 *     the request; tests that need strict thread isolation should wrap
 *     the callback with their own posting mechanism.
 *
 * Rusty-safety:
 *  - No inheritance, no std::thread, no std::mutex.
 *  - `Envelope` is explicitly marked Send so it can cross mpsc.
 *  - Fault-injection state behind rusty::Mutex to keep reads lock-free
 *    on the hot path when no fault is configured.
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

#include "dispatcher.hpp"
#include "messages.hpp"
#include "transport.hpp"

#include "../constants.h"

namespace janus {
namespace raft {

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

// Carries one outbound RPC across the switchboard. `deliver` is a
// closure bound by the sender: when the receiving worker invokes it
// with its local dispatcher, the dispatcher is called with the request
// and the reply callback is fired.
struct Envelope {
  siteid_t from{0};
  siteid_t to{0};
  // @unsafe { rusty::Function is Send per rusty convention }
  rusty::Function<void(DispatcherProxy&)> deliver;
};

}  // namespace raft
}  // namespace janus

// Mark Envelope as Send so it may cross mpsc::channel boundaries.
namespace rusty {
template <>
struct is_send<janus::raft::Envelope> : std::true_type {};
}  // namespace rusty

namespace janus {
namespace raft {

// ---------------------------------------------------------------------------
// Fault-injection configuration
// ---------------------------------------------------------------------------

struct ChannelFaults {
  // Directed pairs whose outbound traffic should be silently dropped.
  std::vector<std::pair<siteid_t, siteid_t>> dropped;
  // If non-empty, each site in a partition can only talk to peers in
  // the same partition. One site belongs to one partition.
  std::vector<std::vector<siteid_t>> partitions;

  // @safe
  bool is_dropped(siteid_t from, siteid_t to) const {
    for (auto& p : dropped) {
      if (p.first == from && p.second == to) return true;
    }
    if (!partitions.empty()) {
      auto pf = find_partition(from);
      auto pt = find_partition(to);
      if (pf != pt) return true;
    }
    return false;
  }

 private:
  // @safe
  int find_partition(siteid_t s) const {
    for (size_t i = 0; i < partitions.size(); ++i) {
      for (auto x : partitions[i]) {
        if (x == s) return static_cast<int>(i);
      }
    }
    return -1;  // unknown site — treat as its own partition
  }
};

// ---------------------------------------------------------------------------
// ChannelSwitchboard
// ---------------------------------------------------------------------------

class ChannelSwitchboard {
 public:
  // @safe
  ChannelSwitchboard() = default;

  // @safe - registers a new site. Returns the receiver side that the
  // worker should drain. Must be called once per site before any
  // adapter sends to it.
  rusty::sync::mpsc::Receiver<Envelope> register_site(siteid_t s) {
    auto [tx, rx] = rusty::sync::mpsc::channel<Envelope>();
    senders_.push_back({s, std::move(tx)});
    return std::move(rx);
  }

  // @unsafe { pushes into mpsc; drops silently if the dest is gone }
  void send(Envelope env) {
    {
      // @unsafe { faults_ is read-only here; caller holds no lock
      //           because fault updates happen from the test thread
      //           while RPCs are paused }
      if (faults_.is_dropped(env.from, env.to)) return;
    }
    for (auto& pair : senders_) {
      if (pair.first == env.to) {
        // @unsafe { Sender::send returns Result; ignore errors for MVP }
        (void)pair.second.send(std::move(env));
        return;
      }
    }
    // no matching site — drop
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
// ChannelTransportAdapter — satisfies TransportFacade
// ---------------------------------------------------------------------------

class ChannelTransportAdapter {
 public:
  // @unsafe { non-owning switchboard pointer; switchboard outlives
  //           every adapter built on top of it }
  ChannelTransportAdapter(ChannelSwitchboard* sw, siteid_t self, parid_t par)
      : sw_(sw), self_(self), par_(par) {}

  // @safe
  siteid_t self_site_id() const { return self_; }

  // @safe - fire-and-forget envelope push
  void send_timeout_now(siteid_t dst, TimeoutNowReq req,
                        OnTimeoutNowReply on_reply) {
    // @unsafe { captures by move }
    auto deliver_fn = [req = std::move(req), on_reply = std::move(on_reply),
                       from = self_, dst](DispatcherProxy& disp) mutable {
      disp->handle_timeout_now(std::move(req),
          [on_reply = std::move(on_reply), dst](TimeoutNowReply r) mutable {
            on_reply(dst, std::move(r));
          });
    };
    Envelope env{self_, dst, rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // @safe
  void send_vote_durable(siteid_t candidate, VoteDurableReq req) {
    auto deliver_fn = [req = std::move(req)](DispatcherProxy& disp) mutable {
      disp->handle_vote_durable(std::move(req),
          [](VoteDurableReply) {});  // fire-and-forget
    };
    Envelope env{self_, candidate,
                 rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // @safe
  void send_append_entries_durable(siteid_t leader, AppendEntriesDurableReq req) {
    auto deliver_fn = [req = std::move(req)](DispatcherProxy& disp) mutable {
      disp->handle_append_entries_durable(std::move(req),
          [](AppendEntriesDurableReply) {});
    };
    Envelope env{self_, leader,
                 rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // @safe
  void send_notify_restart(siteid_t dst, parid_t /*par*/) {
    NotifyRestartReq req{};
    req.restarted_site_id = self_;
    auto deliver_fn = [req](DispatcherProxy& disp) mutable {
      disp->handle_notify_restart(req,
          [](NotifyRestartReply) {});
    };
    Envelope env{self_, dst,
                 rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // @safe
  void send_append_entries(siteid_t dst, AppendEntriesReq req,
                           OnAppendEntriesReply on_reply) {
    auto deliver_fn = [req = std::move(req), on_reply = std::move(on_reply),
                       dst](DispatcherProxy& disp) mutable {
      disp->handle_append_entries(std::move(req),
          [on_reply = std::move(on_reply), dst](AppendEntriesReply r) mutable {
            on_reply(dst, std::move(r));
          });
    };
    Envelope env{self_, dst,
                 rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // @safe
  void send_empty_append_entries(siteid_t dst, EmptyAppendEntriesReq req,
                                 OnAppendEntriesReply on_reply) {
    auto deliver_fn = [req = std::move(req), on_reply = std::move(on_reply),
                       dst](DispatcherProxy& disp) mutable {
      disp->handle_empty_append_entries(std::move(req),
          [on_reply = std::move(on_reply), dst](EmptyAppendEntriesReply r) mutable {
            AppendEntriesReply rr{};
            rr.follower_append_ok = r.follower_append_ok;
            rr.follower_current_term = r.follower_current_term;
            rr.follower_last_log_index = r.follower_last_log_index;
            rr.follower_ack_type = r.follower_ack_type;
            on_reply(dst, std::move(rr));
          });
    };
    Envelope env{self_, dst,
                 rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // @safe - broadcasts one envelope per peer configured via set_peers().
  void broadcast_vote(parid_t /*par*/, VoteReq req, OnVoteReply on_reply) {
    // Share on_reply across peers via a std::shared_ptr holder: rusty::Arc
    // yields const deref, but OnVoteReply::operator() is non-const.
    // @unsafe { std::shared_ptr at adapter boundary }
    struct Holder { OnVoteReply cb; };
    auto holder = std::make_shared<Holder>(Holder{std::move(on_reply)});
    for (auto peer : peers_) {
      if (peer == self_) continue;
      auto holder_ref = holder;
      auto deliver_fn = [req, holder_ref, peer](DispatcherProxy& disp) mutable {
        disp->handle_vote(req,
            [holder_ref, peer](VoteReply r) mutable {
              holder_ref->cb(peer, std::move(r));
            });
      };
      Envelope env{self_, peer,
                   rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
      sw_->send(std::move(env));
    }
  }

  // @safe
  void send_install_snapshot(siteid_t dst, InstallSnapshotReq req,
                             OnInstallSnapshotReply on_reply) {
    auto deliver_fn = [req = std::move(req), on_reply = std::move(on_reply),
                       dst](DispatcherProxy& disp) mutable {
      disp->handle_install_snapshot(std::move(req),
          [on_reply = std::move(on_reply), dst](InstallSnapshotReply r) mutable {
            on_reply(dst, std::move(r));
          });
    };
    Envelope env{self_, dst,
                 rusty::Function<void(DispatcherProxy&)>(std::move(deliver_fn))};
    sw_->send(std::move(env));
  }

  // Configure peer list for broadcast_vote. Must be called before any
  // vote broadcast runs.
  // @safe
  void set_peers(std::vector<siteid_t> peers) { peers_ = std::move(peers); }

 private:
  ChannelSwitchboard* sw_{nullptr};
  siteid_t             self_{0};
  parid_t              par_{0};
  std::vector<siteid_t> peers_{};
};

// @safe - factory produces a TransportProxy backed by the channel adapter.
inline TransportProxy make_channel_transport(ChannelSwitchboard* sw,
                                             siteid_t self,
                                             parid_t  par,
                                             std::vector<siteid_t> peers) {
  ChannelTransportAdapter a{sw, self, par};
  a.set_peers(std::move(peers));
  return pro::make_proxy<TransportFacade, ChannelTransportAdapter>(std::move(a));
}

// ---------------------------------------------------------------------------
// ChannelNodeWorker — drains a receiver and invokes a DispatcherProxy.
// Not a thread by itself; callers can spin a rusty::thread around
// ::run_until_empty() or ::run_forever(). Tests that run a single
// step at a time can call ::step().
// ---------------------------------------------------------------------------

class ChannelNodeWorker {
 public:
  // @safe
  ChannelNodeWorker(rusty::sync::mpsc::Receiver<Envelope> rx,
                    DispatcherProxy dispatcher)
      : rx_(std::move(rx)), dispatcher_(std::move(dispatcher)) {}

  // @unsafe { try_recv / Result unwrap on rusty boundary }
  // Returns true if one envelope was drained.
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
