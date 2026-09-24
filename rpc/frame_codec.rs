// Canonical Rust source for the srpc.frame_codec module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::Cell;

use crate::internal_protocol::{
    encode_response_size, response_has_extended_header, response_payload_size,
};

// Verus specs (behind #[cfg(verus)], invisible to rustc and rusty-cpp) pin the
// peek-header bound (T4-style workaround for the from_ne_bytes wall, see
// docs/verification.md). The enum/struct they mention are admitted in
// verify/src/frame_codec_proofs.rs via external type specifications.
#[cfg(verus)]
use vstd::prelude::*;

// The 4-byte header word, read out of the leading bytes.
//
// Verus cannot process `i32::from_ne_bytes`: vstd does not specify it, and its
// const-generic array signature cannot be matched by `assume_specification`. So
// the call is isolated here and this body is trusted (`external_body`) -- the
// same shape vstd's own bytes.rs uses to wrap the std byte calls.
//
// The `ensures` is a TRUSTED AXIOM, and it is target-conditional: it states the
// little-endian decomposition, which is what native-endian marshalling is on the
// little-endian targets srpc supports (x86_64, aarch64-LE) -- the same assumption
// tests/wire_golden_rust.rs already encodes in its byte vectors. It could not be
// stated as the endian-agnostic "from(to(x)) == x", because that would require
// calling one exec helper inside the other's spec, which Verus disallows.
//
// SCOPE: only the write->peek round trip (T5) rests on this. The peek-header
// bound (T6) does NOT -- that follows from the two range guards regardless of
// what these bytes decode to.
#[cfg_attr(verus, verus_verify(external_body))]
#[cfg_attr(verus, verus_spec(r =>
    ensures r == ((((b0 as u32) | ((b1 as u32) << 8) | ((b2 as u32) << 16)
        | ((b3 as u32) << 24))) as i32),
))]
fn header_word_from_bytes(b0: u8, b1: u8, b2: u8, b3: u8) -> i32 {
    i32::from_ne_bytes([b0, b1, b2, b3])
}

// The store side of the same trusted boundary (wraps `to_ne_bytes`), with the
// matching little-endian axiom. Same scope caveat as above.
#[cfg_attr(verus, verus_verify(external_body))]
#[cfg_attr(verus, verus_spec(
    requires out_buf.len() >= 4,
    ensures
        final(out_buf)@.len() == old(out_buf)@.len(),
        final(out_buf)@[0] == (((w as u32)) & 0xFF) as u8,
        final(out_buf)@[1] == (((w as u32) >> 8) & 0xFF) as u8,
        final(out_buf)@[2] == (((w as u32) >> 16) & 0xFF) as u8,
        final(out_buf)@[3] == (((w as u32) >> 24) & 0xFF) as u8,
))]
fn store_header_word(out_buf: &mut [u8], w: i32) {
    let bytes: [u8; 4] = w.to_ne_bytes();
    out_buf[0] = bytes[0];
    out_buf[1] = bytes[1];
    out_buf[2] = bytes[2];
    out_buf[3] = bytes[3];
}

#[cfg_attr(verus, verus_verify)]
pub const kFrameHeaderSize: usize = 4;
// Largest payload a single frame may carry.
//
// This is a STREAM-INTEGRITY bound, not a resource policy. The 4-byte header
// is the only framing signal on the wire, so if a connection ever
// desynchronises -- a short write, a reconnect that resumes mid-frame, a bug
// upstream -- the decoder reads payload bytes as a header and gets a garbage
// length. With no bound it ACCEPTS that length and waits for bytes that will
// never arrive: next_frame() returns NeedMoreBytes forever, consume_frame()
// never advances the cursor, the buffer is never compacted, and the
// connection wedges silently -- no error, no log, no close, no reconnect.
//
// A bound turns that into Malformed -> error -> close -> reconnect, which is
// a failure the caller can see and recover from.
//
// 64 MiB is far above any real srpc message and rejects 127/128 of the
// 31-bit size space, so a desync is caught on the first bad header with high
// probability. It is the only knob: raise it if a legitimate message ever
// needs to be larger.
//
// It must also stay <= i32::MAX - kFrameHeaderSize, so that
// FrameHeader::total_frame_size() cannot overflow.
#[cfg_attr(verus, verus_verify)]
pub const kMaxFramePayloadSize: i32 = 64 * 1024 * 1024;

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(i32)]
pub enum FrameDecodeStatus {
    NeedMoreBytes = 0,
    Complete = 1,
    Malformed = 2,
}

