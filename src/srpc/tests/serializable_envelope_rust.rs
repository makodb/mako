#![allow(unsafe_code)]

use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, SerializablePayload, SerializableRegistry, Serialize, Deserialize};
use srpc::serializable_envelope::{PayloadMember, SerializableEnvelope};
use std::sync::Arc;
mod serialization_helpers;
use serialization_helpers::{encode, decode};

struct PayloadSet;

#[derive(Clone, Default, Debug, PartialEq)]
struct Payload {
    value: i32,
}

impl SerializablePayload for Payload {
    fn save(&self, archive: &mut BinaryWriteArchive) { self.value.serialize(archive); }
    fn load(&mut self, archive: &mut BinaryReadArchive) { self.value.deserialize(archive); }
    fn kind(&self) -> i32 { 61 }
}

impl PayloadMember<PayloadSet> for Payload {
    const KIND: i32 = 61;
}

#[test]
fn empty_clone_pack_and_unpack_preserve_payload_ownership() {
    let empty = SerializableEnvelope::<PayloadSet>::default();
    assert!(!empty.has_value());
    assert_eq!(empty.kind(), 0);
    assert!(empty == empty.clone());
    assert!(empty.unpack::<Payload>().is_null());
    assert!(empty.unpack_shared::<Payload>().is_none());

    let payload = Arc::new(Payload { value: 7 });
    let packed = SerializableEnvelope::<PayloadSet>::pack_aliased(payload.clone());
    assert!(packed.has_value());
    assert_eq!(packed.kind(), 61);
    assert_eq!(packed.kind_, 61);
    assert!(packed.is_a::<Payload>());
    assert!(Arc::ptr_eq(&packed.unpack_shared::<Payload>().unwrap(), &payload));
    let clone = packed.clone();
    assert!(clone == packed);
    assert_eq!(clone.unpack::<Payload>(), Arc::as_ptr(&payload));

    let copied = SerializableEnvelope::<PayloadSet>::pack(&*payload);
    assert!(copied != packed);
    assert_eq!(unsafe { &*copied.unpack::<Payload>() }, &*payload);
    assert_ne!(copied.unpack::<Payload>(), Arc::as_ptr(&payload));
}

#[test]
fn wire_round_trip_preserves_kind_and_payload() {
    SerializableRegistry::clear_for_testing();
    SerializableRegistry::reg::<Payload>(61);
    let outgoing = SerializableEnvelope::<PayloadSet>::pack(&Payload { value: 0x1234_5678 });
    let encoded = encode(|archive| outgoing.save(archive));
    let mut expected = vec![61];
    expected.extend_from_slice(&0x1234_5678_i32.to_ne_bytes());
    assert_eq!(encoded, expected);
    let (mut incoming, remaining) = decode(&expected, |archive| {
        let mut envelope = SerializableEnvelope::<PayloadSet>::default();
        envelope.load(archive);
        envelope
    });
    assert_eq!(remaining, 0);
    assert_eq!(incoming.kind(), 61);
    assert_eq!(unsafe { (*incoming.unpack::<Payload>()).value }, 0x1234_5678);
    // No clone or borrowed shared handle survives this mutation window.
    unsafe { (*incoming.unpack_mut::<Payload>()).value = 99; }
    assert_eq!(unsafe { (*incoming.unpack::<Payload>()).value }, 99);
    SerializableRegistry::clear_for_testing();
}
