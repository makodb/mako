//! Connection lifecycle — the port of `src/rrr/rpc/connection_state.cpp`.
//!
//! ```text
//!   New ─▶ Connecting ─┬─▶ Connected ─▶ Disconnecting ─▶ Disconnected ─┐
//!            ▲         ├─▶ Failed                    ─▶ Failed         │
//!            │         └─▶ Disconnected                                │
//!            └──────────────── (reconnect from Disconnected/Failed) ◀──┘
//! ```
//!
//! `Failed` is reachable only from the ACTIVE states — `Connecting`,
//! `Connected`, `Disconnecting`. The settled states do not fail:
//! `New` has attempted nothing, and `Disconnected`/`Failed` have
//! nothing in flight to fail, so they only re-enter by connecting.
//! `New` is entered once and never returned to — a connection that has
//! been used is never brand new again.
//!
//! [`ConnectionStateMachine::transition_to`] REFUSES an illegal
//! transition and reports it rather than performing it, so a caller
//! that gets the lifecycle wrong finds out instead of silently
//! corrupting the state. [`ConnectionStateMachine::force_state`] is the
//! deliberate escape hatch (teardown paths that must land in a known
//! state regardless of where they were).

use std::cell::Cell;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum ConnectionState {
    New = 0,
    Connecting = 1,
    Connected = 2,
    Disconnecting = 3,
    Disconnected = 4,
    Failed = 5,
}

impl ConnectionState {
    pub fn as_str(self) -> &'static str {
        match self {
            ConnectionState::New => "NEW",
            ConnectionState::Connecting => "CONNECTING",
            ConnectionState::Connected => "CONNECTED",
            ConnectionState::Disconnecting => "DISCONNECTING",
            ConnectionState::Disconnected => "DISCONNECTED",
            ConnectionState::Failed => "FAILED",
        }
    }

    /// Whether requests may be sent in this state.
    pub fn is_usable(self) -> bool {
        self == ConnectionState::Connected
    }

    /// Whether a reconnect may start from here.
    pub fn is_reconnectable(self) -> bool {
        matches!(
            self,
            ConnectionState::Disconnected | ConnectionState::Failed
        )
    }

    /// Whether the connection is settled (no transition pending).
    pub fn is_terminal_for_now(self) -> bool {
        matches!(
            self,
            ConnectionState::Disconnected | ConnectionState::Failed
        )
    }
}

impl std::fmt::Display for ConnectionState {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Callback invoked after a state change, as `(from, to)`.
type StateChangeCallback = Box<dyn Fn(ConnectionState, ConnectionState)>;

pub struct ConnectionStateMachine {
    state: Cell<ConnectionState>,
    on_state_change: Option<StateChangeCallback>,
}

impl ConnectionStateMachine {
    pub fn new() -> ConnectionStateMachine {
        ConnectionStateMachine {
            state: Cell::new(ConnectionState::New),
            on_state_change: None,
        }
    }

    /// Observe every state change. Replaces any previous observer.
    pub fn with_observer(mut self, cb: StateChangeCallback) -> ConnectionStateMachine {
        self.on_state_change = Some(cb);
        self
    }

    pub fn state(&self) -> ConnectionState {
        self.state.get()
    }

    /// The lifecycle's transition table.
    pub fn is_valid_transition(from: ConnectionState, to: ConnectionState) -> bool {
        match from {
            // Never returned to: a used connection is not brand new.
            ConnectionState::New => to == ConnectionState::Connecting,
            ConnectionState::Connecting => matches!(
                to,
                ConnectionState::Connected
                    | ConnectionState::Failed
                    | ConnectionState::Disconnected
            ),
            ConnectionState::Connected => {
                matches!(to, ConnectionState::Disconnecting | ConnectionState::Failed)
            }
            ConnectionState::Disconnecting => {
                matches!(to, ConnectionState::Disconnected | ConnectionState::Failed)
            }
            // Both settled states re-enter the lifecycle by connecting.
            ConnectionState::Disconnected | ConnectionState::Failed => {
                to == ConnectionState::Connecting
            }
        }
    }

    pub fn can_transition_to(&self, new_state: ConnectionState) -> bool {
        ConnectionStateMachine::is_valid_transition(self.state.get(), new_state)
    }

    /// Attempt a transition. Returns `false` and changes nothing if the
    /// move is not part of the lifecycle.
    pub fn transition_to(&self, new_state: ConnectionState) -> bool {
        let current = self.state.get();
        if !ConnectionStateMachine::is_valid_transition(current, new_state) {
            return false;
        }
        self.state.set(new_state);
        if let Some(cb) = &self.on_state_change {
            cb(current, new_state);
        }
        true
    }

    /// Set the state regardless of the table — for teardown paths that
    /// must land somewhere known. Still notifies the observer.
    pub fn force_state(&self, new_state: ConnectionState) {
        let current = self.state.get();
        self.state.set(new_state);
        if let Some(cb) = &self.on_state_change {
            cb(current, new_state);
        }
    }

    pub fn is_connected(&self) -> bool {
        self.state.get() == ConnectionState::Connected
    }

