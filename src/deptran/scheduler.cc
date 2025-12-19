#include "__dep__.h"
#include "constants.h"
#include "tx.h"
#include "scheduler.h"
#include "rcc/graph.h"
#include "rcc/graph_marshaler.h"
#include "marshal-value.h"
#include "procedure.h"
#include "rcc_rpc.h"
#include "frame.h"
#include "bench/tpcc/workload.h"
#include "executor.h"
#include "coordinator.h"
#include "classic/coordinator.h"
#include "../bench/rw/workload.h"
#include "raft/server.h"
#include "config.h"

#include <algorithm>
#include <gperftools/profiler.h>

namespace janus {

shared_ptr<Tx> TxLogServer::CreateTx(epoch_t epoch, txnid_t tid, bool
read_only) {
  Log_debug("create tid %ld", tid);
  verify(dtxns_.find(tid) == dtxns_.end());
  if (epoch == 0) {
    epoch = epoch_mgr_.curr_epoch_;
  }
  verify(epoch_mgr_.IsActive(epoch));
  auto dtxn = frame_->CreateTx(epoch, tid, read_only, this);
  if (dtxn != nullptr) {
    dtxns_[tid] = dtxn;
    dtxn->recorder_ = this->recorder_;
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
  Log_debug("create tid %" PRIx64, tx_id);
  verify(dtxns_.find(tx_id) == dtxns_.end());
  auto dtxn = frame_->CreateTx(epoch_mgr_.curr_epoch_, tx_id, ro, this);
  if (dtxn != nullptr) {
    dtxns_[tx_id] = dtxn;
    dtxn->recorder_ = this->recorder_;
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
  //Log_info("The current server is %d", site_id_);
  shared_ptr<Tx> ret = nullptr;
  auto it = dtxns_.find(tid);
  if (it == dtxns_.end()) {
    ret = CreateTx(tid, ro);
  } else {
    ret = it->second;
  }
  //Log_info("Tx is %ld", tid);
  verify(ret != nullptr);
  verify(ret->tid_ == tid);
  return ret;
}
void TxLogServer::DestroyTx(i64 tid) {
  Log_debug("destroy tid %lx", tid);
  auto it = dtxns_.find(tid);
  // verify(it != dtxns_.end());
  if (it != dtxns_.end()) {
    dtxns_.erase(it);
  }
}

shared_ptr<Tx> TxLogServer::GetTx(txnid_t tid) {
  // Log_debug("DTxnMgr::get(%ld)\n", tid);
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

  if (IS_MODE_2PL) {
    verify(mdb_txn_mgr_->rtti() == mdb::symbol_t::TXN_2PL);
    verify(txn->rtti() == mdb::symbol_t::TXN_2PL);
  } else {

  }
  verify(txn != nullptr);
  return txn;
}

// TODO move this to the dtxn class
void TxLogServer::get_prepare_log(i64 txn_id,
                                  const std::vector<i32> &sids,
                                  std::string *str) {
  auto it = mdb_txns_.find(txn_id);
  verify(it != mdb_txns_.end() && it->second != NULL);

  // marshal txn_id
  uint64_t len = str->size();
  str->resize(len + sizeof(txn_id));
  memcpy((void *) (str->data()), (void *) (&txn_id), sizeof(txn_id));
  len += sizeof(txn_id);
  verify(len == str->size());

  // p denotes prepare log
  const char prepare_tag = 'p';
  str->resize(len + sizeof(prepare_tag));
  memcpy((void *) (str->data() + len),
         (void *) &prepare_tag,
         sizeof(prepare_tag));
  len += sizeof(prepare_tag);
  verify(len == str->size());

  // marshal related servers
  uint32_t num_servers = sids.size();
  str->resize(len + sizeof(num_servers) + sizeof(i32) * num_servers);
  memcpy((void *) (str->data() + len),
         (void *) &num_servers,
         sizeof(num_servers));
  len += sizeof(num_servers);
  for (uint32_t i = 0; i < num_servers; i++) {
    memcpy((void *) (str->data() + len), (void *) (&(sids[i])), sizeof(i32));
    len += sizeof(i32);
  }
  verify(len == str->size());

  switch (mode_) {
    case MODE_2PL:
    case MODE_OCC:((mdb::Txn2PL *) it->second)->marshal_stage(*str);
      break;
    default:verify(0);
  }
}

TxLogServer::TxLogServer() : mtx_() {
  mdb_txn_mgr_ = make_shared<mdb::TxnMgrUnsafe>();
  if (Config::GetConfig()->do_logging()) {
    auto path = Config::GetConfig()->log_path();
    // TODO free this
//    recorder_ = new Recorder(path);
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
                                        nullptr,
                                        id++,
                                        txn_reg_);
  coord->frame_ = rep_frame_;
  coord->dep_id_ = dep_id;
  coord->par_id_ = partition_id_;
  //Log_info("Partition id set: %d", partition_id_);
  coord->loc_id_ = this->loc_id_;
  coord->dep_id_ = dep_id;
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
    case MODE_RCC:
    case MODE_RO6:
      mdb_txn_mgr_ = make_shared<mdb::TxnMgrUnsafe>();
      break;
    default:verify(0);
  }
}

TxLogServer::~TxLogServer() {
  auto it = mdb_txns_.begin();
  for (; it != mdb_txns_.end(); it++)
    Log::info("tid: %ld still running", it->first);
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
  Log_info("loc_id=%d witness size distribution 50pct %.2f 90pct %.2f 99pct %.2f ave %.2f",
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
      up_pause->Set(1);
    }
    txn_box.paused_pieces_.clear();
  } else {
    auto &up_pause = txn_box.paused_pieces_[inn_id];
    verify(up_pause);
    up_pause->Set(1);
    txn_box.paused_pieces_.erase(inn_id);
  }
}

void TxLogServer::reg_table(const std::string &name,
                            mdb::Table *tbl) {
  verify(mdb_txn_mgr_ != NULL);
  mdb_txn_mgr_->reg_table(name, tbl);
  if (name == TPCC_TB_ORDER) {
    mdb::Schema *schema = new mdb::Schema();
    const mdb::Schema *o_schema = tbl->schema();
    mdb::Schema::iterator it = o_schema->begin();
    for (; it != o_schema->end(); it++)
      if (it->indexed)
        if (it->name != "o_id")
          schema->add_column(it->name.c_str(), it->type, true);
    schema->add_column("o_c_id", Value::I32, true);
    schema->add_column("o_id", Value::I32, false);
    mdb_txn_mgr_->reg_table(TPCC_TB_ORDER_C_ID_SECONDARY,
                            new mdb::SortedTable(name, schema));
  }
}

void TxLogServer::DestroyExecutor(txnid_t txn_id) {
  Log_debug("destroy tid %ld\n", txn_id);
  auto it = executors_.find(txn_id);
  verify(it != executors_.end());
  auto exec = it->second;
  executors_.erase(it);
  delete exec;
}

void TxLogServer::Pause() {
  Log_info("!!!!!!!! TxLogServer::Pause()");
  commo_->Pause();
  paused_ = true;
};

void TxLogServer::Resume() {
  commo_->Resume();
  paused_ = false;
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
      Log_info("truncate epoch %d", epoch_to_truncate);
      commo()->SendTruncateEpoch(epoch_to_truncate);
    }
  }
}

