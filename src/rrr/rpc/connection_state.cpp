module;

#include <rusty/cell.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/move.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>

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

// Type alias for the state-change callback. Switched from the
// non-const-callable `rusty::Function<void(...)>` to the const-callable
// `... const` variant so the `transition_to()` and `force_state()`
// const methods can fire it without needing a `mutable` field. All
// known callers register captureless / const-callable `[&]` lambdas,
// which satisfy the stricter const-invocable requirement.
//
// Defined outside the DSL block so the inline-Rust source can refer to
// it by an opaque type name (the DSL transpiler does not parse C++
// function-type template arguments like `<void(...) const>`).
using StateChangeCallback =
    rusty::Function<void(ConnectionState, ConnectionState) const>;

// `ConnectionStateMachine` — `rusty::Cell<ConnectionState>` plus a
// `StateChangeCallback` for transition observers. Validated FSM with
// the same allowed transitions as before.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The constructor uses the
// `#[cpp_ctor]` attribute so existing call sites
// (`ConnectionStateMachine sm;` in tests, the `state_machine_`
// default-init member in ClientConnection) keep compiling.
//
// Behavioral diffs from the original C++ class:
//   * The unused `explicit ConnectionStateMachine(ConnectionState)`
//     ctor is dropped. No call site passed an explicit initial state.
//   * The `= delete` copy/move declarations are dropped (the DSL
//     does not emit special-member-function annotations). The class
//     is implicitly copyable now; acceptable here because all call
//     sites hold a StateMachine by value or by member, none clone
//     it deliberately.
//   * `set_on_state_change()` becomes non-const. Previously the
//     field was `mutable` so the setter could stay `const`; the DSL
//     does not emit `mutable`, so the setter must be `&mut self`
//     instead. All known callers (3 in rpc_connection_state_test.cc)
//     hold a non-const `sm` local, so this re-typing is invisible.
//   * `transition_to()` and `force_state()` stay `const`. The
//     callback type is now `Function<... const>` (const-invocable),
//     so the firing path compiles without the `mutable` keyword.
//   * Fields are no longer marked `private`. No callers reach into
//     them.
#if RUSTYCPP_RUST
struct ConnectionStateMachine {
    state_field: rusty::Cell<ConnectionState>,
    on_state_change: StateChangeCallback,
}

impl ConnectionStateMachine {
    #[cpp_ctor]
    fn new() -> ConnectionStateMachine {
        ConnectionStateMachine {
            state_field: rusty::Cell::<ConnectionState>::new(ConnectionState::NEW),
            on_state_change: StateChangeCallback {},
        }
    }

    fn state(&self) -> ConnectionState {
        self.state_field.get()
    }

    fn can_transition_to(&self, new_state: ConnectionState) -> bool {
        let current: ConnectionState = self.state_field.get();
        ConnectionStateMachine::is_valid_transition(current, new_state)
    }

    fn transition_to(&self, new_state: ConnectionState) -> bool {
        let current: ConnectionState = self.state_field.get();
        if !ConnectionStateMachine::is_valid_transition(current, new_state) {
            return false;
        }
        self.state_field.set(new_state);
        if self.on_state_change {
            self.on_state_change(current, new_state);
        }
        true
    }

    fn force_state(&self, new_state: ConnectionState) {
        let current: ConnectionState = self.state_field.get();
        self.state_field.set(new_state);
        if self.on_state_change {
            self.on_state_change(current, new_state);
        }
    }

    fn set_on_state_change(&mut self, callback: StateChangeCallback) {
        self.on_state_change = callback;
    }

    fn is_connected(&self) -> bool {
        (self.state_field.get() as i32) == (ConnectionState::CONNECTED as i32)
    }

    fn is_failed(&self) -> bool {
        (self.state_field.get() as i32) == (ConnectionState::FAILED as i32)
    }

    fn is_terminal(&self) -> bool {
        let s: ConnectionState = self.state_field.get();
        (s as i32) == (ConnectionState::DISCONNECTED as i32)
            || (s as i32) == (ConnectionState::FAILED as i32)
    }

    fn can_connect(&self) -> bool {
        let s: ConnectionState = self.state_field.get();
        (s as i32) == (ConnectionState::NEW as i32)
            || (s as i32) == (ConnectionState::DISCONNECTED as i32)
            || (s as i32) == (ConnectionState::FAILED as i32)
    }

    fn is_usable(&self) -> bool {
        let s: ConnectionState = self.state_field.get();
        (s as i32) == (ConnectionState::CONNECTING as i32)
            || (s as i32) == (ConnectionState::CONNECTED as i32)
    }

