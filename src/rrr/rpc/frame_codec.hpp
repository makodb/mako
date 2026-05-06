#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstring>


#include "internal_protocol.hpp"

/**
 * SRPC Channel Frame Codec.
 *
 * Centralizes the wire-level framing rules that previously lived inline
 * inside `ClientConnection::handle_read` / `ServerConnection::handle_read`
 * and `ClientConnection::send_request` / `ServerConnection::reply`.
 *
 * Wire format (unchanged):
 *
 *     <size:int32_t> <payload: size bytes>
 *
 * `size` is in host byte order to match `Marshal::peek` / `set_bookmark`
 * (existing implementations memcpy the i32 directly without endian
 * conversion). The high bit of `size` carries the response
 * extended-header flag (`internal_protocol.hpp::kResponseHeaderExtFlag`);
 * request frames must keep that bit clear.
 *
 * The codec is intentionally stateless except for `FrameStreamReader`,
 * which buffers fragmented inbound bytes and yields complete frames in
 * arrival order. It does not interpret payload bytes — that is the RPC
 * layer's job.
 *
 * ============================================================================
 * Why a dedicated codec module
 * ============================================================================
 *
 * Today, three places reimplement the same framing rules:
 *
 *   - `ClientConnection::handle_read`   — peek size, decode extended bit,
 *                                          consume size+payload.
 *   - `ServerConnection::handle_read`   — peek size, consume size+payload.
 *   - `ClientConnection::send_request`  — bookmark size, write payload,
 *                                          fill size; symmetrically for
 *                                          `ServerConnection::reply`.
 *
 * Splitting the channel layer needs all three to be
 * driven from one place so the channel sees frames as opaque
 * `(payload, size)` byte spans. This module provides that single source
 * of truth. Byte-for-byte wire compatibility is guaranteed by the
 * contract guard test in `rpc_frame_codec_test.cc`.
 *
 * ============================================================================
 * Fragmentation and coalescing
 * ============================================================================
 *
 * Real TCP reads return arbitrary chunk sizes: half a header, three
 * frames coalesced, etc. `FrameStreamReader::append` accepts any number
 * of bytes; `next_frame` is the only authority on whether a complete
 * frame is now available.
 *
 *   - `next_frame` returns `NeedMoreBytes` until the full
 *     `<size> + <payload>` window is buffered.
 *   - `next_frame` returns `Malformed` if the size header decodes to a
 *     negative payload size, signaling stream corruption.
 *   - On `Complete`, the returned `FrameView::payload` aliases the
 *     reader's internal buffer and stays valid until the next
 *     `consume_frame()` / `append()` / `reset()` call.
 *
 * Coalesced writes are supported by `frame_codec_encode_into`, which
 * appends one frame to a `std::vector<uint8_t>`; callers loop to batch
 * N frames into a single `send(2)` syscall.
 */
namespace rrr {

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

    constexpr std::int32_t total_frame_size() const {
        return payload_size + static_cast<std::int32_t>(kFrameHeaderSize);
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
inline bool frame_codec_write_header(std::uint8_t* out_buf,
                                     std::int32_t payload_size,
                                     bool extended_header_flag) {
    if (out_buf == nullptr) return false;
    if (payload_size < 0)   return false;
    if (payload_size > kMaxFramePayloadSize) return false;

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
    void append(const std::uint8_t* data, std::size_t size);

    // Try to view the next frame in the buffer.
    //   - `Complete`        — fills `out_view`. Bytes stay buffered until
    //                         `consume_frame()` is called.
    //   - `NeedMoreBytes`   — header or payload bytes still missing.
    //   - `Malformed`       — header decoded to a negative payload size;
    //                         caller should treat the stream as
    //                         corrupted and call `reset()`.
    FrameDecodeStatus next_frame(FrameView& out_view) const;

    // Drop the most recently peeked frame from the buffer. Must be
    // preceded by a `Complete` from `next_frame`. Calling without a
    // preceding `Complete` is a no-op.
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

}  // namespace rrr
