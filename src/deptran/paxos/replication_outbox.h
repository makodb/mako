#pragma once

// Per-destination ordered outbox for must-deliver replication requests.
//
// Header-only and free of Config: MultiPaxosCommo (commo.cc) keeps one outbox
// per peer site, and tests/test_replication_admission.cc drives the same code
// over a real loopback connection by injecting its own admit function.

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/refcell.hpp>
#include <rusty/vecdeque.hpp>

#include "../__dep__.h"
#include "../constants.h"

namespace janus {

using ReplicationWrite = rusty::Function<void(srpc::BinaryWriteArchive&)>;

// One replication request waiting for its destination's channel.
//
// The payload is serialized when the pump admits the request, which can be
// long after Send returned. Nothing `write` reads may therefore change after
// Send: capture values, or shared payloads (a janus::Command's Arc) that no
// one mutates once submitted. Every current caller passes an Accept, Decide
// or learner Forward of an already submitted log entry.
struct ReplicationMessage {
  i32 rpc_id = 0;
  // Completes exactly once once the request was admitted: its reply, or the
  // error of a connection closed under it.
  srpc::FutureAttr attr;
  ReplicationWrite write;
  // Runs instead of attr's callback when admission refuses the request
  // before a future exists (no connection, open circuit).
  rusty::Function<void(i32)> refused;
};

// What the watcher reports while one admission keeps waiting.
struct ReplicationStall {
  siteid_t site_id;
  i32 rpc_id;
  size_t queued;       // requests queued behind the waiting one
  uint64_t waited_us;  // since this request's admission began
};

// Admits one request on the destination's connection, waiting up to
// admission_timeout_us for channel space, with the contract of
// srpc::ClientConnection::request_with_admission (an Err is a refusal before
// any future exists; an Ok future completes exactly once). Production binds
// the peer's current connection (commo.cc request_replication).
using ReplicationAdmit = rusty::Function<srpc::FutureResult(
    i32 rpc_id, const srpc::FutureAttr& attr, uint64_t admission_timeout_us,
    ReplicationWrite& write) const>;
using ReplicationStallReport =
    rusty::Function<void(const ReplicationStall&) const>;

// @unsafe - std::format logging of one stall report.
inline void LogReplicationStall(const ReplicationStall& stall) {
  Log_warn("replication to site {} stalled: rpc {} has waited {:.1f} s for "
           "channel space, {} more queued behind it",
           stall.site_id, stall.rpc_id,
           static_cast<double>(stall.waited_us) / 1e6, stall.queued);
}

// A destination's requests, in the order the protocol issued them, and the
// destination's only admission waiter, one pump fiber.
//
// - Send never suspends its caller. Accept, Decide and Forward are
//   must-deliver: replicas apply only a committed prefix, a Decide carries no
//   value (it commits the accepted one) and there is no catch-up path, so a
//   request dropped on a congested but open channel stalls its destination
//   forever. The pump therefore admits with no deadline: congestion delays a
//   request and never fails it. Closing the connection still completes the
//   waiting request with an error, and the requests behind it are refused.
// - Strict FIFO per destination: on each connection an Accept precedes its
//   Decide and learner forwards keep commit order. One congested destination
//   (the leader's own loopback included) delays only itself.
// - O(destinations) waiters instead of O(messages): only the pump sleeps in
//   admission. While it waits, one watcher fiber per destination reports a
//   wait that reaches stall_report_after_us (10 s), and again every
//   stall_report_after_us after that, checking every tenth of it; it
//   replaces the "failed: 11" lines the old 30 s admission deadline
//   produced, without dropping anything.
// - Unbounded: the outbox grows without limit if a live peer stops reading.
//   It replaces an equally unbounded pile of suspended per-message fibers,
//   each of which held a stack.
// - Owner thread only. Nothing here is synchronized; Send verify()s that it
//   runs on the owner poll thread recorded at construction.
class ReplicationOutbox {
 public:
  static constexpr uint64_t kNoAdmissionDeadline = UINT64_MAX;
  static constexpr uint64_t kStallReportAfterUs = 10'000'000;

  // @safe - Takes ownership of the admit function and the report hook.
  ReplicationOutbox(siteid_t site_id, rusty::Arc<srpc::PollThread> owner,
                    ReplicationAdmit admit,
                    uint64_t stall_report_after_us = kStallReportAfterUs,
                    ReplicationStallReport report = ReplicationStallReport{})
      : state_(rusty::Arc<State>::make(site_id, std::move(owner),
                                       std::move(admit), stall_report_after_us,
                                       std::move(report))) {}

  // @safe - Another handle to the same outbox.
  ReplicationOutbox clone() const { return ReplicationOutbox(state_.clone()); }

