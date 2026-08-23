#include "__dep__.h"
#include "frame.h"
#include "config.h"
#include "marshal-value.h"
#include "coordinator.h"
#include "tx.h"
#include "scheduler.h"
#include "sharding.h"
#include "benchmark_registry.h"

#include "paxos/frame.h"


namespace janus {

Frame* CreateRaftFrameBuiltin(int mode);

Frame* Frame::RegFrame(int mode,
                       function<Frame*()> frame_init) {
  auto& mode_to_frame = Frame::ModeToFrame();
  auto it = mode_to_frame.find(mode);
  verify(it == mode_to_frame.end());
  mode_to_frame[mode] = frame_init;
  return frame_init();
}

Frame* Frame::GetFrame(int mode) {
  return GetFrame(mode, -1);
}

Frame* Frame::GetFrame(int mode, int replica_mode) {
  Frame *frame = nullptr;
  // some built-in mode
  switch (mode) {
    case MODE_MULTI_PAXOS:
      frame = new MultiPaxosFrame(mode);
      break;
    case MODE_RAFT:
      frame = CreateRaftFrameBuiltin(mode);
      break;
    default:
      auto& mode_to_frame = Frame::ModeToFrame();
      auto it = mode_to_frame.find(mode);
      verify(it != mode_to_frame.end());
      frame = it->second();
  }

  return frame;
}

int Frame::Name2Mode(string name) {
  auto &m = Frame::FrameNameToMode();
  auto it = m.find(name);
  verify(it != m.end());
  return it->second;
}

Frame* Frame::GetFrame(string name) {
  return GetFrame(Name2Mode(name));
}

Frame* Frame::RegFrame(int mode,
                       vector<string> names,
                       function<Frame*()> frame) {
  for (auto name: names) {
    //verify(frame_name_mode_s.find(name) == frame_name_mode_s.end());
    auto &m = Frame::FrameNameToMode();
    m[name] = mode;
  }
  return RegFrame(mode, frame);
}

Sharding* Frame::CreateSharding() {
  EnsureBenchmarkRegistryInitialized();
  auto& registry = BenchmarkRegistry::Instance();
  Sharding* ret = nullptr;
  auto bench = Config::config_s->benchmark_;
  ret = registry.CreateSharding(bench);
  verify(ret != nullptr);
  return ret;
}

Sharding* Frame::CreateSharding(Sharding *sd) {
  verify(sd != nullptr);
  Sharding* ret = CreateSharding();
  *ret = *sd;
  ret->frame_ = this;
  return ret;
}

mdb::Row* Frame::CreateRow(const mdb::Schema *schema,
                           vector<Value> &row_data) {
  return mdb::VersionedRow::create(schema, row_data);
}

Coordinator* Frame::CreateBulkCoordinator(Config *config, int benchmark) {
  verify(0);
  Coordinator *coo;
  return coo;
}

void Frame::GetTxTypes(std::map<int32_t, std::string>& txn_types) {
  EnsureBenchmarkRegistryInitialized();
  auto benchmark_ = Config::config_s->benchmark_;
  txn_types = BenchmarkRegistry::Instance().GetTxnTypes(benchmark_);
  verify(!txn_types.empty());
}

TxData* Frame::CreateTxnCommand(TxRequest& req, shared_ptr<TxnRegistry> reg) {
  EnsureBenchmarkRegistryInitialized();
  auto& registry = BenchmarkRegistry::Instance();
  auto benchmark = Config::config_s->benchmark_;
  TxData *cmd = registry.CreateTxn(benchmark);
  verify(cmd != NULL);
  cmd->txn_reg_ = reg;
  cmd->Init(req);
  verify(cmd->n_pieces_dispatchable_ > 0);
  return cmd;
}

//TxData * Frame::CreateChopper(TxRequest &req, TxnRegistry* reg) {
//  return CreateTxnCommand(req, reg);
//}

Communicator* Frame::CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
  commo_ = new Communicator(std::move(poll_thread_worker));
  return commo_;
}

shared_ptr<Tx> Frame::CreateTx(epoch_t epoch, txnid_t tid,
                               bool ro, TxLogServer *mgr) {
  (void)epoch;
  (void)tid;
  (void)ro;
  (void)mgr;
  Log_error("transaction-engine Tx creation is not supported by replication-only frames");
  verify(0);
  return nullptr;
}

Workload * Frame::CreateTxGenerator() {
  EnsureBenchmarkRegistryInitialized();
  auto& registry = BenchmarkRegistry::Instance();
  auto benchmark = Config::config_s->benchmark_;
  Workload * gen = registry.CreateTxGenerator(benchmark, Config::GetConfig());
  verify(gen != nullptr);
  return gen;
}

map<string, int> &Frame::FrameNameToMode() {
  static map<string, int> frame_name_mode_s = {
      {"multi_paxos",   MODE_MULTI_PAXOS},
      {"raft",          MODE_RAFT},
  };
  return frame_name_mode_s;
}

map<int, function<Frame*()>> &Frame::ModeToFrame() {
  static map<int, function<Frame*()>> frame_s_ = {};
  return frame_s_;
}
} // namespace janus;
