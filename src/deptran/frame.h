#pragma once

#include <std_compat.hpp>

#include <cstdint>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>

#include "config.h"
#include "srpc/srpc.hpp"

namespace janus {

class TxLogServer;
class Communicator;

class Frame {
 public:
  static Frame *GetFrame(int mode);

  Config::SiteInfo *site_info_ = nullptr;
  virtual ~Frame() = default;

  virtual TxLogServer *CreateScheduler() = 0;
  virtual Communicator *CreateCommo(
      rusty::Option<rusty::Arc<srpc::PollThread>> poll_thread_worker =
          rusty::None) = 0;
  virtual std::vector<srpc::ServiceProxy> CreateRpcServices(
      uint32_t site_id,
      TxLogServer *rep_sched,
      rusty::Arc<srpc::PollThread> poll_thread_worker) = 0;
};

} // namespace janus
