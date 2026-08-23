#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

#include "__dep__.h"
#include "constants.h"
#include "tx.h"
#include "scheduler.h"
#include "marshal-value.h"
#include "procedure.h"
#include "rcc_rpc.h"
#include "frame.h"
#include "benchmark_registry.h"
#include "executor.h"
#include "coordinator.h"
#include "raft/server.h"
#include "config.h"

#include <gperftools/profiler.h>

import std;

namespace janus {

shared_ptr<Tx> TxLogServer::CreateTx(epoch_t epoch, txnid_t tid, bool
read_only) {
  Log_debug("create tid {}", tid);
  verify(dtxns_.find(tid) == dtxns_.end());
  if (epoch == 0) {
    epoch = epoch_mgr_.curr_epoch_;
  }
  verify(epoch_mgr_.IsActive(epoch));
  auto dtxn = frame_->CreateTx(epoch, tid, read_only, this);
  if (dtxn != nullptr) {
    dtxns_[tid] = dtxn;
    // removed
    // `dtxn->recorder_ = this->recorder_;` — both fields gone.
    dtxn->txn_reg_ = txn_reg_;
    verify(txn_reg_ != nullptr);
    verify(dtxn->tid_ == tid);
  } else {
    verify(0);
  }
  if (epoch_enabled_) {
    epoch_mgr_.AddToEpoch(epoch, tid);
    TriggerUpgradeEpoch();
  }
  dtxn->sched_ = this;
  return dtxn;
}

shared_ptr<Tx> TxLogServer::CreateTx(txnid_t tx_id, bool ro) {
  Log_debug("create tid {:x}", tx_id);
  verify(dtxns_.find(tx_id) == dtxns_.end());
  auto dtxn = frame_->CreateTx(epoch_mgr_.curr_epoch_, tx_id, ro, this);
  if (dtxn != nullptr) {
    dtxns_[tx_id] = dtxn;
    // removed
    // `dtxn->recorder_ = this->recorder_;` — both fields gone.
    verify(txn_reg_);
    dtxn->txn_reg_ = txn_reg_;
    verify(dtxn->tid_ == tx_id);
    if (epoch_enabled_) {
      epoch_mgr_.AddToCurrent(tx_id);
      TriggerUpgradeEpoch();
    }
    dtxn->sched_ = this;
  } else {
    // for multi-paxos this would happen.
    // verify(0);
  }
  return dtxn;
}

shared_ptr<Tx> TxLogServer::GetOrCreateTx(txnid_t tid, bool ro) {
  //Log_info("The current server is {}", site_id_);
  shared_ptr<Tx> ret = nullptr;
  auto it = dtxns_.find(tid);
  if (it == dtxns_.end()) {
    ret = CreateTx(tid, ro);
  } else {
    ret = it->second;
  }
  //Log_info("Tx is {}", tid);
  verify(ret != nullptr);
  verify(ret->tid_ == tid);
  return ret;
}
void TxLogServer::DestroyTx(i64 tid) {
  Log_debug("destroy tid {:x}", tid);
  auto it = dtxns_.find(tid);
  // verify(it != dtxns_.end());
  if (it != dtxns_.end()) {
    dtxns_.erase(it);
  }
}

shared_ptr<Tx> TxLogServer::GetTx(txnid_t tid) {
  // Log_debug("DTxnMgr::get({})\n", tid);
  auto it = dtxns_.find(tid);
  // verify(it != dtxns_.end());
  if (it != dtxns_.end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

mdb::Txn *TxLogServer::GetMTxn(const i64 tid) {
  mdb::Txn *txn = nullptr;
  auto it = mdb_txns_.find(tid);
  if (it == mdb_txns_.end()) {
    verify(0);
  } else {
    txn = it->second;
  }
  return txn;
}

mdb::Txn *TxLogServer::RemoveMTxn(const i64 tid) {
  mdb::Txn *txn = nullptr;
  auto it = mdb_txns_.find(tid);
  verify(it != mdb_txns_.end());
  txn = it->second;
  mdb_txns_.erase(it);
  return txn;
}

mdb::Txn *TxLogServer::GetOrCreateMTxn(const i64 tid) {
  mdb::Txn *txn = nullptr;
  auto it = mdb_txns_.find(tid);
  if (it == mdb_txns_.end()) {
    txn = mdb_txn_mgr_->start(tid);
    // using occ lazy mode: increment version at commit time
    auto mode = Config::GetConfig()->tx_proto_;
    if (mode == MODE_OCC || mode == MODE_MDCC) {
      ((mdb::TxnOCC *) txn)->set_policy(mdb::OCC_LAZY);
    }
    auto ret = mdb_txns_.insert(std::pair<i64, mdb::Txn *>(tid, txn));
    verify(ret.second);
  } else {
    txn = it->second;
  }

  verify(txn != nullptr);
  return txn;
}

// removed `TxLogServer::get_prepare_log`
// (~42 LOC) — only call site was the now-deleted `do_logging()`-
// gated branch in `SchedulerClassic::Prepare`, which built a `log`
// string only to discard it.

TxLogServer::TxLogServer() : mtx_() {
  mdb_txn_mgr_ = make_shared<mdb::TxnMgrUnsafe>();
  // removed `if (do_logging()) { ... }`
  // block — body was a commented-out
  // `// recorder_ = new Recorder(path);` and the field is gone.
}

// @unsafe - Logs recovery status
void TxLogServer::SetRecoveryMode(bool recovering) {
  in_state_machine_recovery_ = recovering;
  if (!recovering && transactions_recovered_ > 0) {
    Log_info("[STATE-RECOVERY] Site {}: Recovery complete, {} transactions applied",
             site_id_, transactions_recovered_);
  }
}

Coordinator *TxLogServer::CreateRepCoord(const i64& dep_id) {
  Coordinator *coord;
  static cooid_t cid = 0;
  int32_t benchmark = 0;
  static id_t id = 0;
  verify(rep_frame_ != nullptr);
  coord = rep_frame_->CreateCoordinator(cid++,
                                        Config::GetConfig(),
                                        benchmark,
                                        rusty::None,
                                        id++,
                                        txn_reg_);
  coord->frame_ = rep_frame_;
  coord->dep_id_ = dep_id;
  coord->par_id_ = partition_id_;
  //Log_info("Partition id set: {}", partition_id_);
  coord->loc_id_ = this->loc_id_;
  // removed a second
  // `coord->dep_id_ = dep_id;` immediately below this line — it was
  // a duplicate write of the same value already done above.
  return coord;
}


TxLogServer::TxLogServer(int mode) : TxLogServer() {
  mode_ = mode;
  switch (mode) {
    case MODE_MDCC:
    case MODE_OCC:
      mdb_txn_mgr_ = make_shared<mdb::TxnMgrOCC>();
      break;
    case MODE_NONE:
    case MODE_RPC_NULL:
      mdb_txn_mgr_ = make_shared<mdb::TxnMgrUnsafe>();
      break;
    default:verify(0);
  }
}

TxLogServer::~TxLogServer() {
  auto it = mdb_txns_.begin();
  for (; it != mdb_txns_.end(); it++)
    Log_info("tid: {} still running", it->first);
  if (it != mdb_txns_.end() && it->second) {
    delete it->second;
    it->second = NULL;
  }
  mdb_txns_.clear();
#ifdef CPU_PROFILE_SEVER
  if (site_id_ == 0) {
    ProfilerStop();
  }
#endif
  std::vector<double> witness_size_distribution = witness_.witness_size_distribution();
  Log_info("loc_id={} witness size distribution 50pct {:.2f} 90pct {:.2f} 99pct {:.2f} ave {:.2f}",
    loc_id_, witness_size_distribution[0], witness_size_distribution[1], witness_size_distribution[2], witness_size_distribution[3]);
#ifdef WITNESS_LOG_DEBUG
  if (loc_id_ == 0 || loc_id_ == 1)
    witness_.print_log();
#endif

}

/**
 *
 * @param txn_box
 * @param inn_id, if 0, execute all pieces.
 */
void TxLogServer::Execute(Tx &txn_box,
                          innid_t inn_id) {
  if (inn_id == 0) {
    for (auto &pair : txn_box.paused_pieces_) {
      auto &up_pause = pair.second;
      verify(up_pause);
      up_pause->set(1);
    }
    txn_box.paused_pieces_.clear();
  } else {
    auto &up_pause = txn_box.paused_pieces_[inn_id];
    verify(up_pause);
    up_pause->set(1);
    txn_box.paused_pieces_.erase(inn_id);
  }
}

void TxLogServer::reg_table(const std::string &name,
                            mdb::Table *tbl) {
  EnsureBenchmarkRegistryInitialized();
  auto table_names = BenchmarkRegistry::Instance().GetTableNames();
  verify(mdb_txn_mgr_ != NULL);
  mdb_txn_mgr_->reg_table(name, tbl);
  if (name == table_names.tpcc_order) {
    mdb::Schema *schema = new mdb::Schema();
    const mdb::Schema *o_schema = tbl->schema();
    mdb::Schema::iterator it = o_schema->begin();
    for (; it != o_schema->end(); it++)
      if (it->indexed)
        if (it->name != "o_id")
          schema->add_column(it->name.c_str(), it->type, true);
    schema->add_column("o_c_id", Value::I32, true);
    schema->add_column("o_id", Value::I32, false);
    mdb_txn_mgr_->reg_table(table_names.tpcc_order_c_id_secondary,
                            new mdb::SortedTable(name, schema));
  }
}

void TxLogServer::DestroyExecutor(txnid_t txn_id) {
  Log_debug("destroy tid {}\n", txn_id);
  auto it = executors_.find(txn_id);
  verify(it != executors_.end());
  auto exec = it->second;
  executors_.erase(it);
  delete exec;
}

void TxLogServer::Pause() {
  Log_info("!!!!!!!! TxLogServer::Pause()");
  commo_->Pause();
  // removed `paused_ = true;` — the
  // `paused_` field on TxLogServer had no readers anywhere; the
  // field went away in the same commit.
};

void TxLogServer::Resume() {
  commo_->Resume();
  // removed `paused_ = false;` — see the
  // companion comment on Pause() above.
};

void TxLogServer::TriggerUpgradeEpoch() {
  if (site_id_ == 0) {
    auto t_now = std::time(nullptr);
    auto d = std::difftime(t_now, last_upgrade_time_);
    if (d < EPOCH_DURATION || in_upgrade_epoch_) {
      return;
    }
    last_upgrade_time_ = t_now;
    in_upgrade_epoch_ = true;
    epoch_t epoch = epoch_mgr_.curr_epoch_;
    commo()->SendUpgradeEpoch(epoch,
                              std::bind(&TxLogServer::UpgradeEpochAck,
                                        this,
                                        std::placeholders::_1,
                                        std::placeholders::_2,
                                        std::placeholders::_3));
  }
}

void TxLogServer::UpgradeEpochAck(parid_t par_id,
                                  siteid_t site_id,
                                  int32_t res) {
  auto parids = Config::GetConfig()->GetAllPartitionIds();
  epoch_replies_[par_id][site_id] = res;
  if (epoch_replies_.size() < parids.size()) {
    return;
  }
  for (auto &pair: epoch_replies_) {
    auto par_id = pair.first;
    auto par_size = Config::GetConfig()->GetPartitionSize(par_id);
    verify(epoch_replies_[par_id].size() <= par_size);
    if (epoch_replies_[par_id].size() != par_size) {
      return;
    }
  }

  epoch_t smallest_inactive = 0xFFFFFFFF;
  for (auto &pair1 : epoch_replies_) {
    for (auto &pair2 : pair1.second) {
      if (smallest_inactive > pair2.second) {
        smallest_inactive = pair2.second;
      }
    }
  }
  in_upgrade_epoch_ = false;
  epoch_replies_.clear();
  int x = 5;
  if (smallest_inactive >= x) {
    epoch_t epoch_to_truncate = smallest_inactive - x;
    if (epoch_to_truncate >= epoch_mgr_.oldest_active_) {
      Log_info("truncate epoch {}", epoch_to_truncate);
      commo()->SendTruncateEpoch(epoch_to_truncate);
    }
  }
}

int32_t TxLogServer::OnUpgradeEpoch(uint32_t old_epoch) {
  epoch_mgr_.GrowActive();
  epoch_mgr_.GrowBuffer();
  return epoch_mgr_.CheckBufferInactive();
}

// removed dead helpers
// `GetUniqueCmdID(shared_ptr<Marshallable>)`, `DBGet(...)`, and
// `DBPut(...)`.  None had callers anywhere in the tree.

void TxLogServer::OriginalPathUnexecutedCmdConflictPlaceHolder(const janus::Command& cmd) {
  if (Config::GetConfig()->tx_proto_ == MODE_RULE && SimpleRWCommand::NeedRecordConflictInOriginalPath(cmd)) {
    rep_sched_->witness_.push_back(cmd);
  }
}

void TxLogServer::RuleWitnessGC(const janus::Command& cmd) {
  if (Config::GetConfig()->tx_proto_ == MODE_RULE)
    witness_.remove(cmd);
}

void RevoveryCandidates::push_back(uint64_t cmd_id, const janus::Command& cmd, bool is_write) {
  candidates_[cmd_id] = cmd;
  if (total_write_ == 0 && is_write) {
    verify(to_recover_id_ == (uint64_t)(-1));
    to_recover_id_ = cmd_id;
    // Log_info("[JETPACK-Witness] Set to_recover_id_ = {} (first write)", cmd_id);
  }
  total_write_ += is_write;
#ifdef JETPACK_DEDUPLICATE_OPTIMIZATION
  appeared_[cmd_id] = true;
#endif
}

bool RevoveryCandidates::remove(uint64_t cmd_id) {
  auto it = candidates_.find(cmd_id);
  if (it != candidates_.end()) {
    // it->second is Command; SimpleRWCommand still
    // takes shared_ptr<Marshallable>.
    SimpleRWCommand parsed_cmd = SimpleRWCommand(it->second);
    if (total_write_ == 1 && parsed_cmd.IsWrite()) {
      to_recover_id_ = (uint64_t)(-1);
    }
    total_write_ -= parsed_cmd.IsWrite();
    candidates_.erase(cmd_id);
    return 1;
  } else {
    return 0;
  }
}

bool RevoveryCandidates::has_appeared(uint64_t cmd_id) {
  return appeared_[cmd_id];
}

size_t RevoveryCandidates::size() const {
  return candidates_.size();
}

int RevoveryCandidates::total_write() {
  return total_write_;
}

bool RevoveryCandidates::has_cmd_to_recover() const {
  return to_recover_id_ != (uint64_t)(-1);
}

janus::Command RevoveryCandidates::cmd_to_recover() {
  if (to_recover_id_ != (uint64_t)(-1)) {
    auto it = candidates_.find(to_recover_id_);
    if (it != candidates_.end()) {
      return it->second;
    }
  }
  return janus::Command{};
}

bool Witness::push_back(const janus::Command& cmd_env) {
  // SimpleRWCommand ctor + WitnessLog ctor still take
  // shared_ptr<Marshallable>; unwrap at the boundary.
  // RevoveryCandidates::push_back takes Command directly (prep6aa).
  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd_env);
  key_t key = parsed_cmd.key_;
  uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);

#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-DEBUG] Witness::push_back called for key={}, cmd_id={}", key, cmd_id);
#endif

#ifdef READ_NOT_CONFLICT_OPTIMIZATION
  if (candidates_[key].total_write() == 0) {
#endif
#ifndef READ_NOT_CONFLICT_OPTIMIZATION
  if (candidates_[key].size() == 0) {
#endif
    // not exist conflict
    // Log_info("[JETPACK-Witness] candidates_[{}].push_back {}", key, cmd_id);
    candidates_[key].push_back(cmd_id, cmd_env, parsed_cmd.IsWrite());
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-DEBUG] Added cmd to candidates[{}], no conflict", key);
#endif
#ifdef WITNESS_LOG_DEBUG
    witness_log_.push_back(WitnessLog(0, cmd_env, 1, witness_size_));
#endif
    witness_size_distribution_.mid_time_append(++witness_size_);
    return true;
  } else {
    // exist conflict, candidates_[key].size() >= 1
    // Log_info("[JETPACK-Witness] candidates_[{}].push_back {}", key, cmd_id);
    candidates_[key].push_back(cmd_id, cmd_env, parsed_cmd.IsWrite());
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-DEBUG] Added cmd to candidates[{}], WITH conflict (size now={})",
             key, candidates_[key].size());
#endif
#ifdef WITNESS_LOG_DEBUG
    witness_log_.push_back(WitnessLog(0, cmd_env, 0, witness_size_));
#endif
    return false;
  }
}

