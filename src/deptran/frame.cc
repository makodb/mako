#include "__dep__.h"
#include "frame.h"
#include "config.h"
#include "marshal-value.h"
#include "coordinator.h"
#include "tx.h"
#include "service.h"
#include "scheduler.h"
#include "none/coordinator.h"
#include "none/scheduler.h"
#include "occ/tx.h"
#include "occ/coordinator.h"
#include "benchmark_registry.h"

#include "occ/scheduler.h"

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
    case MODE_NONE:
    case MODE_NOTX:
    case MODE_MDCC:
    case MODE_OCC:
      frame = new Frame(mode, replica_mode);
      break;
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
//  auto mode = Config::GetConfig()->cc_mode_;
  auto mode = mode_;
  mdb::Row* r = nullptr;
  switch (mode) {
    case MODE_NONE: // FIXME
    case MODE_MDCC:
    case MODE_OCC:
    default:
      r = mdb::VersionedRow::create(schema, row_data);
      break;
  }
  return r;
}

Coordinator* Frame::CreateCoordinator(cooid_t coo_id,
                                      Config *config,
                                      int benchmark,
                                      rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                      uint32_t id,
                                      shared_ptr<TxnRegistry> txn_reg) {
  // TODO: clean this up; make Coordinator subclasses assign txn_reg_
  Coordinator *coo;
  auto attr = this;
//  auto mode = Config::GetConfig()->cc_mode_;
  auto mode = mode_;
  switch (mode) {
    case MODE_OCC:
    case MODE_RPC_NULL:
      coo = new CoordinatorOcc(coo_id,
                         benchmark,
                         client_status.is_some() ? rusty::Some(client_status.as_ref().unwrap().clone()) : rusty::None,
                         id);
      ((Coordinator*)coo)->txn_reg_ = txn_reg;
      break;
    case MODE_MDCC:
//      coo = (Coordinator*)new mdcc::MdccCoordinator(coo_id, id, config, ccsi);
      break;
    case MODE_NONE:
    default:
      coo = new CoordinatorNone(coo_id,
                          benchmark,
                          client_status.is_some() ? rusty::Some(client_status.as_ref().unwrap().clone()) : rusty::None,
                          id);
      ((Coordinator*)coo)->txn_reg_ = txn_reg;
      break;
  }
  coo->frame_ = this;
  return coo;
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
  cmd->sss_ = Config::GetConfig()->sharding_;
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
  shared_ptr<Tx> sp_tx;
	/*struct timespec begin, end;
	clock_gettime(CLOCK_MONOTONIC, &begin);*/
  Log_debug("enter CreateTx");
	switch (mode_) {
    case MODE_OCC:
      sp_tx.reset(new TxOcc(epoch, tid, mgr));
      break;
    case MODE_MULTI_PAXOS:
    case MODE_RAFT:
      break;
    case MODE_NONE:
    case MODE_NOTX:
    default:
      sp_tx.reset(new TxClassic(epoch, tid, mgr));
      break;
  }
	/*clock_gettime(CLOCK_MONOTONIC, &end);
	Log_info("time of CreateTx on server: {}", end.tv_nsec-begin.tv_nsec);*/
  Log_debug("exit CreateTx, Tx address={}", (void*)sp_tx.get());
  return sp_tx;
}

Executor* Frame::CreateExecutor(cmdid_t cmd_id, TxLogServer* sched) {
  Executor* exec = nullptr;
//  auto mode = Config::GetConfig()->cc_mode_;
//  switch (mode) {
//    case MODE_NONE:
//      verify(0);
//    case MODE_OCC:
//      exec = new OCCExecutor(cmd_id, sched);
//      break;
//    default:
//      verify(0);
//  }
  return exec;
}

TxLogServer* Frame::CreateScheduler() {
  Log_info("enter CreateScheduler, mode={}", Config::GetConfig()->tx_proto_);
  auto mode = Config::GetConfig()->tx_proto_;
  TxLogServer *sch = nullptr;
  switch(mode) {
    case MODE_OCC:
      sch = new SchedulerOcc();
      break;
    case MODE_MDCC:
//      sch = new mdcc::MdccScheduler();
      break;
    case MODE_NOTX:
    case MODE_NONE:
      sch = new SchedulerNone();
      break;
    case MODE_RPC_NULL:
      verify(0);
      break;
    default:
      verify(0);
//      sch = new CustomSched();
  }
  
  verify(sch);
  sch->frame_ = this;
  verify(svr_ == nullptr);
  svr_ = sch;
  return sch;
}

Workload * Frame::CreateTxGenerator() {
  EnsureBenchmarkRegistryInitialized();
  auto& registry = BenchmarkRegistry::Instance();
  auto benchmark = Config::config_s->benchmark_;
  Workload * gen = registry.CreateTxGenerator(benchmark, Config::GetConfig());
  verify(gen != nullptr);
  return gen;
}

vector<rrr::ServiceProxy> Frame::CreateRpcServices(uint32_t site_id,
                                                TxLogServer *dtxn_sched,
                                                rusty::Arc<rrr::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<rrr::ServiceProxy>();
  switch(mode_) {
    case MODE_MDCC:
    case MODE_OCC:
    case MODE_NONE:
    case MODE_NOTX:
    default:
      result.push_back(rrr::make_service_proxy_from_typed_box(rusty::make_box<ClassicServiceImpl>(dtxn_sched, poll_thread_worker)));
      break;
  }
  return result;
}
map<string, int> &Frame::FrameNameToMode() {
  static map<string, int> frame_name_mode_s = {
      {"none",          MODE_NONE},
      {"occ",           MODE_OCC},
      {"notx",          MODE_NOTX},
      {"rpc_null",      MODE_RPC_NULL},
      {"mdcc",          MODE_MDCC},
      {"multi_paxos",   MODE_MULTI_PAXOS},
      {"raft",          MODE_RAFT},
      {"epaxos",        MODE_NOT_READY},
      {"rep_commit",    MODE_NOT_READY},
  };
  return frame_name_mode_s;
}

map<int, function<Frame*()>> &Frame::ModeToFrame() {
  static map<int, function<Frame*()>> frame_s_ = {};
  return frame_s_;
}
} // namespace janus;
