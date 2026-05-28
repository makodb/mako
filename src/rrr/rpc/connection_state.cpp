module;

#include <stdint.h>
#include <rusty/cell.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/rusty.hpp>

export module rrr.connection_state;

import std;

export namespace rrr {

enum class ConnectionState : int {
    NEW = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    DISCONNECTING = 3,
    DISCONNECTED = 4,
    FAILED = 5
};

inline const char* connection_state_to_string(ConnectionState state) {
    switch (state) {
        case ConnectionState::NEW: return "NEW";
        case ConnectionState::CONNECTING: return "CONNECTING";
        case ConnectionState::CONNECTED: return "CONNECTED";
        case ConnectionState::DISCONNECTING: return "DISCONNECTING";
        case ConnectionState::DISCONNECTED: return "DISCONNECTED";
        case ConnectionState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

// Free helpers backing the pure state-classifier predicates on
// `ConnectionStateMachine` (and the central transition table). Each
// helper takes the raw integer discriminant of `ConnectionState`
// (NEW=0..FAILED=5); the C++ member methods cast at the boundary.
// Authored as inline Rust DSL.
#if RUSTYCPP_RUST
fn connection_is_valid_transition(from: i32, to: i32) -> bool {
    if from == 0 {
        // NEW → CONNECTING
        return to == 1;
    }
    if from == 1 {
        // CONNECTING → CONNECTED | FAILED | DISCONNECTED
        return to == 2 || to == 5 || to == 4;
    }
    if from == 2 {
        // CONNECTED → DISCONNECTING | FAILED
        return to == 3 || to == 5;
    }
    if from == 3 {
        // DISCONNECTING → DISCONNECTED | FAILED
        return to == 4 || to == 5;
    }
    if from == 4 {
        // DISCONNECTED → CONNECTING
        return to == 1;
    }
    if from == 5 {
        // FAILED → CONNECTING
        return to == 1;
    }
    false
}

fn connection_is_terminal(s: i32) -> bool {
    s == 4 || s == 5
}

fn connection_can_connect(s: i32) -> bool {
    s == 0 || s == 4 || s == 5
}

fn connection_is_usable(s: i32) -> bool {
    s == 1 || s == 2
}
#endif
/*RUSTYCPP:GEN-BEGIN id=connection_state.1 version=1 rust_sha256=efa3b250b9497ffd63a43664a7493db5719f3bf70c23bf0531de70907c04fe51*/
bool connection_is_valid_transition(int32_t from, int32_t to);
bool connection_is_terminal(int32_t s);
bool connection_can_connect(int32_t s);
bool connection_is_usable(int32_t s);

bool connection_is_valid_transition(int32_t from, int32_t to) {
    if (rusty::detail::deref_if_pointer_like(from) == static_cast<int32_t>(0)) {
        return rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(1);
    }
    if (rusty::detail::deref_if_pointer_like(from) == static_cast<int32_t>(1)) {
        return ((rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(2)) || (rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(5))) || (rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(4));
    }
    if (rusty::detail::deref_if_pointer_like(from) == static_cast<int32_t>(2)) {
        return (rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(3)) || (rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(5));
    }
    if (rusty::detail::deref_if_pointer_like(from) == static_cast<int32_t>(3)) {
        return (rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(4)) || (rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(5));
    }
    if (rusty::detail::deref_if_pointer_like(from) == static_cast<int32_t>(4)) {
        return rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(1);
    }
    if (rusty::detail::deref_if_pointer_like(from) == static_cast<int32_t>(5)) {
        return rusty::detail::deref_if_pointer_like(to) == static_cast<int32_t>(1);
    }
    return false;
}

bool connection_is_terminal(int32_t s) {
    return (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(4)) || (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(5));
}

bool connection_can_connect(int32_t s) {
    return ((rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(0)) || (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(4))) || (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(5));
}

bool connection_is_usable(int32_t s) {
    return (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(1)) || (rusty::detail::deref_if_pointer_like(s) == static_cast<int32_t>(2));
}
/*RUSTYCPP:GEN-END id=connection_state.1*/

// @safe - Pure state machine: rusty::Cell<ConnectionState> + rusty::Function
// callback. No raw pointers, syscalls, or operator-overload chains.
class ConnectionStateMachine {
private:
    rusty::Cell<ConnectionState> state_{ConnectionState::NEW};
    // mutable: state-change callback registration happens through a
    // const-callable setter; the body uses rusty::Function move-assign
    // (no extra synchronization needed because set_on_state_change is
    // called once at setup time and not concurrent with the firings).
    mutable rusty::Function<void(ConnectionState, ConnectionState)> on_state_change_;

public:
    ConnectionStateMachine() = default;

    explicit ConnectionStateMachine(ConnectionState initial_state)
        : state_(initial_state) {}

    ConnectionStateMachine(const ConnectionStateMachine&) = delete;
    ConnectionStateMachine& operator=(const ConnectionStateMachine&) = delete;

    ConnectionStateMachine(ConnectionStateMachine&&) = default;
    ConnectionStateMachine& operator=(ConnectionStateMachine&&) = default;

    ConnectionState state() const {
        return state_.get();
    }

    bool can_transition_to(ConnectionState new_state) const {
        ConnectionState current = state_.get();
        return is_valid_transition(current, new_state);
    }

    // const: state_ is rusty::Cell (interior-mutable); on_state_change_
    // is mutable. The body's only writes are state_.set(...) and the
    // callback invocation, both safe on a const StateMachine.
    bool transition_to(ConnectionState new_state) const {
        ConnectionState current = state_.get();

        if (!is_valid_transition(current, new_state)) {
            return false;
        }

        state_.set(new_state);

        if (on_state_change_) {
            on_state_change_(current, new_state);
        }

        return true;
    }

    // const: same reason as transition_to.
    void force_state(ConnectionState new_state) const {
        ConnectionState current = state_.get();
        state_.set(new_state);

        if (on_state_change_) {
            on_state_change_(current, new_state);
        }
    }

    // const: on_state_change_ is mutable; one-shot registration at setup.
    void set_on_state_change(
        rusty::Function<void(ConnectionState, ConnectionState)> callback) const {
        on_state_change_ = std::move(callback);
    }

    bool is_connected() const {
        return state_.get() == ConnectionState::CONNECTED;
    }

    bool is_failed() const {
        return state_.get() == ConnectionState::FAILED;
    }

    bool is_terminal() const {
        return connection_is_terminal(static_cast<int32_t>(state_.get()));
    }

    bool can_connect() const {
        return connection_can_connect(static_cast<int32_t>(state_.get()));
    }

    bool is_usable() const {
        return connection_is_usable(static_cast<int32_t>(state_.get()));
    }

private:
    static bool is_valid_transition(ConnectionState from, ConnectionState to) {
        return connection_is_valid_transition(
            static_cast<int32_t>(from), static_cast<int32_t>(to));
    }
};

} // export namespace rrr