int Witness::remove(const janus::Command& cmd_env) {
  // SimpleRWCommand + WitnessLog still take shared_ptr;
  // marshallable_cast works on Command directly via Envelope overload.
  if (cmd_env.kind_ != TpcBatchCommand::static_kind()) {
    SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd_env);
    bool removed = candidates_[parsed_cmd.key_].remove(SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second));
    if (removed) {
      witness_size_distribution_.mid_time_append(--witness_size_);
      // if (candidates_[parsed_cmd.key_].size() == 0)
      //   candidates_.erase(parsed_cmd.key_);
    }
#ifdef WITNESS_LOG_DEBUG
    witness_log_.push_back(WitnessLog(1, cmd_env, removed, witness_size_));
#endif
    return removed;
  } else {
    const auto cmds = marshallable_cast<TpcBatchCommand>(cmd_env);
    verify(cmds.is_some());
    int total_removed = 0;
    for (auto& c: cmds.unwrap()->cmds_) {
      SimpleRWCommand parsed_cmd{janus::Command::pack_aliased(c.clone())};
      bool removed = candidates_[parsed_cmd.key_].remove(SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second));
      if (removed) {
        witness_size_distribution_.mid_time_append(--witness_size_);
        total_removed++;
        // if (candidates_[parsed_cmd.key_].size() == 0)
        //   candidates_.erase(parsed_cmd.key_);
      }
#ifdef WITNESS_LOG_DEBUG
      // c is Arc<TpcCommitCommand>; wraps into Command via the
      // templated Arc ctor (explicit clone per repo convention).
      witness_log_.push_back(WitnessLog(1, janus::Command::pack_aliased(c.clone()), removed, witness_size_));
#endif
    }
    return total_removed;
  }
}

