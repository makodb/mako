// rrr.fiber_channel — fiber-blocking wrapper over a
// `ChannelConnectionProxy` (formerly fiber_channel.hpp +
// fiber_channel.cpp).
//
// The channel layer's primitive is callback-driven: every backend
// (TcpConnection, in-memory channel, …) only has to implement
// `set_on_frame` / `set_on_closed` / `set_on_error`. That keeps
// backends simple but pushes callback-shaped code into every
// consumer. `FiberChannel` provides loop-shaped ergonomics on top:
//
//   FiberChannel fc(channel_proxy);
//   while (auto frame = fc.recv_frame()) {
//       handle(*frame);
//   }
//
// `recv_frame()` suspends the calling fiber until a frame arrives or
// the channel is closed. `send_frame(...)` forwards to the proxy
// (non-suspending — the proxy's outbound queue is internally
// thread-safe).
//
// Threading: must be constructed, destroyed, and `recv_frame()`-ed
// on the reactor (poll) thread. Only **one fiber** may call
// `recv_frame()` at a time; the underlying `IntEvent` is
// single-waiter. `send_frame(...)`, `close()`, and `is_closed()` are
// safe from any thread.
//
// Lifetime: the wrapper installs lambda callbacks on the proxy that
// capture `this`. The proxy is owned by the wrapper, so the lambdas
// can never outlive the wrapper. On destruction, the proxy is
// dropped, destroying the callback closures held by the underlying
// connection — so callbacks stop firing before any other state is
// torn down.
//
// Clang 22 quirk: importing `rrr.fiber_channel` into a TU that also
// `import std;`s makes clang ambiguate
// `operator new(size_t, std::align_val_t)` inside
// `std::__libcpp_allocate<std::shared_ptr<rusty::Waker>>`
// instantiations. The other rpc modules don't exhibit this; we
// found no in-module workaround. The workaround lives in
// `fiber_channel.hpp` (a 1-line `#include <memory>` shim) which
// `rrr/rrr.hpp` `#include`s *before* `import rrr.fiber_channel;`.
// That textual anchor pins libc++'s `operator new` in the global
// module ahead of the import, and the ambiguity disappears. Empirical
// — see docs/dev/srpc_module_migration_plan.md for the diagnostic.
module;

#include <cstddef>
#include <cstdint>

// `<rusty/rusty.hpp>` pre-instantiates libc++ container templates
// (e.g. `std::vector<uint8_t>::assign`, `std::deque<>::push_back`).
// Without this, clang 22 crashes CodeGen at `EmitScalarExpr` inside
// `EmitReturnStmt` when those instantiations are emitted from
// module-purview function bodies. Same trick as in other rpc
// modules.
#include <rusty/rusty.hpp>

export module rrr.fiber_channel;

import std;
import rrr.channel;
import rrr.reactor;
import rrr.threading;

// @safe - FiberChannel: fiber-blocking wrapper over a
// `ChannelConnectionProxy`. Bodies use SpinMutex<std::deque> for the
// inbound queue, `rusty::Cell<bool>` for the closed flag, and
// IntEvent for parking. Per-method `// @unsafe` overrides cover the
// ctor (which installs lambda callbacks through `ch_->set_on_*` —
// std::unique_ptr deref + rusty::Function ctor chain), the dtor
// (same set_on_* detach), `on_inbound_frame` (raw `const uint8_t*`
// byte arithmetic into the std::vector buffer), and `is_closed`
// (const_cast through the ChannelConnectionProxy).
export namespace rrr {

/**
 * Heap-owned copy of an inbound frame's payload. The wrapping is
 * necessary because the channel-layer `ChannelFrame::payload` is
 * only valid for the duration of the `on_frame` callback.
 */
struct OwnedFrame {
    std::vector<std::uint8_t> bytes;
};

// @safe - see file header.
class FiberChannel {
 public:
    explicit FiberChannel(ChannelConnectionProxy ch);
    ~FiberChannel();

    FiberChannel(const FiberChannel&)            = delete;
    FiberChannel& operator=(const FiberChannel&) = delete;
    FiberChannel(FiberChannel&&)                 = delete;
    FiberChannel& operator=(FiberChannel&&)      = delete;

    /**
     * Wire up `on_frame` / `on_closed` / `on_error` callbacks on the
     * owned `ChannelConnectionProxy`. Must be called exactly once,
     * immediately after construction (when the FiberChannel is in
     * its final memory location — typically inside a `rusty::Box`).
     * Split from the ctor so the lambda installation can live in a
     * named method body, matching the rusty-cpp DSL constraint that
     * `#[cpp_ctor]` bodies are pure field-init lists (no arbitrary
     * post-construction statements).
     */
    void bind_callbacks();

