mod serialization_helpers;
use serialization_helpers::{encode, decode};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, SerializablePayload};
use std::any::TypeId;

use srpc::serializable::{
    BufferSink, BufferSource, DecodeError, Deserialize, Serializable, SerializableRegistry,
    Serialize, SinkBase, SourceBase, make_serializable_proxy_copy,
    make_serializable_proxy_default,
};
use srpc::basetypes::SparseInt;
use srpc::basetypes::{v32 as SerializableV32, v64 as SerializableV64};

fn expected_sparse(value: i64) -> (usize, [u8; 9]) {
    let size = if (-64..=63).contains(&value) {
        1
    } else if (-8_192..=8_191).contains(&value) {
        2
    } else if (-1_048_576..=1_048_575).contains(&value) {
        3
    } else if (-134_217_728..=134_217_727).contains(&value) {
        4
    } else if (-17_179_869_184..=17_179_869_183).contains(&value) {
        5
    } else if (-2_199_023_255_552..=2_199_023_255_551).contains(&value) {
        6
    } else if (-281_474_976_710_656..=281_474_976_710_655).contains(&value) {
        7
    } else {
        // The 8-byte (0xFE) rung is retired (docs/testing-plan.md 4.1);
        // everything past the 7-byte range uses the 9-byte (0xFF) encoding.
        9
    };
    let mut out = [0u8; 9];
    if size <= 7 {
        let raw = value as u64;
        for (index, byte) in out[..size].iter_mut().enumerate() {
            *byte = (raw >> (8 * (size - 1 - index))) as u8;
        }
        let prefix = [0, 0, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc][size];
        out[0] &= 0xff >> size;
        out[0] |= prefix;
    } else {
        out[0] = 0xff;
        out[1..].copy_from_slice(&(value as u64).to_be_bytes());
    }
    (size, out)
}

#[test]
#[allow(unsafe_code)]
fn sparse_encoding_matches_independent_wire_oracle() {
    let values = [
        i64::MIN,
        -36_028_797_018_963_969,
        -36_028_797_018_963_968,
        -281_474_976_710_657,
        -281_474_976_710_656,
        -2_199_023_255_553,
        -2_199_023_255_552,
        -17_179_869_185,
        -17_179_869_184,
        -134_217_729,
        -134_217_728,
        -1_048_577,
        -1_048_576,
        -8_193,
        -8_192,
        -65,
        -64,
        -1,
        0,
        1,
        63,
        64,
        8_191,
        8_192,
        1_048_575,
        1_048_576,
        134_217_727,
        134_217_728,
        17_179_869_183,
        17_179_869_184,
        2_199_023_255_551,
        2_199_023_255_552,
        281_474_976_710_655,
        281_474_976_710_656,
        36_028_797_018_963_967,
        36_028_797_018_963_968,
        i64::MAX,
    ];
    for value in values {
        let (expected_size, expected) = expected_sparse(value);
        let mut actual = [0xa5; 10];
        let actual_size = unsafe { SparseInt::dump64(value, actual.as_mut_ptr()) };
        assert_eq!(actual_size, expected_size, "value {value}");
        assert_eq!(&actual[..expected_size], &expected[..expected_size], "value {value}");
        assert_eq!(actual[expected_size], 0xa5, "value {value}");
        assert_eq!(SparseInt::buf_size(actual[0]), expected_size);
        assert_eq!(unsafe { SparseInt::load64(actual.as_ptr()) }, value);

        if let Ok(value32) = i32::try_from(value) {
            let mut actual32 = [0xa5; 6];
            let size32 = unsafe { SparseInt::dump32(value32, actual32.as_mut_ptr()) };
            let (expected32_size, expected32) = expected_sparse(value32 as i64);
            assert_eq!(size32, expected32_size);
            assert_eq!(&actual32[..size32], &expected32[..size32]);
            assert_eq!(actual32[size32], 0xa5);
            assert_eq!(unsafe { SparseInt::load32(actual32.as_ptr()) }, value32);
        }
    }
}

