#include "application_log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace janus::raft {
namespace {

constexpr std::array<char, 8> kMagic = {'M', 'A', 'K', 'O', 'R', 'A', 'F', 'T'};
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 20;

void AppendU32Le(uint32_t value, std::string* out) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

uint32_t ReadU32Le(const char* data) {
  uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<uint32_t>(static_cast<unsigned char>(data[shift / 8]))
             << shift;
  }
  return value;
}

}  // namespace

bool EncodeApplicationLog(const char* payload,
                          int length,
                          uint32_t partition_id,
                          std::string* encoded) {
  if (!encoded || length < 0 ||
      length > std::numeric_limits<int>::max() -
                   static_cast<int>(kHeaderSize) ||
      (length > 0 && !payload)) {
    return false;
  }

  encoded->clear();
  encoded->reserve(kHeaderSize + static_cast<size_t>(length));
  encoded->append(kMagic.data(), kMagic.size());
  encoded->push_back(static_cast<char>(kVersion));
  encoded->push_back(0);  // flags, reserved for a future compatible version
  encoded->push_back(0);  // reserved
  encoded->push_back(0);  // reserved
  AppendU32Le(partition_id, encoded);
  AppendU32Le(static_cast<uint32_t>(length), encoded);
  encoded->append(payload ? payload : "", static_cast<size_t>(length));
  return true;
}

bool DecodeApplicationLog(const std::string& encoded,
                          const char** payload,
                          int* length,
                          uint32_t* partition_id) {
  if (!payload || !length || !partition_id || encoded.size() < kHeaderSize) {
    return false;
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), encoded.begin())) {
    return false;
  }
  if (static_cast<uint8_t>(encoded[8]) != kVersion || encoded[9] != 0 ||
      encoded[10] != 0 || encoded[11] != 0) {
    return false;
  }

  const uint32_t decoded_partition = ReadU32Le(encoded.data() + 12);
  const uint32_t decoded_length = ReadU32Le(encoded.data() + 16);
  if (decoded_length > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      static_cast<size_t>(decoded_length) != encoded.size() - kHeaderSize) {
    return false;
  }

  *partition_id = decoded_partition;
  *length = static_cast<int>(decoded_length);
  *payload = encoded.data() + kHeaderSize;
  return true;
}

}  // namespace janus::raft
