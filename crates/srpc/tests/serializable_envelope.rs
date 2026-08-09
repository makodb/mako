#![allow(unsafe_code)]

use srpc::rpc::serializable_envelope::cpp::rrr::serializable::{
    BinaryReadArchive, BinaryWriteArchive, SerializablePayload, SerializableRegistry,
};
use srpc::rpc::serializable_envelope::{
    deserialize, marshallable_cast, serialize, PayloadMember, SerializableEnvelope,
};
use std::sync::Arc;

struct EnvelopeTestSet;

#[derive(Clone, Default, Debug, Eq, PartialEq)]
struct Alpha {
    value: i32,
    tag: u8,
}

impl PayloadMember<EnvelopeTestSet> for Alpha {
    const KIND: i32 = 60_i32;
}

impl SerializablePayload for Alpha {
    fn kind(&self) -> i32 {
        <Alpha as PayloadMember<EnvelopeTestSet>>::KIND
    }

    fn save(&self, archive: &mut BinaryWriteArchive) {
        archive.write_bytes(&self.value.to_le_bytes());
        archive.write_bytes(&[self.tag]);
    }

    fn load(&mut self, archive: &mut BinaryReadArchive) {
        let mut value = [0_u8; 4];
        archive.read_exact(&mut value);
        self.value = i32::from_le_bytes(value);
        let mut tag = [0_u8; 1];
        archive.read_exact(&mut tag);
        self.tag = tag[0];
    }
}

#[derive(Clone, Default, Debug, Eq, PartialEq)]
struct Beta {
    value: u16,
}

impl PayloadMember<EnvelopeTestSet> for Beta {
    const KIND: i32 = 61_i32;
}

impl SerializablePayload for Beta {
    fn kind(&self) -> i32 {
        <Beta as PayloadMember<EnvelopeTestSet>>::KIND
    }

    fn save(&self, archive: &mut BinaryWriteArchive) {
        archive.write_bytes(&self.value.to_le_bytes());
    }

    fn load(&mut self, archive: &mut BinaryReadArchive) {
        let mut value = [0_u8; 2];
        archive.read_exact(&mut value);
        self.value = u16::from_le_bytes(value);
    }
}

fn register_payloads() {
    SerializableRegistry::register::<Alpha>(<Alpha as PayloadMember<EnvelopeTestSet>>::KIND);
    SerializableRegistry::register::<Beta>(<Beta as PayloadMember<EnvelopeTestSet>>::KIND);
}

#[test]
fn native_public_shim_supports_nominal_membership_and_empty_state() {
    register_payloads();
    assert_eq!(<Alpha as PayloadMember<EnvelopeTestSet>>::KIND, 60_i32);
    assert_eq!(<Beta as PayloadMember<EnvelopeTestSet>>::KIND, 61_i32);

    let empty = SerializableEnvelope::<EnvelopeTestSet>::default();
    let another = SerializableEnvelope::<EnvelopeTestSet>::default();
    assert!(!empty.has_value());
    assert_eq!(empty.kind(), 0_i32);
    assert_eq!(empty.kind_, 0_i32);
    assert!(empty.unpack::<Alpha>().is_null());
    assert!(empty.unpack_shared::<Alpha>().is_none());
    assert!(!empty.is_a::<Alpha>());
    assert!(empty == another);
}

#[test]
fn pack_copies_while_pack_aliased_retains_the_original_arc() {
    register_payloads();
    let mut source = Alpha { value: 7, tag: 3 };
    let packed = SerializableEnvelope::<EnvelopeTestSet>::pack(&source);
    source.value = 99;
    assert_eq!(source.value, 99_i32);

    assert!(packed.has_value());
    assert_eq!(packed.kind(), 60_i32);
    assert_eq!(packed.kind_, 60_i32);
    assert_eq!(unsafe { &*packed.unpack::<Alpha>() }.value, 7_i32);
    assert!(packed.unpack::<Beta>().is_null());
    assert!(packed.unpack_shared::<Beta>().is_none());
    assert!(!packed.is_a::<Beta>());

    let payload = Arc::new(Alpha { value: 41, tag: 9 });
    let aliased = SerializableEnvelope::<EnvelopeTestSet>::pack_aliased(payload.clone());
    let recovered = aliased.unpack_shared::<Alpha>().unwrap();
    assert!(Arc::ptr_eq(&payload, &recovered));
    assert_eq!(recovered.value, 41_i32);
    assert_eq!(recovered.tag, 9_u8);
}

