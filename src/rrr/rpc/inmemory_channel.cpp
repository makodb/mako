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
// Authored as inline Rust DSL. The 3 listener-registry methods stay
// as free functions (defined further down in the file) because they
// access `listeners_.lock()` directly — moving them to free fns
// taking `InMemorySwitchboard&` (non-const) lets us drop the
// `mutable` qualifier. Arc-holding callers wrap with `const_cast`.
#if RUSTYCPP_RUST
struct InMemorySwitchboard {
    listeners_: SpinMutex<rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.switchboard version=1 rust_sha256=30003e262fabab00305d5e421bd0985a89193c27f8c6845c33dfd3c6f1e2863d*/
struct InMemorySwitchboard;

struct InMemorySwitchboard {
    SpinMutex<rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>> listeners_;
};
/*RUSTYCPP:GEN-END id=inmemory_channel.switchboard*/

// Free functions (non-DSL) — see definitions further down.
bool inmemory_switchboard_register_listener(InMemorySwitchboard& self,
                                            std::string addr,
                                            rusty::sync::Weak<InMemoryListener> listener);
void inmemory_switchboard_unregister_listener(InMemorySwitchboard& self,
                                              const std::string& addr);
rusty::Option<rusty::Arc<InMemoryListener>> inmemory_switchboard_find_listener(
    InMemorySwitchboard& self, const std::string& addr);

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
// Per-channel callback table + fault-injection knobs that live inside
// `InMemoryConnectionState::inner`'s SpinMutex.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Drops the per-field NSDMI defaults — DSL aggregates rely on
// implicit value-init for zero-equivalent defaults:
//   * `bool` zero-inits to false (matches `a_closed = false`)
//   * `i32` zero-inits to 0 (matches `drop_next_sends_a = 0` etc.)
//   * `ChannelError` zero-inits to its first enumerator,
//     `ChannelError::None == 0` (matches the previous default)
//   * `std::string` default-inits to "" (the previous behavior)
//   * `OnXCallback` is an Arc<Function const>-backed wrapper; its
//     default ctor builds an empty Arc that surfaces as
//     `operator bool() == false` — same "unset callback" state as
//     before.
// Aggregate `InMemoryConnectionStateInner{}` thus preserves the
// original per-field defaults.
#if RUSTYCPP_RUST
struct InMemoryConnectionStateInner {
    a_peer_address: std::string,
    a_on_frame: OnFrameCallback,
    a_on_closed: OnClosedCallback,
    a_on_error: OnErrorCallback,
    a_closed: bool,
    b_peer_address: std::string,
    b_on_frame: OnFrameCallback,
    b_on_closed: OnClosedCallback,
    b_on_error: OnErrorCallback,
    b_closed: bool,
    drop_next_sends_a: i32,
    drop_next_sends_b: i32,
    send_error_count_a: i32,
    send_error_count_b: i32,
    send_error_a: ChannelError,
    send_error_b: ChannelError,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.inner version=1 rust_sha256=02b49f4f03431f720d1cb5ad65ad56b8c65fb0946cf1e354a239dea4999cc9a7*/
struct InMemoryConnectionStateInner;

struct InMemoryConnectionStateInner {
    std::string a_peer_address;
    OnFrameCallback a_on_frame;
    OnClosedCallback a_on_closed;
    OnErrorCallback a_on_error;
    bool a_closed;
    std::string b_peer_address;
    OnFrameCallback b_on_frame;
    OnClosedCallback b_on_closed;
    OnErrorCallback b_on_error;
    bool b_closed;
    int32_t drop_next_sends_a;
    int32_t drop_next_sends_b;
    int32_t send_error_count_a;
    int32_t send_error_count_b;
    ChannelError send_error_a;
    ChannelError send_error_b;
};
/*RUSTYCPP:GEN-END id=inmemory_channel.inner*/

// SpinMutex-owned inner state (rusty-style "data inside the mutex").
// All per-side callbacks, closed flags, and fault-injection knobs
// live in `InMemoryConnectionStateInner`; access through
// `inner.lock().unwrap()->...`. The legacy `mutable` qualifier on the
// field is gone — every caller already routes through
// `InMemoryChannel::mut_state()` (a const_cast wrapper), so the
// mutable shortcut was redundant.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
struct InMemoryConnectionState {
    inner: SpinMutex<InMemoryConnectionStateInner>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.state version=1 rust_sha256=aeb65d317f31a48e1aa021975b7e4d2dd8a348a4f325cfb725e4a93778f0e56e*/
struct InMemoryConnectionState;

struct InMemoryConnectionState {
    SpinMutex<InMemoryConnectionStateInner> inner;
};
/*RUSTYCPP:GEN-END id=inmemory_channel.state*/

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
// InMemoryChannel is now an aggregate (public fields, no user ctors,
// no `= delete`) — same shape as TcpFactory / InMemoryFactory /
// InMemoryListener. `rusty::Arc<InMemoryConnectionState>` carries
// the non-copyability contract implicitly; the `bool is_a_side_`
// field is trivially copyable. The legacy "non-movable — callbacks
// may capture pointers into this object" guard was overly
// conservative: callbacks are set on the *Arc-allocated heap copy*
// via `set_on_frame` etc., so the captured `this` is the heap
// address regardless of which T value flowed through new_() into
// the Arc allocation. Implicit movability of T is safe here.
struct InMemoryChannel {
    // @safe - static factory matching the rrr DSL `fn new(arg) -> Self`
    // pattern.
    static InMemoryChannel new_(rusty::Arc<InMemoryConnectionState> state, bool is_a_side);

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

// @safe - aggregate-init builds the struct in place.
inline InMemoryChannel InMemoryChannel::new_(rusty::Arc<InMemoryConnectionState> state, bool is_a_side) {
    return InMemoryChannel{.state_ = std::move(state), .is_a_side_ = is_a_side};
}

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

// SpinMutex-owned mutable state for InMemoryListener (rusty-style
// "data inside the mutex"). Sister type to InMemoryConnectionStateInner.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. Drops the `closed = false`
// NSDMI default — value-init via the aggregate path zero-inits bool
// to false. The OnXCallback default ctors build empty Arcs that
// surface as `operator bool() == false`, matching the previous
// behavior.
#if RUSTYCPP_RUST
struct InMemoryListenerInnerState {
    local_address: std::string,
    closed: bool,
    on_accept: OnAcceptCallback,
    on_error: OnErrorCallback,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.listener_inner version=1 rust_sha256=0e307ba7a4052691b887fdbea7ab0ac6ba244d9fac30cc6c3f2ca3824c1028cb*/
struct InMemoryListenerInnerState;

struct InMemoryListenerInnerState {
    std::string local_address;
    bool closed;
    OnAcceptCallback on_accept;
    OnErrorCallback on_error;
};
/*RUSTYCPP:GEN-END id=inmemory_channel.listener_inner*/

/**
 * In-memory accept-side listener. Implements the
 * `ChannelListenerBase` contract.
 *
 * On `listen(addr)`, the listener registers itself in the
 * switchboard. On `close()`, it unregisters and refuses further
 * accepts. Existing accepted connections are unaffected.
 */
// Authored as inline Rust DSL. The 6 ChannelListenerBase methods +
// accept_for_connect stay as free functions (defined further down in
// the file) — their bodies do non-trivial inner_.lock() routines +
// switchboard registration that don't translate to the DSL grammar.
// Because all methods now route through the adapter's mut_listener()
// const_cast (or are extracted free fns taking InMemoryListener&),
// the `mutable` qualifier on `inner_` is no longer needed.
#if RUSTYCPP_RUST
struct InMemoryListener {
    switchboard_: Arc<InMemorySwitchboard>,
    self_weak_: rusty::Option<rusty::sync::Weak<InMemoryListener>>,
    inner_: SpinMutex<InMemoryListenerInnerState>,
}

impl InMemoryListener {
    fn new(switchboard: Arc<InMemorySwitchboard>) -> InMemoryListener {
        InMemoryListener {
            switchboard_: switchboard,
            self_weak_: rusty::None,
            inner_: SpinMutex::<InMemoryListenerInnerState>::new(InMemoryListenerInnerState{}),
        }
    }

