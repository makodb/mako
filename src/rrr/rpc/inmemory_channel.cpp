module;

#include <cstdint>
#include <cstdlib>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/weak.hpp>

export module rrr.inmemory_channel;

import std;
import rusty;
import rrr.channel;
import rrr.debugging;
import rrr.logging;
import rrr.threading;

// @safe - in-memory channel backend. Switchboard + Listener bodies use
// SpinMutex + rusty::HashMap + rusty::Weak (all safe). InMemoryChannel
// and the four `*Adapter` shims thread through const_cast helpers
// (`mut_state` / `mut_conn` / `mut_listener` / `mut_factory`) and
// `send_frame` does raw `uint8_t*` byte slicing — those methods carry
// per-method `// @unsafe` below. InMemoryListener::accept_for_connect,
// InMemoryFactory::connect/make_listener, and the test helper
// make_channel_pair_for_testing also const_cast inline and are
// `// @unsafe`.
export namespace rrr {


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
 * Thread-safe: an internal `SpinMutex` owns the listener map. Most
 * tests are single-threaded but the locking lets a test fire
 * `on_frame` on one thread while another connects.
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
     * directly. Internal state (the SpinMutex-wrapped `listeners_`)
     * is `mutable` — the standard pattern for thread-safe shared
     * registries.
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
    // SpinMutex owns the HashMap (rusty-style "data inside the mutex").
    // Replaces the prior pair `mu_` + `listeners_` pattern.
    mutable SpinMutex<rusty::HashMap<std::string,
                                     rusty::sync::Weak<InMemoryListener>>> listeners_;
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
    // SpinMutex-owned inner state (rusty-style "data inside the mutex").
    // All per-side callbacks, closed flags, and fault-injection knobs
    // live here; access through `inner.lock().unwrap()->...`.
    struct Inner {
        // A-side state.  `OnXCallback` is the Arc<Function const>-
        // backed wrapper from channel.hpp; default-construction holds
        // an Arc wrapping an empty inner Function, which the wrapper
        // surfaces as `operator bool() == false` (the unset-callback
        // state).
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
    };

    mutable SpinMutex<Inner> inner;
};

// ---------------------------------------------------------------------------
// InMemoryChannel
// ---------------------------------------------------------------------------

