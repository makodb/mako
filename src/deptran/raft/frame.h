#pragma once

#include <memory>
#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "server.h"
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/atomic.hpp>

namespace janus {

struct RaftTransport;  // raft-rt


// @unsafe - inherits from non-@interface Frame (individual methods are @safe)
class RaftFrame : public Frame {
 private:
#ifdef RAFT_TEST_CORO
  static std::mutex raft_test_mutex_;
  static uint16_t n_replicas_;
  static map<siteid_t, RaftFrame*> frames_;
  static bool all_sites_created_s;
  // -1 until the lab fiber finishes, then the lab harness's status
  // (raft_lab_rust_run, src/deptran/raft/shell/lab.rs).
  static rusty::sync::atomic::AtomicI32 lab_test_result_;
  static uint16_t n_commo_created_;
  static bool is_lab_test_config_;        // True if running raft lab test (1 partition, 5 replicas)
  static bool lab_test_config_checked_;   // True once we've checked the config
#endif
 public:
#ifdef RAFT_TEST_CORO
  // @unsafe - Uses the legacy test mutex and global Config singleton.
  static bool IsRaftLabTestConfig();
  // @safe - Atomic result read; -1=incomplete, 0=passed, >0=failed.
  static int RaftLabTestResult();
  // @safe - Returns 1 only for an incomplete/failed in-process RaftLab run.
  static int RaftLabProcessExitCode();
  // The RPCs one replica's transport sent, for the lab fixture
  // (src/deptran/raft/shell/lab.rs). A member because `frames_` is private and
  // the extern "C" kernel that calls this cannot be one.
  // @safe - a map lookup and the transport's counter.
  static uint64_t LabFrameRpcCount(uint32_t loc_id);
  // The lab's bookkeeping: CommoCreated counts one replica's transport as
  // connected; RunIfSite0 waits for all five, runs the harness on this
  // thread's reactor (raft_lane::RunLab) and records its verdict where
  // RaftLabTestResult reads it.
  static void RustLaneLabCommoCreated();
  static void RustLaneLabRunIfSite0(uint32_t locale_id);
#endif
  // This replica's raft-rt transport, where the lab's RPC count lives
  // (LabFrameRpcCount). Borrowed; the worker owns it.
  RaftTransport* rust_transport_ = nullptr;
  RaftFrame() = default;
  ~RaftFrame();
  /* TODO: have another class for common data */
  // NON-OWNING. CreateScheduler() hands the only owning reference to the
  // worker, which deletes it (raft_worker.cc, server_worker.cc); this member
  // is the borrowed back-reference the RAFT_TEST_CORO harness reaches through.
  //
  // It must not be a unique_ptr: that would make the frame a SECOND owner of a
  // pointer the worker already deletes -- a double free that stayed latent
  // only because no Frame is ever deleted, i.e. the leak was load-bearing.
  // This matches MultiPaxosFrame, which has never had a frame-side owner
  // (paxos/frame.cc:30).
  // @unsafe - borrowed raw pointer; ownership is the worker's.
  RaftServer* svr_ = nullptr;

  // Called by the owning worker immediately BEFORE it deletes the scheduler,
  // so the borrowed back-reference above cannot outlive the object. Without
  // it the RAFT_TEST_CORO harness's `!frame->svr_` guards would read a stale
  // non-null pointer and dereference freed memory.
  // @safe - drops a borrow, owns nothing.
  void ReleaseScheduler() { svr_ = nullptr; }
  // Frame's generic factory, and the typed one beneath it. RaftWorker and the
  // RAFT_TEST_CORO ServerWorker call CreateRaftScheduler() so they hold the
  // server as what it is, instead of recovering it with dynamic_cast.
  TxLogServer *CreateScheduler() override { return CreateRaftScheduler(); }
  RaftServer *CreateRaftScheduler();
  // Frame's interface, which Paxos implements: Raft's communicator and RPC
  // service are its transport's (raft_lane.h), so the workers never ask the
  // frame for them, and these refuse.
  Communicator *CreateCommo(
      rusty::Option<rusty::Arc<srpc::PollThread>> poll_thread_worker =
          rusty::None) override;
  std::vector<srpc::ServiceProxy> CreateRpcServices(
      uint32_t site_id,
      TxLogServer *rep_sched,
      rusty::Arc<srpc::PollThread> poll_thread_worker) override;
};

} // namespace janus
