// Canonical Rust source for the rrr.frame_codec module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::Cell;

use crate::internal_protocol::{
    encode_response_size, response_has_extended_header, response_payload_size,
};

pub const kFrameHeaderSize: usize = 4;
pub const kMaxFramePayloadSize: i32 = 0x7fffffff;

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
        self.payload_size.wrapping_add(kFrameHeaderSize as i32)
    }
}

#[allow(clippy::absurd_extreme_comparisons)]
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
    let bytes: [u8; 4] = encoded.to_ne_bytes();
    out_buf[0] = bytes[0];
    out_buf[1] = bytes[1];
    out_buf[2] = bytes[2];
    out_buf[3] = bytes[3];
    true
}

pub fn frame_codec_peek_header(buf: &[u8], out_header: &mut FrameHeader) -> FrameDecodeStatus {
    if buf.len() < kFrameHeaderSize {
        return FrameDecodeStatus::NeedMoreBytes;
    }
    let encoded = i32::from_ne_bytes([buf[0], buf[1], buf[2], buf[3]]);
    let extended_header_flag = response_has_extended_header(encoded);
    let payload_size = response_payload_size(encoded);
    if payload_size < 0 {
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

type FrameBytes = rusty::StdVector<u8>;
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
