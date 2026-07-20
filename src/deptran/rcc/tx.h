#pragma once
#include "../__dep__.h"
#include "graph.h"
#include "../tx.h"
#include "../scheduler.h"
#include "../procedure.h"

#define PHASE_RCC_DISPATCH (1)
#define PHASE_RCC_COMMIT (2)

#define SKIP_I (false)
#define SKIP_D (false)

namespace janus {

class RccTx: public Tx, public Vertex<RccTx> {
 public:
  class StatusBox : public BoxEvent<int> {
   public:
    int Get() {
      verify(is_set_.get());
      return BoxEvent<int>::get();
    }
    void Set(const int& x) {
//      verify(x != REJECT);
      BoxEvent<int>::set(x);
    }
  };
  bool mocking_janus_{false};

  void __DebugCheckParents(rank_t rank);
  void __DebugCheckScc(rank_t rank);
  enum TraverseStatus {ERROR=0, TRAVERSING=1, WAITING_NO_DEADLOCK=2, WAITING_POSSIBLE_DEADLOCK=3, DONE=4};
  static SpinLock __debug_parents_lock_;
  static SpinLock __debug_scc_lock_;
  static std::map<txid_t, parent_set_t> __debug_parents_i_;
  static std::map<txid_t, parent_set_t> __debug_parents_d_;
  static std::map<txid_t, vector<txid_t>> __debug_scc_i_;
  static std::map<txid_t, vector<txid_t>> __debug_scc_d_;
  static thread_local uint64_t timestamp_i_s;
  static thread_local uint64_t timestamp_d_s;
  static uint64_t& timestamp(rank_t rank) {
    verify(rank == RANK_I || rank == RANK_D);
    if (rank == RANK_I) {
      return timestamp_i_s;
    } else {
      return timestamp_d_s;
    }
  };

  static std::map<txid_t, parent_set_t>& __debug_parents(rank_t rank) {
    if (rank == RANK_I) {
      return __debug_parents_i_;
    } else {
      return __debug_parents_d_;
    }
  };
  static std::map<txid_t, vector<txid_t>>& __debug_scc(rank_t rank) {
    if (rank == RANK_I) {
      return __debug_scc_i_;
    } else {
      return __debug_scc_d_;
    }
  };

  // ----below variables get reset whenever current_rank_ is changed
//  bool __debug_commit_received_{false};
//  bool __debug_entered_committing_{false};
//  SharedIntEvent status_{};
//  ballot_t max_seen_ballot_{0};
//  ballot_t max_accepted_ballot_{0};
//  SharedIntEvent commit_received_{};
//  SharedIntEvent log_apply_finished_{};
//  RccTx* traverse_path_start_{nullptr};
//  RccTx* traverse_path_waitingon_{nullptr};
//  TraverseStatus traverse_path_waiting_status_{ERROR};
//  shared_ptr<IntEvent> sp_ev_commit_{Reactor::create_sp_event<IntEvent>()};
//  TxnOutput *p_output_reply_ = nullptr;
//  TxnOutput output_ = {};
//  bool log_apply_started_{false}; // compared to ???
//  // also reset phase_ and fully_dispatched.
//  bool need_validation_{false};
//  bool waiting_all_anc_committing_{false};
//  SharedIntEvent wait_all_anc_commit_done_{};
//  bool __debug_local_validated_foreign_{false};
//  shared_ptr<StatusBox> local_validated_{Reactor::create_sp_event<StatusBox>()};
//  shared_ptr<StatusBox> global_validated_{Reactor::create_sp_event<StatusBox>()};
//  vector<SimpleCommand> dreqs_ = {};
//  // ----above variables get reset whenever current_rank_ is changed


  class RccTxStatus {
   public:
    int value_{};
//  vector<shared_ptr<IntEvent>> events_{};
    vector<shared_ptr<IntEvent>> events_{}; // waiting for commits
    void set(const int& v) {
      value_ = v;
      if (v >= TXN_CMT) {
        for (auto& sp_ev: events_) {
          verify(sp_ev->target_.get() == TXN_CMT);
          sp_ev->set(v);
        }
        events_.clear();
      }
      return;
    }

