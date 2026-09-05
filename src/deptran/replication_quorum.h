#pragma once

#include "__dep__.h"

namespace janus {

// Construction shim for the inline-Rust DSL `janus::QuorumEventWrapper`.
// Protocol-specific quorum events derive from this small replication-only
// adapter rather than from the RPC transport.
class QuorumEventBase : public QuorumEventWrapper {
 public:
  QuorumEventBase(int n_total, int quorum)
      : QuorumEventWrapper(QuorumEventWrapper::new_(n_total, quorum)) {}
};

}  // namespace janus
