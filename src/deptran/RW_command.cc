#include "RW_command.h"

#include "bench/rw/procedure.h"
#include "bench/rw/workload.h"
#include "classic/tpc_command.h"
#include "../rrr/misc/rand.hpp"

namespace janus {

static int volatile x =
    MarshallDeputy::RegInitializer(MarshallDeputy::CMD_KV,
                                     [] () -> Marshallable* {
                                       return new SimpleRWCommand;
                                     });

SimpleRWCommand::SimpleRWCommand(): Marshallable(MarshallDeputy::CMD_KV) {
  //Log_info("[copilot+] SimpleRWCommand Empty created");
  type_ = RW_BENCHMARK_NOOP;
  key_ = 0;
  value_ = 0;
}

SimpleRWCommand::SimpleRWCommand(shared_ptr<Marshallable> cmd): Marshallable(MarshallDeputy::CMD_KV) {
  verify(cmd != nullptr);
  //Log_info("[copilot+] SimpleRWCommand created");
  shared_ptr<vector<shared_ptr<SimpleCommand>>> sp_vec_piece{nullptr};
  shared_ptr<TxPieceData> vector0;
  if (unlikely(cmd->kind_ == MarshallDeputy::CMD_TPC_BATCH)) {
    shared_ptr<TpcBatchCommand> batch_cmd = dynamic_pointer_cast<TpcBatchCommand>(cmd);
    verify(batch_cmd->Size() == 1);
    shared_ptr<TpcCommitCommand> tpc_cmd = batch_cmd->cmds_[0];
    VecPieceData *cmd_cast = (VecPieceData*)(tpc_cmd->cmd_.get());
    is_recovery_command_ = cmd_cast->is_recovery_command_;
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
    vector0 = *(sp_vec_piece->begin());
  } else if (likely(cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT)) {
    shared_ptr<TpcCommitCommand> tpc_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
    VecPieceData *cmd_cast = (VecPieceData*)(tpc_cmd->cmd_.get());
    is_recovery_command_ = cmd_cast->is_recovery_command_;
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
    vector0 = *(sp_vec_piece->begin());
  } else if (cmd->kind_ == MarshallDeputy::CMD_VEC_PIECE) {
    shared_ptr<VecPieceData> cmd_cast = dynamic_pointer_cast<VecPieceData>(cmd);
    is_recovery_command_ = cmd_cast->is_recovery_command_;
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
    vector0 = *(sp_vec_piece->begin());
  } else if (cmd->kind_ == MarshallDeputy::CONTAINER_CMD) {
    vector0 = dynamic_pointer_cast<TxPieceData>(cmd);
  } else {
    verify(0);
  }
  
  TxWorkspace tx_ws = vector0->input;
  std::map<int32_t, mdb::Value> kv_map = *(tx_ws.values_);
  cmd_id_ = make_pair(vector0->client_id_, vector0->cmd_id_in_client_);
  auto raw_type = vector0->type_;
  rule_mode_on_and_is_original_path_only_command_ = vector0->rule_mode_on_and_is_original_path_only_command_;
  if (vector0->type_ == RW_BENCHMARK_R_TXN || vector0->type_ == RW_BENCHMARK_R_TXN_0) {
    type_ = RW_BENCHMARK_R_TXN;
    key_ = kv_map[0].get_i32();
    value_ = 0;
  } else if (vector0->type_ == RW_BENCHMARK_W_TXN || vector0->type_ == RW_BENCHMARK_W_TXN_0) {
    type_ = RW_BENCHMARK_W_TXN;
    key_ = kv_map[0].get_i32();
    value_ = kv_map[1].get_i32();
  } else if (vector0->type_ == RW_BENCHMARK_FINISH) {
    type_ = vector0->type_;
    key_ = kv_map[0].get_i32();
    value_ = kv_map[1].get_i32();
  } else if (vector0->type_ == RW_BENCHMARK_NOOP) {
    type_ = vector0->type_;
    key_ = kv_map[0].get_i32();
    value_ = 0;
  } else {
    verify(0);
  }
  // key_t key = (*(*(((VecPieceData*)(dynamic_pointer_cast<TpcCommitCommand>(cmd)->cmd_.get()))->sp_vec_piece_data_->begin()))->input.values_)[0].get_i32();
}

// SimpleRWCommand::SimpleRWCommand(const SimpleRWCommand &o): Marshallable(o.kind_) {
//   type_ = o.type_;
//   key_ = o.key_;
//   value_ = o.value_;
// }


string SimpleRWCommand::cmd_to_string() {
  //Log_info("[copilot+] enter cmd_to_string of %p", (void*)(this));
  //Log_info("[copilot+] cmd_type=%d", type_);
  if (RW_BENCHMARK_NOOP == type_)
    return string("NoOp k=" + to_string(key_));
  else if (RW_BENCHMARK_R_TXN == type_)
    return string("<" + to_string(cmd_id_.first) + ", " + to_string(cmd_id_.second) + ">" + "Read k=" + to_string(key_));
  else if (RW_BENCHMARK_W_TXN == type_)
    return string("<" + to_string(cmd_id_.first) + ", " + to_string(cmd_id_.second) + ">" + "Write k=" + to_string(key_) + " v=" + to_string(value_));
  else if (RW_BENCHMARK_FINISH == type_)
    return string("<" + to_string(cmd_id_.first) + ", " + to_string(cmd_id_.second) + ">" + "Finish k=" + to_string(key_) + " v=" + to_string(value_));
  else
    verify(0);
  // if (RW_BENCHMARK_NOOP == type_)
  //   return string("<%d, %d> NoOp", cmd_id_.first, cmd_id_.second);
  // else if (RW_BENCHMARK_R_TXN == type_)
  //   return string("<%d, %d> Read k=" + to_string(key_), cmd_id_.first, cmd_id_.second);
  // else if (RW_BENCHMARK_W_TXN == type_)
  //   return string("<%d, %d> Write k=" + to_string(key_) + " v=" + to_string(value_), cmd_id_.first, cmd_id_.second);
  // else if (RW_BENCHMARK_FINISH == type_)
  //   return string("<%d, %d> Finish v=" + to_string(value_), cmd_id_.first, cmd_id_.second);
  // else
  //   verify(0);
}


bool SimpleRWCommand::same_as(SimpleRWCommand &other) {
  return type_ == other.type_ && key_ == other.key_ && value_ == other.value_ &&
          (cmd_id_ == other.cmd_id_ || type_ == RW_BENCHMARK_FINISH);
}


Marshal& SimpleRWCommand::ToMarshal(Marshal& m) const {
  m << type_;
  m << key_;
  m << value_;
  return m;
}

Marshal& SimpleRWCommand::FromMarshal(Marshal& m) {
  m >> type_;
  m >> key_;
  m >> value_;
  return m;
}

bool SimpleRWCommand::IsRead() {
    return type_ == RW_BENCHMARK_R_TXN || type_ == RW_BENCHMARK_R_TXN_0;
}

bool SimpleRWCommand::IsWrite() {
  return type_ == RW_BENCHMARK_W_TXN || type_ == RW_BENCHMARK_W_TXN_0;
}

bool SimpleRWCommand::IsRecoveryCommand() {
  return is_recovery_command_;
}

pair<int32_t, int32_t> SimpleRWCommand::GetCmdID(shared_ptr<Marshallable> cmd) {
  if (cmd == nullptr) {
    return make_pair(-32768, -32768);
  }
  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  return parsed_cmd.cmd_id_;
}

uint64_t SimpleRWCommand::GetCombinedCmdID(shared_ptr<Marshallable> cmd) {
  pair<int32_t, int32_t> cmd_id = GetCmdID(cmd);
  return CombineInt32(cmd_id.first, cmd_id.second);
}

double SimpleRWCommand::GetCurrentMsTime() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000.0;
}

