#![allow(unsafe_code)]

use srpc::any_message::any_message_registry;
use srpc::any_message::{deserialize, reg_any_message_as, serialize, AnyMessage};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, SerializablePayload, Serialize, Deserialize};
use std::sync::Arc;
mod serialization_helpers;
use serialization_helpers::{encode, decode};
use std::any::TypeId;
use std::sync::Mutex;

static TEST_LOCK: Mutex<()> = Mutex::new(());
const GRAPH_NAME: &str = "srpc.test.GraphPayload";
const ALIAS: &str = "srpc.test.GraphPayload.v2";

#[derive(Default, Debug, Eq, PartialEq)]
struct GraphPayload {
    node_count: i32,
    label: String,
}

impl SerializablePayload for GraphPayload {
    fn save(&self, archive: &mut BinaryWriteArchive) {
        self.node_count.serialize(archive);
        self.label.serialize(archive);
    }
    fn load(&mut self, archive: &mut BinaryReadArchive) {
        self.node_count.deserialize(archive);
        self.label.deserialize(archive);
    }
    fn kind(&self) -> i32 { 61 }
}

fn reset_and_register() {
    any_message_registry::clear_for_testing();
    assert_eq!(
        reg_any_message_as::<GraphPayload>(GRAPH_NAME.to_owned()),
        0_i32
    );
}

#[test]
fn registry_and_pack_preserve_name_type_and_shared_payload_identity() {
    let _guard = TEST_LOCK.lock().unwrap();
    any_message_registry::clear_for_testing();

    let empty = AnyMessage::default();
    assert!(empty.type_name_.is_empty());
    assert!(empty.payload_.is_none());
    assert!(!empty.is_a::<GraphPayload>());
    assert!(any_message_registry::name_for_type_owned(TypeId::of::<GraphPayload>()).is_empty());

    reset_and_register();
    assert!(any_message_registry::is_registered_name(GRAPH_NAME));
    assert!(any_message_registry::is_registered_type(TypeId::of::<
        GraphPayload,
    >()));

    let payload = Arc::new(GraphPayload {
        node_count: 42_i32,
        label: "shared".to_owned(),
    });
    let message = AnyMessage::pack(payload.clone());
    assert_eq!(message.type_name_, GRAPH_NAME);
    assert!(message.is_a::<GraphPayload>());
    let unpacked = message.unpack::<GraphPayload>().unwrap();
    assert!(Arc::ptr_eq(&unpacked, &payload));
    assert_eq!(unpacked.node_count, 42);
    assert_eq!(unpacked.label, "shared");
}

#[test]
fn direct_and_free_archive_paths_preserve_independent_wire_bytes() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_and_register();

    let outgoing = AnyMessage::pack(Arc::new(GraphPayload {
        node_count: 0x1234_5678_i32,
        label: "wire-trip".to_owned(),
    }));
    let direct = encode(|archive| outgoing.save(archive));
    let free = encode(|archive| serialize(&outgoing, archive));
    let mut expected = vec![GRAPH_NAME.len() as u8];
    expected.extend_from_slice(GRAPH_NAME.as_bytes());
    expected.extend_from_slice(&0x1234_5678_i32.to_ne_bytes());
    expected.push(9);
    expected.extend_from_slice(b"wire-trip");
    assert_eq!(direct, expected);
    assert_eq!(free, expected);

    let (incoming, remaining) = decode(&expected, |archive| {
        let mut message = AnyMessage::default();
        deserialize(&mut message, archive);
        message
    });
    assert_eq!(remaining, 0);
    assert_eq!(incoming.type_name_, GRAPH_NAME);
    let payload = incoming.unpack::<GraphPayload>().unwrap();
    assert_eq!(payload.node_count, 0x1234_5678);
    assert_eq!(payload.label, "wire-trip");
}

#[test]
fn alias_decodes_but_the_first_registered_name_remains_canonical() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_and_register();
    assert_eq!(reg_any_message_as::<GraphPayload>(ALIAS.to_owned()), 0_i32);
    assert_eq!(
        any_message_registry::name_for_type_owned(TypeId::of::<GraphPayload>()),
        GRAPH_NAME
    );

    let outgoing = AnyMessage::pack_as(
        ALIAS.to_owned(),
        Arc::new(GraphPayload {
            node_count: 5_i32,
            label: "alias".to_owned(),
        }),
    );
    let encoded = encode(|archive| serialize(&outgoing, archive));
    let (incoming, remaining) = decode(&encoded, |archive| {
        let mut message = AnyMessage::default();
        deserialize(&mut message, archive);
        message
    });
    assert_eq!(remaining, 0);

    assert_eq!(incoming.type_name_, ALIAS);
    assert!(!incoming.is_a::<GraphPayload>());
    assert!(incoming.unpack::<GraphPayload>().is_none());
}

#[derive(Default)]
struct OtherPayload {
    value: u64,
}

impl SerializablePayload for OtherPayload {
    fn save(&self, archive: &mut BinaryWriteArchive) { self.value.serialize(archive); }
    fn load(&mut self, archive: &mut BinaryReadArchive) { self.value.deserialize(archive); }
    fn kind(&self) -> i32 { 62 }
}

#[test]
fn registered_name_spoof_cannot_change_the_actual_holder_type() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_and_register();
    let other_name = "srpc.test.OtherPayload".to_owned();
    reg_any_message_as::<OtherPayload>(other_name.clone());
    let message = AnyMessage::pack_as(other_name, Arc::new(GraphPayload {
        node_count: 123,
        label: "actual graph".to_owned(),
    }));
    assert!(message.is_a::<OtherPayload>());
    assert!(message.unpack::<OtherPayload>().is_none());
    assert!(message.unpack::<u64>().is_none());
}
