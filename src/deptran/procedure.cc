#include "__dep__.h"
#include "marshal-value.h"
#include "coordinator.h"
#include "procedure.h"
#include "rrr/misc/serializable.hpp"
#include "benchmark_control_rpc.h"


namespace janus {

// Registry keys come from each payload's explicit MakoCommands membership.
static int volatile x1 = rrr::SerializableRegistry::reg<VecPieceData>(VecPieceData::static_kind());
static int volatile x2 = rrr::SerializableRegistry::reg<VecRecData>(VecRecData::static_kind());
static int volatile x3 = rrr::SerializableRegistry::reg<ViewData>(ViewData::static_kind());
static int volatile x4 = rrr::SerializableRegistry::reg<KeyCmdBatchData>(KeyCmdBatchData::static_kind());

TxWorkspace::TxWorkspace() {
  values_ = std::make_shared<map<int32_t, Value>>();
}

TxWorkspace::~TxWorkspace() {
//  delete values_;
}

TxWorkspace::TxWorkspace(const TxWorkspace& rhs)
    : keys_(rhs.keys_), values_{rhs.values_} {
}

void TxWorkspace::Aggregate(const TxWorkspace& rhs) {
  keys_.insert(rhs.keys_.begin(), rhs.keys_.end());
  if (values_ != rhs.values_) {
    values_->insert(rhs.values_->begin(), rhs.values_->end());
  }
}

TxWorkspace& TxWorkspace::operator=(const TxWorkspace& rhs) {
  keys_ = rhs.keys_;
  values_ = rhs.values_;
  return *this;
}

TxWorkspace& TxWorkspace::operator=(const map<int32_t, Value>& rhs) {
  keys_.clear();
  for (const auto& pair: rhs) {
    keys_.insert(pair.first);
  }
  *values_ = rhs;
  return *this;
}

Value& TxWorkspace::operator[](size_t idx) {
  keys_.insert(idx);
  return (*values_)[idx];
}

TxData::TxData() {
  clock_gettime(&start_time_);
  // removed `read_only_failed_ = false;`
  // (the field went away with its only remaining writer; default
  // initialization for the bool was already false anyway).
  pre_time_ = timespec2ms(start_time_);
  early_return_ = Config::GetConfig()->do_early_return();
}

// archive operators for TxWorkspace.
// Wire format byte-for-byte identical to the Marshal-based pair
// above: keys_ (set<int32_t>), then per-present-key (k, value) pairs,
// terminated by k=-1.
void serialize(const TxWorkspace &ws, BinaryWriteArchive& ar) {
  rrr::Serialize_::serialize(ws.keys_, ar);
  auto& input_vars = *ws.values_;
  for (int32_t k : ws.keys_) {
    auto it = input_vars.find(k);
    if (it != input_vars.end()) {
      rrr::Serialize_::serialize(k, ar);
      rrr::Serialize_::serialize(it->second, ar);
    }
  }
  rrr::Serialize_::serialize(static_cast<int32_t>(-1), ar);
}

BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const TxWorkspace &ws) { serialize(ws, ar); return ar; }

void deserialize(TxWorkspace &ws, BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(ws.keys_, ar);
  while (true) {
    int32_t k;
    rrr::Deserialize_::deserialize(k, ar);
    if (k >= 0) {
      Value v;
      rrr::Deserialize_::deserialize(v, ar);
      (*ws.values_)[k] = v;
    } else {
      break;
    }
  }
}

BinaryReadArchive& operator>>(BinaryReadArchive& ar, TxWorkspace &ws) { deserialize(ws, ar); return ar; }

// archive operators for TxReply. Wire format
// byte-for-byte identical to the Marshal-based pair above:
//   res_ (i32) | output_ (map<int32_t, Value>) | n_try_ (i32) |
//   time_ (double) | txn_type_ (i32) |
//   has_view_data (bool_t) | optional MarshallDeputy view_md
void serialize(const TxReply& reply, BinaryWriteArchive& ar) {
  rrr::Serialize_::serialize(reply.res_, ar);
  rrr::Serialize_::serialize(reply.output_, ar);
  rrr::Serialize_::serialize(reply.n_try_, ar);
  // start_time_ is intentionally not serialized (legacy comment).
  rrr::Serialize_::serialize(reply.time_, ar);
  rrr::Serialize_::serialize(reply.txn_type_, ar);

  bool_t has_view_data = reply.sp_view_data_.is_some() ? 1 : 0;
  rrr::Serialize_::serialize(has_view_data, ar);
  if (has_view_data) {
    janus::Command view_md = reply.sp_view_data_.unwrap().clone();
    rrr::Serialize_::serialize(view_md, ar);
  }
}

BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const TxReply& reply) { serialize(reply, ar); return ar; }

void deserialize(TxReply& reply, BinaryReadArchive& ar) {
  rrr::Deserialize_::deserialize(reply.res_, ar);
  rrr::Deserialize_::deserialize(reply.output_, ar);
  rrr::Deserialize_::deserialize(reply.n_try_, ar);
  memset(&reply.start_time_, 0, sizeof(reply.start_time_));
  rrr::Deserialize_::deserialize(reply.time_, ar);
  rrr::Deserialize_::deserialize(reply.txn_type_, ar);

  bool_t has_view_data;
  rrr::Deserialize_::deserialize(has_view_data, ar);
  if (has_view_data) {
    janus::Command view_md;
    rrr::Deserialize_::deserialize(view_md, ar);
    reply.sp_view_data_ = marshallable_cast<ViewData>(view_md);
  } else {
    reply.sp_view_data_ = rusty::Option<rusty::Arc<ViewData>>();
  }
}

BinaryReadArchive& operator>>(BinaryReadArchive& ar, TxReply& reply) { deserialize(reply, ar); return ar; }

set<parid_t>& TxData::GetPartitionIds() {
  return partition_ids_;
}

bool TxData::IsOneRound() {
  return false;
}

ReadyPiecesData TxData::GetReadyPiecesData(int32_t max) {
  // Log_info("n_pieces_dispatched_ {} n_pieces_dispatchable_ {} n_pieces_all_ {}", n_pieces_dispatched_, n_pieces_dispatchable_, n_pieces_all_);
  // n_pieces_dispatched_ = 0; // [JetPack TODO] remove this
  verify(n_pieces_dispatched_ <= n_pieces_dispatchable_); // [JetPack TODO] recover this to <
  verify(n_pieces_dispatched_ <= n_pieces_all_); // [JetPack TODO] recover this to <
  ReadyPiecesData ready_pieces_data;

  if (n_pieces_dispatched_ == n_pieces_dispatchable_ || n_pieces_dispatched_ == n_pieces_all_)
    return ready_pieces_data;

//  int n_debug = 0;
  for (auto &kv : status_) {
    auto pi = kv.first;
    auto &status = kv.second;
    if (status == DISPATCHABLE) {
      status = INIT;
      shared_ptr<TxPieceData> piece_data = std::make_shared<TxPieceData>();
      piece_data->inn_id_ = pi;
      piece_data->partition_id_ = GetPiecePartitionId(pi);
      piece_data->type_ = pi;
      piece_data->root_id_ = id_;
      piece_data->root_type_ = type_;
      piece_data->client_id_ = client_id_;
      piece_data->cmd_id_in_client_ = cmd_id_in_client_;
      piece_data->input = inputs_[pi];
      piece_data->output_size = output_size_[pi];
      // removed `piece_data->root_ = this;`
      // — the `root_` back-pointer field on SimpleCommand was unread.
      piece_data->timestamp_ = timestamp_;
      piece_data->rank_ = ranks_[pi]; // TODO fix bug here
      map_piece_data_[pi] = piece_data;
      ready_pieces_data[piece_data->partition_id_].push_back(piece_data);
      partition_ids_.insert(piece_data->partition_id_);
      Log_debug("getting piece data piece id: {}", pi);
      verify(status_[pi] == INIT);
      status_[pi] = DISPATCHED;
      verify(type_ == type());
      verify(piece_data->root_type_ == type());
      verify(piece_data->root_type_ > 0);

      n_pieces_dispatched_++;
      max--;
      if (max == 0) break;
    }
  }
//  verify(ready_pieces_data.size() > 0);
  return ready_pieces_data;
}

// removed `shared_ptr<TxPieceData>
// TxData::GetNextReadySubCmd()`.  The function body started with
// `verify(0)` (intentionally disabled) and the only call sites were
// commented-out legacy coordinator code.  The live dispatch path is
// `TxData::GetReadyPiecesData(int32_t max)`.

bool TxData::OutputReady() {
  if (n_pieces_all_ == n_pieces_dispatch_acked_) {
    return true;
  } else {
    return false;
  }
}