double SimpleRWCommand::zero_time_;

void SimpleRWCommand::SetZeroTime() {
  zero_time_ = GetCurrentMsTime();
}

double SimpleRWCommand::GetMsTimeElaps() {
  return GetCurrentMsTime() - zero_time_;
}

double SimpleRWCommand::GetCommandMsTime(shared_ptr<Marshallable> cmd) {
  shared_ptr<VecPieceData> cmd_cast{nullptr};
  if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT) {
    shared_ptr<TpcCommitCommand> tpc_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
    cmd_cast = dynamic_pointer_cast<VecPieceData>(tpc_cmd->cmd_);
  } else if (cmd->kind_ == MarshallDeputy::CMD_VEC_PIECE) {
    cmd_cast = dynamic_pointer_cast<VecPieceData>(cmd);
  } else {
    verify(0);
  }
  return cmd_cast->time_sent_from_client_;
}

double SimpleRWCommand::GetCommandMsTimeElaps(shared_ptr<Marshallable> cmd) {
  return GetCurrentMsTime() - GetCommandMsTime(cmd);
}

key_t SimpleRWCommand::GetKey(shared_ptr<Marshallable> cmd) {
  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  return parsed_cmd.key_;
}

bool SimpleRWCommand::NeedRecordConflictInOriginalPath(shared_ptr<Marshallable> cmd) {
  shared_ptr<vector<shared_ptr<SimpleCommand>>> sp_vec_piece{nullptr};
  shared_ptr<TxPieceData> vector0;
  if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT) {
    shared_ptr<TpcCommitCommand> tpc_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
    VecPieceData *cmd_cast = (VecPieceData*)(tpc_cmd->cmd_.get());
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
    vector0 = *(sp_vec_piece->begin());
  } else if (cmd->kind_ == MarshallDeputy::CMD_VEC_PIECE) {
    shared_ptr<VecPieceData> cmd_cast = dynamic_pointer_cast<VecPieceData>(cmd);
    sp_vec_piece = cmd_cast->sp_vec_piece_data_;
    vector0 = *(sp_vec_piece->begin());
  } else if (cmd->kind_ == MarshallDeputy::CONTAINER_CMD){ // This only verified it's CmdData, but I assume it is SimpleCommand
    vector0 = dynamic_pointer_cast<TxPieceData>(cmd);
  } else {
    verify(0);
  }
  return vector0->rule_mode_on_and_is_original_path_only_command_;
}

