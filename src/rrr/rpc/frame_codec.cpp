
// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "frame_codec.hpp"

#include "../rrr.hpp"

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

void FrameStreamReader::append(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return;
    buf_.insert(buf_.end(), data, data + size);
}

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

void FrameStreamReader::compact_if_needed() {
    if (read_pos_ == 0) return;
    if (read_pos_ < kCompactThresholdBytes) return;

    const std::size_t remaining = buf_.size() - read_pos_;
    if (remaining > 0) {
        std::memmove(buf_.data(), buf_.data() + read_pos_, remaining);
    }
    buf_.resize(remaining);
    read_pos_ = 0;
}

}  // namespace rrr
