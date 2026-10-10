#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "raft_lane.h"
#include "server.h"
#include "config.h"
#include <rusty/slice.hpp>
// #include "../kv/server.h"


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
uint16_t RaftFrame::n_replicas_ = 0;
map<siteid_t, RaftFrame*> RaftFrame::frames_ = {};
bool RaftFrame::all_sites_created_s = false;
rusty::sync::atomic::AtomicI32 RaftFrame::lab_test_result_{-1};
uint16_t RaftFrame::n_commo_created_ = 0;
bool RaftFrame::is_lab_test_config_ = false;
bool RaftFrame::lab_test_config_checked_ = false;

// The lab's RPC counter, for the Rust fixture (lab.rs::rpc_count): every RPC
// the replica's transport sent. Here rather than with the kernels in server.cc
// because RaftFrame::frames_ is private and this file is a member; through
// the frame because ServerWorker hands it the transport.
// @safe - a map lookup and the transport's counter.
uint64_t RaftFrame::LabFrameRpcCount(uint32_t loc_id) {
  auto it = RaftFrame::frames_.find(static_cast<siteid_t>(loc_id));
  if (it == RaftFrame::frames_.end() || it->second == nullptr ||
      it->second->rust_transport_ == nullptr) {
    return 0;
  }
  return raft_lane::RpcCount(it->second->rust_transport_);
}

extern "C" uint64_t raft_lab_frame_rpc_count(uint32_t loc_id) {
  return RaftFrame::LabFrameRpcCount(loc_id);
}

void RaftFrame::RustLaneLabCommoCreated() {
  if (!IsRaftLabTestConfig()) {
    return;
  }
  std::lock_guard<std::mutex> lock(raft_test_mutex_);
  verify(raft_frame_all_schedulers_created(n_replicas_, 5));
  n_commo_created_++;
}

void RaftFrame::RustLaneLabRunIfSite0(uint32_t locale_id) {
  if (!IsRaftLabTestConfig() || !raft_frame_should_create_test_fiber(locale_id)) {
    return;
  }
  // Wait until all five replicas are connected and started.
  raft_test_mutex_.lock();
  while (raft_frame_more_commos_needed(n_commo_created_, 5)) {
    raft_test_mutex_.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    raft_test_mutex_.lock();
  }
  raft_test_mutex_.unlock();
  const int test_result = raft_lane::RunLab();
  lab_test_result_.store(test_result, rusty::sync::atomic::Ordering::Release);
  Log_info("Lab harness finished with {}", test_result);
}

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
RaftServer *RaftFrame::CreateRaftScheduler() {
  if(svr_ == nullptr)
  {
    // The caller -- the worker -- takes ownership. The frame keeps only a
    // borrowed back-reference for the RAFT_TEST_CORO harness.
    // @unsafe
    { svr_ = new RaftServer(); }
  }
  else
  {
    // @unsafe { Log_error is not borrow-checked }
    Log_error("[RAFT] RaftFrame::CreateScheduler called but scheduler already exists");
    return svr_;
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

  return svr_;
}

// @safe - unreachable: see frame.h.
Communicator *RaftFrame::CreateCommo(rusty::Option<rusty::Arc<srpc::PollThread>>) {
  Log_fatal("RaftFrame::CreateCommo: Raft's communicator is its transport (raft_lane.h)");
  return nullptr;
}

// @safe - unreachable: see frame.h.
std::vector<srpc::ServiceProxy> RaftFrame::CreateRpcServices(uint32_t, TxLogServer*,
                                                             rusty::Arc<srpc::PollThread>) {
  Log_fatal("RaftFrame::CreateRpcServices: Raft's service is its transport's (raft_lane.h)");
  return {};
}

} // namespace janus;
