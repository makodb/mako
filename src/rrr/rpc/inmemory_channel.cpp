// Workstream K, leaf 6a — in-memory channel backend implementation.

#include <std_compat.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/weak.hpp>

#include "inmemory_channel.hpp"

#include "../base/all.hpp"  // Log_*

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
    // Upgrade through the pointer before any mutation invalidates it.
    auto upgraded = val_opt.unwrap()->upgrade();
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

ChannelError InMemoryChannel::send_frame(const ChannelFrame& f) {
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
        std::lock_guard<std::mutex> lk(mut_state().mu);
        auto& st = mut_state();
        if (is_a_side_) {
            self_already_closed = st.a_closed;
            peer_already_closed = st.b_closed;
            peer_on_frame       = st.b_on_frame;
            if (st.drop_next_sends_a > 0) {
                drop_this_send = true;
                --st.drop_next_sends_a;
            } else if (st.send_error_count_a > 0) {
                inject_error  = true;
                injected_err  = st.send_error_a;
                --st.send_error_count_a;
            }
        } else {
            self_already_closed = st.b_closed;
            peer_already_closed = st.a_closed;
            peer_on_frame       = st.a_on_frame;
            if (st.drop_next_sends_b > 0) {
                drop_this_send = true;
                --st.drop_next_sends_b;
            } else if (st.send_error_count_b > 0) {
                inject_error  = true;
                injected_err  = st.send_error_b;
                --st.send_error_count_b;
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

void InMemoryChannel::inject_drop_next_sends(int count) {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    if (is_a_side_) {
        mut_state().drop_next_sends_a = count;
    } else {
        mut_state().drop_next_sends_b = count;
    }
}

void InMemoryChannel::inject_send_error(ChannelError err, int count) {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    if (is_a_side_) {
        mut_state().send_error_a       = err;
        mut_state().send_error_count_a = count;
    } else {
        mut_state().send_error_b       = err;
        mut_state().send_error_count_b = count;
    }
}

void InMemoryChannel::clear_fault_injection() {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    if (is_a_side_) {
        mut_state().drop_next_sends_a  = 0;
        mut_state().send_error_count_a = 0;
        mut_state().send_error_a       = ChannelError::None;
    } else {
        mut_state().drop_next_sends_b  = 0;
        mut_state().send_error_count_b = 0;
        mut_state().send_error_b       = ChannelError::None;
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
void InMemoryChannel::close() {
    OnClosedCallback peer_on_closed;
    bool fire_peer_closed = false;
    {
        std::lock_guard<std::mutex> lk(mut_state().mu);
        if (is_a_side_) {
            if (mut_state().a_closed) return;  // idempotent
            mut_state().a_closed = true;
            // Notify peer if it's not already closed.
            if (!mut_state().b_closed) {
                peer_on_closed = mut_state().b_on_closed;
                fire_peer_closed = true;
            }
        } else {
            if (mut_state().b_closed) return;
            mut_state().b_closed = true;
            if (!mut_state().a_closed) {
                peer_on_closed = mut_state().a_on_closed;
                fire_peer_closed = true;
            }
        }
    }
    if (fire_peer_closed && peer_on_closed) {
        peer_on_closed(ChannelError::None);
    }
}

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
    std::lock_guard<std::mutex> lk(mut_state().mu);
    return mut_state().a_closed || mut_state().b_closed;
}

std::string InMemoryChannel::peer_address() const {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    return is_a_side_ ? mut_state().b_peer_address : mut_state().a_peer_address;
}

void InMemoryChannel::set_on_frame(OnFrameCallback cb) {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    if (is_a_side_) mut_state().a_on_frame  = std::move(cb);
    else            mut_state().b_on_frame  = std::move(cb);
}

void InMemoryChannel::set_on_closed(OnClosedCallback cb) {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    if (is_a_side_) mut_state().a_on_closed = std::move(cb);
    else            mut_state().b_on_closed = std::move(cb);
}

void InMemoryChannel::set_on_error(OnErrorCallback cb) {
    std::lock_guard<std::mutex> lk(mut_state().mu);
    if (is_a_side_) mut_state().a_on_error  = std::move(cb);
    else            mut_state().b_on_error  = std::move(cb);
}

// ---------------------------------------------------------------------------
// InMemoryListener
// ---------------------------------------------------------------------------

ChannelError InMemoryListener::listen(std::string_view addr) {
    rusty::sync::Weak<InMemoryListener> w;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) {
            return ChannelError::Internal;
        }
        if (!local_address_.empty()) {
            // Already listening; treat as idempotent if same addr.
            if (local_address_ == std::string(addr)) {
                return ChannelError::None;
            }
            return ChannelError::AddressInUse;
        }
        if (self_weak_.is_none()) {
            Log_error("rrr::InMemoryListener::listen: self_weak_ not set "
                      "(caller must call set_self_weak before listen)");
            return ChannelError::Internal;
        }
        local_address_ = std::string(addr);
        w = self_weak_.as_ref().unwrap().clone();
    }
    if (!switchboard_->register_listener(std::string(addr), std::move(w))) {
        // Address already taken in the switchboard.
        std::lock_guard<std::mutex> lk(mu_);
        local_address_.clear();
        return ChannelError::AddressInUse;
    }
    return ChannelError::None;
}

void InMemoryListener::close() {
    std::string addr_to_unregister;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        closed_ = true;
        addr_to_unregister = local_address_;
    }
    if (!addr_to_unregister.empty()) {
        switchboard_->unregister_listener(addr_to_unregister);
    }
}

bool InMemoryListener::is_closed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_;
}

