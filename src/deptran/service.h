#pragma once

#include "__dep__.h"
#include "rcc_rpc.h"

namespace janus {

class ServerControlServiceImpl;
class TxLogServer;
class SimpleCommand;
class Communicator;
class SchedulerClassic;
class ClassicServiceImpl : public ClassicService {

 public:
//  std::mutex mtx_;
  // removed `Recorder* recorder_{nullptr};`
  // — only assignment was a commented-out
  // `recorder_ = new Recorder(path);` in `service.cc::SetupTransport`
  // and the only readers were `auto& recorder = s->recorder_;
  // if (recorder) {...}` blocks in `paxos_worker.cc::WaitForShutdown`
  // and `server_worker.cc::WaitForShutdown` (also gone in this phase).
  Communicator* comm_{nullptr};

  TxLogServer* dtxn_sched_;
  rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;
  std::atomic<int32_t> clt_cnt_{0};

  ~ClassicServiceImpl() {
    if (dtxn_sched_)
      delete dtxn_sched_;
  }

  TxLogServer* dtxn_sched() {
    return dtxn_sched_;
  }

  void rpc_null(rrr::DeferredReply done);

	void ReElect(bool_t* success,
							 rrr::DeferredReply done);
  void Dispatch(const i64& cmd_id,
								const DepId& dep_id,
                const janus::Command& cmd,
                int32_t* res,
                TxnOutput* output,
                uint64_t* coro_id,
                janus::Command* view_data,
                rrr::DeferredReply done);

  void FailoverPauseSocketOut(rrr::i32* res,
                              rrr::DeferredReply done);

  void FailoverResumeSocketOut(rrr::i32* res,
                               rrr::DeferredReply done);

  void IsLeader(const locid_t& can_id,
                 bool_t* is_leader,
                 rrr::DeferredReply done);

  void SimpleCmd (const SimpleCommand& cmd, 
                      i32* res, rrr::DeferredReply done);

  void Prepare(const i64& tid,
               const std::vector<i32>& sids,
               const DepId& dep_id,
               i32* res,
							 bool_t* slow,
               uint64_t* coro_id,
               rrr::DeferredReply done);

  void Commit(const i64& tid,
              const DepId& dep_id,
              i32* res,
							bool_t* slow,
              uint64_t* coro_id,
              janus::Command* view_data,
              rrr::DeferredReply done);

  void Abort(const i64& tid,
             const DepId& dep_id,
             i32* res,
						 bool_t* slow,
             uint64_t* coro_id,
             janus::Command* view_data,
             rrr::DeferredReply done);

  void EarlyAbort(const i64& tid,
                  i32* res,
                  rrr::DeferredReply done);

  void UpgradeEpoch(const uint32_t& curr_epoch,
                    int32_t* res,
                    rrr::DeferredReply done);

  void TruncateEpoch(const uint32_t& old_epoch,
                     rrr::DeferredReply done);

  void MsgString(const string& arg,
                 string* ret,
                 rrr::DeferredReply done);

  void MsgMarshall(const janus::Command& arg,
                   janus::Command* ret,
                   rrr::DeferredReply done);

#ifdef PIECE_COUNT
  typedef struct piece_count_key_t{
      i32 t_type;
      i32 p_type;
      bool operator<(const piece_count_key_t &rhs) const {
          if (t_type < rhs.t_type)
              return true;
          else if (t_type == rhs.t_type && p_type < rhs.p_type)
              return true;
          return false;
      }
  } piece_count_key_t;

  std::map<piece_count_key_t, uint64_t> piece_count_;

  std::unordered_set<i64> piece_count_tid_;

  uint64_t piece_count_prepare_fail_, piece_count_prepare_success_;

  base::Timer piece_count_timer_;
#endif

 public:

  ClassicServiceImpl() = delete;

  ClassicServiceImpl(TxLogServer* sched,
                     rusty::Arc<rrr::PollThread> poll_thread_worker);

  void JetpackBeginRecovery(const janus::Command& old_view,
                            const janus::Command& new_view, 
                            const epoch_t& new_view_id, 
                            rrr::DeferredReply done);
  
  void JetpackPullIdSet(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        bool_t* ok,
                        epoch_t* reply_jepoch,
                        epoch_t* reply_oepoch,
                        janus::Command* reply_old_view,
                        janus::Command* reply_new_view,
                        janus::Command* id_set,
                        rrr::DeferredReply done);

  void JetpackPullCmd(const epoch_t& jepoch,
                      const epoch_t& oepoch,
                      const janus::Command& key_batch,
                      bool_t* ok,
                      epoch_t* reply_jepoch,
                      epoch_t* reply_oepoch,
                      janus::Command* reply_old_view,
                      janus::Command* reply_new_view,
                      janus::Command* cmd_batch,
                      rrr::DeferredReply done);
 
  void JetpackRecordCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const int32_t& sid,
                        const int32_t& rid,
                        const janus::Command& cmd_batch, 
                        rrr::DeferredReply done);
 
  void JetpackPrepare(const epoch_t& jepoch,
                      const epoch_t& oepoch,
                      const ballot_t& max_seen_ballot,
                      bool_t* ok,
                      epoch_t* reply_jepoch,
                      epoch_t* reply_oepoch,
                      janus::Command* reply_old_view,
                      janus::Command* reply_new_view,
                      ballot_t* reply_max_seen_ballot,
                      ballot_t* accepted_ballot,
                      int32_t* replied_sid,
                      int32_t* replied_set_size,
                      rrr::DeferredReply done);
 