bool SimpleRWCommand::Conflict(shared_ptr<Marshallable> cmd1, shared_ptr<Marshallable> cmd2) {
  SimpleRWCommand parsed_cmd1 = SimpleRWCommand(cmd1);
  SimpleRWCommand parsed_cmd2 = SimpleRWCommand(cmd2);
  if (parsed_cmd1.key_ != parsed_cmd2.key_) {
    // Log_info("Not Conflict %d with %d", parsed_cmd1.key_, parsed_cmd2.key_);
    return false;
  }
  // Log_info("Conflict if %d or %d", parsed_cmd1.IsWrite(), parsed_cmd2.IsWrite());
  return parsed_cmd1.IsWrite() || parsed_cmd2.IsWrite();
}

void KeyDistribution::Insert(key_t key) {
  key_count_[key]++;
}

void KeyDistribution::Print() {
  sort_vec_.clear();
  int sum = 0;
  for (auto it = key_count_.begin(); it != key_count_.end(); it++) {
    sort_vec_.push_back(make_pair(-it->second, it->first));
    sum += it->second;
  }
  sort(sort_vec_.begin(), sort_vec_.end());
  int cnt = 0;
  for (auto it = sort_vec_.begin(); it != sort_vec_.end() && cnt <= 100; it++, cnt++) {
    Log_info("[KeyDistribution] key = %d occur = %d pct= %.2f", it->second, -it->first, -it->first * 100.0 / sum);
  }
}



void OneArmedBandit::Record(bool success) {
  if (attempt_cnt == 100) {
    // overwrite old records
    success_cnt = success_cnt - records[ptr] + success;
    records[ptr++] = success;
    if (ptr == 100) ptr = 0;
  } else {
    // write new records
    attempt_cnt ++;
    success_cnt += success;
    records[ptr++] = success;
    if (ptr == 100) ptr = 0;
  }
}

void OneArmedBandit::RecordSuccess() {
  Record(1);
}

void OneArmedBandit::RecordFail() {
  Record(0);
}

double OneArmedBandit::ConsultAttemptRate() {
  if (attempt_cnt == 0)
    return 1.0;
  else
    return std::min((success_cnt + 5.0) / attempt_cnt, 1.0);
}

bool OneArmedBandit::ConsultAttempt() {
  if (attempt_cnt == 0)
    return true;
  else
    return RandomGenerator::rand(0, attempt_cnt - 1) < success_cnt + 5; // success_cnt + 5 > attempt_cnt is fine, still 100%
}


}