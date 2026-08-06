#pragma once

#include "../__dep__.h"
#include "../executor.h"
#include "../command.h"

namespace janus {

class RaftExecutor: public Executor {
 public:
  using Executor::Executor;

  // Legacy Executor compatibility hooks. Raft does not use the classic
  // prepare/decide executor protocol; RaftServer::Start/commit processing is
  // the active path. Keep these symbols for callers that construct an
  // Executor through the legacy Frame factory.
  ballot_t Prepare(const ballot_t ballot);

  // removed dead `Accept` and
  // `AppendEntries` methods — both `Log_warn`-and-return-0 stubs that
  // didn't override anything in the `Executor` base, and had no
  // callers anywhere in the tree.

  ballot_t Decide(ballot_t ballot, CmdData& cmd);
};

} // namespace janus
