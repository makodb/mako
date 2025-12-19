#include <memory>

#pragma once

#include "__dep__.h"
#include "command.h"
#include "rcc/graph.h"
#include "command_marshaler.h"
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
  std::shared_ptr<ViewData> sp_view_data_ = nullptr;
};

class TxWorkspace {
 public:
  set<int32_t> keys_ = {};
  std::shared_ptr<map<int32_t, Value>> values_{};
  std::shared_ptr<map<int32_t, shared_ptr<IntEvent>>> value_events_{};
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
    auto e = Reactor::CreateSpEvent<Event>();
    e->Wait([this, k](int x)->bool{
      auto it = this->values_->find(k);
      return (it != this->values_->end());
    });
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
  function<void(TxReply &)> callback_ = [] (TxReply&)->void {verify(0);};
  function<void()> fail_callback_ = [] () {
    verify(0);
  };
  void get_log(i64 tid, std::string &log);
  
};

Marshal& operator << (Marshal& m, const TxWorkspace &ws);

Marshal& operator >> (Marshal& m, TxWorkspace& ws);

Marshal& operator << (Marshal& m, const TxReply& reply);

Marshal& operator >> (Marshal& m, TxReply& reply);

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
  CmdData* root_ = nullptr;
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
  virtual CmdData* RootCmd() const {return root_;}
  virtual CmdData* Clone() const override {
    SimpleCommand* cmd = new SimpleCommand();
    *cmd = *this;
    return cmd;
  }
  virtual ~SimpleCommand() {};
};

typedef SimpleCommand TxPieceData;

typedef map<parid_t, vector<shared_ptr<SimpleCommand>>> ReadyPiecesData;

class VecPieceData : public Marshallable {
 public:
  // TODO move shared_ptr into the vector.
  shared_ptr<vector<shared_ptr<SimpleCommand>>> sp_vec_piece_data_{};
  double time_sent_from_client_ = -1e9; // <0 means null, unit is ms
  bool_t is_recovery_command_ = false; // Flag to indicate this is a recovery command
  VecPieceData() : Marshallable(MarshallDeputy::CMD_VEC_PIECE) {

  }

  Marshal& ToMarshal(Marshal& m) const override {
    verify(sp_vec_piece_data_);
    m << (int32_t) sp_vec_piece_data_->size();
    for (auto sp : *sp_vec_piece_data_) {
      m << *sp;
    }
    m << time_sent_from_client_;
    m << is_recovery_command_;
//    m << *sp_vec_piece_data_;
    return m;
  }

  Marshal& FromMarshal(Marshal& m) override {
    verify(!sp_vec_piece_data_);
    sp_vec_piece_data_ = std::make_shared<vector<shared_ptr<TxPieceData>>>();
    int32_t sz;
    m >> sz;
    for (int i = 0; i < sz; i++) {
      auto x = std::make_shared<TxPieceData>();
      m >> *x;
      sp_vec_piece_data_->push_back(x);
    }
    m >> time_sent_from_client_;
    m >> is_recovery_command_;
//    m >> *sp_vec_piece_data_;
    return m;
  }
};

class VecRecData : public Marshallable {
 public:
  // TODO move shared_ptr into the vector.
  shared_ptr<vector<key_t>> key_data_{};
  VecRecData() : Marshallable(MarshallDeputy::CMD_REC_VEC) {

  }

  Marshal& ToMarshal(Marshal& m) const override {
    verify(key_data_);
    m << (int32_t) key_data_->size();
    for (const key_t& k: *key_data_) {
      m << k;
    }
//    m << *key_data_;
    return m;
  }

  Marshal& FromMarshal(Marshal& m) override {
    verify(!key_data_);
    key_data_ = std::make_shared<vector<key_t>>();
    int32_t sz;
    m >> sz;
    for (int i = 0; i < sz; i++) {
      key_t x;
      m >> x;
      key_data_->push_back(x);
    }
//    m >> *key_data_;
    return m;
  }
};

class ViewData : public Marshallable {
 public:
  View view_;
  parid_t partition_id_ = 0; // partition id for which this view applies
  
  ViewData() : Marshallable(MarshallDeputy::CMD_VIEW_DATA) {}
  
  ViewData(const View& view) : Marshallable(MarshallDeputy::CMD_VIEW_DATA), view_(view) {}
  
