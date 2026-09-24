use std::cell::Cell;
use std::mem::{align_of, offset_of, size_of};
use std::rc::Rc;

use srpc::connection_state::{
    connection_state_to_string, ConnectionState, ConnectionStateMachine, StateChangeCallback,
};

#[test]
fn layout_discriminants_and_callback_type_match_cpp() {
    macro_rules! assert_not_auto_trait {
        ($type:ty, $auto_trait:ident) => {{
            trait AmbiguousIfImplemented<Marker> {
                fn marker() {}
            }
            impl<T: ?Sized> AmbiguousIfImplemented<()> for T {}
            impl<T: ?Sized + $auto_trait> AmbiguousIfImplemented<u8> for T {}
            let _ = <$type as AmbiguousIfImplemented<_>>::marker;
        }};
    }
    assert_not_auto_trait!(StateChangeCallback, Send);
    assert_not_auto_trait!(StateChangeCallback, Sync);
    assert_not_auto_trait!(ConnectionStateMachine, Send);
    assert_not_auto_trait!(ConnectionStateMachine, Sync);

    assert_eq!(size_of::<ConnectionState>(), 4);
    assert_eq!(align_of::<ConnectionState>(), 4);
    assert_eq!(ConnectionState::NEW as i32, 0);
    assert_eq!(ConnectionState::CONNECTING as i32, 1);
    assert_eq!(ConnectionState::CONNECTED as i32, 2);
    assert_eq!(ConnectionState::DISCONNECTING as i32, 3);
    assert_eq!(ConnectionState::DISCONNECTED as i32, 4);
    assert_eq!(ConnectionState::FAILED as i32, 5);

    assert_eq!(size_of::<StateChangeCallback>(), 48);
    assert_eq!(align_of::<StateChangeCallback>(), 16);
    assert_eq!(size_of::<ConnectionStateMachine>(), 64);
    assert_eq!(align_of::<ConnectionStateMachine>(), 16);
    assert_eq!(offset_of!(ConnectionStateMachine, state_field), 0);
    assert_eq!(offset_of!(ConnectionStateMachine, on_state_change), 16);
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
    assert!(machine.on_state_change.is_empty());
    assert_eq!(machine.state(), ConnectionState::NEW);
    assert!(machine.can_connect());
    assert!(!machine.is_usable());
    assert!(!machine.transition_to(ConnectionState::CONNECTED));
    assert_eq!(machine.state(), ConnectionState::NEW);

    assert!(machine.transition_to(ConnectionState::CONNECTING));
    assert!(machine.is_usable());

    let observed = Rc::new(Cell::new((ConnectionState::NEW, ConnectionState::NEW)));
    let callback_observed = Rc::clone(&observed);
    machine.set_on_state_change(StateChangeCallback::from_callable(move |from, to| {
        callback_observed.set((from, to));
    }));
    assert!(!machine.on_state_change.is_empty());

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
