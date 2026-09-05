#pragma once

#include <cstdint>
#include <string>

namespace janus::raft {

// Encodes the Mako application bytes and their routing partition inside the
// generic replication LogEntry payload. The fixed header makes the payload
// self-identifying and leaves room for future compatible versions.
bool EncodeApplicationLog(const char* payload,
                          int length,
                          uint32_t partition_id,
                          std::string* encoded);

// Returns views into `encoded`; callers must keep that string alive until they
// finish consuming `payload`.
bool DecodeApplicationLog(const std::string& encoded,
                          const char** payload,
                          int* length,
                          uint32_t* partition_id);

}  // namespace janus::raft
