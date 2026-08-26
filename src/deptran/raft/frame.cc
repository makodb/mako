#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "server.h"
#include "service.h"
#include "commo.h"
#include "config.h"
#include "test.h"
#include <rusty/slice.hpp>
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

// RAFT_TEST_CORO scalar thresholds. Test fibers, locks, maps, and reactor
// scheduling remain in the hand-written C++ path.
#if RUSTYCPP_RUST
pub const fn raft_frame_has_single_partition(num_partitions: u32) -> bool {
    num_partitions == 1
}

pub const fn raft_frame_has_expected_partition_size(partition_size: i32) -> bool {
    partition_size == 5
}

pub const fn raft_frame_can_register_lab_scheduler(n_replicas: u16,
                                                   expected_replicas: u16) -> bool {
    n_replicas < expected_replicas
}

pub const fn raft_frame_all_schedulers_created(n_replicas: u16,
                                               expected_replicas: u16) -> bool {
    n_replicas == expected_replicas
}

pub const fn raft_frame_should_create_test_fiber(site_id: u32) -> bool {
    site_id == 0
}

pub const fn raft_frame_more_commos_needed(n_commos: u16,
                                           expected_replicas: u16) -> bool {
    n_commos < expected_replicas
}

pub const fn raft_frame_is_lab_config(replica_protocol: i32,
                                      raft_protocol: i32,
                                      num_partitions: u32,
                                      partition_size: i32,
                                      local_server_count: usize) -> bool {
    replica_protocol == raft_protocol &&
        num_partitions == 1 &&
        partition_size == 5 &&
        local_server_count == 5
}

