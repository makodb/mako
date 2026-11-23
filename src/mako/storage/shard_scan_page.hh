#pragma once

#include <cstddef>
#include <cstdint>

namespace mako {
struct ShardingRequest;

// Optional backend reader for a range page on an admitted participant.
class ShardScanPageReader {
public:
  virtual ~ShardScanPageReader() = default;
  virtual void read_scan_page(const ShardingRequest& request, const uint8_t* input,
                              size_t length, uint8_t* output, size_t capacity,
                              size_t* output_length) = 0;
};
}
