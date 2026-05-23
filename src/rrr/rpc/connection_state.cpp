module;

#include <rusty/cell.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>

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
        ConnectionState s = state_.get();
        return s == ConnectionState::DISCONNECTED || s == ConnectionState::FAILED;
    }

    bool can_connect() const {
        ConnectionState s = state_.get();
        return s == ConnectionState::NEW ||
               s == ConnectionState::DISCONNECTED ||
               s == ConnectionState::FAILED;
    }

    bool is_usable() const {
        ConnectionState s = state_.get();
        return s == ConnectionState::CONNECTING || s == ConnectionState::CONNECTED;
    }

private:
    static bool is_valid_transition(ConnectionState from, ConnectionState to) {
        switch (from) {
            case ConnectionState::NEW:
                return to == ConnectionState::CONNECTING;

            case ConnectionState::CONNECTING:
                return to == ConnectionState::CONNECTED ||
                       to == ConnectionState::FAILED ||
                       to == ConnectionState::DISCONNECTED;

            case ConnectionState::CONNECTED:
                return to == ConnectionState::DISCONNECTING ||
                       to == ConnectionState::FAILED;

            case ConnectionState::DISCONNECTING:
                return to == ConnectionState::DISCONNECTED ||
                       to == ConnectionState::FAILED;

            case ConnectionState::DISCONNECTED:
                return to == ConnectionState::CONNECTING;

            case ConnectionState::FAILED:
                return to == ConnectionState::CONNECTING;

            default:
                return false;
        }
    }
};

} // export namespace rrr
