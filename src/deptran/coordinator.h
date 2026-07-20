#pragma once

#include "__dep__.h"
#include "constants.h"
#include "procedure.h"
//#include "all.h"
#include "msg.h"
#include "communicator.h"
#include "scheduler.h"
#include "client_worker.h"
#include "client_status.h"

namespace janus {

enum ForwardRequestState { NONE=0, PROCESS_FORWARD_REQUEST, FORWARD_TO_LEADER };

//class CoordinatorBase {
//public:
//  std::mutex mtx_;
//  uint32_t n_start_ = 0;
//  locid_t loc_id_ = -1;
//  virtual ~CoordinatorBase() = default;
//  // TODO do_one should be replaced with Submit.
//  virtual void DoTxAsync(TxRequest &) = 0;
//  virtual void Reset() = 0;
//  virtual void restart(TxData *ch) = 0;
//};

class Coordinator {
 public:
  void *svr_workers_g{nullptr};

  static std::mutex _dbg_txid_lock_;
  static std::unordered_set<txid_t> _dbg_txid_set_;
  bool _inuse_{false};
  // removed `uint32_t n_start_ = 0;` —
  // declared but never read.  The live counter is
  // `client_status_->txn_start_one(...)` which lives on
  // `ClientStatus`.
  locid_t loc_id_ = -1;
  uint32_t coo_id_;
  uint32_t offset_;
  uint32_t cli_id_;
  uint32_t coro_id_;
	i64 dep_id_ = -1;
	int concurrent;
  // removed `std::vector<int> ids_;` —
  // declared and `push_back`ed once at `communicator.cc:762` but
  // never read.  The push site was deleted alongside the field.
  parid_t par_id_ = -1;
  slotid_t slot_id_ = 0;
  ballot_t curr_ballot_ = 1;

	std::vector<rusty::Arc<QuorumEvent>> quorum_events_;
  // Unused live (only a commented reference in mencius/commo.cc); nullable,
  // was a default-null shared_ptr — Option<Arc> keeps the empty state without
  // eagerly constructing a placeholder QuorumEvent.
  rusty::Option<rusty::Arc<QuorumEvent>> sp_quorum_event;
  // Assigned from BroadcastDispatch() (which now returns Arc<IntEvent>) before
  // every use in classic/coordinator.cc via `sp_int_event->...`. Arc has no
  // null/default state and the Coordinator ctor (coordinator.cc) does not
  // initialize it, so a default member initializer supplies a throwaway event
  // that is overwritten before any wait (same pattern as rcc/tx.h).
  rusty::Arc<IntEvent> sp_int_event{Reactor::create_sp_event<IntEvent>()};
  int benchmark_;
  // Shared client status for statistics tracking
  rusty::Option<rusty::Arc<ClientStatus>> client_status_;
  // Transaction timeout in microseconds (from config, default 30 seconds)
  uint64_t txn_timeout_{30000000};
  uint32_t thread_id_;
  // removed `bool batch_optimal_ = false;`
  // — declared but the only reference was a commented-out
  // `verify(!batch_optimal_)` in `snow/ro6_coord.cc:243`; never
  // written or read in production paths.
	bool slow_ = false;
  bool retry_wait_;
  // Nullable: client_worker.cc creates these lazily (rusty::Some(...)) and
  // resets them to rusty::None after each transaction, so they must be Option.
  rusty::Option<rusty::Arc<IntEvent>> sp_ev_commit_{};
  rusty::Option<rusty::Arc<IntEvent>> sp_ev_done_{};

  std::atomic<uint64_t> next_pie_id_;
  std::atomic<uint64_t> next_txn_id_;

  std::recursive_mutex mtx_{};
  // removed `Recorder *recorder_{nullptr};`
  // — only assignment was `recorder_ = NULL;` in the constructor;
  // no surviving `recorder_ = new Recorder(...)` call site, so the
  // field was always nullptr.  The `if (recorder_) delete recorder_;`
  // destructor cleanup was dead-after-null-check-only.  The
  // `JanusCoordinator::recorder_` shadow declaration is also removed
  // in this phase.
  CmdData *cmd_{nullptr};
  phase_t phase_ = 0;
  map<innid_t, bool> dispatch_acks_ = {};
  // removed `map<innid_t, bool> handout_outs_ = {};`
  // — declared but never written or read anywhere in the codebase.
  Sharding* sharding_ = nullptr;
  shared_ptr<TxnRegistry> txn_reg_{nullptr};
  Communicator* commo_ = nullptr;
  Frame* frame_ = nullptr;
  ClientWorker* client_worker_ = nullptr;

  txid_t ongoing_tx_id_{0};
  ForwardRequestState forward_status_ = NONE;

