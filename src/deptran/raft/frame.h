#pragma once

#include <memory>
#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "commo.h"
#include "server.h"
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>

namespace janus {

// @unsafe - inherits from non-@interface Frame (individual methods are @safe)
class RaftFrame : public Frame {
 private:
  // Safe shared mutable counter using Arc<Cell<T>> pattern
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_ = rusty::Arc<rusty::Cell<slotid_t>>::make(1);
#ifdef RAFT_TEST_CORO
  static std::mutex raft_test_mutex_;
  // raft_test_fiber_ demoted to a file-scope static in frame.cc because
  // rusty::Rc is now module-only (no header). All references live in
  // frame.cc; nothing outside this TU consumes the field.
  static uint16_t n_replicas_;
  static map<siteid_t, RaftFrame*> frames_;
  static bool all_sites_created_s;
  static bool tests_done_;
  static uint16_t n_commo_created_;
  static bool is_lab_test_config_;        // True if running raft lab test (1 partition, 5 replicas)
  static bool lab_test_config_checked_;   // True once we've checked the config
  static bool IsRaftLabTestConfig();      // Check if we're in lab test configuration
#endif
 public:
  RaftFrame(int mode);
  ~RaftFrame();  // Destructor to clean up owned resources
  std::unique_ptr<RaftCommo> commo_;  // @unsafe - unique_ptr kept for test file compatibility
  /* TODO: have another class for common data */
  std::unique_ptr<RaftServer> svr_;  // @unsafe - unique_ptr kept for test file compatibility
  Executor *CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>()) override;
  vector<srpc::ServiceProxy> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<srpc::PollThread> poll_thread_worker) override;
};

} // namespace janus
