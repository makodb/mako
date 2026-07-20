module;

#include <cstdint>
#include <cstdlib>

#include <rusty/arc.hpp>
#include <rusty/rusty.hpp>
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
// rusty::Mutex + rusty::HashMap + rusty::Weak (all safe). InMemoryChannel
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
 * Thread-safe: an internal `rusty::Mutex` owns the listener map. Most
 * tests are single-threaded but the locking lets a test fire
 * `on_frame` on one thread while another connects.
 */
// Authored as inline Rust DSL, registry methods included (the former
// backing free functions moved in once HashMap::get's Option<V&> return
// lowered — category-K sweep). Methods take &self: the Mutex's
// const-qualified lock() provides the interior mutability, so
// Arc-holding callers need no const_cast.
// Hand-bridge (playbook §7.2): rusty::sync::Weak has only a default C++
// ctor, which the DSL cannot spell.
inline rusty::sync::Weak<InMemoryListener> empty_listener_weak() { return {}; }

#if RUSTYCPP_RUST
struct InMemorySwitchboard {
    listeners_: rusty::Mutex<rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>>,
}

impl InMemorySwitchboard {
    // Factory: rusty::Mutex has no default ctor (unlike the retired SpinMutex),
    // so build the empty switchboard explicitly. Callers use new_() instead of
    // Arc::make() (which would default-construct the now-non-default aggregate).
    fn new() -> InMemorySwitchboard {
        InMemorySwitchboard {
            listeners_: rusty::Mutex::<rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>>::new(rusty::HashMap::<std::string, rusty::sync::Weak<InMemoryListener>>::new()),
        }
    }

    // Claim `addr`. False if already taken (caller must close the
    // existing listener first).
    fn register_listener(&self, addr: std::string, listener: rusty::sync::Weak<InMemoryListener>) -> bool {
        let mut guard = self.listeners_.lock().unwrap();
        if (*guard).contains_key(addr) {
            return false;
        }
        (*guard).insert(addr, listener);
        true
    }

    fn unregister_listener(&self, addr: &std::string) {
        let mut guard = self.listeners_.lock().unwrap();
        (*guard).remove(addr);
    }