bool Witness::has_appeared(const janus::Command& cmd_env) {
  // SimpleRWCommand still takes shared_ptr;
  // marshallable_cast works on Command directly.
  // For a batched command, return whether all of them have appeared
  if (cmd_env.kind_ != TpcBatchCommand::static_kind()) {
    SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd_env);
    uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
    return candidates_[parsed_cmd.key_].has_appeared(cmd_id);
  } else {
    const auto cmds = marshallable_cast<TpcBatchCommand>(cmd_env);
    verify(cmds.is_some());
    bool all_has_appeared = true;
    for (auto& c: cmds.unwrap()->cmds_) {
      SimpleRWCommand parsed_cmd{janus::Command::pack_aliased(c.clone())};
      uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
      if (!candidates_[parsed_cmd.key_].has_appeared(cmd_id)) {
        all_has_appeared = false;
        break;
      }
    }
    return all_has_appeared;
  }
}

// removed `void Witness::set_belongs_to_leader(bool)`
// — see the companion comment on the deleted field in scheduler.h.

std::vector<double> Witness::witness_size_distribution() {
  // Log_info("witness 50pct {} {:.2f}" , witness_size_distribution_.count(), witness_size_distribution_.pct50());
  // Log_info("witness 90pct {} {:.2f}" , witness_size_distribution_.count(), witness_size_distribution_.pct90());
  // Log_info("witness 99pct {} {:.2f}" , witness_size_distribution_.count(), witness_size_distribution_.pct99());
  // Log_info("witness ave {} {:.2f}" , witness_size_distribution_.count(), witness_size_distribution_.ave());
  std::vector<double> ret;
  ret.push_back(witness_size_distribution_.pct50());
  ret.push_back(witness_size_distribution_.pct90());
  ret.push_back(witness_size_distribution_.pct99());
  ret.push_back(witness_size_distribution_.ave());
  // Log_info("witness ret {:.2f} {:.2f} {:.2f} {:.2f}", ret[0], ret[1], ret[2], ret[3]);
  return ret;
}

