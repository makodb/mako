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
    std::ostringstream oss;
    oss << std::setw(7) << "count" << std::setw(9) << count();
    oss << std::setw(7) << " 0pct" << std::setw(9) << std::fixed << std::setprecision(2) << pct(0.0);
    oss << std::setw(7) << "50pct" << std::setw(9) << std::fixed << std::setprecision(2) << pct(0.5);
    oss << std::setw(7) << "90pct" << std::setw(9) << std::fixed << std::setprecision(2) << pct(0.9);
    oss << std::setw(7) << "99pct" << std::setw(9) << std::fixed << std::setprecision(2) << pct(0.99);
    oss << std::setw(7) << "  ave" << std::setw(9) << std::fixed << std::setprecision(2) << ave();
    return oss.str();
  }
  string distribution() {
    std::ostringstream oss;
    for (int i = 0; i <= 100; i += 10) {
      // oss << i << "pct ";
      oss << std::setw(9) << std::fixed << std::setprecision(2) << pct(i / 100.0);
    }
    return oss.str();
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
      ss << std::fixed << std::setprecision(6) << -it->first * 100.0 / count() << " (" << it->second << "), ";
    }
    return ss.str();
  }
};

class RevoveryCandidates {
  // <cmd_id, cmd>
  unordered_map<uint64_t, shared_ptr<Marshallable>> candidates_;
  unordered_map<uint64_t, bool> appeared_;
  int total_write_ = 0;
  uint64_t to_recover_id_ = -1;
 public:
  RevoveryCandidates() {}
  void push_back(uint64_t cmd_id, shared_ptr<Marshallable> cmd, bool is_write);
  bool remove(uint64_t cmd_id);
  bool has_appeared(uint64_t cmd_id);
  size_t size() const;
  int total_write();
  bool has_cmd_to_recover() const;
  shared_ptr<Marshallable> cmd_to_recover();
};

class Witness {
  class WitnessLog {
   public:
    double time_;
    int operation_; // 0: push_back; 1: remove
    shared_ptr<Marshallable> cmd_;
    bool success_;
    int size_;
    WitnessLog(int operation, shared_ptr<Marshallable> cmd, bool success, int size):
      operation_(operation), cmd_(cmd), success_(success), size_(size) {
      time_ = SimpleRWCommand::GetCurrentMsTime();
    }
    void print(double init_time) {
      pair<int32_t, int32_t> cmd_id = SimpleRWCommand::GetCmdID(cmd_);
      uint64_t cmd_id_combined = SimpleRWCommand::GetCombinedCmdID(cmd_);
      if (operation_ == 0) {
        Log_info("Log %.2f size %d suc %d key %" PRId32 " push_back %" PRId32 " %" PRId32 " %" PRId64, time_ - init_time, size_, success_, SimpleRWCommand::GetKey(cmd_), cmd_id.first, cmd_id.second, cmd_id_combined);
      } else if (operation_ == 1) {
        Log_info("Log %.2f size %d suc %d key %" PRId32 " remove %" PRId32 " %" PRId32 " %" PRId64, time_ - init_time, size_, success_, SimpleRWCommand::GetKey(cmd_), cmd_id.first, cmd_id.second, cmd_id_combined);
      } else {
        verify(0);
      }
    }
  };
  // Workstream N Phase 4e-7: removed `bool belongs_to_leader_{false};`
  // and `void set_belongs_to_leader(bool);` — the field was already
  // commented `// discard`; it was set in 3 callers
  // (`mencius/server.h:48`, `fpga_raft/server.h:75`,
  // `copilot/server.cc:26`) but never read by anything.  The 3 calls
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
  // Workstream N Phase 4e-7: removed `bool committed_ = false;` —
  // set by `TxLogServer::OnJetpackCommit` at scheduler.cc:1437 but
  // never read anywhere.  The matching `sid_` / `set_size_` fields
  // it was set alongside ARE read at scheduler.cc:1388-1389, so
  // those stay; only `committed_` was dead state.
  /* Recover related end */

  Witness() {};
  ~Witness() {};
  // return whether meet conflict, but not whether push_back success
  bool push_back(const shared_ptr<Marshallable>& cmd);
  // return how many cmd have been removed (cmd may be CMD_TPC_BATCH)
  int remove(const shared_ptr<Marshallable>& cmd);
  // return whether all cmds appeared before
  bool has_appeared(const shared_ptr<Marshallable>& cmd);
  // return 50pct, 90pct, 99pct, ave of the witness_size_distribution_
  std::vector<double> witness_size_distribution();
  /* Recover related begin */
  bool has_cmd_to_recover(key_t key) {
    return candidates_[key].has_cmd_to_recover();
  }
  shared_ptr<Marshallable> cmd_to_recover(key_t key) {
    return candidates_[key].cmd_to_recover();
  }
  shared_ptr<VecRecData> id_set();
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
    // Log_info("RecentAverage ave %d %d", filled_once_, pointer_);
    verify(filled_once_ || pointer_ > 0);
    return filled_once_ ? sum / size_ : sum / pointer_;
  }
};