pub const fn raft_frame_lab_process_exit_code(is_lab_config: bool,
                                              test_result: i32) -> i32 {
    if is_lab_config && test_result != 0 {
        1
    } else {
        0
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_frame.lab_decisions version=1 rust_sha256=7563957a599c4f80f1732b50ab0fe15fb4b26e3360aa0883e659e9467439c5ba*/
constexpr bool raft_frame_has_single_partition(uint32_t num_partitions);
constexpr bool raft_frame_has_expected_partition_size(int32_t partition_size);
constexpr bool raft_frame_can_register_lab_scheduler(uint16_t n_replicas, uint16_t expected_replicas);
constexpr bool raft_frame_all_schedulers_created(uint16_t n_replicas, uint16_t expected_replicas);
constexpr bool raft_frame_should_create_test_fiber(uint32_t site_id);
constexpr bool raft_frame_more_commos_needed(uint16_t n_commos, uint16_t expected_replicas);
constexpr bool raft_frame_is_lab_config(int32_t replica_protocol, int32_t raft_protocol, uint32_t num_partitions, int32_t partition_size, size_t local_server_count);
constexpr int32_t raft_frame_lab_process_exit_code(bool is_lab_config, int32_t test_result);
constexpr bool raft_frame_has_single_partition(uint32_t num_partitions) {
    return rusty::detail::deref_if_pointer_like(num_partitions) == static_cast<uint32_t>(1);
}
constexpr bool raft_frame_has_expected_partition_size(int32_t partition_size) {
    return rusty::detail::deref_if_pointer_like(partition_size) == static_cast<int32_t>(5);
}
constexpr bool raft_frame_can_register_lab_scheduler(uint16_t n_replicas, uint16_t expected_replicas) {
    return rusty::detail::deref_if_pointer_like(n_replicas) < rusty::detail::deref_if_pointer_like(expected_replicas);
}
constexpr bool raft_frame_all_schedulers_created(uint16_t n_replicas, uint16_t expected_replicas) {
    return rusty::detail::deref_if_pointer_like(n_replicas) == rusty::detail::deref_if_pointer_like(expected_replicas);
}
constexpr bool raft_frame_should_create_test_fiber(uint32_t site_id) {
    return rusty::detail::deref_if_pointer_like(site_id) == static_cast<uint32_t>(0);
}
constexpr bool raft_frame_more_commos_needed(uint16_t n_commos, uint16_t expected_replicas) {
    return rusty::detail::deref_if_pointer_like(n_commos) < rusty::detail::deref_if_pointer_like(expected_replicas);
}
constexpr bool raft_frame_is_lab_config(int32_t replica_protocol, int32_t raft_protocol, uint32_t num_partitions, int32_t partition_size, size_t local_server_count) {
    return (((rusty::detail::deref_if_pointer_like(replica_protocol) == rusty::detail::deref_if_pointer_like(raft_protocol)) && (rusty::detail::deref_if_pointer_like(num_partitions) == static_cast<uint32_t>(1))) && (rusty::detail::deref_if_pointer_like(partition_size) == static_cast<int32_t>(5))) && (rusty::detail::deref_if_pointer_like(local_server_count) == static_cast<size_t>(5));
}
constexpr int32_t raft_frame_lab_process_exit_code(bool is_lab_config, int32_t test_result) {
    if (rusty::detail::deref_if_pointer_like(is_lab_config) && (rusty::detail::deref_if_pointer_like(test_result) != static_cast<int32_t>(0))) {
        return static_cast<int32_t>(1);
    } else {
        return static_cast<int32_t>(0);
    }
}
/*RUSTYCPP:GEN-END id=raft_frame.lab_decisions*/

static_assert(raft_frame_has_single_partition(1));
static_assert(!raft_frame_has_single_partition(2));
static_assert(raft_frame_has_expected_partition_size(5));
static_assert(!raft_frame_has_expected_partition_size(4));
static_assert(raft_frame_can_register_lab_scheduler(4, 5));
static_assert(!raft_frame_can_register_lab_scheduler(5, 5));
static_assert(raft_frame_all_schedulers_created(5, 5));
static_assert(raft_frame_should_create_test_fiber(0));
static_assert(!raft_frame_should_create_test_fiber(1));
static_assert(raft_frame_more_commos_needed(4, 5));
static_assert(!raft_frame_more_commos_needed(5, 5));
static_assert(!raft_frame_more_commos_needed(6, 5));
static_assert(raft_frame_is_lab_config(MODE_RAFT, MODE_RAFT, 1, 5, 5));
static_assert(!raft_frame_is_lab_config(MODE_NONE, MODE_RAFT, 1, 5, 5));
static_assert(!raft_frame_is_lab_config(MODE_RAFT, MODE_RAFT, 1, 5, 1));
static_assert(raft_frame_lab_process_exit_code(true, -1) == 1);
static_assert(raft_frame_lab_process_exit_code(true, 1) == 1);
static_assert(raft_frame_lab_process_exit_code(true, 0) == 0);
static_assert(raft_frame_lab_process_exit_code(false, -1) == 0);

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
rusty::sync::atomic::AtomicI32 RaftFrame::lab_test_result_{-1};
uint16_t RaftFrame::n_commo_created_ = 0;
bool RaftFrame::is_lab_test_config_ = false;
bool RaftFrame::lab_test_config_checked_ = false;

// @unsafe - Serializes the shared test-config cache with the legacy test mutex.
bool RaftFrame::IsRaftLabTestConfig() {
  std::lock_guard<std::mutex> lock(raft_test_mutex_);  // @unsafe
  if (!lab_test_config_checked_) {
    auto config = Config::GetConfig();
    if (config != nullptr) {
      // The lab embeds all five Raft replicas in this one process. Topology
      // alone is insufficient: distributed/non-Raft 1x5 configurations must
      // not start the in-process lab fiber or inherit its exit status.
      const size_t local_server_count = config->GetMyServers().size();
      is_lab_test_config_ = raft_frame_is_lab_config(
          config->replica_proto_, MODE_RAFT, config->GetNumPartition(),
          config->GetPartitionSize(0), local_server_count);
      lab_test_config_checked_ = true;
      Log_info("RaftFrame: Lab test config check: protocol={}, partitions={}, "
               "replicas={}, local_servers={}, is_lab_test={}",
               config->replica_proto_, config->GetNumPartition(),
               config->GetPartitionSize(0), local_server_count,
               is_lab_test_config_ ? "true" : "false");
    }
  }
  return is_lab_test_config_;
}

// @safe - Atomic read used by the process entry point after the reactor exits.
int RaftFrame::RaftLabTestResult() {
  return lab_test_result_.load(
      rusty::sync::atomic::Ordering::Acquire);
}

// @safe - Pure DSL exit mapping over an acquire-loaded lab result.
int RaftFrame::RaftLabProcessExitCode() {
  return raft_frame_lab_process_exit_code(
      IsRaftLabTestConfig(), RaftLabTestResult());
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
    verify(raft_frame_can_register_lab_scheduler(n_replicas_, 5));
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
        int test_result = test.Run();
        test.Cleanup();
        lab_test_result_.store(
            test_result, rusty::sync::atomic::Ordering::Release);
        Log_info("Test fiber: Tests completed, turning off reactor loop");
        // Turn off Reactor loop
        Reactor::get_reactor()->looping_.set(false);
        return;
      }));
      Log_info("raft_test_fiber_ id={}",
               raft_test_fiber_.as_ref().unwrap()->id.get());

      // wait until n_commo_created_ == 5, then resume the fiber
      raft_test_mutex_.lock();
      while (raft_frame_more_commos_needed(n_commo_created_, 5)) {
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