// removed `void TxData::Merge(TxnOutput&)`
// — the only remaining call site (`rcc/coord.cc:214`) was already
// commented out. The live overloads
// `Merge(CmdData&)` and `Merge(innid_t, map<int32_t, Value>&)` cover
// the per-piece merge path.

void TxData::Merge(innid_t inn_id, map<int32_t, Value>& output) {
  verify(outputs_.find(inn_id) == outputs_.end());
  n_pieces_dispatch_acked_++;
  // Log_info("n_pieces_all_={} n_pieces_dispatchable_={}", n_pieces_all_, n_pieces_dispatchable_);
  verify(n_pieces_all_ >= n_pieces_dispatchable_);
  verify(n_pieces_dispatchable_ >= n_pieces_dispatched_);
  verify(n_pieces_dispatched_ >= n_pieces_dispatch_acked_);
  outputs_[inn_id] = output;
  map_piece_data_[inn_id]->output = output;
  this->HandleOutput(inn_id, SUCCESS, output);
}

void TxData::Merge(CmdData &cmd) {
  auto simple_cmd = (SimpleCommand *) &cmd;
  auto pi = cmd.inn_id();
  auto &output = simple_cmd->output;
  Merge(pi, output);
}

bool TxData::HasMoreUnsentPiece() {
  verify(n_pieces_all_ >= n_pieces_dispatchable_);
  verify(n_pieces_dispatchable_ >= n_pieces_dispatched_);
  verify(n_pieces_dispatched_ >= n_pieces_dispatch_acked_);
  //Log_info("dispatch record: {}, {}", n_pieces_dispatchable_, n_pieces_dispatched_);
  if (n_pieces_dispatchable_ == n_pieces_dispatched_) {
    verify(n_pieces_all_ == n_pieces_dispatched_ ||
           n_pieces_dispatch_acked_ < n_pieces_dispatched_);
    return false;
  } else {
    verify(n_pieces_dispatchable_ > n_pieces_dispatched_);
    //Log_info("dispatch record 2: {}, {}", n_pieces_dispatchable_, n_pieces_dispatched_);
    return true;
  }
}

void TxData::Reset() {
  n_pieces_dispatchable_ = 0;
  n_pieces_dispatch_acked_ = 0;
  n_pieces_dispatched_ = 0;
  outputs_.clear();
}

// removed `void TxData::read_only_reset()`
// — its only call sites were already commented out, and the
// `read_only_failed_` field it
// reset went away in the same commit.  Callers needing a reset use
// `TxData::Reset()` directly.
//
//bool Procedure::read_only_start_callback(int pi,
//                                          int *res,
//                                          map<int32_t, mdb::Value> &output) {
//  verify(pi < GetNPieceAll());
//  if (res == NULL) { // phase one, store outputs only
//    outputs_[pi] = output;
//  }
//  else {
//    // phase two, check if this try not failed yet
//    // and outputs is consistent with previous stored one
//    // store current outputs
//    if (read_only_failed_) {
//      *res = REJECT;
//    }
//    else if (pi >= outputs_.size()) {
//      *res = REJECT;
//      read_only_failed_ = true;
//    }
//    else if (is_consistent(outputs_[pi], output))
//      *res = SUCCESS;
//    else {
//      *res = REJECT;
//      read_only_failed_ = true;
//    }
//    outputs_[pi] = output;
//  }
//  return start_callback(pi, SUCCESS, output);
//}

double TxData::last_attempt_latency() {
  double tmp = pre_time_;
  struct timespec t_buf;
  clock_gettime(&t_buf);
  pre_time_ = timespec2ms(t_buf);
  return pre_time_ - tmp;
}

TxReply &TxData::get_reply() {
  reply_.start_time_ = start_time_;
  reply_.n_try_ = n_try_;
  struct timespec t_buf;
  clock_gettime(&t_buf);
  reply_.time_ = timespec2ms(t_buf) - timespec2ms(start_time_);
  reply_.txn_type_ = (int32_t) type_;
  return reply_;
}

// removed `void TxRequest::get_log(i64,
// std::string&)` — both call sites were already commented-out code
// in legacy coordinators.  The method was a 14-line bookkeeping helper
// for an old logging path that is no
// longer invoked.

//` defaults will trigger
// if the dead path is ever exercised — a stricter, more honest
// failure than silently writing partial bytes.)

} // namespace janus
