#pragma once

#include <memory>
#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "commo.h"
#include "server.h"
#include <rusty/arc.hpp>
#include <rusty/cell.hpp>

namespace janus {

// @safe
class RaftFrame : public Frame {
 private:
  // Safe shared mutable counter using Arc<Cell<T>> pattern
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_ = rusty::Arc<rusty::Cell<slotid_t>>::make(1);
#ifdef RAFT_TEST_CORO
  static std::mutex raft_test_mutex_;
  static std::shared_ptr<Coroutine> raft_test_coro_;
  static uint16_t n_replicas_;
  static map<siteid_t, RaftFrame*> frames_;
  static bool all_sites_created_s;
  static bool tests_done_;
  static uint16_t n_commo_created_;
#endif
 public:
  RaftFrame(int mode);
  ~RaftFrame();  // Destructor to clean up owned resources
  std::unique_ptr<RaftCommo> commo_;  // Owned RaftCommo, automatically cleaned up
  /* TODO: have another class for common data */
  std::unique_ptr<RaftServer> svr_;  // Owned RaftServer, automatically cleaned up
  Executor *CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 ClientControlServiceImpl *ccsi,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>()) override;
  vector<rrr::Service *> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<rrr::PollThread> poll_thread_worker,
                                           ServerControlServiceImpl *scsi) override;
};

} // namespace janus