    /**
     * Suspend the calling fiber until a frame arrives or the channel
     * is closed.
     *
     *   - Returns `Some(frame)` when a frame is available.
     *   - Returns `None` after the channel has been closed *and* all
     *     queued frames have been delivered.
     */
    rusty::Option<OwnedFrame> recv_frame();

    /** Forward to the channel proxy. Non-suspending. Thread-safe. */
    ChannelError send_frame(const ChannelFrame& f);

    /**
     * Forward to the channel proxy. Idempotent. Thread-safe. The
     * actual `on_closed` callback fires later on the reactor thread,
     * waking any parked `recv_frame()`.
     */
    void close();

    bool is_closed() const;

    /** Underlying channel proxy access (non-owning). For tests. */
    ChannelConnectionProxy& channel_for_test() { return ch_; }

 private:
    void on_inbound_frame(const ChannelFrame& f);
    void on_inbound_closed(ChannelError reason);
    void on_inbound_error(ChannelError err, std::string_view msg);

    void signal_pending_recv();

    ChannelConnectionProxy ch_;

    // Inbound queue. Touched by the on_frame callback (poll thread)
    // and by recv_frame (also poll thread). Both paths run on the
    // same thread but we keep a SpinMutex as a defensive guard so
    // that any future cross-thread close paths stay safe.
    SpinMutex<std::deque<OwnedFrame>> queue_{std::deque<OwnedFrame>{}};

    // Single-shot event used to wake a parked recv_frame fiber. We
    // create a fresh `IntEvent` per wait (since the codebase's
    // `IntEvent` does not support re-arming after `DONE`).
    // `pending_recv_event_` is set when a recv fiber is parked and
    // cleared when it wakes.
    std::shared_ptr<IntEvent> pending_recv_event_;

    rusty::Cell<bool> closed_{false};
};

}  // export namespace rrr

// @safe - impl namespace. Out-of-class definitions inherit their
// per-method `// @safe` / `// @unsafe` from the matching declarations
// in the export namespace above.
namespace rrr {

// @safe - Pure field-init; lambda installation moved to bind_callbacks().
FiberChannel::FiberChannel(ChannelConnectionProxy ch)
    : ch_(std::move(ch)) {}

// @unsafe - `ch_->set_on_*` driven through std::unique_ptr deref +
// rusty::Function ctor chain on three captured `[this]` lambdas.
void FiberChannel::bind_callbacks() {
    // Install callbacks. Each fires on the reactor (poll) thread; we
    // forward to member methods that touch local state. Capturing
    // `this` is safe because callers always hold FiberChannel inside
    // a `rusty::Box`, so its address is pinned for the channel
    // proxy's lifetime (which the FiberChannel owns).
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

// @unsafe - `ch_->set_on_*({})` detach driven through std::unique_ptr deref.
FiberChannel::~FiberChannel() {
    // Detach callbacks before the proxy destructor runs to make sure
    // any in-flight callback dispatch can't race with member teardown.
    ch_->set_on_frame ({});
    ch_->set_on_closed({});
    ch_->set_on_error ({});
}

// @unsafe - `bytes.assign(f.payload, f.payload + f.size)` raw
// `const uint8_t*` arithmetic.
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
    // Fatal errors are followed by on_closed; non-fatal errors are
    // silently ignored at this layer.
}

void FiberChannel::signal_pending_recv() {
    auto event = pending_recv_event_;  // copy shared_ptr defensively
    if (event) {
        // @unsafe { IntEvent::set is not annotated @safe yet. }
        {
            event->set(1);
        }
    }
}

// @unsafe - std::unique_ptr deref through `ch_->send_frame(f)`.
ChannelError FiberChannel::send_frame(const ChannelFrame& f) {
    return ch_->send_frame(f);
}

// @unsafe - const_cast through the ChannelConnectionProxy reference
// + std::unique_ptr deref.
bool FiberChannel::is_closed() const {
    if (closed_.get()) return true;
    auto& mut_ch = const_cast<ChannelConnectionProxy&>(ch_);
    return mut_ch->is_closed();
}

// @unsafe - std::unique_ptr deref through `ch_->close()`.
void FiberChannel::close() {
    ch_->close();
}

rusty::Option<OwnedFrame> FiberChannel::recv_frame() {
    while (true) {
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

        auto event = Reactor::create_sp_event<IntEvent>();
        pending_recv_event_ = event;

        {
            auto guard = queue_.lock().unwrap();
            if (!guard->empty() || closed_.get()) {
                pending_recv_event_.reset();
                continue;
            }
        }

        // @unsafe { Event::wait is the fiber-suspending primitive,
        //           not annotated @safe yet. }
        {
            event->wait();
        }
        pending_recv_event_.reset();
    }
}

}  // namespace rrr
