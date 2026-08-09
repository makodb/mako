//! RPC connection lifecycle state machine.
//!
//! Names, discriminants, fields, and methods intentionally match the
//! `rrr.connection_state` C++ module.  This file is also intended to be the
//! source from which that module is generated.

use std::cell::Cell;

#[allow(non_camel_case_types)]
#[derive(Clone, Copy)]
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
pub fn connection_state_to_string(state: self::ConnectionState) -> &'static str {
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

/// Observer invoked after a state change, as `(from, to)`.
pub type StateChangeCallback = Box<dyn Fn(self::ConnectionState, self::ConnectionState)>;

/// Single-connection lifecycle state.
///
/// The callback is explicitly optional in Rust.  The C++ consumer lowering
/// represents `Option<Box<dyn Fn(...)>>` as nullable `rusty::Function`, so the
/// option does not add a second discriminator or alter the legacy layout.
pub struct ConnectionStateMachine {
    pub state_field: Cell<ConnectionState>,
    pub on_state_change: Option<Box<dyn Fn(ConnectionState, ConnectionState)>>,
}

impl ConnectionStateMachine {
    pub fn new() -> ConnectionStateMachine {
        ConnectionStateMachine {
            state_field: Cell::new(ConnectionState::NEW),
            on_state_change: None,
        }
    }

    pub fn state(&self) -> ConnectionState {
        self.state_field.get()
    }

    pub fn can_transition_to(&self, new_state: self::ConnectionState) -> bool {
        let current = self.state_field.get();
        ConnectionStateMachine::is_valid_transition(current, new_state)
    }

    pub fn transition_to(&self, new_state: self::ConnectionState) -> bool {
        let current = self.state_field.get();
        if !ConnectionStateMachine::is_valid_transition(current, new_state) {
            return false;
        }

        self.state_field.set(new_state);
        if let Some(callback) = &self.on_state_change {
            callback(current, new_state);
        }
        true
    }

    pub fn force_state(&self, new_state: self::ConnectionState) {
        let current = self.state_field.get();
        self.state_field.set(new_state);
        if let Some(callback) = &self.on_state_change {
            callback(current, new_state);
        }
    }

    pub fn set_on_state_change(&mut self, callback: self::StateChangeCallback) {
        self.on_state_change = Some(callback);
    }

    pub fn is_connected(&self) -> bool {
        (self.state_field.get() as i32) == (ConnectionState::CONNECTED as i32)
    }

    pub fn is_failed(&self) -> bool {
        (self.state_field.get() as i32) == (ConnectionState::FAILED as i32)
    }

    pub fn is_terminal(&self) -> bool {
        let state = self.state_field.get();
        (state as i32) == (ConnectionState::DISCONNECTED as i32)
            || (state as i32) == (ConnectionState::FAILED as i32)
    }

    pub fn can_connect(&self) -> bool {
        let state = self.state_field.get();
        (state as i32) == (ConnectionState::NEW as i32)
            || (state as i32) == (ConnectionState::DISCONNECTED as i32)
            || (state as i32) == (ConnectionState::FAILED as i32)
    }

    pub fn is_usable(&self) -> bool {
        let state = self.state_field.get();
        (state as i32) == (ConnectionState::CONNECTING as i32)
            || (state as i32) == (ConnectionState::CONNECTED as i32)
    }

    pub fn is_valid_transition(from: self::ConnectionState, to: self::ConnectionState) -> bool {
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
