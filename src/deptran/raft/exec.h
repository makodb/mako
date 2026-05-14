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

  // removed dead `Accept` and
  // `AppendEntries` methods — both `Log_warn`-and-return-0 stubs that
  // didn't override anything in the `Executor` base, and had no
  // callers anywhere in the tree.

  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  ballot_t Decide(ballot_t ballot, CmdData& cmd);
};

} // namespace janus
