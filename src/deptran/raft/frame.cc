#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "server.h"
#include "service.h"
#include "commo.h"
#include "config.h"
#include "test.h"
// #include "../kv/server.h"

// rusty::Rc lives in the rusty module only (the legacy <rusty/rc.hpp>
// header was retired). Pull it in here so the file-scope
// raft_test_fiber_ below can name rusty::Option<rusty::Rc<Fiber>>.
// Per libc++'s textual-then-module ordering, imports follow #includes.
import rusty;

// @external: {
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   std::make_unique: [safe, (...) -> owned]
//   std::make_shared: [safe, (...) -> owned]
//   Config::GetConfig: [safe, () -> *]
//   Reactor::get_reactor: [safe, () -> *]
//   rusty::make_box: [safe, (...) -> owned]
// }

namespace janus {

// @safe - Properly cleans up owned resources via Option<Box<T>>
RaftFrame::~RaftFrame() {
}

#ifdef RAFT_TEST_CORO
std::mutex RaftFrame::raft_test_mutex_;
// File-scope static (used to be RaftFrame::raft_test_fiber_; demoted
// because rusty::Rc is module-only and frame.h can't reach it). All
// references below resolve via namespace lookup once the class member
// is gone.
static rusty::Option<rusty::Rc<Fiber>> raft_test_fiber_;
uint16_t RaftFrame::n_replicas_ = 0;
map<siteid_t, RaftFrame*> RaftFrame::frames_ = {};
bool RaftFrame::all_sites_created_s = false;
bool RaftFrame::tests_done_ = false;
uint16_t RaftFrame::n_commo_created_ = 0;
bool RaftFrame::is_lab_test_config_ = false;
bool RaftFrame::lab_test_config_checked_ = false;

// @safe - Check if running in raft lab test configuration (1 partition, 5 replicas)
bool RaftFrame::IsRaftLabTestConfig() {
  if (!lab_test_config_checked_) {
    auto config = Config::GetConfig();
    if (config != nullptr) {
      // Raft lab test configuration: 1 partition with exactly 5 replicas
      is_lab_test_config_ = (config->GetNumPartition() == 1 &&
                              config->GetPartitionSize(0) == 5);
      lab_test_config_checked_ = true;
      Log_info("RaftFrame: Lab test config check: partitions={}, replicas={}, is_lab_test={}",
               config->GetNumPartition(), config->GetPartitionSize(0),
               is_lab_test_config_ ? "true" : "false");
    }
  }
  return is_lab_test_config_;
}
#endif


// @unsafe - returns raw pointer to owned member (caller does not take ownership), calls Log_error/Log_debug
TxLogServer *RaftFrame::CreateScheduler() {
  if(svr_ == nullptr)
  {
    // @unsafe
    { svr_ = std::make_unique<RaftServer>(); }
  }
  else
  {
    // @unsafe { Log_error is not borrow-checked }
    Log_error("[RAFT] RaftFrame::CreateScheduler called but scheduler already exists");
    return svr_.get();
  }
  // @unsafe
  { Log_debug("create new raft sched loc: {}", this->site_info_->locale_id); }

#ifdef RAFT_TEST_CORO
  // Only run test framework code if in raft lab test configuration
  if (IsRaftLabTestConfig()) {
    raft_test_mutex_.lock();
    verify(n_replicas_ < 5);
    frames_[this->site_info_->locale_id] = this;
    n_replicas_++;
    raft_test_mutex_.unlock();
  }
#endif

  return svr_.get();
}

// @unsafe - returns raw pointer to owned member, external calls marked @external [safe]
Communicator *RaftFrame::CreateCommo(
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker) {
  // We only have 1 instance of RaftFrame object that is returned from
  // GetFrame method. RaftCommo currently seems ok to share among the
  // clients of this method.
  Log_info("CreateCommo: Thread ID = {}", std::this_thread::get_id());
  {
    auto guard = rrr::sp_running_fiber_th_.borrow();
    Log_info("CreateCommo: sp_running_fiber_th_ = {}", (*guard).is_some() ? (void*)(*guard).as_ref().unwrap().get() : nullptr);
  }
  if (commo_ == nullptr) {
    Log_info("CreateCommo: Creating new RaftCommo");
    commo_ = std::make_unique<RaftCommo>(std::move(poll_thread_worker));
  }

  #ifdef RAFT_TEST_CORO
  // Only run test framework code if in raft lab test configuration
  if (IsRaftLabTestConfig()) {
    Log_info("CreateCommo: RAFT_TEST_CORO enabled (lab test mode)");
    raft_test_mutex_.lock();
    Log_info("CreateCommo: n_replicas_ = {}, n_commo_ = {}", n_replicas_, n_commo_created_);

    // Simple verification: ensure all 5 schedulers are created
    verify(n_replicas_ == 5);

    // Simple counter increment: track communicator creation
    // Find this frame in the map and increment counter
    bool found = false;
    for (const auto& pair : frames_) {
      if (pair.second == this) {
        found = true;
        break;
      }
    }
    verify(found); // This frame should exist in frames_

    // Use a simple counter approach like lab solution
    n_commo_created_++;
    Log_info("CreateCommo: n_commo_ now = {}", n_commo_created_);
    raft_test_mutex_.unlock();

    // Only site 0 creates and manages the test fiber
    if (site_info_->locale_id == 0) {
      Log_info("CreateCommo: About to create test fiber");
      verify(raft_test_fiber_.is_none());
      Log_info("Creating Raft test fiber");

      raft_test_fiber_ = rusty::Some(Fiber::create_run([this] () {
        Log_info("Test fiber: Starting execution");
        Log_info("Test fiber: Thread ID = {}", std::this_thread::get_id());
        {
          auto guard = rrr::sp_running_fiber_th_.borrow();
          Log_info("Test fiber: sp_running_fiber_th_ = {}", (*guard).is_some() ? (void*)(*guard).as_ref().unwrap().get() : nullptr);
        }

        // Yield until all 5 communicators are initialized
        Log_info("Test fiber: About to yield");
        auto current_fiber = Fiber::current_fiber();
        if (current_fiber.is_some()) {
          current_fiber.unwrap()->yield_();
        }
        Log_info("Test fiber: Resumed after yield");

        // Run tests
        verify(n_replicas_ == 5);
        auto testconfig = new RaftTestConfig(frames_);
        RaftLabTest test(testconfig);
        test.Run();
        test.Cleanup();
        Log_info("Test fiber: Tests completed, turning off reactor loop");
        // Turn off Reactor loop
        Reactor::get_reactor()->looping_.set(false);
        return;
      }));
      Log_info("raft_test_fiber_ id={}",
               raft_test_fiber_.as_ref().unwrap()->id.get());

      // wait until n_commo_created_ == 5, then resume the fiber
      raft_test_mutex_.lock();
      while (n_commo_created_ < 5) {
        raft_test_mutex_.unlock();
        sleep(0.1);
        raft_test_mutex_.lock();
      }
      raft_test_mutex_.unlock();
      Reactor::get_reactor()->continue_fiber(raft_test_fiber_.as_ref().unwrap().clone());
    }
  }
  #endif

  Log_info("CreateCommo: Returning commo_ = {}", (void*)commo_.get());
  return commo_.get();
}

// @unsafe - external calls marked @external [safe]
std::vector<rrr::ServiceProxy>
RaftFrame::CreateRpcServices(uint32_t site_id,
                                   TxLogServer *rep_sched,
                                   rusty::Arc<rrr::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<rrr::ServiceProxy>();
  switch (config->replica_proto_) {
    // Fix 2: Pass poll_thread_worker to RaftServiceImpl so it can be
    // retrieved during Restart() to ensure inbound/outbound use same thread
    case MODE_RAFT: {
      auto* server = dynamic_cast<RaftServer*>(rep_sched);
      verify(server != nullptr);
      result.push_back(rrr::make_service_proxy_from_typed_box(
          rusty::make_box<RaftServiceImpl>(server, poll_thread_worker.clone())));
      break;
    }
    default:break;
  }
  return result;
}

} // namespace janus;
