module;

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>

export module rrr:rpc.fiber_channel;

import :base.threading;
import :rpc.channel;
import :reactor.event;
import :reactor.reactor;

/**
 * `FiberChannel` — fiber-blocking wrapper over a
 * `ChannelConnectionProxy` (Workstream K, sub-leaf 4c1).
 *
 * The channel layer's primitive is callback-driven: every backend
 * (TcpConnection, future in-memory channel, etc.) only has to
 * implement `set_on_frame` / `set_on_closed` / `set_on_error`. That
 * keeps backends simple but pushes callback-shaped code into every
 * consumer.
 *
 * `FiberChannel` provides the loop-shaped ergonomics on top:
 *
 *   FiberChannel fc(channel_proxy);
 *   while (auto frame = fc.recv_frame()) {
 *       handle(*frame);
 *   }
 *
 * `recv_frame()` suspends the calling fiber until a frame arrives or
 * the channel is closed. `send_frame(...)` forwards to the proxy
 * (non-suspending — the proxy's outbound queue is internally
 * thread-safe).
 *
 * ============================================================================
 * Threading
 * ============================================================================
 *
 * `FiberChannel` is single-threaded: it must be constructed,
 * destroyed, and `recv_frame()`-ed on the reactor (poll) thread —
 * the same thread that fires callbacks on the underlying
 * `ChannelConnectionProxy`. This matches the channel-layer contract.
 *
 * `send_frame(...)`, `close()`, `is_closed()` are safe to call from
 * any thread because they delegate to the channel proxy, whose
 * facade contract makes those methods thread-safe.
 *
 * Only **one fiber** may call `recv_frame()` at a time. Concurrent
 * waiters are not supported; the underlying `IntEvent` only
 * accommodates one waiter.
 *
 * ============================================================================
 * Lifetime
 * ============================================================================
 *
 * The wrapper installs lambda callbacks on the proxy that capture
 * `this`. The proxy is owned by the wrapper, so the lambdas can
 * never outlive the wrapper. On destruction, the proxy is dropped,
 * which destroys the callback closures held by the underlying
 * connection — so callbacks stop firing before any other state is
 * torn down.
 *
 * If the user closes the underlying connection externally (e.g.,
 * via another reference to the same channel proxy clone), the
 * `on_closed` callback fires on the reactor thread and sets the
 * `closed_` latch. A blocked `recv_frame()` is woken via the same
 * callback path.
 */
export namespace rrr {

/**
 * Heap-owned copy of an inbound frame's payload. The wrapping is
 * necessary because the channel-layer `ChannelFrame::payload` is
 * only valid for the duration of the `on_frame` callback.
 */
struct OwnedFrame {
    std::vector<std::uint8_t> bytes;
};

class FiberChannel {
 public:
    /**
     * Wrap `ch`. Installs `on_frame` / `on_closed` / `on_error`
     * callbacks on the proxy; previous callbacks (if any) are
     * overwritten.
     *
     * **Must be constructed on the reactor thread**, because it
     * allocates an `IntEvent` via `Reactor::create_sp_event` which
     * registers with the current reactor.
     */
    explicit FiberChannel(ChannelConnectionProxy ch);
    ~FiberChannel();

    FiberChannel(const FiberChannel&)            = delete;
    FiberChannel& operator=(const FiberChannel&) = delete;
    FiberChannel(FiberChannel&&)                 = delete;
    FiberChannel& operator=(FiberChannel&&)      = delete;

    /**
     * Suspend the calling fiber until a frame arrives or the
     * channel is closed.
     *
     *   - Returns `Some(frame)` when a frame is available.
     *   - Returns `None` after the channel has been closed *and*
     *     all queued frames have been delivered.
     *
     * Must be called from a fiber on the reactor thread (enforced
     * by `IntEvent::wait`).
     */
    rusty::Option<OwnedFrame> recv_frame();

    /**
     * Forward to the channel proxy. Non-suspending. Safe from any
     * thread per the channel-layer facade contract.
     */
    ChannelError send_frame(const ChannelFrame& f);

    /**
     * Forward to the channel proxy. Idempotent. Safe from any
     * thread. The actual `on_closed` callback fires later on the
     * reactor thread, which wakes any parked `recv_frame()`.
     */
    void close();

    /**
     * True if either:
     *   - the FiberChannel's local close latch has been set
     *     (`on_closed` callback fired on the reactor thread), OR
     *   - the underlying proxy reports closed.
     *
     * The two can disagree briefly: a caller on another thread may
     * `proxy.close()` before the reactor processes the resulting
     * `on_closed` callback. `is_closed()` returning the conjunction
     * means "from this point on, frame I/O is unsafe" — which is
     * the predicate request paths actually want.
     */
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

}  // namespace rrr
