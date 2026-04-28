#pragma once

// Workstream K, leaf 6a — in-memory `ChannelConnection` /
// `ChannelListener` / `ChannelFactory` backend for deterministic
// SRPC tests.
//
// =====================================================================
// Design
// =====================================================================
//
// The in-memory backend mirrors the TCP backend's facade contract
// (`channel.hpp`) but skips the poll thread and the kernel network
// stack entirely. Everything happens in-process, on the calling
// thread (or whichever thread the user invokes the API from).
//
// **Switchboard** — a lookup registry that maps bind addresses to
// listening `InMemoryListener`s. `InMemoryFactory::connect(addr)`
// queries the switchboard, finds the matching listener, builds a
// paired `(client_side, server_side)` connection, fires the
// listener's `on_accept` callback with `server_side`, and returns
// `client_side` as the `ConnectResult.connection`.
//
// **InMemoryChannel pair** — two `InMemoryChannel` instances that
// share an `InMemoryConnectionState` (heap-allocated, refcounted).
// The state holds:
//   - both halves' `on_frame` / `on_closed` / `on_error` callbacks,
//   - a `closed_` latch (one per side),
//   - the peer addresses (for `peer_address()`).
// `send_frame` from side A invokes side B's `on_frame` callback
// inline, with a non-owning view onto the bytes (the channel-layer
// contract states the payload pointer is only valid for the
// duration of the callback). Bytes are also copied into a temporary
// buffer for safe handoff in case the receiver yields or stashes
// the pointer briefly.
//
// **Threading** — for 6a, all callbacks fire synchronously on the
// caller's thread. This is sufficient for unit/integration tests
// where the test thread drives both sides. Future leaves may add a
// "poll thread mode" that defers callbacks to a real `PollThread`
// to mirror TCP-backend timing behavior more closely.
//
// =====================================================================
// Usage
// =====================================================================
//
//   auto switchboard = rusty::Arc<InMemorySwitchboard>::make();
//   auto factory_arc = rusty::Arc<InMemoryFactory>::make(switchboard);
//   ChannelFactoryProxy factory = make_inmemory_factory_proxy(factory_arc);
//
//   // Server side: create a listener, register an accept handler.
//   auto listener = (*factory)->make_listener();
//   listener->set_on_accept([&](ChannelConnectionProxy peer) {
//       // attach server-side handlers, e.g. on_frame, etc.
//   });
//   listener->listen("inmemory://server-A");
//
//   // Client side: connect.
//   auto result = (*factory)->connect("inmemory://server-A");
//   ASSERT_EQ(result.error, ChannelError::None);
//   auto client_conn = std::move(result.connection);
//
//   // Bidirectional frame exchange — synchronous.
//   client_conn->set_on_frame([&](const ChannelFrame& f) {
//       // The server's reply lands here.
//   });
//   ChannelFrame f{bytes, size};
//   client_conn->send_frame(f);  // → fires server-side on_frame inline.

#include <std_compat.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/hashmap.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/weak.hpp>

#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RRR_INMEMORY_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RRR_INMEMORY_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RRR_INMEMORY_RESTORE_RR_MACRO
#endif

#include "channel.hpp"

namespace rrr {

class InMemoryListener;

// ---------------------------------------------------------------------------
// Switchboard
// ---------------------------------------------------------------------------

/**
 * Registry of bind-address → listener mappings.
 *
 * Tests typically construct a fresh switchboard per test (so address
 * collisions across tests don't cross-pollinate). The factory holds
 * an `Arc<InMemorySwitchboard>`; each listener registers itself in
 * the switchboard on `listen(addr)` and unregisters on `close()`.
 *
 * Thread-safe: an internal `std::mutex` guards the map. Most tests
 * are single-threaded but the locking lets a test fire `on_frame` on
 * one thread while another connects.
 */
class InMemorySwitchboard {
 public:
    InMemorySwitchboard()  = default;
    ~InMemorySwitchboard() = default;

    InMemorySwitchboard(const InMemorySwitchboard&) = delete;
    InMemorySwitchboard& operator=(const InMemorySwitchboard&) = delete;

    /**
     * Register a listener under `addr`. Returns true on success;
     * returns false if `addr` is already taken (the caller should
     * pick another address or close the existing listener).
     *
     * `const` so callers holding `Arc<InMemorySwitchboard>` (which
     * dereferences to `const InMemorySwitchboard*`) can register
     * directly. Internal state (`mu_` / `listeners_`) is `mutable`
     * — the standard pattern for thread-safe shared registries.
     */
    bool register_listener(std::string addr,
                           rusty::sync::Weak<InMemoryListener> listener) const;