#[test]
#[allow(unsafe_code)]
fn buffer_source_sink_and_sparse_leaf_impls_match_wire_contract() {
    let payload = [0x10, 0x20, 0x30, 0x40, 0x50];
    let mut sink = BufferSink { bytes: vec![1, 2] };
    unsafe { SinkBase::write_bytes(&mut sink, payload.as_ptr(), payload.len()) };
    assert_eq!(sink.bytes, [1, 2, 0x10, 0x20, 0x30, 0x40, 0x50]);
    unsafe { SinkBase::write_bytes(&mut sink, core::ptr::null(), 0) };
    assert_eq!(sink.bytes.len(), 7);

    let mut source = BufferSource::new(payload.as_ptr(), payload.len());
    let mut first = [0u8; 3];
    assert_eq!(
        unsafe { SourceBase::read_bytes(&mut source, first.as_mut_ptr(), 3) },
        3
    );
    assert_eq!(first, [0x10, 0x20, 0x30]);
    assert_eq!(source.pos(), 3);
    assert_eq!(source.remaining(), 2);
    assert!(!source.eof());
    let mut rest = [0u8; 4];
    assert_eq!(
        unsafe { SourceBase::read_bytes(&mut source, rest.as_mut_ptr(), 4) },
        2
    );
    assert_eq!(&rest[..2], &[0x40, 0x50]);
    assert!(source.eof());

    // Pin the trait implementations themselves.
    let mut v32 = SerializableV32::new(-8_193);
    let mut v64 = SerializableV64::new(36_028_797_018_963_968);
    let _serialize32: fn(&SerializableV32, &mut srpc::serializable::BinaryWriteArchive) =
        <SerializableV32 as Serialize>::serialize;
    let _serialize64: fn(&SerializableV64, &mut srpc::serializable::BinaryWriteArchive) =
        <SerializableV64 as Serialize>::serialize;
    let _deserialize32: fn(&mut SerializableV32, &mut srpc::serializable::BinaryReadArchive) =
        <SerializableV32 as Deserialize>::deserialize;
    let _deserialize64: fn(&mut SerializableV64, &mut srpc::serializable::BinaryReadArchive) =
        <SerializableV64 as Deserialize>::deserialize;
    v32.set(v32.get());
    v64.set(v64.get());
}

#[test]
fn serializable_kind_is_exact_and_nonzero() {
    assert_eq!(Serializable::<7>::static_kind(), 7);
    assert_eq!(Serializable::<-9> {}.kind(), -9);
}


#[derive(Default, Clone, Debug, PartialEq)]
struct Payload { value: i64, values: Vec<i32> }

impl SerializablePayload for Payload {
    fn save(&self, archive: &mut BinaryWriteArchive) {
        self.value.serialize(archive);
        self.values.serialize(archive);
    }
    fn load(&mut self, archive: &mut BinaryReadArchive) {
        self.value.deserialize(archive);
        self.values.deserialize(archive);
    }
    fn kind(&self) -> i32 { 61 }
}

#[test]
#[allow(unsafe_code)]
fn proxy_factories_and_registry_preserve_payloads() {
    let original = Payload { value: -9_001, values: vec![2, 3, 5] };
    let copy = make_serializable_proxy_copy(&original);
    assert_eq!(copy.kind(), 61);
    let bytes = encode(|archive| copy.save(archive));
    let expected = encode(|archive| original.save(archive));
    assert_eq!(bytes, expected);

    let mut fresh = make_serializable_proxy_default::<Payload>();
    let (_, remaining) = decode(&bytes, |archive| {
        std::sync::Arc::get_mut(&mut fresh).unwrap().load(archive);
    });
    assert_eq!(remaining, 0);
    let holder = unsafe { srpc::serializable::serializable_holder_of::<Payload>(std::sync::Arc::as_ptr(&fresh)) };
    assert!(!holder.is_null());
    assert_eq!(unsafe { &*(*holder).ptr }, &original);

    SerializableRegistry::clear_for_testing();
    SerializableRegistry::reg::<Payload>(61);
    assert!(SerializableRegistry::is_registered(61));
    let created = SerializableRegistry::create(61);
    assert_eq!(created.kind(), 61);
    assert_eq!(created.payload_type_id(), TypeId::of::<Payload>());
    let mut calls = 0;
    srpc::serializable::serializable_registry_register_factory(
        62,
        Box::new(move || {
            calls += 1;
            srpc::serializable::make_serializable_proxy(std::sync::Arc::new(Payload {
                value: calls,
                values: vec![calls as i32],
            }))
        }),
    );
    for expected_call in 1..=2 {
        let created = SerializableRegistry::create(62);
        let holder = unsafe { srpc::serializable::serializable_holder_of::<Payload>(std::sync::Arc::as_ptr(&created)) };
        assert_eq!(unsafe { &*(*holder).ptr }.value, expected_call);
    }
    SerializableRegistry::clear_for_testing();
}

