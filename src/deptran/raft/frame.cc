#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "exec.h"
#include "coordinator.h"
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

#if RUSTYCPP_RUST
pub fn raft_frame_is_lab_test_config(num_partitions: u32,
                                     partition_size: i32) -> bool {
    num_partitions == 1 && partition_size == 5
}

pub fn raft_frame_can_register_lab_scheduler(n_replicas: u16,
                                             expected_replicas: u16) -> bool {
    n_replicas < expected_replicas
}

pub fn raft_frame_all_schedulers_created(n_replicas: u16,
                                         expected_replicas: u16) -> bool {
    n_replicas == expected_replicas
}

pub fn raft_frame_all_commos_created(n_commos: u16,
                                     expected_replicas: u16) -> bool {
    n_commos == expected_replicas
}

pub fn raft_frame_should_create_test_fiber(site_id: u32) -> bool {
    site_id == 0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=frame.1 version=1 rust_sha256=11b8a444910a0d11445cedbc0bb3094f6716f332f83ba63a6a185e0fa2e862f8*/
bool raft_frame_is_lab_test_config(uint32_t num_partitions, int32_t partition_size);
bool raft_frame_can_register_lab_scheduler(uint16_t n_replicas, uint16_t expected_replicas);
bool raft_frame_all_schedulers_created(uint16_t n_replicas, uint16_t expected_replicas);
bool raft_frame_all_commos_created(uint16_t n_commos, uint16_t expected_replicas);
bool raft_frame_should_create_test_fiber(uint32_t site_id);

bool raft_frame_is_lab_test_config(uint32_t num_partitions, int32_t partition_size) {
    return (rusty::detail::deref_if_pointer_like(num_partitions) == static_cast<uint32_t>(1)) && (rusty::detail::deref_if_pointer_like(partition_size) == static_cast<int32_t>(5));
}

bool raft_frame_can_register_lab_scheduler(uint16_t n_replicas, uint16_t expected_replicas) {
    return rusty::detail::deref_if_pointer_like(n_replicas) < rusty::detail::deref_if_pointer_like(expected_replicas);
}

bool raft_frame_all_schedulers_created(uint16_t n_replicas, uint16_t expected_replicas) {
    return rusty::detail::deref_if_pointer_like(n_replicas) == rusty::detail::deref_if_pointer_like(expected_replicas);
}

bool raft_frame_all_commos_created(uint16_t n_commos, uint16_t expected_replicas) {
    return rusty::detail::deref_if_pointer_like(n_commos) == rusty::detail::deref_if_pointer_like(expected_replicas);
}

bool raft_frame_should_create_test_fiber(uint32_t site_id) {
    return rusty::detail::deref_if_pointer_like(site_id) == static_cast<uint32_t>(0);
}
/*RUSTYCPP:GEN-END id=frame.1*/

REG_FRAME(MODE_RAFT, vector<string>({"raft"}), RaftFrame);

Frame* CreateRaftFrameBuiltin(int mode) {
  return new RaftFrame(mode);
}

// @safe
RaftFrame::RaftFrame(int mode) : Frame(mode) {

}

// @safe - owned unique_ptr members release resources after the destructor body.
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
      is_lab_test_config_ = raft_frame_is_lab_test_config(
          config->GetNumPartition(), config->GetPartitionSize(0));
      lab_test_config_checked_ = true;
      Log_info("RaftFrame: Lab test config check: partitions={}, replicas={}, is_lab_test={}",
               config->GetNumPartition(), config->GetPartitionSize(0),
               is_lab_test_config_ ? "true" : "false");
    }
  }
  return is_lab_test_config_;
}
#endif


// @unsafe - legacy Frame factory returns a raw owning pointer via new; the
// inherited caller contract is responsible for deletion.
Executor *RaftFrame::CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) {
  Executor *exec = new RaftExecutor(cmd_id, sched);
  return exec;
}