    pub fn is_usable(&self) -> bool {
        self.state.get().is_usable()
    }
}

impl Default for ConnectionStateMachine {
    fn default() -> ConnectionStateMachine {
        ConnectionStateMachine::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    const ALL: [ConnectionState; 6] = [
        ConnectionState::New,
        ConnectionState::Connecting,
        ConnectionState::Connected,
        ConnectionState::Disconnecting,
        ConnectionState::Disconnected,
        ConnectionState::Failed,
    ];

    #[test]
    fn discriminants_and_names() {
        assert_eq!(ConnectionState::New as i32, 0);
        assert_eq!(ConnectionState::Connecting as i32, 1);
        assert_eq!(ConnectionState::Connected as i32, 2);
        assert_eq!(ConnectionState::Disconnecting as i32, 3);
        assert_eq!(ConnectionState::Disconnected as i32, 4);
        assert_eq!(ConnectionState::Failed as i32, 5);
        assert_eq!(ConnectionState::Disconnecting.to_string(), "DISCONNECTING");
    }

    /// The whole table, stated once so an accidental widening shows up.
    #[test]
    fn transition_table_is_exactly_this() {
        let allowed: [(ConnectionState, ConnectionState); 10] = [
            (ConnectionState::New, ConnectionState::Connecting),
            (ConnectionState::Connecting, ConnectionState::Connected),
            (ConnectionState::Connecting, ConnectionState::Failed),
            (ConnectionState::Connecting, ConnectionState::Disconnected),
            (ConnectionState::Connected, ConnectionState::Disconnecting),
            (ConnectionState::Connected, ConnectionState::Failed),
            (
                ConnectionState::Disconnecting,
                ConnectionState::Disconnected,
            ),
            (ConnectionState::Disconnecting, ConnectionState::Failed),
            (ConnectionState::Disconnected, ConnectionState::Connecting),
            (ConnectionState::Failed, ConnectionState::Connecting),
        ];
        let mut i = 0;
        while i < ALL.len() {
            let mut j = 0;
            while j < ALL.len() {
                let (from, to) = (ALL[i], ALL[j]);
                let want = allowed.contains(&(from, to));
                assert_eq!(
                    ConnectionStateMachine::is_valid_transition(from, to),
                    want,
                    "{from} -> {to}"
                );
                j += 1;
            }
            i += 1;
        }
    }

    #[test]
    fn nothing_returns_to_new() {
        let mut i = 0;
        while i < ALL.len() {
            assert!(
                !ConnectionStateMachine::is_valid_transition(ALL[i], ConnectionState::New),
                "{} must not reach New",
                ALL[i]
            );
            i += 1;
        }
    }

    #[test]
    fn happy_path_walks_the_lifecycle() {
        let m = ConnectionStateMachine::new();
        assert_eq!(m.state(), ConnectionState::New);
        assert!(m.transition_to(ConnectionState::Connecting));
        assert!(m.transition_to(ConnectionState::Connected));
        assert!(m.is_connected() && m.is_usable());
        assert!(m.transition_to(ConnectionState::Disconnecting));
        assert!(m.transition_to(ConnectionState::Disconnected));
        assert!(!m.is_usable());
        // …and reconnects.
        assert!(m.transition_to(ConnectionState::Connecting));
        assert!(m.transition_to(ConnectionState::Connected));
    }

    #[test]
    fn illegal_transition_is_refused_and_changes_nothing() {
        let m = ConnectionStateMachine::new();
        assert!(
            !m.transition_to(ConnectionState::Connected),
            "New -> Connected"
        );
        assert_eq!(m.state(), ConnectionState::New, "state untouched");
        assert!(!m.can_transition_to(ConnectionState::Failed));
        assert!(m.transition_to(ConnectionState::Connecting));
        assert!(!m.transition_to(ConnectionState::Disconnecting));
        assert_eq!(m.state(), ConnectionState::Connecting);
    }

    #[test]
    fn force_state_ignores_the_table() {
        let m = ConnectionStateMachine::new();
        m.force_state(ConnectionState::Connected);
        assert_eq!(
            m.state(),
            ConnectionState::Connected,
            "forced past the table"
        );
        m.force_state(ConnectionState::New);
        assert_eq!(m.state(), ConnectionState::New, "even back to New");
    }

    #[test]
    fn observer_sees_from_and_to_for_both_paths() {
        let seen: Rc<std::cell::RefCell<Vec<(ConnectionState, ConnectionState)>>> =
            Rc::new(std::cell::RefCell::new(Vec::new()));
        let sink = Rc::clone(&seen);
        let m = ConnectionStateMachine::new()
            .with_observer(Box::new(move |from, to| sink.borrow_mut().push((from, to))));

        assert!(m.transition_to(ConnectionState::Connecting));
        assert!(!m.transition_to(ConnectionState::Disconnecting), "refused");
        m.force_state(ConnectionState::Failed);

        let log = seen.borrow();
        assert_eq!(
            *log,
            vec![
                (ConnectionState::New, ConnectionState::Connecting),
                (ConnectionState::Connecting, ConnectionState::Failed),
            ],
            "a refused transition must not notify"
        );
    }

    #[test]
    fn state_predicates() {
        assert!(ConnectionState::Connected.is_usable());
        assert!(!ConnectionState::Connecting.is_usable());
        assert!(ConnectionState::Failed.is_reconnectable());
        assert!(ConnectionState::Disconnected.is_reconnectable());
        assert!(!ConnectionState::Connected.is_reconnectable());
        assert!(ConnectionState::Failed.is_terminal_for_now());
        assert!(!ConnectionState::New.is_terminal_for_now());
    }

    /// Only the ACTIVE states fail. The settled ones have nothing in
    /// flight to fail and re-enter the lifecycle by connecting instead.
    #[test]
    fn only_active_states_can_fail() {
        let mut i = 0;
        while i < ALL.len() {
            let s = ALL[i];
            let active = matches!(
                s,
                ConnectionState::Connecting
                    | ConnectionState::Connected
                    | ConnectionState::Disconnecting
            );
            assert_eq!(
                ConnectionStateMachine::is_valid_transition(s, ConnectionState::Failed),
                active,
                "{s} -> Failed"
            );
            i += 1;
        }
    }
}
