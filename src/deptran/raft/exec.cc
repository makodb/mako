#include "exec.h"

namespace janus {

// @unsafe - calls Log_warn (non-borrow-checked I/O)
ballot_t RaftExecutor::Prepare(const ballot_t ballot) {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] RaftExecutor::Prepare called but not implemented for Raft");
  return 0;
}

// removed dead `Accept` and
// `AppendEntries` impls — both were `Log_warn`-and-return-0 stubs
// with no callers anywhere in the tree.

// @unsafe - calls Log_warn (non-borrow-checked I/O)
ballot_t RaftExecutor::Decide(ballot_t ballot, CmdData& cmd) {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] RaftExecutor::Decide called but not implemented for Raft");
  return 0;
}

} // namespace janus
