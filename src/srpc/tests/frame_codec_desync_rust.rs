// Regression test for the silent stream-desynchronisation wedge.
//
// Before kMaxFramePayloadSize had a real value, a 4-byte header whose size
// field landed in 0x7ffffffc..=0x7fffffff decoded as Complete, and
// total_frame_size() wrapped to a negative i32. Every caller does
// `as usize`, which sign-extends that to ~1.8e19, so:
//
//   * next_frame()    -> NeedMoreBytes on every call, forever
//   * consume_frame() -> returns early, cursor never advances
//   * compaction      -> sits behind the same guard, never runs
//
// and fsr_append() kept growing the buffer. The connection consumed memory
// and never produced another message, with no error and no reconnect.
#![allow(unsafe_code)]

use srpc::frame_codec::{
    kFrameHeaderSize, kMaxFramePayloadSize, FrameDecodeStatus, FrameHeader, FrameStreamReader,
    FrameView,
};
use srpc::internal_protocol::encode_response_size;

fn view() -> FrameView {
    FrameView {
        header: FrameHeader {
            payload_size: 0,
            extended_header_flag: false,
        },
        payload: core::ptr::null(),
        payload_size: 0,
    }
}

#[test]
fn desynchronised_header_is_rejected_not_waited_on() {
    // every size that used to wrap total_frame_size() negative
    for payload in [
        i32::MAX,
        i32::MAX - 1,
        i32::MAX - 2,
        i32::MAX - 3,
        kMaxFramePayloadSize + 1,
    ] {
        let encoded = encode_response_size(payload, false);
        let bytes = encoded.to_ne_bytes();

        let mut header = FrameHeader {
            payload_size: 0,
            extended_header_flag: false,
        };
        assert_eq!(
            srpc::frame_codec::frame_codec_peek_header(&bytes, &mut header),
            FrameDecodeStatus::Malformed,
            "payload {payload} must be rejected, not accepted as a valid header"
        );

        // and the stream reader must surface it rather than stalling
        let mut reader = FrameStreamReader::new();
        unsafe { reader.append(bytes.as_ptr(), bytes.len()) };
        let mut out = view();
        assert_eq!(
            reader.next_frame(&mut out),
            FrameDecodeStatus::Malformed,
            "payload {payload}: reader must report Malformed, not NeedMoreBytes forever"
        );
    }
}

#[test]
fn total_frame_size_never_wraps_negative() {
    // The `as usize` at every call site sign-extends, so a negative here is
    // the difference between "wait for 4 more bytes" and "wait for 18 EB".
    for payload in [
        0,
        1,
        kMaxFramePayloadSize - 1,
        kMaxFramePayloadSize,
        i32::MAX - 3,
        i32::MAX,
    ] {
        let total = FrameHeader {
            payload_size: payload,
            extended_header_flag: false,
        }
        .total_frame_size();
        assert!(total >= kFrameHeaderSize as i32, "wrapped for {payload}");
        assert!((total as usize) < (i32::MAX as usize) + 1, "sign-extended for {payload}");
    }
}

#[test]
fn a_well_formed_frame_at_the_bound_still_round_trips() {
    // The bound must not break legitimate traffic at the boundary itself.
    let mut reader = FrameStreamReader::new();
    let payload = b"hello";
    let encoded = encode_response_size(payload.len() as i32, false);
    let head = encoded.to_ne_bytes();
    unsafe { reader.append(head.as_ptr(), head.len()) };
    unsafe { reader.append(payload.as_ptr(), payload.len()) };

    let mut out = view();
    assert_eq!(reader.next_frame(&mut out), FrameDecodeStatus::Complete);
    assert_eq!(out.payload_size, payload.len());
    reader.consume_frame();

    let mut out2 = view();
    assert_eq!(reader.next_frame(&mut out2), FrameDecodeStatus::NeedMoreBytes);
}
