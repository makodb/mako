#pragma once

#include <functional>
#include <mutex>
#include <utility>

#include "constants.h"
#include "mako_commands.h"

namespace janus {

class Communicator;

// Common state shared by the two replication engines. Transaction execution,
// MemDB ownership, epoch management, and the retired Jetpack recovery plane do
// not belong in the replication server hierarchy.
class TxLogServer {
 public:
  using LearnerAction = std::function<int(int, Command)>;

  locid_t loc_id_ = static_cast<locid_t>(-1);
  siteid_t site_id_ = static_cast<siteid_t>(-1);
  LearnerAction app_next_{};
  Communicator* commo_ = nullptr;
  parid_t partition_id_ = 0;
  std::recursive_mutex mtx_{};

  TxLogServer() = default;
  virtual ~TxLogServer();

  void RegLearnerAction(LearnerAction learner_action) {
    app_next_ = std::move(learner_action);
  }
};

}  // namespace janus
