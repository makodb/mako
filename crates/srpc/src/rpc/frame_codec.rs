//! Stream framing for the legacy `rrr.frame_codec` C++ module.
//!
//! This valid-Rust owner intentionally preserves the original, pre-inline-DSL
//! C++ contract.  In particular, the C++ consumer still sees raw pointer/count
//! APIs, `const char*` status strings, a real `FrameStreamReader()` constructor,
//! and the original 32-byte, move-only reader ABI.  The later inline
//! conversion's span overloads, `std::string_view`, `new_()` factory, and
//! `Cursor + Cell` layout were migration artifacts rather than changes to the
//! framing protocol.
//!
//! `FrameView::payload` is deliberately zero-copy: it aliases the reader's
//! private byte buffer until the next `consume_frame`, `append`, or `reset`.

#![allow(non_camel_case_types, non_upper_case_globals, unsafe_code)]

use crate::wire::internal_protocol::{
    encode_response_size, response_has_extended_header, response_payload_size,
};

// Native Rust uses Vec.  The Mako consumer profile maps this generic alias to
// `std::vector`, preserving the public encoder signature.
type LegacyStdVector<T> = Vec<T>;

// Native Linux Rust spells C `char` as i8.  The consumer profile maps this
// alias to C++ `char`, so the exported status helper retains `const char*`
// rather than the inline DSL's accidental `std::string_view` return type.
type LegacyCChar = i8;

// Native Rust returns an ordinary static string.  The consumer profile maps
// this whole alias to `const char*`, preserving the historical constexpr C++
// spelling while letting the emitter return string literals directly.
type LegacyCString = &'static str;

// The reader needs the historical move-only C++ special-member contract as
// well as its 24-byte byte-buffer field.  Rust Vec supplies that contract
// natively; the C++ consumer maps this private alias to rusty::String, whose
// three-word byte storage is likewise move-only.  The public encoder uses the
// separate LegacyStdVector alias above and therefore remains std::vector.
type LegacyByteBuffer = Vec<LegacyCChar>;

/// Size of the single native-endian i32 frame prefix.
pub const kFrameHeaderSize: usize = 4_usize;

/// Largest payload representable in the low 31 bits of the prefix.
pub const kMaxFramePayloadSize: i32 = 0x7fff_ffff_i32;

/// Result of looking at a possibly incomplete frame.
#[repr(i32)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
pub enum FrameDecodeStatus {
    NeedMoreBytes = 0,
    Complete = 1,
    Malformed = 2,
}

/// Preserve the original C++ `const char*` API, including its invalid-enum
/// fallback.  Native Rust returns the equivalent static string slice; the
/// generated C++ returns a statically allocated, NUL-terminated literal.
pub const fn frame_decode_status_to_string(status: self::FrameDecodeStatus) -> LegacyCString {
    let code: i32 = status as i32;
    if code == 0_i32 {
        return "NeedMoreBytes";
    }
    if code == 1_i32 {
        return "Complete";
    }
    if code == 2_i32 {
        return "Malformed";
    }
    "Unknown"
}

/// Decoded contents of the four-byte frame prefix.
///
/// The release emitter keeps this an aggregate but does not reproduce C++
/// default-member initializers.  Every closed-world consumer was audited to
/// value-initialize it with `{}` or initialize both fields explicitly; adding
/// a constructor would instead break that aggregate API.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, PartialEq, Eq))]
pub struct FrameHeader {
    pub payload_size: i32,
    pub extended_header_flag: bool,
}

impl FrameHeader {
    pub const fn total_frame_size(&self) -> i32 {
        self.payload_size + kFrameHeaderSize as i32
    }
}

