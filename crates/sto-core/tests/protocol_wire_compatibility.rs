//! Captured v2 control envelopes remain readable after appending 2PC variants.
use sto_core::protocol::wire::{self, Message, Stream};

#[test]
fn existing_v2_control_envelopes_keep_their_wire_tags() {
    let status = [2, 0, 0, 0, 1, 0, 0, 0];
    assert!(matches!(
        wire::decode::<Message>(&status).unwrap(),
        Message::ReplicaStatus
    ));
    assert_eq!(wire::encode(&Message::ReplicaStatus).unwrap(), status);
    let open = [
        2, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 7, 0, 0, 0, 11, 0, 0, 0, 0, 0, 0, 0,
    ];
    assert!(matches!(
        wire::decode::<Message>(&open).unwrap(),
        Message::OpenStream {
            stream: Stream { node: 3, worker: 7 },
            nonce: 11
        }
    ));
    assert_eq!(
        wire::encode(&Message::OpenStream {
            stream: Stream { node: 3, worker: 7 },
            nonce: 11,
        })
        .unwrap(),
        open
    );
    let mut future = status;
    future[0] = 3;
    assert!(wire::decode::<Message>(&future).is_err());
    assert!(wire::decode::<Message>(&[status.as_slice(), &[0]].concat()).is_err());
}