#[test]
#[should_panic(expected = "Serializable kind 0 is reserved")]
fn serializable_kind_zero_is_rejected() {
    let _ = Serializable::<0>::static_kind();
}

// The proxies used to be unreachable in the Rust lane: `rusty::make_box` was a
// diverging stub, so every `make_*_proxy_*` panicked.  Even once that was fixed,
// `BinaryWriteArchive` dispatched through `rusty::srpc_sink_write`, whose facade
// body was empty -- so writes were silently discarded.  These three tests pin
// both halves, and they assert against the ORIGINAL sink/source rather than the
// proxy, so a proxy that copied its target instead of borrowing it would fail.

#[test]
#[allow(unsafe_code)]
fn sink_proxy_forwards_writes_to_the_pointed_to_buffer_sink() {
    let mut sink = BufferSink { bytes: Vec::new() };
    let first = [0xAAu8, 0xBB];
    let second = [0xCCu8];
    {
        // SAFETY: `sink` outlives the proxy and is not moved while borrowed.
        let mut proxy = unsafe { srpc::serializable::make_sink_proxy_buffer(&raw mut sink) };
        // SAFETY: both slices are live for the duration of each call.
        unsafe {
            proxy.write_bytes(first.as_ptr(), first.len());
            proxy.write_bytes(second.as_ptr(), second.len());
        }
    }
    // Two writes through one proxy, observed on the caller's own sink: this is
    // what fails if the proxy ever holds a copy.
    assert_eq!(sink.bytes, vec![0xAA, 0xBB, 0xCC]);
}

#[test]
#[allow(unsafe_code)]
fn source_proxy_forwards_reads_and_advances_the_pointed_to_cursor() {
    let data = [1u8, 2, 3, 4, 5];
    let mut source = BufferSource::new(data.as_ptr(), data.len());
    let mut out = [0u8; 5];
    let read = {
        // SAFETY: `source` and `data` both outlive the proxy.
        let mut proxy = unsafe { srpc::serializable::make_source_proxy_buffer(&raw mut source) };
        // SAFETY: `out` is live and large enough for the requested length.
        unsafe { proxy.read_bytes(out.as_mut_ptr(), out.len()) }
    };
    assert_eq!(read, 5);
    assert_eq!(out, data);
    // The cursor moved on the caller's source, not on a copy.
    assert_eq!(source.pos(), 5);
    assert!(source.eof());
}

#[test]
#[allow(unsafe_code)]
fn archive_round_trips_a_leaf_through_both_proxies() {
    let mut sink = BufferSink { bytes: Vec::new() };
    {
        // SAFETY: `sink` outlives the archive that borrows it.
        let mut archive = srpc::serializable::BinaryWriteArchive {
            sink_: unsafe { srpc::serializable::make_sink_proxy_buffer(&raw mut sink) },
        };
        <SerializableV64 as Serialize>::serialize(&SerializableV64::new(-9_001), &mut archive);
        <SerializableV32 as Serialize>::serialize(&SerializableV32::new(77), &mut archive);
    }
    // If `srpc_sink_write` were still the empty facade stub, this would be 0.
    assert!(!sink.bytes.is_empty());

    let encoded = sink.bytes.clone();
    let mut source = BufferSource::new(encoded.as_ptr(), encoded.len());
    let (v64, v32) = {
        // SAFETY: `source` and `encoded` both outlive the archive.
        let mut archive = srpc::serializable::BinaryReadArchive::new(unsafe {
            srpc::serializable::make_source_proxy_buffer(&raw mut source)
        });
        let mut v64 = SerializableV64::new(0);
        let mut v32 = SerializableV32::new(0);
        <SerializableV64 as Deserialize>::deserialize(&mut v64, &mut archive);
        <SerializableV32 as Deserialize>::deserialize(&mut v32, &mut archive);
        (v64, v32)
    };
    assert_eq!(v64.get(), -9_001);
    assert_eq!(v32.get(), 77);
    assert!(source.eof());
}