    /**
     * Unregister a listener bound to `addr`. No-op if not registered.
     */
    void unregister_listener(const std::string& addr) const;

    /**
     * Look up the listener registered for `addr`, or rusty::None if
     * no listener is bound to that address.
     */
    rusty::Option<rusty::Arc<InMemoryListener>> find_listener(
        const std::string& addr) const;

 private:
    mutable std::mutex mu_;
    mutable rusty::HashMap<std::string,
                           rusty::sync::Weak<InMemoryListener>> listeners_;
};

// ---------------------------------------------------------------------------
// InMemoryConnectionState (shared between paired channels)
// ---------------------------------------------------------------------------

/**
 * Heap-allocated state shared between the two halves of an
 * `InMemoryChannel` pair. Each `InMemoryChannel` holds an
 * `Arc<InMemoryConnectionState>` and an "is_a_side" flag so the
 * channel knows which half of the pair it is.
 *
 * The state stores the per-side callbacks and a `closed_a` /
 * `closed_b` latch pair. The connection is logically closed (both
 * sides observe `is_closed() == true`) as soon as either side has
 * called `close()`.
 */
struct InMemoryConnectionState {
    // A-side state
    std::string       a_peer_address;
    OnFrameCallback   a_on_frame;
    OnClosedCallback  a_on_closed;
    OnErrorCallback   a_on_error;
    bool              a_closed = false;

    // B-side state
    std::string       b_peer_address;
    OnFrameCallback   b_on_frame;
    OnClosedCallback  b_on_closed;
    OnErrorCallback   b_on_error;
    bool              b_closed = false;

    // 6c — Fault injection state. Per-side knobs decide whether
    // each upcoming `send_frame` should:
    //   * be silently dropped (returns `ChannelError::None`, peer
    //     gets nothing), OR
    //   * return a specific `ChannelError`, OR
    //   * proceed normally.
    //
    // Counters tick down on each `send_frame` call from the
    // corresponding side; when zero, the channel returns to normal
    // delivery. Drop counters take precedence — when both are set
    // the drop fires first while its counter is positive, then the
    // error injection takes over.
    int               drop_next_sends_a  = 0;
    int               drop_next_sends_b  = 0;
    int               send_error_count_a = 0;
    int               send_error_count_b = 0;
    ChannelError      send_error_a       = ChannelError::None;
    ChannelError      send_error_b       = ChannelError::None;

    std::mutex        mu;
};

// ---------------------------------------------------------------------------
// InMemoryChannel
// ---------------------------------------------------------------------------

/**
 * One half of a paired in-memory connection. Implements the
 * `ChannelConnectionFacade` contract.
 *
 * `is_a_side` selects which set of callbacks/state this half talks
 * to (its own) and which set it dispatches into when the peer's
 * `send_frame` fires (the other side's). The state object is shared.
 */
class InMemoryChannel {
 public:
    InMemoryChannel(rusty::Arc<InMemoryConnectionState> state, bool is_a_side)
        : state_(std::move(state)), is_a_side_(is_a_side) {}

    ~InMemoryChannel() = default;

    InMemoryChannel(const InMemoryChannel&)            = delete;
    InMemoryChannel& operator=(const InMemoryChannel&) = delete;
    // Non-movable — InMemoryConnectionState's callbacks may capture
    // pointers into this object. Ownership is via Arc.
    InMemoryChannel(InMemoryChannel&&)                 = delete;
    InMemoryChannel& operator=(InMemoryChannel&&)      = delete;

    // ChannelConnectionFacade methods.
    ChannelError send_frame(const ChannelFrame& f);
    void         flush()              {}
    void         close();
    bool         is_closed() const;
    std::string  peer_address() const;

    void set_on_frame (OnFrameCallback  cb);
    void set_on_closed(OnClosedCallback cb);
    void set_on_error (OnErrorCallback  cb);

    // ---- 6c: fault injection (test-only). ------------------------
    /**
     * Drop the next `count` calls to `send_frame` on this side.
     * Each dropped call returns `ChannelError::None` (so the sender
     * doesn't notice the drop) but the bytes never reach the peer's
     * `on_frame`. After the counter hits zero, `send_frame`
     * resumes normal delivery.
     *
     * Setting `count` to 0 explicitly clears the drop counter.
     * Calling this multiple times overwrites the previous value
     * (it does NOT add).
     *
     * Drop counters take precedence over error counters: if both
     * are set, drops fire first while the drop counter is positive.
     */
    void inject_drop_next_sends(int count);