rusty::Arc<VecRecData> Witness::id_set() {
  // Fill-then-wrap: build the local, wrap in an Arc once complete.
  VecRecData result;
  result.key_data_ = std::make_shared<vector<key_t>>();

  for (const auto& kv : candidates_) {
    key_t key = kv.first;
    if (kv.second.has_cmd_to_recover()) {
      result.key_data_->push_back(key);
    }
  }

#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY-Witness] id_set size {}", result.key_data_->size());
#endif

  return rusty::Arc<VecRecData>::make(std::move(result));
}

void Witness::reset() {
  candidates_.clear();
  witness_size_ = 0;
  witness_size_distribution_ = Distribution();
  
  // Reset recovery related fields
  max_seen_ballot_ = -1;
  max_accepted_ballot_ = -1;
  sid_ = -1;
  set_size_ = 0;
  // removed `committed_ = false;` — the
  // Witness::committed_ field was deleted in the same commit.
}


#ifdef WITNESS_LOG_DEBUG
void Witness::print_log() {
  if (witness_log_.size() == 0)
    return;
  for (int i = 0; i < witness_log_.size(); i++) {
    witness_log_[i].print(witness_log_[0].time_);
  }
}
#endif


void TxLogServer::JetpackRecoveryEntry() {
  jetpack_recovery_start_time_ = std::chrono::steady_clock::now();
  Log_info("[JETPACK-RECOVERY] ===== STARTING JETPACK RECOVERY ======");
  Log_info("[JETPACK-RECOVERY] Leader: site_id={}, jepoch={}, oepoch={}", site_id_, jepoch_, oepoch_);
  
  // Step 1: Begin recovery - broadcast to all replicas in old_view
  JetpackBeginRecovery();
  
  // Step 2: Pull ID sets and recover commands, then proceed with consensus
  JetpackRecovery();
  
  auto recovery_end_time = std::chrono::steady_clock::now();
  auto recovery_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      recovery_end_time - jetpack_recovery_start_time_).count();
  Log_info("[JETPACK-RECOVERY] ===== JETPACK RECOVERY COMPLETED ====== duration={}ms",
           static_cast<long long>(recovery_duration_ms));
}

void TxLogServer::JetpackBeginRecovery() {
  Log_info("[JETPACK-RECOVERY] Step 1: Broadcasting BeginRecovery to partition {}", partition_id_);
  Log_info("[JETPACK-RECOVERY] BeginRecovery: old_view leader={}, new_view leader={}, oepoch={}", 
           old_view_.GetLeader(), new_view_.GetLeader(), oepoch_);
  
  // Wait for majority to receive BeginRecovery
  auto e = commo()->JetpackBroadcastBeginRecovery(partition_id_, site_id_, old_view_, new_view_, oepoch_);
  e->wait();
  
  if (!e->yes()) {
    Log_info("[JETPACK-RECOVERY] BeginRecovery FAILED: got {}/{} responses", e->n_voted_yes_.get(), e->n_total_);
    return;
  }
  Log_info("[JETPACK-RECOVERY] BeginRecovery SUCCESS: got {}/{} responses", e->n_voted_yes_.get(), e->n_total_);
}

void TxLogServer::JetpackRecovery() {
  Log_info("[JETPACK-RECOVERY] Step 2: Broadcasting PullIdSet to collect command IDs");
  
  // Step 1: Broadcast PullIdSet and collect f+1 PullIdSetAck replies
  auto id_set_e = commo()->JetpackBroadcastPullIdSet(partition_id_, site_id_, jepoch_, oepoch_);
  id_set_e->wait();
  
  if (!id_set_e->yes()) {
    Log_info("[JETPACK-RECOVERY] PullIdSet FAILED: got {}/{} responses", id_set_e->q().n_voted_yes_.get(), id_set_e->q().n_total_);
    // Update local jepoch, oepoch from the responses
    if (id_set_e->max_jepoch_ > jepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating jepoch from {} to {}", jepoch_, id_set_e->max_jepoch_);
#endif
      jepoch_ = id_set_e->max_jepoch_;
      witness_.reset(); // Reset witness when jepoch increases
    }
    if (id_set_e->max_oepoch_ > oepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating oepoch from {} to {}", oepoch_, id_set_e->max_oepoch_);
#endif
      oepoch_ = id_set_e->max_oepoch_;
      // TODO: Update old_view_ and new_view_ from responses
    }
    return;
  }
  
  Log_info("[JETPACK-RECOVERY] PullIdSet SUCCESS: got {}/{} responses", id_set_e->q().n_voted_yes_.get(), id_set_e->q().n_total_);
  
  // Make union of all key_set with largest jepoch
  shared_ptr<vector<key_t>> key_set = id_set_e->GetMergedKeys();
  
  // Step 2: Create unique sid (combine replica id and increasing number)
  sid = ((sid_cnt_++) << 8) | loc_id_;
  rid = 0;
  
  Log_info("[JETPACK-RECOVERY] Step 3: Processing {} keys for sid={}", key_set->size(), sid);
  const auto step3_start_time = std::chrono::steady_clock::now();
  const int batch_size = std::max(1, Config::GetConfig()->GetJetpackRecoveryBatchSize());
  size_t processed = 0;
  while (processed < key_set->size()) {
    size_t batch_end = std::min(key_set->size(), processed + static_cast<size_t>(batch_size));
    std::vector<key_t> batch_keys(key_set->begin() + processed, key_set->begin() + batch_end);
    Log_info("[JETPACK-RECOVERY] Step 3: PullCmd batch [{}, {}) (size={}/{})",
             processed, batch_end, batch_keys.size(), key_set->size());

    auto pull_start = std::chrono::steady_clock::now();
    auto pulled_cmd_e = commo()->JetpackBroadcastPullCmd(partition_id_, site_id_, batch_keys, jepoch_, oepoch_);
    pulled_cmd_e->wait();
    auto pull_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - pull_start).count();

    if (!pulled_cmd_e->yes()) {
      Log_info("[JETPACK-RECOVERY] PullCmd batch FAILED: got {}/{} responses wait={}ms",
               pulled_cmd_e->q().n_voted_yes_.get(), pulled_cmd_e->q().n_total_, (long long) pull_wait_ms);
      if (pulled_cmd_e->max_jepoch_ > jepoch_) {
        jepoch_ = pulled_cmd_e->max_jepoch_;
        witness_.reset();
      }
      if (pulled_cmd_e->max_oepoch_ > oepoch_) {
        oepoch_ = pulled_cmd_e->max_oepoch_;
      }
      processed = batch_end;
      continue;
    }

    auto recovered_entries = pulled_cmd_e->GetRecoveredCommands();
    Log_info("[JETPACK-RECOVERY] PullCmd batch SUCCESS: recovered {}/{} keys wait={}ms",
             recovered_entries.size(), batch_keys.size(), (long long) pull_wait_ms);

    if (!recovered_entries.empty()) {
      auto record_start = std::chrono::steady_clock::now();
      auto record_e = commo()->JetpackBroadcastRecordCmd(partition_id_, site_id_, jepoch_, oepoch_, sid, rid, recovered_entries);
      if (record_e) {
        record_e->wait();
        auto record_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_start).count();
        if (record_e->yes()) {
          rid += recovered_entries.size();
          Log_info("[JETPACK-RECOVERY] RecordCmd batch SUCCESS: recorded={} new_rid={} wait={}ms",
                   recovered_entries.size(), rid, (long long) record_wait_ms);
        } else {
          Log_info("[JETPACK-RECOVERY] RecordCmd batch FAILED: got {}/{} responses wait={}ms",
                   record_e->n_voted_yes_.get(), record_e->n_total_, (long long) record_wait_ms);
        }
      }
    }

    processed = batch_end;
  }

  auto step3_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - step3_start_time).count();
  Log_info("[JETPACK-RECOVERY] Step 3 completed for sid={}: processed={} keys, recorded={} cmds, duration={}ms",
           sid, key_set->size(), rid, (long long) step3_duration_ms);
  
  // Step 3: Use Paxos-like procedure to make consensus on sid and set_size
  JetpackPrepare(sid, rid);
  
}