// Exercise the same generic entry points used by RPC header encoding.
#[test]
#[allow(unsafe_code)]
fn canonical_dispatchers_round_trip_header_leaves() {
    let mut sink = BufferSink { bytes: Vec::new() };
    {
        let mut ar = srpc::serializable::BinaryWriteArchive {
            // SAFETY: `sink` outlives the archive that borrows it.
            sink_: unsafe { srpc::serializable::make_sink_proxy_buffer(&raw mut sink) },
        };
        // SAFETY (all six facade calls below): foreign named-module boundary;
        // both borrows are held only for the duration of each call.
        {
            srpc::serializable::Serialize_::serialize(&SerializableV64::new(-77_000), &mut ar);
            srpc::serializable::Serialize_::serialize(&123_456_789_i64, &mut ar);
            srpc::serializable::Serialize_::serialize(&SerializableV32::new(63), &mut ar);
        }
    }
    assert!(!sink.bytes.is_empty());

    let encoded = sink.bytes.clone();
    let mut source = BufferSource::new(encoded.as_ptr(), encoded.len());
    {
        // SAFETY: `source` and `encoded` both outlive the archive.
        let mut ar = srpc::serializable::BinaryReadArchive::new(unsafe {
            srpc::serializable::make_source_proxy_buffer(&raw mut source)
        });
        let mut v64 = SerializableV64::new(0);
        let mut plain = 0i64;
        let mut v32 = SerializableV32::new(0);
        // SAFETY: same facade-boundary contract as the write side.
        {
            srpc::serializable::Deserialize_::deserialize(&mut v64, &mut ar);
            srpc::serializable::Deserialize_::deserialize(&mut plain, &mut ar);
            srpc::serializable::Deserialize_::deserialize(&mut v32, &mut ar);
        }
        assert_eq!(v64.get(), -77_000);
        assert_eq!(plain, 123_456_789);
        assert_eq!(v32.get(), 63);
    }
    assert!(source.eof());
}


#[test]
fn containers_round_trip_through_canonical_generic_dispatch() {
    let original = vec![vec![1i64, 2, 3], vec![], vec![-9_001, i64::MAX]];
    let bytes = encode(|archive| srpc::serializable::Serialize_::serialize(&original, archive));
    let mut expected = vec![3, 3];
    for value in [1i64, 2, 3] { expected.extend_from_slice(&value.to_ne_bytes()); }
    expected.extend_from_slice(&[0, 2]);
    for value in [-9_001i64, i64::MAX] { expected.extend_from_slice(&value.to_ne_bytes()); }
    assert_eq!(bytes, expected);
    let (restored, remaining) = decode(&expected, |archive| {
        let mut restored = Vec::<Vec<i64>>::new();
        srpc::serializable::Deserialize_::deserialize(&mut restored, archive);
        restored
    });
    assert_eq!(restored, original);
    assert_eq!(remaining, 0);
}

fn round_trip<T: Serialize + Deserialize + Default>(value: &T) -> T {
    let bytes = encode(|archive| value.serialize(archive));
    let (restored, remaining) = decode(&bytes, |archive| {
        let mut restored = T::default();
        restored.deserialize(archive);
        restored
    });
    assert_eq!(remaining, 0);
    restored
}

#[test]
fn standard_collections_keep_their_rust_insert_semantics() {
    let mut ordered = std::collections::BTreeMap::new();
    ordered.insert(3i32, 30i32);
    ordered.insert(1, 10);
    ordered.insert(3, 99);
    assert_eq!(round_trip(&ordered), ordered);
    let mut unordered = std::collections::HashMap::new();
    unordered.insert(3i32, 30i32);
    unordered.insert(1, 10);
    unordered.insert(3, 99);
    assert_eq!(round_trip(&unordered), unordered);
    let ordered: std::collections::BTreeSet<_> = [3i32, 1, 3].into_iter().collect();
    let unordered: std::collections::HashSet<_> = [3i32, 1, 3].into_iter().collect();
    assert_eq!(round_trip(&ordered), ordered);
    assert_eq!(round_trip(&unordered), unordered);
}

