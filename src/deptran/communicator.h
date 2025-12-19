#pragma once

#include "__dep__.h"
#include "constants.h"
#include "msg.h"
#include "config.h"
#include "command_marshaler.h"
#include "procedure.h"
#include "deptran/rcc/dep_graph.h"
#include "rcc_rpc.h"
#include <ctime>
#include <unordered_map>
#include <rusty/arc.hpp>

namespace janus {

static void _wan_wait() {
  int num = 50;
  Reactor::CreateSpEvent<NeverEvent>()->Wait(num*1000);
}

static void _wan_wait_time(int m) {
  this_thread::sleep_for(chrono::milliseconds(m));
}


#ifdef SIMULATE_WAN

#define WAN_WAIT _wan_wait();

#define WAN_WAIT_TIME(m) _wan_wait_time(m);

#else

#define WAN_WAIT ;
#define WAN_WAIT_TIME ;

#endif

class Coordinator;
class ClassicProxy;
class ClientControlProxy;
class TxLogServer;

typedef std::pair<siteid_t, ClassicProxy*> SiteProxyPair;
typedef std::pair<siteid_t, ClientControlProxy*> ClientSiteProxyPair;

class MessageEvent : public IntEvent {
 public:
  shardid_t shard_id_;
  svrid_t svr_id_;
  string msg_;
  MessageEvent(svrid_t svr_id) : IntEvent(), svr_id_(svr_id) {

  }

  MessageEvent(shardid_t shard_id, svrid_t svr_id)
      : IntEvent(), shard_id_(shard_id), svr_id_(svr_id) {

  }
};

class PaxosPrepareQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
//  ballot_t max_ballot_{0};
  bool HasAcceptedValue() {
    // TODO implement this
    return false;
  }
  void FeedResponse(bool y) {
    if (y) {
      n_voted_yes_++;
    } else {
      n_voted_no_++;
    }
    // Self-notification: call Test() to push to ready queue when quorum reached
    Test();
  }


};

class PaxosAcceptQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  void FeedResponse(bool y) {
    if (y) {
      n_voted_yes_++;
    } else {
      n_voted_no_++;
    }
    // Self-notification: call Test() to push to ready queue when quorum reached
    Test();
  }
};

class GetLeaderQuorumEvent : public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  void FeedResponse(bool y, locid_t leader_id) {
    if (y) {
      leader_id_ = leader_id;
      VoteYes();
    } else {
      VoteNo();
    }
  }

  bool No() override { return n_voted_no_ == n_total_; }

  bool IsReady() override {
    if (Yes()) {
      return true;
    } else if (No()) {
      return true;
    }

    return false;
  }
};

/************************RULE begin*********************************/

class RuleSpeculativeExecuteQuorumEvent: public QuorumEvent {
  bool has_result_ = false;
  value_t result_;
  int num_leader_{0};
  int n_leader_yes_{0};
  int n_leader_no_{0};
 public:
  RuleSpeculativeExecuteQuorumEvent(int n_total, int quorum, int num_leader)
    : QuorumEvent(n_total, quorum) {
      num_leader_ = num_leader;
  }
  void FeedResponse(bool y, value_t result, bool is_leader);
  bool Yes() override;
  bool No() override;
  value_t GetResult();
};

class JetpackPullIdSetQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  std::vector<shared_ptr<VecRecData>> id_sets_;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, const MarshallDeputy& id_set) {
    if (y) {
      VoteYes();
      // If ok=true, jepoch and oepoch are not larger than local, so we can update id_sets
      auto vec_rec_data = std::dynamic_pointer_cast<VecRecData>(id_set.sp_data_);
      if (vec_rec_data) {
        id_sets_.push_back(vec_rec_data);
      }
    } else {
      VoteNo();
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

class JetpackPullCmdQuorumEvent: public QuorumEvent {
 public:
  JetpackPullCmdQuorumEvent(int n_total, int quorum, const std::vector<key_t>& keys)
      : QuorumEvent(n_total, quorum), ordered_keys_(keys) {
    key_states_.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); i++) {
      key_index_[keys[i]] = i;
      key_states_.push_back(KeyState{keys[i], {}, 0, nullptr});
    }
    int f = (n_total_ - 1) / 2;
    majority_threshold_ = (f + 2 + 1) / 2;
  }

  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, const MarshallDeputy& batch_md) {
    if (y) {
      VoteYes();
      auto batch = std::dynamic_pointer_cast<KeyCmdBatchData>(batch_md.sp_data_);
      if (batch) {
        for (size_t i = 0; i < batch->Size(); i++) {
          auto it = key_index_.find(batch->GetKey(i));
          if (it == key_index_.end()) {
            continue;
          }
          auto cmd = batch->GetCommand(i);
          if (!cmd) {
            continue;
          }
          auto& state = key_states_[it->second];
          uint64_t cmd_id = SimpleRWCommand::GetCombinedCmdID(cmd);
          int count = ++state.cmd_counts_[cmd_id];
          if (count > state.max_count) {
            state.max_count = count;
            state.max_cmd = cmd;
          }
        }
      }
    } else {
      VoteNo();
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
    }
  }

  std::vector<std::pair<key_t, shared_ptr<Marshallable>>> GetRecoveredCommands() const {
    std::vector<std::pair<key_t, shared_ptr<Marshallable>>> result;
    for (const auto& state : key_states_) {
      if (state.max_cmd && state.max_count >= majority_threshold_) {
        result.emplace_back(state.key, state.max_cmd);
      }
    }
    return result;
  }

  const std::vector<key_t>& OrderedKeys() const { return ordered_keys_; }

  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;

 private:
  struct KeyState {
    key_t key;
    std::unordered_map<uint64_t, int> cmd_counts_;
    int max_count = 0;
    shared_ptr<Marshallable> max_cmd = nullptr;
  };

  std::vector<key_t> ordered_keys_;
  std::vector<KeyState> key_states_;
  std::unordered_map<key_t, size_t> key_index_;
  int majority_threshold_{0};
};

class JetpackPrepareQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  ballot_t max_accepted_ballot_ = -1;
  ballot_t max_seen_ballot_ = -1;
  int accepted_sid_ = -1;
  int accepted_set_size_ = 0;
  bool has_accepted_value_ = false;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, ballot_t accepted_ballot, int sid, int set_size, ballot_t max_seen_ballot) {
    if (y) {
      VoteYes();
      // Track the highest accepted ballot and its value
      if (accepted_ballot > max_accepted_ballot_) {
        max_accepted_ballot_ = accepted_ballot;
        accepted_sid_ = sid;
        accepted_set_size_ = set_size;
        has_accepted_value_ = true;
      }
    } else {
      VoteNo();
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

class JetpackAcceptQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  ballot_t max_seen_ballot_ = -1;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, ballot_t max_seen_ballot) {
    if (y) {
      VoteYes();
    } else {
      VoteNo();
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

class JetpackPullRecSetInsQuorumEvent: public QuorumEvent {
 public:
  using QuorumEvent::QuorumEvent;
  epoch_t max_jepoch_ = -1;
  epoch_t max_oepoch_ = -1;
  shared_ptr<Marshallable> recovered_cmd_;
  
  void FeedResponse(bool y, epoch_t jepoch, epoch_t oepoch, const MarshallDeputy& cmd) {
    if (y) {
      VoteYes();
      // Store the recovered command if we get one
      if (!recovered_cmd_ && cmd.sp_data_) {
        recovered_cmd_ = cmd.sp_data_;
      }
    } else {
      VoteNo();
      // Track max epochs for local update
      if (jepoch > max_jepoch_) {
        max_jepoch_ = jepoch;
      }
      if (oepoch > max_oepoch_) {
        max_oepoch_ = oepoch;
      }
    }
  }
  
  shared_ptr<Marshallable> GetRecoveredCmd() {
    return recovered_cmd_;
  }
};

/************************RULE end*********************************/

class Communicator {
 public:
  static uint64_t global_id;
  const int CONNECT_TIMEOUT_MS = 120*1000;
  const int CONNECT_SLEEP_MS = 1000;
  rusty::Option<rusty::Arc<rrr::PollThread>> rpc_poll_;
  TxLogServer *rep_sched_ = nullptr;  // Jetpack protocols (Copilot) need direct scheduler access
  locid_t loc_id_ = -1;
  map<siteid_t, rusty::Arc<rrr::Client>> rpc_clients_{};
  map<siteid_t, ClassicProxy *> rpc_proxies_{};
  map<parid_t, vector<SiteProxyPair>> rpc_par_proxies_{};
  map<parid_t, SiteProxyPair> leader_cache_ = {};
  unordered_map<uint64_t, pair<rrr::i64, rrr::i64>> outbound_{};
	map<uint64_t, double> lat_util_{};
  locid_t leader_ = 0;
  
  // Global view tracking for all partitions (shared across all communicators)
  static std::map<parid_t, View> partition_views_;
  static std::mutex partition_views_mutex_;
	int outbound = 0;
	int outbounds[100];
	int ob_index = 0;
	int begin_index = 0;
	bool paused = false;
	bool slow = false;
	int index;
	int cpu_index;
	int low_util;
  int total;
	int total_;
	shared_ptr<QuorumEvent> qe;
  rrr::i64 window[200];
  rrr::i64 window_time;
  rrr::i64 total_time;
	rrr::i64 window_avg;
	rrr::i64 total_avg;
	double cpu_stor[10];
	double cpu_total;
	double cpu = 1.0;
	double last_cpu = 1.0;
	double tx;
  vector<ClientSiteProxyPair> client_leaders_;
  std::atomic_bool client_leaders_connected_;
  std::vector<std::thread> threads;
  bool broadcasting_to_leaders_only_{true};
  bool follower_forwarding{false};
	std::mutex lock_;
	std::mutex count_lock_;
	std::condition_variable cv_;
	bool waiting = false;
  
  // Callback function type for getting dynamic leader
  using LeaderCallback = std::function<locid_t(parid_t)>;
  LeaderCallback leader_callback_ = nullptr;

  Communicator(rusty::Option<rusty::Arc<PollThread>> rpc_poll = rusty::None);
  virtual ~Communicator();
  
  void SetLeaderCallback(LeaderCallback callback) {
    leader_callback_ = callback;
  }

  SiteProxyPair RandomProxyForPartition(parid_t partition_id) const;
  SiteProxyPair LeaderProxyForPartition(parid_t, int idx=-1) const;

  SiteProxyPair NearestProxyForPartition(parid_t) const;
  void SetLeaderCache(parid_t par_id, SiteProxyPair& proxy) {
    leader_cache_[par_id] = proxy;
  }
  virtual SiteProxyPair DispatchProxyForPartition(parid_t par_id) const {
    return LeaderProxyForPartition(par_id);
  };
  locid_t GenerateNewLeaderId(parid_t par_id) {
    return leader_cache_[par_id].first = leader_cache_[par_id].first + 1;
  };
  
  // View management methods (static for global access)
  // @unsafe
  static void UpdatePartitionView(parid_t partition_id, const std::shared_ptr<ViewData>& view_data);
  static View GetPartitionView(parid_t partition_id);
  static locid_t GetLeaderForPartition(parid_t partition_id);
  std::pair<int, ClassicProxy*> ConnectToSite(Config::SiteInfo &site,
                                              std::chrono::milliseconds timeout_ms);
  ClientSiteProxyPair ConnectToClientSite(Config::SiteInfo &site,
                                          std::chrono::milliseconds timeout);
  void Pause();
  void Resume();
  void ConnectClientLeaders();
  void WaitConnectClientLeaders();

  vector<function<bool(const string& arg, string& ret)> >
      msg_string_handlers_{};
  vector<function<bool(const MarshallDeputy& arg,
                       MarshallDeputy& ret)> > msg_marshall_handlers_{};

	void ResetProfiles();
  void SendStart(SimpleCommand& cmd,
                 int32_t output_size,
                 std::function<void(rusty::Arc<Future> fu)> &callback);
  virtual void BroadcastDispatch(shared_ptr<vector<shared_ptr<SimpleCommand>>> vec_piece_data,
                         Coordinator *coo,
                         const std::function<void(int res, TxnOutput &)> &) ;
  virtual void SyncBroadcastDispatch(shared_ptr<vector<shared_ptr<SimpleCommand>>> vec_piece_data,
                         Coordinator *coo,
                         const std::function<void(int res, TxnOutput &)> &) ;

	shared_ptr<QuorumEvent> SendReelect();

  shared_ptr<IntEvent> BroadcastDispatch(ReadyPiecesData cmds_by_par,
                        Coordinator* coo,
                        TxData* txn);

  shared_ptr<AndEvent> SendPrepare(Coordinator* coo,
                                         txnid_t tid,
                                         std::vector<int32_t>& sids);
  shared_ptr<AndEvent> SendCommit(Coordinator* coo,
                                     txnid_t tid);
  shared_ptr<AndEvent> SendAbort(Coordinator* coo,
                                    txnid_t tid);
  /*void SendPrepare(parid_t gid,
                   txnid_t tid,
                   std::vector<int32_t> &sids,
                   const std::function<void(int)> &callback) ;*/
  /*void SendCommit(parid_t pid,
                  txnid_t tid,
                  const std::function<void()> &callback) ;
  void SendAbort(parid_t pid,
                 txnid_t tid,
                 const std::function<void()> &callback) ;*/
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
  vector<shared_ptr<MessageEvent>> BroadcastMessage(shardid_t shard_id,
                                                    svrid_t svr_id,
                                                    string& msg);
  std::shared_ptr<MessageEvent> SendMessage(svrid_t svr_id, string& msg);

  void AddMessageHandler(std::function<bool(const string&, string&)>);
  void AddMessageHandler(std::function<bool(const MarshallDeputy&,
                                            MarshallDeputy&)>);

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastBulkPrepare(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        std::function<void(ballot_t, int)> cb){
      verify(0);
    }

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastHeartBeat(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(ballot_t, int)>& cb){
      verify(0);
    }

    virtual void ForwardToLearner(parid_t par_id,
                                  uint64_t slot,
                                  ballot_t ballot,
                                  shared_ptr<Marshallable> cmd,
                                  const std::function<void(uint64_t, ballot_t)>& cb) {
      verify(0);
    }

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncLog(parid_t par_id,
                      shared_ptr<Marshallable> cmd,
                      const std::function<void(shared_ptr<MarshallDeputy>, ballot_t, int)>& cb){
      verify(0);
    }

   virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncNoOps(parid_t par_id,
                    shared_ptr<Marshallable> cmd,
                    const std::function<void(ballot_t, int)>& cb){

	verify(0);
   }

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncCommit(parid_t par_id,
                      shared_ptr<Marshallable> cmd,
                      const std::function<void(ballot_t, int)>& cb){
      verify(0);
    }
  shared_ptr<GetLeaderQuorumEvent> BroadcastGetLeader(parid_t par_id, locid_t cur_pause);
  shared_ptr<QuorumEvent> FailoverPauseSocketOut(parid_t par_id, locid_t loc_id);
  shared_ptr<QuorumEvent> FailoverResumeSocketOut(parid_t par_id, locid_t loc_id);
  void SetNewLeaderProxy(parid_t par_id, locid_t loc_id);
  void SendSimpleCmd(groupid_t gid, SimpleCommand& cmd, std::vector<int32_t>& sids,
      const function<void(int)>& callback);
  
  /* Jetpack recovery begin */
  shared_ptr<QuorumEvent> JetpackBroadcastBeginRecovery(parid_t par_id, locid_t loc_id, 
                                                       const View& old_view, 
                                                       const View& new_view, 
                                                       epoch_t new_view_id);
  shared_ptr<JetpackPullIdSetQuorumEvent> JetpackBroadcastPullIdSet(parid_t par_id, locid_t loc_id,
                                                                   epoch_t jepoch, epoch_t oepoch);
  shared_ptr<JetpackPullCmdQuorumEvent> JetpackBroadcastPullCmd(parid_t par_id, locid_t loc_id, 
                                                               const std::vector<key_t>& keys, epoch_t jepoch, epoch_t oepoch);
  shared_ptr<QuorumEvent> JetpackBroadcastRecordCmd(parid_t par_id, locid_t loc_id,
                                                    epoch_t jepoch, epoch_t oepoch, 
                                                    int sid, int rid, 
                                                    const std::vector<std::pair<key_t, shared_ptr<Marshallable>>>& cmds);
  shared_ptr<JetpackPrepareQuorumEvent> JetpackBroadcastPrepare(parid_t par_id, locid_t loc_id, 
                                                               epoch_t jepoch, epoch_t oepoch, 
                                                               ballot_t max_seen_ballot);
  shared_ptr<JetpackAcceptQuorumEvent> JetpackBroadcastAccept(parid_t par_id, locid_t loc_id, 
                                                            epoch_t jepoch, epoch_t oepoch, 
                                                            ballot_t max_seen_ballot, int sid, int set_size);
  shared_ptr<QuorumEvent> JetpackBroadcastCommit(parid_t par_id, locid_t loc_id, 
                                                 epoch_t jepoch, epoch_t oepoch, 
                                                 int sid, int set_size);
  shared_ptr<JetpackPullRecSetInsQuorumEvent> JetpackBroadcastPullRecSetIns(parid_t par_id, locid_t loc_id, 
                                                                           epoch_t jepoch, epoch_t oepoch, 
                                                                           int sid, int rid);
  shared_ptr<QuorumEvent> JetpackBroadcastFinishRecovery(parid_t par_id, locid_t loc_id, epoch_t oepoch);
  /* Jetpack recovery end */
};

} // namespace janus
