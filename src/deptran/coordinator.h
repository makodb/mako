#pragma once

#include "constants.h"

namespace janus {

class Communicator;

// Common ownership surface for replication coordinators. Protocol-specific
// phase state belongs to the derived coordinator.
class Coordinator {
 public:
  locid_t loc_id_ = static_cast<locid_t>(-1);
  parid_t par_id_ = static_cast<parid_t>(-1);
  Communicator* commo_ = nullptr;

  virtual ~Coordinator() = default;
};

}  // namespace janus