int32_t TxLogServer::OnUpgradeEpoch(uint32_t old_epoch) {
  epoch_mgr_.GrowActive();
  epoch_mgr_.GrowBuffer();
  return epoch_mgr_.CheckBufferInactive();
}

UniqueCmdID TxLogServer::GetUniqueCmdID(shared_ptr<Marshallable> cmd) {
  shared_ptr<vector<shared_ptr<SimpleCommand>>> sp_vec_piece{nullptr};
  if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT) {
    shared_ptr<TpcCommitCommand> tpc_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
    VecPieceData *cmd_cast = (VecPieceData*)(tpc_cmd->cmd_.get());
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
  } else if (cmd->kind_ == MarshallDeputy::CMD_VEC_PIECE) {
    shared_ptr<VecPieceData> cmd_cast = dynamic_pointer_cast<VecPieceData>(cmd);
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
  } else {
    verify(0);
  }
  shared_ptr<TxPieceData> vector0 = *(sp_vec_piece->begin());
  shared_ptr<CmdData> casted_cmd = dynamic_pointer_cast<CmdData>(vector0);
  UniqueCmdID cmd_id;
  cmd_id.client_id_ = casted_cmd->client_id_;
  cmd_id.cmd_id_ = casted_cmd->cmd_id_in_client_;
  return cmd_id;
}

value_t TxLogServer::DBGet(const shared_ptr<Marshallable>& cmd) {
  shared_ptr<SimpleRWCommand> parsed_cmd_ = make_shared<SimpleRWCommand>(cmd);
  return kv_table_[parsed_cmd_->key_];
}

value_t TxLogServer::DBPut(const shared_ptr<Marshallable>& cmd) {
  shared_ptr<SimpleRWCommand> parsed_cmd_ = make_shared<SimpleRWCommand>(cmd);
  kv_table_[parsed_cmd_->key_] = parsed_cmd_->value_;
  return 1;
}


// below are about rule

void TxLogServer::OnRuleSpeculativeExecute(const shared_ptr<Marshallable>& cmd,
                    bool_t* accepted,
                    value_t* result,
                    bool_t* is_leader) {
#ifdef ZERO_OVERHEAD
  // if (rep_sched_->ConflictWithOriginalUnexecutedLog(cmd))
  //   Log_info("Conflict!");
  if (rep_sched_->witness_.push_back(cmd) && !rep_sched_->ConflictWithOriginalUnexecutedLog(cmd)) {
#else
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-DEBUG] OnRuleSpeculativeExecute about to push_back loc_id %d ", loc_id_);
#endif
  if (rep_sched_->witness_.push_back(cmd)) {
#endif
    // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
    // Log_info("Server %d OnRuleSpeculativeExecute <%d, %d> key %d", rep_sched_->loc_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second, parsed_cmd.key_);
    // Log_info("witness_.push_back server %d push cmd_id <%d, %d> %lld key %d success 1", loc_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second,
      // (long long)SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second), parsed_cmd.key_);
    // verify(witness_.remove(cmd));
    *accepted = true;
    // [RULE] TODO: return speculative result
    *result = 0;
  } else {
    *accepted = false;
  }
  *is_leader = IsLeader();
}

void TxLogServer::OriginalPathUnexecutedCmdConflictPlaceHolder(const shared_ptr<Marshallable>& cmd) {
  if (Config::GetConfig()->tx_proto_ == MODE_RULE && SimpleRWCommand::NeedRecordConflictInOriginalPath(cmd)) {
    // Log_info("[JETPACK-Witness] loc_id %d about to push_back", loc_id_);
    rep_sched_->witness_.push_back(cmd);
  }
}


void TxLogServer::RuleWitnessGC(const shared_ptr<Marshallable>& cmd) {
  if (Config::GetConfig()->tx_proto_ == MODE_RULE)
    witness_.remove(cmd);
  // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  // uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
  // Log_info("witness_.remove server %d remove cmd_id <%d, %d> %lld key %d success %d", loc_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second,
  //     (long long)SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second), parsed_cmd.key_, witness_.remove(cmd));
  // Log_info("witness_.remove(cmd) %d", witness_.remove(cmd));
  // witness_.remove(cmd);
}


void RevoveryCandidates::push_back(uint64_t cmd_id, shared_ptr<Marshallable> cmd, bool is_write) {
  candidates_[cmd_id] = cmd;
  if (total_write_ == 0 && is_write) {
    verify(to_recover_id_ == (uint64_t)(-1));
    to_recover_id_ = cmd_id;
    // Log_info("[JETPACK-Witness] Set to_recover_id_ = %lu (first write)", cmd_id);
  }
  total_write_ += is_write;
#ifdef JETPACK_DEDUPLICATE_OPTIMIZATION
  appeared_[cmd_id] = true;
#endif
}