  // @unsafe - Owner thread only (verified). Appends to the outbox and starts
  // the pump if it is idle; never suspends the caller. The pump runs at once
  // until the channel is full or the outbox is empty.
  void Send(ReplicationMessage message) const {
    const State& state = *state_;
    verify(state.owner->is_current_thread());
    state.queue.borrow_mut()->push_back(std::move(message));
    if (state.pumping.get())
      return;
    state.pumping.set(true);
    state.pump_starts.set(state.pump_starts.get() + 1);
    auto pump_state = state_.clone();
    srpc::Fiber::create_run([pump_state] { Pump(pump_state); });
    // Still pumping: the pump suspended for channel space. Start the
    // destination's single watcher unless one is still running.
    if (state.pumping.get() && !state.watching.get()) {
      state.watching.set(true);
      state.watcher_starts.set(state.watcher_starts.get() + 1);
      auto watch_state = state_.clone();
      srpc::Fiber::create_run([watch_state] { Watch(watch_state); });
    }
  }

  // Observers, owner thread only like everything else.
  // @safe - Requests not yet taken by the pump.
  size_t queued() const { return state_->queue.borrow()->len(); }
  // @safe - A pump fiber exists for this destination.
  bool pumping() const { return state_->pumping.get(); }
  // @safe - A watcher fiber exists for this destination.
  bool watching() const { return state_->watching.get(); }
  // @safe - Number of stall reports made so far.
  uint64_t stall_reports() const { return state_->stall_reports.get(); }
  // @safe - Pump fibers started so far: one per busy period, not per message.
  uint64_t pump_starts() const { return state_->pump_starts.get(); }
  // @safe - Watcher fibers started so far.
  uint64_t watcher_starts() const { return state_->watcher_starts.get(); }
  siteid_t site_id() const { return state_->site_id; }

 private:
  struct State {
    // @safe - Aggregate construction for Arc::make.
    State(siteid_t site, rusty::Arc<srpc::PollThread> owner_thread,
          ReplicationAdmit admit_fn, uint64_t report_after_us,
          ReplicationStallReport report_fn)
        : site_id(site),
          owner(std::move(owner_thread)),
          admit(std::move(admit_fn)),
          stall_report_after_us(report_after_us),
          report(std::move(report_fn)) {}

    const siteid_t site_id;
    const rusty::Arc<srpc::PollThread> owner;
    const ReplicationAdmit admit;
    const uint64_t stall_report_after_us;
    const ReplicationStallReport report;
    rusty::RefCell<rusty::VecDeque<ReplicationMessage>> queue;
    rusty::Cell<bool> pumping{false};
    rusty::Cell<bool> watching{false};
    // The admission in progress; admitting_since_us is 0 between admissions.
    rusty::Cell<uint64_t> admitting_since_us{0};
    rusty::Cell<i32> admitting_rpc_id{0};
    rusty::Cell<uint64_t> admitting_reports{0};
    rusty::Cell<uint64_t> stall_reports{0};
    rusty::Cell<uint64_t> pump_starts{0};
    rusty::Cell<uint64_t> watcher_starts{0};
  };

  // @safe - Wraps an existing shared state.
  explicit ReplicationOutbox(rusty::Arc<State> state)
      : state_(std::move(state)) {}

  // @unsafe - The destination's only admission waiter. Admits the outbox in
  // FIFO order; no RefMut is held across a suspension.
  static void Pump(const rusty::Arc<State>& state) {
    for (;;) {
      auto next = state->queue.borrow_mut()->pop_front();
      if (next.is_none())
        break;
      auto message = next.unwrap();
      state->admitting_rpc_id.set(message.rpc_id);
      state->admitting_reports.set(0);
      state->admitting_since_us.set(srpc::Time::now(true));
      auto result = state->admit(message.rpc_id, message.attr,
                                 kNoAdmissionDeadline, message.write);
      state->admitting_since_us.set(0);
      if (result.is_ok()) {
        srpc::Future::safe_release(result.unwrap());
      } else if (message.refused) {
        message.refused(result.unwrap_err());
      }
    }
    state->pumping.set(false);
  }

  // @unsafe - Reports a long admission wait; exits once the pump is idle.
  // Started only when a pump suspended, so at most one per destination and
  // never one per message. Captures the shared state, never the communicator.
  static void Watch(const rusty::Arc<State>& state) {
    const uint64_t threshold = state->stall_report_after_us;
    const uint64_t tick = threshold / 10 > 1000 ? threshold / 10 : 1000;
    for (;;) {
      srpc::Fiber::sleep(tick);
      if (!state->pumping.get())
        break;
      const uint64_t since = state->admitting_since_us.get();
      const uint64_t now = srpc::Time::now(true);
      if (since == 0 || now <= since)
        continue;
      const uint64_t waited = now - since;
      if (waited < (state->admitting_reports.get() + 1) * threshold)
        continue;
      // A late tick reports once, not once per missed threshold.
      state->admitting_reports.set(waited / threshold);
      state->stall_reports.set(state->stall_reports.get() + 1);
      const ReplicationStall stall{state->site_id,
                                   state->admitting_rpc_id.get(),
                                   state->queue.borrow()->len(), waited};
      if (state->report)
        state->report(stall);
      else
        LogReplicationStall(stall);
    }
    state->watching.set(false);
  }

  rusty::Arc<State> state_;
};

}  // namespace janus
