#pragma once
#include "__dep__.h"
#include "constants.h"
#include "command.h"
#include "epochs.h"
#include "kvdb.h"
#include "procedure.h"
#include "view.h"
#include "tx.h"
#include "rcc/tx.h"
#include "classic/tpc_command.h"
#include "RW_command.h"
#include "config.h"
#include <chrono>

namespace janus {

struct UniqueCmdID {
  int32_t client_id_;
  int32_t cmd_id_;
};

class Distribution {
  double created_time_ = SimpleRWCommand::GetCurrentMsTime();
  double recent_100_sum_ = 0;
  // bool pct_lock = false;
 public:
  vector<double> data_;
  void append(double x) {
    // if (pct_lock) return;
    data_.push_back(x);
    recent_100_sum_ += x;
    if (data_.size() > 100)
      recent_100_sum_ -= data_[data_.size() - 101];
  }
  // only append if append_time is in mid 1/3 time (10~20s if duration is 30s)
  void mid_time_append(double x, double append_time) {
    // if (pct_lock) return;
    double duration_3_times = (append_time - created_time_) * 3;
    if (duration_3_times > Config::GetConfig()->duration_ * 1000 && duration_3_times < Config::GetConfig()->duration_ * 2 * 1000)
      data_.push_back(x);
  }
  // only append if append_time is in mid 1/3 time (10~20s if duration is 30s)
  void mid_time_append(double x) {
    // if (pct_lock) return;
    double append_time = SimpleRWCommand::GetCurrentMsTime();
    double duration_3_times = (append_time - created_time_) * 3;
    if (duration_3_times > Config::GetConfig()->duration_ * 1000 && duration_3_times < Config::GetConfig()->duration_ * 2 * 1000)
      data_.push_back(x);
  }
  void merge(Distribution &o) {
    for (int i = 0; i < o.count(); i++)
      data_.push_back(o.data_[i]);
  }
  size_t count() {
    return data_.size();
  }
  double recent_100_ave() { // only work when append only
    if (data_.size() == 0)
      return 0;
    if (data_.size() > 100)
      return recent_100_sum_ / 100;
    else
      return recent_100_sum_ / data_.size();
  }
  double pct(double pct) {
    verify(pct >= 0.0 - 1e-6 && pct <= 100.0 + 1e-6);
    // pct_lock = true;
    if (data_.size() == 0)
      return -1;
    sort(data_.begin(), data_.end());
    int pick = floor(data_.size() * pct);
    if (pick == data_.size())
      pick -= 1;
    return data_[pick];
  }
  double pct50() {
    return pct(0.5);
  }
  double pct90() {
    return pct(0.9);
  }
  double pct99() {
    return pct(0.99);
  }
  double ave() {
    if (data_.size() == 0)
      return -1;
    double sum = 0;
    for (int i = 0; i < data_.size(); i++)
      sum += data_[i];
    return sum / data_.size();
  }
  string statistics() {
    // snprintf instead of iomanip manipulators: under clang-22
    // `import std`, the <iomanip> operator<< overloads are not reliably
    // reachable in module TUs (same fix as rrr base/common.h).
    char buf[64];
    string out;
    snprintf(buf, sizeof(buf), "%7s%9zu", "count", count());
    out += buf;
    snprintf(buf, sizeof(buf), "%7s%9.2f", " 0pct", pct(0.0));
    out += buf;
    snprintf(buf, sizeof(buf), "%7s%9.2f", "50pct", pct(0.5));
    out += buf;
    snprintf(buf, sizeof(buf), "%7s%9.2f", "90pct", pct(0.9));
    out += buf;
    snprintf(buf, sizeof(buf), "%7s%9.2f", "99pct", pct(0.99));
    out += buf;
    snprintf(buf, sizeof(buf), "%7s%9.2f", "  ave", ave());
    out += buf;
    return out;
  }
  string distribution() {
    // snprintf instead of iomanip manipulators (see statistics()).
    char buf[32];
    string out;
    for (int i = 0; i <= 100; i += 10) {
      snprintf(buf, sizeof(buf), "%9.2f", pct(i / 100.0));
      out += buf;
    }
    return out;
  }
};

class Frequency {
  vector<int> keys_;
 public:
  void append(double x) {
    keys_.push_back(x);
  }
  void merge(Frequency &o) {
    for (int i = 0; i < o.count(); i++)
      keys_.push_back(o.keys_[i]);
  }
  size_t count() {
    return keys_.size();
  }
  string top_keys_pcts() {
    unordered_map<int, int> count_map;
    for (auto k: keys_) {
      count_map[k]++;
    }
    set<pair<int, int>> frequency;
    for (auto it: count_map) {
      frequency.insert(make_pair(-it.second, it.first));
    }
    std::stringstream ss;
    int i = 0;
    for (set<pair<int, int>>::iterator it = frequency.begin(); it != frequency.end() && i < 10; it++, i++) {
      // snprintf for the %.6f part instead of iomanip (see statistics()).
      char buf[48];
      snprintf(buf, sizeof(buf), "%.6f", -it->first * 100.0 / count());
      ss << buf << " (" << it->second << "), ";
    }
    return ss.str();
  }
};

// candidates_ migrated to
// `unordered_map<uint64_t, janus::Command>`.
// external API (push_back, cmd_to_recover)
// also takes/returns Command; shared_ptr<Marshallable> callers
// auto-convert via Command's implicit ctor.
class RevoveryCandidates {
  // <cmd_id, cmd>
  unordered_map<uint64_t, janus::Command> candidates_;
  unordered_map<uint64_t, bool> appeared_;
  int total_write_ = 0;
  uint64_t to_recover_id_ = -1;
 public:
  RevoveryCandidates() {}
  void push_back(uint64_t cmd_id, const janus::Command& cmd, bool is_write);
  bool remove(uint64_t cmd_id);
  bool has_appeared(uint64_t cmd_id);
  size_t size() const;
  int total_write();
  bool has_cmd_to_recover() const;
  janus::Command cmd_to_recover();
};

class Witness {
  // WitnessLog::cmd_ migrated from
  // shared_ptr<Marshallable> to janus::Command.  Conditional
  // WITNESS_LOG_DEBUG; never compiled in default builds.
  class WitnessLog {
   public:
    double time_;
    int operation_; // 0: push_back; 1: remove
    janus::Command cmd_;
    bool success_;
    int size_;
    WitnessLog(int operation, const janus::Command& cmd, bool success, int size):
      operation_(operation), cmd_(cmd), success_(success), size_(size) {
      time_ = SimpleRWCommand::GetCurrentMsTime();
    }
    void print(double init_time) {
      pair<int32_t, int32_t> cmd_id = SimpleRWCommand::GetCmdID(cmd_);
      uint64_t cmd_id_combined = SimpleRWCommand::GetCombinedCmdID(cmd_);
      if (operation_ == 0) {
        Log_info("Log {:.2f} size {} suc {} key {} push_back {} {} {}", time_ - init_time, size_, success_, SimpleRWCommand::GetKey(cmd_), cmd_id.first, cmd_id.second, cmd_id_combined);
      } else if (operation_ == 1) {
        Log_info("Log {:.2f} size {} suc {} key {} remove {} {} {}", time_ - init_time, size_, success_, SimpleRWCommand::GetKey(cmd_), cmd_id.first, cmd_id.second, cmd_id_combined);
      } else {
        verify(0);
      }
    }
  };
  // removed `bool belongs_to_leader_{false};`
  // and `void set_belongs_to_leader(bool);` — the field was already
  // commented `// discard`; it was set in 3 callers
  // (`fpga_raft/server.h:75` and `copilot/server.cc:26`) but never read
  // by anything. The calls
  // to the setter were removed alongside the field.
  int witness_size_ = 0;
  Distribution witness_size_distribution_;

#ifdef WITNESS_LOG_DEBUG
  vector<WitnessLog> witness_log_;
#endif
 public:
  unordered_map<key_t, RevoveryCandidates> candidates_;
  /* Recover related begin */
  ballot_t max_seen_ballot_ = -1, max_accepted_ballot_ = -1;
  int sid_ = -1, set_size_ = 0;
  // removed `bool committed_ = false;` —
  // set by `TxLogServer::OnJetpackCommit` at scheduler.cc:1437 but
  // never read anywhere.  The matching `sid_` / `set_size_` fields
  // it was set alongside ARE read at scheduler.cc:1388-1389, so
  // those stay; only `committed_` was dead state.
  /* Recover related end */