/**
 * One half of a paired in-memory connection. Implements the
 * `ChannelConnectionBase` contract.
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

    // ChannelConnectionBase methods.
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
    // @unsafe - const_cast through Arc::get<T*>().
    InMemoryConnectionState& mut_state() const {
        return const_cast<InMemoryConnectionState&>(*state_.get());
    }

    rusty::Arc<InMemoryConnectionState> state_;
    bool is_a_side_;
};

// Adapter wrapping `Arc<InMemoryChannel>` for the channel virtual
// base. Mirrors `TcpConnectionChannelAdapter`.
class InMemoryChannelAdapter : public ChannelConnectionBase {
 public:
    explicit InMemoryChannelAdapter(rusty::Arc<InMemoryChannel> conn)
        : conn_(std::move(conn)) {}

    // @unsafe - forwards through mut_conn() const_cast.
    ChannelError send_frame(const ChannelFrame& f) override { return mut_conn().send_frame(f); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         flush() override              { mut_conn().flush(); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         close() override              { mut_conn().close(); }
    // @unsafe - forwards through conn_-> to InMemoryChannel::is_closed which calls mut_state.
    bool         is_closed() const override    { return conn_->is_closed(); }
    // @unsafe - forwards through conn_-> to InMemoryChannel::peer_address which calls mut_state.
    std::string  peer_address() const override { return conn_->peer_address(); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         set_on_frame (OnFrameCallback  cb) override { mut_conn().set_on_frame (std::move(cb)); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         set_on_closed(OnClosedCallback cb) override { mut_conn().set_on_closed(std::move(cb)); }
    // @unsafe - forwards through mut_conn() const_cast.
    void         set_on_error (OnErrorCallback  cb) override { mut_conn().set_on_error (std::move(cb)); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
    InMemoryChannel& mut_conn() {
        return const_cast<InMemoryChannel&>(*conn_.get());
    }
    rusty::Arc<InMemoryChannel> conn_;
};

inline ChannelConnectionProxy make_inmemory_channel_proxy(
        rusty::Arc<InMemoryChannel> conn) {
    return rusty::make_box<InMemoryChannelAdapter>(std::move(conn));
}

// ---------------------------------------------------------------------------
// InMemoryListener
// ---------------------------------------------------------------------------

/**
 * In-memory accept-side listener. Implements the
 * `ChannelListenerBase` contract.
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

    // ChannelListenerBase methods.
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
    // Fields outside the SpinMutex: switchboard_ is set once at
    // construction (immutable Arc), self_weak_ is set once via
    // set_self_weak() before any concurrent listener access.
    rusty::Arc<InMemorySwitchboard> switchboard_;
    rusty::Option<rusty::sync::Weak<InMemoryListener>> self_weak_{rusty::None};

    // SpinMutex-owned mutable state (rusty-style "data inside the mutex").
    struct InnerState {
        std::string        local_address;
        bool               closed = false;
        OnAcceptCallback   on_accept;
        OnErrorCallback    on_error;
    };
    mutable SpinMutex<InnerState> inner_;
};

// Adapter wrapping `Arc<InMemoryListener>` for the listener-proxy
// facade. Mirrors `TcpListenerChannelAdapter` (equivalent in spirit).
class InMemoryListenerAdapter : public ChannelListenerBase {
 public:
    explicit InMemoryListenerAdapter(rusty::Arc<InMemoryListener> listener)
        : listener_(std::move(listener)) {}

    // @unsafe - forwards through mut_listener() const_cast.
    ChannelError listen(std::string_view addr) override { return mut_listener().listen(addr); }
    // @unsafe - forwards through mut_listener() const_cast.
    void         close() override              { mut_listener().close(); }
    bool         is_closed() const override    { return listener_->is_closed(); }
    std::string  local_address() const override { return listener_->local_address(); }
    // @unsafe - forwards through mut_listener() const_cast.
    void set_on_accept(OnAcceptCallback cb) override { mut_listener().set_on_accept(std::move(cb)); }
    // @unsafe - forwards through mut_listener() const_cast.
    void set_on_error (OnErrorCallback  cb) override { mut_listener().set_on_error (std::move(cb)); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
    InMemoryListener& mut_listener() {
        return const_cast<InMemoryListener&>(*listener_.get());
    }
    rusty::Arc<InMemoryListener> listener_;
};

inline ChannelListenerProxy make_inmemory_listener_proxy(
        rusty::Arc<InMemoryListener> listener) {
    return rusty::make_box<InMemoryListenerAdapter>(std::move(listener));
}

// ---------------------------------------------------------------------------
// InMemoryFactory
// ---------------------------------------------------------------------------

/**
 * In-memory factory implementing `ChannelFactoryBase`.
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

    // ChannelFactoryBase methods.
    ConnectResult                       connect(std::string_view addr);
    rusty::Option<ChannelListenerProxy> make_listener();
    std::string                         backend_name() const { return "inmemory"; }

    // Switchboard accessor (test introspection).
    const rusty::Arc<InMemorySwitchboard>& switchboard() const {
        return switchboard_;
    }

 private:
    rusty::Arc<InMemorySwitchboard> switchboard_;
};

class InMemoryFactoryAdapter : public ChannelFactoryBase {
 public:
    explicit InMemoryFactoryAdapter(rusty::Arc<InMemoryFactory> factory)
        : factory_(std::move(factory)) {}

    // @unsafe - forwards through mut_factory() const_cast.
    ConnectResult                       connect(std::string_view addr) override { return mut_factory().connect(addr); }
    // @unsafe - forwards through mut_factory() const_cast.
    rusty::Option<ChannelListenerProxy> make_listener() override                { return mut_factory().make_listener(); }
    std::string                         backend_name() const override           { return factory_->backend_name(); }

 private:
    // @unsafe - const_cast through Arc::get<T*>().
    InMemoryFactory& mut_factory() {
        return const_cast<InMemoryFactory&>(*factory_.get());
    }
    rusty::Arc<InMemoryFactory> factory_;
};

inline ChannelFactoryProxy make_inmemory_factory_proxy(
        rusty::Arc<InMemoryFactory> factory) {
    return rusty::make_box<InMemoryFactoryAdapter>(std::move(factory));
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


}  // export namespace rrr

// @safe - impl namespace. InMemorySwitchboard methods are pure
// SpinMutex + HashMap + Weak::upgrade and inherit @safe. InMemoryChannel
// out-of-class defs route through mut_state() (@unsafe) so each carries
// a per-method `// @unsafe`. InMemoryListener::accept_for_connect,
// InMemoryFactory::connect/make_listener, and the test helper
// make_channel_pair_for_testing const_cast inline and are `// @unsafe`.
namespace rrr {

// ---------------------------------------------------------------------------
// InMemorySwitchboard
// ---------------------------------------------------------------------------

bool InMemorySwitchboard::register_listener(
        std::string addr,
        rusty::sync::Weak<InMemoryListener> listener) const {
    auto guard = listeners_.lock().unwrap();
    if (guard->contains_key(addr)) {
        // Address already taken. The caller must close the existing
        // listener first.
        return false;
    }
    guard->insert(std::move(addr), std::move(listener));
    return true;
}

void InMemorySwitchboard::unregister_listener(const std::string& addr) const {
    auto guard = listeners_.lock().unwrap();
    guard->remove(addr);
}

rusty::Option<rusty::Arc<InMemoryListener>>
InMemorySwitchboard::find_listener(const std::string& addr) const {
    auto guard = listeners_.lock().unwrap();
    auto val_opt = guard->get(addr);
    if (val_opt.is_none()) {
        return rusty::None;
    }
    // Upgrade through the reference before any mutation invalidates it.
    // HashMap::get now returns Option<V&> (V = Weak<InMemoryListener>),
    // so unwrap() yields a reference — no raw pointer at the call site.
    rusty::Option<rusty::Arc<InMemoryListener>> upgraded =
        val_opt.unwrap().upgrade();
    if (upgraded.is_none()) {
        // The listener was destroyed without unregistering. Clean up.
        guard->remove(addr);
        return rusty::None;
    }
    return upgraded;
}

// ---------------------------------------------------------------------------
// InMemoryChannel
// ---------------------------------------------------------------------------

// @unsafe - mut_state() const_cast + raw `uint8_t*` byte slicing
// (`bytes.assign(f.payload, f.payload + f.size)` then `bytes.data()`).
ChannelError InMemoryChannel::send_frame(const ChannelFrame& f) {
    // Default-constructed wrapper: Arc holds an empty Function; we'll
    // either reassign (wrapper copy = Arc clone, atomic refcount bump)
    // or leave it empty.
    OnFrameCallback peer_on_frame;
    bool peer_already_closed = false;
    bool self_already_closed = false;
    bool drop_this_send      = false;
    bool inject_error        = false;
    ChannelError injected_err = ChannelError::None;

    // Snapshot the peer's on_frame and the closed flags under the
    // lock; release the lock before invoking the callback so the
    // callback can call back into this channel without deadlocking
    // (typical pattern: receiver fires send_frame in response).
    //
    // 6c: also consume one tick of the per-side fault injection
    // counters here. Drop counter takes precedence over error
    // counter — if both are set, drops fire first while the drop
    // counter is positive.
    {
        auto guard = mut_state().inner.lock().unwrap();
        if (is_a_side_) {
            self_already_closed = guard->a_closed;
            peer_already_closed = guard->b_closed;
            peer_on_frame       = guard->b_on_frame;
            if (guard->drop_next_sends_a > 0) {
                drop_this_send = true;
                --guard->drop_next_sends_a;
            } else if (guard->send_error_count_a > 0) {
                inject_error  = true;
                injected_err  = guard->send_error_a;
                --guard->send_error_count_a;
            }
        } else {
            self_already_closed = guard->b_closed;
            peer_already_closed = guard->a_closed;
            peer_on_frame       = guard->a_on_frame;
            if (guard->drop_next_sends_b > 0) {
                drop_this_send = true;
                --guard->drop_next_sends_b;
            } else if (guard->send_error_count_b > 0) {
                inject_error  = true;
                injected_err  = guard->send_error_b;
                --guard->send_error_count_b;
            }
        }
    }

    if (self_already_closed) {
        return ChannelError::ConnectionReset;
    }
    if (peer_already_closed) {
        return ChannelError::ConnectionReset;
    }
    // 6c: fault injection. Drop happens first; then error.
    if (drop_this_send) {
        return ChannelError::None;  // silent drop; sender unaware
    }
    if (inject_error) {
        return injected_err;
    }

    // Copy bytes into a temporary buffer so the peer's callback can
    // safely retain a pointer for the duration of the call (the
    // ChannelFrame contract: payload valid only during on_frame).
    std::vector<std::uint8_t> bytes;
    if (f.size > 0 && f.payload != nullptr) {
        bytes.assign(f.payload, f.payload + f.size);
    }
    ChannelFrame delivered{bytes.data(), bytes.size()};

    if (peer_on_frame) {
        peer_on_frame(delivered);
    }
    return ChannelError::None;
}

// ---------------------------------------------------------------------------
// 6c: fault injection methods (test-only).
// ---------------------------------------------------------------------------

// @unsafe - mut_state() const_cast.
void InMemoryChannel::inject_drop_next_sends(int count) {
    auto guard = mut_state().inner.lock().unwrap();
    if (is_a_side_) {
        guard->drop_next_sends_a = count;
    } else {
        guard->drop_next_sends_b = count;
    }
}

// @unsafe - mut_state() const_cast.
void InMemoryChannel::inject_send_error(ChannelError err, int count) {
    auto guard = mut_state().inner.lock().unwrap();
    if (is_a_side_) {
        guard->send_error_a       = err;
        guard->send_error_count_a = count;
    } else {
        guard->send_error_b       = err;
        guard->send_error_count_b = count;
    }
}

// @unsafe - mut_state() const_cast.
void InMemoryChannel::clear_fault_injection() {
    auto guard = mut_state().inner.lock().unwrap();
    if (is_a_side_) {
        guard->drop_next_sends_a  = 0;
        guard->send_error_count_a = 0;
        guard->send_error_a       = ChannelError::None;
    } else {
        guard->drop_next_sends_b  = 0;
        guard->send_error_count_b = 0;
        guard->send_error_b       = ChannelError::None;
    }
}

// 6b: close semantics — peer-only on_closed fire.
//
// close() flips the local-side closed flag, then synchronously fires
// the *peer's* on_closed callback (if the peer hasn't already
// closed, in which case its callback was — or will be — fired by
// its own close()). close() is idempotent: subsequent calls return
// without re-firing the peer's callback.
//
// Note: unlike `TcpConnection::close()` (which also delivers
// on_closed to *self* via `deliver_on_closed_locked`), this
// implementation does NOT fire self's on_closed. That choice keeps
// the InMemory backend semantics simple — the user-thread caller
// who invoked close() typically does its own cleanup inline; only
// the peer needs an asynchronous notification. Tests that want
// self-close → self-on_closed mirroring should explicitly invoke
// the on_closed callback they installed.
//
// `send_frame` already returns `ChannelError::ConnectionReset` if
// either side is closed, so once close() returns the connection is
// observably dead in both directions (verified by the
// `SendFrameAfterPeerCloseReturnsReset` test).
// @unsafe - mut_state() const_cast.
void InMemoryChannel::close() {
    OnClosedCallback peer_on_closed;
    bool fire_peer_closed = false;
    {
        auto guard = mut_state().inner.lock().unwrap();
        if (is_a_side_) {
            if (guard->a_closed) return;  // idempotent
            guard->a_closed = true;
            // Notify peer if it's not already closed.
            if (!guard->b_closed) {
                peer_on_closed = guard->b_on_closed;
                fire_peer_closed = true;
            }
        } else {
            if (guard->b_closed) return;
            guard->b_closed = true;
            if (!guard->a_closed) {
                peer_on_closed = guard->a_on_closed;
                fire_peer_closed = true;
            }
        }
    }
    if (fire_peer_closed && peer_on_closed) {
        peer_on_closed(ChannelError::None);
    }
}

// @unsafe - mut_state() const_cast.
bool InMemoryChannel::is_closed() const {
    // 6b: report closed if EITHER side has been closed. This matches
    // the TCP backend's behavior — once the peer disconnects, the
    // connection is unusable and `send_frame` will return
    // `ConnectionReset` (verified in `send_frame`'s peer_already_closed
    // check). The channel-layer contract (`channel.hpp`):
    //   "After `is_closed()` returns true, `send_frame` must return
    //    a non-None error..."
    // Reporting joint state (a_closed || b_closed) ensures the
    // implication holds in both directions.
    auto guard = mut_state().inner.lock().unwrap();
    return guard->a_closed || guard->b_closed;
}

// @unsafe - mut_state() const_cast.
std::string InMemoryChannel::peer_address() const {
    auto guard = mut_state().inner.lock().unwrap();
    return is_a_side_ ? guard->b_peer_address : guard->a_peer_address;
}

// @unsafe - mut_state() const_cast.
void InMemoryChannel::set_on_frame(OnFrameCallback cb) {
    auto guard = mut_state().inner.lock().unwrap();
    if (is_a_side_) guard->a_on_frame  = std::move(cb);
    else            guard->b_on_frame  = std::move(cb);
}

// @unsafe - mut_state() const_cast.
void InMemoryChannel::set_on_closed(OnClosedCallback cb) {
    auto guard = mut_state().inner.lock().unwrap();
    if (is_a_side_) guard->a_on_closed = std::move(cb);
    else            guard->b_on_closed = std::move(cb);
}

// @unsafe - mut_state() const_cast.
void InMemoryChannel::set_on_error(OnErrorCallback cb) {
    auto guard = mut_state().inner.lock().unwrap();
    if (is_a_side_) guard->a_on_error  = std::move(cb);
    else            guard->b_on_error  = std::move(cb);
}

// ---------------------------------------------------------------------------
// InMemoryListener
// ---------------------------------------------------------------------------

ChannelError InMemoryListener::listen(std::string_view addr) {
    rusty::sync::Weak<InMemoryListener> w;
    {
        auto guard = inner_.lock().unwrap();
        if (guard->closed) {
            return ChannelError::Internal;
        }
        if (!guard->local_address.empty()) {
            // Already listening; treat as idempotent if same addr.
            if (guard->local_address == std::string(addr)) {
                return ChannelError::None;
            }
            return ChannelError::AddressInUse;
        }
        if (self_weak_.is_none()) {
            Log_error("rrr::InMemoryListener::listen: self_weak_ not set "
                      "(caller must call set_self_weak before listen)");
            return ChannelError::Internal;
        }
        guard->local_address = std::string(addr);
        w = self_weak_.as_ref().unwrap().clone();
    }
    if (!switchboard_->register_listener(std::string(addr), std::move(w))) {
        // Address already taken in the switchboard.
        auto guard = inner_.lock().unwrap();
        guard->local_address.clear();
        return ChannelError::AddressInUse;
    }
    return ChannelError::None;
}

void InMemoryListener::close() {
    std::string addr_to_unregister;
    {
        auto guard = inner_.lock().unwrap();
        if (guard->closed) return;
        guard->closed = true;
        addr_to_unregister = guard->local_address;
    }
    if (!addr_to_unregister.empty()) {
        switchboard_->unregister_listener(addr_to_unregister);
    }
}

bool InMemoryListener::is_closed() const {
    auto guard = inner_.lock().unwrap();
    return guard->closed;
}

std::string InMemoryListener::local_address() const {
    auto guard = inner_.lock().unwrap();
    return guard->local_address;
}

void InMemoryListener::set_on_accept(OnAcceptCallback cb) {
    auto guard = inner_.lock().unwrap();
    guard->on_accept = std::move(cb);
}

void InMemoryListener::set_on_error(OnErrorCallback cb) {
    auto guard = inner_.lock().unwrap();
    guard->on_error = std::move(cb);
}

// @unsafe - inline `const_cast<InMemoryConnectionState*>(state.get())`
// to bootstrap the shared connection state before the per-side Arcs
// are constructed.
rusty::Option<rusty::Arc<InMemoryChannel>>
InMemoryListener::accept_for_connect(const std::string& client_address) {
    OnAcceptCallback cb_to_fire;
    std::string server_address;
    {
        auto guard = inner_.lock().unwrap();
        if (guard->closed || guard->local_address.empty()) {
            return rusty::None;
        }
        cb_to_fire = guard->on_accept;  // wrapper copy = Arc clone (refcount++)
        server_address = guard->local_address;
    }
    if (!cb_to_fire) {
        // Listener exists but has no accept handler installed yet.
        // Mirror TCP: accept the connection and the proxy is dropped
        // (effectively connection refused from the user's perspective).
        // Return None so the factory surfaces ConnectionRefused.
        return rusty::None;
    }

    // Build the paired connection state. Naming convention:
    // `a_peer_address` is the peer of the A-side (client), so it
    // holds the *server's* address. `b_peer_address` is the peer of
    // the B-side (server), holding the *client's* address. The
    // peer_address() accessors swap accordingly:
    //   - A-side (client).peer_address() = b_peer_address... wait,
    // let me re-derive: peer_address() returns the OTHER side's
    // identity. From A's perspective, the peer is B (server). So
    // A.peer_address() = server's identity.
    //
    // Layout: store each side's *own* identity in
    // `<side>_peer_address`. Then A.peer_address() returns
    // b_peer_address (B's identity = the server addr); B's
    // peer_address() returns a_peer_address (A's identity = the
    // client addr).
    auto state = rusty::Arc<InMemoryConnectionState>::make();
    {
        // No external mutators yet — but the SpinMutex-owned inner
        // requires going through the lock guard for any access.
        auto* mut_state = const_cast<InMemoryConnectionState*>(state.get());
        auto guard = mut_state->inner.lock().unwrap();
        guard->a_peer_address = client_address;  // A is the client
        guard->b_peer_address = server_address;  // B is the server
    }
    auto client_side = rusty::Arc<InMemoryChannel>::make(state.clone(),
                                                          /*is_a_side=*/true);
    auto server_side = rusty::Arc<InMemoryChannel>::make(state.clone(),
                                                          /*is_a_side=*/false);

    // Hand the server-side proxy to the on_accept callback. The
    // callback typically wires server-side handlers (set_on_frame
    // etc.) inline.
    cb_to_fire(make_inmemory_channel_proxy(std::move(server_side)));

    return rusty::Some(std::move(client_side));
}

