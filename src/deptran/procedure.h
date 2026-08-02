#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <memory>

#pragma once

#include "__dep__.h"
#include "command.h"
#include <rusty/function.hpp>
#include "rcc/graph.h"
#include "command_marshaler.h"
#include "mako_commands.h"
#include "txn_reg.h"
#include "view.h"

namespace janus {

class Coordinator;
class Sharding;
class ViewData;  // Forward declaration
//class ChopStartResponse;

class TxReply {
 public:
  int32_t res_;
  int32_t n_try_;
  struct timespec start_time_;
  double time_;
  map<int32_t, Value> output_;
  int32_t txn_type_;
  txnid_t tx_id_;
  // Optional view data for client view updates (e.g., when WRONG_LEADER error occurs)
  rusty::Option<rusty::Arc<ViewData>> sp_view_data_{};
  // Timeout flag - set when transaction timed out waiting for shard responses
  bool timed_out_ = false;
};

class TxWorkspace {
 public:
  set<int32_t> keys_ = {};
  std::shared_ptr<map<int32_t, Value>> values_{};
  // removed
  // `std::shared_ptr<map<int32_t, shared_ptr<IntEvent>>> value_events_{};`
  // — defined but never written or read anywhere in the codebase.
  TxWorkspace();
  ~TxWorkspace();
  TxWorkspace(const TxWorkspace& rhs);
  TxWorkspace& operator= (const map<int32_t, Value> &rhs);
  TxWorkspace& operator= (const TxWorkspace& rhs);
  void Aggregate(const TxWorkspace& rhs);
  Value& operator[] (size_t idx);

  size_t count(int32_t k) {
    auto r1 = keys_.count(k);
    auto r2 = (*values_).count(k);
    verify(r1 <= r2);
    return r1;
  }

  Value& at(int32_t k) {
    auto it = values_->find(k);
    verify(it != values_->end());
    return it->second;
  }

  Value& WaitAt(int32_t k) {
    // Predicate wait via IntEvent (the residual Event class was deleted; its
    // 2 predicate call sites moved to IntEvent, which honors state_.test_).
    auto e = reactor_create_sp_event<rrr::IntEvent>();
    (*e->state_.test_.borrow_mut()) = [this, k](int x)->bool{
      auto it = this->values_->find(k);
      return (it != this->values_->end());
    };
    e->wait();
    auto it = values_->find(k);
    verify(it != values_->end());
    return it->second;
  }

  size_t size() const {
    return keys_.size();
  }

  bool VerifyReady() {
    for (auto k: keys_) {
      if (values_->count(k) == 0) {
        verify(0);
        return false;
      }
    }
    return true;
  }