/// Encode the prefix at `out_buf` in native byte order.
///
/// # Safety
///
/// For a non-null `out_buf`, the caller must provide at least
/// [`kFrameHeaderSize`] writable bytes.  A null pointer is rejected before it
/// is dereferenced, matching the historical C++ function.
pub unsafe fn frame_codec_write_header(
    out_buf: *mut u8,
    payload_size: i32,
    extended_header_flag: bool,
) -> bool {
    if out_buf == core::ptr::null_mut() {
        return false;
    }
    if payload_size < 0_i32 {
        return false;
    }
    if payload_size > kMaxFramePayloadSize {
        return false;
    }

    let encoded: i32 = encode_response_size(payload_size, extended_header_flag);
    let bytes: [u8; kFrameHeaderSize] = encoded.to_ne_bytes();
    unsafe {
        core::ptr::copy_nonoverlapping(bytes.as_ptr(), out_buf, kFrameHeaderSize);
    }
    true
}

/// Decode a prefix without requiring the payload to be present.
///
/// `Complete` means only that the prefix was decoded.  The caller still has
/// to compare the available byte count with [`FrameHeader::total_frame_size`].
///
/// # Safety
///
/// If `available >= kFrameHeaderSize`, `buf` must address at least that many
/// readable bytes.  The historical C++ API carried the same precondition.
pub unsafe fn frame_codec_peek_header(
    buf: *const u8,
    available: usize,
    out_header: &mut self::FrameHeader,
) -> self::FrameDecodeStatus {
    if available < kFrameHeaderSize {
        return FrameDecodeStatus::NeedMoreBytes;
    }

    let mut bytes: [u8; kFrameHeaderSize] = [0_u8; kFrameHeaderSize];
    unsafe {
        core::ptr::copy_nonoverlapping(buf, bytes.as_mut_ptr(), kFrameHeaderSize);
    }
    let encoded: i32 = i32::from_ne_bytes(bytes);
    let extended_header_flag: bool = response_has_extended_header(encoded);
    let payload_size: i32 = response_payload_size(encoded);
    if payload_size < 0_i32 {
        return FrameDecodeStatus::Malformed;
    }

    out_header.payload_size = payload_size;
    out_header.extended_header_flag = extended_header_flag;
    FrameDecodeStatus::Complete
}

/// Zero-copy view of one complete buffered frame.
///
/// As with [`FrameHeader`], all closed-world C++ construction is value- or
/// explicit-initialization.  Keeping the aggregate shape preserves those call
/// sites while the native `Default` derive pins the same zero/null state.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default))]
pub struct FrameView {
    pub header: FrameHeader,
    pub payload: *const u8,
    pub payload_size: usize,
}

