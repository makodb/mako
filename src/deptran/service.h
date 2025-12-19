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
  ServerControlServiceImpl* scsi_; // for statistics;
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

  void rpc_null(DeferredReply* defer) override ;

	void ReElect(bool_t* success,
							 DeferredReply* defer) override;

  void RuleSpeculativeExecute(const MarshallDeputy& md,
                              bool_t* accepted,
                              int32_t* result,
                              bool_t* is_leader,
                              rrr::DeferredReply* defer) override;

  void Dispatch(const i64& cmd_id,
								const DepId& dep_id,
                const MarshallDeputy& cmd,
                int32_t* res,
                TxnOutput* output,
                uint64_t* coro_id,
                MarshallDeputy* view_data,
                DeferredReply* defer_reply) override;

  void FailoverPauseSocketOut(rrr::i32* res,
                              rrr::DeferredReply* defer) override ;

  void FailoverResumeSocketOut(rrr::i32* res,
                               rrr::DeferredReply* defer) override ;

  void IsLeader(const locid_t& can_id,
                 bool_t* is_leader,
                 DeferredReply* defer_reply) override ;

  void IsFPGALeader(const locid_t& can_id,
                 bool_t* is_leader,
                 DeferredReply* defer_reply) override ;

  void SimpleCmd (const SimpleCommand& cmd, 
                      i32* res, DeferredReply* defer_reply) override ;

  void Prepare(const i64& tid,
               const std::vector<i32>& sids,
               const DepId& dep_id,
               i32* res,
							 bool_t* slow,
               uint64_t* coro_id,
               DeferredReply* defer) override;

  void Commit(const i64& tid,
              const DepId& dep_id,
              i32* res,
							bool_t* slow,
              uint64_t* coro_id,
	        		Profiling* profile,
              MarshallDeputy* view_data,
              DeferredReply* defer) override;

  void Abort(const i64& tid,
             const DepId& dep_id,
             i32* res,
						 bool_t* slow,
             uint64_t* coro_id,
	        	 Profiling* profile,
             MarshallDeputy* view_data,
             DeferredReply* defer) override;

  void EarlyAbort(const i64& tid,
                  i32* res,
                  DeferredReply* defer) override;

  void UpgradeEpoch(const uint32_t& curr_epoch,
                    int32_t* res,
                    DeferredReply* defer) override;

  void TruncateEpoch(const uint32_t& old_epoch,
                     DeferredReply* defer) override;

  void TapirAccept(const txid_t& cmd_id,
                   const ballot_t& ballot,
                   const int32_t& decision,
                   rrr::DeferredReply* defer) override;
  void TapirFastAccept(const txid_t& cmd_id,
                       const vector<SimpleCommand>& txn_cmds,
                       rrr::i32* res,
                       rrr::DeferredReply* defer) override;
  void TapirDecide(const txid_t& cmd_id,
                   const rrr::i32& decision,
                   rrr::DeferredReply* defer) override;

  void CarouselReadAndPrepare(const i64& cmd_id, const MarshallDeputy& cmd,
      const bool_t& leader, int32_t* res, TxnOutput* output,
      DeferredReply* defer_reply) override;
  void CarouselAccept(const txid_t& cmd_id, const ballot_t& ballot,
      const int32_t& decision, rrr::DeferredReply* defer) override;
  void CarouselFastAccept(const txid_t& cmd_id, const vector<SimpleCommand>& txn_cmds,
      rrr::i32* res, rrr::DeferredReply* defer) override;
  void CarouselDecide(
      const txid_t& cmd_id, const rrr::i32& decision, rrr::DeferredReply* defer) override;

  void MsgString(const string& arg,
                 string* ret,
                 rrr::DeferredReply* defer) override;

  void MsgMarshall(const MarshallDeputy& arg,
                   MarshallDeputy* ret,
                   rrr::DeferredReply* defer) override;

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
                     rusty::Arc<rrr::PollThread> poll_thread_worker,
                     ServerControlServiceImpl* scsi = NULL);

  void RccDispatch(const vector<SimpleCommand>& cmd,
                   int32_t* res,
                   TxnOutput* output,
                   MarshallDeputy* p_md_graph,
                   DeferredReply* defer) override;

  void RccPreAccept(const txid_t& txnid,
                    const rank_t& rank,
                    const vector<SimpleCommand>& cmd,
                    int32_t* res,
                    parent_set_t* parents,
                    DeferredReply* defer) override;

  void RccAccept(const txid_t& txnid,
                 const rank_t& rank,
                 const ballot_t& ballot,
                 const parent_set_t& parents,
                 int32_t* res,
                 DeferredReply* defer) override;

  void RccCommit(const txid_t& cmd_id,
                 const rank_t& rank,
                 const int32_t& need_validation,
                 const parent_set_t& parents,
                 int32_t* res,
                 TxnOutput* output,
                 DeferredReply* defer) override;

  void RccFinish(const txid_t& cmd_id,
                 const MarshallDeputy& md_graph,
                 TxnOutput* output,
                 DeferredReply* defer) override;

  void RccInquire(const txid_t& tid,
                  const int32_t& rank,
                  map<txid_t, parent_set_t>*,
                  DeferredReply*) override;

  void RccDispatchRo(const SimpleCommand& cmd,
                     map<int32_t, Value>* output,
                     DeferredReply* reply) override;

  void RccInquireValidation(const txid_t& txid, const int32_t& rank, int32_t* ret, DeferredReply* reply) override;
  void RccNotifyGlobalValidation(const txid_t& txid, const int32_t& rank, const int32_t& res, DeferredReply* reply) override;

  void JanusDispatch(const vector<SimpleCommand>& cmd,
                     int32_t* p_res,
                     TxnOutput* p_output,
                     MarshallDeputy* p_md_res_graph,
                     DeferredReply* p_defer) override;

  void JanusCommit(const txid_t& cmd_id,
                   const rank_t& rank,
                   const int32_t& need_validation,
                   const MarshallDeputy& graph,
                   int32_t* res,
                   TxnOutput* output,
                   DeferredReply* defer) override;

  void JanusCommitWoGraph(const txid_t& cmd_id,
                          const rank_t& rank,
                          const int32_t& need_validation,
                          int32_t* res,
                          TxnOutput* output,
                          DeferredReply* defer) override;

  void JanusInquire(const epoch_t& epoch,
                    const txid_t& tid,
                    MarshallDeputy* p_md_graph,
                    DeferredReply*) override;

  void JanusPreAccept(const txid_t& txnid,
                      const rank_t& rank,
                      const vector<SimpleCommand>& cmd,
                      const MarshallDeputy& md_graph,
                      int32_t* res,
                      MarshallDeputy* p_md_res_graph,
                      DeferredReply* defer) override;

  void JanusPreAcceptWoGraph(const txid_t& txnid,
                             const rank_t& rank,
                             const vector<SimpleCommand>& cmd,
                             int32_t* res,
                             MarshallDeputy* res_graph,
                             DeferredReply* defer) override;

  void JanusAccept(const txid_t& txnid,
                   const rank_t& rank,
                   const ballot_t& ballot,
                   const MarshallDeputy& md_graph,
                   int32_t* res,
                   DeferredReply* defer) override;

  void PreAcceptFebruus(const txid_t& tx_id,
                        int32_t* res,
                        uint64_t* timestamp,
                        DeferredReply* defer) override;

  void AcceptFebruus(const txid_t& tx_id,
                     const ballot_t& ballot,
                     const uint64_t& timestamp,
                     int32_t* res,
                     DeferredReply* defer) override;

  void CommitFebruus(const txid_t& tx_id,
                     const uint64_t& timestamp,
                     int32_t* res, DeferredReply* defer) override;
  
  void JetpackBeginRecovery(const MarshallDeputy& old_view, 
                            const MarshallDeputy& new_view, 
                            const epoch_t& new_view_id, 
                            rrr::DeferredReply* defer) override;
  
  void JetpackPullIdSet(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        bool_t* ok,
                        epoch_t* reply_jepoch,
                        epoch_t* reply_oepoch,
                        MarshallDeputy* reply_old_view,
                        MarshallDeputy* reply_new_view,
                        MarshallDeputy* id_set,
                        rrr::DeferredReply* defer) override;

  void JetpackPullCmd(const epoch_t& jepoch,
                      const epoch_t& oepoch,
                      const MarshallDeputy& key_batch,
                      bool_t* ok,
                      epoch_t* reply_jepoch,
                      epoch_t* reply_oepoch,
                      MarshallDeputy* reply_old_view,
                      MarshallDeputy* reply_new_view,
                      MarshallDeputy* cmd_batch,
                      rrr::DeferredReply* defer) override;
 
  void JetpackRecordCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const int32_t& sid,
                        const int32_t& rid,
                        const MarshallDeputy& cmd_batch, 
                        rrr::DeferredReply* defer) override;
 
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
                      rrr::DeferredReply* defer) override;
 
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
                     rrr::DeferredReply* defer) override;
 
  void JetpackCommit(const epoch_t& jepoch,
                     const epoch_t& oepoch, 
                     const int32_t& sid, 
                     const int32_t& set_size, 
                     rrr::DeferredReply* defer) override;
 
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
                            rrr::DeferredReply* defer) override;

  void JetpackFinishRecovery(const epoch_t& oepoch,
                             rrr::DeferredReply* defer) override;

 protected:
  void RegisterStats();
};

} // namespace janus