    void wait_until_gte(int x) {
      verify(x == TXN_CMT);
      if (value_ >= x) {
        return;
      }
      auto sp_ev =  Reactor::create_sp_event<IntEvent>();
      sp_ev->value_.set(value_);
      sp_ev->target_.set(x);
      events_.push_back(sp_ev);
//  sp_ev->wait_timeout(1000*1000*1000);
//  verify(sp_ev->status_ != EventStatus::TIMEOUT);
      sp_ev->wait();
    }
  };

  class RccSubTx {
   public:
    bool __debug_commit_received_{false};
    bool __debug_entered_committing_{false};
    RccTxStatus status_{};
    ballot_t max_seen_ballot_{0};
    ballot_t max_accepted_ballot_{0};
    SharedIntEvent commit_received_{};
    SharedIntEvent log_apply_finished_{};
    RccTx* traverse_path_start_{nullptr};
    RccTx* traverse_path_waitingon_{nullptr};
    TraverseStatus traverse_path_waiting_status_{ERROR};
    shared_ptr<IntEvent> sp_ev_commit_{Reactor::create_sp_event<IntEvent>()};
    TxnOutput *p_output_reply_ = nullptr;
    TxnOutput output_ = {};
    bool log_apply_started_{false}; // compared to ???
    // also reset phase_ and fully_dispatched.
    bool need_validation_{false};
    bool waiting_all_anc_committing_{false};
    SharedIntEvent wait_all_anc_commit_done_{};
    bool __debug_local_validated_foreign_{false};
    shared_ptr<StatusBox> local_validated_{Reactor::create_sp_event<StatusBox>()};
    shared_ptr<StatusBox> global_validated_{Reactor::create_sp_event<StatusBox>()};
    shared_ptr<IntEvent> fully_dispatched_{Reactor::create_sp_event<IntEvent>()};
//  bool fully_dispatched_{false};
    vector<SimpleCommand> dreqs_ = {};
    // hopefully this makes involve checks faster
    enum InvolveEnum {UNKNOWN, INVOLVED, FOREIGN};
    InvolveEnum involve_flag_{UNKNOWN};

    std::set<uint32_t> partition_;
    std::vector<uint64_t> pieces_;
    bool during_commit = false;
    bool during_asking = false;
    bool inquire_acked_ = false;
    bool all_anc_cmt_hint{false};
    bool all_nonscc_parents_executed_hint{false};
    bool scc_all_nonscc_parents_executed_hint_{false};


    virtual bool UpdateStatus(int s) {
      if (status_.value_ < s) {
        status_.set(status_.value_ | s);
        if (status_.value_ >= TXN_CMT) {
//          __DebugCheckParents();
        }
        return true;
      } else {
        return false;
      }
    }


    inline int8_t status() const {
      return status_.value_;
    }

    inline bool IsAborted() const {
      return (status_.value_ & TXN_ABT);
    }

    inline bool IsCommitting() const {
//    return (status_.value_ & TXN_CMT);
      return (status_.value_ >= TXN_CMT);
    }

    inline bool IsDecided() const {
      return (status_.value_ & TXN_DCD);
    }

    inline bool HasLogApplyStarted() const {
//    if (executed_) {
//      verify(IsDecided());
//    }
      return log_apply_started_;
    }

    inline void union_status(int8_t status,
                             bool is_trigger = false,
                             bool is_server = false) {

      if (false) {
        // TODO
        // I cannot change the status of this txn.
      } else {
#ifdef DEBUG_CODE
        verify((status_ | status) >= status_);
#endif
        status_.set(status_.value_ |= status);

      }
    }

    bool Involve(parid_t id) {
      if (involve_flag_ == INVOLVED)
        return true;
      if (involve_flag_ == FOREIGN)
        return false;
//    verify(partition_.size() > 0);
      auto it = partition_.find(id);
      if (it == partition_.end()) {
        if (IsCommitting()) {
          involve_flag_ = FOREIGN;
        }
        return false;
      } else {
        if (IsCommitting()) {
          involve_flag_ = INVOLVED;
        }
        return true;
      }
    }