void TxLogServer::JetpackPrepare(int default_sid, int default_set_size) {
  Log_info("[JETPACK-RECOVERY] Step 4: Starting Paxos Prepare phase for consensus");
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Prepare: default_sid={}, default_set_size={}, ballot={}", 
           default_sid, default_set_size, witness_.max_seen_ballot_);
#endif
  
  // Use Paxos-like procedure to make consensus on sid and set_size
  
  auto e = commo()->JetpackBroadcastPrepare(partition_id_, site_id_, jepoch_, oepoch_, witness_.max_seen_ballot_);
  
  e->wait();
  
  if (!e->yes()) {
    Log_info("[JETPACK-RECOVERY] Prepare FAILED: got {}/{} responses", e->q().n_voted_yes_.get(), e->q().n_total_);
    // Update local epochs and ballots from failed responses
    if (e->max_jepoch_ > jepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating jepoch from {} to {}", jepoch_, e->max_jepoch_);
#endif
      jepoch_ = e->max_jepoch_;
      witness_.reset();
    }
    if (e->max_oepoch_ > oepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating oepoch from {} to {}", oepoch_, e->max_oepoch_);
#endif
      oepoch_ = e->max_oepoch_;
    }
    if (e->max_seen_ballot_ > witness_.max_seen_ballot_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating ballot from {} to {}", 
               witness_.max_seen_ballot_, e->max_seen_ballot_);
#endif
      witness_.max_seen_ballot_ = e->max_seen_ballot_;
    }
    return;
  }
  
  Log_info("[JETPACK-RECOVERY] Prepare SUCCESS: got {}/{} responses", e->q().n_voted_yes_.get(), e->q().n_total_);
  
  // Determine which sid and set_size to propose
  int propose_sid = default_sid;        // Default value from recovery
  int propose_set_size = default_set_size;   // Default value from recovery
  
  if (e->HasValue()) {
    // Use the value from the highest accepted ballot
    propose_sid = e->GetSid();
    propose_set_size = e->GetSetSize();
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Using previously accepted value: sid={}, set_size={}", propose_sid, propose_set_size);
#endif
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] No previous value, proposing recovered values: sid={}, set_size={}", propose_sid, propose_set_size);
#endif
  }
  
  JetpackAccept(propose_sid, propose_set_size);
}

void TxLogServer::JetpackAccept(int propose_sid, int propose_set_size) {
  Log_info("[JETPACK-RECOVERY] Step 5: Starting Paxos Accept phase");
  
  // Update local max_seen_ballot before accept
  witness_.max_seen_ballot_++;
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Accept: proposing sid={}, set_size={}, ballot={}", 
           propose_sid, propose_set_size, witness_.max_seen_ballot_);
#endif
  
  auto e = commo()->JetpackBroadcastAccept(partition_id_, site_id_, jepoch_, oepoch_, 
                                          witness_.max_seen_ballot_, propose_sid, propose_set_size);
  e->wait();
  
  if (!e->yes()) {
    Log_info("[JETPACK-RECOVERY] Accept FAILED: got {}/{} responses", e->q().n_voted_yes_.get(), e->q().n_total_);
    // Update local epochs and ballots from failed responses
    if (e->max_jepoch_ > jepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating jepoch from {} to {}", jepoch_, e->max_jepoch_);
#endif
      jepoch_ = e->max_jepoch_;
      witness_.reset();
    }
    if (e->max_oepoch_ > oepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating oepoch from {} to {}", oepoch_, e->max_oepoch_);
#endif
      oepoch_ = e->max_oepoch_;
    }
    if (e->max_seen_ballot_ > witness_.max_seen_ballot_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating ballot from {} to {}", 
               witness_.max_seen_ballot_, e->max_seen_ballot_);
#endif
      witness_.max_seen_ballot_ = e->max_seen_ballot_;
    }
    return;
  }
  
  Log_info("[JETPACK-RECOVERY] Accept SUCCESS: got {}/{} responses, proceeding to commit sid={}, set_size={}", 
           e->q().n_voted_yes_.get(), e->q().n_total_, propose_sid, propose_set_size);
  JetpackCommit(propose_sid, propose_set_size);
}

void TxLogServer::JetpackCommit(int commit_sid, int commit_set_size) {
  Log_info("[JETPACK-RECOVERY] Step 6: Broadcasting Commit for consensus decision");
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Commit: sid={}, set_size={}", commit_sid, commit_set_size);
#endif
  
  // Commit cannot fail - it's just notification after successful Accept
  auto e = commo()->JetpackBroadcastCommit(partition_id_, site_id_, jepoch_, oepoch_, commit_sid, commit_set_size);
  e->wait(); // Wait for at least 1 response (quorum size can be 1)
  
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Commit sent for sid={}, set_size={}, proceeding to resubmit", commit_sid, commit_set_size);
#endif
  JetpackResubmit(commit_sid, commit_set_size);
}

