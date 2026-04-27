module;

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>

module rrr:impl.rpc.fiber_channel;

import rrr;

namespace rrr {

FiberChannel::FiberChannel(ChannelConnectionProxy ch)
    : ch_(std::move(ch)) {

    // Install callbacks. Each fires on the reactor (poll) thread;
    // we forward to member methods that touch local state.
    ch_->set_on_frame([this](const ChannelFrame& f) {
        on_inbound_frame(f);
    });
    ch_->set_on_closed([this](ChannelError reason) {
        on_inbound_closed(reason);
    });
    ch_->set_on_error([this](ChannelError err, std::string_view msg) {
        on_inbound_error(err, msg);
    });
}

FiberChannel::~FiberChannel() {
    // Detach callbacks before the proxy destructor runs to make
    // sure any in-flight callback dispatch can't race with member
    // teardown. `set_on_*(nullptr)` is harmless if the underlying
    // type never re-checks.
    ch_->set_on_frame ({});
    ch_->set_on_closed({});
    ch_->set_on_error ({});
    // The proxy is dropped here; underlying connection holds no
    // further reference to `this`.
}

void FiberChannel::on_inbound_frame(const ChannelFrame& f) {
    OwnedFrame copy;
    copy.bytes.assign(f.payload, f.payload + f.size);
    {
        auto guard = queue_.lock().unwrap();
        guard->push_back(std::move(copy));
    }
    signal_pending_recv();
}

void FiberChannel::on_inbound_closed(ChannelError /*reason*/) {
    closed_.set(true);
    signal_pending_recv();
}

void FiberChannel::on_inbound_error(ChannelError /*err*/,
                                    std::string_view /*msg*/) {
    // The channel-layer contract follows fatal errors with
    // `on_closed`, so the fiber wakeup happens there. Non-fatal
    // errors (rare) are silently ignored at this layer; the upper
    // RPC layer can install its own on_error hook on the proxy if
    // it cares. For sub-leaf 4c1 the FiberChannel owns the on_error
    // slot, but we don't surface it through `recv_frame()` —
    // sub-leaf 4d will revisit if needed.
}

void FiberChannel::signal_pending_recv() {
    auto event = pending_recv_event_;  // copy shared_ptr defensively
    if (event) {
        // `IntEvent::set` is fine to call multiple times; only the
        // first transition to ready wakes the fiber. Subsequent
        // sets are no-ops. We pass `1` to satisfy the default
        // `target_=1`.
        event->set(1);
    }
}

ChannelError FiberChannel::send_frame(const ChannelFrame& f) {
    return ch_->send_frame(f);
}

bool FiberChannel::is_closed() const {
    if (closed_.get()) return true;
    // Consult the proxy as well: a caller may have closed it from
    // another thread before the reactor processed `on_closed`. The
    // const_cast matches how `send_frame` reaches a non-const facade
    // method through interior-mutable backends.
    auto& mut_ch = const_cast<ChannelConnectionProxy&>(ch_);
    return mut_ch->is_closed();
}

void FiberChannel::close() {
    ch_->close();
    // The on_closed callback fires later on the reactor thread.
}

rusty::Option<OwnedFrame> FiberChannel::recv_frame() {
    while (true) {
        // Try the queue first. If a frame is available, return it —
        // even if `closed_` is set, drain queued frames before
        // returning None so no inbound data is lost.
        {
            auto guard = queue_.lock().unwrap();
            if (!guard->empty()) {
                OwnedFrame f = std::move(guard->front());
                guard->pop_front();
                return rusty::Some(std::move(f));
            }
        }

        if (closed_.get()) {
            return rusty::None;
        }

        // Empty + open: park the fiber. Allocate a fresh IntEvent
        // (the codebase's IntEvent doesn't support re-arming once
        // `DONE`). The callback path uses `pending_recv_event_` to
        // signal it.
        auto event = Reactor::create_sp_event<IntEvent>();
        pending_recv_event_ = event;

        // Race: if a frame arrived between the queue-empty check
        // above and this point, the callback's `signal_pending_recv`
        // already saw the previous (null or stale) event. Solve by
        // re-checking the queue before suspending.
        {
            auto guard = queue_.lock().unwrap();
            if (!guard->empty() || closed_.get()) {
                pending_recv_event_.reset();
                // Loop back; the next iteration will see the queue.
                continue;
            }
        }

        event->wait();  // suspend fiber

        pending_recv_event_.reset();
        // Loop and re-check.
    }
}

}  // namespace rrr
