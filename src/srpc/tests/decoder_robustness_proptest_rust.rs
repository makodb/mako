// Tier 1.3 of docs/testing-plan.md: decoder-robustness fuzzing over the
// hostile-input paths.  A network peer (or a corrupted stream) can hand the
// decoder any bytes at all; every decode entry point must reject bounded and
// never panic, never read out of bounds, and never fail to terminate.
//
// This is the always-gating form of a fuzzer: proptest drives arbitrary byte
// vectors (and near-valid structured frames) through the public decode paths
// in the normal `cargo test` lane, with shrinking on any failure -- so a
// crash arrives as the SMALLEST offending input, not a raw corpus entry.  A
// separate out-of-lane cargo-fuzz target could push coverage further (noted
// in the plan) but would not gate; this harness does.
//
// What "no panic" buys with ASan on (the -DSRPC_SANITIZER=address build):
// the same inputs also assert no memory unsafety in the `unsafe` framer/varint
// code.

use proptest::prelude::*;

use srpc::basetypes::SparseInt;
use srpc::frame_codec::{
    frame_codec_peek_header, FrameDecodeStatus, FrameHeader, FrameStreamReader, FrameView,
};
use srpc::internal_protocol::{response_has_extended_header, response_payload_size};


fn view() -> FrameView {
    FrameView {
        header: FrameHeader { payload_size: 0, extended_header_flag: false },
        payload: core::ptr::null(),
        payload_size: 0,
    }
}

proptest! {
    // Peeking a header over ANY four-plus bytes returns a valid status and
    // never panics; a Complete result always carries a nonnegative, bounded
    // payload size.
    #[test]
    fn peek_header_never_panics_on_arbitrary_bytes(bytes in prop::collection::vec(any::<u8>(), 0..64)) {
        let mut header = FrameHeader { payload_size: -1, extended_header_flag: false };
        let status = frame_codec_peek_header(&bytes, &mut header);
        match status {
            FrameDecodeStatus::Complete => {
                prop_assert!(header.payload_size >= 0, "Complete implies nonnegative size");
                prop_assert!(
                    header.payload_size <= srpc::frame_codec::kMaxFramePayloadSize,
                    "Complete implies bounded size"
                );
            }
            FrameDecodeStatus::NeedMoreBytes => {
                prop_assert!(bytes.len() < srpc::frame_codec::kFrameHeaderSize,
                    "NeedMoreBytes only with a short buffer");
            }
            FrameDecodeStatus::Malformed => {}
        }
    }

    // The response-size bit helpers accept every i32 without panicking, and
    // the decoded payload size is always nonnegative (the mask guarantees it).
    #[test]
    fn response_size_helpers_total_on_any_i32(encoded in any::<i32>()) {
        let _ = response_has_extended_header(encoded);
        let size = response_payload_size(encoded);
        prop_assert!(size >= 0, "masked payload size is never negative");
    }

    // SparseInt load over arbitrary bytes must not panic or read OOB: give it
    // a full 9-byte buffer (the max any encoding occupies) filled with random
    // bytes.  The decoded value is unconstrained; termination and safety are
    // the contract.
    #[test]
    #[allow(unsafe_code)]
    fn sparse_load_never_panics_on_arbitrary_bytes(bytes in prop::array::uniform9(any::<u8>())) {
        // SAFETY: load32/load64 read at most 9 bytes; the buffer has 9.
        let _v32 = unsafe { SparseInt::load32(bytes.as_ptr()) };
        let _v64 = unsafe { SparseInt::load64(bytes.as_ptr()) };
    }

    // The stream reader, fed arbitrary bytes in arbitrary chunks, never
    // panics and always makes a decision (Complete/NeedMoreBytes/Malformed);
    // it must not loop forever.  We bound the drain so a hypothetical
    // non-terminating decoder fails the test by exceeding the cap rather than
    // hanging CI.
    #[test]
    #[allow(unsafe_code)]
    fn stream_reader_never_panics_on_arbitrary_chunked_bytes(
        chunks in prop::collection::vec(prop::collection::vec(any::<u8>(), 0..24), 0..24)
    ) {
        let mut reader = FrameStreamReader::new();
        let mut decisions = 0u32;
        for chunk in &chunks {
            if !chunk.is_empty() {
                // SAFETY: chunk is a live initialized slice for this call.
                unsafe { reader.append(chunk.as_ptr(), chunk.len()) };
            }
            // Drain whatever is decodable, bounded.
            loop {
                let mut v = view();
                let status = reader.next_frame(&mut v);
                decisions += 1;
                prop_assert!(decisions < 100_000, "decoder must terminate, not loop");
                match status {
                    FrameDecodeStatus::Complete => reader.consume_frame(),
                    FrameDecodeStatus::NeedMoreBytes | FrameDecodeStatus::Malformed => break,
                }
            }
        }
    }
}
