#![allow(unsafe_code)]

use srpc::any_message::any_message_registry;
use srpc::any_message::{deserialize, reg_any_message_as, serialize, AnyMessage};
use rusty::srpc::serializable::{BinaryReadArchive, BinaryWriteArchive};
use rusty::Arc;
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
    assert!(any_message_registry::is_registered_name(
        &GRAPH_NAME.to_owned()
    ));
    assert!(any_message_registry::is_registered_type(TypeId::of::<
        GraphPayload,
    >()));

    let payload = Arc::<GraphPayload>::make(GraphPayload {
        node_count: 42_i32,
        label: "shared".to_owned(),
    });
    let message = AnyMessage::pack(payload);
    assert_eq!(message.type_name_, GRAPH_NAME);
    assert!(message.is_a::<GraphPayload>());
    // The rustc-only serializable facade intentionally keeps holder recovery
    // opaque. The generated-C++ runtime gate owns concrete downcast identity.
    assert!(message.unpack::<GraphPayload>().is_none());
}

#[test]
fn direct_and_free_archive_paths_preserve_independent_wire_bytes() {
    let _guard = TEST_LOCK.lock().unwrap();
    reset_and_register();

    let outgoing = AnyMessage::pack(Arc::<GraphPayload>::make(GraphPayload {
        node_count: 0x1234_5678_i32,
        label: "wire-trip".to_owned(),
    }));
    let mut direct_writer = BinaryWriteArchive::new();
    outgoing.save(&mut direct_writer);
    let mut free_writer = BinaryWriteArchive::new();
    serialize(&outgoing, &mut free_writer);
    assert_eq!(direct_writer.as_bytes(), free_writer.as_bytes());
    assert_eq!(free_writer.as_bytes()[0], GRAPH_NAME.len() as u8);
    assert_eq!(
        &free_writer.as_bytes()[1..1 + GRAPH_NAME.len()],
        GRAPH_NAME.as_bytes()
    );

    let mut reader = BinaryReadArchive::new(free_writer.as_bytes());
    let mut incoming = AnyMessage::default();
    deserialize(&mut incoming, &mut reader);
    assert_eq!(reader.remaining(), 0_usize);
    assert_eq!(incoming.type_name_, GRAPH_NAME);
    assert!(incoming.payload_.is_some());
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
        Arc::<GraphPayload>::make(GraphPayload {
            node_count: 5_i32,
            label: "alias".to_owned(),
        }),
    );
    let mut writer = BinaryWriteArchive::new();
    serialize(&outgoing, &mut writer);
    let mut reader = BinaryReadArchive::new(writer.as_bytes());
    let mut incoming = AnyMessage::default();
    deserialize(&mut incoming, &mut reader);

    assert_eq!(incoming.type_name_, ALIAS);
    assert!(!incoming.is_a::<GraphPayload>());
    assert!(incoming.unpack::<GraphPayload>().is_none());
}
