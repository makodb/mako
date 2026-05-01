#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/cell.hpp>
#include <rusty/fn.hpp>




namespace rrr {

/**
 * Connection lifecycle states for RPC client connections.
 *
 * State transition diagram:
 *     NEW -> CONNECTING -> CONNECTED -> DISCONNECTING -> DISCONNECTED
 *                |              |              |
 *                +------->------+-------->-----+-----> FAILED
 */
enum class ConnectionState : int {
    NEW = 0,          // Initial state, not yet connected
    CONNECTING = 1,   // Connection attempt in progress
    CONNECTED = 2,    // Successfully connected
    DISCONNECTING = 3,// Graceful disconnect in progress
    DISCONNECTED = 4, // Cleanly disconnected
    FAILED = 5        // Connection failed (error occurred)
};

// @safe - Convert ConnectionState to string for logging
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

/**
 * Thread-safe connection state machine.
 *
 * Manages connection lifecycle states with:
 * - Atomic state transitions via rusty::Cell
 * - Validation of state transitions
 * - Optional callbacks on state changes
 *
 * Usage:
 *   ConnectionStateMachine sm;
 *   sm.set_on_state_change([](auto from, auto to) { Log_info("State: %s -> %s", ...); });
 *   sm.transition_to(ConnectionState::CONNECTING);  // Returns true
 *   sm.transition_to(ConnectionState::CONNECTED);   // Returns true
 *   sm.transition_to(ConnectionState::NEW);         // Returns false (invalid)
 */
// @safe - Thread-safe connection state management using rusty::Cell for interior mutability
class ConnectionStateMachine {
private:
    // Cell provides interior mutability for trivially copyable ConnectionState enum
    rusty::Cell<ConnectionState> state_{ConnectionState::NEW};

    // Callback invoked after successful state transitions.
    // rusty::Function is move-only; the callback is set once during
    // initialization (via set_on_state_change) and not modified thereafter.
    rusty::Function<void(ConnectionState, ConnectionState)> on_state_change_;

public:
    // @safe - Default constructor, starts in NEW state
    ConnectionStateMachine() = default;

    // @safe - Constructor with initial state (for testing)
    explicit ConnectionStateMachine(ConnectionState initial_state)
        : state_(initial_state) {}

    // Delete copy (state machines should not be copied)
    ConnectionStateMachine(const ConnectionStateMachine&) = delete;
    ConnectionStateMachine& operator=(const ConnectionStateMachine&) = delete;

    // Move is allowed
    ConnectionStateMachine(ConnectionStateMachine&&) = default;
    ConnectionStateMachine& operator=(ConnectionStateMachine&&) = default;

    // @safe - Get current state
    ConnectionState state() const {
        return state_.get();
    }

    // @safe - Check if a transition from current state to new_state is valid
    bool can_transition_to(ConnectionState new_state) const {
        ConnectionState current = state_.get();
        return is_valid_transition(current, new_state);
    }

    // @safe - Attempt to transition to a new state
    // Returns true if transition was successful, false if invalid
    // Invokes on_state_change_ callback after successful transition
    bool transition_to(ConnectionState new_state) {
        ConnectionState current = state_.get();

        if (!is_valid_transition(current, new_state)) {
            return false;
        }

        state_.set(new_state);

        // Invoke callback if set
        // @unsafe - rusty::Function::operator bool is not annotated
        {
            if (on_state_change_) {
                on_state_change_(current, new_state);
            }
        }

        return true;
    }

    // @safe - Force state transition without validation (for error recovery)
    // Use with caution - primarily for forcing FAILED state from any state
    void force_state(ConnectionState new_state) {
        ConnectionState current = state_.get();
        state_.set(new_state);

        // @unsafe - rusty::Function::operator bool is not annotated
        {
            if (on_state_change_) {
                on_state_change_(current, new_state);
            }
        }
    }

    // @safe - Set callback for state changes
    // Callback receives (from_state, to_state)
    void set_on_state_change(rusty::Function<void(ConnectionState, ConnectionState)> callback) {
        // @unsafe
        { on_state_change_ = std::move(callback); }
    }

    // @safe - Check if currently connected
    bool is_connected() const {
        return state_.get() == ConnectionState::CONNECTED;
    }

    // @safe - Check if in failed state
    bool is_failed() const {
        return state_.get() == ConnectionState::FAILED;
    }

    // @safe - Check if in terminal state (DISCONNECTED or FAILED)
    bool is_terminal() const {
        ConnectionState s = state_.get();
        return s == ConnectionState::DISCONNECTED || s == ConnectionState::FAILED;
    }

    // @safe - Check if connection can be attempted (NEW, DISCONNECTED, or FAILED)
    bool can_connect() const {
        ConnectionState s = state_.get();
        return s == ConnectionState::NEW ||
               s == ConnectionState::DISCONNECTED ||
               s == ConnectionState::FAILED;
    }

    // @safe - Check if in a connecting or connected state
    bool is_usable() const {
        ConnectionState s = state_.get();
        return s == ConnectionState::CONNECTING || s == ConnectionState::CONNECTED;
    }

private:
    // @safe - Validate state transition according to state machine rules
    static bool is_valid_transition(ConnectionState from, ConnectionState to) {
        switch (from) {
            case ConnectionState::NEW:
                // Can only start connecting from NEW
                return to == ConnectionState::CONNECTING;

            case ConnectionState::CONNECTING:
                // Connection attempt can succeed, fail, or be cancelled
                return to == ConnectionState::CONNECTED ||
                       to == ConnectionState::FAILED ||
                       to == ConnectionState::DISCONNECTED;

            case ConnectionState::CONNECTED:
                // Can disconnect gracefully or fail
                return to == ConnectionState::DISCONNECTING ||
                       to == ConnectionState::FAILED;

            case ConnectionState::DISCONNECTING:
                // Disconnect can complete or fail
                return to == ConnectionState::DISCONNECTED ||
                       to == ConnectionState::FAILED;

            case ConnectionState::DISCONNECTED:
                // Can attempt to reconnect
                return to == ConnectionState::CONNECTING;

            case ConnectionState::FAILED:
                // Can attempt to reconnect from failed state
                return to == ConnectionState::CONNECTING;

            default:
                return false;
        }
    }
};

} // namespace rrr
