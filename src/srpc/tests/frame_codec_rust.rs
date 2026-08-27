#![allow(unsafe_code)]

use srpc::frame_codec::{
    frame_codec_encode_into, frame_codec_peek_header, frame_codec_write_header,
    frame_decode_status_to_string, kFrameHeaderSize, kMaxFramePayloadSize, FrameDecodeStatus,
    FrameHeader, FrameStreamReader, FrameView,
};
use srpc::internal_protocol::encode_response_size;

fn native_bytes(value: i32) -> [u8; 4] {
    value.to_ne_bytes()
}

#[test]
fn header_boundaries_and_transactional_encode() {
    assert_eq!(kFrameHeaderSize, 4);
    assert_eq!(kMaxFramePayloadSize, 64 * 1024 * 1024);
    // The bound must leave room for the 4-byte header, or
    // total_frame_size() would be the very overflow it exists to prevent.
    assert!(kMaxFramePayloadSize <= i32::MAX - kFrameHeaderSize as i32);
    assert_eq!(
        frame_decode_status_to_string(FrameDecodeStatus::NeedMoreBytes),
        "NeedMoreBytes"
    );
    assert_eq!(
        frame_decode_status_to_string(FrameDecodeStatus::Complete),
        "Complete"
    );
    assert_eq!(
        frame_decode_status_to_string(FrameDecodeStatus::Malformed),
        "Malformed"
    );

    let mut bytes = [0xa1, 0xa2, 0xa3, 0xa4, 0xa5];
    let unchanged = bytes;
    assert!(!frame_codec_write_header(&mut bytes[..3], 1, false));
    assert_eq!(bytes, unchanged);
    assert!(!frame_codec_write_header(&mut bytes, -1, true));
    assert_eq!(bytes, unchanged);
    assert!(frame_codec_write_header(&mut bytes, 0, true));
    assert_eq!(&bytes[..4], &native_bytes(i32::MIN));
    assert_eq!(bytes[4], 0xa5);
    // The encoder now refuses sizes past the bound, so it can never put a
    // header on the wire that the peer's decoder is obliged to reject.
    assert!(!frame_codec_write_header(&mut bytes, i32::MAX, false));
    assert!(!frame_codec_write_header(&mut bytes, kMaxFramePayloadSize + 1, false));
    assert!(frame_codec_write_header(&mut bytes, kMaxFramePayloadSize, false));
    assert_eq!(&bytes[..4], &native_bytes(kMaxFramePayloadSize));
    assert!(frame_codec_write_header(&mut bytes, kMaxFramePayloadSize, true));
    assert_eq!(
        &bytes[..4],
        &native_bytes(encode_response_size(kMaxFramePayloadSize, true))
    );

    let mut decoded = FrameHeader {
        payload_size: 17,
        extended_header_flag: true,
    };
    assert_eq!(
        frame_codec_peek_header(&bytes[..3], &mut decoded),
        FrameDecodeStatus::NeedMoreBytes
    );
    assert_eq!(decoded.payload_size, 17);
    assert!(decoded.extended_header_flag);

    for (encoded, payload, extended) in [
        (0, 0, false),
        (kMaxFramePayloadSize, kMaxFramePayloadSize, false),
        (i32::MIN, 0, true),
        (
            encode_response_size(kMaxFramePayloadSize, true),
            kMaxFramePayloadSize,
            true,
        ),
    ] {
        let mut header = FrameHeader {
            payload_size: -7,
            extended_header_flag: !extended,
        };
        assert_eq!(
            frame_codec_peek_header(&native_bytes(encoded), &mut header),
            FrameDecodeStatus::Complete
        );
        assert_eq!(header.payload_size, payload);
        assert_eq!(header.extended_header_flag, extended);
    }
    // An all-ones word -- the canonical shape of a corrupted or
    // desynchronised read -- used to decode as a perfectly valid header
    // claiming an i32::MAX payload. It is now rejected.
    {
        let mut header = FrameHeader {
            payload_size: 0,
            extended_header_flag: false,
        };
        assert_eq!(
            frame_codec_peek_header(&native_bytes(-1), &mut header),
            FrameDecodeStatus::Malformed
        );
    }

    // Regression: this used to assert `i32::MIN + 3` -- it PINNED the
    // wrapping overflow as correct, so it could never fail on the bug.
    // total_frame_size() must never be negative: every caller does
    // `as usize`, which sign-extends a negative into ~1.8e19 and makes the
    // "do I have the whole frame yet?" guard true forever.
    for payload in [0, 1, kMaxFramePayloadSize, i32::MAX - 1, i32::MAX] {
        let total = FrameHeader {
            payload_size: payload,
            extended_header_flag: false,
        }
        .total_frame_size();
        assert!(total >= 0, "total_frame_size wrapped negative for {payload}");
        assert!(total >= kFrameHeaderSize as i32);
    }
    assert_eq!(
        FrameHeader {
            payload_size: kMaxFramePayloadSize,
            extended_header_flag: false,
        }
        .total_frame_size(),
        kMaxFramePayloadSize + kFrameHeaderSize as i32
    );

    // A desynchronised stream must be REJECTED, not waited on forever.
    // Before the bound existed, peek_header accepted every 4-byte pattern and
    // the connection wedged silently instead of erroring and reconnecting.
    {
        let mut header = FrameHeader {
            payload_size: 0,
            extended_header_flag: false,
        };
        let oversize = encode_response_size(kMaxFramePayloadSize + 1, false);
        assert_eq!(
            frame_codec_peek_header(&native_bytes(oversize), &mut header),
            FrameDecodeStatus::Malformed
        );
        let way_oversize = encode_response_size(i32::MAX, false);
        assert_eq!(
            frame_codec_peek_header(&native_bytes(way_oversize), &mut header),
            FrameDecodeStatus::Malformed
        );
    }

    let mut encoded = rusty::StdVector::from(vec![9, 8]);
    let before = encoded.to_vec();
    assert!(!unsafe { frame_codec_encode_into(&mut encoded, core::ptr::null(), -1, false) });
    assert_eq!(encoded, before);
    assert!(!unsafe { frame_codec_encode_into(&mut encoded, core::ptr::null(), 1, false) });
    assert_eq!(encoded, before);
    assert!(unsafe { frame_codec_encode_into(&mut encoded, core::ptr::null(), 0, false) });
    assert_eq!(&encoded[2..], &native_bytes(0));
}