bool RevoveryCandidates::remove(uint64_t cmd_id) {
  auto it = candidates_.find(cmd_id);
  if (it != candidates_.end()) {
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

shared_ptr<Marshallable> RevoveryCandidates::cmd_to_recover() {
  
  if (to_recover_id_ != (uint64_t)(-1)) {
    if (candidates_.find(to_recover_id_) != candidates_.end()) {
      return candidates_[to_recover_id_];
    } else {
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

bool Witness::push_back(const shared_ptr<Marshallable>& cmd) {
  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  key_t key = parsed_cmd.key_;
  uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
  
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-DEBUG] Witness::push_back called for key=%d, cmd_id=%lu", key, cmd_id);
#endif

#ifdef READ_NOT_CONFLICT_OPTIMIZATION
  if (candidates_[key].total_write() == 0) {
#endif
#ifndef READ_NOT_CONFLICT_OPTIMIZATION
  if (candidates_[key].size() == 0) {
#endif
    // not exist conflict
    // Log_info("[JETPACK-Witness] candidates_[%d].push_back %lu", key, cmd_id);
    candidates_[key].push_back(cmd_id, cmd, parsed_cmd.IsWrite());
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-DEBUG] Added cmd to candidates[%d], no conflict", key);
#endif
#ifdef WITNESS_LOG_DEBUG
    witness_log_.push_back(WitnessLog(0, cmd, 1, witness_size_));
#endif
    witness_size_distribution_.mid_time_append(++witness_size_);
    return true;
  } else {
    // exist conflict, candidates_[key].size() >= 1
    // Log_info("[JETPACK-Witness] candidates_[%d].push_back %lu", key, cmd_id);
    candidates_[key].push_back(cmd_id, cmd, parsed_cmd.IsWrite());
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-DEBUG] Added cmd to candidates[%d], WITH conflict (size now=%zu)", 
             key, candidates_[key].size());
#endif
#ifdef WITNESS_LOG_DEBUG
    witness_log_.push_back(WitnessLog(0, cmd, 0, witness_size_));
#endif
    return false;
  }
}

int Witness::remove(const shared_ptr<Marshallable>& cmd) {
  if (cmd->kind_ != MarshallDeputy::CMD_TPC_BATCH) {
    SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
    bool removed = candidates_[parsed_cmd.key_].remove(SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second));
    if (removed) {
      witness_size_distribution_.mid_time_append(--witness_size_);
      // if (candidates_[parsed_cmd.key_].size() == 0)
      //   candidates_.erase(parsed_cmd.key_);
    }
#ifdef WITNESS_LOG_DEBUG
    witness_log_.push_back(WitnessLog(1, cmd, removed, witness_size_));
#endif
    return removed;
  } else {
    auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
    int total_removed = 0;
    for (auto& c: cmds->cmds_) {
      SimpleRWCommand parsed_cmd = SimpleRWCommand(c);
      bool removed = candidates_[parsed_cmd.key_].remove(SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second));
      if (removed) {
        witness_size_distribution_.mid_time_append(--witness_size_);
        total_removed++;
        // if (candidates_[parsed_cmd.key_].size() == 0)
        //   candidates_.erase(parsed_cmd.key_);
      }
#ifdef WITNESS_LOG_DEBUG
      witness_log_.push_back(WitnessLog(1, c, removed, witness_size_));
#endif
    }
    return total_removed;
  }
}

bool Witness::has_appeared(const shared_ptr<Marshallable>& cmd) {
  // For a batched command, return whether all of them have appeared
  if (cmd->kind_ != MarshallDeputy::CMD_TPC_BATCH) {
    SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
    uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
    return candidates_[parsed_cmd.key_].has_appeared(cmd_id);
  } else {
    auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
    bool all_has_appeared = true;
    for (auto& c: cmds->cmds_) {
      SimpleRWCommand parsed_cmd = SimpleRWCommand(c);
      uint64_t cmd_id = SimpleRWCommand::CombineInt32(parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
      if (!candidates_[parsed_cmd.key_].has_appeared(cmd_id)) {
        all_has_appeared = false;
        break;
      }
    }
    return all_has_appeared;
  }
}

void Witness::set_belongs_to_leader(bool belongs_to_leader) {
  belongs_to_leader_ = belongs_to_leader;
}

std::vector<double> Witness::witness_size_distribution() {
  // Log_info("witness 50pct %d %.2f" , witness_size_distribution_.count(), witness_size_distribution_.pct50());
  // Log_info("witness 90pct %d %.2f" , witness_size_distribution_.count(), witness_size_distribution_.pct90());
  // Log_info("witness 99pct %d %.2f" , witness_size_distribution_.count(), witness_size_distribution_.pct99());
  // Log_info("witness ave %d %.2f" , witness_size_distribution_.count(), witness_size_distribution_.ave());
  std::vector<double> ret;
  ret.push_back(witness_size_distribution_.pct50());
  ret.push_back(witness_size_distribution_.pct90());
  ret.push_back(witness_size_distribution_.pct99());
  ret.push_back(witness_size_distribution_.ave());
  // Log_info("witness ret %.2f %.2f %.2f %.2f", ret[0], ret[1], ret[2], ret[3]);
  return ret;
}

shared_ptr<VecRecData> Witness::id_set() {
  auto result = std::make_shared<VecRecData>();
  result->key_data_ = std::make_shared<vector<key_t>>();
  
  for (const auto& kv : candidates_) {
    key_t key = kv.first;
    if (kv.second.has_cmd_to_recover()) {
      result->key_data_->push_back(key);
    }
  }
  
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY-Witness] id_set size %d", result->key_data_->size());
#endif

  return result;
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
  committed_ = false;
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
  Log_info("[JETPACK-RECOVERY] Leader: site_id=%d, jepoch=%d, oepoch=%d", site_id_, jepoch_, oepoch_);
  
  // Step 1: Begin recovery - broadcast to all replicas in old_view
  JetpackBeginRecovery();
  
  // Step 2: Pull ID sets and recover commands, then proceed with consensus
  JetpackRecovery();
  
  auto recovery_end_time = std::chrono::steady_clock::now();
  auto recovery_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      recovery_end_time - jetpack_recovery_start_time_).count();
  Log_info("[JETPACK-RECOVERY] ===== JETPACK RECOVERY COMPLETED ====== duration=%lldms",
           static_cast<long long>(recovery_duration_ms));
}