void TxLogServer::JetpackResubmit(int sid, int set_size) {
  Log_info("[JETPACK-RECOVERY] Step 7: Starting resubmit process for sid={} with {} commands", sid, set_size);
  
  // Create an event to track all recovery dispatches
  rusty::Option<rusty::Arc<IntEvent>> recovery_event = rusty::None;
  if (set_size > 0) {
    recovery_event = rusty::Some(create_sp_int_event(set_size));
    // Log_info("[JETPACK-RECOVERY-EVENT] Created recovery event: target={}, initial value={}, event_ptr={}", 
    //          recovery_event->target_.get(), recovery_event->value_.get(), recovery_event.get());
  }
  
  // For committed (sid, set_size) pair, ensure all positions exist locally
  for (int rid = 0; rid < set_size; rid++) {
    janus::Command cmd = rec_set_.get(sid, rid);
    if (!cmd.has_value()) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Missing command at sid={}, rid={}, pulling from replicas", sid, rid);
#endif
      // Pull missing command from other replicas
      auto pull_e = commo()->JetpackBroadcastPullRecSetIns(partition_id_, site_id_, jepoch_, oepoch_, sid, rid);
      Log_info("[JETPACK-RECOVERY] Waiting for PullRecSetIns sid={} rid={} (site={})", sid, rid, site_id_);
      pull_e->wait();
      Log_info("[JETPACK-RECOVERY] PullRecSetIns completed sid={} rid={} (site={}) success={}", sid, rid, site_id_, pull_e->yes());
      if (pull_e->yes()) {
        cmd = pull_e->GetRecoveredCmd();
        if (cmd.has_value()) {
          rec_set_.insert(sid, rid, cmd);
#ifdef JETPACK_RECOVERY_DEBUG
          Log_info("[JETPACK-RECOVERY] Successfully pulled missing command for sid={}, rid={}", sid, rid);
#endif
        } else {
#ifdef JETPACK_RECOVERY_DEBUG
          Log_info("[JETPACK-RECOVERY] PullRecSetIns returned no command for sid={}, rid={}", sid, rid);
#endif
        }
      } else {
#ifdef JETPACK_RECOVERY_DEBUG
        Log_info("[JETPACK-RECOVERY] PullRecSetIns FAILED for sid={}, rid={}: got {}/{} responses",
                 sid, rid, pull_e->q().n_voted_yes_.get(), pull_e->q().n_total_);
#endif
      }
    }

    // Resubmit command via broadcast dispatch to find leader
    verify(cmd.has_value()); // Command must exist after pull attempt

#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Resubmitting command for sid={}, rid={} via broadcast dispatch", sid, rid);
#endif

    // Use the new dispatch method that will find the leader
    DispatchRecoveredCommand(cmd, recovery_event);
    if (((rid + 1) % 100) == 0 || rid + 1 == set_size) {
      Log_info("[JETPACK-RECOVERY] Step 7: Resubmitted {}/{} commands for sid={}",
               rid + 1, set_size, sid);
    }
    
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Command dispatched for sid={}, rid={}", sid, rid);
#endif
  }
  
  // Wait for all recovery dispatches to complete
  if (recovery_event.is_some() && recovery_event.as_ref().unwrap()->target_.get() > 0) {
    // Log_info("[JETPACK-RECOVERY-EVENT] Starting Wait(): current value={}, target={}",
    //          recovery_event.as_ref().unwrap()->value_.get(), recovery_event.as_ref().unwrap()->target_.get());
    auto start_time = std::chrono::steady_clock::now();
    recovery_event.as_ref().unwrap()->wait();
    auto end_time = std::chrono::steady_clock::now();
    auto wait_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    Log_info("[JETPACK-RECOVERY-EVENT] Wait() completed after {}ms. Final value={}, target={}",
             wait_duration, recovery_event.as_ref().unwrap()->value_.get(), recovery_event.as_ref().unwrap()->target_.get());
    Log_info("[JETPACK-RECOVERY] All recovery completed");
  }
  
  Log_info("[JETPACK-RECOVERY] Step 8: Broadcasting FinishRecovery to complete recovery");
  
  // Finally, broadcast FinishRecovery to update jepoch and make fast path available
  auto e = commo()->JetpackBroadcastFinishRecovery(partition_id_, site_id_, oepoch_);
  e->wait();
  
  Log_info("[JETPACK-RECOVERY] FinishRecovery broadcast completed, fast path restored");
}

void TxLogServer::DispatchRecoveredCommand(const janus::Command& cmd, rusty::Option<rusty::Arc<IntEvent>> recovery_event) {
  // Determine if this is tx_sched or rep_sched
  const char* sched_type = "UNKNOWN";
  if (rep_sched_ && this == rep_sched_) {
    sched_type = "REP_SCHED";
  } else if (!rep_sched_ || rep_sched_ != this) {
    sched_type = "TX_SCHED";
  }

  // Log_info("[JETPACK-RECOVERY] DispatchRecoveredCommand called on {} TxLogServer {} (site_id={})",
  //          sched_type, this, site_id_);
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Dispatching recovered command, kind={}", cmd.kind_);
#endif

  // Extract the inner command if this is a TpcCommitCommand
  janus::Command inner_cmd = cmd;
  if (cmd.kind_ == TpcCommitCommand::static_kind()) {
    const auto tpc_cmd = marshallable_cast<TpcCommitCommand>(cmd);
    verify(tpc_cmd.is_some());
    if (tpc_cmd.is_some() && tpc_cmd.unwrap()->cmd_.has_value()) {
      inner_cmd = tpc_cmd.unwrap()->cmd_;
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Extracted inner command from TpcCommitCommand, inner kind={}", inner_cmd.kind_);
#endif
    }
  }

  // Check if the inner command is VecPieceData
  if (inner_cmd.kind_ == VecPieceData::static_kind()) {
    const auto vec_piece_data = marshallable_cast<VecPieceData>(inner_cmd);
    if (vec_piece_data.is_some() && vec_piece_data.unwrap()->sp_vec_piece_data_) {
      // Mark this as a recovery command
      // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
      {
        auto& mut_vpd =
            *const_cast<VecPieceData*>(vec_piece_data.unwrap().get());
        mut_vpd.is_recovery_command_ = true;
      }

#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Dispatching VecPieceData with {} pieces",
               vec_piece_data.unwrap()->sp_vec_piece_data_->size());
#endif

      // Get the partition ID and command ID from the pieces
      auto par_id = vec_piece_data.unwrap()->sp_vec_piece_data_->at(0)->PartitionId();
      auto cmd_id = vec_piece_data.unwrap()->sp_vec_piece_data_->at(0)->root_id_;
      
      // The communicator's view should already be updated from OnJetpackBeginRecovery
      // Double-check that we have the right view
      auto comm = commo();
      // Log_info("[JETPACK-RECOVERY] Using communicator {} (loc_id={}) for recovery dispatch", 
      //          comm, comm->loc_id_);
      auto current_leader = comm->GetLeaderForPartition(par_id);
      // Log_info("[JETPACK-RECOVERY] Dispatching to partition {}, current leader is {}", 
      //          par_id, current_leader);
      // auto view_snapshot = comm->GetPartitionView(par_id);
      // Log_info("[JETPACK-RECOVERY] Resubmit dispatch partition {} targeting leader locale {} view={}", 
      //          par_id, current_leader, view_snapshot.ToString().c_str());
      
      // Set up callback to handle dispatch response
      auto callback = [this, par_id, recovery_event, cmd_id](int res, TxnOutput& output) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Dispatch callback received, res={} (sid={} rid={} target={}, current={})",
               res, sid, rid, recovery_event.is_some() ? recovery_event.as_ref().unwrap()->target_.get() : -1,
               recovery_event.is_some() ? recovery_event.as_ref().unwrap()->value_.get() : -1);
#endif
        if (res == WRONG_LEADER) {
          // This shouldn't happen if we updated the view correctly during BeginRecovery
          Log_error("[JETPACK-RECOVERY] Received WRONG_LEADER during recovery dispatch for partition {}. "
                    "This indicates the view was not properly updated during BeginRecovery.", par_id);
          // The BroadcastDispatch callback should have already updated the view
        } else if (res == SUCCESS) {
          // Log_info("[JETPACK-RECOVERY] Command successfully dispatched during recovery");
        } else if (res == REJECT) {
          Log_info("[JETPACK-RECOVERY] Command rejected during recovery dispatch (expected if tx already processed)");
        } else {
          Log_warn("[JETPACK-RECOVERY] Dispatch failed with result: {}", res);
        }
        
        // Signal that this recovery dispatch is complete
        if (recovery_event.is_some()) {
          int old_value = recovery_event.as_ref().unwrap()->value_.get();
          // Log_info("[JETPACK-RECOVERY-EVENT] About to increment recovery_event: current value={}, target={}, partition={}, res={}",
          //          old_value, recovery_event.as_ref().unwrap()->target_.get(), par_id, res);
          // Log_info("[JETPACK-RECOVERY-EVENT] This increment is happening in BroadcastDispatch callback (dispatch ACK received)");
          recovery_event.as_ref().unwrap()->set(old_value + 1);
          if (recovery_event.as_ref().unwrap()->value_.get() % 100 == 0 || recovery_event.as_ref().unwrap()->is_ready())
            Log_info("[JETPACK-RECOVERY-EVENT] After increment: new value={}, target={}. Event ready={}",
                    recovery_event.as_ref().unwrap()->value_.get(), recovery_event.as_ref().unwrap()->target_.get(),
                    recovery_event.as_ref().unwrap()->is_ready() ? "YES" : "NO");
        }
      };
      
      // Use BroadcastDispatch to send to the leader
      // Log_info("[JETPACK-RECOVERY] DispatchRecoveredCommand sending cmd_id=0x{:x} to partition {} (leader locale {}, sched={}, target={})",
      //          (unsigned long long)cmd_id, par_id, current_leader, sched_type, recovery_event ? recovery_event->target_.get() : -1);
      comm->BroadcastDispatch(vec_piece_data.unwrap()->sp_vec_piece_data_, callback);
      
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Command dispatched through communicator to leader");
#endif
    } else {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] WARNING: Inner command is not VecPieceData, cannot dispatch");
