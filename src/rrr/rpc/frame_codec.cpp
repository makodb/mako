module;

#include <stdint.h>
#include <string.h>

#include <rusty/rusty.hpp>

export module rrr.frame_codec;

import std;
import rrr.internal_protocol;

// @safe - wire-protocol frame codec. The free codec functions and the
// FrameStreamReader methods that take or compute on raw `uint8_t*` /
// `const uint8_t*` (write_header / peek_header / encode_into /
// FrameStreamReader::append / next_frame / consume_frame /
// compact_if_needed) carry per-method `// @unsafe` because they do
// raw pointer arithmetic + std::memcpy / std::memmove on the
// transport hot path. The trivial accessors (reset, buffered_bytes,
// empty) and the POD structs inherit namespace @safe.
// SP-5 follow-up: rewrite this codec on top of `rusty::io::Cursor`
// once perf benchmarks of the cursor path are in.
export namespace rrr {


// ---------------------------------------------------------------------------
// Wire-format constants
// ---------------------------------------------------------------------------

// Size of the on-wire header (a single i32 carrying payload size + flag).
inline constexpr std::size_t kFrameHeaderSize = sizeof(std::int32_t);

// Maximum payload size representable on the wire. The high bit of the i32
// is reserved for the response extended-header flag, so the remaining 31
// bits cap the payload at 2 GiB - 1.
inline constexpr std::int32_t kMaxFramePayloadSize =
    static_cast<std::int32_t>(kResponseSizeMask);

// ---------------------------------------------------------------------------
// Decode results
// ---------------------------------------------------------------------------

enum class FrameDecodeStatus : int {
    NeedMoreBytes = 0, // not enough buffered bytes to form a complete frame
    Complete      = 1, // a full frame is available
    Malformed     = 2, // size header decodes to a negative or out-of-range size
};

inline constexpr const char* frame_decode_status_to_string(FrameDecodeStatus s) {
    switch (s) {
        case FrameDecodeStatus::NeedMoreBytes: return "NeedMoreBytes";
        case FrameDecodeStatus::Complete:      return "Complete";
        case FrameDecodeStatus::Malformed:     return "Malformed";
    }
    return "Unknown";
}

// Free helpers backing three pure-arithmetic / range-check pieces of
// the frame codec: `FrameHeader::total_frame_size` (payload + 4-byte
// header), `frame_codec_encode_into`'s payload-size validation, and
// `FrameStreamReader::compact_if_needed`'s threshold check. Authored
// as inline Rust DSL.
#if RUSTYCPP_RUST
fn frame_header_total_size(payload_size: i32) -> i32 {
    payload_size + 4
}

fn frame_codec_payload_size_valid(payload_size: i32, max_size: i32) -> bool {
    payload_size >= 0 && payload_size <= max_size
}

fn frame_codec_should_compact(read_pos: u64, threshold: u64) -> bool {
    read_pos != 0 && read_pos >= threshold
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame_codec.1 version=1 rust_sha256=00c5af3d52b9278063ac134032b89ded2ded7b016b6bc523085fee261088c5c2*/
int32_t frame_header_total_size(int32_t payload_size);
bool frame_codec_payload_size_valid(int32_t payload_size, int32_t max_size);
bool frame_codec_should_compact(uint64_t read_pos, uint64_t threshold);

int32_t frame_header_total_size(int32_t payload_size) {
    return rusty::detail::deref_if_pointer_like(payload_size) + static_cast<int32_t>(4);
}

bool frame_codec_payload_size_valid(int32_t payload_size, int32_t max_size) {
    return (rusty::detail::deref_if_pointer_like(payload_size) >= 0) && (rusty::detail::deref_if_pointer_like(payload_size) <= rusty::detail::deref_if_pointer_like(max_size));
}

bool frame_codec_should_compact(uint64_t read_pos, uint64_t threshold) {
    return (rusty::detail::deref_if_pointer_like(read_pos) != static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(read_pos) >= rusty::detail::deref_if_pointer_like(threshold));
}
/*RUSTYCPP:GEN-END id=frame_codec.1*/

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
 */
struct FrameHeader {
    std::int32_t payload_size = 0;
    bool         extended_header_flag = false;

    std::int32_t total_frame_size() const {
        return frame_header_total_size(payload_size);
    }
};

// ---------------------------------------------------------------------------
// Stateless encode / decode
// ---------------------------------------------------------------------------

/**
 * Encode a frame header into the first 4 bytes of `out_buf`. Returns
 * `false` if `payload_size` is negative or exceeds
 * `kMaxFramePayloadSize`; in that case `out_buf` is left untouched and
 * the caller must surface a transport error.
 *
 * The on-wire size is written in host byte order to match the existing
 * `Marshal::write_bookmark` semantics.
 */
// @unsafe - writes the 4-byte size prefix into a raw `uint8_t*` via memcpy.
inline bool frame_codec_write_header(std::uint8_t* out_buf,
                                     std::int32_t payload_size,
                                     bool extended_header_flag) {
    if (out_buf == nullptr) return false;
    if (!frame_codec_payload_size_valid(payload_size, kMaxFramePayloadSize)) {
        return false;
    }

    const std::int32_t encoded =
        encode_response_size(payload_size, extended_header_flag);
    std::memcpy(out_buf, &encoded, kFrameHeaderSize);
    return true;
}

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
// @unsafe - reads the 4-byte size prefix out of a raw `const uint8_t*` via memcpy.
inline FrameDecodeStatus frame_codec_peek_header(const std::uint8_t* buf,
                                                 std::size_t available,
                                                 FrameHeader& out_header) {
    if (available < kFrameHeaderSize) {
        return FrameDecodeStatus::NeedMoreBytes;
    }
    std::int32_t encoded = 0;
    std::memcpy(&encoded, buf, kFrameHeaderSize);

    const bool ext = response_has_extended_header(encoded);
    const std::int32_t payload = response_payload_size(encoded);
    if (payload < 0) {
        return FrameDecodeStatus::Malformed;
    }
    out_header.payload_size = payload;
    out_header.extended_header_flag = ext;
    return FrameDecodeStatus::Complete;
}

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
 */
struct FrameView {
    FrameHeader          header;
    const std::uint8_t*  payload = nullptr;
    std::size_t          payload_size = 0;
};

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
class FrameStreamReader {
 public:
    FrameStreamReader();
    ~FrameStreamReader();

    FrameStreamReader(const FrameStreamReader&)            = delete;
    FrameStreamReader& operator=(const FrameStreamReader&) = delete;
    FrameStreamReader(FrameStreamReader&&)                 = default;
    FrameStreamReader& operator=(FrameStreamReader&&)      = default;

    // Append `size` bytes from `data` to the internal buffer.
    // No-op if `size == 0`. `data` may be null only if `size == 0`.
    // @unsafe - takes a raw `const uint8_t*` (pointer + size pair from
    // the transport).
    void append(const std::uint8_t* data, std::size_t size);

    // Try to view the next frame in the buffer.
    //   - `Complete`        — fills `out_view`. Bytes stay buffered until
    //                         `consume_frame()` is called.
    //   - `NeedMoreBytes`   — header or payload bytes still missing.
    //   - `Malformed`       — header decoded to a negative payload size;
    //                         caller should treat the stream as
    //                         corrupted and call `reset()`.
    // @unsafe - computes `buf_.data() + read_pos_` and stores a raw
    // `const uint8_t*` payload pointer into the out FrameView.
    FrameDecodeStatus next_frame(FrameView& out_view) const;

    // Drop the most recently peeked frame from the buffer. Must be
    // preceded by a `Complete` from `next_frame`. Calling without a
    // preceding `Complete` is a no-op.
    // @unsafe - re-peeks the header via raw `buf_.data() + read_pos_`.
    void consume_frame();

    // Drop everything in the buffer (e.g., after a malformed frame or
    // before a reconnect attempt).
    void reset();

    // Number of buffered bytes that have not yet been consumed.
    std::size_t buffered_bytes() const;

    bool empty() const { return buffered_bytes() == 0; }

 private:
    void compact_if_needed();

    std::vector<std::uint8_t> buf_;
    std::size_t               read_pos_ = 0;
};


}  // export namespace rrr

// @safe - impl namespace. Out-of-class definitions inherit their
// per-method `// @unsafe` from the matching declarations above.
namespace rrr {

namespace {

// Compact the buffer when the consumed prefix grows past this threshold,
// so long-lived connections don't accumulate unbounded slack at the
// front of the buffer. Tuned to a small multiple of a typical RPC frame
// to amortize the memmove cost across many frames.
constexpr std::size_t kCompactThresholdBytes = 64 * 1024;

}  // namespace

// ---------------------------------------------------------------------------
// frame_codec_encode_into
// ---------------------------------------------------------------------------

// @unsafe - see export declaration: raw `const uint8_t*` payload +
// `out.data() + offset` arithmetic + memcpy.
bool frame_codec_encode_into(std::vector<std::uint8_t>& out,
                             const std::uint8_t* payload,
                             std::int32_t payload_size,
                             bool extended_header_flag) {
    if (!frame_codec_payload_size_valid(payload_size, kMaxFramePayloadSize)) {
        return false;
    }
    if (payload == nullptr && payload_size > 0)    return false;

    const std::size_t prev_size = out.size();
    const std::size_t needed =
        kFrameHeaderSize + static_cast<std::size_t>(payload_size);
    out.resize(prev_size + needed);

    if (!frame_codec_write_header(out.data() + prev_size,
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

FrameStreamReader::FrameStreamReader() = default;
FrameStreamReader::~FrameStreamReader() = default;

// @unsafe - takes raw `const uint8_t*` data + size pair from transport.
void FrameStreamReader::append(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return;
    buf_.insert(buf_.end(), data, data + size);
}

// @unsafe - `buf_.data() + read_pos_` arithmetic; stores a raw
// `const uint8_t*` payload pointer into the out FrameView.
FrameDecodeStatus FrameStreamReader::next_frame(FrameView& out_view) const {
    const std::size_t available = buffered_bytes();
    const std::uint8_t* head = buf_.data() + read_pos_;

    FrameHeader header;
    const FrameDecodeStatus header_status =
        frame_codec_peek_header(head, available, header);
    if (header_status != FrameDecodeStatus::Complete) {
        return header_status;
    }

    const std::size_t total = static_cast<std::size_t>(header.total_frame_size());
    if (available < total) {
        return FrameDecodeStatus::NeedMoreBytes;
    }

    out_view.header       = header;
    out_view.payload      = head + kFrameHeaderSize;
    out_view.payload_size = static_cast<std::size_t>(header.payload_size);
    return FrameDecodeStatus::Complete;
}

// @unsafe - re-peeks the header via raw `buf_.data() + read_pos_`.
void FrameStreamReader::consume_frame() {
    const std::size_t available = buffered_bytes();
    if (available < kFrameHeaderSize) return;

    FrameHeader header;
    if (frame_codec_peek_header(buf_.data() + read_pos_, available, header)
        != FrameDecodeStatus::Complete) {
        return;
    }
    const std::size_t total = static_cast<std::size_t>(header.total_frame_size());
    if (available < total) return;

    read_pos_ += total;
    compact_if_needed();
}

void FrameStreamReader::reset() {
    buf_.clear();
    read_pos_ = 0;
}

std::size_t FrameStreamReader::buffered_bytes() const {
    return buf_.size() - read_pos_;
}

// @unsafe - `std::memmove` from `buf_.data() + read_pos_` to `buf_.data()`.
void FrameStreamReader::compact_if_needed() {
    if (!frame_codec_should_compact(read_pos_, kCompactThresholdBytes)) {
        return;
    }

    const std::size_t remaining = buf_.size() - read_pos_;
    if (remaining > 0) {
        std::memmove(buf_.data(), buf_.data() + read_pos_, remaining);
    }
    buf_.resize(remaining);
    read_pos_ = 0;
}


}  // namespace rrr
