#pragma once

#include "__dep__.h"
#include "constants.h"
#include "msg.h"
#include "config.h"
#include "command_marshaler.h"
#include "RW_command.h"
#include "procedure.h"
#include "rcc_rpc.h"
#include <ctime>
#include <unordered_map>
#include <rusty/arc.hpp>

namespace janus {

// SIMULATE_WAN plumbing removed (never enabled — constants.h keeps the
// define commented out); the macros stay as no-ops for the live call sites.
#define WAN_WAIT ;
#define WAN_WAIT_TIME(m) ;

class Coordinator;
class ClassicProxy;
class ClientControlProxy;
class TxLogServer;

typedef std::pair<siteid_t, ClassicProxy*> SiteProxyPair;
typedef std::pair<siteid_t, ClientControlProxy*> ClientSiteProxyPair;

// Construction shim for the inline-Rust DSL `janus::QuorumEventWrapper`
// (src/rrr/reactor/reactor.cpp): a DSL struct emits a `static new_` factory
// rather than a real 2-arg constructor, so the per-protocol quorum events
// derive from this adapter instead of the wrapper directly.
class QuorumEventBase : public QuorumEventWrapper {
 public:
  QuorumEventBase(int n_total, int quorum)
      : QuorumEventWrapper(QuorumEventWrapper::new_(n_total, quorum)) {}
};


class PaxosPrepareQuorumEvent: public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;
//  ballot_t max_ballot_{0};
  bool HasAcceptedValue() {
    // TODO implement this
    return false;
  }
  void FeedResponse(bool y) {
    if (y) {
      q().n_voted_yes_.set(q().n_voted_yes_.get() + 1);
    } else {
      q().n_voted_no_.set(q().n_voted_no_.get() + 1);
    }
    // Self-notification: call test() to push to ready queue when quorum reached
    test();
  }


};

class PaxosAcceptQuorumEvent: public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;
  void FeedResponse(bool y) {
    if (y) {
      q().n_voted_yes_.set(q().n_voted_yes_.get() + 1);
    } else {
      q().n_voted_no_.set(q().n_voted_no_.get() + 1);
    }
    // Self-notification: call test() to push to ready queue when quorum reached
    test();
  }
};

class GetLeaderQuorumEvent : public QuorumEventBase {
 public:
  // Quorum math now lives on QuorumEvent as QuorumPolicy::ALL_NO (S3):
  // no() == every voter said no; is_ready() == yes()||no().
  GetLeaderQuorumEvent(int n_total, int quorum)
      : QuorumEventBase(n_total, quorum) {
    q().policy_.set(QuorumPolicy::ALL_NO);
  }
  void FeedResponse(bool y, locid_t leader_id) {
    if (y) {
      q().leader_id_.set(leader_id);
      vote_yes();
    } else {
      vote_no();
    }
  }
};

class JetpackPullIdSetQuorumEvent: public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;
  std::vector<rusty::Arc<VecRecData>> id_sets_;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, const janus::Command& id_set) {
    if (y) {
      vote_yes();
      // If ok=true, jepoch and oepoch are not larger than local, so we can update id_sets
      auto vec_rec_data = marshallable_cast<VecRecData>(id_set);
      if (vec_rec_data.is_some()) {
        // intentional extraction — last use of the Option local
        id_sets_.push_back(vec_rec_data.unwrap());
      }
    } else {
      vote_no();
      // If ok=false, we need to find max jepoch and oepoch for updating local values
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
    }
  }
  
  shared_ptr<vector<key_t>> GetMergedKeys() {
    auto result = std::make_shared<vector<key_t>>();
    std::set<key_t> unique_keys;
    
    for (const auto& id_set : id_sets_) {
      if (id_set && id_set->key_data_) {
        for (const auto& key : *id_set->key_data_) {
          unique_keys.insert(key);
        }
      }
    }
    
    for (const auto& key : unique_keys) {
      result->push_back(key);
    }
    return result;
  }
};

