#include "__dep__.h"
#include "frame.h"
#include "paxos/frame.h"
#include "raft/frame.h"

namespace janus {

Frame* Frame::GetFrame(int mode) {
  switch (mode) {
    case MODE_MULTI_PAXOS:
      return new MultiPaxosFrame();
    case MODE_RAFT:
      return new RaftFrame();
    default:
      Log_error("Unsupported replication frame mode: {}", mode);
      verify(0);
      return nullptr;
  }
}
} // namespace janus;
