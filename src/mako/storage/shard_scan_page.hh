#pragma once

#include <cstddef>
#include <cstdint>
#include "cluster/full_scan.h"
#include "lib/sharding_admission.h"
#include "storage/abstract_db.h"

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

namespace mako {
inline void require_scan_status(uint32_t status) {
  if (status != MAKO_SHARD_OK) throw abstract_db::abstract_abort_exception();
}
inline MakoScanIdentity scan_identity(const ShardingRequest& request) {
  if (request.coordinate_length > sizeof(request.coordinate))
    throw abstract_db::abstract_abort_exception();
  return {request.transaction, request.grant, request.snapshot_version, request.table,
          uint32_t(request.fixed_coordinate),
          {request.coordinate, request.coordinate_length}};
}
struct ScanPageState {
  MakoScanPage* page = nullptr;
  ~ScanPageState() { mako_scan_page_free(page); }
  ScanPageState() = default;
  ScanPageState(const ScanPageState&) = delete;
  ScanPageState& operator=(const ScanPageState&) = delete;
};
}
