use std::collections::BTreeMap;
use sto_rocksdb::{Command, CommandBody, Error, Mutation, RequestId, MAX_COMMAND_BYTES};

fn command() -> Command {
    Command {
        group: u64::MAX,
        request: RequestId {
            client: u64::MAX,
            sequence: u64::MAX,
        },
        body: CommandBody::Transaction {
            mutations: vec![
                Mutation {
                    table: 1,
                    key: Vec::new(),
                    value: None,
                    generation: u64::MAX,
                    was_present: false,
                },
                Mutation {
                    table: 2,
                    key: Vec::new(),
                    value: None,
                    generation: 1,
                    was_present: false,
                },
            ],
            count_deltas: BTreeMap::new(),
        },
    }
}
#[test]
fn logical_codec_preserves_full_width_identity_and_empty_binary_keys() {
    let command = command();
    let bytes = command.encode().unwrap();
    assert_eq!(Command::decode(&bytes).unwrap(), command);
    for truncated in 0..bytes.len() {
        assert!(Command::decode(&bytes[..truncated]).is_err());
    }
    let mut trailing = bytes.clone();
    trailing.push(0);
    assert!(Command::decode(&trailing).is_err());
    let mut wrong_version = bytes;
    wrong_version[7] = b'2';
    assert!(Command::decode(&wrong_version).is_err());
}
#[test]
fn noncanonical_operations_and_inconsistent_counts_cannot_be_decided() {
    let mut command = command();
    let CommandBody::Transaction { mutations, .. } = &mut command.body else {
        unreachable!()
    };
    mutations[0].value = Some(vec![0, 255, 0]);
    assert!(command.encode().is_err());
    let CommandBody::Transaction {
        mutations,
        count_deltas,
    } = &mut command.body
    else {
        unreachable!()
    };
    count_deltas.insert(1, 1);
    mutations.swap(0, 1);
    assert!(command.encode().is_err());
    let CommandBody::Transaction { mutations, .. } = &mut command.body else {
        unreachable!()
    };
    mutations.swap(0, 1);
    assert_eq!(
        Command::decode(&command.encode().unwrap()).unwrap(),
        command
    );
}
#[test]
fn encoded_limits_are_checked_without_truncating_the_request() {
    let mut command = command();
    let CommandBody::Transaction {
        mutations,
        count_deltas,
    } = &mut command.body
    else {
        unreachable!()
    };
    mutations[0].value = Some(vec![7; MAX_COMMAND_BYTES]);
    count_deltas.insert(1, 1);
    assert!(matches!(command.encode(), Err(Error::TooLarge)));
    assert!(Command::create_table(
        1,
        RequestId {
            client: 1,
            sequence: 1
        },
        String::new()
    )
    .is_err());
}
