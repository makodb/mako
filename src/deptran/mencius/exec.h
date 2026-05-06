#pragma once

#include "../__dep__.h"
#include "../executor.h"
#include "../command.h"

namespace janus {

class MenciusExecutor: public Executor {
 public:
  using Executor::Executor;

  /**
   * return max_ballot
   */
  ballot_t Prepare(const ballot_t ballot);

  // removed dead `Suggest`
  // method — `verify(0); return 0;` stub with no callers.

  ballot_t Decide(ballot_t ballot, CmdData& cmd);
};

} // namespace janus