void TxLogServer::JetpackBeginRecovery() {
  Log_info("[JETPACK-RECOVERY] Step 1: Broadcasting BeginRecovery to partition %d", partition_id_);
  Log_info("[JETPACK-RECOVERY] BeginRecovery: old_view leader=%d, new_view leader=%d, oepoch=%d", 
           old_view_.GetLeader(), new_view_.GetLeader(), oepoch_);
  
  // Wait for majority to receive BeginRecovery
  auto e = commo()->JetpackBroadcastBeginRecovery(partition_id_, site_id_, old_view_, new_view_, oepoch_);
  e->Wait();
  
  if (!e->Yes()) {
    Log_info("[JETPACK-RECOVERY] BeginRecovery FAILED: got %d/%d responses", e->n_voted_yes_, e->n_total_);
    return;
  }
  Log_info("[JETPACK-RECOVERY] BeginRecovery SUCCESS: got %d/%d responses", e->n_voted_yes_, e->n_total_);
}

void TxLogServer::JetpackRecovery() {
  Log_info("[JETPACK-RECOVERY] Step 2: Broadcasting PullIdSet to collect command IDs");
  
  // Step 1: Broadcast PullIdSet and collect f+1 PullIdSetAck replies
  auto id_set_e = commo()->JetpackBroadcastPullIdSet(partition_id_, site_id_, jepoch_, oepoch_);
  id_set_e->Wait();
  
  if (!id_set_e->Yes()) {
    Log_info("[JETPACK-RECOVERY] PullIdSet FAILED: got %d/%d responses", id_set_e->n_voted_yes_, id_set_e->n_total_);
    // Update local jepoch, oepoch from the responses
    if (id_set_e->max_jepoch_ > jepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating jepoch from %d to %d", jepoch_, id_set_e->max_jepoch_);
#endif
      jepoch_ = id_set_e->max_jepoch_;
      witness_.reset(); // Reset witness when jepoch increases
    }
    if (id_set_e->max_oepoch_ > oepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating oepoch from %d to %d", oepoch_, id_set_e->max_oepoch_);
#endif
      oepoch_ = id_set_e->max_oepoch_;
      // TODO: Update old_view_ and new_view_ from responses
    }
    return;
  }
  
  Log_info("[JETPACK-RECOVERY] PullIdSet SUCCESS: got %d/%d responses", id_set_e->n_voted_yes_, id_set_e->n_total_);
  
  // Make union of all key_set with largest jepoch
  shared_ptr<vector<key_t>> key_set = id_set_e->GetMergedKeys();
  
  // Step 2: Create unique sid (combine replica id and increasing number)
  sid = ((sid_cnt_++) << 8) | loc_id_;
  rid = 0;
  
  Log_info("[JETPACK-RECOVERY] Step 3: Processing %zu keys for sid=%d", key_set->size(), sid);
  const auto step3_start_time = std::chrono::steady_clock::now();
  const int batch_size = std::max(1, Config::GetConfig()->GetJetpackRecoveryBatchSize());
  size_t processed = 0;
  while (processed < key_set->size()) {
    size_t batch_end = std::min(key_set->size(), processed + static_cast<size_t>(batch_size));
    std::vector<key_t> batch_keys(key_set->begin() + processed, key_set->begin() + batch_end);
    Log_info("[JETPACK-RECOVERY] Step 3: PullCmd batch [%zu, %zu) (size=%zu/%zu)",
             processed, batch_end, batch_keys.size(), key_set->size());

    auto pull_start = std::chrono::steady_clock::now();
    auto pulled_cmd_e = commo()->JetpackBroadcastPullCmd(partition_id_, site_id_, batch_keys, jepoch_, oepoch_);
    pulled_cmd_e->Wait();
    auto pull_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - pull_start).count();

    if (!pulled_cmd_e->Yes()) {
      Log_info("[JETPACK-RECOVERY] PullCmd batch FAILED: got %d/%d responses wait=%lldms",
               pulled_cmd_e->n_voted_yes_, pulled_cmd_e->n_total_, (long long) pull_wait_ms);
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
    Log_info("[JETPACK-RECOVERY] PullCmd batch SUCCESS: recovered %zu/%zu keys wait=%lldms",
             recovered_entries.size(), batch_keys.size(), (long long) pull_wait_ms);

    if (!recovered_entries.empty()) {
      auto record_start = std::chrono::steady_clock::now();
      auto record_e = commo()->JetpackBroadcastRecordCmd(partition_id_, site_id_, jepoch_, oepoch_, sid, rid, recovered_entries);
      if (record_e) {
        record_e->Wait();
        auto record_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - record_start).count();
        if (record_e->Yes()) {
          rid += recovered_entries.size();
          Log_info("[JETPACK-RECOVERY] RecordCmd batch SUCCESS: recorded=%zu new_rid=%d wait=%lldms",
                   recovered_entries.size(), rid, (long long) record_wait_ms);
        } else {
          Log_info("[JETPACK-RECOVERY] RecordCmd batch FAILED: got %d/%d responses wait=%lldms",
                   record_e->n_voted_yes_, record_e->n_total_, (long long) record_wait_ms);
        }
      }
    }

    processed = batch_end;
  }

  auto step3_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - step3_start_time).count();
  Log_info("[JETPACK-RECOVERY] Step 3 completed for sid=%d: processed=%zu keys, recorded=%d cmds, duration=%lldms",
           sid, key_set->size(), rid, (long long) step3_duration_ms);
  
  // Step 3: Use Paxos-like procedure to make consensus on sid and set_size
  JetpackPrepare(sid, rid);
  
}

