// Tier 1.1 of docs/testing-plan.md: property-based round-trip over the wire
// codecs.  `decode(encode(x)) == x` across the whole input space, with
// shrinking, which is exactly the test class that would have caught the
// SparseInt length-8 defect the moment it shipped instead of leaving it as a
// hand-pinned curiosity in basetypes_rust.rs.
//
// Two codecs are exercised:
//   * SparseInt v32/v64 -- the signed varint under `v32`/`v64` wire fields;
//   * the 4-byte frame header -- native-endian size word with the bit-31
//     extended-header flag.
//
// The historical SparseInt length-8 defect (dump64 reporting 8 while writing
// 9, dropping the low byte of any value in +/-[2^48, 2^55)) is FIXED as of
// plan item 4.1: the broken 8-byte (0xFE) rung is retired on the write side
// and those values now use the correct 9-byte (0xFF) encoding. The v64
// property below therefore covers the FULL i64 range, and the test that used
// to pin the defect now asserts the whole band round-trips.

use proptest::prelude::*;

use srpc::basetypes::SparseInt;
use srpc::frame_codec::{
    frame_codec_peek_header, frame_codec_write_header, kFrameHeaderSize,
    kMaxFramePayloadSize, FrameDecodeStatus, FrameHeader,
};


// The upper bound of the SparseInt 7-byte range (inclusive): values with a
// magnitude at or below this round-trip through the <=7-byte path unharmed.
const SPARSE_7BYTE_MAX: i64 = 281_474_976_710_655;

// Archive-realistic round trip: BinaryWriteArchive writes exactly the REPORTED
// byte count (`Serialize for v32/v64` in misc/serializable.rs calls
// `write_bytes(buf, bsize)`), so a faithful round trip persists only those
// `reported` bytes into a fresh zero-filled buffer before decoding. This is
// the path that reaches the wire -- and the one where the length-8 defect
// (dump64 writes 9 bytes but reports 8) actually manifests.
#[allow(unsafe_code)]
fn sparse_roundtrip_32(val: i32) -> (i32, usize) {
    let mut written = [0u8; 9];
    // SAFETY: written has 9 bytes; dump32 needs at most 5.
    let n = unsafe { SparseInt::dump32(val, written.as_mut_ptr()) };
    let mut persisted = [0u8; 9];
    persisted[..n].copy_from_slice(&written[..n]);
    // SAFETY: load reads a valid encoding prefix in a 9-byte buffer.
    let back = unsafe { SparseInt::load32(persisted.as_ptr()) };
    (back, n)
}

#[allow(unsafe_code)]
fn sparse_roundtrip_64(val: i64) -> (i64, usize) {
    let mut written = [0u8; 9];
    // SAFETY: written has 9 bytes; dump64 needs at most 9.
    let n = unsafe { SparseInt::dump64(val, written.as_mut_ptr()) };
    let mut persisted = [0u8; 9];
    persisted[..n].copy_from_slice(&written[..n]);
    // SAFETY: load reads a valid encoding prefix in a 9-byte buffer.
    let back = unsafe { SparseInt::load64(persisted.as_ptr()) };
    (back, n)
}

proptest! {
    // v32 is unaffected by the length-8 defect at every magnitude.
    #[test]
    fn v32_sparse_round_trips_for_every_i32(val in any::<i32>()) {
        let (back, n) = sparse_roundtrip_32(val);
        prop_assert_eq!(back, val, "v32 must round-trip");
        prop_assert!((1..=5).contains(&n), "v32 encodes in 1..=5 bytes, got {}", n);
        prop_assert_eq!(n, SparseInt::val_size(val as i64), "reported length matches val_size");
    }

    // v64 round-trips across the FULL i64 range now that the length-8 rung is
    // retired (item 4.1). The former defect band +/-[2^48, 2^55) is included.
    #[test]
    fn v64_sparse_round_trips_for_every_i64(val in any::<i64>()) {
        let (back, n) = sparse_roundtrip_64(val);
        prop_assert_eq!(back, val, "v64 must round-trip for every i64");
        prop_assert!((1..=9).contains(&n), "v64 encodes in 1..=9 bytes, got {}", n);
        prop_assert_eq!(n, SparseInt::val_size(val), "reported length matches val_size");
        // The retired 8-byte rung means values past the 7-byte range use 9.
        if val.unsigned_abs() > (SPARSE_7BYTE_MAX as u64) {
            prop_assert_eq!(n, 9, "past the 7-byte range, encoding is 9 bytes (0xFF)");
        }
    }

    // The frame header round-trips every legal payload size and flag.
    #[test]
    fn frame_header_round_trips(
        payload_size in 0i32..=(kMaxFramePayloadSize),
        extended in any::<bool>(),
    ) {
        let mut buf = [0u8; kFrameHeaderSize];
        prop_assert!(
            frame_codec_write_header(&mut buf, payload_size, extended),
            "a legal payload size must encode"
        );
        let mut header = FrameHeader { payload_size: -1, extended_header_flag: false };
        let status = frame_codec_peek_header(&buf, &mut header);
        prop_assert_eq!(status, FrameDecodeStatus::Complete, "a written header must peek Complete");
        prop_assert_eq!(header.payload_size, payload_size, "payload size round-trips");
        prop_assert_eq!(header.extended_header_flag, extended, "extended flag round-trips");
    }

    // Out-of-range payload sizes are refused by the writer, never silently
    // truncated into the size field.
    #[test]
    fn frame_header_rejects_oversized_payloads(
        payload_size in (kMaxFramePayloadSize + 1)..=i32::MAX,
    ) {
        let mut buf = [0u8; kFrameHeaderSize];
        prop_assert!(
            !frame_codec_write_header(&mut buf, payload_size, false),
            "a payload over kMaxFramePayloadSize must be refused"
        );
    }

    // Negative payload sizes are refused.
    #[test]
    fn frame_header_rejects_negative_payloads(payload_size in i32::MIN..0i32) {
        let mut buf = [0u8; kFrameHeaderSize];
        prop_assert!(
            !frame_codec_write_header(&mut buf, payload_size, false),
            "a negative payload size must be refused"
        );
    }
}

// The former length-8 defect band now round-trips (item 4.1). This pins the
// fix at the exact historical example that used to lose its low byte, and at
// both band boundaries, so a regression back to the 0xFE rung is caught.
#[test]
fn former_length8_band_now_round_trips() {
    for v in [
        36_028_797_018_963_967_i64,          // top of the old 0xFE band (used to -> ...712)
        -36_028_797_018_963_967_i64,
        281_474_976_710_656_i64,             // first value past the 7-byte range
        (1_i64 << 50) | 0xAB,                // low byte significant, squarely in-band
    ] {
        let (back, n) = sparse_roundtrip_64(v);
        assert_eq!(back, v, "band value {v} must round-trip after the fix");
        assert_eq!(n, 9, "band values now use the 9-byte (0xFF) encoding");
    }
}