  // should be reset on issuing a new request
  uint32_t n_retry_ = 0;
  // below should be reset on retry.
  bool committed_ = false;
  bool commit_reported_ = false;
  bool validation_result_{true};
  bool aborted_ = false;
  // removed `bool repeat_ = false;` —
  // default-initialised false, written only to false at
  // `classic/coordinator.cc:187`, read only at
  // `classic/coordinator.cc:462` inside an empty-body
  // `if(repeat_) {}` (which was therefore unreachable code).
  uint32_t n_dispatch_ = 0;
  uint32_t n_dispatch_ack_ = 0;
  // removed `uint32_t n_prepare_req_ = 0;`
  // — only ever zeroed (in Reset() and classic/coordinator.cc:180);
  // never incremented or read.  Counterpart `n_prepare_ack_` stays
  // because classic/coordinator.cc::DispatchAck increments it.
  uint32_t n_prepare_ack_ = 0;
  uint32_t n_finish_req_ = 0;
  uint32_t n_finish_ack_ = 0;
  // removed `std::vector<int> site_prepare_;`,
  // `site_commit_;`, and `site_abort_;` — write-only counters with
  // no observers.  All `site_*[rp]++` increments and the
  // `site_prepare_[i] = 0` reset loop were dead-as-side-effect.
  // Companion `.resize(...)` initialisations in `coordinator.cc::Reset`
  // and the loop in `classic/coordinator.cc::DispatchRetry` removed
  // alongside the fields.
  // removed `std::vector<int> site_piece_;`
  // — resized at coordinator.cc:53 alongside the other site_* vectors
  // but never written or read otherwise (only commented-out debug
  // logging at coordinator.cc:60-64 referenced it).  The resize call
  // was removed alongside the field.
  rusty::Function<void()> commit_callback_ = [] () {verify(0);};
  // removed
  //   `rusty::Function<void()> exe_callback_ = [] () {verify(0);};`
  // — declared but never set or invoked anywhere.
  // above should be reset

  /******global unique id begin********/
  int cmd_in_client_count = 0;
  /******global unique id end********/

  double created_time_ = SimpleRWCommand::GetCurrentMsTime();
  
#ifdef LATENCY_DEBUG
  Distribution client2leader_, client2test_point_, client2leader_send_;
#endif

  bool go_to_fastpath_;

#ifdef TXN_STAT
  typedef struct txn_stat_t {
    uint64_t                             n_serv_tch;
    uint64_t                             n_txn;
    std::unordered_map<int32_t, uint64_t>piece_cnt;
    txn_stat_t() : n_serv_tch(0), n_txn(0) {}
    void one(uint64_t _n_serv_tch, const std::vector<int32_t>& pie) {
      n_serv_tch += _n_serv_tch;
      n_txn++;

      for (int i = 0; i < pie.size(); i++) {
        if (pie[i] != 0) {
          auto it = piece_cnt.find(pie[i]);

          if (it == piece_cnt.end()) {
            piece_cnt[pie[i]] = 1;
          } else {
            piece_cnt[pie[i]]++;
          }
        }
      }
    }

    void output() {
      Log::info("SERV_TCH: %lu, TXN_CNT: %lu, MEAN_SERV_TCH_PER_TXN: %lf",
                n_serv_tch, n_txn, ((double)n_serv_tch) / n_txn);

      for (auto& it : piece_cnt) {
        Log::info("\tPIECE: %d, PIECE_CNT: %lu, MEAN_PIECE_PER_TXN: %lf",
                  it.first, it.second, ((double)it.second) / n_txn);
      }
    }
  } txn_stat_t;
  std::unordered_map<int32_t, txn_stat_t> txn_stats_;
#endif /* ifdef TXN_STAT */
  // Constructor takes Arc<ClientStatus> for statistics tracking
  Coordinator(uint32_t coo_id,
              int benchmark,
              rusty::Option<rusty::Arc<ClientStatus>> client_status = rusty::None,
              uint32_t thread_id = 0);

  virtual ~Coordinator();

  /** thread unsafe */
  uint64_t next_pie_id() {
    return this->next_pie_id_++;
  }

  /** thread unsafe */
  uint64_t next_txn_id() {
    auto ret = next_txn_id_++;
#ifdef DEBUG_CHECK
    _dbg_txid_lock_.lock();
    verify(_dbg_txid_set_.count(ret) == 0);
    _dbg_txid_set_.insert(ret);
    _dbg_txid_lock_.unlock();
#endif
    return ret;
  }

  virtual void DoTxAsync(TxRequest &) = 0;
  virtual void SetNewLeader(parid_t, volatile locid_t*) { verify(0); };
  virtual void FailoverPauseSocketOut(parid_t, locid_t) { verify(0); };
  virtual void FailoverResumeSocketOut(parid_t, locid_t) { verify(0); };
  // Submit/BulkSubmit/assignCmd
  // take const janus::Command&; shared_ptr<Marshallable> callers
  // auto-convert via Command's implicit ctor.
  virtual void Submit(const janus::Command& cmd,
                      rusty::Function<void()> commit_callback = {},
                      rusty::Function<void()> exe_callback = {}) {
    verify(0);
  }

  virtual void assignCmd(const janus::Command& cmd){
    verify(0);
  }

  virtual void BulkSubmit(const janus::Command& cmd,
                           rusty::Function<void()> commit_callback = {},
                           rusty::Function<void()> exe_callback = {}){
    verify(0);
  }

  virtual void Reset() {
    committed_ = false;
    commit_reported_ = false;
    aborted_ = false;
    n_dispatch_ = 0;
    n_dispatch_ack_ = 0;
    // removed `n_prepare_req_ = 0;` — field gone.
    n_prepare_ack_ = 0;
    n_finish_req_ = 0;
    n_finish_ack_ = 0;
  }
  virtual uint64_t GenerateTimestamp() {
    uint64_t t;
    switch(Config::GetConfig()->timestamp_) {
      case Config::TimestampType::CLOCK:
        t = std::time(nullptr);
        t = t << 32;
        t |= (uint64_t) coo_id_;
        break;
      case Config::TimestampType::COUNTER:
        t = next_txn_id_.load();
        t = t << 32;
        t |= (uint64_t) coo_id_;
      default:
        verify(0);
    }
    return t;
  }
  virtual void restart(TxData *ch) {verify(0);};
  virtual void Restart() = 0;
  virtual void set_slot(int slot){
    verify(0);
  }
};

} // namespace janus
