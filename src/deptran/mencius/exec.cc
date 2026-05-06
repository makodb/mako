#include "exec.h"

namespace janus {


ballot_t MenciusExecutor::Prepare(const ballot_t ballot) {
  verify(0);
  return 0;
}

// removed dead `Suggest`
// impl — `verify(0); return 0;` stub with no callers.

ballot_t MenciusExecutor::Decide(ballot_t ballot, CmdData& cmd) {
  verify(0);
  return 0;
}

} // namespace janus