    fn set_self_weak(&mut self, w: rusty::sync::Weak<InMemoryListener>) {
        self.self_weak_ = rusty::Some(w);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.listener version=1 rust_sha256=e690807d639187c541b00b32764ae78b9c9c0fb7fd80b7785596aa01f1030250*/
struct InMemoryListener;

struct InMemoryListener {
    rusty::Arc<InMemorySwitchboard> switchboard_;
    rusty::Option<rusty::sync::Weak<InMemoryListener>> self_weak_;
    SpinMutex<InMemoryListenerInnerState> inner_;

    static InMemoryListener new_(rusty::Arc<InMemorySwitchboard> switchboard);
    void set_self_weak(rusty::sync::Weak<InMemoryListener> w);
};


InMemoryListener InMemoryListener::new_(rusty::Arc<InMemorySwitchboard> switchboard) {
    return InMemoryListener{.switchboard_ = std::move(switchboard), .self_weak_ = rusty::None, .inner_ = SpinMutex<InMemoryListenerInnerState>::new_(InMemoryListenerInnerState{})};
}

void InMemoryListener::set_self_weak(rusty::sync::Weak<InMemoryListener> w) {
    this->self_weak_ = rusty::Option<rusty::sync::Weak<InMemoryListener>>(std::move(w));
}
/*RUSTYCPP:GEN-END id=inmemory_channel.listener*/

// Free functions (non-DSL) — see definitions further down.
ChannelError inmemory_listener_listen(InMemoryListener& self, std::string_view addr);
void         inmemory_listener_close(InMemoryListener& self);
bool         inmemory_listener_is_closed(InMemoryListener& self);
std::string  inmemory_listener_local_address(InMemoryListener& self);
void         inmemory_listener_set_on_accept(InMemoryListener& self, OnAcceptCallback cb);
void         inmemory_listener_set_on_error(InMemoryListener& self, OnErrorCallback cb);
rusty::Option<rusty::Arc<InMemoryChannel>> inmemory_listener_accept_for_connect(InMemoryListener& self, const std::string& client_address);

// Adapter wrapping `Arc<InMemoryListener>` for the listener-proxy
// facade. Mirrors `TcpListenerChannelAdapter` (equivalent in spirit).
class InMemoryListenerAdapter : public ChannelListenerBase {
 public:
    explicit InMemoryListenerAdapter(rusty::Arc<InMemoryListener> listener)
        : listener_(std::move(listener)) {}

    // @unsafe - forwards through mut_listener() const_cast.
    ChannelError listen(std::string_view addr) override { return inmemory_listener_listen(mut_listener(), addr); }
    // @unsafe - forwards through mut_listener() const_cast.
    void         close() override              { inmemory_listener_close(mut_listener()); }
    // @unsafe - forwards through mut_listener() const_cast (is_closed
    // mutates inner_ via lock() — the const wrapper on the adapter
    // matches the trait signature).
    bool         is_closed() const override    { return inmemory_listener_is_closed(const_cast<InMemoryListenerAdapter*>(this)->mut_listener()); }
    // @unsafe - forwards through mut_listener() const_cast.
    std::string  local_address() const override { return inmemory_listener_local_address(const_cast<InMemoryListenerAdapter*>(this)->mut_listener()); }
    // @unsafe - forwards through mut_listener() const_cast.
    void set_on_accept(OnAcceptCallback cb) override { inmemory_listener_set_on_accept(mut_listener(), std::move(cb)); }
    // @unsafe - forwards through mut_listener() const_cast.
    void set_on_error (OnErrorCallback  cb) override { inmemory_listener_set_on_error(mut_listener(), std::move(cb)); }

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
// InMemoryFactory is now an aggregate (public fields, no user ctors,
// no `= delete`) — same shape as TcpFactory and the rrr DSL-style
// aggregates. Non-copyability falls out implicitly because the
// `rusty::Arc<InMemorySwitchboard>` field is itself non-copyable.
// Callers build via `Arc<InMemoryFactory>::new_(InMemoryFactory::new_(arg))`.
// Authored as inline Rust DSL. `connect` and `make_listener` stay as
// free functions (defined further down in the file) — their bodies
// hold socket-free in-memory pairing logic that doesn't translate to
// the DSL grammar today. The dead `switchboard()` accessor was
// removed in the same pass.
#if RUSTYCPP_RUST
struct InMemoryFactory {
    switchboard_: Arc<InMemorySwitchboard>,
}

impl InMemoryFactory {
    fn new(switchboard: Arc<InMemorySwitchboard>) -> InMemoryFactory {
        InMemoryFactory { switchboard_: switchboard }
    }

    fn backend_name(&self) -> std::string {
        std::string("inmemory")
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.factory version=1 rust_sha256=77e1d650978cb4e87a40744ffef09024ba831ff54a9f7024a905dffee28721ec*/
struct InMemoryFactory;

struct InMemoryFactory {
    rusty::Arc<InMemorySwitchboard> switchboard_;

    static InMemoryFactory new_(rusty::Arc<InMemorySwitchboard> switchboard);
    std::string backend_name() const;
};


InMemoryFactory InMemoryFactory::new_(rusty::Arc<InMemorySwitchboard> switchboard) {
    return InMemoryFactory{.switchboard_ = std::move(switchboard)};
}

std::string InMemoryFactory::backend_name() const {
    return std::string("inmemory");
}
/*RUSTYCPP:GEN-END id=inmemory_channel.factory*/

// Free functions (non-DSL) — see definitions further down.
ConnectResult                       inmemory_factory_connect(InMemoryFactory& self, std::string_view addr);
rusty::Option<ChannelListenerProxy> inmemory_factory_make_listener(InMemoryFactory& self);

class InMemoryFactoryAdapter : public ChannelFactoryBase {
 public:
    explicit InMemoryFactoryAdapter(rusty::Arc<InMemoryFactory> factory)
        : factory_(std::move(factory)) {}

    // @unsafe - forwards through mut_factory() const_cast.
    ConnectResult                       connect(std::string_view addr) override { return inmemory_factory_connect(mut_factory(), addr); }
    // @unsafe - forwards through mut_factory() const_cast.
    rusty::Option<ChannelListenerProxy> make_listener() override                { return inmemory_factory_make_listener(mut_factory()); }
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

bool inmemory_switchboard_register_listener(InMemorySwitchboard& self,
                                            std::string addr,
                                            rusty::sync::Weak<InMemoryListener> listener) {
    auto guard = self.listeners_.lock().unwrap();
    if (guard->contains_key(addr)) {
        // Address already taken. The caller must close the existing
        // listener first.
        return false;
    }
    guard->insert(std::move(addr), std::move(listener));
    return true;
}

void inmemory_switchboard_unregister_listener(InMemorySwitchboard& self,
                                              const std::string& addr) {
    auto guard = self.listeners_.lock().unwrap();
    guard->remove(addr);
}

rusty::Option<rusty::Arc<InMemoryListener>>
inmemory_switchboard_find_listener(InMemorySwitchboard& self, const std::string& addr) {
    auto guard = self.listeners_.lock().unwrap();
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

ChannelError inmemory_listener_listen(InMemoryListener& self, std::string_view addr) {
    rusty::sync::Weak<InMemoryListener> w;
    {
        auto guard = self.inner_.lock().unwrap();
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
        if (self.self_weak_.is_none()) {
            Log_error("rrr::InMemoryListener::listen: self_weak_ not set "
                      "(caller must call set_self_weak before listen)");
            return ChannelError::Internal;
        }
        guard->local_address = std::string(addr);
        w = self.self_weak_.as_ref().unwrap().clone();
    }
    if (!inmemory_switchboard_register_listener(const_cast<InMemorySwitchboard&>(*self.switchboard_.get()), std::string(addr), std::move(w))) {
        // Address already taken in the switchboard.
        auto guard = self.inner_.lock().unwrap();
        guard->local_address.clear();
        return ChannelError::AddressInUse;
    }
    return ChannelError::None;
}

void inmemory_listener_close(InMemoryListener& self) {
    std::string addr_to_unregister;
    {
        auto guard = self.inner_.lock().unwrap();
        if (guard->closed) return;
        guard->closed = true;
        addr_to_unregister = guard->local_address;
    }
    if (!addr_to_unregister.empty()) {
        inmemory_switchboard_unregister_listener(const_cast<InMemorySwitchboard&>(*self.switchboard_.get()), addr_to_unregister);
    }
}

bool inmemory_listener_is_closed(InMemoryListener& self) {
    auto guard = self.inner_.lock().unwrap();
    return guard->closed;
}

std::string inmemory_listener_local_address(InMemoryListener& self) {
    auto guard = self.inner_.lock().unwrap();
    return guard->local_address;
}

void inmemory_listener_set_on_accept(InMemoryListener& self, OnAcceptCallback cb) {
    auto guard = self.inner_.lock().unwrap();
    guard->on_accept = std::move(cb);
}

void inmemory_listener_set_on_error(InMemoryListener& self, OnErrorCallback cb) {
    auto guard = self.inner_.lock().unwrap();
    guard->on_error = std::move(cb);
}

// @unsafe - inline `const_cast<InMemoryConnectionState*>(state.get())`
// to bootstrap the shared connection state before the per-side Arcs
// are constructed.
rusty::Option<rusty::Arc<InMemoryChannel>>
inmemory_listener_accept_for_connect(InMemoryListener& self, const std::string& client_address) {
    OnAcceptCallback cb_to_fire;
    std::string server_address;
    {
        auto guard = self.inner_.lock().unwrap();
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
    auto client_side = rusty::Arc<InMemoryChannel>::new_(InMemoryChannel::new_(state.clone(),
                                                          /*is_a_side=*/true));
    auto server_side = rusty::Arc<InMemoryChannel>::new_(InMemoryChannel::new_(state.clone(),
                                                          /*is_a_side=*/false));

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
ConnectResult inmemory_factory_connect(InMemoryFactory& self, std::string_view addr) {
    std::string addr_str(addr);
    auto listener_opt = inmemory_switchboard_find_listener(const_cast<InMemorySwitchboard&>(*self.switchboard_.get()), addr_str);
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
    auto client_opt = inmemory_listener_accept_for_connect(mut_listener, client_address);
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
    auto a_side = rusty::Arc<InMemoryChannel>::new_(InMemoryChannel::new_(state.clone(), /*is_a_side=*/true));
    auto b_side = rusty::Arc<InMemoryChannel>::new_(InMemoryChannel::new_(state.clone(), /*is_a_side=*/false));
    return {std::move(a_side), std::move(b_side)};
}

// @unsafe - inline `const_cast<InMemoryListener&>(*listener.get())` to
// wire `self_weak_` before publishing the listener.
rusty::Option<ChannelListenerProxy> inmemory_factory_make_listener(InMemoryFactory& self) {
    auto listener = rusty::Arc<InMemoryListener>::new_(InMemoryListener::new_(self.switchboard_));
    // Wire the self-weak so the listener can register itself in the
    // switchboard. Mirrors TcpFactory::make_listener.
    {
        auto& mut_l = const_cast<InMemoryListener&>(*listener.get());
        mut_l.set_self_weak(rusty::sync::downgrade(listener));
    }
    return rusty::Some(make_inmemory_listener_proxy(std::move(listener)));
}


}  // namespace rrr