struct ResponseData {
  // pair<ver_t, ver_t> pos_of_this_pack;
  map<pair<int, int>, vector<shared_ptr<Marshallable> > >responses_;
  shared_ptr<Marshallable> max_cmd_{nullptr};
  int received_count_ = 0, accept_count_ = 0, max_accept_count_ = 0;
  double first_seen_time_ = 0;
  bool done_{false};
  pair<int, int> append_response(const shared_ptr<Marshallable>& cmd) {
    shared_ptr<VecPieceData> vec_piece;
    if (cmd->kind_ == TpcCommitCommand::static_kind()) { // original through tx svr
      shared_ptr<TpcCommitCommand> tpc_cmd = marshallable_cast<TpcCommitCommand>(cmd);
      verify(tpc_cmd != nullptr);
      vec_piece = marshallable_cast<VecPieceData>(tpc_cmd->cmd_);
    } else if (cmd->kind_ == VecPieceData::static_kind()) { // jetpack broadcast
      vec_piece = marshallable_cast<VecPieceData>(cmd);
    } else {
      verify(0);
    }
    verify(vec_piece != nullptr);
    shared_ptr<CmdData> md = vec_piece->sp_vec_piece_data_->at(0);
    pair<int, int> cmd_id = {md->client_id_, md->cmd_id_in_client_};
    responses_[cmd_id].push_back(cmd);
    accept_count_++;
    if (responses_[cmd_id].size() > max_accept_count_) {
      max_accept_count_ = responses_[cmd_id].size();
      max_cmd_ = cmd;
    }
    return {accept_count_, max_accept_count_};
  }
  shared_ptr<Marshallable> GetMaxCmd() {
    return max_cmd_;
  }
};

// Workstream N L10f-prep6e (2026-05-03): rec_set_ migrated from
// `vector<shared_ptr<Marshallable>>` to `vector<janus::Command>`.
// External API (insert / get) retains the legacy
// shared_ptr<Marshallable> shape for caller-side compatibility.
class RecoverySet {
  std::unordered_map<int, std::vector<janus::Command>> rec_set_;
 public:
  void insert(int sid, int rid, shared_ptr<Marshallable> cmd) {
    if (rec_set_[sid].size() <= rid) {
        rec_set_[sid].resize(rid + 1);
    }
    rec_set_[sid][rid] = cmd;  // Command::operator=(shared_ptr<Marshallable>)
  }
  shared_ptr<Marshallable> get(int sid, int rid) {
    if (rec_set_[sid].size() <= rid) {
        rec_set_[sid].resize(rid + 1);
    }
    return rec_set_[sid][rid].inner_marshallable();
  }
};

// View class is defined in view.h



