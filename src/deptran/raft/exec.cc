#include "exec.h"

namespace janus {

// @unsafe - calls Log_warn (non-borrow-checked I/O)
ballot_t RaftExecutor::Prepare(const ballot_t ballot) {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] RaftExecutor::Prepare called but not implemented for Raft");
  return 0;
}

// @unsafe - calls Log_warn (non-borrow-checked I/O)
ballot_t RaftExecutor::Accept(const ballot_t ballot,
                                    shared_ptr<Marshallable> cmd) {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] RaftExecutor::Accept called but not implemented for Raft");
  return 0;
}

// @unsafe - calls Log_warn (non-borrow-checked I/O)
ballot_t RaftExecutor::AppendEntries(const ballot_t ballot,
                                         shared_ptr<Marshallable> cmd) {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] RaftExecutor::AppendEntries called but not implemented for Raft");
  return 0;
}

// @unsafe - calls Log_warn (non-borrow-checked I/O)
ballot_t RaftExecutor::Decide(ballot_t ballot, CmdData& cmd) {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] RaftExecutor::Decide called but not implemented for Raft");
  return 0;
}

} // namespace janus