void TxLogServer::JetpackPrepare(int default_sid, int default_set_size) {
  Log_info("[JETPACK-RECOVERY] Step 4: Starting Paxos Prepare phase for consensus");
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Prepare: default_sid=%d, default_set_size=%d, ballot=%lld", 
           default_sid, default_set_size, witness_.max_seen_ballot_);
#endif
  
  // Use Paxos-like procedure to make consensus on sid and set_size
  
  auto e = commo()->JetpackBroadcastPrepare(partition_id_, site_id_, jepoch_, oepoch_, witness_.max_seen_ballot_);
  
  e->Wait();
  
  if (!e->Yes()) {
    Log_info("[JETPACK-RECOVERY] Prepare FAILED: got %d/%d responses", e->n_voted_yes_, e->n_total_);
    // Update local epochs and ballots from failed responses
    if (e->max_jepoch_ > jepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating jepoch from %d to %d", jepoch_, e->max_jepoch_);
#endif
      jepoch_ = e->max_jepoch_;
      witness_.reset();
    }
    if (e->max_oepoch_ > oepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating oepoch from %d to %d", oepoch_, e->max_oepoch_);
#endif
      oepoch_ = e->max_oepoch_;
    }
    if (e->max_seen_ballot_ > witness_.max_seen_ballot_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating ballot from %lld to %lld", 
               witness_.max_seen_ballot_, e->max_seen_ballot_);
#endif
      witness_.max_seen_ballot_ = e->max_seen_ballot_;
    }
    return;
  }
  
  Log_info("[JETPACK-RECOVERY] Prepare SUCCESS: got %d/%d responses", e->n_voted_yes_, e->n_total_);
  
  // Determine which sid and set_size to propose
  int propose_sid = default_sid;        // Default value from recovery
  int propose_set_size = default_set_size;   // Default value from recovery
  
  if (e->HasValue()) {
    // Use the value from the highest accepted ballot
    propose_sid = e->GetSid();
    propose_set_size = e->GetSetSize();
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Using previously accepted value: sid=%d, set_size=%d", propose_sid, propose_set_size);
#endif
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] No previous value, proposing recovered values: sid=%d, set_size=%d", propose_sid, propose_set_size);
#endif
  }
  
  JetpackAccept(propose_sid, propose_set_size);
}

void TxLogServer::JetpackAccept(int propose_sid, int propose_set_size) {
  Log_info("[JETPACK-RECOVERY] Step 5: Starting Paxos Accept phase");
  
  // Update local max_seen_ballot before accept
  witness_.max_seen_ballot_++;
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Accept: proposing sid=%d, set_size=%d, ballot=%lld", 
           propose_sid, propose_set_size, witness_.max_seen_ballot_);
#endif
  
  auto e = commo()->JetpackBroadcastAccept(partition_id_, site_id_, jepoch_, oepoch_, 
                                          witness_.max_seen_ballot_, propose_sid, propose_set_size);
  e->Wait();
  
  if (!e->Yes()) {
    Log_info("[JETPACK-RECOVERY] Accept FAILED: got %d/%d responses", e->n_voted_yes_, e->n_total_);
    // Update local epochs and ballots from failed responses
    if (e->max_jepoch_ > jepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating jepoch from %d to %d", jepoch_, e->max_jepoch_);
#endif
      jepoch_ = e->max_jepoch_;
      witness_.reset();
    }
    if (e->max_oepoch_ > oepoch_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating oepoch from %d to %d", oepoch_, e->max_oepoch_);
#endif
      oepoch_ = e->max_oepoch_;
    }
    if (e->max_seen_ballot_ > witness_.max_seen_ballot_) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Updating ballot from %lld to %lld", 
               witness_.max_seen_ballot_, e->max_seen_ballot_);
#endif
      witness_.max_seen_ballot_ = e->max_seen_ballot_;
    }
    return;
  }
  
  Log_info("[JETPACK-RECOVERY] Accept SUCCESS: got %d/%d responses, proceeding to commit sid=%d, set_size=%d", 
           e->n_voted_yes_, e->n_total_, propose_sid, propose_set_size);
  JetpackCommit(propose_sid, propose_set_size);
}

void TxLogServer::JetpackCommit(int commit_sid, int commit_set_size) {
  Log_info("[JETPACK-RECOVERY] Step 6: Broadcasting Commit for consensus decision");
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Commit: sid=%d, set_size=%d", commit_sid, commit_set_size);
#endif
  
  // Commit cannot fail - it's just notification after successful Accept
  auto e = commo()->JetpackBroadcastCommit(partition_id_, site_id_, jepoch_, oepoch_, commit_sid, commit_set_size);
  e->Wait(); // Wait for at least 1 response (quorum size can be 1)
  
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Commit sent for sid=%d, set_size=%d, proceeding to resubmit", commit_sid, commit_set_size);
#endif
  JetpackResubmit(commit_sid, commit_set_size);
}

