module;

#include <stdint.h>
#include <string.h>

#include <rusty/rusty.hpp>
#include <rusty/io.hpp>

export module rrr.frame_codec;

import std;
import rrr.internal_protocol;

// @safe - wire-protocol frame codec. write_header and peek_header are DSL
// over `&[u8]` slices now; only `encode_into` still carries a per-method
// `// @unsafe`, for the raw payload pointer and the memcpy into `out`.
// The POD structs and trivial accessors inherit namespace @safe.
// SP-5 DONE: FrameStreamReader is now built on `rusty::io::Cursor`.
// The buffer + read offset live in `cursor_`; the unread bytes are
// peeked via `cursor_.fill_buf()` (a `std::span`) and dropped via
// `cursor_.consume(n)`, so `next_frame` / `consume_frame` no longer do
// raw `buf_.data() + read_pos_` arithmetic and `compact_if_needed` no
// longer does `std::memmove` (it copies the unread tail off the span).
// The only residual @unsafe are inherent boundaries: `append` (raw
// transport pointer in) and the zero-copy FrameView payload pointer.
export namespace rrr {


// ---------------------------------------------------------------------------
// Wire-format constants
// ---------------------------------------------------------------------------
//
// `kFrameHeaderSize` is the on-wire size of the i32 carrying
// `<payload_size> | <extended_header_flag_bit>` (= sizeof(int32_t) = 4
// on every target we build for). `kMaxFramePayloadSize` is the low 31
// bits of that i32 (the high bit is reserved for the
// extended-header flag), capping the payload at 2 GiB - 1.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definitions.
#if RUSTYCPP_RUST
const kFrameHeaderSize: usize = 4;
const kMaxFramePayloadSize: i32 = 0x7fffffff;
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.1 version=1 rust_sha256=65acb26ca87ce3b0e1228a8f10bedaf8c0c0c20fde095d1fb4a702de2e70fd46*/
constexpr size_t kFrameHeaderSize = static_cast<size_t>(4);
constexpr int32_t kMaxFramePayloadSize = static_cast<int32_t>(2147483647);
/*RUSTYCPP:GEN-END id=frame_codec.1*/

// ---------------------------------------------------------------------------
// Decode results
// ---------------------------------------------------------------------------

// `FrameDecodeStatus` — return code from frame_codec_peek_header /
// FrameStreamReader::next_frame. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block.
#if RUSTYCPP_RUST
#[repr(i32)]
enum FrameDecodeStatus {
    NeedMoreBytes = 0,
    Complete = 1,
    Malformed = 2,
}

fn frame_decode_status_to_string(s: FrameDecodeStatus) -> &'static str {
    match s {
        FrameDecodeStatus::NeedMoreBytes => "NeedMoreBytes",
        FrameDecodeStatus::Complete => "Complete",
        FrameDecodeStatus::Malformed => "Malformed",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.decode_status version=1 rust_sha256=a551fc663d1f39de8aa705d886078b3ccd672134ae82bf29b0944f59b80ada57*/
enum class FrameDecodeStatus;
constexpr FrameDecodeStatus FrameDecodeStatus_NeedMoreBytes();
constexpr FrameDecodeStatus FrameDecodeStatus_Complete();
constexpr FrameDecodeStatus FrameDecodeStatus_Malformed();
std::string_view frame_decode_status_to_string(FrameDecodeStatus s);

enum class FrameDecodeStatus {
    NeedMoreBytes = 0,
    Complete = 1,
    Malformed = 2
};
inline constexpr FrameDecodeStatus FrameDecodeStatus_NeedMoreBytes() { return FrameDecodeStatus::NeedMoreBytes; }
inline constexpr FrameDecodeStatus FrameDecodeStatus_Complete() { return FrameDecodeStatus::Complete; }
inline constexpr FrameDecodeStatus FrameDecodeStatus_Malformed() { return FrameDecodeStatus::Malformed; }

std::string_view frame_decode_status_to_string(FrameDecodeStatus s) {
    return ({ auto&& _m = s; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == FrameDecodeStatus::NeedMoreBytes)) { _match_value.emplace(std::move(std::string_view("NeedMoreBytes"))); _m_matched = true; } if (!_m_matched && (_m == FrameDecodeStatus::Complete)) { _match_value.emplace(std::move(std::string_view("Complete"))); _m_matched = true; } if (!_m_matched && (_m == FrameDecodeStatus::Malformed)) { _match_value.emplace(std::move(std::string_view("Malformed"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=frame_codec.decode_status*/

// ---------------------------------------------------------------------------
// Frame header
// ---------------------------------------------------------------------------

/**
 * Decoded view of the 4-byte size prefix.
 *
 *   - `payload_size` excludes the 4-byte header itself.
 *   - `extended_header_flag` is set when the high bit of the on-wire i32
 *     is set. The RPC layer interprets this as "the response payload
 *     starts with `<server_instance_id>` after `<error_code>`".
 *     Request frames must always have this flag clear.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `/*RUSTYCPP:GEN-BEGIN ... END*\/` block with the C++ struct + method.
 * The generated struct is still an aggregate, so `FrameHeader{}`
 * continues to value-initialize both fields to 0/false at every call
 * site (peek_header, FrameView::header, next_frame, consume_frame).
 */
#if RUSTYCPP_RUST
struct FrameHeader {
    payload_size: i32,
    extended_header_flag: bool,
}

impl FrameHeader {
    fn total_frame_size(&self) -> i32 {
        self.payload_size + (kFrameHeaderSize as i32)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.3 version=1 rust_sha256=54b8840038c07349c37dba3672294103f924f418bbde9e2625bb295fcf5d3888*/
struct FrameHeader;

struct FrameHeader {
    int32_t payload_size;
    bool extended_header_flag;

    int32_t total_frame_size() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


int32_t FrameHeader::total_frame_size() const {
    return rusty::detail::deref_if_pointer_like(this->payload_size) + ((static_cast<int32_t>(kFrameHeaderSize)));
}
/*RUSTYCPP:GEN-END id=frame_codec.3*/

// ---------------------------------------------------------------------------
// Stateless encode / decode
// ---------------------------------------------------------------------------

/**
 * Encode a frame header into the first 4 bytes of `out_buf`. Returns
 * `false` if `payload_size` is negative or exceeds
 * `kMaxFramePayloadSize`, or if `out_buf` is shorter than the header;
 * in that case `out_buf` is left untouched and the caller must surface
 * a transport error.
 *
 * The on-wire size is written in host byte order to match the existing
 * `Marshal::write_bookmark` semantics. `to_ne_bytes()` lowers to
 * `std::bit_cast<std::array<uint8_t, 4>>`, which is byte-for-byte the
 * `memcpy` this used to do.
 *
 * Authored as inline Rust DSL. This took a raw `uint8_t*` + an implicit
 * "caller guarantees 4 bytes" contract, enforced only by a null check.
 * Per docs/dev/rrr_migration_policy.md rule 2 the call site was rewritten
 * to pass a slice instead of teaching the DSL to emit pointer arithmetic:
 * `&mut [u8]` lowers to `std::span<uint8_t>`, which cannot be null and
 * carries its own length, so the null check becomes a real bounds check.
 */
#if RUSTYCPP_RUST
fn frame_codec_write_header(out_buf: &mut [u8],
                            payload_size: i32,
                            extended_header_flag: bool) -> bool {
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
    let bytes = encoded.to_ne_bytes();
    out_buf[0] = bytes[0];
    out_buf[1] = bytes[1];
    out_buf[2] = bytes[2];
    out_buf[3] = bytes[3];
    true
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.write_header version=1 rust_sha256=547130034455231d856c4f0bc5409a449515ec828096af98f5bf56a0cc38c58e*/
bool frame_codec_write_header(std::span<uint8_t> out_buf, int32_t payload_size, bool extended_header_flag);

bool frame_codec_write_header(std::span<uint8_t> out_buf, int32_t payload_size, bool extended_header_flag) {
    if (rusty::detail::deref_if_pointer_like(payload_size) < 0) {
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(payload_size) > rusty::detail::deref_if_pointer_like(kMaxFramePayloadSize)) {
        return false;
    }
    if (rusty::len(out_buf) < rusty::detail::deref_if_pointer_like(kFrameHeaderSize)) {
        return false;
    }
    const int32_t encoded = encode_response_size(std::move(payload_size), std::move(extended_header_flag));
    const auto bytes = ([&]() { auto __v = encoded; return std::bit_cast<std::array<uint8_t, sizeof(__v)>>(__v); }());
    out_buf[static_cast<size_t>(0)] = bytes[static_cast<size_t>(0)];
    out_buf[static_cast<size_t>(1)] = bytes[static_cast<size_t>(1)];
    out_buf[static_cast<size_t>(2)] = bytes[static_cast<size_t>(2)];
    out_buf[static_cast<size_t>(3)] = bytes[static_cast<size_t>(3)];
    return true;
}
/*RUSTYCPP:GEN-END id=frame_codec.write_header*/

/**
 * Peek at the size prefix in `buf`. Does not require the full payload
 * to be buffered.
 *
 *   - `NeedMoreBytes` if `available < kFrameHeaderSize`.
 *   - `Malformed` if the encoded i32 decodes to a negative size.
 *     (The high bit is the extended-header flag, so a literal negative
 *     i32 with that flag clear is the malformed condition.)
 *   - `Complete` otherwise; `out_header` is populated.
 *
 * `Complete` here only means "header decoded"; the caller must still
 * compare `available` against `total_frame_size()` before treating the
 * frame as fully present. `FrameStreamReader` does that comparison
 * internally.
 */
// Authored as inline Rust DSL. Rule 2 again: the `(const uint8_t*,
// size_t available)` pointer+length pair collapses into a single
// `&[u8]`, which lowers to `std::span<const uint8_t>` and carries its
// own length — so `available` disappears from the signature rather than
// the DSL learning to emit `memcpy` off a raw pointer. `from_ne_bytes`
// reads the prefix in host byte order, matching the old memcpy.
#if RUSTYCPP_RUST
fn frame_codec_peek_header(buf: &[u8], out_header: &mut FrameHeader) -> FrameDecodeStatus {
    if buf.len() < kFrameHeaderSize {
        return FrameDecodeStatus::NeedMoreBytes;
    }
    let encoded: i32 = i32::from_ne_bytes([buf[0], buf[1], buf[2], buf[3]]);
    let ext: bool = response_has_extended_header(encoded);
    let payload: i32 = response_payload_size(encoded);
    if payload < 0 {
        return FrameDecodeStatus::Malformed;
    }
    out_header.payload_size = payload;
    out_header.extended_header_flag = ext;
    FrameDecodeStatus::Complete
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.peek_header version=1 rust_sha256=5c58d8dcf39312d45a761d25aac00fd80bc2a2c8ff5d812e48f669cb9d4b3706*/
FrameDecodeStatus frame_codec_peek_header(std::span<const uint8_t> buf, FrameHeader& out_header) {
    FrameHeader* out_header_shadow1 = &out_header;
    if (rusty::len(buf) < rusty::detail::deref_if_pointer_like(kFrameHeaderSize)) {
        return FrameDecodeStatus_NeedMoreBytes();
    }
    const int32_t encoded = rusty::from_ne_bytes<int32_t>(std::array{buf[static_cast<size_t>(0)], buf[static_cast<size_t>(1)], buf[static_cast<size_t>(2)], buf[static_cast<size_t>(3)]});
    bool ext = response_has_extended_header(std::move(encoded));
    int32_t payload = response_payload_size(std::move(encoded));
    if (rusty::detail::deref_if_pointer_like(payload) < 0) {
        return FrameDecodeStatus_Malformed();
    }
    (*out_header_shadow1).payload_size = std::move(payload);
    (*out_header_shadow1).extended_header_flag = std::move(ext);
    return FrameDecodeStatus_Complete();
}
/*RUSTYCPP:GEN-END id=frame_codec.peek_header*/

// ---------------------------------------------------------------------------
// Frame view (handed back from FrameStreamReader)
// ---------------------------------------------------------------------------

/**
 * View into a single decoded inbound frame. `payload` aliases the
 * `FrameStreamReader`'s internal buffer and is valid only until the
 * next `consume_frame()` / `append()` / `reset()` call.
 *
 * Note that `payload_size == header.payload_size` always; the field is
 * duplicated for ergonomic parity with `ChannelFrame`.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `/*RUSTYCPP:GEN-BEGIN ... END*\/` block with the C++ struct. The
 * generated struct is still an aggregate, so every call site's
 * `FrameView v{}` continues to value-init `header` (both fields 0),
 * `payload` (nullptr) and `payload_size` (0).
 */
#if RUSTYCPP_RUST
struct FrameView {
    header: FrameHeader,
    payload: *const u8,
    payload_size: usize,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.4 version=1 rust_sha256=b360ce69953a7f80a792566fe786167aaee1d16a0a48b9b9c4a51811f22da1bf*/
struct FrameView;

struct FrameView {
    FrameHeader header;
    const uint8_t* payload;
    size_t payload_size;
};
/*RUSTYCPP:GEN-END id=frame_codec.4*/

// ---------------------------------------------------------------------------
// Coalesced encoding helper
// ---------------------------------------------------------------------------

/**
 * Append one fully-formed frame to `out`:
 *
 *     [header (4 bytes)] [payload (payload_size bytes)]
 *
 * Returns `false` (without modifying `out`) if `payload_size` is out of
 * range or if `payload` is null with non-zero size.
 *
 * Callers loop to coalesce N frames into a single contiguous buffer
 * before issuing one `send(2)` syscall — this is what the TCP backend
 * will use to drain its outbound queue without one syscall per frame.
 */
// @unsafe - takes a raw `const uint8_t*` payload, advances `out.data() +
// offset` to write the header + memcpy the payload bytes.
bool frame_codec_encode_into(std::vector<std::uint8_t>& out,
                             const std::uint8_t* payload,
                             std::int32_t payload_size,
                             bool extended_header_flag);

// ---------------------------------------------------------------------------
// FrameStreamReader
// ---------------------------------------------------------------------------

/**
 * Buffers inbound bytes from a stream-oriented transport and emits
 * complete frames in wire order.
 *
 * Threading: not internally synchronized. Each connection's reader is
 * driven from a single poll thread; the channel layer enforces that
 * invariant.
 *
 * Memory: the reader holds a single contiguous `std::vector<uint8_t>`.
 * `consume_frame` advances a read offset rather than relocating bytes
 * on the hot path; the buffer is compacted (pending data shifted to
 * the front) when the consumed prefix grows past an implementation-
 * defined threshold so long-lived connections don't accumulate
 * unbounded slack.
 */
// The Cursor alias + construction helper let the DSL spell the field
// type and the `new()` factory init (the DSL can't express the
// `rusty::io::Cursor<std::vector<..>>` template args or a
// `std::vector{}` construction inline).
using FrameCursor = rusty::io::Cursor<std::vector<std::uint8_t>>;

inline FrameCursor make_frame_cursor() {
    return FrameCursor::new_(std::vector<std::uint8_t>{});
}

struct FrameStreamReader;

// Hand-written backing free fns for the DSL methods whose bodies are
// raw-pointer / std::span / std::vector interop (not DSL-expressible).
// Definitions in the impl namespace at the bottom of this file.
void fsr_append(FrameStreamReader& self, const std::uint8_t* data,
                std::size_t size);
FrameDecodeStatus fsr_next_frame(const FrameStreamReader& self,
                                 FrameView& out_view);
void fsr_consume_frame(FrameStreamReader& self);

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. Construction goes through the
// `FrameStreamReader::new_()` factory (`fn new()` in the DSL).
#if RUSTYCPP_RUST
struct FrameStreamReader {
    // SP-5: the buffer + read offset are owned by the Cursor.
    // `cursor_.position()` is the read offset (old `read_pos_`);
    // `cursor_.get_ref()` is the backing vector (old `buf_`). The
    // unread bytes are peeked via `cursor_.fill_buf()` (a `std::span`)
    // and dropped via `cursor_.consume(n)`.
    cursor_: FrameCursor,
    // copy-`=delete` marker: the manual class deleted its copy ctor (a
    // silent buffer copy would be a hot-path perf bug), and `FrameCursor`
    // alone is copyable. `Cell` is in the transpiler's known-non-copyable
    // set, so this field keeps the struct move-only. Never read.
    noncopy_: Cell<bool>,
}

impl FrameStreamReader {
    fn new() -> FrameStreamReader {
        FrameStreamReader {
            cursor_: make_frame_cursor(),
            noncopy_: Cell::new(false),
        }
    }

    // Append `size` bytes from `data` to the internal buffer.
    // No-op if `size == 0`. `data` may be null only if `size == 0`.
    // @unsafe - takes a raw `const uint8_t*` (pointer + size pair from
    // the transport).
    fn append(&mut self, data: *const u8, size: usize) {
        fsr_append(self, data, size)
    }

    // Try to view the next frame in the buffer.
    //   - `Complete`        — fills `out_view`. Bytes stay buffered until
    //                         `consume_frame()` is called.
    //   - `NeedMoreBytes`   — header or payload bytes still missing.
    //   - `Malformed`       — header decoded to a negative payload size;
    //                         caller should treat the stream as
    //                         corrupted and call `reset()`.
    // @unsafe - the free fn stores a zero-copy raw payload pointer into
    // the out FrameView.
    fn next_frame(&self, out_view: &mut FrameView) -> FrameDecodeStatus {
        fsr_next_frame(self, out_view)
    }

    // Drop the most recently peeked frame from the buffer. Must be
    // preceded by a `Complete` from `next_frame`. Calling without a
    // preceding `Complete` is a no-op.
    fn consume_frame(&mut self) {
        fsr_consume_frame(self)
    }

    // Drop everything in the buffer (e.g., after a malformed frame or
    // before a reconnect attempt).
    fn reset(&mut self) {
        self.cursor_.get_mut().clear();
        self.cursor_.set_position(0);
    }

    // Number of buffered bytes that have not yet been consumed.
    fn buffered_bytes(&self) -> usize {
        self.cursor_.remaining_len()
    }

    fn empty(&self) -> bool {
        self.buffered_bytes() == 0
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.frame_stream_reader version=1 rust_sha256=261c2ae9373c4fdb641b9b741b60a85e57923e8f9646d69afa82e8ecdca57065*/
struct FrameStreamReader;

struct FrameStreamReader {
    FrameCursor cursor_;
    rusty::Cell<bool> noncopy_;

    static FrameStreamReader new_();
    void append(const uint8_t* data, size_t size);
    FrameDecodeStatus next_frame(FrameView& out_view) const;
    void consume_frame();
    void reset();
    size_t buffered_bytes() const;
    bool empty() const;
};


FrameStreamReader FrameStreamReader::new_() {
    return FrameStreamReader{.cursor_ = make_frame_cursor(), .noncopy_ = rusty::Cell<bool>::new_(false)};
}

void FrameStreamReader::append(const uint8_t* data, size_t size) {
    fsr_append((*this), data, std::move(size));
}

FrameDecodeStatus FrameStreamReader::next_frame(FrameView& out_view) const {
    return fsr_next_frame((*this), out_view);
}

void FrameStreamReader::consume_frame() {
    fsr_consume_frame((*this));
}

void FrameStreamReader::reset() {
    this->cursor_.get_mut().clear();
    this->cursor_.set_position(0);
}

size_t FrameStreamReader::buffered_bytes() const {
    return this->cursor_.remaining_len();
}

bool FrameStreamReader::empty() const {
    return this->buffered_bytes() == static_cast<size_t>(0);
}
/*RUSTYCPP:GEN-END id=frame_codec.frame_stream_reader*/


}  // export namespace rrr

// @safe - impl namespace. Out-of-class definitions inherit their
// per-method `// @unsafe` from the matching declarations above.
namespace rrr {

namespace {

// Compact the buffer when the consumed prefix grows past this threshold,
// so long-lived connections don't accumulate unbounded slack at the
// front of the buffer. Tuned to a small multiple of a typical RPC frame
// to amortize the memmove cost across many frames.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definition.
#if RUSTYCPP_RUST
const kCompactThresholdBytes: usize = 64 * 1024;
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.2 version=1 rust_sha256=ade771f22e7be5d8311223bfcb4465698724808595170a67078906b947aaff5e*/
constexpr size_t kCompactThresholdBytes = static_cast<size_t>(64) * static_cast<size_t>(1024);
/*RUSTYCPP:GEN-END id=frame_codec.2*/

}  // namespace

// ---------------------------------------------------------------------------
// frame_codec_encode_into
// ---------------------------------------------------------------------------

// DEFERRED — the last hand-written function in this file, blocked on a
// data-structure migration rather than on anything about the codec.
//
// The `(const uint8_t* payload, i32 payload_size)` half is the same
// slice-in-disguise that write_header/peek_header turned out to be, and
// converts the same way. The blocker is `out`: a DSL `&mut Vec<u8>`
// lowers to `rusty::Vec<uint8_t>&`, which is the transpiled rustc Vec,
// NOT `std::vector`. Every caller passes `std::vector<std::uint8_t>`
// (`TcpOutBuf`, and `rusty::Mutex<std::vector<uint8_t>> outbound_`).
//
// So the rule-2 rewrite here is not a call-site edit, it is migrating
// the transport's outbound buffer type. `rusty::Vec` does offer
// data()/size()/clear()/resize(n, v), but tcp_channel drains that buffer
// with `buf.erase(buf.begin(), buf.begin() + offset)` — iterator-pair
// erase, which rustc's Vec does not have (it has `drain`). Converting
// therefore means rewriting the drain path on the hot outbound path of a
// branch whose stated Goal 1 is performance parity, which deserves its
// own change with its own measurements — not a side effect of porting a
// codec function.
//
// Enabling unit: migrate TcpOutBuf -> rusty::Vec<uint8_t> (drain via
// `drain`), then this function converts mechanically.
//
// @unsafe - see export declaration: raw `const uint8_t*` payload +
// `out.data() + offset` arithmetic + memcpy.
bool frame_codec_encode_into(std::vector<std::uint8_t>& out,
                             const std::uint8_t* payload,
                             std::int32_t payload_size,
                             bool extended_header_flag) {
    if (payload_size < 0)                          return false;
    if (payload_size > kMaxFramePayloadSize)       return false;
    if (payload == nullptr && payload_size > 0)    return false;

    const std::size_t prev_size = out.size();
    const std::size_t needed =
        kFrameHeaderSize + static_cast<std::size_t>(payload_size);
    out.resize(prev_size + needed);

    if (!frame_codec_write_header(std::span<std::uint8_t>(out).subspan(prev_size),
                                  payload_size,
                                  extended_header_flag)) {
        out.resize(prev_size);
        return false;
    }
    if (payload_size > 0) {
        std::memcpy(out.data() + prev_size + kFrameHeaderSize,
                    payload,
                    static_cast<std::size_t>(payload_size));
    }
    return true;
}

// ---------------------------------------------------------------------------
// FrameStreamReader
// ---------------------------------------------------------------------------

// @unsafe - takes raw `const uint8_t*` data + size pair from transport
// (an inherent boundary). The buffer growth itself is via the Cursor's
// owned vector.
void fsr_append(FrameStreamReader& self, const std::uint8_t* data,
                std::size_t size) {
    if (size == 0) return;
    auto& buf = self.cursor_.get_mut();
    buf.insert(buf.end(), data, data + size);
}

// @safe-ish - peeks the unread bytes via `cursor_.fill_buf()` (a span,
// no `buf_.data() + read_pos_` arithmetic). The lone @unsafe is storing
// the zero-copy `span.data() + kFrameHeaderSize` payload pointer into the
// out FrameView (inherent to FrameView being a view, not an owner).
FrameDecodeStatus fsr_next_frame(const FrameStreamReader& self,
                                 FrameView& out_view) {
    const std::span<const std::uint8_t> rem = self.cursor_.fill_buf();

    FrameHeader header;
    const FrameDecodeStatus header_status =
        frame_codec_peek_header(rem, header);
    if (header_status != FrameDecodeStatus::Complete) {
        return header_status;
    }

    const std::size_t total = static_cast<std::size_t>(header.total_frame_size());
    if (rem.size() < total) {
        return FrameDecodeStatus::NeedMoreBytes;
    }

    out_view.header       = header;
    // @unsafe { zero-copy view: span -> raw payload pointer }
    out_view.payload      = rem.data() + kFrameHeaderSize;
    out_view.payload_size = static_cast<std::size_t>(header.payload_size);
    return FrameDecodeStatus::Complete;
}

namespace {

// @safe - compacts by copying the unread tail (via `cursor_.fill_buf()`
// span) into a fresh buffer and re-seating the Cursor — no `std::memmove`
// + raw `buf_.data() + read_pos_` arithmetic. Rare path (only past the
// 64 KiB compaction threshold), so the one alloc is well amortized.
// (Was the private method `compact_if_needed`; only fsr_consume_frame
// calls it, so it drops off the struct API entirely.)
void fsr_compact_if_needed(FrameStreamReader& self) {
    const std::size_t read_pos = self.cursor_.position();
    if (read_pos == 0) return;
    if (read_pos < kCompactThresholdBytes) return;

    const std::span<const std::uint8_t> rem = self.cursor_.fill_buf();
    std::vector<std::uint8_t> compacted(rem.begin(), rem.end());
    self.cursor_ = rusty::io::Cursor<std::vector<std::uint8_t>>::new_(
        std::move(compacted));
}

}  // namespace

// @safe-ish - peeks via `cursor_.fill_buf()` (span); advances the read
// offset via `cursor_.consume(total)` instead of `read_pos_ += total`.
// @unsafe - stays a kernel for a NON-OBVIOUS reason: the EXPORT BOUNDARY.
// Its body is DSL-expressible (tried and verified: fill_buf() binds to a
// `let`, FrameHeader is a two-field DSL struct, and the peek out-param
// works as `&header`). But inlining it into the DSL method
// FrameStreamReader::consume_frame moves this call to
// fsr_compact_if_needed UP into `export namespace rrr` (lines 26-524),
// while that kernel is defined in the non-exported `namespace rrr` below.
// A forward declaration inside the export block is itself exported and
// does not match a non-exported definition, so it COMPILES and then fails
// to LINK ("undefined reference to rrr::fsr_compact_if_needed").
//
// Fixing it means exporting the kernel (a module API change) or
// restructuring the namespace blocks -- both larger than the 15 lines.
// Attempted and reverted; see playbook §7.34. Do not retry without
// deciding on one of those two.
void fsr_consume_frame(FrameStreamReader& self) {
    const std::span<const std::uint8_t> rem = self.cursor_.fill_buf();
    if (rem.size() < kFrameHeaderSize) return;

    FrameHeader header;
    if (frame_codec_peek_header(rem, header)
        != FrameDecodeStatus::Complete) {
        return;
    }
    const std::size_t total = static_cast<std::size_t>(header.total_frame_size());
    if (rem.size() < total) return;

    self.cursor_.consume(total);
    fsr_compact_if_needed(self);
}


}  // namespace rrr
