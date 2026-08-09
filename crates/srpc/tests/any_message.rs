#![allow(unsafe_code)]

use srpc::rpc::any_message::any_message_registry;
use srpc::rpc::any_message::cpp::rrr::serializable::{
    BinaryReadArchive, BinaryWriteArchive, SerializablePayload,
};
use srpc::rpc::any_message::{deserialize, reg_any_message_as, serialize, AnyMessage};
use std::any::TypeId;
use std::sync::{Arc, Mutex};

static TEST_LOCK: Mutex<()> = Mutex::new(());

const GRAPH_NAME: &str = "rrr.test.GraphPayload";
const OTHER_NAME: &str = "rrr.test.OtherPayload";

#[derive(Default, Debug, Eq, PartialEq)]
struct GraphPayload {
    node_count: i32,
    label: String,
}

impl SerializablePayload for GraphPayload {
    fn save(&self, archive: &mut BinaryWriteArchive) {
        archive.write_bytes(&self.node_count.to_le_bytes());
        assert!(self.label.len() <= 63_usize);
        archive.write_bytes(&[self.label.len() as u8]);
        archive.write_bytes(self.label.as_bytes());
    }

    fn load(&mut self, archive: &mut BinaryReadArchive) {
        let mut node_count = [0_u8; 4];
        archive.read_exact(&mut node_count);
        self.node_count = i32::from_le_bytes(node_count);
        let mut label_length = [0_u8; 1];
        archive.read_exact(&mut label_length);
        let mut label = vec![0_u8; label_length[0] as usize];
        archive.read_exact(&mut label);
        self.label = String::from_utf8(label).unwrap();
    }

    fn kind(&self) -> i32 {
        self.node_count
    }
}

#[derive(Default, Debug, Eq, PartialEq)]
struct OtherPayload {
    value: u64,
}

impl SerializablePayload for OtherPayload {
    fn save(&self, archive: &mut BinaryWriteArchive) {
        archive.write_bytes(&self.value.to_le_bytes());
    }

    fn load(&mut self, archive: &mut BinaryReadArchive) {
        let mut value = [0_u8; 8];
        archive.read_exact(&mut value);
        self.value = u64::from_le_bytes(value);
    }

    fn kind(&self) -> i32 {
        0_i32
    }
}

fn reset_and_register() {
    any_message_registry::clear_for_testing();
    assert_eq!(
        reg_any_message_as::<GraphPayload>(GRAPH_NAME.to_owned()),
        0_i32
    );
    assert_eq!(
        reg_any_message_as::<OtherPayload>(OTHER_NAME.to_owned()),
        0_i32
    );
}

#[test]
fn registry_preserves_the_public_type_and_name_contract() {
    let _test_guard = TEST_LOCK.lock().unwrap();
    any_message_registry::clear_for_testing();

    let empty = AnyMessage::default();
    assert!(empty.type_name_.is_empty());
    assert!(empty.payload_.is_none());
    assert!(!empty.is_a::<GraphPayload>());
    assert!(any_message_registry::name_for_type_owned(TypeId::of::<GraphPayload>()).is_empty());
    assert!(!any_message_registry::is_registered_name(
        &GRAPH_NAME.to_owned()
    ));
    assert!(!any_message_registry::is_registered_type(TypeId::of::<
        GraphPayload,
    >()));
    assert!(any_message_registry::create(&GRAPH_NAME.to_owned()).is_none());

    let mut invocation_count = 0_i32;
    let stateful_factory: any_message_registry::Factory = Box::new(move || {
        invocation_count += 1_i32;
        let pointer = Arc::new(GraphPayload {
            node_count: invocation_count,
            label: "factory".to_owned(),
        });
        let holder = unsafe {
            srpc::rpc::any_message::cpp::rrr::serializable::details::SerializableSharedPtrHolder(
                pointer,
            )
        };
        let concrete = Arc::new(holder);
        concrete
    });
    assert_eq!(
        any_message_registry::register_type(
            "rrr.test.StatefulFactory".to_owned(),
            TypeId::of::<GraphPayload>(),
            stateful_factory,
        ),
        0_i32
    );
    assert_eq!(
        any_message_registry::create(&"rrr.test.StatefulFactory".to_owned())
            .unwrap()
            .kind(),
        1_i32
    );
    assert_eq!(
        any_message_registry::create(&"rrr.test.StatefulFactory".to_owned())
            .unwrap()
            .kind(),
        2_i32
    );

    reset_and_register();
    assert!(any_message_registry::is_registered_name(
        &GRAPH_NAME.to_owned()
    ));
    assert!(any_message_registry::is_registered_type(TypeId::of::<
        GraphPayload,
    >()));
    assert_eq!(
        any_message_registry::name_for_type_owned(TypeId::of::<GraphPayload>()),
        GRAPH_NAME
    );

    let first = any_message_registry::create(&GRAPH_NAME.to_owned()).unwrap();
    let second = any_message_registry::create(&GRAPH_NAME.to_owned()).unwrap();
    assert!(!Arc::ptr_eq(&first, &second));
}