  ViewData(const View& view, parid_t pid) : Marshallable(MarshallDeputy::CMD_VIEW_DATA), view_(view), partition_id_(pid) {}
  
  // Get the embedded View
  const View& GetView() const { return view_; }
  View& GetView() { return view_; }
  
  Marshal& ToMarshal(Marshal& m) const override {
    m << view_.n_;
    m << view_.view_id_;
    m << view_.timestamp_;
    m << (int32_t)view_.leaders_.size();
    for (int leader : view_.leaders_) {
      m << leader;
    }
    m << partition_id_;
    return m;
  }
  
  Marshal& FromMarshal(Marshal& m) override {
    m >> view_.n_;
    m >> view_.view_id_;
    m >> view_.timestamp_;
    int32_t leader_count;
    m >> leader_count;
    view_.leaders_.clear();
    view_.leaders_.reserve(leader_count);
    for (int i = 0; i < leader_count; i++) {
      int leader;
      m >> leader;
      view_.leaders_.push_back(leader);
    }
    m >> partition_id_;
    return m;
  }
  
  std::string ToString() const {
    return "ViewData{partition=" + std::to_string(partition_id_) + 
           ", " + view_.ToString() + "}";
  }
};

class KeyCmdBatchData : public Marshallable {
 public:
  std::vector<key_t> keys_;
  std::vector<shared_ptr<Marshallable>> commands_;

  KeyCmdBatchData() : Marshallable(MarshallDeputy::CMD_KEY_CMD_BATCH) {}

  void AddEntry(key_t key, const shared_ptr<Marshallable>& cmd) {
    if (!cmd) {
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

  shared_ptr<Marshallable> GetCommand(size_t idx) const {
    verify(idx < commands_.size());
    return commands_[idx];
  }

  Marshal& ToMarshal(Marshal& m) const override {
    verify(keys_.size() == commands_.size());
    int32_t sz = commands_.size();
    m << sz;
    for (int32_t i = 0; i < sz; i++) {
      m << keys_[i];
      MarshallDeputy deputy;
      deputy.SetMarshallable(commands_[i]);
      m << deputy;
    }
    return m;
  }

  Marshal& FromMarshal(Marshal& m) override {
    int32_t sz = 0;
    m >> sz;
    keys_.resize(sz);
    commands_.resize(sz);
    for (int32_t i = 0; i < sz; i++) {
      m >> keys_[i];
      MarshallDeputy deputy;
      m >> deputy;
      commands_[i] = deputy.sp_data_;
    }
    return m;
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
  bool read_only_failed_ = false;
  double pre_time_ = 0.0;
  bool early_return_ = false;
 protected:
  template<class T>
  T ChooseRandom(const std::vector<T>& v) {
    return v[rrr::RandomGenerator::rand(0,v.size()-1)];
  }
 public:
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
  /** finished pieces counting */
  int n_finished_ = 0;

  int max_try_ = 0;
  int n_try_ = 0;

  bool validation_ok_{true};
  bool need_validation_{false};

  weak_ptr<TxnRegistry> txn_reg_{};
  Sharding *sss_ = nullptr;

  std::function<void(TxReply &)> callback_;
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
  virtual void read_only_reset();
  virtual int GetNPieceAll() {
    return n_pieces_all_;
  }
  virtual bool OutputReady();
  virtual bool IsFinished(){verify(0);}
  virtual void Merge(CmdData&) override;
  virtual void Merge(innid_t inn_id, map<int32_t, Value>& output);
  virtual void Merge(TxnOutput& output);
  virtual bool HasMoreUnsentPiece();
  virtual shared_ptr<TxPieceData> GetNextReadySubCmd();
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
  vector<SimpleCommand> GetCmdsByPartitionAndRank(parid_t par_id, rank_t rank);

  Marshal& ToMarshal(Marshal& m) const override;
  Marshal& FromMarshal(Marshal& m) override;

  inline bool can_retry() {
    return (max_try_ == 0 || n_try_ < max_try_);
  }

  inline bool do_early_return() {
    return early_return_;
  }

  inline void disable_early_return() {
    early_return_ = false;
  }

  double last_attempt_latency();

  TxReply &get_reply();

  /** for retry */
  virtual void Reset() override;

  virtual ~TxData() {}
};

} // namespace rcc