void TxLogServer::JetpackResubmit(int sid, int set_size) {
  Log_info("[JETPACK-RECOVERY] Step 7: Starting resubmit process for sid=%d with %d commands", sid, set_size);
  
  // Create an event to track all recovery dispatches
  shared_ptr<IntEvent> recovery_event = nullptr;
  if (set_size > 0) {
    recovery_event = Reactor::CreateSpEvent<IntEvent>(set_size);
    // Log_info("[JETPACK-RECOVERY-EVENT] Created recovery event: target=%d, initial value=%d, event_ptr=%p", 
    //          recovery_event->target_, recovery_event->value_, recovery_event.get());
  }
  
  // For committed (sid, set_size) pair, ensure all positions exist locally
  for (int rid = 0; rid < set_size; rid++) {
    auto cmd = rec_set_.get(sid, rid);
    if (!cmd) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Missing command at sid=%d, rid=%d, pulling from replicas", sid, rid);
#endif
      // Pull missing command from other replicas
      auto pull_e = commo()->JetpackBroadcastPullRecSetIns(partition_id_, site_id_, jepoch_, oepoch_, sid, rid);
      Log_info("[JETPACK-RECOVERY] Waiting for PullRecSetIns sid=%d rid=%d (site=%d)", sid, rid, site_id_);
      pull_e->Wait();
      Log_info("[JETPACK-RECOVERY] PullRecSetIns completed sid=%d rid=%d (site=%d) success=%d", sid, rid, site_id_, pull_e->Yes());
      if (pull_e->Yes()) {
        cmd = pull_e->GetRecoveredCmd();
        if (cmd) {
          rec_set_.insert(sid, rid, cmd);
#ifdef JETPACK_RECOVERY_DEBUG
          Log_info("[JETPACK-RECOVERY] Successfully pulled missing command for sid=%d, rid=%d", sid, rid);
#endif
        } else {
#ifdef JETPACK_RECOVERY_DEBUG
          Log_info("[JETPACK-RECOVERY] PullRecSetIns returned no command for sid=%d, rid=%d", sid, rid);
#endif
        }
      } else {
#ifdef JETPACK_RECOVERY_DEBUG
        Log_info("[JETPACK-RECOVERY] PullRecSetIns FAILED for sid=%d, rid=%d: got %d/%d responses", 
                 sid, rid, pull_e->n_voted_yes_, pull_e->n_total_);
#endif
      }
    }
    
    // Resubmit command via broadcast dispatch to find leader
    verify(cmd != nullptr); // Command must exist after pull attempt
    
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Resubmitting command for sid=%d, rid=%d via broadcast dispatch", sid, rid);
#endif
    
    // Use the new dispatch method that will find the leader
    DispatchRecoveredCommand(cmd, recovery_event);
    if (((rid + 1) % 100) == 0 || rid + 1 == set_size) {
      Log_info("[JETPACK-RECOVERY] Step 7: Resubmitted %d/%d commands for sid=%d",
               rid + 1, set_size, sid);
    }
    
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Command dispatched for sid=%d, rid=%d", sid, rid);
#endif
  }
  
  // Wait for all recovery dispatches to complete
  if (recovery_event && recovery_event->target_ > 0) {
    // Log_info("[JETPACK-RECOVERY-EVENT] Starting Wait(): current value=%d, target=%d", 
    //          recovery_event->value_, recovery_event->target_);
    auto start_time = std::chrono::steady_clock::now();
    recovery_event->Wait();
    auto end_time = std::chrono::steady_clock::now();
    auto wait_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    Log_info("[JETPACK-RECOVERY-EVENT] Wait() completed after %ldms. Final value=%d, target=%d", 
             wait_duration, recovery_event->value_, recovery_event->target_);
    Log_info("[JETPACK-RECOVERY] All recovery completed");
  }
  
  Log_info("[JETPACK-RECOVERY] Step 8: Broadcasting FinishRecovery to complete recovery");
  
  // Finally, broadcast FinishRecovery to update jepoch and make fast path available
  auto e = commo()->JetpackBroadcastFinishRecovery(partition_id_, site_id_, oepoch_);
  e->Wait();
  
  Log_info("[JETPACK-RECOVERY] FinishRecovery broadcast completed, fast path restored");
}

void TxLogServer::DispatchRecoveredCommand(shared_ptr<Marshallable> cmd, shared_ptr<IntEvent> recovery_event) {
  // Determine if this is tx_sched or rep_sched
  const char* sched_type = "UNKNOWN";
  if (rep_sched_ && this == rep_sched_) {
    sched_type = "REP_SCHED";
  } else if (!rep_sched_ || rep_sched_ != this) {
    sched_type = "TX_SCHED";
  }
  
  // Log_info("[JETPACK-RECOVERY] DispatchRecoveredCommand called on %s TxLogServer %p (site_id=%d)", 
  //          sched_type, this, site_id_);
#ifdef JETPACK_RECOVERY_DEBUG
  Log_info("[JETPACK-RECOVERY] Dispatching recovered command, kind=%d", cmd->kind_);
#endif
  
  // Extract the inner command if this is a TpcCommitCommand
  shared_ptr<Marshallable> inner_cmd = cmd;
  if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT) {
    auto tpc_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
    if (tpc_cmd && tpc_cmd->cmd_) {
      inner_cmd = tpc_cmd->cmd_;
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Extracted inner command from TpcCommitCommand, inner kind=%d", inner_cmd->kind_);
#endif
    }
  }
  
  // Check if the inner command is VecPieceData
  if (inner_cmd->kind_ == MarshallDeputy::CMD_VEC_PIECE) {
    auto vec_piece_data = dynamic_pointer_cast<VecPieceData>(inner_cmd);
    if (vec_piece_data && vec_piece_data->sp_vec_piece_data_) {
      // Mark this as a recovery command
      vec_piece_data->is_recovery_command_ = true;

#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Dispatching VecPieceData with %zu pieces", 
               vec_piece_data->sp_vec_piece_data_->size());
#endif
      
      // Get the partition ID and command ID from the pieces
      auto par_id = vec_piece_data->sp_vec_piece_data_->at(0)->PartitionId();
      auto cmd_id = vec_piece_data->sp_vec_piece_data_->at(0)->root_id_;
      
      // The communicator's view should already be updated from OnJetpackBeginRecovery
      // Double-check that we have the right view
      auto comm = commo();
      // Log_info("[JETPACK-RECOVERY] Using communicator %p (loc_id=%d) for recovery dispatch", 
      //          comm, comm->loc_id_);
      auto current_leader = comm->GetLeaderForPartition(par_id);
      // Log_info("[JETPACK-RECOVERY] Dispatching to partition %d, current leader is %d", 
      //          par_id, current_leader);
      // auto view_snapshot = comm->GetPartitionView(par_id);
      // Log_info("[JETPACK-RECOVERY] Resubmit dispatch partition %d targeting leader locale %d view=%s", 
      //          par_id, current_leader, view_snapshot.ToString().c_str());
      
      // Create a temporary coordinator for dispatching
      // Using a special coordinator ID for recovery operations
      auto coo = std::make_unique<CoordinatorClassic>(999999, // special ID for recovery
                                                       Config::GetConfig()->benchmark_, 
                                                       nullptr, 
                                                       0);
      coo->loc_id_ = site_id_;
      coo->par_id_ = partition_id_;
      
      // Set up callback to handle dispatch response
      auto callback = [this, par_id, recovery_event, cmd_id](int res, TxnOutput& output) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Dispatch callback received, res=%d (sid=%d rid=%d target=%d, current=%d)",
               res, sid, rid, recovery_event ? recovery_event->target_ : -1,
               recovery_event ? recovery_event->value_ : -1);