// Workstream N Phase 4e-9: removed `struct CommitNotification` —
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
  // Workstream N Phase 4e-9: removed `bool simulated_fail_ = false;`
  // — declared but never written or read anywhere.
  std::chrono::steady_clock::time_point jetpack_recovery_start_time_{};
  /* Some Jetpack elements end */

  void *svr_workers_g{nullptr};

  locid_t loc_id_ = -1;
  siteid_t site_id_ = -1;
  unordered_map<txid_t, shared_ptr<Tx>> dtxns_{};
  unordered_map<txid_t, mdb::Txn *> mdb_txns_{};
  unordered_map<txid_t, Executor *> executors_{};

  // L6-A2 (2026-05-01): app_next_ now takes janus::Command (not
  // shared_ptr<Marshallable>) so user code is one type level removed
  // from the rrr framework's wire-boundary shared_ptr.  MarshallDeputy
  // ctors are non-explicit, so callers passing `shared_ptr<Marshallable>`
  // (e.g. `instance->log_`) auto-convert at the call site.
  function<int(int, janus::Command)> app_next_{};
  // Workstream N Phase 4e-9: removed
  //   `function<shared_ptr<vector<MultiValue>>(Marshallable&)> key_deps_{};`
  // — declared but never written or invoked anywhere.

  shared_ptr<mdb::TxnMgr> mdb_txn_mgr_{};
  int mode_;
  // Workstream N Phase 4e-34: removed `Recorder *recorder_ = nullptr;`
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

  // Workstream N Phase 4e-9: removed `bool paused_ = false;` — set
  // in `TxLogServer::Pause()` / `Resume()` (scheduler.cc:333, 338)
  // alongside `commo_->Pause()` / `commo_->Resume()` calls but
  // never read.  The Pause/Resume methods themselves are kept
  // (called from `server_worker.cc:329, 335` and `service.cc:371,
  // 396`) since the `commo_->Pause()` side effect is real; only the
  // dead `paused_` writes were removed.

  // Phase 2.4: State machine recovery tracking
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

  // Workstream N Phase 4e-41: removed `get_prepare_log` declaration —
  // see scheduler.cc retirement comment.

  // TODO: (Shuai: I am not sure this is supposed to be here.)
  // I think it used to initialized the database?
  // So it should be somewhere else?
  void reg_table(const string &name,
                 mdb::Table *tbl
  );

  virtual int32_t Dispatch(cmdid_t cmd_id,
                        shared_ptr<Marshallable> cmd,
                        TxnOutput& ret_output,
                        std::shared_ptr<ViewData>& view_data) {
    verify(0);
    return REJECT;
  }

  // L6-A2 (2026-05-01): take a `function<int(int, janus::Command)>`.
  // Lambdas registered here see the deputy directly; if they need the
  // legacy shared_ptr they can call `md.inner()`, or use the
  // `marshallable_cast<T>(md)` overload to downcast to a concrete type.
  void RegLearnerAction(function<int(int, janus::Command)> learner_action) {
    app_next_ = learner_action;
  }

  // L6-A2 (2026-05-01): take janus::Command (matches RegLearnerAction
  // signature above).  Body uses `md.inner()` / `marshallable_cast<T>(md)`
  // to access the underlying typed payload.
  virtual int Next(int, janus::Command md) { verify(0); };
  /**
   * Check if the command is already committed
   * @param commit_cmd command to be checked
   * @return true if it's already committed, false otherwise
   */
  virtual bool CheckCommitted(Marshallable& commit_cmd) { verify(0); }

  // Workstream N Phase 4e-7: removed `virtual void Next(Marshallable&)
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

  void ApplyToDatabase(shared_ptr<Marshallable> cmd) {
    SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
    // Log_info("Apply Write %d key %d value %d", parsed_cmd.IsWrite(), parsed_cmd.key_, parsed_cmd.value_);
    if (parsed_cmd.IsWrite()) {
      database_[parsed_cmd.key_] = parsed_cmd.value_;
      database_operation_count_++;
    }
  }

  uint32_t ChecksumXor() {
    Log_info("database_operation_count_ %d", database_operation_count_);
    uint32_t checksum = 0;
    for (const auto& kv : database_) {
        checksum ^= static_cast<uint32_t>(kv.first);
        checksum ^= static_cast<uint32_t>(kv.second);
    }
    return checksum;
  }

  UniqueCmdID GetUniqueCmdID(shared_ptr<Marshallable> cmd);

  value_t DBGet(const shared_ptr<Marshallable>& cmd);

  value_t DBPut(const shared_ptr<Marshallable>& cmd);

  // This used for garbage collection / evaluation data structure grows over time
  void PrintStructureSize();

  // below are about rule

  Witness witness_;

  // For Rule usage
  void OnRuleSpeculativeExecute(const shared_ptr<Marshallable>& cmd,
                                bool_t* accepted,
                                value_t* result,
                                bool_t* is_leader);

  void OriginalPathUnexecutedCmdConflictPlaceHolder(const shared_ptr<Marshallable>& cmd);

  // @unsafe
  void RuleWitnessGC(const shared_ptr<Marshallable>& cmd);

#ifdef ZERO_OVERHEAD
  virtual bool ConflictWithOriginalUnexecutedLog(const shared_ptr<Marshallable>& cmd) {
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
  void DispatchRecoveredCommand(shared_ptr<Marshallable> cmd, shared_ptr<IntEvent> recovery_event = nullptr);
  
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
                          shared_ptr<VecRecData> id_set);
  
  // @unsafe
  virtual void OnJetpackPullCmd(const epoch_t& jepoch,
                        const epoch_t& oepoch,
                        const std::vector<key_t>& keys,
                        bool_t* ok, 
                        epoch_t* reply_jepoch, 
                        epoch_t* reply_oepoch,
                        janus::Command* reply_old_view,
                        janus::Command* reply_new_view,
                        shared_ptr<KeyCmdBatchData>& batch);
  
  void OnJetpackRecordCmd(const epoch_t& jepoch, 
                          const epoch_t& oepoch, 
                          const int32_t& sid, 
                          const int32_t& rid, 
                          shared_ptr<KeyCmdBatchData>& batch);
  
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
  
  void OnJetpackPullRecSetIns(const epoch_t& jepoch,
                              const epoch_t& oepoch, 
                              const int32_t& sid, 
                              const int32_t& rid, 
                              bool_t* ok, 
                              epoch_t* reply_jepoch,
                              epoch_t* reply_oepoch,
                              janus::Command* reply_old_view,
                              janus::Command* reply_new_view,
                              shared_ptr<Marshallable> cmd);
  
  void OnJetpackFinishRecovery(const epoch_t& oepoch);


};

} // namespace janus