#[allow(unreachable_patterns, clippy::unnecessary_literal_unwrap)]
pub fn frame_decode_status_to_string(status: FrameDecodeStatus) -> &'static str {
    match status {
        FrameDecodeStatus::NeedMoreBytes => "NeedMoreBytes",
        FrameDecodeStatus::Complete => "Complete",
        FrameDecodeStatus::Malformed => "Malformed",
        _ => {
            let impossible: Option<i32> = None;
            let _unreachable = impossible.expect("unreachable");
            ""
        }
    }
}

#[repr(C)]
pub struct FrameHeader {
    pub payload_size: i32,
    pub extended_header_flag: bool,
}

impl FrameHeader {
    pub fn total_frame_size(&self) -> i32 {
        // Saturating, not wrapping. Every caller does `total_frame_size() as
        // usize`, and casting a NEGATIVE i32 to usize sign-extends: a wrapped
        // -2147483645 becomes 18446744071562067971, which makes the
        // `rem.len() < total` guard true forever and wedges the stream.
        // Saturation keeps a malformed header merely unsatisfiable rather
        // than catastrophically so; peek_header rejects it before that.
        self.payload_size.saturating_add(kFrameHeaderSize as i32)
    }
}

// T5 (encode side): on success the four leader bytes are the little-endian
// image of encode_response_size(payload_size, flag). encode's definition is
// inlined because a spec cannot call an exec function.
#[cfg_attr(verus, verus_spec(r =>
    ensures
        final(out_buf)@.len() == old(out_buf)@.len(),
        // succeeds exactly on the in-range, big-enough-buffer case
        (0i32 <= payload_size && payload_size <= kMaxFramePayloadSize
            && old(out_buf)@.len() >= kFrameHeaderSize) ==> r == true,
        (r == true) ==> (
            final(out_buf)@[0] == ((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else {
                (payload_size as u32) & 0x7fffffffu32
            }) as i32) as u32) & 0xFF) as u8
            && final(out_buf)@[1] == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else {
                (payload_size as u32) & 0x7fffffffu32
            }) as i32) as u32) >> 8) & 0xFF) as u8
            && final(out_buf)@[2] == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else {
                (payload_size as u32) & 0x7fffffffu32
            }) as i32) as u32) >> 16) & 0xFF) as u8
            && final(out_buf)@[3] == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else {
                (payload_size as u32) & 0x7fffffffu32
            }) as i32) as u32) >> 24) & 0xFF) as u8
        ),
))]
pub fn frame_codec_write_header(
    out_buf: &mut [u8],
    payload_size: i32,
    extended_header_flag: bool,
) -> bool {
    if payload_size < 0 {
        return false;
    }
    if payload_size > kMaxFramePayloadSize {
        return false;
    }
    if out_buf.len() < kFrameHeaderSize {
        return false;
    }
    let encoded: i32 = encode_response_size(payload_size, extended_header_flag);
    store_header_word(out_buf, encoded);
    true
}