  void insert(map<int32_t, Value>& m) {
    // TODO
    for (auto& pair : m) {
      keys_.insert(pair.first);
    }
    (*values_).insert(m.begin(), m.end());
  }
};

class TxRequest {
 public:
  uint32_t tx_type_ = ~0;
  TxWorkspace input_{};    // the inputs for the transactions.
  int n_try_ = 20;
  /******global unique id begin********/
  int client_id_ = -1;
  int cmd_id_in_client_ = -1;
  /******global unique id end********/
  rusty::Function<void(TxReply &)> callback_ = [] (TxReply&)->void {verify(0);};
  // removed
  //   `rusty::Function<void()> fail_callback_ = [] () { verify(0); };`
  // and `void get_log(i64 tid, std::string &log);` — neither was
  // referenced outside the definitions themselves.  `fail_callback_`
  // was never set or invoked anywhere.  `get_log` had only two call
  // sites and both were already commented-out
  // (`snow/ro6_coord.cc:247`, `rcc/coord.cc:27`).
};

// Phase 8 batch 4 / Marshal-deprecation slice A: the archive serde free
// functions own the TxWorkspace/TxReply wire formats (the Marshal-form
// mirrors are deleted — zero callers).
void serialize(const TxWorkspace &ws, BinaryWriteArchive &ar);
void deserialize(TxWorkspace &ws, BinaryReadArchive &ar);
void serialize(const TxReply &reply, BinaryWriteArchive &ar);
void deserialize(TxReply &reply, BinaryReadArchive &ar);

// archive operators for TxWorkspace. Used by the
// 6 SimpleCommand archive operators which feed VecPieceData's
// Serializable save/load.
BinaryWriteArchive& operator << (BinaryWriteArchive& ar, const TxWorkspace &ws);

BinaryReadArchive& operator >> (BinaryReadArchive& ar, TxWorkspace& ws);

// archive operators for TxReply. Used by the rcc_rpc.h archive
// emission now that rpcgen defaults to --archive.
BinaryWriteArchive& operator << (BinaryWriteArchive& ar, const TxReply& reply);

BinaryReadArchive& operator >> (BinaryReadArchive& ar, TxReply& reply);

enum CommandStatus {
  WAITING=-1,
  DISPATCHABLE=0,
  DISPATCHED=1,
  OUTPUT_READY=2,
  INIT=3
};

// TODO rename to TxPieceData? Seems a bad name. Should figure out a better name.
class SimpleCommand: public CmdData {
 public:
  // removed the dead `CmdData* root_`
  // back-pointer.  It was written by `TxData::GetReadyPiecesData`
  // and `TxData::GetNextReadySubCmd` (procedure.cc:288, 329) but
  // never read by anything; the matching `RootCmd()` accessor and
  // the `Clone()` override that copy-constructed it were equally
  // unused.  See the companion comment on `CmdData::Clone` in
  // `command.h` for the full audit.
  uint64_t timestamp_{0};
  int32_t rank_{RANK_UNDEFINED};
  TxWorkspace input{};
  map<int32_t, Value> output{};
  int32_t output_size = 0;
  parid_t partition_id_ = 0xFFFFFFFF;
//  int32_t __debug_{10};
  virtual parid_t PartitionId() const {
    verify(partition_id_ != 0xFFFFFFFF);
    return partition_id_;
  }
  virtual ~SimpleCommand() {};
};

typedef SimpleCommand TxPieceData;

typedef map<parid_t, vector<shared_ptr<SimpleCommand>>> ReadyPiecesData;

// migrated from Marshallable to Serializable.
// Wire format preserved byte-for-byte:
//   int32_t sp_vec_piece_data_->size()
//   per SimpleCommand: SimpleCommand bytes (via Phase 4d-6 archive op)
//   double time_sent_from_client_
//   bool_t is_recovery_command_
// The nested SimpleCommand serialization uses the Phase 4d-6
// archive operators in `command_marshaler.cc`, which mirror the
// existing Marshal-based ones byte-for-byte.
class VecPieceData : public rrr::Serializable<VecPieceData, MakoCommands> {
 public:
  // TODO move shared_ptr into the vector.
  shared_ptr<vector<shared_ptr<SimpleCommand>>> sp_vec_piece_data_{};
  double time_sent_from_client_ = -1e9; // <0 means null, unit is ms
  bool_t is_recovery_command_ = false; // Flag to indicate this is a recovery command
  VecPieceData() = default;

  void save(BinaryWriteArchive& ar) const {
    verify(sp_vec_piece_data_);
    rrr::Serialize_::serialize(static_cast<int32_t>(sp_vec_piece_data_->size()), ar);
    for (const auto& sp : *sp_vec_piece_data_) {
      rrr::Serialize_::serialize(*sp, ar);
    }
    rrr::Serialize_::serialize(time_sent_from_client_, ar);
    rrr::Serialize_::serialize(is_recovery_command_, ar);
  }

  void load(BinaryReadArchive& ar) {
    verify(!sp_vec_piece_data_);
    sp_vec_piece_data_ = std::make_shared<vector<shared_ptr<TxPieceData>>>();
    int32_t sz;
    rrr::Deserialize_::deserialize(sz, ar);
    for (int i = 0; i < sz; i++) {
      auto x = std::make_shared<TxPieceData>();
      rrr::Deserialize_::deserialize(*x, ar);
      sp_vec_piece_data_->push_back(x);
    }
    rrr::Deserialize_::deserialize(time_sent_from_client_, ar);
    rrr::Deserialize_::deserialize(is_recovery_command_, ar);
  }
};

// TypeList-derived kind.
class VecRecData : public rrr::Serializable<VecRecData, MakoCommands> {
 public:
  // TODO move shared_ptr into the vector.
  shared_ptr<vector<key_t>> key_data_{};
  VecRecData() = default;

  void save(BinaryWriteArchive& ar) const {
    verify(key_data_);
    rrr::Serialize_::serialize(static_cast<int32_t>(key_data_->size()), ar);
    for (const key_t& k : *key_data_) {
      rrr::Serialize_::serialize(k, ar);
    }
  }

  void load(BinaryReadArchive& ar) {
    verify(!key_data_);
    key_data_ = std::make_shared<vector<key_t>>();
    int32_t sz;
    rrr::Deserialize_::deserialize(sz, ar);
    for (int i = 0; i < sz; i++) {
      key_t x;
      rrr::Deserialize_::deserialize(x, ar);
      key_data_->push_back(x);
    }
  }
};

// TypeList-derived kind.
class ViewData : public rrr::Serializable<ViewData, MakoCommands> {
 public:
  View view_;
  parid_t partition_id_ = 0; // partition id for which this view applies

  ViewData() = default;

  explicit ViewData(const View& view) : view_(view) {}

  ViewData(const View& view, parid_t pid) : view_(view), partition_id_(pid) {}