#[test]
fn pack_and_unpack_retain_payload_identity_and_reject_wrong_types() {
    let _test_guard = TEST_LOCK.lock().unwrap();
    reset_and_register();

    let payload = Arc::new(GraphPayload {
        node_count: 42_i32,
        label: "hello".to_owned(),
    });
    let message = AnyMessage::pack(payload.clone());

    assert_eq!(message.type_name_, GRAPH_NAME);
    assert!(message.is_a::<GraphPayload>());
    assert!(!message.is_a::<OtherPayload>());
    let recovered = message.unpack::<GraphPayload>().unwrap();
    assert!(Arc::ptr_eq(&payload, &recovered));
    assert!(message.unpack::<OtherPayload>().is_none());

    let spoofed = AnyMessage::pack_as(OTHER_NAME.to_owned(), payload);
    assert!(spoofed.is_a::<OtherPayload>());
    assert!(spoofed.unpack::<OtherPayload>().is_none());
}

#[test]
fn direct_and_free_archive_entry_points_preserve_wire_bytes_and_payload() {
    let _test_guard = TEST_LOCK.lock().unwrap();
    reset_and_register();

    let payload = Arc::new(GraphPayload {
        node_count: 0x1234_5678_i32,
        label: "wire-trip".to_owned(),
    });
    let outgoing = AnyMessage::pack(payload);

    let mut direct_writer = BinaryWriteArchive::new();
    outgoing.save(&mut direct_writer);

    let mut free_writer = BinaryWriteArchive::new();
    serialize(&outgoing, &mut free_writer);
    assert_eq!(free_writer.as_bytes(), direct_writer.as_bytes());
    assert_eq!(free_writer.as_bytes()[0], GRAPH_NAME.len() as u8);
    assert_eq!(
        &free_writer.as_bytes()[1..1 + GRAPH_NAME.len()],
        GRAPH_NAME.as_bytes()
    );

    let bytes = free_writer.into_bytes();
    let mut direct_reader = BinaryReadArchive::new(&bytes);
    let mut direct_decoded = AnyMessage::default();
    direct_decoded.load(&mut direct_reader);
    assert_eq!(direct_reader.remaining(), 0_usize);
    assert_eq!(
        direct_decoded.unpack::<GraphPayload>().unwrap().as_ref(),
        &GraphPayload {
            node_count: 0x1234_5678_i32,
            label: "wire-trip".to_owned(),
        }
    );

    let mut free_reader = BinaryReadArchive::new(&bytes);
    let mut free_decoded = AnyMessage::default();
    deserialize(&mut free_decoded, &mut free_reader);
    assert_eq!(free_reader.remaining(), 0_usize);
    assert_eq!(free_decoded.type_name_, GRAPH_NAME);
    assert!(free_decoded.is_a::<GraphPayload>());
}

#[test]
fn an_alias_factory_decodes_but_the_first_name_remains_canonical() {
    let _test_guard = TEST_LOCK.lock().unwrap();
    reset_and_register();

    const ALIAS: &str = "graph.alias.v1";
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
    let mut writer = BinaryWriteArchive::new();
    serialize(&outgoing, &mut writer);

    let mut reader = BinaryReadArchive::new(writer.as_bytes());
    let mut incoming = AnyMessage::default();
    deserialize(&mut incoming, &mut reader);
    assert_eq!(incoming.type_name_, ALIAS);
    assert!(!incoming.is_a::<GraphPayload>());
    assert!(incoming.unpack::<GraphPayload>().is_none());
}

#[test]
fn owner_records_the_strict_generation_recipe() {
    let owner = include_str!("../src/rpc/any_message.rs");

    for required in [
        "pub struct AnyMessage",
        "pub type Factory = Box<dyn FnMut() -> SerializableProxy + Send + Sync>",
        "name_by_type_hash: HashMap<usize, LegacyStdString>",
        "unsafe { cpp_rusty::arc_make_default::<T>() }",
        "unsafe { cpp_rusty::type_id_hash_code(type_id) }",
        "let concrete: Arc<serializable::details::SerializableSharedPtrHolder<T>>",
        "let factory: any_message_registry::Factory",
        "None => Default::default()",
    ] {
        assert!(owner.contains(required), "missing owner recipe: {required}");
    }
}
