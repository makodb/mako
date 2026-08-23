#include "RW_command.h"

#include "bench/rw/procedure.h"
#include "bench/rw/workload.h"
#include "tpc_command.h"

#include "rrr/rrr.hpp"

namespace janus {

// Registry key comes from the payload's explicit MakoCommands membership.
static int volatile x = rrr::SerializableRegistry::reg<SimpleRWCommand>(SimpleRWCommand::static_kind());

SimpleRWCommand::SimpleRWCommand() {
  type_ = RW_BENCHMARK_NOOP;
  key_ = 0;
  value_ = 0;
}

// primary Command-taking ctor.
// Was the body of `SimpleRWCommand(shared_ptr<Marshallable>)`; now
// uses Command's `kind_` and the Envelope `marshallable_cast<T>`
// overload directly.  The legacy shared_ptr<Marshallable> ctor
// delegates here.
SimpleRWCommand::SimpleRWCommand(const Command& cmd) {
  verify(cmd.has_value());
  rusty::Option<rusty::Arc<VecPieceData>> cmd_cast{};
  if (unlikely(cmd.kind_ == TpcBatchCommand::static_kind())) {
    const auto batch_cmd = marshallable_cast<TpcBatchCommand>(cmd);
    verify(batch_cmd.is_some());
    verify(batch_cmd.unwrap()->Size() == 1);
    const auto& tpc_cmd = batch_cmd.unwrap()->cmds_[0];
    cmd_cast = marshallable_cast<VecPieceData>(tpc_cmd->cmd_);
  } else if (likely(cmd.kind_ == TpcCommitCommand::static_kind())) {
    const auto tpc_cmd = marshallable_cast<TpcCommitCommand>(cmd);
    verify(tpc_cmd.is_some());
    cmd_cast = marshallable_cast<VecPieceData>(tpc_cmd.unwrap()->cmd_);
  } else if (cmd.kind_ == VecPieceData::static_kind()) {
    cmd_cast = marshallable_cast<VecPieceData>(cmd);
  } else {
    // removed the `MarshallDeputy::CONTAINER_CMD`
    // branch — it was reachable only when CmdData inherited
    // Marshallable, which is no longer the case.  Callers holding a
    // `SimpleCommand` directly use the
    // `SimpleRWCommand(const SimpleCommand&)` ctor instead.
    verify(0);
  }
  verify(cmd_cast.is_some());
  shared_ptr<TxPieceData> vector0 =
      *(cmd_cast.as_ref().unwrap()->sp_vec_piece_data_->begin());
  *this = SimpleRWCommand(*vector0);
  // is_recovery_command_ lives on the wrapper (VecPieceData), not on
  // the inner SimpleCommand — patch it back after delegating.
  is_recovery_command_ = cmd_cast.as_ref().unwrap()->is_recovery_command_;
}

// SimpleCommand-direct ctor.
SimpleRWCommand::SimpleRWCommand(const SimpleCommand& cmd) {
  std::map<int32_t, mdb::Value> kv_map = *(cmd.input.values_);
  cmd_id_ = make_pair(cmd.client_id_, cmd.cmd_id_in_client_);
  if (cmd.type_ == RW_BENCHMARK_R_TXN || cmd.type_ == RW_BENCHMARK_R_TXN_0) {
    type_ = RW_BENCHMARK_R_TXN;
    key_ = kv_map[0].get_i32();
    value_ = 0;
  } else if (cmd.type_ == RW_BENCHMARK_W_TXN || cmd.type_ == RW_BENCHMARK_W_TXN_0) {
    type_ = RW_BENCHMARK_W_TXN;
    key_ = kv_map[0].get_i32();
    value_ = kv_map[1].get_i32();
  } else if (cmd.type_ == RW_BENCHMARK_FINISH) {
    type_ = cmd.type_;
    key_ = kv_map[0].get_i32();
    value_ = kv_map[1].get_i32();
  } else if (cmd.type_ == RW_BENCHMARK_NOOP) {
    type_ = cmd.type_;
    key_ = kv_map[0].get_i32();
    value_ = 0;
  } else {
    verify(0);
  }
}

// SimpleRWCommand::SimpleRWCommand(const SimpleRWCommand &o): Marshallable(o.kind_) {
//   type_ = o.type_;
//   key_ = o.key_;
//   value_ = o.value_;
// }


string SimpleRWCommand::cmd_to_string() {
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


// Serializable save/load. Wire format
// identical to the legacy to_marshal/from_marshal pair (just three
// fields: type_, key_, value_).
void SimpleRWCommand::save(BinaryWriteArchive& ar) const {
  rrr::Serialize_::serialize(type_, ar);
  rrr::Serialize_::serialize(key_, ar);
  rrr::Serialize_::serialize(value_, ar);
}

void SimpleRWCommand::load(BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(type_, ar);
  rrr::Deserialize_::deserialize(key_, ar);
  rrr::Deserialize_::deserialize(value_, ar);
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

pair<int32_t, int32_t> SimpleRWCommand::GetCmdID(const Command& cmd) {
  if (!cmd.has_value()) {
    return make_pair(-32768, -32768);
  }
  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  return parsed_cmd.cmd_id_;
}

uint64_t SimpleRWCommand::GetCombinedCmdID(const Command& cmd) {
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

double SimpleRWCommand::GetCommandMsTime(const Command& cmd) {
  rusty::Option<rusty::Arc<VecPieceData>> cmd_cast{};
  if (cmd.kind_ == TpcCommitCommand::static_kind()) {
    const auto tpc_cmd = marshallable_cast<TpcCommitCommand>(cmd);
    verify(tpc_cmd.is_some());
    cmd_cast = marshallable_cast<VecPieceData>(tpc_cmd.unwrap()->cmd_);
  } else if (cmd.kind_ == VecPieceData::static_kind()) {
    cmd_cast = marshallable_cast<VecPieceData>(cmd);
  } else {
    verify(0);
  }
  verify(cmd_cast.is_some());
  return cmd_cast.as_ref().unwrap()->time_sent_from_client_;
}

double SimpleRWCommand::GetCommandMsTimeElaps(const Command& cmd) {
  return GetCurrentMsTime() - GetCommandMsTime(cmd);
}

key_t SimpleRWCommand::GetKey(const Command& cmd) {
  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  return parsed_cmd.key_;
}

}
