use srpc::threading::SharedCell as Cell;
use std::mem::{align_of, size_of};
use std::sync::Arc as Rc;

use srpc::connection_state::{
    StateChangeCallback,     connection_state_to_string, ConnectionState, ConnectionStateMachine,
};

#[test]
fn state_layout_discriminants_and_callback_thread_traits_are_stable() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<StateChangeCallback>();
    assert_send_sync::<ConnectionStateMachine>();

    assert_eq!(size_of::<ConnectionState>(), 4);
    assert_eq!(align_of::<ConnectionState>(), 4);
    assert_eq!(ConnectionState::NEW as i32, 0);
    assert_eq!(ConnectionState::CONNECTING as i32, 1);
    assert_eq!(ConnectionState::CONNECTED as i32, 2);
    assert_eq!(ConnectionState::DISCONNECTING as i32, 3);
    assert_eq!(ConnectionState::DISCONNECTED as i32, 4);
    assert_eq!(ConnectionState::FAILED as i32, 5);

}

#[test]
fn names_and_transition_table_are_exact() {
    let rows = [
        (ConnectionState::NEW, "NEW"),
        (ConnectionState::CONNECTING, "CONNECTING"),
        (ConnectionState::CONNECTED, "CONNECTED"),
        (ConnectionState::DISCONNECTING, "DISCONNECTING"),
        (ConnectionState::DISCONNECTED, "DISCONNECTED"),
        (ConnectionState::FAILED, "FAILED"),
    ];
    for (state, name) in rows {
        assert_eq!(connection_state_to_string(state), name);
    }

    for from in rows.map(|row| row.0) {
        for to in rows.map(|row| row.0) {
            let expected = matches!(
                (from, to),
                (ConnectionState::NEW, ConnectionState::CONNECTING)
                    | (ConnectionState::CONNECTING, ConnectionState::CONNECTED)
                    | (ConnectionState::CONNECTING, ConnectionState::FAILED)
                    | (ConnectionState::CONNECTING, ConnectionState::DISCONNECTED)
                    | (ConnectionState::CONNECTED, ConnectionState::DISCONNECTING)
                    | (ConnectionState::CONNECTED, ConnectionState::FAILED)
                    | (
                        ConnectionState::DISCONNECTING,
                        ConnectionState::DISCONNECTED
                    )
                    | (ConnectionState::DISCONNECTING, ConnectionState::FAILED)
                    | (ConnectionState::DISCONNECTED, ConnectionState::CONNECTING)
                    | (ConnectionState::FAILED, ConnectionState::CONNECTING)
            );
            assert_eq!(
                ConnectionStateMachine::is_valid_transition(from, to),
                expected
            );
        }
    }
}

#[test]
fn default_empty_callback_and_installed_callback_preserve_state_behavior() {
    let mut machine = ConnectionStateMachine::new();
    assert!(machine.on_state_change.is_none());
    assert_eq!(machine.state(), ConnectionState::NEW);
    assert!(machine.can_connect());
    assert!(!machine.is_usable());
    assert!(!machine.transition_to(ConnectionState::CONNECTED));
    assert_eq!(machine.state(), ConnectionState::NEW);

    assert!(machine.transition_to(ConnectionState::CONNECTING));
    assert!(machine.is_usable());

    let observed = Rc::new(Cell::new((ConnectionState::NEW, ConnectionState::NEW)));
    let callback_observed = Rc::clone(&observed);
    machine.set_on_state_change(Some(Box::new(move |from, to| {
        callback_observed.set((from, to));
    })));
    assert!(!machine.on_state_change.is_none());

    assert!(machine.transition_to(ConnectionState::CONNECTED));
    assert_eq!(
        observed.get(),
        (ConnectionState::CONNECTING, ConnectionState::CONNECTED)
    );
    assert!(machine.is_connected());

    machine.force_state(ConnectionState::FAILED);
    assert_eq!(
        observed.get(),
        (ConnectionState::CONNECTED, ConnectionState::FAILED)
    );
    assert!(machine.is_failed());
    assert!(machine.is_terminal());
    assert!(machine.can_connect());
}