  Witness() {};
  ~Witness() {};
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  // return whether meet conflict, but not whether push_back success
  bool push_back(const janus::Command& cmd);
  // return how many cmd have been removed (cmd may be CMD_TPC_BATCH)
  int remove(const janus::Command& cmd);
  // return whether all cmds appeared before
  bool has_appeared(const janus::Command& cmd);
  // return 50pct, 90pct, 99pct, ave of the witness_size_distribution_
  std::vector<double> witness_size_distribution();
  /* Recover related begin */
  bool has_cmd_to_recover(key_t key) {
    return candidates_[key].has_cmd_to_recover();
  }
  // returns Command;
  // shared_ptr<Marshallable> callers auto-convert via implicit ctor.
  janus::Command cmd_to_recover(key_t key) {
    return candidates_[key].cmd_to_recover();
  }
  rusty::Arc<VecRecData> id_set();
  void reset();
  /* Recover related end */
#ifdef WITNESS_LOG_DEBUG
  void print_log();
#endif
};

class RecentAverage {
  vector<double> data_;
  int size_, pointer_ = 0;
  double sum = 0;
  bool filled_once_ = false;
 public:
  RecentAverage(int size): size_(size) {
    // intentionally left blank
  }
  void append(double x) {
    if (!filled_once_) {
      data_.push_back(x);
      pointer_++;
    } else {
      sum -= data_[pointer_];
      data_[++pointer_] = x;
    }
    sum += x;
    if (pointer_ == size_) {
      pointer_ = 0;
      filled_once_ = true;
    }
  }
  bool filled_once() {
    return filled_once_;
  }
  double ave() {
    // Log_info("RecentAverage ave {} {}", filled_once_, pointer_);
    verify(filled_once_ || pointer_ > 0);
    return filled_once_ ? sum / size_ : sum / pointer_;
  }
};

// removed dead `struct ResponseData`
// (~30 LOC) — declared with `responses_`, `max_cmd_`, `accept_count_` etc.
// fields plus `append_response()` and `GetMaxCmd()` methods, but never
// instantiated, referenced, or constructed anywhere in the tree.

// rec_set_ migrated from
// `vector<shared_ptr<Marshallable>>` to `vector<janus::Command>`.
// external API now also uses Command;
// shared_ptr<Marshallable> callers auto-convert via Command's
// implicit ctor.
class RecoverySet {
  std::unordered_map<int, std::vector<janus::Command>> rec_set_;
 public:
  void insert(int sid, int rid, const janus::Command& cmd) {
    if (rec_set_[sid].size() <= rid) {
        rec_set_[sid].resize(rid + 1);
    }
    rec_set_[sid][rid] = cmd;
  }
  const janus::Command& get(int sid, int rid) {
    if (rec_set_[sid].size() <= rid) {
        rec_set_[sid].resize(rid + 1);
    }
    return rec_set_[sid][rid];
  }
};

// View class is defined in view.h



// removed `struct CommitNotification` —
// declared with 7 fields (client_stored_, committed_, commit_result_,
// commit_callback_, coordinator_stored_, coordinator_commit_result_,
// coordinator_replied_, receive_time_) but never instantiated
// anywhere.  `grep "CommitNotification\s+"` returned only the
// definition itself; the matches on `testSpeculativeCommitNotification`
// / `testDurableCommitNotification` are unrelated test method names.

class TxnRegistry;
class Executor;
class Coordinator;
class Frame;
class Communicator;
class TxLogServer {
 public:

  /* Some Jetpack elements begin */
  enum JetpackStatus {RECOVERY, READY};
  int jetpack_status_ = JetpackStatus::READY;
  epoch_t jepoch_, oepoch_;
  View old_view_, new_view_;
  int sid, rid, sid_cnt_ = 0;
  RecoverySet rec_set_;
  // removed `bool simulated_fail_ = false;`
  // — declared but never written or read anywhere.
  std::chrono::steady_clock::time_point jetpack_recovery_start_time_{};
  /* Some Jetpack elements end */

  void *svr_workers_g{nullptr};

  locid_t loc_id_ = -1;
  siteid_t site_id_ = -1;
  unordered_map<txid_t, shared_ptr<Tx>> dtxns_{};
  unordered_map<txid_t, mdb::Txn *> mdb_txns_{};
  unordered_map<txid_t, Executor *> executors_{};

  // app_next_ now takes janus::Command (not
  // shared_ptr<Marshallable>) so user code is one type level removed
  // from the rrr framework's wire-boundary shared_ptr.  MarshallDeputy
  // ctors are non-explicit, so callers passing `shared_ptr<Marshallable>`
  // (e.g. `instance->log_`) auto-convert at the call site.
  function<int(int, janus::Command)> app_next_{};
  // removed
  //   `function<shared_ptr<vector<MultiValue>>(Marshallable&)> key_deps_{};`
  // — declared but never written or invoked anywhere.

