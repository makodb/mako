#pragma once

#include "__dep__.h"
#include "rcc_rpc.h"

#define DepTranServiceImpl ClassicServiceImpl

namespace janus {

class ServerControlServiceImpl;
class TxLogServer;
class SimpleCommand;
class Communicator;
class SchedulerClassic;
class ClassicServiceImpl : public ClassicService {

 public:
  AvgStat stat_sz_gra_start_;
  AvgStat stat_sz_gra_commit_;
  AvgStat stat_sz_gra_ask_;
  AvgStat stat_sz_scc_;
  AvgStat stat_n_ask_;
  AvgStat stat_ro6_sz_vector_;
  uint64_t n_asking_ = 0;

//  std::mutex mtx_;
  Recorder* recorder_{nullptr};
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

  void RuleSpeculativeExecute(const MarshallDeputy& md,
                              bool_t* accepted,
                              int32_t* result,
                              bool_t* is_leader,
                              rrr::DeferredReply done);

  void Dispatch(const i64& cmd_id,
								const DepId& dep_id,
                const MarshallDeputy& cmd,
                int32_t* res,
                TxnOutput* output,
                uint64_t* coro_id,
                MarshallDeputy* view_data,
                rrr::DeferredReply done);

  void FailoverPauseSocketOut(rrr::i32* res,
                              rrr::DeferredReply done);

  void FailoverResumeSocketOut(rrr::i32* res,
                               rrr::DeferredReply done);

  void IsLeader(const locid_t& can_id,
                 bool_t* is_leader,
                 rrr::DeferredReply done);

  void IsFPGALeader(const locid_t& can_id,
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
	        		Profiling* profile,
              MarshallDeputy* view_data,
              rrr::DeferredReply done);

  void Abort(const i64& tid,
             const DepId& dep_id,
             i32* res,
						 bool_t* slow,
             uint64_t* coro_id,
	        	 Profiling* profile,
             MarshallDeputy* view_data,
             rrr::DeferredReply done);

  void EarlyAbort(const i64& tid,
                  i32* res,
                  rrr::DeferredReply done);

  void UpgradeEpoch(const uint32_t& curr_epoch,
                    int32_t* res,
                    rrr::DeferredReply done);

  void TruncateEpoch(const uint32_t& old_epoch,
                     rrr::DeferredReply done);

  void TapirAccept(const txid_t& cmd_id,
                   const ballot_t& ballot,
                   const int32_t& decision,
                   rrr::DeferredReply done);
  void TapirFastAccept(const txid_t& cmd_id,
                       const vector<SimpleCommand>& txn_cmds,
                       rrr::i32* res,
                       rrr::DeferredReply done);
  void TapirDecide(const txid_t& cmd_id,
                   const rrr::i32& decision,
                   rrr::DeferredReply done);

  void CarouselReadAndPrepare(const i64& cmd_id, const MarshallDeputy& cmd,
      const bool_t& leader, int32_t* res, TxnOutput* output,
      rrr::DeferredReply done);
  void CarouselAccept(const txid_t& cmd_id, const ballot_t& ballot,
      const int32_t& decision, rrr::DeferredReply done);
  void CarouselFastAccept(const txid_t& cmd_id, const vector<SimpleCommand>& txn_cmds,
      rrr::i32* res, rrr::DeferredReply done);
  void CarouselDecide(
      const txid_t& cmd_id, const rrr::i32& decision, rrr::DeferredReply done);

  void MsgString(const string& arg,
                 string* ret,
                 rrr::DeferredReply done);