// T6: a Complete decode always reports a payload size inside the valid range,
// which is what makes the reader's `as usize` casts safe. The bound follows from
// the two guards below (< 0 and > kMaxFramePayloadSize both reject before
// Complete), so it holds for whatever the leader bytes decode to.
// T5 (decode side) is the second clause: on Complete the reported size and flag
// are the internal_protocol decode of the little-endian leader word. Those
// decodes are inlined (a spec cannot call an exec function); the word itself
// comes from the trusted little-endian axiom on header_word_from_bytes.
#[cfg_attr(verus, verus_spec(r =>
    ensures
        // reports Complete exactly when the buffer is long enough and the
        // decoded size is within the frame bound (it is never negative)
        (buf@.len() >= kFrameHeaderSize
            && 0i32 <= (((((((buf@[0] as u32) | ((buf@[1] as u32) << 8)
                | ((buf@[2] as u32) << 16) | ((buf@[3] as u32) << 24))) as i32) as u32)
                & 0x7fffffffu32) as i32)
            && (((((((buf@[0] as u32) | ((buf@[1] as u32) << 8) | ((buf@[2] as u32) << 16)
                | ((buf@[3] as u32) << 24))) as i32) as u32) & 0x7fffffffu32) as i32)
                <= kMaxFramePayloadSize) ==> r == FrameDecodeStatus::Complete,
        (r == FrameDecodeStatus::Complete) ==> (
            0i32 <= final(out_header).payload_size
                && final(out_header).payload_size <= kMaxFramePayloadSize
        ),
        (r == FrameDecodeStatus::Complete) ==> (
            final(out_header).payload_size == (((((((buf@[0] as u32)
                | ((buf@[1] as u32) << 8) | ((buf@[2] as u32) << 16)
                | ((buf@[3] as u32) << 24))) as i32) as u32) & 0x7fffffffu32) as i32)
            && final(out_header).extended_header_flag == (((((((buf@[0] as u32)
                | ((buf@[1] as u32) << 8) | ((buf@[2] as u32) << 16)
                | ((buf@[3] as u32) << 24))) as i32) as u32) & 0x80000000u32) != 0)
        ),
))]
pub fn frame_codec_peek_header(buf: &[u8], out_header: &mut FrameHeader) -> FrameDecodeStatus {
    if buf.len() < kFrameHeaderSize {
        return FrameDecodeStatus::NeedMoreBytes;
    }
    let encoded = header_word_from_bytes(buf[0], buf[1], buf[2], buf[3]);
    let extended_header_flag = response_has_extended_header(encoded);
    let payload_size = response_payload_size(encoded);
    // response_payload_size() masks with kResponseSizeMask, so payload_size
    // is never negative -- this arm is defence in depth, not the live check.
    if payload_size < 0 {
        return FrameDecodeStatus::Malformed;
    }
    // The live check. Before kMaxFramePayloadSize had a real value this was
    // unsatisfiable and the decoder could not reject ANY header, so a
    // desynchronised stream was indistinguishable from a slow one.
    if payload_size > kMaxFramePayloadSize {
        return FrameDecodeStatus::Malformed;
    }
    out_header.payload_size = payload_size;
    out_header.extended_header_flag = extended_header_flag;
    FrameDecodeStatus::Complete
}

#[repr(C)]
pub struct FrameView {
    pub header: FrameHeader,
    pub payload: *const u8,
    pub payload_size: usize,
}

type FrameBytes = Vec<u8>;
pub type FrameCursor = std::io::Cursor<FrameBytes>;

pub fn make_frame_cursor() -> FrameCursor {
    FrameCursor::new(Default::default())
}

#[repr(C)]
pub struct FrameStreamReader {
    pub cursor_: FrameCursor,
    pub noncopy_: Cell<bool>,
}

impl FrameStreamReader {
    #[allow(clippy::new_without_default)]
    pub fn new() -> FrameStreamReader {
        FrameStreamReader {
            cursor_: make_frame_cursor(),
            noncopy_: Cell::new(false),
        }
    }

    /// Appends `size` bytes starting at `data`.
    ///
    /// # Safety
    ///
    /// When `size` is nonzero, `data` must point to `size` initialized bytes
    /// that remain readable across any allocation performed by this call. The
    /// source range must not overlap the reader's destination buffer.
    #[allow(unsafe_code)]
    pub unsafe fn append(&mut self, data: *const u8, size: usize) {
        fsr_append(self, data, size)
    }

    #[allow(unsafe_code)]
    pub fn next_frame(&self, out_view: &mut FrameView) -> FrameDecodeStatus {
        let position = self.cursor_.position() as usize;
        let buffer: &FrameBytes = self.cursor_.get_ref();
        let rem: &[u8] = if position >= buffer.len() {
            &buffer[buffer.len()..]
        } else {
            &buffer[position..]
        };
        let mut header = FrameHeader {
            payload_size: 0,
            extended_header_flag: false,
        };
        let header_status = frame_codec_peek_header(rem, &mut header);
        if header_status != FrameDecodeStatus::Complete {
            return header_status;
        }
        let total = header.total_frame_size() as usize;
        if rem.len() < total {
            return FrameDecodeStatus::NeedMoreBytes;
        }
        let payload_size = header.payload_size as usize;
        out_view.header = header;
        out_view.payload = unsafe { rem.as_ptr().add(kFrameHeaderSize) };
        out_view.payload_size = payload_size;
        FrameDecodeStatus::Complete
    }