  shared_ptr<mdb::TxnMgr> mdb_txn_mgr_{};
  int mode_;
  // removed `Recorder *recorder_ = nullptr;`
  // — only assignment was a commented-out
  // `recorder_ = new Recorder(path);` in `scheduler.cc::SetupTransport`,
  // and the only readers were `dtxn->recorder_ = this->recorder_;`
  // propagation lines in `scheduler.cc::CreateRccDtxn` /
  // `CreateTx` (also gone in this phase).  Field always nullptr.
  Frame *frame_ = nullptr;
  Frame *rep_frame_ = nullptr;
  TxLogServer *tx_sched_ = nullptr;
  TxLogServer *rep_sched_ = nullptr;
  Communicator *commo_{nullptr};
  //  Coordinator* rep_coord_ = nullptr;
  shared_ptr<TxnRegistry> txn_reg_{nullptr};
  parid_t partition_id_{};
  std::recursive_mutex mtx_{};

  bool epoch_enabled_{false};
  EpochMgr epoch_mgr_{};
  std::time_t last_upgrade_time_{0};
  map<parid_t, map<siteid_t, epoch_t>> epoch_replies_{};
  bool in_upgrade_epoch_{false};
  const int EPOCH_DURATION = 5;

  // removed `bool paused_ = false;` — set
  // in `TxLogServer::Pause()` / `Resume()` (scheduler.cc:333, 338)
  // alongside `commo_->Pause()` / `commo_->Resume()` calls but
  // never read.  The Pause/Resume methods themselves are kept
  // (called from `server_worker.cc:329, 335` and `service.cc:371,
  // 396`) since the `commo_->Pause()` side effect is real; only the
  // dead `paused_` writes were removed.

  // State machine recovery tracking
  bool in_state_machine_recovery_{false};
  size_t transactions_recovered_{0};

  // @safe - Check if recovery is in progress
  bool IsRecovering() const { return in_state_machine_recovery_; }

  // @safe - Get count of recovered transactions
  size_t GetRecoveredTransactionCount() const { return transactions_recovered_; }

  // @unsafe - Set recovery mode and log when complete
  void SetRecoveryMode(bool recovering);

#ifdef CHECK_ISO
  typedef map<Row*, map<colid_t, int>> deltas_t;
  deltas_t deltas_{};

  void MergeDeltas(deltas_t deltas) {
    verify(deltas.size() > 0);
    for (auto& pair1: deltas) {
      Row* r = pair1.first;
      for (auto& pair2: pair1.second) {
        colid_t c = pair2.first;
        int delta = pair2.second;
        deltas_[r][c] += delta;
        int v = r->get_column(c).get_i32();
        int x = deltas_[r][c];
      }
    }
    deltas.clear();
  }

  void CheckDeltas() {
    for (auto& pair1: deltas_) {
      Row* r = pair1.first;
      for (auto& pair2: pair1.second) {
        colid_t c = pair2.first;
        int delta = pair2.second;
        int v = r->get_column(c).get_i32();
        verify(delta == v);
      }
    }
  }
#endif

  Communicator *commo() {
    verify(commo_ != nullptr);
    return commo_;
  }

  TxLogServer();
  TxLogServer(int mode);
  virtual ~TxLogServer();


  virtual void SetPartitionId(parid_t par_id) {
    partition_id_ = par_id;
  }

  // runs in a coroutine.

  virtual bool HandleConflicts(Tx &dtxn,
                               innid_t inn_id,
                               vector<string> &conflicts) {
    return false;
  };
  virtual bool HandleConflicts(Tx &dtxn,
                               innid_t inn_id,
                               vector<conf_id_t> &conflicts) {
    Log_fatal("unimplemnted feature: handle conflicts!");
    return false;
  };
  virtual void Execute(Tx &txn_box,
                       innid_t inn_id);

