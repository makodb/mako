// Tier 1.2 of docs/testing-plan.md: framing under adversarial chunk
// boundaries.  A length-prefixed framer's worst failure mode is desync -- a
// parser that assumes "one read delivers one whole frame" wedges the moment
// the transport splits a frame across reads or coalesces several into one.
// SRPC's FrameStreamReader is fed by whatever the socket hands it, so it must
// reassemble correctly regardless of how the byte stream is chopped.
//
// These tests drive the reader with every hostile boundary: one byte at a
// time, split mid-header, split mid-payload, several whole frames in one
// append, and a whole frame followed by a partial next one.  The reference is
// the byte-identical run where the same frames arrive in one append.

use srpc::frame_codec::{
    frame_codec_encode_into, FrameDecodeStatus, FrameStreamReader, FrameView,
};

// A standalone byte buffer type matching FrameBytes' element type; we build
// frames into a Vec and feed slices of it to the reader.
fn view() -> FrameView {
    FrameView {
        header: srpc::frame_codec::FrameHeader {
            payload_size: 0,
            extended_header_flag: false,
        },
        payload: core::ptr::null(),
        payload_size: 0,
    }
}

// Encode one frame carrying `payload` into a fresh Vec<u8> and return the
// on-wire bytes.
#[allow(unsafe_code)]
fn encode_frame(payload: &[u8], extended: bool) -> Vec<u8> {
    let mut out = Vec::<u8>::new();
    let ok = unsafe {
        frame_codec_encode_into(
            &mut out,
            if payload.is_empty() { core::ptr::null() } else { payload.as_ptr() },
            payload.len() as i32,
            extended,
        )
    };
    assert!(ok, "encode must succeed for a legal payload");
    (0..out.len()).map(|i| out[i]).collect()
}

// Read every complete frame currently buffered, returning each frame's payload
// bytes copied out.  Stops at the first NeedMoreBytes.
#[allow(unsafe_code)]
fn drain_frames(reader: &mut FrameStreamReader) -> Vec<Vec<u8>> {
    let mut frames = Vec::new();
    loop {
        let mut v = view();
        match reader.next_frame(&mut v) {
            FrameDecodeStatus::Complete => {
                // SAFETY: Complete guarantees payload/payload_size are valid
                // for the duration before consume_frame.
                let bytes: Vec<u8> = if v.payload_size == 0 {
                    Vec::new()
                } else {
                    unsafe { std::slice::from_raw_parts(v.payload, v.payload_size) }.to_vec()
                };
                frames.push(bytes);
                reader.consume_frame();
            }
            FrameDecodeStatus::NeedMoreBytes => break,
            other => panic!("unexpected framing status while draining: {other:?}"),
        }
    }
    frames
}

// Feed `bytes` to a reader in chunks of exactly `chunk` (last chunk may be
// short), draining complete frames after each append, and return all payloads
// recovered.
#[allow(unsafe_code)]
fn feed_in_chunks(bytes: &[u8], chunk: usize) -> Vec<Vec<u8>> {
    let mut reader = FrameStreamReader::new();
    let mut all = Vec::new();
    let mut i = 0usize;
    while i < bytes.len() {
        let end = (i + chunk).min(bytes.len());
        let piece = &bytes[i..end];
        // SAFETY: piece is a live initialized slice for the call's duration.
        unsafe { reader.append(piece.as_ptr(), piece.len()) };
        all.extend(drain_frames(&mut reader));
        i = end;
    }
    all
}

#[test]
fn single_frame_reassembles_at_every_chunk_size() {
    let payload: Vec<u8> = (0..137u32).map(|i| (i % 251) as u8).collect();
    let wire = encode_frame(&payload, false);

    // chunk = 1 (one byte per read) is the maximally adversarial delivery;
    // every larger chunk up to the whole frame must agree with it.
    for chunk in 1..=wire.len() {
        let got = feed_in_chunks(&wire, chunk);
        assert_eq!(got.len(), 1, "exactly one frame at chunk={chunk}");
        assert_eq!(got[0], payload, "payload intact at chunk={chunk}");
    }
}

