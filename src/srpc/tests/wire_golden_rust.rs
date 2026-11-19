// Tier 2.2 of docs/testing-plan.md: golden wire vectors.
//
// Round-trip property tests (1.1) prove the encoder and decoder AGREE with
// each other, but an implementation is its own oracle there -- both halves
// could drift together and stay self-consistent yet wire-incompatible.
// Golden vectors pin the EXACT bytes, so a change to either half that alters
// the wire is caught. Because both lanes (rustc and the generated C++) are
// produced from these same canonical sources, pinning the Rust encoder's
// bytes also pins the C++ lane's -- this is the affordable slice of
// differential/cross-lane testing.
//
// Two byte-order regimes:
//   * SparseInt v32/v64 are MSB-first varints, endianness-independent, so
//     their golden bytes are portable and asserted on every platform.
//   * Fixed-width integers and f64 are written by copying the value's own
//     bytes with NO byte-order normalization (the deliberate native-endian
//     wire, book ch.7/10). Their golden bytes are little-endian; asserted
//     only on little-endian targets (x86_64 / aarch64-LE, what SRPC targets)
//     and documented as the native-endian contract, not portability.
//
// Vectors captured from the current encoder and reviewed by hand against the
// SparseInt unary-length-prefix scheme before pinning.

use srpc::basetypes::{v32 as SerializableV32, v64 as SerializableV64};
use srpc::serializable::{
    make_sink_proxy_buffer, make_source_proxy_buffer, BinaryReadArchive, BinaryWriteArchive,
    BufferSink, BufferSource, Deserialize, Serialize,
};

#[allow(unsafe_code)]
fn encode(f: impl FnOnce(&mut BinaryWriteArchive)) -> Vec<u8> {
    let mut sink = BufferSink { bytes: Vec::new() };
    {
        // SAFETY: `sink` outlives the archive that borrows it.
        let mut ar = BinaryWriteArchive {
            sink_: unsafe { make_sink_proxy_buffer(&raw mut sink) },
        };
        f(&mut ar);
    }
    sink.bytes.clone()
}

fn v32_bytes(v: i32) -> Vec<u8> {
    encode(|a| Serialize::serialize(&SerializableV32::new(v), a))
}
fn v64_bytes(v: i64) -> Vec<u8> {
    encode(|a| Serialize::serialize(&SerializableV64::new(v), a))
}

#[test]
fn v32_golden_vectors_are_portable_and_decode_back() {
    // (value, exact wire bytes). MSB-first varint, byte 0 carries a unary
    // length prefix in its high bits.
    let cases: [(i32, &[u8]); 5] = [
        (0, &[0]),
        (1, &[1]),
        (-1, &[127]),
        (300, &[129, 44]),
        (70_000, &[193, 17, 112]),
    ];
    for (val, gold) in cases {
        assert_eq!(v32_bytes(val), gold, "v32({val}) wire bytes");
        // Decoder accepts the exact golden bytes.
        let mut src = BufferSource::new(gold.as_ptr(), gold.len());
        #[allow(unsafe_code)]
        let mut ar = BinaryReadArchive::new(unsafe {
            make_source_proxy_buffer(&raw mut src)
        });
        let mut back = SerializableV32::new(0);
        Deserialize::deserialize(&mut back, &mut ar);
        assert_eq!(back.get(), val, "v32({val}) decodes from golden bytes");
    }
}

#[test]
fn v64_golden_vectors_are_portable_and_decode_back() {
    let cases: [(i64, &[u8]); 2] = [
        (1_000_000_000_000, &[248, 232, 212, 165, 16, 0]),
        (-9001, &[223, 220, 215]),
    ];
    for (val, gold) in cases {
        assert_eq!(v64_bytes(val), gold, "v64({val}) wire bytes");
        let mut src = BufferSource::new(gold.as_ptr(), gold.len());
        #[allow(unsafe_code)]
        let mut ar = BinaryReadArchive::new(unsafe {
            make_source_proxy_buffer(&raw mut src)
        });
        let mut back = SerializableV64::new(0);
        Deserialize::deserialize(&mut back, &mut ar);
        assert_eq!(back.get(), val, "v64({val}) decodes from golden bytes");
    }
}

// Fixed-width and f64 goldens are native-endian by design. Pin them on
// little-endian targets (what SRPC targets); on a big-endian host the wire
// would differ by design, so the assertion would be wrong to make.
#[cfg(target_endian = "little")]
#[test]
fn fixed_width_golden_vectors_are_native_little_endian() {
    assert_eq!(encode(|a| Serialize::serialize(&42i32, a)), [42, 0, 0, 0], "i32(42) LE");
    assert_eq!(
        encode(|a| Serialize::serialize(&0x0102_0304_0506_0708i64, a)),
        [8, 7, 6, 5, 4, 3, 2, 1],
        "i64 LE: least-significant byte first"
    );
    assert_eq!(encode(|a| Serialize::serialize(&258u16, a)), [2, 1], "u16(258) LE");
    assert_eq!(
        encode(|a| Serialize::serialize(&1.5f64, a)),
        [0, 0, 0, 0, 0, 0, 248, 63],
        "f64(1.5) IEEE-754 LE"
    );
}