#endif
      Log_error("[JETPACK-RECOVERY] DispatchRecoveredCommand failed: inner command kind={} (expected VecPieceData)", inner_cmd.kind_);
    }
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] WARNING: Command kind {} not supported for dispatch", inner_cmd.kind_);
#endif
    Log_error("[JETPACK-RECOVERY] DispatchRecoveredCommand unsupported command kind={}", inner_cmd.kind_);
  }
}

void TxLogServer::OnJetpackBeginRecovery(const janus::Command& old_view,
                                         const janus::Command& new_view, 
                                         const epoch_t& new_view_id) {
  rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::RECOVERY;
  rep_sched_->oepoch_ = new_view_id;
  auto config = Config::GetConfig();
  
  // Extract ViewData from janus::Command parameters
  const auto sp_old_view_data = marshallable_cast<ViewData>(old_view);
  const auto sp_new_view_data = marshallable_cast<ViewData>(new_view);

  // Update the views if extraction was successful
  if (sp_old_view_data.is_some()) {
    rep_sched_->old_view_ = sp_old_view_data.unwrap()->GetView();
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Updated old_view from janus::Command");
#endif
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Warning: Could not extract old_view from janus::Command");
#endif
  }
  
  if (sp_new_view_data.is_some()) {
    const View& incoming_view = sp_new_view_data.unwrap()->GetView();
    Log_info("[VIEW_DEBUG] OnJetpackBeginRecovery partition {} view transition {} -> {}",
             partition_id_, rep_sched_->new_view_.ToString().c_str(), incoming_view.ToString().c_str());
    rep_sched_->new_view_ = incoming_view;
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Updated new_view from janus::Command");
#endif
    
    // Update the communicator's view immediately
    if (commo_) {
      auto my_comm = commo();
      Log_info("[JETPACK-RECOVERY] This TxLogServer {} has communicator {} (loc_id={})", 
               (void*)this, (void*)my_comm, my_comm ? my_comm->loc_id_ : -1);
      if (my_comm) {
        my_comm->UpdatePartitionView(partition_id_, *sp_new_view_data.unwrap());
      }
    }
    
    // // Also update rep_sched's communicator if different
    // if (rep_sched_ && rep_sched_ != this && rep_sched_->commo_) {
    //   auto rep_comm = rep_sched_->commo();
    //   Log_info("[JETPACK-RECOVERY] Also updating rep_sched {} communicator {} (loc_id={})", 
    //            rep_sched_, rep_comm, rep_comm ? rep_comm->loc_id_ : -1);
    //   if (rep_comm) {
    //     rep_comm->UpdatePartitionView(partition_id_, sp_new_view_data);
    //   }
    // }
    
    Log_info("[JETPACK-RECOVERY] Updated communicator view(s) for partition {} during BeginRecovery: {}",
             partition_id_, sp_new_view_data.unwrap()->GetView().ToString().c_str());

    // Log leader information from the new view
    if (!sp_new_view_data.unwrap()->GetView().leaders_.empty()) {
      int new_leader = sp_new_view_data.unwrap()->GetView().GetLeader();
      bool should_be_leader = (new_leader == site_id_);
      Log_info("[JETPACK-VIEW-UPDATE] New view leader is {}, this server is {}, should_be_leader={}", 
               new_leader, site_id_, should_be_leader);
      
      // Demote immediately if the recovery view picked a different leader
      if (config->replica_proto_ == MODE_RAFT && rep_sched_) {
        if (auto* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
          if (new_leader != raft_server->site_id_ && raft_server->IsLeader()) {
            Log_info("[JETPACK-VIEW-UPDATE] Stepping down due to BeginRecovery view update; new leader={}", new_leader);
            raft_server->setIsLeader(false);
          }
        }
      }
    } else {
      Log_info("[JETPACK-VIEW-UPDATE] WARNING: New view has no leaders in the new view");
    }
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Warning: Could not extract new_view from janus::Command");
#endif
  }
}