#endif
        if (res == WRONG_LEADER) {
          // This shouldn't happen if we updated the view correctly during BeginRecovery
          Log_error("[JETPACK-RECOVERY] Received WRONG_LEADER during recovery dispatch for partition %d. "
                    "This indicates the view was not properly updated during BeginRecovery.", par_id);
          // The BroadcastDispatch callback should have already updated the view
        } else if (res == SUCCESS) {
          // Log_info("[JETPACK-RECOVERY] Command successfully dispatched during recovery");
        } else if (res == REJECT) {
          Log_info("[JETPACK-RECOVERY] Command rejected during recovery dispatch (expected if tx already processed)");
        } else {
          Log_warn("[JETPACK-RECOVERY] Dispatch failed with result: %d", res);
        }
        
        // Signal that this recovery dispatch is complete
        if (recovery_event) {
          int old_value = recovery_event->value_;
          // Log_info("[JETPACK-RECOVERY-EVENT] About to increment recovery_event: current value=%d, target=%d, partition=%d, res=%d", 
          //          old_value, recovery_event->target_, par_id, res);
          // Log_info("[JETPACK-RECOVERY-EVENT] This increment is happening in BroadcastDispatch callback (dispatch ACK received)");
          recovery_event->Set(old_value + 1);
          if (recovery_event->value_ % 100 == 0 || recovery_event->IsReady())
            Log_info("[JETPACK-RECOVERY-EVENT] After increment: new value=%d, target=%d. Event ready=%s", 
                    recovery_event->value_, recovery_event->target_, 
                    recovery_event->IsReady() ? "YES" : "NO");
        }
      };
      
      // Use BroadcastDispatch to send to the leader
      // Log_info("[JETPACK-RECOVERY] DispatchRecoveredCommand sending cmd_id=0x%llx to partition %d (leader locale %d, sched=%s, target=%d)",
      //          (unsigned long long)cmd_id, par_id, current_leader, sched_type, recovery_event ? recovery_event->target_ : -1);
      comm->BroadcastDispatch(vec_piece_data->sp_vec_piece_data_, coo.get(), callback);
      
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] Command dispatched through communicator to leader");
#endif
    } else {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-RECOVERY] WARNING: Inner command is not VecPieceData, cannot dispatch");
#endif
      Log_error("[JETPACK-RECOVERY] DispatchRecoveredCommand failed: inner command kind=%d (expected VecPieceData)", inner_cmd->kind_);
    }
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] WARNING: Command kind %d not supported for dispatch", inner_cmd->kind_);
#endif
    Log_error("[JETPACK-RECOVERY] DispatchRecoveredCommand unsupported command kind=%d", inner_cmd->kind_);
  }
}

void TxLogServer::OnJetpackBeginRecovery(const MarshallDeputy& old_view,
                                         const MarshallDeputy& new_view, 
                                         const epoch_t& new_view_id) {
  rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::RECOVERY;
  rep_sched_->oepoch_ = new_view_id;
  auto config = Config::GetConfig();
  
  // Extract ViewData from MarshallDeputy parameters
  auto sp_old_view_data = dynamic_pointer_cast<ViewData>(old_view.sp_data_);
  auto sp_new_view_data = dynamic_pointer_cast<ViewData>(new_view.sp_data_);
  
  // Update the views if extraction was successful
  if (sp_old_view_data) {
    rep_sched_->old_view_ = sp_old_view_data->GetView();
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Updated old_view from MarshallDeputy");
#endif
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Warning: Could not extract old_view from MarshallDeputy");
#endif
  }
  
  if (sp_new_view_data) {
    const View& incoming_view = sp_new_view_data->GetView();
    Log_info("[VIEW_DEBUG] OnJetpackBeginRecovery partition %d view transition %s -> %s",
             partition_id_, rep_sched_->new_view_.ToString().c_str(), incoming_view.ToString().c_str());
    rep_sched_->new_view_ = incoming_view;
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Updated new_view from MarshallDeputy");
#endif
    
    // Update the communicator's view immediately
    if (commo_) {
      auto my_comm = commo();
      Log_info("[JETPACK-RECOVERY] This TxLogServer %p has communicator %p (loc_id=%d)", 
               this, my_comm, my_comm ? my_comm->loc_id_ : -1);
      if (my_comm) {
        my_comm->UpdatePartitionView(partition_id_, sp_new_view_data);
      }
    }
    
    // // Also update rep_sched's communicator if different
    // if (rep_sched_ && rep_sched_ != this && rep_sched_->commo_) {
    //   auto rep_comm = rep_sched_->commo();
    //   Log_info("[JETPACK-RECOVERY] Also updating rep_sched %p communicator %p (loc_id=%d)", 
    //            rep_sched_, rep_comm, rep_comm ? rep_comm->loc_id_ : -1);
    //   if (rep_comm) {
    //     rep_comm->UpdatePartitionView(partition_id_, sp_new_view_data);
    //   }
    // }
    
    Log_info("[JETPACK-RECOVERY] Updated communicator view(s) for partition %d during BeginRecovery: %s", 
             partition_id_, sp_new_view_data->GetView().ToString().c_str());
    
    // Log leader information from the new view
    if (!sp_new_view_data->GetView().leaders_.empty()) {
      int new_leader = sp_new_view_data->GetView().GetLeader();
      bool should_be_leader = (new_leader == site_id_);
      Log_info("[JETPACK-VIEW-UPDATE] New view leader is %d, this server is %d, should_be_leader=%d", 
               new_leader, site_id_, should_be_leader);
      
      // Demote immediately if the recovery view picked a different leader
      if ((config->replica_proto_ == MODE_RAFT || config->replica_proto_ == MODE_FPGA_RAFT) && rep_sched_) {
        if (auto* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
          if (new_leader != raft_server->site_id_ && raft_server->IsLeader()) {
            Log_info("[JETPACK-VIEW-UPDATE] Stepping down due to BeginRecovery view update; new leader=%d", new_leader);
            raft_server->setIsLeader(false);
          }
        }
      }
    } else {
      Log_info("[JETPACK-VIEW-UPDATE] WARNING: New view has no leaders in the new view");
    }
  } else {
#ifdef JETPACK_RECOVERY_DEBUG
    Log_info("[JETPACK-RECOVERY] Warning: Could not extract new_view from MarshallDeputy");
#endif
  }
}