    // Upgrade through the Option<V&> reference BEFORE any mutation
    // invalidates it; a dead Weak is lazily cleaned up.
    fn find_listener(&self, addr: &std::string) -> Option<rusty::Arc<InMemoryListener>> {
        let mut guard = self.listeners_.lock().unwrap();
        let val_opt = (*guard).get(addr);
        if val_opt.is_none() {
            return None;
        }
        let upgraded = val_opt.unwrap().upgrade();
        if upgraded.is_none() {
            (*guard).remove(addr);
            return None;
        }
        upgraded
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.switchboard version=1 rust_sha256=ad457f1472b1d7a0a3e9ade8be199b8b21175b8b12accd17ee0d56ef32656ce0*/
struct InMemorySwitchboard;

struct InMemorySwitchboard {
    rusty::Mutex<rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>> listeners_;

    static InMemorySwitchboard new_();
    bool register_listener(std::string addr, rusty::sync::Weak<InMemoryListener> listener) const;
    void unregister_listener(const std::string& addr) const;
    rusty::Option<rusty::Arc<InMemoryListener>> find_listener(const std::string& addr) const;
};


InMemorySwitchboard InMemorySwitchboard::new_() {
    return InMemorySwitchboard{.listeners_ = rusty::Mutex<rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>>::new_(rusty::HashMap<std::string, rusty::sync::Weak<InMemoryListener>>())};
}

bool InMemorySwitchboard::register_listener(std::string addr, rusty::sync::Weak<InMemoryListener> listener) const {
    auto guard = this->listeners_.lock().unwrap();
    if (((*guard)).contains_key(std::move(addr))) {
        return false;
    }
    ((*guard)).insert(std::move(addr), std::move(listener));
    return true;
}

void InMemorySwitchboard::unregister_listener(const std::string& addr) const {
    auto guard = this->listeners_.lock().unwrap();
    ((*guard)).remove(addr);
}

rusty::Option<rusty::Arc<InMemoryListener>> InMemorySwitchboard::find_listener(const std::string& addr) const {
    auto guard = this->listeners_.lock().unwrap();
    auto val_opt = ((*guard)).get(addr);
    if (val_opt.is_none()) {
        return rusty::Option<rusty::Arc<InMemoryListener>>{rusty::None};
    }
    auto upgraded = val_opt.unwrap().upgrade();
    if (upgraded.is_none()) {
        ((*guard)).remove(addr);
        return rusty::Option<rusty::Arc<InMemoryListener>>{rusty::None};
    }
    return std::move(upgraded);
}
/*RUSTYCPP:GEN-END id=inmemory_channel.switchboard*/


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
// `InMemoryConnectionState::inner`'s rusty::Mutex.
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

// rusty::Mutex-owned inner state (rusty-style "data inside the mutex").
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
    inner: rusty::Mutex<InMemoryConnectionStateInner>,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.state version=1 rust_sha256=2674b2babd547611539fad40ab7eaba1a73bbd93f15962b0c506a687a2391ee9*/
struct InMemoryConnectionState;

struct InMemoryConnectionState {
    rusty::Mutex<InMemoryConnectionStateInner> inner;
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
// Authored as inline Rust DSL. The 11 ChannelConnectionBase /
// fault-injection methods stay as free functions (defined further
// down in the file) — their bodies do non-trivial state mutation,
// callback dispatch under the rusty::Mutex, and raw byte slicing that
// don't translate to the DSL grammar. Same free-fn extraction
// pattern as InMemoryListener / InMemorySwitchboard.
struct InMemoryChannel;
ChannelError inmemory_channel_send_frame(const InMemoryChannel& self, const ChannelFrame& f);
void         inmemory_channel_inject_drop_next_sends(const InMemoryChannel& self, int count);
void         inmemory_channel_inject_send_error(const InMemoryChannel& self, ChannelError err, int count);
void         inmemory_channel_clear_fault_injection(const InMemoryChannel& self);


#if RUSTYCPP_RUST
struct InMemoryChannel {
    state_: Arc<InMemoryConnectionState>,
    is_a_side_: bool,
}

impl InMemoryChannel {
    fn new(state: Arc<InMemoryConnectionState>, is_a_side: bool) -> InMemoryChannel {
        InMemoryChannel { state_: state, is_a_side_: is_a_side }
    }

    fn send_frame(&self, f: &ChannelFrame) -> ChannelError {
        inmemory_channel_send_frame(self, f)
    }
    // No buffered output to drain — peer callbacks fire synchronously
    // inside send_frame.
    fn flush(&self) {
    }

    // Idempotent close; notifies the peer's on_closed OUTSIDE the lock.
    // rusty::Mutex::lock() is const-qualified (interior mutability), so
    // no const_cast is needed — the old free-fn's cast was vestigial.
    fn close(&self) {
        let mut peer_on_closed = empty_on_closed_callback();
        let mut fire_peer_closed = false;
        {
            let mut guard = self.state_.inner.lock().unwrap();
            if self.is_a_side_ {
                if (*guard).a_closed {
                    return;
                }
                (*guard).a_closed = true;
                if !(*guard).b_closed {
                    peer_on_closed = (*guard).b_on_closed;
                    fire_peer_closed = true;
                }
            } else {
                if (*guard).b_closed {
                    return;
                }
                (*guard).b_closed = true;
                if !(*guard).a_closed {
                    peer_on_closed = (*guard).a_on_closed;
                    fire_peer_closed = true;
                }
            }
        }
        if fire_peer_closed && peer_on_closed {
            peer_on_closed(ChannelError::None);
        }
    }

    // Closed if EITHER side closed — matches the TCP backend contract
    // ("after is_closed(), send_frame returns a non-None error").
    fn is_closed(&self) -> bool {
        let guard = self.state_.inner.lock().unwrap();
        (*guard).a_closed || (*guard).b_closed
    }

    fn peer_address(&self) -> std::string {
        let guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ { (*guard).b_peer_address } else { (*guard).a_peer_address }
    }

    fn set_on_frame(&self, cb: OnFrameCallback) {
        let mut guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ { (*guard).a_on_frame = cb; } else { (*guard).b_on_frame = cb; }
    }

    fn set_on_closed(&self, cb: OnClosedCallback) {
        let mut guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ { (*guard).a_on_closed = cb; } else { (*guard).b_on_closed = cb; }
    }

    fn set_on_error(&self, cb: OnErrorCallback) {
        let mut guard = self.state_.inner.lock().unwrap();
        if self.is_a_side_ { (*guard).a_on_error = cb; } else { (*guard).b_on_error = cb; }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.channel version=1 rust_sha256=1ee505d9094ee40fb84530c86f83b746c8dad4a466a2cb120f739023a1bc14cf*/
struct InMemoryChannel;

struct InMemoryChannel {
    rusty::Arc<InMemoryConnectionState> state_;
    bool is_a_side_;

    static InMemoryChannel new_(rusty::Arc<InMemoryConnectionState> state, bool is_a_side);
    ChannelError send_frame(const ChannelFrame& f) const;
    void flush() const;
    void close() const;
    bool is_closed() const;
    std::string peer_address() const;
    void set_on_frame(OnFrameCallback cb) const;
    void set_on_closed(OnClosedCallback cb) const;
    void set_on_error(OnErrorCallback cb) const;
};


InMemoryChannel InMemoryChannel::new_(rusty::Arc<InMemoryConnectionState> state, bool is_a_side) {
    return InMemoryChannel{.state_ = std::move(state), .is_a_side_ = std::move(is_a_side)};
}

ChannelError InMemoryChannel::send_frame(const ChannelFrame& f) const {
    return inmemory_channel_send_frame((*this), f);
}

void InMemoryChannel::flush() const {
}

void InMemoryChannel::close() const {
    auto peer_on_closed = empty_on_closed_callback();
    auto fire_peer_closed = false;
    {
        auto guard = (*this->state_).inner.lock().unwrap();
        if (this->is_a_side_) {
            if ((rusty::detail::deref_if_pointer_like(guard)).a_closed) {
                return;
            }
            (rusty::detail::deref_if_pointer_like(guard)).a_closed = true;
            if (rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(guard)).b_closed)) {
                peer_on_closed = (rusty::detail::deref_if_pointer_like(guard)).b_on_closed;
                fire_peer_closed = true;
            }
        } else {
            if ((rusty::detail::deref_if_pointer_like(guard)).b_closed) {
                return;
            }
            (rusty::detail::deref_if_pointer_like(guard)).b_closed = true;
            if (rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(guard)).a_closed)) {
                peer_on_closed = (rusty::detail::deref_if_pointer_like(guard)).a_on_closed;
                fire_peer_closed = true;
            }
        }
    }
    if (rusty::detail::deref_if_pointer_like(fire_peer_closed) && rusty::detail::deref_if_pointer_like(peer_on_closed)) {
        peer_on_closed(ChannelError::None);
    }
}

bool InMemoryChannel::is_closed() const {
    const auto guard = (*this->state_).inner.lock().unwrap();
    return rusty::detail::deref_if_pointer_like((rusty::detail::deref_if_pointer_like(guard)).a_closed) || rusty::detail::deref_if_pointer_like((rusty::detail::deref_if_pointer_like(guard)).b_closed);
}

std::string InMemoryChannel::peer_address() const {
    const auto guard = (*this->state_).inner.lock().unwrap();
    if (this->is_a_side_) {
        return (rusty::detail::deref_if_pointer_like(guard)).b_peer_address;
    } else {
        return (rusty::detail::deref_if_pointer_like(guard)).a_peer_address;
    }
}

void InMemoryChannel::set_on_frame(OnFrameCallback cb) const {
    auto guard = (*this->state_).inner.lock().unwrap();
    if (this->is_a_side_) {
        (rusty::detail::deref_if_pointer_like(guard)).a_on_frame = std::move(cb);
    } else {
        (rusty::detail::deref_if_pointer_like(guard)).b_on_frame = std::move(cb);
    }
}

void InMemoryChannel::set_on_closed(OnClosedCallback cb) const {
    auto guard = (*this->state_).inner.lock().unwrap();
    if (this->is_a_side_) {
        (rusty::detail::deref_if_pointer_like(guard)).a_on_closed = std::move(cb);
    } else {
        (rusty::detail::deref_if_pointer_like(guard)).b_on_closed = std::move(cb);
    }
}

void InMemoryChannel::set_on_error(OnErrorCallback cb) const {
    auto guard = (*this->state_).inner.lock().unwrap();
    if (this->is_a_side_) {
        (rusty::detail::deref_if_pointer_like(guard)).a_on_error = std::move(cb);
    } else {
        (rusty::detail::deref_if_pointer_like(guard)).b_on_error = std::move(cb);
    }
}
/*RUSTYCPP:GEN-END id=inmemory_channel.channel*/

// Free functions (non-DSL) — see definitions further down. Each
// inlines the `const_cast<InMemoryConnectionState&>(*self.state_.
// get())` pattern that the legacy `const_cast<InMemoryConnectionState&>(*self.state_.get())` helper performed.
// ChannelConnectionBase methods.
// Fault injection (test-only).
// `InMemoryChannelShim` — Arc-holding ChannelConnectionBase
// implementor (the tcp_channel shim recipe; no const_cast idiom).
#if RUSTYCPP_RUST
struct InMemoryChannelShim {
    conn_: Arc<InMemoryChannel>,
}

#[cpp_inherit]
impl ChannelConnectionBase for InMemoryChannelShim {
    fn send_frame(&mut self, f: &ChannelFrame) -> ChannelError {
        self.conn_.send_frame(f)
    }
    fn flush(&mut self) {
        self.conn_.flush()
    }
    fn close(&mut self) {
        self.conn_.close()
    }
    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }
    fn peer_address(&self) -> std::string {
        self.conn_.peer_address()
    }
    fn set_on_frame(&mut self, cb: OnFrameCallback) {
        self.conn_.set_on_frame(cb)
    }
    fn set_on_closed(&mut self, cb: OnClosedCallback) {
        self.conn_.set_on_closed(cb)
    }
    fn set_on_error(&mut self, cb: OnErrorCallback) {
        self.conn_.set_on_error(cb)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.channel_shim version=1 rust_sha256=37e409e9ff85357b15cbc068e6708fda8c09eef387cde4d96851daeb73b93dee*/
struct InMemoryChannelShim;

struct InMemoryChannelShim : public ChannelConnectionBase {
    rusty::Arc<InMemoryChannel> conn_;
    InMemoryChannelShim(rusty::Arc<InMemoryChannel> conn__init) : ChannelConnectionBase(), conn_(std::move(conn__init)) {}
    InMemoryChannelShim(InMemoryChannelShim&& other) noexcept : ChannelConnectionBase(), conn_(std::move(other.conn_)) {}


    ChannelError send_frame(const ChannelFrame& f);
    void flush();
    void close();
    bool is_closed() const;
    std::string peer_address() const;
    void set_on_frame(OnFrameCallback cb);
    void set_on_closed(OnClosedCallback cb);
    void set_on_error(OnErrorCallback cb);
};


ChannelError InMemoryChannelShim::send_frame(const ChannelFrame& f) {
    return this->conn_->send_frame(f);
}

void InMemoryChannelShim::flush() {
    this->conn_->flush();
}

void InMemoryChannelShim::close() {
    this->conn_->close();
}

bool InMemoryChannelShim::is_closed() const {
    return this->conn_->is_closed();
}

std::string InMemoryChannelShim::peer_address() const {
    return this->conn_->peer_address();
}

void InMemoryChannelShim::set_on_frame(OnFrameCallback cb) {
    this->conn_->set_on_frame(std::move(cb));
}

void InMemoryChannelShim::set_on_closed(OnClosedCallback cb) {
    this->conn_->set_on_closed(std::move(cb));
}

void InMemoryChannelShim::set_on_error(OnErrorCallback cb) {
    this->conn_->set_on_error(std::move(cb));
}
/*RUSTYCPP:GEN-END id=inmemory_channel.channel_shim*/

inline ChannelConnectionProxy make_inmemory_channel_proxy(
        rusty::Arc<InMemoryChannel> conn) {
    return rusty::make_box<InMemoryChannelShim>(std::move(conn));
}

// ---------------------------------------------------------------------------
// InMemoryListener
// ---------------------------------------------------------------------------

// rusty::Mutex-owned mutable state for InMemoryListener (rusty-style
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
struct InMemoryListener;
rusty::Option<rusty::Arc<InMemoryChannel>> inmemory_listener_accept_for_connect(const InMemoryListener& self, const std::string& client_address);

#if RUSTYCPP_RUST
struct InMemoryListener {
    switchboard_: Arc<InMemorySwitchboard>,
    self_weak_: rusty::Option<rusty::sync::Weak<InMemoryListener>>,
    inner_: rusty::Mutex<InMemoryListenerInnerState>,
}

impl InMemoryListener {
    fn new(switchboard: Arc<InMemorySwitchboard>) -> InMemoryListener {
        InMemoryListener {
            switchboard_: switchboard,
            self_weak_: rusty::None,
            inner_: rusty::Mutex::<InMemoryListenerInnerState>::new(InMemoryListenerInnerState{}),
        }
    }

    // Bind to `addr`: claim it in the listener state under the lock, then
    // register with the switchboard OUTSIDE the lock (its own mutex; keeps
    // the original lock ordering). Rolls back the claim on collision.
    fn listen(&self, addr: std::string_view) -> ChannelError {
        let mut w = empty_listener_weak();
        {
            let mut guard = self.inner_.lock().unwrap();
            if (*guard).closed {
                return ChannelError_Internal();
            }
            if !(*guard).local_address.empty() {
                if (*guard).local_address == std::string(addr) {
                    return ChannelError_None();
                }
                return ChannelError_AddressInUse();
            }
            if self.self_weak_.is_none() {
                Log_error("rrr::InMemoryListener::listen: self_weak_ not set (caller must call set_self_weak before listen)");
                return ChannelError_Internal();
            }
            (*guard).local_address = std::string(addr);
            w = self.self_weak_.as_ref().unwrap().clone();
        }
        if !self.switchboard_.register_listener(std::string(addr), w) {
            let mut guard = self.inner_.lock().unwrap();
            (*guard).local_address.clear();
            return ChannelError_AddressInUse();
        }
        ChannelError_None()
    }

    // Idempotent; unregisters from the switchboard outside the lock.
    fn close(&self) {
        let mut addr_to_unregister = std::string();
        {
            let mut guard = self.inner_.lock().unwrap();
            if (*guard).closed {
                return;
            }
            (*guard).closed = true;
            addr_to_unregister = (*guard).local_address;
        }
        if !addr_to_unregister.empty() {
            self.switchboard_.unregister_listener(addr_to_unregister);
        }
    }

    fn is_closed(&self) -> bool {
        let guard = self.inner_.lock().unwrap();
        (*guard).closed
    }

    fn local_address(&self) -> std::string {
        let guard = self.inner_.lock().unwrap();
        (*guard).local_address
    }

    fn set_on_accept(&self, cb: OnAcceptCallback) {
        let mut guard = self.inner_.lock().unwrap();
        (*guard).on_accept = cb;
    }

    fn set_on_error(&self, cb: OnErrorCallback) {
        let mut guard = self.inner_.lock().unwrap();
        (*guard).on_error = cb;
    }

    fn set_self_weak(&mut self, w: rusty::sync::Weak<InMemoryListener>) {
        self.self_weak_ = rusty::Some(w);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.listener version=1 rust_sha256=633a52629cffdbbdd512df2ef14021581247305951dd5637146cc50ddb16cb0c*/
struct InMemoryListener;

struct InMemoryListener {
    rusty::Arc<InMemorySwitchboard> switchboard_;
    rusty::Option<rusty::sync::Weak<InMemoryListener>> self_weak_;
    rusty::Mutex<InMemoryListenerInnerState> inner_;

    static InMemoryListener new_(rusty::Arc<InMemorySwitchboard> switchboard);
    ChannelError listen(std::string_view addr) const;
    void close() const;
    bool is_closed() const;
    std::string local_address() const;
    void set_on_accept(OnAcceptCallback cb) const;
    void set_on_error(OnErrorCallback cb) const;
    void set_self_weak(rusty::sync::Weak<InMemoryListener> w);
};


InMemoryListener InMemoryListener::new_(rusty::Arc<InMemorySwitchboard> switchboard) {
    return InMemoryListener{.switchboard_ = std::move(switchboard), .self_weak_ = rusty::None, .inner_ = rusty::Mutex<InMemoryListenerInnerState>::new_(InMemoryListenerInnerState{})};
}

ChannelError InMemoryListener::listen(std::string_view addr) const {
    auto w = empty_listener_weak();
    {
        auto guard = this->inner_.lock().unwrap();
        if ((*guard).closed) {
            return ChannelError_Internal();
        }
        if (rusty::detail::rust_not((*guard).local_address.empty())) {
            if (rusty::detail::deref_if_pointer_like((*guard).local_address) == std::string(std::move(addr))) {
                return ChannelError_None();
            }
            return ChannelError_AddressInUse();
        }
        if (this->self_weak_.is_none()) {
            Log_error("rrr::InMemoryListener::listen: self_weak_ not set (caller must call set_self_weak before listen)");
            return ChannelError_Internal();
        }
        (*guard).local_address = std::string(std::move(addr));
        w = rusty::clone(this->self_weak_.as_ref().unwrap());
    }
    if (rusty::detail::rust_not(this->switchboard_->register_listener(std::string(std::move(addr)), std::move(w)))) {
        auto guard = this->inner_.lock().unwrap();
        (*guard).local_address.clear();
        return ChannelError_AddressInUse();
    }
    return ChannelError_None();
}

void InMemoryListener::close() const {
    auto addr_to_unregister = std::string();
    {
        auto guard = this->inner_.lock().unwrap();
        if ((*guard).closed) {
            return;
        }
        (*guard).closed = true;
        addr_to_unregister = (*guard).local_address;
    }
    if (rusty::detail::rust_not(addr_to_unregister.empty())) {
        this->switchboard_->unregister_listener(std::move(addr_to_unregister));
    }
}

bool InMemoryListener::is_closed() const {
    auto guard = this->inner_.lock().unwrap();
    return (*guard).closed;
}

std::string InMemoryListener::local_address() const {
    auto guard = this->inner_.lock().unwrap();
    return (*guard).local_address;
}

void InMemoryListener::set_on_accept(OnAcceptCallback cb) const {
    auto guard = this->inner_.lock().unwrap();
    (*guard).on_accept = std::move(cb);
}

void InMemoryListener::set_on_error(OnErrorCallback cb) const {
    auto guard = this->inner_.lock().unwrap();
    (*guard).on_error = std::move(cb);
}

void InMemoryListener::set_self_weak(rusty::sync::Weak<InMemoryListener> w) {
    this->self_weak_ = rusty::Option<rusty::sync::Weak<InMemoryListener>>(std::move(w));
}
/*RUSTYCPP:GEN-END id=inmemory_channel.listener*/

// Free functions (non-DSL) — see definitions further down.
// `InMemoryListenerShim` — Arc-holding ChannelListenerBase implementor.
#if RUSTYCPP_RUST
struct InMemoryListenerShim {
    listener_: Arc<InMemoryListener>,
}

#[cpp_inherit]
impl ChannelListenerBase for InMemoryListenerShim {
    fn listen(&mut self, addr: std::string_view) -> ChannelError {
        self.listener_.listen(addr)
    }
    fn close(&mut self) {
        self.listener_.close()
    }
    fn is_closed(&self) -> bool {
        self.listener_.is_closed()
    }
    fn local_address(&self) -> std::string {
        self.listener_.local_address()
    }
    fn set_on_accept(&mut self, cb: OnAcceptCallback) {
        self.listener_.set_on_accept(cb)
    }
    fn set_on_error(&mut self, cb: OnErrorCallback) {
        self.listener_.set_on_error(cb)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.listener_shim version=1 rust_sha256=3a449bef6164ababe77eda44b5a3616d4927690e7902448fd49aa658089a0f35*/
struct InMemoryListenerShim;

struct InMemoryListenerShim : public ChannelListenerBase {
    rusty::Arc<InMemoryListener> listener_;
    InMemoryListenerShim(rusty::Arc<InMemoryListener> listener__init) : ChannelListenerBase(), listener_(std::move(listener__init)) {}
    InMemoryListenerShim(InMemoryListenerShim&& other) noexcept : ChannelListenerBase(), listener_(std::move(other.listener_)) {}


    ChannelError listen(std::string_view addr);
    void close();
    bool is_closed() const;
    std::string local_address() const;
    void set_on_accept(OnAcceptCallback cb);
    void set_on_error(OnErrorCallback cb);
};


ChannelError InMemoryListenerShim::listen(std::string_view addr) {
    return this->listener_->listen(std::move(rusty::to_string_view(addr)));
}

void InMemoryListenerShim::close() {
    this->listener_->close();
}

bool InMemoryListenerShim::is_closed() const {
    return this->listener_->is_closed();
}

std::string InMemoryListenerShim::local_address() const {
    return this->listener_->local_address();
}

void InMemoryListenerShim::set_on_accept(OnAcceptCallback cb) {
    this->listener_->set_on_accept(std::move(cb));
}

void InMemoryListenerShim::set_on_error(OnErrorCallback cb) {
    this->listener_->set_on_error(std::move(cb));
}
/*RUSTYCPP:GEN-END id=inmemory_channel.listener_shim*/

inline ChannelListenerProxy make_inmemory_listener_proxy(
        rusty::Arc<InMemoryListener> listener) {
    return rusty::make_box<InMemoryListenerShim>(std::move(listener));
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
struct InMemoryFactory;
ConnectResult                       inmemory_factory_connect(const InMemoryFactory& self, std::string_view addr);
rusty::Option<ChannelListenerProxy> inmemory_factory_make_listener(const InMemoryFactory& self);

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

    fn connect(&self, addr: std::string_view) -> ConnectResult {
        inmemory_factory_connect(self, addr)
    }
    fn make_listener(&self) -> Option<ChannelListenerProxy> {
        inmemory_factory_make_listener(self)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.factory version=1 rust_sha256=84bc41b442e3a61a85011952e3286b1862aa4264e9579d5c76e2d059aedb9895*/
struct InMemoryFactory;

struct InMemoryFactory {
    rusty::Arc<InMemorySwitchboard> switchboard_;

    static InMemoryFactory new_(rusty::Arc<InMemorySwitchboard> switchboard);
    std::string backend_name() const;
    ConnectResult connect(std::string_view addr) const;
    rusty::Option<ChannelListenerProxy> make_listener() const;
};


InMemoryFactory InMemoryFactory::new_(rusty::Arc<InMemorySwitchboard> switchboard) {
    return InMemoryFactory{.switchboard_ = std::move(switchboard)};
}

std::string InMemoryFactory::backend_name() const {
    return std::string("inmemory");
}

ConnectResult InMemoryFactory::connect(std::string_view addr) const {
    return inmemory_factory_connect((*this), std::move(addr));
}

rusty::Option<ChannelListenerProxy> InMemoryFactory::make_listener() const {
    return inmemory_factory_make_listener((*this));
}
/*RUSTYCPP:GEN-END id=inmemory_channel.factory*/

// Free functions (non-DSL) — see definitions further down.
// `InMemoryFactoryShim` — Arc-holding ChannelFactoryBase implementor.
#if RUSTYCPP_RUST
struct InMemoryFactoryShim {
    factory_: Arc<InMemoryFactory>,
}

#[cpp_inherit]
impl ChannelFactoryBase for InMemoryFactoryShim {
    fn connect(&mut self, addr: std::string_view) -> ConnectResult {
        self.factory_.connect(addr)
    }
    fn make_listener(&mut self) -> Option<ChannelListenerProxy> {
        self.factory_.make_listener()
    }
    fn backend_name(&self) -> std::string {
        self.factory_.backend_name()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=inmemory_channel.factory_shim version=1 rust_sha256=9eb2534f896c904a27534805c79152c0ee305a82995a331bd38f19749bdda658*/
struct InMemoryFactoryShim;

struct InMemoryFactoryShim : public ChannelFactoryBase {
    rusty::Arc<InMemoryFactory> factory_;
    InMemoryFactoryShim(rusty::Arc<InMemoryFactory> factory__init) : ChannelFactoryBase(), factory_(std::move(factory__init)) {}
    InMemoryFactoryShim(InMemoryFactoryShim&& other) noexcept : ChannelFactoryBase(), factory_(std::move(other.factory_)) {}


    ConnectResult connect(std::string_view addr);
    rusty::Option<ChannelListenerProxy> make_listener();
    std::string backend_name() const;
};


ConnectResult InMemoryFactoryShim::connect(std::string_view addr) {
    return this->factory_->connect(std::move(rusty::to_string_view(addr)));
}

rusty::Option<ChannelListenerProxy> InMemoryFactoryShim::make_listener() {
    return this->factory_->make_listener();
}

std::string InMemoryFactoryShim::backend_name() const {
    return this->factory_->backend_name();
}
/*RUSTYCPP:GEN-END id=inmemory_channel.factory_shim*/

inline ChannelFactoryProxy make_inmemory_factory_proxy(
        rusty::Arc<InMemoryFactory> factory) {
    return rusty::make_box<InMemoryFactoryShim>(std::move(factory));
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
// rusty::Mutex + HashMap + Weak::upgrade and inherit @safe. InMemoryChannel
// out-of-class defs route through const_cast<InMemoryConnectionState&>(*self.state_.get()) (@unsafe) so each carries
// a per-method `// @unsafe`. InMemoryListener::accept_for_connect,
// InMemoryFactory::connect/make_listener, and the test helper
// make_channel_pair_for_testing const_cast inline and are `// @unsafe`.
namespace rrr {

// ---------------------------------------------------------------------------
// InMemorySwitchboard
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// InMemoryChannel
// ---------------------------------------------------------------------------

// @unsafe - const_cast<InMemoryConnectionState&>(*self.state_.get()) const_cast + raw `uint8_t*` byte slicing
// (`bytes.assign(f.payload, f.payload + f.size)` then `bytes.data()`).
ChannelError inmemory_channel_send_frame(const InMemoryChannel& self, const ChannelFrame& f) {
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
        auto guard = const_cast<InMemoryConnectionState&>(*self.state_.get()).inner.lock().unwrap();
        if (self.is_a_side_) {
            self_already_closed = (*guard).a_closed;
            peer_already_closed = (*guard).b_closed;
            peer_on_frame       = (*guard).b_on_frame;
            if ((*guard).drop_next_sends_a > 0) {
                drop_this_send = true;
                --(*guard).drop_next_sends_a;
            } else if ((*guard).send_error_count_a > 0) {
                inject_error  = true;
                injected_err  = (*guard).send_error_a;
                --(*guard).send_error_count_a;
            }
        } else {
            self_already_closed = (*guard).b_closed;
            peer_already_closed = (*guard).a_closed;
            peer_on_frame       = (*guard).a_on_frame;
            if ((*guard).drop_next_sends_b > 0) {
                drop_this_send = true;
                --(*guard).drop_next_sends_b;
            } else if ((*guard).send_error_count_b > 0) {
                inject_error  = true;
                injected_err  = (*guard).send_error_b;
                --(*guard).send_error_count_b;
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

// @unsafe - const_cast<InMemoryConnectionState&>(*self.state_.get()) const_cast.
void inmemory_channel_inject_drop_next_sends(const InMemoryChannel& self, int count) {
    auto guard = const_cast<InMemoryConnectionState&>(*self.state_.get()).inner.lock().unwrap();
    if (self.is_a_side_) {
        (*guard).drop_next_sends_a = count;
    } else {
        (*guard).drop_next_sends_b = count;
    }
}

// @unsafe - const_cast<InMemoryConnectionState&>(*self.state_.get()) const_cast.
void inmemory_channel_inject_send_error(const InMemoryChannel& self, ChannelError err, int count) {
    auto guard = const_cast<InMemoryConnectionState&>(*self.state_.get()).inner.lock().unwrap();
    if (self.is_a_side_) {
        (*guard).send_error_a       = err;
        (*guard).send_error_count_a = count;
    } else {
        (*guard).send_error_b       = err;
        (*guard).send_error_count_b = count;
    }
}

// @unsafe - const_cast<InMemoryConnectionState&>(*self.state_.get()) const_cast.
void inmemory_channel_clear_fault_injection(const InMemoryChannel& self) {
    auto guard = const_cast<InMemoryConnectionState&>(*self.state_.get()).inner.lock().unwrap();
    if (self.is_a_side_) {
        (*guard).drop_next_sends_a  = 0;
        (*guard).send_error_count_a = 0;
        (*guard).send_error_a       = ChannelError::None;
    } else {
        (*guard).drop_next_sends_b  = 0;
        (*guard).send_error_count_b = 0;
        (*guard).send_error_b       = ChannelError::None;
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

// ---------------------------------------------------------------------------
// InMemoryListener
// ---------------------------------------------------------------------------

// @unsafe - inline `const_cast<InMemoryConnectionState*>(state.get())`
// to bootstrap the shared connection state before the per-side Arcs
// are constructed.
rusty::Option<rusty::Arc<InMemoryChannel>>
inmemory_listener_accept_for_connect(const InMemoryListener& self, const std::string& client_address) {
    OnAcceptCallback cb_to_fire;
    std::string server_address;
    {
        auto guard = self.inner_.lock().unwrap();
        if ((*guard).closed || (*guard).local_address.empty()) {
            return rusty::None;
        }
        cb_to_fire = (*guard).on_accept;  // wrapper copy = Arc clone (refcount++)
        server_address = (*guard).local_address;
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
    // rusty::Mutex has no default ctor (unlike the retired SpinMutex), so build
    // the state with an explicitly value-initialized inner instead of make().
    auto state = rusty::Arc<InMemoryConnectionState>::new_(InMemoryConnectionState{
        .inner = rusty::Mutex<InMemoryConnectionStateInner>::new_(InMemoryConnectionStateInner{})});
    {
        // No external mutators yet — but the rusty::Mutex-owned inner
        // requires going through the lock guard for any access.
        auto* mut_state = const_cast<InMemoryConnectionState*>(state.get());
        auto guard = mut_state->inner.lock().unwrap();
        (*guard).a_peer_address = client_address;  // A is the client
        (*guard).b_peer_address = server_address;  // B is the server
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
ConnectResult inmemory_factory_connect(const InMemoryFactory& self, std::string_view addr) {
    std::string addr_str(addr);
    auto listener_opt = (*self.switchboard_.get()).find_listener(addr_str);
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
    // rusty::Mutex has no default ctor (unlike the retired SpinMutex), so build
    // the state with an explicitly value-initialized inner instead of make().
    auto state = rusty::Arc<InMemoryConnectionState>::new_(InMemoryConnectionState{
        .inner = rusty::Mutex<InMemoryConnectionStateInner>::new_(InMemoryConnectionStateInner{})});
    {
        auto* mut_state = const_cast<InMemoryConnectionState*>(state.get());
        auto guard = mut_state->inner.lock().unwrap();
        (*guard).a_peer_address = std::move(a_addr);
        (*guard).b_peer_address = std::move(b_addr);
    }
    auto a_side = rusty::Arc<InMemoryChannel>::new_(InMemoryChannel::new_(state.clone(), /*is_a_side=*/true));
    auto b_side = rusty::Arc<InMemoryChannel>::new_(InMemoryChannel::new_(state.clone(), /*is_a_side=*/false));
    return {std::move(a_side), std::move(b_side)};
}

// @unsafe - inline `const_cast<InMemoryListener&>(*listener.get())` to
// wire `self_weak_` before publishing the listener.
rusty::Option<ChannelListenerProxy> inmemory_factory_make_listener(const InMemoryFactory& self) {
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
