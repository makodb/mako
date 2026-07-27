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
#include <rusty/rc.hpp>

namespace janus {

// @unsafe - owns the Raft protocol singletons behind raw-pointer factory APIs
// inherited from Frame. Callers receive borrowed raw pointers from
// CreateScheduler()/CreateCommo(); RaftFrame keeps ownership in the unique_ptrs.
class RaftFrame : public Frame {
 private:
  // Safe shared mutable counter using Arc<Cell<T>> pattern
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_ = rusty::Arc<rusty::Cell<slotid_t>>::make(1);
#ifdef RAFT_TEST_CORO
  // @unsafe - test-only global coordination state; keep hand-written while the
  // lab coroutine harness exists.
  static std::mutex raft_test_mutex_;
  static rusty::Option<rusty::Rc<Fiber>> raft_test_fiber_;
  static uint16_t n_replicas_;
  // @unsafe - borrowed frame registry for RAFT_TEST_CORO; entries are not owned
  // by this map.
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
  // @unsafe - owning communicator handle. Exposed as a raw borrowed
  // Communicator* by CreateCommo() for legacy scheduler/coordinator APIs.
  std::unique_ptr<RaftCommo> commo_;
  // RaftFrame currently owns both RaftCommo and RaftServer so coordinators can
  // borrow the same common Raft state through legacy Frame factory APIs.
  // @unsafe - owning scheduler/server handle. Exposed as a raw borrowed
  // TxLogServer* by CreateScheduler(); callers must not delete it.
  std::unique_ptr<RaftServer> svr_;
  Executor *CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>()) override;
  vector<rrr::ServiceProxy> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<rrr::PollThread> poll_thread_worker) override;
};

} // namespace janus
