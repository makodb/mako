#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>





namespace rrr {

// Reserved RPC ID for internal heartbeat probes exchanged by ClientConnection.
constexpr int32_t kInternalHeartbeatRpcId = std::numeric_limits<int32_t>::min();

// Response-frame extension flag (high bit in <size> field).
// Wire format remains: <size> <header...> <payload...>
// If this flag is set, response header includes <server_instance_id> after <error_code>.
constexpr uint32_t kResponseHeaderExtFlag = 0x80000000u;
constexpr uint32_t kResponseSizeMask = 0x7fffffffu;

inline constexpr bool response_has_extended_header(int32_t encoded_size) {
    return (static_cast<uint32_t>(encoded_size) & kResponseHeaderExtFlag) != 0;
}

inline constexpr int32_t response_payload_size(int32_t encoded_size) {
    return static_cast<int32_t>(static_cast<uint32_t>(encoded_size) & kResponseSizeMask);
}

inline constexpr int32_t encode_response_size(int32_t payload_size, bool extended_header) {
    const uint32_t base = static_cast<uint32_t>(payload_size) & kResponseSizeMask;
    return static_cast<int32_t>(extended_header ? (base | kResponseHeaderExtFlag) : base);
}

} // namespace rrr