  Coordinator *CreateRepCoord(const i64& dep_id=0);
  virtual shared_ptr<Tx> GetTx(txnid_t tx_id);
  virtual shared_ptr<Tx> CreateTx(txnid_t tx_id,
                                  bool ro = false);
  virtual shared_ptr<Tx> CreateTx(epoch_t epoch,
                                  txnid_t txn_id,
                                  bool read_only = false);
  virtual shared_ptr<Tx> GetOrCreateTx(txnid_t tid, bool ro = false);
  // @unsafe - Manages transaction lifecycle, calls external methods
  void DestroyTx(i64 tid);

  virtual void DestroyExecutor(txnid_t txn_id);

  inline int get_mode() { return mode_; }

  // Below are function calls that go deeper into the mdb.
  // They are merged from the called TxnRunner.

  inline mdb::Table
  *get_table(const string &name) {
    return mdb_txn_mgr_->get_table(name);
  }

  virtual mdb::Txn *GetMTxn(const i64 tid);
  virtual mdb::Txn *GetOrCreateMTxn(const i64 tid);
  virtual mdb::Txn *RemoveMTxn(const i64 tid);

  // removed `get_prepare_log` declaration —
  // see scheduler.cc retirement comment.

  // TODO: (Shuai: I am not sure this is supposed to be here.)
  // I think it used to initialized the database?
  // So it should be somewhere else?
  void reg_table(const string &name,
                 mdb::Table *tbl
  );

  // takes janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  virtual int32_t Dispatch(cmdid_t cmd_id,
                        const janus::Command& cmd,
                        TxnOutput& ret_output,
                        std::shared_ptr<ViewData>& view_data) {
    verify(0);
    return REJECT;
  }

  // take a `function<int(int, janus::Command)>`.
  // Lambdas registered here see the deputy directly; if they need the
  // legacy shared_ptr they can call `md.inner()`, or use the
  // `marshallable_cast<T>(md)` overload to downcast to a concrete type.
  void RegLearnerAction(function<int(int, janus::Command)> learner_action) {
    app_next_ = learner_action;
  }

  // take janus::Command (matches RegLearnerAction
  // signature above).  Body uses `md.inner()` / `marshallable_cast<T>(md)`
  // to access the underlying typed payload.
  virtual int Next(int, janus::Command md) { verify(0); };
  /**
   * Check if the command is already committed
   * @param commit_cmd command to be checked
   * @return true if it's already committed, false otherwise
   */
  virtual bool CheckCommitted(const janus::Command& commit_cmd) { verify(0); }

  // removed `virtual void Next(Marshallable&)
  // { verify(0); }` — declared on the base but never overridden in
  // any subclass and never called.  The live virtual is the
  // `Next(int, shared_ptr<Marshallable>)` overload above.

	virtual void Setup() { verify(0); } ;
  virtual bool IsLeader() {
    if (rep_sched_) {
      return rep_sched_->IsLeader();
    }
    return false;
  }
  // @unsafe
  virtual bool IsFPGALeader() { verify(0); } ;
	virtual bool RequestVote() { verify(0); return false;};
  virtual void Pause();
  virtual void Resume();

  // epoch related functions
  void TriggerUpgradeEpoch();
  void UpgradeEpochAck(parid_t par_id, siteid_t site_id, int res);
  virtual int32_t OnUpgradeEpoch(uint32_t old_epoch);
  
  // application k-v table for rw workload
  unordered_map<key_t, value_t> kv_table_;


  // For checksum
  unordered_map<key_t, value_t> database_;
  int database_operation_count_ = 0;

  // takes janus::Command;
  // unwraps to shared_ptr<Marshallable> at the boundary into
  // SimpleRWCommand which still takes the legacy shape.
  void ApplyToDatabase(const janus::Command& cmd) {
    SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
    // Log_info("Apply Write {} key {} value {}", parsed_cmd.IsWrite(), parsed_cmd.key_, parsed_cmd.value_);
    if (parsed_cmd.IsWrite()) {
      database_[parsed_cmd.key_] = parsed_cmd.value_;
      database_operation_count_++;
    }
  }