/// Append `[prefix][payload]` to the legacy C++ output vector.
///
/// On validation failure `out` is left byte-for-byte unchanged.
///
/// # Safety
///
/// When `payload_size > 0`, `payload` must address at least that many readable
/// bytes.  A null pointer with a non-zero size is rejected.
pub unsafe fn frame_codec_encode_into(
    out: &mut LegacyStdVector<u8>,
    payload: *const u8,
    payload_size: i32,
    extended_header_flag: bool,
) -> bool {
    if payload_size < 0_i32 {
        return false;
    }
    if payload_size > kMaxFramePayloadSize {
        return false;
    }
    if payload.is_null() && payload_size > 0_i32 {
        return false;
    }

    let previous_size: usize = out.len();
    let needed: usize = kFrameHeaderSize + payload_size as usize;
    out.resize(previous_size + needed, 0_u8);

    let header_out: *mut u8 = unsafe { out.as_mut_ptr().add(previous_size) };
    if !unsafe { frame_codec_write_header(header_out, payload_size, extended_header_flag) } {
        out.resize(previous_size, 0_u8);
        return false;
    }

    if payload_size > 0_i32 {
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

/// Streaming assembler for arbitrarily fragmented or coalesced frames.
///
/// The two fields intentionally reproduce the original C++ object layout.
/// The byte buffer owns all buffered bytes; `read_pos_` advances without
/// moving them on the hot path and compaction occurs only after a 64 KiB
/// prefix has been consumed.
#[repr(C)]
pub struct FrameStreamReader {
    // Rust keeps these fields private.  The current C++ emitter widens struct
    // field visibility; a closed-world audit found no consumer access, so the
    // widening does not weaken an invariant used anywhere in Mako.
    buf_: LegacyByteBuffer,
    read_pos_: usize,
}

const kCompactThresholdBytes: usize = 64_usize * 1024_usize;

impl FrameStreamReader {
    /// The C++ consumer lowers this marker to `FrameStreamReader()` rather
    /// than a Rust-style `new_()` factory.
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> FrameStreamReader {
        FrameStreamReader {
            buf_: Default::default(),
            read_pos_: 0_usize,
        }
    }

    /// Append arbitrary transport bytes.
    ///
    /// # Safety
    ///
    /// When `size > 0`, `data` must address at least `size` readable bytes.
    pub unsafe fn append(&mut self, data: *const u8, size: usize) {
        if size == 0_usize {
            return;
        }

        self.buf_.reserve(size);
        let mut index: usize = 0_usize;
        while index < size {
            let byte: LegacyCChar = unsafe { *data.add(index) } as LegacyCChar;
            self.buf_.push(byte);
            index += 1_usize;
        }
    }

    /// Peek at the next complete frame without consuming or copying it.
    pub fn next_frame(&self, out_view: &mut self::FrameView) -> self::FrameDecodeStatus {
        let available: usize = self.buffered_bytes();
        let head: *const u8 = unsafe { self.buf_.as_ptr().add(self.read_pos_) as *const u8 };

        let mut header: FrameHeader = FrameHeader {
            payload_size: 0_i32,
            extended_header_flag: false,
        };
        let status: FrameDecodeStatus =
            unsafe { frame_codec_peek_header(head, available, &mut header) };
        if status != FrameDecodeStatus::Complete {
            return status;
        }

        let total: usize = header.total_frame_size() as usize;
        if available < total {
            return FrameDecodeStatus::NeedMoreBytes;
        }

        let payload_size: usize = header.payload_size as usize;
        out_view.header = header;
        out_view.payload = unsafe { head.add(kFrameHeaderSize) };
        out_view.payload_size = payload_size;
        FrameDecodeStatus::Complete
    }

    /// Consume the next frame if, and only if, it is fully buffered.
    pub fn consume_frame(&mut self) {
        let available: usize = self.buffered_bytes();
        if available < kFrameHeaderSize {
            return;
        }

        let head: *const u8 = unsafe { self.buf_.as_ptr().add(self.read_pos_) as *const u8 };
        let mut header: FrameHeader = FrameHeader {
            payload_size: 0_i32,
            extended_header_flag: false,
        };
        if unsafe { frame_codec_peek_header(head, available, &mut header) }
            != FrameDecodeStatus::Complete
        {
            return;
        }

        let total: usize = header.total_frame_size() as usize;
        if available < total {
            return;
        }

        self.read_pos_ += total;
        fsr_compact_if_needed(self);
    }

    /// Discard every buffered byte while retaining vector capacity.
    pub fn reset(&mut self) {
        self.buf_.clear();
        self.read_pos_ = 0_usize;
    }

    pub fn buffered_bytes(&self) -> usize {
        self.buf_.len() - self.read_pos_
    }

    pub fn empty(&self) -> bool {
        self.buffered_bytes() == 0_usize
    }
}

fn fsr_compact_if_needed(reader: &mut FrameStreamReader) {
    if reader.read_pos_ == 0_usize {
        return;
    }
    if reader.read_pos_ < kCompactThresholdBytes {
        return;
    }

    let remaining: usize = reader.buf_.len() - reader.read_pos_;
    let mut index: usize = 0_usize;
    while index < remaining {
        reader.buf_[index] = reader.buf_[reader.read_pos_ + index];
        index += 1_usize;
    }

    // LegacyByteBuffer maps to rusty::String in C++.  Its truncate operation
    // validates a UTF-8 boundary even though this buffer intentionally stores
    // arbitrary bytes.  `remaining` is strictly inside the old allocation
    // because read_pos_ is non-zero, so replace that now-dead byte with an
    // ASCII NUL before truncating.  This also retains the historical vector
    // capacity and allocation-free compaction behavior.
    reader.buf_[remaining] = 0_i8;
    reader.buf_.truncate(remaining);
    reader.read_pos_ = 0_usize;
}