    pub fn consume_frame(&mut self) {
        fsr_consume_frame(self)
    }

    pub fn reset(&mut self) {
        self.cursor_.get_mut().clear();
        self.cursor_.set_position(0u64);
    }

    #[allow(clippy::implicit_saturating_sub)]
    pub fn buffered_bytes(&self) -> usize {
        let length = self.cursor_.get_ref().len();
        let position = self.cursor_.position() as usize;
        if position >= length {
            0usize
        } else {
            length - position
        }
    }

    pub fn empty(&self) -> bool {
        self.buffered_bytes() == 0
    }
}

#[allow(unsafe_code, clippy::absurd_extreme_comparisons)]
/// Appends an encoded frame containing `payload_size` bytes from `payload`.
///
/// # Safety
///
/// `payload` may be null; a null pointer with a positive `payload_size` is
/// rejected before access. When `payload` is non-null and `payload_size` is
/// positive, it must point to that many initialized bytes which remain readable
/// across any allocation performed by this call. The source range must not
/// overlap `out`'s destination storage.
pub unsafe fn frame_codec_encode_into(
    out: &mut FrameBytes,
    payload: *const u8,
    payload_size: i32,
    extended_header_flag: bool,
) -> bool {
    if payload_size < 0 {
        return false;
    }
    if payload_size > kMaxFramePayloadSize {
        return false;
    }
    if payload.is_null() && payload_size > 0 {
        return false;
    }

    let previous_size = out.len();
    let needed = kFrameHeaderSize + (payload_size as usize);
    out.resize(previous_size + needed, 0u8);
    if !frame_codec_write_header(
        &mut out[previous_size..],
        payload_size,
        extended_header_flag,
    ) {
        out.resize(previous_size, 0u8);
        return false;
    }
    if payload_size > 0 {
        unsafe {
            core::ptr::copy_nonoverlapping(
                payload,
                out.as_mut_ptr().add(previous_size + kFrameHeaderSize),
                payload_size as usize,
            );
        }
    }
    true
}

#[allow(unsafe_code)]
/// Appends raw bytes to `reader`'s backing buffer.
///
/// # Safety
///
/// When `size` is nonzero, `data` must point to `size` initialized bytes that
/// remain readable across any allocation performed by this call. The source
/// range must not overlap the reader's destination buffer.
pub unsafe fn fsr_append(reader: &mut FrameStreamReader, data: *const u8, size: usize) {
    if size == 0 {
        return;
    }
    let buffer = reader.cursor_.get_mut();
    let old_size = buffer.len();
    buffer.resize(old_size + size, 0u8);
    unsafe {
        core::ptr::copy_nonoverlapping(data, buffer.as_mut_ptr().add(old_size), size);
    }
}

#[allow(unsafe_code)]
pub fn fsr_consume_frame(reader: &mut FrameStreamReader) {
    let position = reader.cursor_.position() as usize;
    let buffer: &FrameBytes = reader.cursor_.get_ref();
    let rem: &[u8] = if position >= buffer.len() {
        &buffer[buffer.len()..]
    } else {
        &buffer[position..]
    };
    if rem.len() < kFrameHeaderSize {
        return;
    }
    let mut header = FrameHeader {
        payload_size: 0,
        extended_header_flag: false,
    };
    if frame_codec_peek_header(rem, &mut header) != FrameDecodeStatus::Complete {
        return;
    }
    let total = header.total_frame_size() as usize;
    if rem.len() < total {
        return;
    }
    reader.cursor_.set_position((position + total) as u64);

    let read_position = reader.cursor_.position() as usize;
    let compact_threshold_bytes = 64usize * 1024usize;
    if read_position == 0 || read_position < compact_threshold_bytes {
        return;
    }
    let buffer = reader.cursor_.get_mut();
    let remaining = buffer.len() - read_position;
    unsafe {
        core::ptr::copy(
            buffer.as_ptr().add(read_position),
            buffer.as_mut_ptr(),
            remaining,
        );
    }
    buffer.resize(remaining, 0u8);
    reader.cursor_.set_position(0u64);
}