class JetpackPullCmdQuorumEvent: public QuorumEventBase {
 public:
  JetpackPullCmdQuorumEvent(int n_total, int quorum, const std::vector<key_t>& keys)
      : QuorumEventBase(n_total, quorum), ordered_keys_(keys) {
    key_states_.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); i++) {
      key_index_[keys[i]] = i;
      key_states_.push_back(KeyState{keys[i], {}, 0, janus::Command{}});
    }
    int f = (q().n_total_ - 1) / 2;
    majority_threshold_ = (f + 2 + 1) / 2;
  }

  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, const janus::Command& batch_md) {
    if (y) {
      vote_yes();
      const auto batch = marshallable_cast<KeyCmdBatchData>(batch_md);
      if (batch.is_some()) {
        for (size_t i = 0; i < batch.unwrap()->Size(); i++) {
          auto it = key_index_.find(batch.unwrap()->GetKey(i));
          if (it == key_index_.end()) {
            continue;
          }
          const auto& cmd = batch.unwrap()->GetCommand(i);
          if (!cmd.has_value()) {
            continue;
          }
          auto& state = key_states_[it->second];
          // GetCombinedCmdID still takes shared_ptr<Marshallable>.
          uint64_t cmd_id = SimpleRWCommand::GetCombinedCmdID(cmd);
          int count = ++state.cmd_counts_[cmd_id];
          if (count > state.max_count) {
            state.max_count = count;
            state.max_cmd = cmd;
          }
        }
      }
    } else {
      vote_no();
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
    }
  }

  // return Commands directly
  // (no inner_marshallable() unwrap).  JetpackBroadcastRecordCmd takes
  // the same shape on input.
  std::vector<std::pair<key_t, janus::Command>> GetRecoveredCommands() const {
    std::vector<std::pair<key_t, janus::Command>> result;
    for (const auto& state : key_states_) {
      if (state.max_cmd.has_value() && state.max_count >= majority_threshold_) {
        result.emplace_back(state.key, state.max_cmd);
      }
    }
    return result;
  }

  const std::vector<key_t>& OrderedKeys() const { return ordered_keys_; }

  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;

 private:
  // max_cmd migrated to janus::Command.
  struct KeyState {
    key_t key;
    std::unordered_map<uint64_t, int> cmd_counts_;
    int max_count = 0;
    janus::Command max_cmd{};
  };

  std::vector<key_t> ordered_keys_;
  std::vector<KeyState> key_states_;
  std::unordered_map<key_t, size_t> key_index_;
  int majority_threshold_{0};
};

class JetpackPrepareQuorumEvent: public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  ballot_t max_accepted_ballot_ = -1;
  ballot_t max_seen_ballot_ = -1;
  int accepted_sid_ = -1;
  int accepted_set_size_ = 0;
  bool has_accepted_value_ = false;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, ballot_t accepted_ballot, int sid, int set_size, ballot_t max_seen_ballot) {
    if (y) {
      vote_yes();
      // Track the highest accepted ballot and its value
      if (accepted_ballot > max_accepted_ballot_) {
        max_accepted_ballot_ = accepted_ballot;
        accepted_sid_ = sid;
        accepted_set_size_ = set_size;
        has_accepted_value_ = true;
      }
    } else {
      vote_no();
      // Track max epochs and max_seen_ballot for local update
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
      if (max_seen_ballot > max_seen_ballot_) {
        max_seen_ballot_ = max_seen_ballot;
      }
    }
  }
  
  bool HasValue() {
    return has_accepted_value_;
  }
  
  int GetSid() {
    return accepted_sid_;
  }
  
  int GetSetSize() {
    return accepted_set_size_;
  }
};

class JetpackAcceptQuorumEvent: public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  ballot_t max_seen_ballot_ = -1;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, ballot_t max_seen_ballot) {
    if (y) {
      vote_yes();
    } else {
      vote_no();
      // Track max epochs and max_seen_ballot for local update
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
      if (max_seen_ballot > max_seen_ballot_) {
        max_seen_ballot_ = max_seen_ballot;
      }
    }
  }
};