    /**
     * Make the next `count` calls to `send_frame` on this side
     * return `err` instead of delivering. After the counter hits
     * zero, `send_frame` resumes normal delivery.
     *
     * Use `count == 0` to clear (the value of `err` is ignored).
     * Calling this multiple times overwrites the previous value
     * (it does NOT add).
     */
    void inject_send_error(ChannelError err, int count);

    /**
     * Reset all fault-injection state on this side (drop counter,
     * error counter, error code).
     */
    void clear_fault_injection();

 private:
    // The state is held by Arc; both halves of the pair share it.
    // Arc::operator-> returns a const-pointer, so all mutation goes
    // through `mut_state()` which const_casts to a mutable reference.
    InMemoryConnectionState& mut_state() const {
        return const_cast<InMemoryConnectionState&>(*state_.get());
    }

    rusty::Arc<InMemoryConnectionState> state_;
    bool is_a_side_;
};

// Adapter wrapping `Arc<InMemoryChannel>` for the channel-proxy
// facade. Mirrors `TcpConnectionChannelAdapter`.
class InMemoryChannelAdapter {
 public:
    explicit InMemoryChannelAdapter(rusty::Arc<InMemoryChannel> conn)
        : conn_(std::move(conn)) {}

    ChannelError send_frame(const ChannelFrame& f) { return mut_conn().send_frame(f); }
    void         flush()              { mut_conn().flush(); }
    void         close()              { mut_conn().close(); }
    bool         is_closed() const    { return conn_->is_closed(); }
    std::string  peer_address() const { return conn_->peer_address(); }
    void         set_on_frame (OnFrameCallback  cb) { mut_conn().set_on_frame (std::move(cb)); }
    void         set_on_closed(OnClosedCallback cb) { mut_conn().set_on_closed(std::move(cb)); }
    void         set_on_error (OnErrorCallback  cb) { mut_conn().set_on_error (std::move(cb)); }

 private:
    InMemoryChannel& mut_conn() {
        return const_cast<InMemoryChannel&>(*conn_.get());
    }
    rusty::Arc<InMemoryChannel> conn_;
};

inline ChannelConnectionProxy make_inmemory_channel_proxy(
        rusty::Arc<InMemoryChannel> conn) {
    return pro::make_proxy<ChannelConnectionFacade,
                           InMemoryChannelAdapter>(std::move(conn));
}

// ---------------------------------------------------------------------------
// InMemoryListener
// ---------------------------------------------------------------------------

/**
 * In-memory accept-side listener. Implements the
 * `ChannelListenerFacade` contract.
 *
 * On `listen(addr)`, the listener registers itself in the
 * switchboard. On `close()`, it unregisters and refuses further
 * accepts. Existing accepted connections are unaffected.
 */
class InMemoryListener {
 public:
    explicit InMemoryListener(rusty::Arc<InMemorySwitchboard> switchboard)
        : switchboard_(std::move(switchboard)) {}

    ~InMemoryListener() = default;

    InMemoryListener(const InMemoryListener&)            = delete;
    InMemoryListener& operator=(const InMemoryListener&) = delete;
    InMemoryListener(InMemoryListener&&)                 = delete;
    InMemoryListener& operator=(InMemoryListener&&)      = delete;

    // ChannelListenerFacade methods.
    ChannelError listen(std::string_view addr);
    void         close();
    bool         is_closed() const;
    std::string  local_address() const;
    void set_on_accept(OnAcceptCallback cb);
    void set_on_error (OnErrorCallback  cb);

    // Internal — invoked by InMemoryFactory::connect(). Builds a
    // paired connection state, fires `on_accept` with the server
    // half, and returns the client half. Returns nullptr-equivalent
    // (default-constructed proxy) if `closed_`.
    rusty::Option<rusty::Arc<InMemoryChannel>> accept_for_connect(
        const std::string& client_address);

    /// Test-only: set the self-weak so the switchboard can upgrade
    /// to an Arc when looking up listeners.
    void set_self_weak(rusty::sync::Weak<InMemoryListener> w) {
        self_weak_ = std::move(w);
    }

 private:
    rusty::Arc<InMemorySwitchboard> switchboard_;
    rusty::Option<rusty::sync::Weak<InMemoryListener>> self_weak_{rusty::None};

