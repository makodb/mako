#pragma once

#include "deptran/classic/coordinator.h"

namespace janus {
class CoordinatorNone : public CoordinatorClassic {
 public:
  using CoordinatorClassic::CoordinatorClassic;
  enum Phase {INIT_END=0, DISPATCH=1};
  double cur_time_3_times;
  void GotoNextPhase() override;
};
} // namespace janus