  // Get the embedded View
  const View& GetView() const { return view_; }
  View& GetView() { return view_; }

  void save(BinaryWriteArchive& ar) const {
    rrr::Serialize_::serialize(view_.n_, ar);
    rrr::Serialize_::serialize(view_.view_id_, ar);
    rrr::Serialize_::serialize(view_.timestamp_, ar);
    rrr::Serialize_::serialize(static_cast<int32_t>(view_.leaders_.size()), ar);
    for (int leader : view_.leaders_) {
      rrr::Serialize_::serialize(leader, ar);
    }
    rrr::Serialize_::serialize(partition_id_, ar);
  }

  void load(BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(view_.n_, ar);
    rrr::Deserialize_::deserialize(view_.view_id_, ar);
    rrr::Deserialize_::deserialize(view_.timestamp_, ar);
    int32_t leader_count;
    rrr::Deserialize_::deserialize(leader_count, ar);
    view_.leaders_.clear();
    view_.leaders_.reserve(leader_count);
    for (int i = 0; i < leader_count; i++) {
      int leader;
      rrr::Deserialize_::deserialize(leader, ar);
      view_.leaders_.push_back(leader);
    }
    rrr::Deserialize_::deserialize(partition_id_, ar);
  }

  std::string ToString() const {
    return "ViewData{partition=" + std::to_string(partition_id_) +
           ", " + view_.ToString() + "}";
  }
};

// TypeList-derived kind. Uses Phase 3f-prep
// nested-MarshallDeputy archive operators for the per-entry command
// payloads.
//
// `commands_` migrated from
// `vector<shared_ptr<Marshallable>>` to `vector<Command>`.
// external API (AddEntry / GetCommand)
// also uses Command directly; shared_ptr<Marshallable> callers
// auto-convert via Command's implicit ctor.  save/load drives
// Command archive ops directly; wire format unchanged.
class KeyCmdBatchData : public rrr::Serializable<KeyCmdBatchData,
                                                 MakoCommands> {
 public:
  std::vector<key_t> keys_;
  std::vector<Command> commands_;

  KeyCmdBatchData() = default;

  void AddEntry(key_t key, const Command& cmd) {
    if (!cmd.has_value()) {
      return;
    }
    keys_.push_back(key);
    commands_.push_back(cmd);
  }

  size_t Size() const {
    verify(keys_.size() == commands_.size());
    return commands_.size();
  }

  key_t GetKey(size_t idx) const {
    verify(idx < keys_.size());
    return keys_[idx];
  }

  const Command& GetCommand(size_t idx) const {
    verify(idx < commands_.size());
    return commands_[idx];
  }

  void save(BinaryWriteArchive& ar) const {
    verify(keys_.size() == commands_.size());
    int32_t sz = commands_.size();
    rrr::Serialize_::serialize(sz, ar);
    for (int32_t i = 0; i < sz; i++) {
      rrr::Serialize_::serialize(keys_[i], ar);
      // drive Command's archive op directly (same wire
      // format as the previous `MarshallDeputy(commands_[i])` round-trip).
      rrr::Serialize_::serialize(commands_[i], ar);
    }
  }

  void load(BinaryReadArchive& ar) {
    int32_t sz = 0;
    rrr::Deserialize_::deserialize(sz, ar);
    keys_.resize(sz);
    commands_.resize(sz);
    for (int32_t i = 0; i < sz; i++) {
      rrr::Deserialize_::deserialize(keys_[i], ar);
      rrr::Deserialize_::deserialize(commands_[i], ar);
    }
  }
};

/**
 * input ready levels:
 *   1. shard ready
 *   2. conflict ready
 *   3. all (execute) ready
 */
class TxData: public CmdData {
 private:
  static inline bool is_consistent(map<int32_t, Value> &previous,
                                   map<int32_t, Value> &current) {
    if (current.size() != previous.size())
      return false;
    for (size_t i = 0; i < current.size(); i++)
      if (current[i] != previous[i])
        return false;
    return true;
  }
  map<innid_t, TxWorkspace> inputs_ = {};  // input of each piece.
 public:
  // removed `bool read_only_failed_ = false;`
  // — the field was reset in `TxData::TxData()` and inside the now-
  // deleted `read_only_reset()` but never read by anything.  The
  // only `read_only_failed_ = true` writers were already
  // commented-out code in procedure.cc.
  double pre_time_ = 0.0;
  bool early_return_ = false;
 public:
  // removed protected `ChooseRandom<T>`
  // template — defined here but never instantiated anywhere in the
  // codebase (`grep ChooseRandom` returned only the definition).
  txnid_t txn_id_; // TODO obsolete
  uint64_t timestamp_ = 0;
  TxWorkspace ws_ = {}; // workspace.
  TxWorkspace ws_init_ = {};
  TxnOutput outputs_ = {};
  map<int32_t, int32_t> output_size_ = {};
  map<int32_t, cmdtype_t> p_types_ = {};                  // types of each piece.
  map<int32_t, parid_t> sharding_ = {};
  map<int32_t, int32_t> status_ = {}; // -1 waiting; 0 ready; 1 ongoing; 2
  map<innid_t, rank_t> ranks_ = {};  // input of each piece.
  // finished;
  map<int32_t, shared_ptr<TxPieceData>> map_piece_data_ = {};
  std::set<parid_t> partition_ids_ = {};
  std::atomic<bool> commit_ ;