class JetpackPullRecSetInsQuorumEvent: public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  // recovered_cmd_ migrated
  // from `shared_ptr<Marshallable>` to `janus::Command`.
  // GetRecoveredCmd now returns Command too;
  // shared_ptr<Marshallable> callers auto-convert via implicit ctor.
  janus::Command recovered_cmd_;

  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, const janus::Command& cmd) {
    if (y) {
      vote_yes();
      // Store the recovered command if we get one
      if (!recovered_cmd_.has_value() && cmd.has_value()) {
        recovered_cmd_ = cmd;
      }
    } else {
      vote_no();
      // Track max epochs for local update
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
    }
  }

  const janus::Command& GetRecoveredCmd() const {
    return recovered_cmd_;
  }
};

class Communicator {
 public:
  static uint64_t global_id;
  const int CONNECT_TIMEOUT_MS = 120*1000;
  const int CONNECT_SLEEP_MS = 1000;
  rusty::Option<rusty::Arc<rrr::PollThread>> rpc_poll_;
  bool owns_poll_thread_ = false;  // True if we created the poll thread, false if passed in
  locid_t loc_id_ = -1;
  map<siteid_t, rusty::Arc<rrr::Client>> rpc_clients_{};
  map<siteid_t, ClassicProxy *> rpc_proxies_{};
  map<parid_t, vector<SiteProxyPair>> rpc_par_proxies_{};
  map<parid_t, SiteProxyPair> leader_cache_ = {};
  // removed `lat_util_` and `leader_`
  // fields — `lat_util_` was referenced only in commented-out
  // `Log_info` lines at `classic/coordinator.cc:474, 651`; `leader_`
  // had no readers or writers anywhere in the codebase.
  //
  // excised the dead CPU-utilization /
  // RPC-latency profiling subsystem.  Removed fields:
  //   - `unordered_map<uint64_t, pair<rrr::i64, rrr::i64>> outbound_`
  //     (RPC start-time map; written in `BroadcastDispatch` callback
  //     at `communicator.cc:782` and read only inside the dead
  //     window-tracking blocks at `communicator.cc:1023-1048` and
  //     `communicator.cc:1161-1192`).
  //   - `int index`, `int total`, `int low_util` (the `_` suffix
  //     versions like `total_` are LIVE and stay).
  //   - `rrr::i64 window[200]`, `window_time`, `total_time`,
  //     `window_avg`, `total_avg` (the rolling-window latency
  //     accounting).
  //   - `double cpu = 1.0`, `last_cpu = 1.0`, `tx` (the CPU /
  //     network utilisation snapshot).
  //   - `void ResetProfiles()` member function — reset all of the
  //     above; called only from the dead `if(false && ...)` re-elect
  //     branches in `classic/coordinator.cc`.
  // The matching writes in `communicator.cc` (the start-time
  // record at line 782, the window-tracking blocks in the Commit /
  // Abort callbacks) and the dead `if(false && ...)` re-elect
  // branches in `classic/coordinator.cc:469-497` and
  // `classic/coordinator.cc:644-678` were removed alongside.

  // Global view tracking for all partitions (shared across all communicators)
  static std::map<parid_t, View> partition_views_;
  static std::mutex partition_views_mutex_;
	int outbound = 0;
	int outbounds[100];
	int ob_index = 0;
	int begin_index = 0;
	bool paused = false;
	bool slow = false;
	int total_;
	// @unsafe { placeholder event so the const-view Arc handle is always
	//   non-null (Arc has no null state); only read on the dead `paused`
	//   debug path in client_worker.cc / classic/coordinator.cc, both of
	//   which deref it via `->`. Was a default-null shared_ptr before. }
	rusty::Arc<QuorumEvent> qe{create_sp_quorum_event(1, 1)};
  vector<ClientSiteProxyPair> client_leaders_;
  std::atomic_bool client_leaders_connected_;
  std::vector<std::thread> threads;
  bool broadcasting_to_leaders_only_{true};
  bool follower_forwarding{false};
  // removed `std::mutex lock_;`,
  // `std::condition_variable cv_;`, and `bool waiting = false;`
  // — neither field was accessed via `commo()->`/`commo_->` from
  // any caller, nor by any internal `Communicator` member function.
  // The companion `count_lock_` IS live (used in
  // `classic/coordinator.cc:118-121` and `client_worker.cc:306-309`)
  // and stays.
	std::mutex count_lock_;

  // Callback function type for getting dynamic leader
  using LeaderCallback = std::function<locid_t(parid_t)>;
  LeaderCallback leader_callback_ = nullptr;