  uint32_t ChecksumXor() {
    Log_info("database_operation_count_ {}", database_operation_count_);
    uint32_t checksum = 0;
    for (const auto& kv : database_) {
        checksum ^= static_cast<uint32_t>(kv.first);
        checksum ^= static_cast<uint32_t>(kv.second);
    }
    return checksum;
  }

  // This used for garbage collection / evaluation data structure grows over time
  void PrintStructureSize();

  // below are about rule

  Witness witness_;

  // For Rule usage
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void OnRuleSpeculativeExecute(const janus::Command& cmd,
                                bool_t* accepted,
                                value_t* result,
                                bool_t* is_leader);

  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void OriginalPathUnexecutedCmdConflictPlaceHolder(const janus::Command& cmd);

  // @unsafe
  // takes janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void RuleWitnessGC(const janus::Command& cmd);

#ifdef ZERO_OVERHEAD
  // takes janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  virtual bool ConflictWithOriginalUnexecutedLog(const janus::Command& cmd) {
    // This function should be overrided by the deriviated class (replica server)
    assert(0);
    return false;
  }
#endif

  // @unsafe
  void JetpackRecoveryEntry();

  void JetpackBeginRecovery();

  void JetpackRecovery();

  void JetpackPrepare(int sid, int set_size);

  void JetpackAccept(int sid, int set_size);

  void JetpackCommit(int sid, int set_size);

  void JetpackResubmit(int sid, int set_size);
  // take janus::Command;
  // shared_ptr<Marshallable> callers auto-convert via Command's
  // implicit ctor.
  void DispatchRecoveredCommand(const janus::Command& cmd, rusty::Option<rusty::Arc<IntEvent>> recovery_event = rusty::None);
  
  void OnJetpackBeginRecovery(const janus::Command& old_view,
                              const janus::Command& new_view, 
                              const epoch_t& new_view_id);
  
  void OnJetpackPullIdSet(const epoch_t& jepoch,
                          const epoch_t& oepoch,
                          bool_t* ok,
                          epoch_t* reply_jepoch,
                          epoch_t* reply_oepoch,
                          janus::Command* reply_old_view,
                          janus::Command* reply_new_view,
                          VecRecData& id_set);
  
  // @unsafe
  virtual void OnJetpackPullCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const std::vector<key_t>& keys,
                        bool_t* ok, 
                        epoch_t* reply_jepoch, 
                        epoch_t* reply_oepoch,
                        janus::Command* reply_old_view,
                        janus::Command* reply_new_view,
                        KeyCmdBatchData& batch);
  
  void OnJetpackRecordCmd(const epoch_t& jepoch, 
                          const epoch_t& oepoch, 
                          const int32_t& sid, 
                          const int32_t& rid, 
                          const KeyCmdBatchData& batch);
  
  void OnJetpackPrepare(const epoch_t& jepoch, 
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
                        int32_t* replied_set_size);
  
  void OnJetpackAccept(const epoch_t& jepoch, 
                       const epoch_t& oepoch, 
                       const ballot_t& max_seen_ballot, 
                       const int32_t& sid, 
                       const int32_t& set_size,
                       bool_t* ok,
                       epoch_t* reply_jepoch,
                       epoch_t* reply_oepoch,
                       janus::Command* reply_old_view,
                       janus::Command* reply_new_view,
                       ballot_t* reply_max_seen_ballot);
  
  void OnJetpackCommit(const epoch_t& jepoch, 
                       const epoch_t& oepoch, 
                       const int32_t& sid, 
                       const int32_t& set_size);
  
  // dropped trailing dead
  // `shared_ptr<Marshallable> cmd` parameter — was assigned by the
  // body but passed by value, so the assignment never propagated to
  // the caller.  Caller in service.cc::JetpackPullRecSetIns retains
  // its `cmd->set_marshallable(empty TpcCommitCommand)` pre-fill as
  // the actual wire-out value.
  void OnJetpackPullRecSetIns(const epoch_t& jepoch,
                              const epoch_t& oepoch,
                              const int32_t& sid,
                              const int32_t& rid,
                              bool_t* ok,
                              epoch_t* reply_jepoch,
                              epoch_t* reply_oepoch,
                              janus::Command* reply_old_view,
                              janus::Command* reply_new_view);
  
  void OnJetpackFinishRecovery(const epoch_t& oepoch);


};

} // namespace janus
