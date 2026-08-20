use rrr::serializable_envelope::{PayloadMember, SerializableEnvelope};

struct PayloadSet;

#[derive(Clone)]
struct Payload {
    value: i32,
}

impl PayloadMember<PayloadSet> for Payload {
    const KIND: i32 = 61;
}

#[test]
fn empty_clone_and_pack_state_are_rustc_checked() {
    let empty = SerializableEnvelope::<PayloadSet>::default();
    assert!(!empty.has_value());
    assert_eq!(empty.kind(), 0);
    assert!(empty == empty.clone());

    let payload = Payload { value: 7 };
    assert_eq!(payload.value, 7);
    let packed = SerializableEnvelope::<PayloadSet>::pack(&payload);
    assert!(packed.has_value());
    assert_eq!(packed.kind(), 0);

    // The rustc-only registry/holder facade is intentionally opaque. The
    // generated C++ consumer gate owns the concrete holder recovery oracle.
    assert!(!packed.is_a::<Payload>());
    assert!(packed.unpack::<Payload>().is_null());

    let _unsafe_mutation_boundary: unsafe fn(
        &mut SerializableEnvelope<PayloadSet>,
    ) -> *mut Payload = SerializableEnvelope::<PayloadSet>::unpack_mut::<Payload>;
}