#[test]
fn header_split_across_appends_is_not_a_desync() {
    let payload = vec![0xAAu8; 40];
    let wire = encode_frame(&payload, false);
    // Split inside the 4-byte header (after 1, 2, 3 bytes).
    for split in 1..=3usize {
        let got = feed_in_chunks(&wire, split.max(1));
        // feed_in_chunks with chunk<4 already exercises header splits; assert
        // the specific two-append case too.
        let mut reader = FrameStreamReader::new();
        #[allow(unsafe_code)]
        unsafe {
            reader.append(wire[..split].as_ptr(), split);
        }
        let mut v = view();
        assert_eq!(
            reader.next_frame(&mut v),
            FrameDecodeStatus::NeedMoreBytes,
            "a partial header must ask for more, not desync (split={split})"
        );
        #[allow(unsafe_code)]
        unsafe {
            reader.append(wire[split..].as_ptr(), wire.len() - split);
        }
        let frames = drain_frames(&mut reader);
        assert_eq!(frames, vec![payload.clone()], "reassembles after the rest arrives");
        assert_eq!(got, vec![payload.clone()]);
    }
}

#[test]
fn payload_split_across_appends_reassembles() {
    let payload: Vec<u8> = (0..500u32).map(|i| (i * 7 % 256) as u8).collect();
    let wire = encode_frame(&payload, false);
    // Split partway into the payload (header complete, payload partial).
    let split = 4 + 200;
    let mut reader = FrameStreamReader::new();
    #[allow(unsafe_code)]
    unsafe {
        reader.append(wire[..split].as_ptr(), split);
    }
    let mut v = view();
    assert_eq!(
        reader.next_frame(&mut v),
        FrameDecodeStatus::NeedMoreBytes,
        "a complete header with a partial payload must ask for more"
    );
    #[allow(unsafe_code)]
    unsafe {
        reader.append(wire[split..].as_ptr(), wire.len() - split);
    }
    assert_eq!(drain_frames(&mut reader), vec![payload]);
}

#[test]
fn several_whole_frames_in_one_append_all_decode() {
    let payloads: Vec<Vec<u8>> = vec![
        vec![],                       // empty payload
        vec![1u8],                    // one byte
        (0..300u32).map(|i| i as u8).collect(), // multi-hundred
        vec![0xFFu8; 64],
    ];
    let mut wire = Vec::new();
    for p in &payloads {
        wire.extend(encode_frame(p, false));
    }
    // One append containing all four frames.
    let mut reader = FrameStreamReader::new();
    #[allow(unsafe_code)]
    unsafe {
        reader.append(wire.as_ptr(), wire.len());
    }
    assert_eq!(drain_frames(&mut reader), payloads);

    // And the same four frames delivered one byte at a time.
    assert_eq!(feed_in_chunks(&wire, 1), payloads);
}

#[test]
fn whole_frame_plus_partial_next_yields_one_then_waits() {
    let first = vec![0x11u8; 20];
    let second = vec![0x22u8; 20];
    let mut wire = encode_frame(&first, false);
    let second_wire = encode_frame(&second, false);
    // Append the whole first frame plus only half of the second.
    let half = second_wire.len() / 2;
    wire.extend_from_slice(&second_wire[..half]);

    let mut reader = FrameStreamReader::new();
    #[allow(unsafe_code)]
    unsafe {
        reader.append(wire.as_ptr(), wire.len());
    }
    // The first frame decodes; the reader then waits for the rest of the second.
    let frames = drain_frames(&mut reader);
    assert_eq!(frames, vec![first], "first frame decodes, second is incomplete");

    #[allow(unsafe_code)]
    unsafe {
        reader.append(second_wire[half..].as_ptr(), second_wire.len() - half);
    }
    assert_eq!(drain_frames(&mut reader), vec![second], "second completes after the rest");
}

// C stubs: frame_codec pulls in nothing from the C seam, but link the module
// graph's requirements defensively via the reader path (none needed today;
// left empty intentionally, matching frame_codec_desync_rust.rs which also
// declares none).
