#include "exec.h"

namespace janus {

// Compatibility-only hook: no Raft call site invokes the classic prepare
// protocol. Returning the supplied ballot preserves the harmless legacy
// shape without producing a misleading runtime warning.
ballot_t RaftExecutor::Prepare(const ballot_t ballot) {
  return ballot;
}

// removed dead `Accept` and
// `AppendEntries` impls — both were `Log_warn`-and-return-0 stubs
// with no callers anywhere in the tree.

// Compatibility-only hook: Raft commit/application bypasses this classic
// executor entry point. Preserve the ballot and leave command application to
// RaftServer's normal replay path.
ballot_t RaftExecutor::Decide(ballot_t ballot, CmdData& cmd) {
  (void)cmd;
  return ballot;
}

} // namespace janus
