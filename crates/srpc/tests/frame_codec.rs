#![allow(unsafe_code)]

use srpc::rpc::frame_codec::{
    frame_codec_encode_into, frame_codec_peek_header, frame_codec_write_header,
    frame_decode_status_to_string, kFrameHeaderSize, kMaxFramePayloadSize, FrameDecodeStatus,
    FrameHeader, FrameStreamReader, FrameView,
};
use std::mem::{align_of, offset_of, size_of};

fn encode(payload: &[u8], extended: bool) -> Vec<u8> {
    let mut out = Vec::<u8>::new();
    assert!(unsafe {
        frame_codec_encode_into(&mut out, payload.as_ptr(), payload.len() as i32, extended)
    });
    out
}

#[test]
fn historical_constants_enum_strings_and_layout_are_exact() {
    assert_eq!(kFrameHeaderSize, 4);
    assert_eq!(kMaxFramePayloadSize, i32::MAX);
    assert_eq!(FrameDecodeStatus::NeedMoreBytes as i32, 0);
    assert_eq!(FrameDecodeStatus::Complete as i32, 1);
    assert_eq!(FrameDecodeStatus::Malformed as i32, 2);
    assert_eq!(size_of::<FrameDecodeStatus>(), 4);
    assert_eq!(align_of::<FrameDecodeStatus>(), 4);
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

    assert_eq!(offset_of!(FrameHeader, payload_size), 0);
    assert_eq!(offset_of!(FrameHeader, extended_header_flag), 4);
    assert_eq!(size_of::<FrameHeader>(), 8);
    assert_eq!(align_of::<FrameHeader>(), 4);
    let default_header = FrameHeader::default();
    assert_eq!(default_header.payload_size, 0);
    assert!(!default_header.extended_header_flag);

    assert_eq!(offset_of!(FrameView, header), 0);
    assert_eq!(offset_of!(FrameView, payload), 8);
    assert_eq!(offset_of!(FrameView, payload_size), 16);
    assert_eq!(size_of::<FrameView>(), 24);
    assert_eq!(align_of::<FrameView>(), 8);
    let default_view = FrameView::default();
    assert_eq!(default_view.header, default_header);
    assert!(default_view.payload.is_null());
    assert_eq!(default_view.payload_size, 0);

    // std::vector<uint8_t> and Rust Vec<u8> are both three machine words on
    // the supported 64-bit ABI; the trailing read offset makes the historical
    // reader exactly four words.
    assert_eq!(size_of::<FrameStreamReader>(), 32);
    assert_eq!(align_of::<FrameStreamReader>(), 8);
}

#[test]
fn raw_header_api_is_native_endian_byte_exact_and_failure_is_non_mutating() {
    let mut header = [0xAA_u8; 4];
    assert!(!unsafe { frame_codec_write_header(core::ptr::null_mut(), 1, false) });
    assert!(!unsafe { frame_codec_write_header(header.as_mut_ptr(), -1, false) });
    assert_eq!(header, [0xAA; 4]);

    assert!(unsafe { frame_codec_write_header(header.as_mut_ptr(), 17, false) });
    assert_eq!(header, 17_i32.to_ne_bytes());
    let mut decoded = FrameHeader::default();
    assert_eq!(
        unsafe { frame_codec_peek_header(header.as_ptr(), header.len(), &mut decoded) },
        FrameDecodeStatus::Complete
    );
    assert_eq!(decoded.payload_size, 17);
    assert!(!decoded.extended_header_flag);
    assert_eq!(decoded.total_frame_size(), 21);

    assert!(unsafe { frame_codec_write_header(header.as_mut_ptr(), 4096, true) });
    assert_eq!(header, (4096_i32 | i32::MIN).to_ne_bytes());
    assert_eq!(
        unsafe { frame_codec_peek_header(header.as_ptr(), header.len(), &mut decoded) },
        FrameDecodeStatus::Complete
    );
    assert_eq!(decoded.payload_size, 4096);
    assert!(decoded.extended_header_flag);

    let sentinel = decoded;
    assert_eq!(
        unsafe { frame_codec_peek_header(header.as_ptr(), 3, &mut decoded) },
        FrameDecodeStatus::NeedMoreBytes
    );
    assert_eq!(
        decoded, sentinel,
        "short input must not touch the out-header"
    );
}