  Communicator(rusty::Option<rusty::Arc<PollThread>> rpc_poll = rusty::None);
  virtual ~Communicator();
  
  void SetLeaderCallback(LeaderCallback callback) {
    leader_callback_ = callback;
  }

  SiteProxyPair RandomProxyForPartition(parid_t partition_id) const;
  SiteProxyPair LeaderProxyForPartition(parid_t) const;

  SiteProxyPair NearestProxyForPartition(parid_t) const;
  void SetLeaderCache(parid_t par_id, SiteProxyPair& proxy) {
    leader_cache_[par_id] = proxy;
  }
  locid_t GenerateNewLeaderId(parid_t par_id) {
    return leader_cache_[par_id].first = leader_cache_[par_id].first + 1;
  };
  
  // View management methods (static for global access)
  // @unsafe
  static void UpdatePartitionView(parid_t partition_id, const ViewData& view_data);
  static View GetPartitionView(parid_t partition_id);
  static locid_t GetLeaderForPartition(parid_t partition_id);
  std::pair<int, ClassicProxy*> ConnectToSite(Config::SiteInfo &site,
                                              std::chrono::milliseconds timeout_ms);
  ClientSiteProxyPair ConnectToClientSite(Config::SiteInfo &site,
                                          std::chrono::milliseconds timeout);

  /**
   * Ensure the client connection to a site is healthy.
   * If the connection is in FAILED or DISCONNECTED state, attempts reconnection.
   *
   * @param site_id The site ID to check
   * @return true if connection is now available, false otherwise
   */
  bool EnsureClientConnected(siteid_t site_id);

  bool ReconnectToSite(siteid_t site_id, parid_t par_id);
  void Pause();
  void Resume();
  void ConnectClientLeaders();
  void WaitConnectClientLeaders();

  vector<function<bool(const string& arg, string& ret)> >
      msg_string_handlers_{};
  vector<function<bool(const janus::Command& arg,
                       janus::Command& ret)> > msg_marshall_handlers_{};

  void SendStart(SimpleCommand& cmd,
                 int32_t output_size,
                 std::function<void(rusty::Arc<Future> fu)> &callback);
  virtual void BroadcastDispatch(shared_ptr<vector<shared_ptr<SimpleCommand>>> vec_piece_data,
                         const std::function<void(int res, TxnOutput &)> &) ;
  // removed `SyncBroadcastDispatch(...)`
  // declaration — only call site was the now-deleted
  // `CoordinatorClassic::DispatchSync`.

	rusty::Arc<QuorumEvent> SendReelect();

  rusty::Arc<WaitAll> SendPrepare(Coordinator* coo,
                                         txnid_t tid,
                                         std::vector<int32_t>& sids);
  rusty::Arc<WaitAll> SendCommit(Coordinator* coo,
                                     txnid_t tid);
  rusty::Arc<WaitAll> SendAbort(Coordinator* coo,
                                    txnid_t tid);
  /*void SendPrepare(parid_t gid,
                   txnid_t tid,
                   std::vector<int32_t> &sids,
                   const std::function<void(int)> &callback) ;*/
  /*void SendCommit(parid_t pid,
                  txnid_t tid,
                  rusty::Function<void()> callback) ;
  void SendAbort(parid_t pid,
                 txnid_t tid,
                 rusty::Function<void()> callback) ;*/
  void SendEarlyAbort(parid_t pid,
                      txnid_t tid) ;

  // for debug
  std::set<std::pair<parid_t, txnid_t>> phase_three_sent_;

  void ___LogSent(parid_t pid, txnid_t tid);

  void SendUpgradeEpoch(epoch_t curr_epoch,
                        const function<void(parid_t,
                                            siteid_t,
                                            int32_t& graph)>& callback);

  void SendTruncateEpoch(epoch_t old_epoch);
  void SendForwardTxnRequest(TxRequest& req, Coordinator* coo, std::function<void(const TxReply&)> callback);

  /**
   *
   * @param shard_id 0 means broadcast to all shards.
   * @param svr_id 0 means broadcast to all replicas in that shard.
   * @param msg
   */