#[test]
fn fragmented_coalesced_and_compacted_streams() {
    let first_payload = *b"abc";
    let second_payload = [0x55, 0xaa];
    let mut first = rusty::StdVector::default();
    let mut second = rusty::StdVector::default();
    assert!(unsafe {
        frame_codec_encode_into(
            &mut first,
            first_payload.as_ptr(),
            first_payload.len() as i32,
            false,
        )
    });
    assert!(unsafe {
        frame_codec_encode_into(
            &mut second,
            second_payload.as_ptr(),
            second_payload.len() as i32,
            true,
        )
    });

    let mut reader = FrameStreamReader::new();
    reader.cursor_.set_position(99);
    let mut beyond_end = FrameView {
        header: FrameHeader {
            payload_size: 7,
            extended_header_flag: true,
        },
        payload: core::ptr::dangling(),
        payload_size: 11,
    };
    assert_eq!(
        reader.next_frame(&mut beyond_end),
        FrameDecodeStatus::NeedMoreBytes
    );
    reader.consume_frame();
    assert_eq!(reader.cursor_.position(), 99);
    assert_eq!(reader.buffered_bytes(), 0);
    reader.reset();

    let mut view = FrameView {
        header: FrameHeader {
            payload_size: 91,
            extended_header_flag: true,
        },
        payload: core::ptr::dangling(),
        payload_size: 77,
    };
    for (index, byte) in first.iter().enumerate() {
        unsafe { reader.append(byte, 1) };
        let status = reader.next_frame(&mut view);
        if index + 1 < first.len() {
            assert_eq!(status, FrameDecodeStatus::NeedMoreBytes);
            assert_eq!(view.header.payload_size, 91);
            assert_eq!(view.payload_size, 77);
        } else {
            assert_eq!(status, FrameDecodeStatus::Complete);
        }
    }
    assert_eq!(view.payload_size, first_payload.len());
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, view.payload_size) },
        first_payload
    );
    reader.consume_frame();
    assert!(reader.empty());

    let mut coalesced = first.to_vec();
    coalesced.extend_from_slice(&second);
    unsafe { reader.append(coalesced.as_ptr(), coalesced.len()) };
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    reader.consume_frame();
    assert_eq!(reader.buffered_bytes(), second.len());
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert!(view.header.extended_header_flag);
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, view.payload_size) },
        second_payload
    );
    reader.consume_frame();
    assert!(reader.empty());

    const COMPACT_TOTAL: usize = 64 * 1024;
    let compact_payload: Vec<u8> = (0..COMPACT_TOTAL - 4)
        .map(|index| ((index * 17) & 0xff) as u8)
        .collect();
    let mut compact = rusty::StdVector::default();
    assert!(unsafe {
        frame_codec_encode_into(
            &mut compact,
            compact_payload.as_ptr(),
            compact_payload.len() as i32,
            false,
        )
    });
    let mut combined = compact.to_vec();
    combined.extend_from_slice(&second);
    unsafe { reader.append(combined.as_ptr(), combined.len()) };
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    reader.consume_frame();
    assert_eq!(reader.cursor_.position(), 0);
    assert_eq!(reader.cursor_.get_ref().len(), second.len());
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, view.payload_size) },
        second_payload
    );
}
