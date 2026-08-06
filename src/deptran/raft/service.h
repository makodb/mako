#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "deptran/procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include "server.h"
#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>
#include <rusty/sync/atomic.hpp>

import rusty;

// @external: {
//   verify: [safe, (bool) -> void],
//   clock_gettime: [safe, (int, timespec*) -> int],
//   srand: [safe, (unsigned int) -> void]
// }

class SimpleCommand;
namespace janus {

class TxLogServer;
class RaftServer;

#if RUSTYCPP_RUST
pub struct RaftServiceStateCore {
    svr_: rusty::sync::atomic::AtomicPtr<RaftServer>,
    site_id_: rusty::Cell<u16>,
    poll_thread_: rusty::Option<rusty::Arc<rrr::PollThread>>,
}

impl RaftServiceStateCore {
    // @unsafe - stores a borrowed RaftServer pointer plus shared poll thread.
    fn new(svr: *mut RaftServer,
           site_id: u16,
           poll_thread: rusty::Arc<rrr::PollThread>) -> RaftServiceStateCore {
        RaftServiceStateCore {
            svr_: rusty::sync::atomic::AtomicPtr::<RaftServer>::new_(svr),
            site_id_: rusty::Cell::<u16>::new_(site_id),
            poll_thread_: rusty::Some(poll_thread),
        }
    }

    // @safe - identity read.
    fn site_id(&self) -> u16 {
        self.site_id_.get()
    }

    // @unsafe - returns the currently published borrowed server pointer.
    fn server(&self) -> *mut RaftServer {
        self.svr_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    // @unsafe - publishes a borrowed server pointer for later RPC handlers.
    fn set_server(&mut self, svr: *mut RaftServer) {
        self.svr_.store(svr, rusty::sync::atomic::Ordering::Release)
    }

    // @safe - returns whether a restart poll thread is still available.
    fn has_poll_thread(&self) -> bool {
        self.poll_thread_.is_some()
    }

    // @safe - clones the shared poll thread handle when present.
    fn clone_poll_thread(&self) -> rusty::Option<rusty::Arc<rrr::PollThread>> {
        if self.poll_thread_.is_some() {
            rusty::Some(self.poll_thread_.as_ref().unwrap().clone())
        } else {
            rusty::None
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=service.state_core version=1 rust_sha256=15c5df40cf0c2f4ac0f88a51efc3d5c957ae1bd485daaa2a882929fd5b501e5d*/
struct RaftServiceStateCore;

struct RaftServiceStateCore {
    rusty::sync::atomic::AtomicPtr<RaftServer> svr_;
    rusty::Cell<uint16_t> site_id_;
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;

    static RaftServiceStateCore new_(RaftServer* svr, uint16_t site_id, rusty::Arc<rrr::PollThread> poll_thread);
    uint16_t site_id() const;
    RaftServer* server() const;
    void set_server(RaftServer* svr);
    bool has_poll_thread() const;
    rusty::Option<rusty::Arc<rrr::PollThread>> clone_poll_thread() const;
};


inline RaftServiceStateCore RaftServiceStateCore::new_(RaftServer* svr, uint16_t site_id, rusty::Arc<rrr::PollThread> poll_thread) {
    return RaftServiceStateCore{.svr_ = rusty::sync::atomic::AtomicPtr<RaftServer>::new_(svr), .site_id_ = rusty::Cell<uint16_t>::new_(std::move(site_id)), .poll_thread_ = rusty::Option<rusty::Arc<rrr::PollThread>>(std::move(poll_thread))};
}

inline uint16_t RaftServiceStateCore::site_id() const {
    return this->site_id_.get();
}

inline RaftServer* RaftServiceStateCore::server() const {
    return this->svr_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline void RaftServiceStateCore::set_server(RaftServer* svr) {
    this->svr_.store(svr, rusty::sync::atomic::Ordering::Release);
}

inline bool RaftServiceStateCore::has_poll_thread() const {
    return this->poll_thread_.is_some();
}

inline rusty::Option<rusty::Arc<rrr::PollThread>> RaftServiceStateCore::clone_poll_thread() const {
    if (this->poll_thread_.is_some()) {
        return rusty::Option<rusty::Arc<rrr::PollThread>>(rusty::clone(this->poll_thread_.as_ref().unwrap()));
    } else {
        return rusty::None;
    }
}
/*RUSTYCPP:GEN-END id=service.state_core*/

// @unsafe - RPC service adapter. rrr owns service object lifetime after
// registration; this class only tracks a borrowed/atomic RaftServer pointer.
class RaftServiceImpl : public RaftService {
 public:
  // Static registry to find services by site_id (for Kill/Restart support).
  // @unsafe - stores borrowed service pointers; registry does not own entries.
  // The map and its synchronization live in one Rusty mutex so callers cannot
  // access the registry outside its lock scope.
  static rusty::Mutex<rusty::BTreeMap<siteid_t, RaftServiceImpl*>>
      service_registry_;

  // @unsafe - borrowed server pointer, site id, and restart poll-thread handle
  // live in a DSL core. Static registry and RPC overrides stay as C++ bridge
  // boundaries for now.
  RaftServiceStateCore state_core_;

  RaftServiceImpl(TxLogServer* sched, rusty::Arc<rrr::PollThread> poll_thread);

  // Called by test framework during Kill/Restart to publish the current
  // borrowed server pointer. Passing nullptr marks the service as down.
  static void UpdateServer(siteid_t site_id, RaftServer* new_svr);

  // Called by test framework during Restart to get the original poll thread.
  static rusty::Option<rusty::Arc<rrr::PollThread>> GetPollThread(siteid_t site_id);

  // Called by RPC handlers - lock-free atomic read of the borrowed server.
  RaftServer* GetServer();

  // Generated fiber-RPC overrides. The rrr codegen wraps each one in a
  // Fiber::create_run; we return a packed response struct and the
  // framework sends the reply on fiber completion. No DeferredReply.
  rusty::Result<RpcVoteResponse,                rrr::i32> Vote(const RpcVoteRequest& req) override;
  rusty::Result<RpcVoteDurableResponse,         rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) override;
  rusty::Result<RpcAppendEntriesResponse,       rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) override;
  rusty::Result<RpcEmptyAppendEntriesResponse,  rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) override;
  rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) override;
  rusty::Result<RpcTimeoutNowResponse,          rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) override;
  rusty::Result<RpcNotifyRestartResponse,       rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) override;
  rusty::Result<RpcInstallSnapshotResponse,     rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) override;
  rusty::Result<RpcAddServerResponse,           rrr::i32> AddServer(const RpcAddServerRequest& req) override;
  rusty::Result<RpcRemoveServerResponse,        rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) override;
};

} // namespace janus
