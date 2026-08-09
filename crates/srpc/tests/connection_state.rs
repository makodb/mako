use srpc::rpc::connection_state::{
    connection_state_to_string, ConnectionState, ConnectionStateMachine,
};
use std::cell::RefCell;
use std::rc::Rc;

const ALL: [ConnectionState; 6] = [
    ConnectionState::NEW,
    ConnectionState::CONNECTING,
    ConnectionState::CONNECTED,
    ConnectionState::DISCONNECTING,
    ConnectionState::DISCONNECTED,
    ConnectionState::FAILED,
];

fn state_is(actual: ConnectionState, expected: ConnectionState) -> bool {
    (actual as i32) == (expected as i32)
}

#[test]
fn discriminants_and_legacy_names_are_stable() {
    let expected = [
        (0, "NEW"),
        (1, "CONNECTING"),
        (2, "CONNECTED"),
        (3, "DISCONNECTING"),
        (4, "DISCONNECTED"),
        (5, "FAILED"),
    ];
    let mut index = 0;
    while index < ALL.len() {
        assert_eq!(ALL[index] as i32, expected[index].0);
        assert_eq!(connection_state_to_string(ALL[index]), expected[index].1);
        index += 1;
    }
}

#[test]
fn transition_table_is_exact() {
    let allowed = [
        (0, 1),
        (1, 2),
        (1, 5),
        (1, 4),
        (2, 3),
        (2, 5),
        (3, 4),
        (3, 5),
        (4, 1),
        (5, 1),
    ];

    let mut from_index = 0;
    while from_index < ALL.len() {
        let mut to_index = 0;
        while to_index < ALL.len() {
            let from = ALL[from_index];
            let to = ALL[to_index];
            let mut expected = false;
            let mut allowed_index = 0;
            while allowed_index < allowed.len() {
                if allowed[allowed_index] == (from as i32, to as i32) {
                    expected = true;
                }
                allowed_index += 1;
            }
            assert_eq!(
                ConnectionStateMachine::is_valid_transition(from, to),
                expected,
                "{} -> {}",
                from as i32,
                to as i32
            );
            to_index += 1;
        }
        from_index += 1;
    }
}

#[test]
fn transitions_force_and_predicates_match_the_legacy_machine() {
    let machine = ConnectionStateMachine::new();
    assert!(state_is(machine.state(), ConnectionState::NEW));
    assert!(machine.can_connect());
    assert!(!machine.is_usable());
    assert!(!machine.transition_to(ConnectionState::CONNECTED));

    assert!(machine.transition_to(ConnectionState::CONNECTING));
    assert!(machine.is_usable());
    assert!(machine.can_transition_to(ConnectionState::CONNECTED));
    assert!(machine.transition_to(ConnectionState::CONNECTED));
    assert!(machine.is_connected());
    assert!(machine.transition_to(ConnectionState::DISCONNECTING));
    assert!(!machine.is_usable());
    assert!(machine.transition_to(ConnectionState::DISCONNECTED));
    assert!(machine.is_terminal());
    assert!(machine.can_connect());

    machine.force_state(ConnectionState::FAILED);
    assert!(machine.is_failed());
    assert!(machine.is_terminal());
    machine.force_state(ConnectionState::NEW);
    assert!(state_is(machine.state(), ConnectionState::NEW));
}

#[test]
fn callback_observes_only_successful_and_forced_changes() {
    let seen = Rc::new(RefCell::new(Vec::<(i32, i32)>::new()));
    let sink = Rc::clone(&seen);
    let mut machine = ConnectionStateMachine::new();
    machine.set_on_state_change(Box::new(move |from, to| {
        sink.borrow_mut().push((from as i32, to as i32));
    }));

    assert!(machine.transition_to(ConnectionState::CONNECTING));
    assert!(!machine.transition_to(ConnectionState::DISCONNECTING));
    machine.force_state(ConnectionState::FAILED);

    assert_eq!(*seen.borrow(), vec![(0, 1), (1, 5)]);
}