#[test]
fn clone_shares_the_holder_but_independent_packs_do_not() {
    register_payloads();
    let value = Alpha { value: -5, tag: 1 };
    let first = SerializableEnvelope::<EnvelopeTestSet>::pack(&value);
    let clone = first.clone();
    let second = SerializableEnvelope::<EnvelopeTestSet>::pack(&value);

    assert!(first == clone);
    assert!(first != second);
    let first_payload = first.unpack_shared::<Alpha>().unwrap();
    let clone_payload = clone.unpack_shared::<Alpha>().unwrap();
    assert!(Arc::ptr_eq(&first_payload, &clone_payload));

    let cast = marshallable_cast::<Alpha, EnvelopeTestSet>(&clone).unwrap();
    assert!(Arc::ptr_eq(&clone_payload, &cast));
}

#[test]
fn v32_kind_and_payload_bytes_round_trip_through_both_archive_entry_points() {
    register_payloads();
    let value = Alpha {
        value: 0x1234_5678_i32,
        tag: 0xAB_u8,
    };
    let envelope = SerializableEnvelope::<EnvelopeTestSet>::pack(&value);

    let mut direct_writer = BinaryWriteArchive::new();
    envelope.save(&mut direct_writer);
    assert_eq!(
        direct_writer.as_bytes(),
        &[0x3C_u8, 0x78_u8, 0x56_u8, 0x34_u8, 0x12_u8, 0xAB_u8]
    );

    let mut free_writer = BinaryWriteArchive::new();
    serialize(&envelope, &mut free_writer);
    assert_eq!(free_writer.as_bytes(), direct_writer.as_bytes());

    let bytes = free_writer.into_bytes();
    let mut direct_reader = BinaryReadArchive::new(&bytes);
    let mut decoded = SerializableEnvelope::<EnvelopeTestSet>::default();
    decoded.load(&mut direct_reader);
    assert_eq!(direct_reader.remaining(), 0_usize);
    assert_eq!(decoded.kind(), 60_i32);
    assert_eq!(unsafe { &*decoded.unpack::<Alpha>() }, &value);

    let mut free_reader = BinaryReadArchive::new(&bytes);
    let mut decoded_free = SerializableEnvelope::<EnvelopeTestSet>::default();
    deserialize(&mut decoded_free, &mut free_reader);
    assert_eq!(free_reader.remaining(), 0_usize);
    assert_eq!(
        decoded_free.unpack_shared::<Alpha>().unwrap().as_ref(),
        &value
    );
}

#[test]
fn owner_and_generated_contract_exclude_the_cargo_only_shim() {
    let owner = include_str!("../src/rpc/serializable_envelope.rs");
    let generated = include_str!("../cpp/generated/rrr.serializable_envelope.cppm");

    for required in [
        "pub trait PayloadMember<Set>",
        "pub struct SerializableEnvelope<PayloadSet>",
        "pub kind_: i32",
        "pub fn pack<T>",
        "pub fn pack_aliased<T>",
        "pub fn unpack<T>",
        "pub fn unpack_shared<T>",
        "pub fn unpack_mut<T>",
        "pub fn is_a<T>",
        "pub fn marshallable_cast<T, PayloadSet>",
        "pub trait SerializablePayload",
        "pub struct SerializableRegistry",
    ] {
        assert!(
            owner.contains(required),
            "missing owner contract: {required}"
        );
    }

    assert_eq!(
        generated
            .matches("requires (PayloadMember<PayloadSet, T>::value")
            .count(),
        8_usize
    );
    assert_eq!(generated.matches("rusty::clone_like<T>").count(), 1_usize);
    assert!(generated.contains("int32_t kind_;"));
    assert!(generated.contains("rusty::Option<::rrr::SerializableProxy> inner_;"));
    assert!(generated.contains("::rrr::SerializableRegistry::create"));
    assert!(!generated.contains("SerializablePayload"));
    assert!(!generated.contains("SerializableBaseApi"));
    assert!(!generated.contains("EmptySerializableBase"));
    assert!(!generated.contains("TODO transpiler"));
    assert!(!generated.contains("UNSUPPORTED"));
}
