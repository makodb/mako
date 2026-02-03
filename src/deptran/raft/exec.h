#pragma once

#include "../__dep__.h"
#include "../executor.h"
#include "../command.h"

namespace janus {

class RaftExecutor: public Executor {
 public:
  using Executor::Executor;

  /**
   * return max_ballot
   */
  // @safe
  ballot_t Prepare(const ballot_t ballot);

  // @safe
  ballot_t Accept(const ballot_t ballot, shared_ptr<Marshallable> cmd);

  // @safe
  ballot_t AppendEntries(const ballot_t ballot, shared_ptr<Marshallable> cmd);

  // @safe
  ballot_t Decide(ballot_t ballot, CmdData& cmd);
};

} // namespace janus
