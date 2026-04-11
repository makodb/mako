#pragma once

#include <cstdint>
#include <limits>

namespace rrr {

// Reserved RPC ID for internal heartbeat probes exchanged by ClientConnection.
constexpr int32_t kInternalHeartbeatRpcId = std::numeric_limits<int32_t>::min();

} // namespace rrr