#[test]
fn counted_traversal_preserves_sequence_order_without_container_models() {
    use srpc::serializable::{serialize_counted, deserialize_counted};
    let list: std::collections::LinkedList<i32> = [3, 1, 3].into_iter().collect();
    let bytes = encode(|archive| {
        let mut elements = list.iter();
        serialize_counted(list.len(), archive, |output| elements.next().unwrap().serialize(output));
    });
    assert_eq!(bytes, encode(|archive| vec![3i32, 1, 3].serialize(archive)));
    let (restored, remaining) = decode(&bytes, |archive| {
        let mut restored = std::collections::LinkedList::<i32>::new();
        restored.push_back(-1);
        deserialize_counted(&mut restored, archive,
            |storage, _| storage.clear(),
            |storage, input| {
                let mut value = 0i32;
                value.deserialize(input);
                storage.push_back(value);
            });
        restored
    });
    assert_eq!(restored, list);
    assert_eq!(remaining, 0);
}

#[test]
fn counted_map_decode_keeps_first_duplicate_and_consumes_all_values() {
    use srpc::serializable::deserialize_map_first;
    let bytes = encode(|archive| {
        srpc::basetypes::v64::new(3).serialize(archive);
        for (key, value) in [(3i32, 30i32), (1, 10), (3, 99)] {
            key.serialize(archive);
            value.serialize(archive);
        }
    });
    let (restored, remaining) = decode(&bytes, |archive| {
        let mut restored = std::collections::BTreeMap::<i32, i32>::new();
        deserialize_map_first(&mut restored, archive,
            |storage| storage.clear(),
            |storage, key, value| { storage.entry(key).or_insert(value); });
        restored
    });
    assert_eq!(restored.into_iter().collect::<Vec<_>>(), [(1, 10), (3, 30)]);
    assert_eq!(remaining, 0);
}

#[test]
#[allow(unsafe_code)]
fn raw_byte_helpers_preserve_invalid_utf8_and_embedded_nul() {
    use srpc::serializable::{serialize_bytes, deserialize_bytes_with};
    let original = [0xffu8, 0, 0xc0, 0x80, b'a'];
    let encoded = encode(|archive| unsafe { serialize_bytes(original.as_ptr(), original.len(), archive) });
    assert_eq!(encoded, [5, 0xff, 0, 0xc0, 0x80, b'a']);
    let (restored, remaining) = decode(&encoded, |archive| {
        let mut restored = vec![99u8; 20];
        unsafe { deserialize_bytes_with(&mut restored, archive,
            |storage, count| storage.resize(count, 0),
            |storage| storage.as_mut_ptr()); }
        restored
    });
    assert_eq!(restored, original);
    assert_eq!(remaining, 0);
}

#[test]
#[allow(unsafe_code)]
fn invalid_utf8_records_a_decode_error_without_panicking() {
    let encoded = [1u8, 0xff, 7, 0, 0, 0];
    let mut source = BufferSource::new(encoded.as_ptr(), encoded.len());
    let mut archive = BinaryReadArchive::new(unsafe {
        srpc::serializable::make_source_proxy_buffer(&raw mut source)
    });
    let mut text = String::from("unchanged");
    let mut following = 41i32;

    srpc::serializable::Deserialize_::deserialize(&mut text, &mut archive);
    srpc::serializable::Deserialize_::deserialize(&mut following, &mut archive);

    assert!(archive.error() == DecodeError::InvalidUtf8);
    assert!(archive.failed());
    assert!(text.is_empty());
    assert_eq!(following, 41, "a failed archive must stop decoding later fields");
    assert_eq!(source.remaining(), 4);
}

#[test]
fn standard_tuple_fields_match_pair_wire_order() {
    let original = (17i32, String::from("pair"));
    let bytes = encode(|archive| original.serialize(archive));
    let expected = encode(|archive| {
        original.0.serialize(archive);
        original.1.serialize(archive);
    });
    assert_eq!(bytes, expected);
    let (restored, remaining) = decode(&bytes, |archive| {
        let mut restored = (0i32, String::new());
        restored.deserialize(archive);
        restored
    });
    assert_eq!(restored, original);
    assert_eq!(remaining, 0);
}