void TxLogServer::OnJetpackPullIdSet(const epoch_t& jepoch,
                                     const epoch_t& oepoch,
                                     bool_t* ok,
                                     epoch_t* reply_jepoch,
                                     epoch_t* reply_oepoch,
                                     MarshallDeputy* reply_old_view,
                                     MarshallDeputy* reply_new_view,
                                     shared_ptr<VecRecData> id_set) {
  
  
  // Debug print witness candidates
#ifdef JETPACK_RECOVERY_DEBUG
  if (rep_sched_) {

    Log_info("[JETPACK-DEBUG] Witness candidates size: %zu", rep_sched_->witness_.candidates_.size());
    
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
    Log_info("[JETPACK-DEBUG] Witness candidate keys: %s", witness_keys.str().c_str());

  }
#endif
  
  // Initialize MarshallDeputy objects with ViewData objects
  reply_old_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->old_view_));
  reply_new_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->new_view_));
  
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::RECOVERY;
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    // Copy data from witness id_set to the response parameter
    auto witness_id_set = rep_sched_->witness_.id_set();
    id_set->key_data_ = witness_id_set->key_data_;
    
  } else {
    *ok = 0;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    // Initialize empty key_data_ for failed case
    id_set->key_data_ = std::make_shared<vector<key_t>>();
  }
}

void TxLogServer::OnJetpackPullCmd(const epoch_t& jepoch,
                                   const epoch_t& oepoch,
                                   const std::vector<key_t>& keys,
                                   bool_t* ok, 
                                   epoch_t* reply_jepoch, 
                                   epoch_t* reply_oepoch,
                                   MarshallDeputy* reply_old_view,
                                   MarshallDeputy* reply_new_view,
                                   shared_ptr<KeyCmdBatchData>& batch) {
  
  if (!rep_sched_ || !batch) {
    return;
  }
  
  if (!reply_old_view || !reply_new_view) {
    return;
  }
  
  reply_old_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->old_view_));
  reply_new_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->new_view_));
  
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    rep_sched_->jetpack_status_ = TxLogServer::JetpackStatus::RECOVERY;
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    
    for (const auto& key : keys) {
#ifdef JETPACK_RECOVERY_DEBUG
      Log_info("[JETPACK-SCHED-DEBUG] Processing batched key %d for PullCmd", key);
#endif
      auto& candidates = rep_sched_->witness_.candidates_;
      if (candidates.find(key) == candidates.end()) {
        continue;
      }
      if (rep_sched_->witness_.has_cmd_to_recover(key)) {
        auto cmd = rep_sched_->witness_.cmd_to_recover(key);
        if (cmd) {
          batch->AddEntry(key, cmd);
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
                                     shared_ptr<KeyCmdBatchData>& batch) {
  if (!rep_sched_ || !batch) {
    return;
  }
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    for (size_t idx = 0; idx < batch->Size(); idx++) {
      rep_sched_->rec_set_.insert(sid, rid + idx, batch->GetCommand(idx));
    }
  }
}

void TxLogServer::OnJetpackPrepare(const epoch_t& jepoch, 
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
                                   int32_t* replied_set_size) {
  // Initialize MarshallDeputy objects with ViewData objects
  reply_old_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->old_view_));
  reply_new_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->new_view_));
  
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
                                  MarshallDeputy* reply_old_view,
                                  MarshallDeputy* reply_new_view,
                                  ballot_t* reply_max_seen_ballot) {
  // Initialize MarshallDeputy objects with ViewData objects
  reply_old_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->old_view_));
  reply_new_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->new_view_));
  
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
    rep_sched_->witness_.committed_ = true;
  }
}

void TxLogServer::OnJetpackPullRecSetIns(const epoch_t& jepoch,
                                         const epoch_t& oepoch, 
                                         const int32_t& sid, 
                                         const int32_t& rid, 
                                         bool_t* ok, 
                                         epoch_t* reply_jepoch,
                                         epoch_t* reply_oepoch,
                                         MarshallDeputy* reply_old_view,
                                         MarshallDeputy* reply_new_view,
                                         shared_ptr<Marshallable> cmd) {
  // Initialize MarshallDeputy objects with ViewData objects
  reply_old_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->old_view_));
  reply_new_view->SetMarshallable(std::make_shared<ViewData>(rep_sched_->new_view_));
  
  if (jepoch >= rep_sched_->jepoch_ && oepoch >= rep_sched_->oepoch_) {
    *ok = 1;
    *reply_jepoch = rep_sched_->jepoch_;
    *reply_oepoch = rep_sched_->oepoch_;
    cmd = rep_sched_->rec_set_.get(sid, rid);
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