    uint32_t random_server() {
      verify(partition_.size() > 0);
      uint32_t i = RandomGenerator::rand(0, partition_.size() - 1);
      auto it = partition_.begin();
      std::advance(it, i);
      uint32_t id = *(partition_.begin());
      Log::debug("random a related server, id: %x", id);
      return id;
    }

  };

  RccSubTx subtx_i_{};
  RccSubTx subtx_d_{};

  RccSubTx& subtx(int rank) {
    verify(rank == RANK_D || rank == RANK_I);
    if (rank == RANK_D) {
      return subtx_d_;
    } else {
      return subtx_i_;
    }
  }

  bool read_only_ = false;
  map<Row *, map<colid_t, mdb::version_t>> read_vers_{};


//  bool InvolveThisShard() {
//    if (involve_flag_ == UNKNOWN) {
//      verify(status() >= TXN_CMT);
//      if (Involve(TxLogServer::partition_id_)) {
//        involve_flag_ = INVOLVED;
//      } else {
//        involve_flag_ = FOREIGN;
//      }
//    }
//    return (involve_flag_ == INVOLVED);
//  }

  // if any other transactions is blocked by this transaction,
  // add it to this set to make the checking waitlist faster.
  set<RccTx*> to_checks_{};

  RccTx() = delete;
  RccTx(txnid_t id);
  RccTx(RccTx& rhs_dtxn);
  RccTx(epoch_t, txnid_t tid, TxLogServer *mgr, bool ro);

  ~RccTx() noexcept override {
  }

  virtual void DispatchExecute(SimpleCommand &cmd,
                               map<int32_t, Value> *output);

  void CommitValidate(rank_t rank);
  virtual void CommitExecute(int rank);

  virtual void Abort();

  bool ReadColumn(mdb::Row *row,
                  mdb::colid_t col_id,
                  Value *value,
                  int hint_flag) override;

  bool WriteColumn(Row *row,
                   colid_t col_id,
                   const Value &value,
                   int hint_flag = TXN_INSTANT) override;

  void TraceDep(Row* row, colid_t col_id, rank_t hint_flag);
  void CommitDep(Row& row, colid_t col_id);

  virtual void AddParentEdge(shared_ptr<RccTx> other, rank_t rank) override;

  virtual void start_ro(const SimpleCommand&,
                        map<int32_t, Value> &output,
                        rusty::Function<void()> done);


  virtual mdb::Row *CreateRow(
      const mdb::Schema *schema,
      const std::vector<mdb::Value> &values) override {
    return RccRow::create(schema, values);
  }

 public:
  txnid_t id() override {
    return tid_;
  }


  bool operator<(const RccTx &rhs) const {
    return tid_ < rhs.tid_;
  }

  inline void set_id(uint64_t id) {
    tid_ = id;
  }


};

typedef vector<pair<txid_t, ParentEdge<RccTx>>> parent_set_t;


// archive operators for ParentEdge<RccTx>. Used by rcc_rpc.h's
// archive emission to serialize parent_set_t fields (a typedef for
// vector<pair<txid_t, ParentEdge<RccTx>>>). (Marshal-form mirrors
// deleted — Marshal-deprecation slice A.)
inline void serialize(const ParentEdge<RccTx> &e, rrr::BinaryWriteArchive& ar) {
  rrr::Serialize_::serialize(e.partitions_, ar);
}

inline rrr::BinaryWriteArchive& operator<<(rrr::BinaryWriteArchive& ar, const ParentEdge<RccTx> &e) { serialize(e, ar); return ar; }

inline void deserialize(ParentEdge<RccTx> &e, rrr::BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(e.partitions_, ar);
}

inline rrr::BinaryReadArchive& operator>>(rrr::BinaryReadArchive& ar, ParentEdge<RccTx> &e) { deserialize(e, ar); return ar; }

} // namespace janus
