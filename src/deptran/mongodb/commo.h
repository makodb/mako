#pragma once

#include "../communicator.h"

namespace janus {

class MongodbCommo : public Communicator {
 public:
  MongodbCommo() = delete;
  MongodbCommo(rusty::Option<rusty::Arc<PollThread>>);

  // Workstream N L10f-prep6aw (2026-05-03): take const janus::Command&;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void BroadcastCommit(const parid_t par_id,
                       const janus::Command& cmd);
};

} // namespace janus