  /** server involved */
  int n_pieces_all_ = 0;
  int n_pieces_dispatchable_ = 0;
  int n_pieces_dispatch_acked_ = 0;
  int n_pieces_dispatched_ = 0;
  // removed `int n_finished_ = 0;` — the
  // field was previously serialized in the legacy
  // TxData::to_marshal/from_marshal pair (deleted in Phase 5b-1) but
  // never written or read by any other code.

  int max_try_ = 0;
  int n_try_ = 0;

  // removed `bool validation_ok_{true};`
  // (no writer or reader anywhere) and `bool need_validation_{false};`
  // (the only writer was `tx_data().need_validation_ = true;` at
  // `rcc/coord.cc:86`, no readers; that write was removed in this
  // commit).

  weak_ptr<TxnRegistry> txn_reg_{};
  Sharding *sss_ = nullptr;

  rusty::Function<void(TxReply &)> callback_;
  TxReply reply_;
  struct timespec start_time_;

  TxData();

  virtual void Init(TxRequest &req) = 0 ;

  // phase 1, res is NULL
  // phase 2, res returns SUCCESS is output is consistent with previous value
  virtual bool HandleOutput(int pi,
                            int res,
                            map<int32_t, Value> &output) = 0;
  virtual bool IsReadOnly() = 0;
  // removed several dead virtual methods —
  //   `read_only_reset()`, `IsFinished()`, `Merge(TxnOutput&)`, and
  //   `GetNextReadySubCmd()` — none had any production callers. Each
  //   was either a `verify(0)` stub on the base class with no
  //   subclass override (`IsFinished`, `GetNextReadySubCmd`) or a
  //   helper whose only call sites were already commented-out
  //   (`read_only_reset`, `Merge(TxnOutput&)` were referenced only by
  //   commented-out code in `snow/ro6_coord.cc`, `rcc/coord.cc`,
  //   `janus/coordinator.cc`).
  virtual int GetNPieceAll() {
    return n_pieces_all_;
  }
  virtual bool OutputReady();
  virtual void Merge(CmdData&) override;
  virtual void Merge(innid_t inn_id, map<int32_t, Value>& output);
  virtual bool HasMoreUnsentPiece();
  virtual ReadyPiecesData GetReadyPiecesData(int32_t max = 0);
  virtual set<parid_t>& GetPartitionIds() override;
  TxWorkspace& GetWorkspace(innid_t inn_id) {
    verify(inn_id != 0);
    TxWorkspace& ws = inputs_[inn_id];
    if (ws.values_->size() == 0)
      ws.values_ = ws_.values_;
    return ws;
  }

  virtual parid_t GetPiecePartitionId(innid_t inn_id) {
    verify(sharding_.find(inn_id) != sharding_.end());
    return sharding_[inn_id];
  }
  virtual bool IsOneRound();
  vector<SimpleCommand> GetCmdsByPartition(parid_t par_id);
  // removed
  // `vector<SimpleCommand> GetCmdsByPartitionAndRank(parid_t, rank_t)`
  // — declared and defined but never called anywhere.

  // removed dead TxData::to_marshal/from_marshal
  // overrides (never invoked in production). The Marshallable base's
  // `verify(0)` defaults remain for any unintentionally surviving
  // virtual-dispatch path.

  // removed `inline bool can_retry()` —
  // defined but never called.  Removed `inline void
  // disable_early_return()` — only call sites were commented-out
  // code in `snow/ro6_coord.cc:57` and `rcc/coord.cc:105`.

  inline bool do_early_return() {
    return early_return_;
  }

  double last_attempt_latency();

  TxReply &get_reply();

  /** for retry */
  virtual void Reset() override;

  virtual ~TxData() {}
};

} // namespace rcc

// removed an empty `namespace rrr {}` block
// at the bottom of this header — companion to the Phase 4e-2 cleanup
// of the same shape in `tpc_command.h`.  The block previously held
// `TypedMarshallableAdapterTraits<T>` specializations for VecRecData
// / ViewData / KeyCmdBatchData / VecPieceData; the traits machinery
// went away in Phase 5b-5 and the per-type registrations now live in
// `procedure.cc` via `reg_serializable_in_deputy`.