void TxLogServer::OnJetpackPullIdSet(const epoch_t& jepoch,
                                     const epoch_t& oepoch,
                                     bool_t* ok,
                                     epoch_t* reply_jepoch,
                                     epoch_t* reply_oepoch,
                                     janus::Command* reply_old_view,
                                     janus::Command* reply_new_view,
                                     VecRecData& id_set) {
  
  
  // Debug print witness candidates
#ifdef JETPACK_RECOVERY_DEBUG
  if (rep_sched_) {

    Log_info("[JETPACK-DEBUG] Witness candidates size: {}", rep_sched_->witness_.candidates_.size());
    
    // Print all keys in witness candidates
    std::stringstream witness_keys;
    int count = 0;
    for (const auto& kv : rep_sched_->witness_.candidates_) {
      if (count++ < 20) {
        witness_keys << kv.first << "(" << kv.second.size() << " cmds) ";
      }
    }
    if (rep_sched_->witness_.candidates_.size() > 20) {
      witness_keys << "... (and " << (rep_sched_->witness_.candidates_.size() - 20) << " more)";
    }
    Log_info("[JETPACK-DEBUG] Witness candidate keys: {}", witness_keys.str().c_str());

  }
#endif
  
  // Initialize janus::Command objects with ViewData objects
  *reply_old_view = rusty::Arc<ViewData>::make(rep_sched_->old_view_);
  *reply_new_view = rusty::Arc<ViewData>::make(rep_sched_->new_view_);
  
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::RECOVERY;
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    // Copy data from witness id_set to the response parameter
    auto witness_id_set = rep_sched_->witness_.id_set();
    id_set.key_data_ = witness_id_set->key_data_;
    
  } else {
    *ok = 0;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    // Initialize empty key_data_ for failed case
    id_set.key_data_ = std::make_shared<vector<key_t>>();
  }
}

void TxLogServer::OnJetpackPullCmd(const epoch_t& jepoch,
                                   const epoch_t& oepoch,
                                   const std::vector<key_t>& keys,
                                   bool_t* ok, 
                                   epoch_t* reply_jepoch, 
                                   epoch_t* reply_oepoch,
                                   janus::Command* reply_old_view,
                                   janus::Command* reply_new_view,
                                   KeyCmdBatchData& batch) {
  
  if (!rep_sched_) {
    return;
  }
  
  if (!reply_old_view || !reply_new_view) {
    return;
  }
  
  *reply_old_view = rusty::Arc<ViewData>::make(rep_sched_->old_view_);
  *reply_new_view = rusty::Arc<ViewData>::make(rep_sched_->new_view_);
  
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::RECOVERY;
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    
    for (const auto& key : keys) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-SCHED-DEBUG] Processing batched key {} for PullCmd", key);
#endif
      auto& candidates = rep_sched_->witness_.candidates_;
      if (candidates.find(key) == candidates.end()) {
        continue;
      }
      if (rep_sched_->witness_.has_cmd_to_recover(key)) {
        auto cmd = rep_sched_->witness_.cmd_to_recover(key);
        if (cmd.has_value()) {
          batch.AddEntry(key, cmd);
        }
      }
    }
  } else {
    *ok = 0;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
  }
  
}

void TxLogServer::OnJetpackRecordCmd(const epoch_t& jepoch, 
                                     const epoch_t& oepoch, 
                                     const int32_t& sid, 
                                     const int32_t& rid, 
                                     const KeyCmdBatchData& batch) {
  if (!rep_sched_) {
    return;
  }
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    for (size_t idx = 0; idx < batch.Size(); idx++) {
      rep_sched_->rec_set_.insert(sid, rid + idx, batch.GetCommand(idx));
    }
  }
}

void TxLogServer::OnJetpackPrepare(const epoch_t& jepoch, 
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
                                   int32_t* replied_set_size) {
  // Initialize janus::Command objects with ViewData objects
  *reply_old_view = rusty::Arc<ViewData>::make(rep_sched_->old_view_);
  *reply_new_view = rusty::Arc<ViewData>::make(rep_sched_->new_view_);
  
  if (max_seen_ballot > rep_sched_->witness_.max_seen_ballot_) {
    rep_sched_->witness_.max_seen_ballot_ = max_seen_ballot;
  }
  *reply_max_seen_ballot = rep_sched_->witness_.max_seen_ballot_;
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_ && max_seen_ballot >= rep_sched_->witness_.max_seen_ballot_) {
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    *accepted_ballot = rep_sched_->witness_.max_accepted_ballot_;
    *replied_sid = rep_sched_->witness_.sid_;
    *replied_set_size = rep_sched_->witness_.set_size_;
  } else {
    *ok = 0;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
  }
}

void TxLogServer::OnJetpackAccept(const epoch_t& jepoch, 
                                  const epoch_t& oepoch, 
                                  const ballot_t& max_seen_ballot, 
                                  const int32_t& sid, 
                                  const int32_t& set_size,
                                  bool_t* ok,
                                  epoch_t* reply_jepoch,
                                  epoch_t* reply_oepoch,
                                  janus::Command* reply_old_view,
                                  janus::Command* reply_new_view,
                                  ballot_t* reply_max_seen_ballot) {
  // Initialize janus::Command objects with ViewData objects
  *reply_old_view = rusty::Arc<ViewData>::make(rep_sched_->old_view_);
  *reply_new_view = rusty::Arc<ViewData>::make(rep_sched_->new_view_);
  
  if (max_seen_ballot > rep_sched_->witness_.max_seen_ballot_) {
    rep_sched_->witness_.max_seen_ballot_ = max_seen_ballot;
  }
  *reply_max_seen_ballot = rep_sched_->witness_.max_seen_ballot_;
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_ && max_seen_ballot >= rep_sched_->witness_.max_seen_ballot_) {
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    rep_sched_->witness_.max_accepted_ballot_ = max_seen_ballot;
    rep_sched_->witness_.sid_ = sid;
    rep_sched_->witness_.set_size_ = set_size;
  } else {
    *ok = 0;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
  }
}

void TxLogServer::OnJetpackCommit(const epoch_t& jepoch,
                                  const epoch_t& oepoch,
                                  const int32_t& sid,
                                  const int32_t& set_size) {
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    rep_sched_->witness_.sid_ = sid;
    rep_sched_->witness_.set_size_ = set_size;
    // removed
    // `rep_sched_->witness_.committed_ = true;` along with the
    // never-read `committed_` field on Witness.
  }
}

void TxLogServer::OnJetpackPullRecSetIns(const epoch_t& jepoch,
                                         const epoch_t& oepoch,
                                         const int32_t& sid,
                                         const int32_t& rid,
                                         bool_t* ok,
                                         epoch_t* reply_jepoch,
                                         epoch_t* reply_oepoch,
                                         janus::Command* reply_old_view,
                                         janus::Command* reply_new_view) {
  // Initialize janus::Command objects with ViewData objects
  *reply_old_view = rusty::Arc<ViewData>::make(rep_sched_->old_view_);
  *reply_new_view = rusty::Arc<ViewData>::make(rep_sched_->new_view_);

  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    // Note: `cmd` wire-out param populated in service.cc with empty
    // TpcCommitCommand pre-fill — never wired up in scheduler.
  } else {
    *ok = 0;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
  }
}

void TxLogServer::OnJetpackFinishRecovery(const epoch_t& oepoch) {
  if (oepoch >= rep_sched_->oepoch_) {
    rep_sched_->jepoch_ = oepoch;
    rep_sched_->oepoch_ = oepoch;
    rep_sched_->witness_.reset();
    rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::READY;
  }
}

} // namespace janus
