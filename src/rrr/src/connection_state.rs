// Canonical Rust source for the rrr.connection_state module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::Cell;

#[allow(non_camel_case_types)]
#[derive(Clone, Copy, PartialEq, Eq)]
#[cfg_attr(not(any()), derive(Debug))]
#[repr(i32)]
pub enum ConnectionState {
    NEW = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    DISCONNECTING = 3,
    DISCONNECTED = 4,
    FAILED = 5,
}

#[allow(unreachable_patterns)]
pub fn connection_state_to_string(state: ConnectionState) -> &'static str {
    match state {
        ConnectionState::NEW => "NEW",
        ConnectionState::CONNECTING => "CONNECTING",
        ConnectionState::CONNECTED => "CONNECTED",
        ConnectionState::DISCONNECTING => "DISCONNECTING",
        ConnectionState::DISCONNECTED => "DISCONNECTED",
        ConnectionState::FAILED => "FAILED",
        _ => "UNKNOWN",
    }
}

pub type StateChangeCallback = rusty::Function<dyn Fn(ConnectionState, ConnectionState)>;

#[repr(C)]
pub struct ConnectionStateMachine {
    pub state_field: Cell<ConnectionState>,
    pub on_state_change: StateChangeCallback,
}

impl ConnectionStateMachine {
    #[allow(clippy::new_without_default)]
    pub fn new() -> ConnectionStateMachine {
        ConnectionStateMachine {
            state_field: Cell::<ConnectionState>::new(ConnectionState::NEW),
            on_state_change: Default::default(),
        }
    }

    pub fn state(&self) -> ConnectionState {
        self.state_field.get()
    }

    pub fn can_transition_to(&self, new_state: ConnectionState) -> bool {
        let current: ConnectionState = self.state_field.get();
        ConnectionStateMachine::is_valid_transition(current, new_state)
    }

    pub fn transition_to(&self, new_state: ConnectionState) -> bool {
        let current: ConnectionState = self.state_field.get();
        if !ConnectionStateMachine::is_valid_transition(current, new_state) {
            return false;
        }
        self.state_field.set(new_state);
        if !self.on_state_change.is_empty() {
            (self.on_state_change)(current, new_state);
        }
        true
    }

    pub fn force_state(&self, new_state: ConnectionState) {
        let current: ConnectionState = self.state_field.get();
        self.state_field.set(new_state);
        if !self.on_state_change.is_empty() {
            (self.on_state_change)(current, new_state);
        }
    }

    pub fn set_on_state_change(&mut self, callback: self::StateChangeCallback) {
        self.on_state_change = callback;
    }

    pub fn is_connected(&self) -> bool {
        self.state_field.get() == ConnectionState::CONNECTED
    }

    pub fn is_failed(&self) -> bool {
        self.state_field.get() == ConnectionState::FAILED
    }

    pub fn is_terminal(&self) -> bool {
        let state: ConnectionState = self.state_field.get();
        state == ConnectionState::DISCONNECTED || state == ConnectionState::FAILED
    }

    pub fn can_connect(&self) -> bool {
        let state: ConnectionState = self.state_field.get();
        state == ConnectionState::NEW
            || state == ConnectionState::DISCONNECTED
            || state == ConnectionState::FAILED
    }

    pub fn is_usable(&self) -> bool {
        let state: ConnectionState = self.state_field.get();
        state == ConnectionState::CONNECTING || state == ConnectionState::CONNECTED
    }

    pub fn is_valid_transition(from: ConnectionState, to: ConnectionState) -> bool {
        if from == ConnectionState::NEW {
            return to == ConnectionState::CONNECTING;
        }
        if from == ConnectionState::CONNECTING {
            return to == ConnectionState::CONNECTED
                || to == ConnectionState::FAILED
                || to == ConnectionState::DISCONNECTED;
        }
        if from == ConnectionState::CONNECTED {
            return to == ConnectionState::DISCONNECTING || to == ConnectionState::FAILED;
        }
        if from == ConnectionState::DISCONNECTING {
            return to == ConnectionState::DISCONNECTED || to == ConnectionState::FAILED;
        }
        if from == ConnectionState::DISCONNECTED {
            return to == ConnectionState::CONNECTING;
        }
        if from == ConnectionState::FAILED {
            return to == ConnectionState::CONNECTING;
        }
        false
    }
}