  void MsgMarshall(const MarshallDeputy& arg,
                   MarshallDeputy* ret,
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

  void RccDispatch(const vector<SimpleCommand>& cmd,
                   int32_t* res,
                   TxnOutput* output,
                   MarshallDeputy* p_md_graph,
                   rrr::DeferredReply done);

  void RccPreAccept(const txid_t& txnid,
                    const rank_t& rank,
                    const vector<SimpleCommand>& cmd,
                    int32_t* res,
                    parent_set_t* parents,
                    rrr::DeferredReply done);

  void RccAccept(const txid_t& txnid,
                 const rank_t& rank,
                 const ballot_t& ballot,
                 const parent_set_t& parents,
                 int32_t* res,
                 rrr::DeferredReply done);

  void RccCommit(const txid_t& cmd_id,
                 const rank_t& rank,
                 const int32_t& need_validation,
                 const parent_set_t& parents,
                 int32_t* res,
                 TxnOutput* output,
                 rrr::DeferredReply done);

  void RccFinish(const txid_t& cmd_id,
                 const MarshallDeputy& md_graph,
                 TxnOutput* output,
                 rrr::DeferredReply done);

  void RccInquire(const txid_t& tid,
                  const int32_t& rank,
                  map<txid_t, parent_set_t>*,
                  rrr::DeferredReply done);

  void RccDispatchRo(const SimpleCommand& cmd,
                     map<int32_t, Value>* output,
                     rrr::DeferredReply done);

  void RccInquireValidation(const txid_t& txid, const int32_t& rank, int32_t* ret, rrr::DeferredReply done);
  void RccNotifyGlobalValidation(const txid_t& txid, const int32_t& rank, const int32_t& res, rrr::DeferredReply done);

  void JanusDispatch(const vector<SimpleCommand>& cmd,
                     int32_t* p_res,
                     TxnOutput* p_output,
                     MarshallDeputy* p_md_res_graph,
                     rrr::DeferredReply done);

  void JanusCommit(const txid_t& cmd_id,
                   const rank_t& rank,
                   const int32_t& need_validation,
                   const MarshallDeputy& graph,
                   int32_t* res,
                   TxnOutput* output,
                   rrr::DeferredReply done);

  void JanusCommitWoGraph(const txid_t& cmd_id,
                          const rank_t& rank,
                          const int32_t& need_validation,
                          int32_t* res,
                          TxnOutput* output,
                          rrr::DeferredReply done);

  void JanusInquire(const epoch_t& epoch,
                    const txid_t& tid,
                    MarshallDeputy* p_md_graph,
                    rrr::DeferredReply done);

  void JanusPreAccept(const txid_t& txnid,
                      const rank_t& rank,
                      const vector<SimpleCommand>& cmd,
                      const MarshallDeputy& md_graph,
                      int32_t* res,
                      MarshallDeputy* p_md_res_graph,
                      rrr::DeferredReply done);

  void JanusPreAcceptWoGraph(const txid_t& txnid,
                             const rank_t& rank,
                             const vector<SimpleCommand>& cmd,
                             int32_t* res,
                             MarshallDeputy* res_graph,
                             rrr::DeferredReply done);

  void JanusAccept(const txid_t& txnid,
                   const rank_t& rank,
                   const ballot_t& ballot,
                   const MarshallDeputy& md_graph,
                   int32_t* res,
                   rrr::DeferredReply done);

  void PreAcceptFebruus(const txid_t& tx_id,
                        int32_t* res,
                        uint64_t* timestamp,
                        rrr::DeferredReply done);

  void AcceptFebruus(const txid_t& tx_id,
                     const ballot_t& ballot,
                     const uint64_t& timestamp,
                     int32_t* res,
                     rrr::DeferredReply done);

  void CommitFebruus(const txid_t& tx_id,
                     const uint64_t& timestamp,
                     int32_t* res, rrr::DeferredReply done);
  
  void JetpackBeginRecovery(const MarshallDeputy& old_view, 
                            const MarshallDeputy& new_view, 
                            const epoch_t& new_view_id, 
                            rrr::DeferredReply done);
  
  void JetpackPullIdSet(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        bool_t* ok,
                        epoch_t* reply_jepoch,
                        epoch_t* reply_oepoch,
                        MarshallDeputy* reply_old_view,
                        MarshallDeputy* reply_new_view,
                        MarshallDeputy* id_set,
                        rrr::DeferredReply done);

  void JetpackPullCmd(const epoch_t& jepoch,
                      const epoch_t& oepoch,
                      const MarshallDeputy& key_batch,
                      bool_t* ok,
                      epoch_t* reply_jepoch,
                      epoch_t* reply_oepoch,
                      MarshallDeputy* reply_old_view,
                      MarshallDeputy* reply_new_view,
                      MarshallDeputy* cmd_batch,
                      rrr::DeferredReply done);
 
  void JetpackRecordCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const int32_t& sid,
                        const int32_t& rid,
                        const MarshallDeputy& cmd_batch, 
                        rrr::DeferredReply done);
 