  void JetpackAccept(const epoch_t& jepoch,
                     const epoch_t& oepoch,
                     const ballot_t& max_seen_ballot,
                     const int32_t& sid,
                     const int32_t& set_size,
                     bool_t* ok,
                     epoch_t* reply_jepoch,
                     epoch_t* reply_oepoch,
                     janus::Command* reply_old_view,
                     janus::Command* reply_new_view,
                     ballot_t* reply_max_seen_ballot,
                     rrr::DeferredReply done);
 
  void JetpackCommit(const epoch_t& jepoch,
                     const epoch_t& oepoch, 
                     const int32_t& sid, 
                     const int32_t& set_size, 
                     rrr::DeferredReply done);
 
  void JetpackPullRecSetIns(const epoch_t& jepoch,
                            const epoch_t& oepoch,
                            const int32_t& sid,
                            const int32_t& rid,
                            bool_t* ok,
                            epoch_t* reply_jepoch,
                            epoch_t* reply_oepoch,
                            janus::Command* reply_old_view,
                            janus::Command* reply_new_view,
                            janus::Command* cmd,
                            rrr::DeferredReply done);

  void JetpackFinishRecovery(const epoch_t& oepoch,
                             rrr::DeferredReply done);

 protected:
  // BEGIN typed-rpc-decls (ClassicServiceImpl)
  // Typed RPC interface overrides (new API).
  void ReElect(const ClassicService::RpcReElectRequest& req, ClassicService::RpcReElectResponse& resp, rrr::DeferredReply defer) override;
  void Dispatch(const ClassicService::RpcDispatchRequest& req, ClassicService::RpcDispatchResponse& resp, rrr::DeferredReply defer) override;
  void FailoverPauseSocketOut(const ClassicService::RpcFailoverPauseSocketOutRequest& req, ClassicService::RpcFailoverPauseSocketOutResponse& resp, rrr::DeferredReply defer) override;
  void FailoverResumeSocketOut(const ClassicService::RpcFailoverResumeSocketOutRequest& req, ClassicService::RpcFailoverResumeSocketOutResponse& resp, rrr::DeferredReply defer) override;
  void SimpleCmd(const ClassicService::RpcSimpleCmdRequest& req, ClassicService::RpcSimpleCmdResponse& resp, rrr::DeferredReply defer) override;
  void IsLeader(const ClassicService::RpcIsLeaderRequest& req, ClassicService::RpcIsLeaderResponse& resp, rrr::DeferredReply defer) override;
  void Prepare(const ClassicService::RpcPrepareRequest& req, ClassicService::RpcPrepareResponse& resp, rrr::DeferredReply defer) override;
  void Commit(const ClassicService::RpcCommitRequest& req, ClassicService::RpcCommitResponse& resp, rrr::DeferredReply defer) override;
  void Abort(const ClassicService::RpcAbortRequest& req, ClassicService::RpcAbortResponse& resp, rrr::DeferredReply defer) override;
  void EarlyAbort(const ClassicService::RpcEarlyAbortRequest& req, ClassicService::RpcEarlyAbortResponse& resp, rrr::DeferredReply defer) override;
  void rpc_null(const ClassicService::RpcRpcNullRequest& req, ClassicService::RpcRpcNullResponse& resp, rrr::DeferredReply defer) override;
  void UpgradeEpoch(const ClassicService::RpcUpgradeEpochRequest& req, ClassicService::RpcUpgradeEpochResponse& resp, rrr::DeferredReply defer) override;
  void TruncateEpoch(const ClassicService::RpcTruncateEpochRequest& req, ClassicService::RpcTruncateEpochResponse& resp, rrr::DeferredReply defer) override;
  void JetpackBeginRecovery(const ClassicService::RpcJetpackBeginRecoveryRequest& req, ClassicService::RpcJetpackBeginRecoveryResponse& resp, rrr::DeferredReply defer) override;
  void JetpackPullIdSet(const ClassicService::RpcJetpackPullIdSetRequest& req, ClassicService::RpcJetpackPullIdSetResponse& resp, rrr::DeferredReply defer) override;
  void JetpackPullCmd(const ClassicService::RpcJetpackPullCmdRequest& req, ClassicService::RpcJetpackPullCmdResponse& resp, rrr::DeferredReply defer) override;
  void JetpackRecordCmd(const ClassicService::RpcJetpackRecordCmdRequest& req, ClassicService::RpcJetpackRecordCmdResponse& resp, rrr::DeferredReply defer) override;
  void JetpackPrepare(const ClassicService::RpcJetpackPrepareRequest& req, ClassicService::RpcJetpackPrepareResponse& resp, rrr::DeferredReply defer) override;
  void JetpackAccept(const ClassicService::RpcJetpackAcceptRequest& req, ClassicService::RpcJetpackAcceptResponse& resp, rrr::DeferredReply defer) override;
  void JetpackCommit(const ClassicService::RpcJetpackCommitRequest& req, ClassicService::RpcJetpackCommitResponse& resp, rrr::DeferredReply defer) override;
  void JetpackPullRecSetIns(const ClassicService::RpcJetpackPullRecSetInsRequest& req, ClassicService::RpcJetpackPullRecSetInsResponse& resp, rrr::DeferredReply defer) override;
  void JetpackFinishRecovery(const ClassicService::RpcJetpackFinishRecoveryRequest& req, ClassicService::RpcJetpackFinishRecoveryResponse& resp, rrr::DeferredReply defer) override;
  void MsgString(const ClassicService::RpcMsgStringRequest& req, ClassicService::RpcMsgStringResponse& resp, rrr::DeferredReply defer) override;
  void MsgMarshall(const ClassicService::RpcMsgMarshallRequest& req, ClassicService::RpcMsgMarshallResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (ClassicServiceImpl)
};

} // namespace janus
