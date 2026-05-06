#include "exec.h"

namespace janus {


ballot_t MultiPaxosExecutor::Prepare(const ballot_t ballot) {
  verify(0);
  return 0;
}

// removed dead `Accept`
// impl — `verify(0); return 0;` stub with no callers.

ballot_t MultiPaxosExecutor::Decide(ballot_t ballot, CmdData& cmd) {
  verify(0);
  return 0;
}

} // namespace janus