// @unsafe - legacy Frame factory returns a raw owning pointer via new; the
// inherited caller contract is responsible for deletion.
Coordinator *RaftFrame::CreateCoordinator(cooid_t coo_id,
                                                Config *config,
                                                int benchmark,
                                                rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                                uint32_t id,
                                                shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  CoordinatorRaft *coo;
  coo = new CoordinatorRaft(coo_id,
                                  benchmark,
                                  std::move(client_status),
                                  id);
  coo->frame_ = this;
  verify(commo_ != nullptr);
  coo->commo_ = commo_.get();
  // Share the frame-owned server with this coordinator; RaftFrame keeps the
  // pointee alive for the coordinator lifetime.
  verify(svr_ != nullptr);
  coo->svr_ = this->svr_.get();
  coo->slot_hint_ = slot_hint_;  // Safe: Arc copy shares ownership
  coo->slot_id_ = slot_hint_->get();
  slot_hint_->set(slot_hint_->get() + 1);
  coo->n_replica_ = config->GetPartitionSize(site_info_->partition_id_);
  coo->loc_id_ = this->site_info_->locale_id;
  verify(coo->n_replica_ != 0);
  Log_debug("create new fpga raft coord, coo_id: {}", (int) coo->coo_id_);
  return coo;
}

// @unsafe - returns raw pointer to owned member (caller does not take ownership), calls Log_error/Log_debug
TxLogServer *RaftFrame::CreateScheduler() {
  if(svr_ == nullptr)
  {
    // @unsafe
    { svr_ = std::make_unique<RaftServer>(this); }
  }
  else
  {
    // @unsafe { Log_error is not borrow-checked }
    Log_error("[RAFT] RaftFrame::CreateScheduler called but scheduler already exists");
    return svr_.get();
  }
  // @unsafe
  { Log_debug("create new fpga raft sched loc: {}", this->site_info_->locale_id); }

#ifdef RAFT_TEST_CORO
  // Only run test framework code if in raft lab test configuration
  if (IsRaftLabTestConfig()) {
    raft_test_mutex_.lock();
    verify(raft_frame_can_register_lab_scheduler(n_replicas_, 5));
    frames_[this->site_info_->locale_id] = this;
    n_replicas_++;
    raft_test_mutex_.unlock();
  }
#endif

  return svr_.get();
}

// @unsafe - returns raw pointer to owned member, external calls marked @external [safe]
Communicator *RaftFrame::CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
  // We only have 1 instance of RaftFrame object that is returned from
  // GetFrame method. RaftCommo currently seems ok to share among the
  // clients of this method.
  Log_info("CreateCommo: Thread ID = {}", std::this_thread::get_id());
  {
    auto guard = rrr::sp_running_fiber_th_.borrow();
    Log_info("CreateCommo: sp_running_fiber_th_ = {}", (*guard).is_some() ? (void*)(*guard).as_ref().unwrap().get() : nullptr);
  }
  if (commo_ == nullptr) {
    Log_info("CreateCommo: Creating RaftFrame-owned RaftCommo");
    commo_ = std::make_unique<RaftCommo>(std::move(poll_thread_worker));
  }

  #ifdef RAFT_TEST_CORO
  // Only run test framework code if in raft lab test configuration
  if (IsRaftLabTestConfig()) {
    Log_info("CreateCommo: RAFT_TEST_CORO enabled (lab test mode)");
    raft_test_mutex_.lock();
    Log_info("CreateCommo: n_replicas_ = {}, n_commo_ = {}", n_replicas_, n_commo_created_);

    // Simple verification: ensure all 5 schedulers are created
    verify(raft_frame_all_schedulers_created(n_replicas_, 5));

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
    if (raft_frame_should_create_test_fiber(site_info_->locale_id)) {
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
        verify(raft_frame_all_schedulers_created(n_replicas_, 5));
        auto testconfig = new RaftTestConfig(frames_);
        RaftLabTest test(testconfig);
        test.Run();
        test.Cleanup();
        Log_info("Test fiber: Tests completed, turning off reactor loop");
        // Turn off Reactor loop
        Reactor::get_reactor()->looping_.set(false);
        return;
      }));
      Log_info("raft_test_fiber_ id={}", raft_test_fiber_.as_ref().unwrap()->id);

      // wait until n_commo_created_ == 5, then resume the fiber
      raft_test_mutex_.lock();
      while (!raft_frame_all_commos_created(n_commo_created_, 5)) {
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
vector<rrr::ServiceProxy>
RaftFrame::CreateRpcServices(uint32_t site_id,
                                   TxLogServer *rep_sched,
                                   rusty::Arc<rrr::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<rrr::ServiceProxy>();
  switch (config->replica_proto_) {
    // Fix 2: Pass poll_thread_worker to RaftServiceImpl so it can be
    // retrieved during Restart() to ensure inbound/outbound use same thread
    case MODE_RAFT:result.push_back(rrr::make_service_proxy_from_typed_box(rusty::make_box<RaftServiceImpl>(rep_sched, poll_thread_worker.clone())));
    default:break;
  }
  return result;
}

} // namespace janus;