    mutable std::mutex mu_;
    std::string        local_address_;
    bool               closed_ = false;
    OnAcceptCallback   on_accept_;
    OnErrorCallback    on_error_;
};

// Adapter wrapping `Arc<InMemoryListener>` for the listener-proxy
// facade. Mirrors `TcpListenerChannelAdapter` (equivalent in spirit).
class InMemoryListenerAdapter {
 public:
    explicit InMemoryListenerAdapter(rusty::Arc<InMemoryListener> listener)
        : listener_(std::move(listener)) {}

    ChannelError listen(std::string_view addr) { return mut_listener().listen(addr); }
    void         close()              { mut_listener().close(); }
    bool         is_closed() const    { return listener_->is_closed(); }
    std::string  local_address() const { return listener_->local_address(); }
    void set_on_accept(OnAcceptCallback cb) { mut_listener().set_on_accept(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) { mut_listener().set_on_error (std::move(cb)); }

 private:
    InMemoryListener& mut_listener() {
        return const_cast<InMemoryListener&>(*listener_.get());
    }
    rusty::Arc<InMemoryListener> listener_;
};

inline ChannelListenerProxy make_inmemory_listener_proxy(
        rusty::Arc<InMemoryListener> listener) {
    return pro::make_proxy<ChannelListenerFacade,
                           InMemoryListenerAdapter>(std::move(listener));
}

// ---------------------------------------------------------------------------
// InMemoryFactory
// ---------------------------------------------------------------------------

/**
 * In-memory factory implementing `ChannelFactoryFacade`.
 *
 * `connect(addr)` looks up the listener registered for `addr` in the
 * switchboard, builds a paired connection, fires the listener's
 * `on_accept` callback with one half, and returns the other half as
 * the `ConnectResult.connection`. Returns
 * `ChannelError::ConnectionRefused` if no listener is bound to the
 * address, mirroring the TCP backend's semantics.
 *
 * `make_listener()` constructs a fresh `InMemoryListener` wired to
 * the switchboard. The listener self-registers when `listen(addr)`
 * is called.
 */
class InMemoryFactory {
 public:
    explicit InMemoryFactory(rusty::Arc<InMemorySwitchboard> switchboard)
        : switchboard_(std::move(switchboard)) {}

    ~InMemoryFactory() = default;

    InMemoryFactory(const InMemoryFactory&)            = delete;
    InMemoryFactory& operator=(const InMemoryFactory&) = delete;
    InMemoryFactory(InMemoryFactory&&)                 = delete;
    InMemoryFactory& operator=(InMemoryFactory&&)      = delete;

    // ChannelFactoryFacade methods.
    ConnectResult        connect(std::string_view addr);
    ChannelListenerProxy make_listener();
    const char*          backend_name() const { return "inmemory"; }

    // Switchboard accessor (test introspection).
    const rusty::Arc<InMemorySwitchboard>& switchboard() const {
        return switchboard_;
    }

 private:
    rusty::Arc<InMemorySwitchboard> switchboard_;
};

class InMemoryFactoryAdapter {
 public:
    explicit InMemoryFactoryAdapter(rusty::Arc<InMemoryFactory> factory)
        : factory_(std::move(factory)) {}

    ConnectResult        connect(std::string_view addr) { return mut_factory().connect(addr); }
    ChannelListenerProxy make_listener()                { return mut_factory().make_listener(); }
    const char*          backend_name() const           { return factory_->backend_name(); }

 private:
    InMemoryFactory& mut_factory() {
        return const_cast<InMemoryFactory&>(*factory_.get());
    }
    rusty::Arc<InMemoryFactory> factory_;
};

inline ChannelFactoryProxy make_inmemory_factory_proxy(
        rusty::Arc<InMemoryFactory> factory) {
    return pro::make_proxy<ChannelFactoryFacade,
                           InMemoryFactoryAdapter>(std::move(factory));
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/**
 * Construct a connected `(a_side, b_side)` `InMemoryChannel` pair
 * directly, without going through the factory/listener path.
 * Useful for tests that want raw access to the channel halves —
 * for example, to call fault-injection methods (`inject_*`) on a
 * specific side. The two returned Arcs share an underlying
 * `InMemoryConnectionState`, so frames sent via either side fire
 * the other side's `on_frame` synchronously.
 *
 * `a_addr` is what the B-side observes via `peer_address()`;
 * `b_addr` is what the A-side observes via `peer_address()`.
 *
 * Production code uses `InMemoryFactory::connect(addr)` instead
 * (which internally builds a pair via this same path); this
 * helper is a test-only shortcut.
 */
std::pair<rusty::Arc<InMemoryChannel>, rusty::Arc<InMemoryChannel>>
make_channel_pair_for_testing(std::string a_addr, std::string b_addr);

}  // namespace rrr