#[test]
fn encode_appends_coalesced_frames_and_rejects_bad_pointer_or_size_atomically() {
    let mut out = vec![0x5A_u8];
    assert!(!unsafe { frame_codec_encode_into(&mut out, core::ptr::null(), 1, false) });
    assert!(!unsafe { frame_codec_encode_into(&mut out, core::ptr::null(), -1, false) });
    assert_eq!(out, [0x5A]);

    let first = [1_u8, 2, 3];
    let second = [9_u8, 8];
    assert!(unsafe {
        frame_codec_encode_into(&mut out, first.as_ptr(), first.len() as i32, false)
    });
    assert!(unsafe {
        frame_codec_encode_into(&mut out, second.as_ptr(), second.len() as i32, true)
    });
    assert!(unsafe { frame_codec_encode_into(&mut out, core::ptr::null(), 0, false) });

    let first_start = 1;
    assert_eq!(&out[first_start + 4..first_start + 7], &first);
    let second_start = first_start + 4 + first.len();
    assert_eq!(&out[second_start + 4..second_start + 6], &second);
    let third_start = second_start + 4 + second.len();
    assert_eq!(&out[third_start..], &[0, 0, 0, 0]);
}

#[test]
fn reader_reassembles_fragments_and_returns_a_stable_zero_copy_peek() {
    let wire = encode(b"hello", true);
    let mut reader = FrameStreamReader::new();
    let mut view = FrameView::default();

    let mut index = 0_usize;
    while index + 1 < wire.len() {
        unsafe { reader.append(wire.as_ptr().add(index), 1) };
        assert_eq!(
            reader.next_frame(&mut view),
            FrameDecodeStatus::NeedMoreBytes
        );
        index += 1;
    }
    unsafe { reader.append(wire.as_ptr().add(index), 1) };

    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(view.header.payload_size, 5);
    assert!(view.header.extended_header_flag);
    assert_eq!(view.payload_size, 5);
    let first_pointer = view.payload;
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, 5) },
        b"hello"
    );

    // A second peek neither copies nor consumes: it returns the identical
    // pointer into the reader-owned vector.
    let mut repeated = FrameView::default();
    assert_eq!(
        reader.next_frame(&mut repeated),
        FrameDecodeStatus::Complete
    );
    assert_eq!(repeated.payload, first_pointer);
    assert_eq!(reader.buffered_bytes(), wire.len());

    reader.consume_frame();
    assert!(reader.empty());
    assert_eq!(
        reader.next_frame(&mut repeated),
        FrameDecodeStatus::NeedMoreBytes
    );
}

#[test]
fn reader_preserves_fifo_partial_consume_and_reset_contracts() {
    let mut wire = encode(b"abc", false);
    wire.extend_from_slice(&encode(b"z", true));

    let mut reader = FrameStreamReader::new();
    reader.consume_frame();
    assert!(reader.empty());

    unsafe { reader.append(wire.as_ptr(), 2) };
    reader.consume_frame();
    assert_eq!(reader.buffered_bytes(), 2);
    unsafe { reader.append(wire.as_ptr().add(2), wire.len() - 2) };

    let mut view = FrameView::default();
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, 3) },
        b"abc"
    );
    assert!(!view.header.extended_header_flag);
    reader.consume_frame();

    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, 1) },
        b"z"
    );
    assert!(view.header.extended_header_flag);

    reader.reset();
    assert!(reader.empty());
    unsafe { reader.append(wire.as_ptr(), wire.len()) };
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, 3) },
        b"abc"
    );
}

#[test]
fn compaction_threshold_path_keeps_remaining_frame_zero_copy_and_ordered() {
    let large_payload = vec![0xA5_u8; 64 * 1024];
    let mut wire = encode(&large_payload, false);
    let tail = encode(b"tail", true);
    wire.extend_from_slice(&tail);

    let mut reader = FrameStreamReader::new();
    unsafe { reader.append(wire.as_ptr(), wire.len()) };
    let mut view = FrameView::default();
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(view.payload_size, large_payload.len());
    reader.consume_frame();

    // Consuming the large frame crosses 64 KiB and compacts the unread tail
    // to offset zero.  The next view must still point directly at its bytes.
    assert_eq!(reader.buffered_bytes(), tail.len());
    assert_eq!(reader.next_frame(&mut view), FrameDecodeStatus::Complete);
    assert_eq!(
        unsafe { core::slice::from_raw_parts(view.payload, 4) },
        b"tail"
    );
    assert!(view.header.extended_header_flag);
    reader.consume_frame();
    assert!(reader.empty());
}
