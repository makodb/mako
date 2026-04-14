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
  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  ballot_t Prepare(const ballot_t ballot);

  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  ballot_t Accept(const ballot_t ballot, shared_ptr<Marshallable> cmd);

  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  ballot_t AppendEntries(const ballot_t ballot, shared_ptr<Marshallable> cmd);

  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  ballot_t Decide(ballot_t ballot, CmdData& cmd);
};

} // namespace janus