std::string InMemoryListener::local_address() const {
    std::lock_guard<std::mutex> lk(mu_);
    return local_address_;
}

void InMemoryListener::set_on_accept(OnAcceptCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_accept_ = std::move(cb);
}

void InMemoryListener::set_on_error(OnErrorCallback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_error_ = std::move(cb);
}

rusty::Option<rusty::Arc<InMemoryChannel>>
InMemoryListener::accept_for_connect(const std::string& client_address) {
    OnAcceptCallback cb_to_fire;
    std::string server_address;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_ || local_address_.empty()) {
            return rusty::None;
        }
        cb_to_fire = on_accept_;
        server_address = local_address_;
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
        // No external mutators yet — safe to write directly here.
        auto* mut_state = const_cast<InMemoryConnectionState*>(state.get());
        mut_state->a_peer_address = client_address;  // A is the client
        mut_state->b_peer_address = server_address;  // B is the server
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

ConnectResult InMemoryFactory::connect(std::string_view addr) {
    std::string addr_str(addr);
    auto listener_opt = switchboard_->find_listener(addr_str);
    if (listener_opt.is_none()) {
        return ConnectResult{ChannelConnectionProxy{},
                             ChannelError::ConnectionRefused};
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
        return ConnectResult{ChannelConnectionProxy{},
                             ChannelError::ConnectionRefused};
    }
    auto client_side = client_opt.unwrap();
    return ConnectResult{
        make_inmemory_channel_proxy(std::move(client_side)),
        ChannelError::None,
    };
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

std::pair<rusty::Arc<InMemoryChannel>, rusty::Arc<InMemoryChannel>>
make_channel_pair_for_testing(std::string a_addr, std::string b_addr) {
    auto state = rusty::Arc<InMemoryConnectionState>::make();
    {
        auto* mut_state = const_cast<InMemoryConnectionState*>(state.get());
        mut_state->a_peer_address = std::move(a_addr);
        mut_state->b_peer_address = std::move(b_addr);
    }
    auto a_side = rusty::Arc<InMemoryChannel>::make(state.clone(), /*is_a_side=*/true);
    auto b_side = rusty::Arc<InMemoryChannel>::make(state.clone(), /*is_a_side=*/false);
    return {std::move(a_side), std::move(b_side)};
}

ChannelListenerProxy InMemoryFactory::make_listener() {
    auto listener = rusty::Arc<InMemoryListener>::make(switchboard_);
    // Wire the self-weak so the listener can register itself in the
    // switchboard. Mirrors TcpFactory::make_listener.
    {
        auto& mut_l = const_cast<InMemoryListener&>(*listener.get());
        mut_l.set_self_weak(rusty::sync::downgrade(listener));
    }
    return make_inmemory_listener_proxy(std::move(listener));
}

}  // namespace rrr