  void JetpackPrepare(const epoch_t& jepoch,
                      const epoch_t& oepoch,
                      const ballot_t& max_seen_ballot,
                      bool_t* ok,
                      epoch_t* reply_jepoch,
                      epoch_t* reply_oepoch,
                      MarshallDeputy* reply_old_view,
                      MarshallDeputy* reply_new_view,
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
                     MarshallDeputy* reply_old_view,
                     MarshallDeputy* reply_new_view,
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
                            MarshallDeputy* reply_old_view,
                            MarshallDeputy* reply_new_view,
                            MarshallDeputy* cmd,
                            rrr::DeferredReply done);

  void JetpackFinishRecovery(const epoch_t& oepoch,
                             rrr::DeferredReply done);

 protected:
  void RegisterStats();

  // BEGIN typed-rpc-decls (ClassicServiceImpl)
  // Typed RPC interface overrides (new API).
  void ReElect(const ClassicService::RpcReElectRequest& req, ClassicService::RpcReElectResponse& resp, rrr::DeferredReply defer) override;
  void RuleSpeculativeExecute(const ClassicService::RpcRuleSpeculativeExecuteRequest& req, ClassicService::RpcRuleSpeculativeExecuteResponse& resp, rrr::DeferredReply defer) override;
  void Dispatch(const ClassicService::RpcDispatchRequest& req, ClassicService::RpcDispatchResponse& resp, rrr::DeferredReply defer) override;
  void FailoverPauseSocketOut(const ClassicService::RpcFailoverPauseSocketOutRequest& req, ClassicService::RpcFailoverPauseSocketOutResponse& resp, rrr::DeferredReply defer) override;
  void FailoverResumeSocketOut(const ClassicService::RpcFailoverResumeSocketOutRequest& req, ClassicService::RpcFailoverResumeSocketOutResponse& resp, rrr::DeferredReply defer) override;
  void SimpleCmd(const ClassicService::RpcSimpleCmdRequest& req, ClassicService::RpcSimpleCmdResponse& resp, rrr::DeferredReply defer) override;
  void IsLeader(const ClassicService::RpcIsLeaderRequest& req, ClassicService::RpcIsLeaderResponse& resp, rrr::DeferredReply defer) override;
  void IsFPGALeader(const ClassicService::RpcIsFPGALeaderRequest& req, ClassicService::RpcIsFPGALeaderResponse& resp, rrr::DeferredReply defer) override;
  void Prepare(const ClassicService::RpcPrepareRequest& req, ClassicService::RpcPrepareResponse& resp, rrr::DeferredReply defer) override;
  void Commit(const ClassicService::RpcCommitRequest& req, ClassicService::RpcCommitResponse& resp, rrr::DeferredReply defer) override;
  void Abort(const ClassicService::RpcAbortRequest& req, ClassicService::RpcAbortResponse& resp, rrr::DeferredReply defer) override;
  void EarlyAbort(const ClassicService::RpcEarlyAbortRequest& req, ClassicService::RpcEarlyAbortResponse& resp, rrr::DeferredReply defer) override;
  void rpc_null(const ClassicService::RpcRpcNullRequest& req, ClassicService::RpcRpcNullResponse& resp, rrr::DeferredReply defer) override;
  void UpgradeEpoch(const ClassicService::RpcUpgradeEpochRequest& req, ClassicService::RpcUpgradeEpochResponse& resp, rrr::DeferredReply defer) override;
  void TruncateEpoch(const ClassicService::RpcTruncateEpochRequest& req, ClassicService::RpcTruncateEpochResponse& resp, rrr::DeferredReply defer) override;
  void TapirAccept(const ClassicService::RpcTapirAcceptRequest& req, ClassicService::RpcTapirAcceptResponse& resp, rrr::DeferredReply defer) override;
  void TapirFastAccept(const ClassicService::RpcTapirFastAcceptRequest& req, ClassicService::RpcTapirFastAcceptResponse& resp, rrr::DeferredReply defer) override;
  void TapirDecide(const ClassicService::RpcTapirDecideRequest& req, ClassicService::RpcTapirDecideResponse& resp, rrr::DeferredReply defer) override;
  void CarouselReadAndPrepare(const ClassicService::RpcCarouselReadAndPrepareRequest& req, ClassicService::RpcCarouselReadAndPrepareResponse& resp, rrr::DeferredReply defer) override;
  void CarouselAccept(const ClassicService::RpcCarouselAcceptRequest& req, ClassicService::RpcCarouselAcceptResponse& resp, rrr::DeferredReply defer) override;
  void CarouselFastAccept(const ClassicService::RpcCarouselFastAcceptRequest& req, ClassicService::RpcCarouselFastAcceptResponse& resp, rrr::DeferredReply defer) override;
  void CarouselDecide(const ClassicService::RpcCarouselDecideRequest& req, ClassicService::RpcCarouselDecideResponse& resp, rrr::DeferredReply defer) override;
  void RccDispatch(const ClassicService::RpcRccDispatchRequest& req, ClassicService::RpcRccDispatchResponse& resp, rrr::DeferredReply defer) override;
  void RccFinish(const ClassicService::RpcRccFinishRequest& req, ClassicService::RpcRccFinishResponse& resp, rrr::DeferredReply defer) override;
  void RccInquire(const ClassicService::RpcRccInquireRequest& req, ClassicService::RpcRccInquireResponse& resp, rrr::DeferredReply defer) override;
  void RccDispatchRo(const ClassicService::RpcRccDispatchRoRequest& req, ClassicService::RpcRccDispatchRoResponse& resp, rrr::DeferredReply defer) override;
  void RccInquireValidation(const ClassicService::RpcRccInquireValidationRequest& req, ClassicService::RpcRccInquireValidationResponse& resp, rrr::DeferredReply defer) override;
  void RccNotifyGlobalValidation(const ClassicService::RpcRccNotifyGlobalValidationRequest& req, ClassicService::RpcRccNotifyGlobalValidationResponse& resp, rrr::DeferredReply defer) override;
  void JanusDispatch(const ClassicService::RpcJanusDispatchRequest& req, ClassicService::RpcJanusDispatchResponse& resp, rrr::DeferredReply defer) override;
  void JanusCommit(const ClassicService::RpcJanusCommitRequest& req, ClassicService::RpcJanusCommitResponse& resp, rrr::DeferredReply defer) override;
  void RccCommit(const ClassicService::RpcRccCommitRequest& req, ClassicService::RpcRccCommitResponse& resp, rrr::DeferredReply defer) override;
  void JanusCommitWoGraph(const ClassicService::RpcJanusCommitWoGraphRequest& req, ClassicService::RpcJanusCommitWoGraphResponse& resp, rrr::DeferredReply defer) override;
  void JanusInquire(const ClassicService::RpcJanusInquireRequest& req, ClassicService::RpcJanusInquireResponse& resp, rrr::DeferredReply defer) override;
  void RccPreAccept(const ClassicService::RpcRccPreAcceptRequest& req, ClassicService::RpcRccPreAcceptResponse& resp, rrr::DeferredReply defer) override;
  void JanusPreAccept(const ClassicService::RpcJanusPreAcceptRequest& req, ClassicService::RpcJanusPreAcceptResponse& resp, rrr::DeferredReply defer) override;
  void JanusPreAcceptWoGraph(const ClassicService::RpcJanusPreAcceptWoGraphRequest& req, ClassicService::RpcJanusPreAcceptWoGraphResponse& resp, rrr::DeferredReply defer) override;
  void RccAccept(const ClassicService::RpcRccAcceptRequest& req, ClassicService::RpcRccAcceptResponse& resp, rrr::DeferredReply defer) override;
  void JanusAccept(const ClassicService::RpcJanusAcceptRequest& req, ClassicService::RpcJanusAcceptResponse& resp, rrr::DeferredReply defer) override;
  void PreAcceptFebruus(const ClassicService::RpcPreAcceptFebruusRequest& req, ClassicService::RpcPreAcceptFebruusResponse& resp, rrr::DeferredReply defer) override;
  void AcceptFebruus(const ClassicService::RpcAcceptFebruusRequest& req, ClassicService::RpcAcceptFebruusResponse& resp, rrr::DeferredReply defer) override;
  void CommitFebruus(const ClassicService::RpcCommitFebruusRequest& req, ClassicService::RpcCommitFebruusResponse& resp, rrr::DeferredReply defer) override;
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