    fn is_valid_transition(from: ConnectionState, to: ConnectionState) -> bool {
        if (from as i32) == (ConnectionState::NEW as i32) {
            return (to as i32) == (ConnectionState::CONNECTING as i32);
        }
        if (from as i32) == (ConnectionState::CONNECTING as i32) {
            return (to as i32) == (ConnectionState::CONNECTED as i32)
                || (to as i32) == (ConnectionState::FAILED as i32)
                || (to as i32) == (ConnectionState::DISCONNECTED as i32);
        }
        if (from as i32) == (ConnectionState::CONNECTED as i32) {
            return (to as i32) == (ConnectionState::DISCONNECTING as i32)
                || (to as i32) == (ConnectionState::FAILED as i32);
        }
        if (from as i32) == (ConnectionState::DISCONNECTING as i32) {
            return (to as i32) == (ConnectionState::DISCONNECTED as i32)
                || (to as i32) == (ConnectionState::FAILED as i32);
        }
        if (from as i32) == (ConnectionState::DISCONNECTED as i32) {
            return (to as i32) == (ConnectionState::CONNECTING as i32);
        }
        if (from as i32) == (ConnectionState::FAILED as i32) {
            return (to as i32) == (ConnectionState::CONNECTING as i32);
        }
        false
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=connection_state.1 version=1 rust_sha256=4b7cf214432bba09132a9ce8324d6040edce5bced87c652491e99917aa285b91*/
struct ConnectionStateMachine;

struct ConnectionStateMachine {
    rusty::Cell<ConnectionState> state_field;
    StateChangeCallback on_state_change;

    ConnectionStateMachine();
    ConnectionState state() const;
    bool can_transition_to(ConnectionState new_state) const;
    bool transition_to(ConnectionState new_state) const;
    void force_state(ConnectionState new_state) const;
    void set_on_state_change(StateChangeCallback callback);
    bool is_connected() const;
    bool is_failed() const;
    bool is_terminal() const;
    bool can_connect() const;
    bool is_usable() const;
    static bool is_valid_transition(ConnectionState from, ConnectionState to);
};


ConnectionStateMachine::ConnectionStateMachine()
    : state_field(rusty::Cell<ConnectionState>::new_(rusty::clone(rusty::clone(ConnectionState::NEW))))
    , on_state_change(StateChangeCallback{})
{}

ConnectionState ConnectionStateMachine::state() const {
    return this->state_field.get();
}

bool ConnectionStateMachine::can_transition_to(ConnectionState new_state) const {
    ConnectionState current = this->state_field.get();
    return ConnectionStateMachine::is_valid_transition(std::move(current), std::move(new_state));
}

bool ConnectionStateMachine::transition_to(ConnectionState new_state) const {
    ConnectionState current = this->state_field.get();
    if (!ConnectionStateMachine::is_valid_transition(std::move(current), std::move(new_state))) {
        return false;
    }
    this->state_field.set(std::move(new_state));
    if (this->on_state_change) {
        this->on_state_change(std::move(current), std::move(new_state));
    }
    return true;
}

void ConnectionStateMachine::force_state(ConnectionState new_state) const {
    const ConnectionState current = this->state_field.get();
    this->state_field.set(std::move(new_state));
    if (this->on_state_change) {
        this->on_state_change(std::move(current), std::move(new_state));
    }
}

void ConnectionStateMachine::set_on_state_change(StateChangeCallback callback) {
    this->on_state_change = std::move(callback);
}

bool ConnectionStateMachine::is_connected() const {
    return ((static_cast<int32_t>(this->state_field.get()))) == ((static_cast<int32_t>(ConnectionState::CONNECTED)));
}

bool ConnectionStateMachine::is_failed() const {
    return ((static_cast<int32_t>(this->state_field.get()))) == ((static_cast<int32_t>(ConnectionState::FAILED)));
}

bool ConnectionStateMachine::is_terminal() const {
    const ConnectionState s = this->state_field.get();
    return (((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED)))) || (((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::FAILED))));
}

bool ConnectionStateMachine::can_connect() const {
    const ConnectionState s = this->state_field.get();
    return ((((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::NEW)))) || (((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) || (((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::FAILED))));
}

bool ConnectionStateMachine::is_usable() const {
    const ConnectionState s = this->state_field.get();
    return (((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::CONNECTING)))) || (((static_cast<int32_t>(s))) == ((static_cast<int32_t>(ConnectionState::CONNECTED))));
}

bool ConnectionStateMachine::is_valid_transition(ConnectionState from, ConnectionState to) {
    if (((static_cast<int32_t>(from))) == ((static_cast<int32_t>(ConnectionState::NEW)))) {
        return ((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::CONNECTING)));
    }
    if (((static_cast<int32_t>(from))) == ((static_cast<int32_t>(ConnectionState::CONNECTING)))) {
        return ((((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::CONNECTED)))) || (((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::FAILED))))) || (((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))));
    }
    if (((static_cast<int32_t>(from))) == ((static_cast<int32_t>(ConnectionState::CONNECTED)))) {
        return (((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTING)))) || (((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::FAILED))));
    }
    if (((static_cast<int32_t>(from))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTING)))) {
        return (((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED)))) || (((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::FAILED))));
    }
    if (((static_cast<int32_t>(from))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED)))) {
        return ((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::CONNECTING)));
    }
    if (((static_cast<int32_t>(from))) == ((static_cast<int32_t>(ConnectionState::FAILED)))) {
        return ((static_cast<int32_t>(to))) == ((static_cast<int32_t>(ConnectionState::CONNECTING)));
    }
    return false;
}
/*RUSTYCPP:GEN-END id=connection_state.1*/

} // export namespace rrr