// ---------------------------------------------------------------------------
// InMemoryFactory
// ---------------------------------------------------------------------------

// @unsafe - inline `const_cast<InMemoryListener&>(*listener.get())` to
// invoke accept_for_connect on the listener pulled out of the
// switchboard.
ConnectResult InMemoryFactory::connect(std::string_view addr) {
    std::string addr_str(addr);
    auto listener_opt = switchboard_->find_listener(addr_str);
    if (listener_opt.is_none()) {
        return ConnectResult{rusty::None, ChannelError::ConnectionRefused};
    }
    auto listener = listener_opt.unwrap();
    // Use a synthesized client address. Future work could let the
    // factory accept a configurable client-side identity for tests
    // that care about peer_address() values.
    static std::atomic<uint64_t> client_counter{0};
    uint64_t client_id = client_counter.fetch_add(1, std::memory_order_relaxed);
    std::string client_address = "inmemory://client-" + std::to_string(client_id);

    // @unsafe - const_cast through Arc::get for accept_for_connect.
    auto& mut_listener = const_cast<InMemoryListener&>(*listener.get());
    auto client_opt = mut_listener.accept_for_connect(client_address);
    if (client_opt.is_none()) {
        return ConnectResult{rusty::None, ChannelError::ConnectionRefused};
    }
    auto client_side = client_opt.unwrap();
    return ConnectResult{
        rusty::Some(make_inmemory_channel_proxy(std::move(client_side))),
        ChannelError::None,
    };
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// @unsafe - inline `const_cast<InMemoryConnectionState*>(state.get())`
// to bootstrap the shared connection state.
std::pair<rusty::Arc<InMemoryChannel>, rusty::Arc<InMemoryChannel>>
make_channel_pair_for_testing(std::string a_addr, std::string b_addr) {
    auto state = rusty::Arc<InMemoryConnectionState>::make();
    {
        auto* mut_state = const_cast<InMemoryConnectionState*>(state.get());
        auto guard = mut_state->inner.lock().unwrap();
        guard->a_peer_address = std::move(a_addr);
        guard->b_peer_address = std::move(b_addr);
    }
    auto a_side = rusty::Arc<InMemoryChannel>::make(state.clone(), /*is_a_side=*/true);
    auto b_side = rusty::Arc<InMemoryChannel>::make(state.clone(), /*is_a_side=*/false);
    return {std::move(a_side), std::move(b_side)};
}

// @unsafe - inline `const_cast<InMemoryListener&>(*listener.get())` to
// wire `self_weak_` before publishing the listener.
rusty::Option<ChannelListenerProxy> InMemoryFactory::make_listener() {
    auto listener = rusty::Arc<InMemoryListener>::make(switchboard_);
    // Wire the self-weak so the listener can register itself in the
    // switchboard. Mirrors TcpFactory::make_listener.
    {
        auto& mut_l = const_cast<InMemoryListener&>(*listener.get());
        mut_l.set_self_weak(rusty::sync::downgrade(listener));
    }
    return rusty::Some(make_inmemory_listener_proxy(std::move(listener)));
}


}  // namespace rrr