  void AddMessageHandler(std::function<bool(const string&, string&)>);
  void AddMessageHandler(std::function<bool(const janus::Command&,
                                            janus::Command&)>);

  // removed `BroadcastBulkPrepare`,
  // `BroadcastHeartBeat`, `BroadcastSyncNoOps` virtual stubs — the
  // matching `MultiPaxosCommo` overrides + `PaxosWorker::Send*`
  // senders were deleted in Phases 4e-25/4e-26.

    // take janus::Command;
    // shared_ptr<Marshallable> callers auto-convert.
    virtual void ForwardToLearner(parid_t par_id,
                                  uint64_t slot,
                                  ballot_t ballot,
                                  const janus::Command& cmd,
                                  const std::function<void(uint64_t, ballot_t)>& cb) {
      verify(0);
    }

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncLog(parid_t par_id,
                      const janus::Command& cmd,
                      const std::function<void(shared_ptr<janus::Command>, ballot_t, int)>& cb){
      verify(0);
    }

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncCommit(parid_t par_id,
                      const janus::Command& cmd,
                      const std::function<void(ballot_t, int)>& cb){
      verify(0);
    }
  shared_ptr<GetLeaderQuorumEvent> BroadcastGetLeader(parid_t par_id, locid_t cur_pause);
  rusty::Arc<QuorumEvent> FailoverPauseSocketOut(parid_t par_id, locid_t loc_id);
  rusty::Arc<QuorumEvent> FailoverResumeSocketOut(parid_t par_id, locid_t loc_id);
  void SetNewLeaderProxy(parid_t par_id, locid_t loc_id);
  void SendSimpleCmd(groupid_t gid, SimpleCommand& cmd, std::vector<int32_t>& sids,
      const function<void(int)>& callback);
  
  /* Jetpack recovery begin */
  rusty::Arc<QuorumEvent> JetpackBroadcastBeginRecovery(parid_t par_id, locid_t loc_id,
                                                       const View& old_view, 
                                                       const View& new_view, 
                                                       epoch_t new_view_id);
  shared_ptr<JetpackPullIdSetQuorumEvent> JetpackBroadcastPullIdSet(parid_t par_id, locid_t loc_id,
                                                                   epoch_t jepoch, epoch_t oepoch);
  shared_ptr<JetpackPullCmdQuorumEvent> JetpackBroadcastPullCmd(parid_t par_id, locid_t loc_id, 
                                                               const std::vector<key_t>& keys, epoch_t jepoch, epoch_t oepoch);
  // take Commands directly
  // (was vector<pair<key_t, shared_ptr<Marshallable>>>).  Callers
  // produce these from GetRecoveredCommands.
  rusty::Arc<QuorumEvent> JetpackBroadcastRecordCmd(parid_t par_id, locid_t loc_id,
                                                    epoch_t jepoch, epoch_t oepoch,
                                                    int sid, int rid,
                                                    const std::vector<std::pair<key_t, janus::Command>>& cmds);
  shared_ptr<JetpackPrepareQuorumEvent> JetpackBroadcastPrepare(parid_t par_id, locid_t loc_id, 
                                                               epoch_t jepoch, epoch_t oepoch, 
                                                               ballot_t max_seen_ballot);
  shared_ptr<JetpackAcceptQuorumEvent> JetpackBroadcastAccept(parid_t par_id, locid_t loc_id, 
                                                            epoch_t jepoch, epoch_t oepoch, 
                                                            ballot_t max_seen_ballot, int sid, int set_size);
  rusty::Arc<QuorumEvent> JetpackBroadcastCommit(parid_t par_id, locid_t loc_id,
                                                 epoch_t jepoch, epoch_t oepoch, 
                                                 int sid, int set_size);
  shared_ptr<JetpackPullRecSetInsQuorumEvent> JetpackBroadcastPullRecSetIns(parid_t par_id, locid_t loc_id, 
                                                                           epoch_t jepoch, epoch_t oepoch, 
                                                                           int sid, int rid);
  rusty::Arc<QuorumEvent> JetpackBroadcastFinishRecovery(parid_t par_id, locid_t loc_id, epoch_t oepoch);
  /* Jetpack recovery end */
};

} // namespace janus
