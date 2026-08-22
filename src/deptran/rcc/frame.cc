#include "../constants.h"
#include "frame.h"
//#include "exec.h"
//#include "coord.h"
#include "coord.h"
#include "server.h"
#include "tx.h"
#include "commo.h"
#include "config.h"

namespace janus {

REG_FRAME(MODE_RCC, vector<string>({"rococo","rcc"}), FrameRococo);

Executor *FrameRococo::CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) {
  verify(0);
  Executor *exec = nullptr;
  return exec;
}

Coordinator *FrameRococo::CreateCoordinator(cooid_t coo_id,
                                            Config *config,
                                            int benchmark,
                                            rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                            uint32_t id,
                                            shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  RccCoord *coord = new RccCoord(coo_id,
                                 benchmark,
                                 std::move(client_status),
                                 id);
  coord->txn_reg_ = txn_reg;
  coord->frame_ = this;
  return coord;
}

TxLogServer *FrameRococo::CreateScheduler() {
  TxLogServer *sched = new RccServer();
  sched->frame_ = this;
  return sched;
}

vector<rrr::ServiceProxy>
FrameRococo::CreateRpcServices(uint32_t site_id,
                               TxLogServer *sched,
                               rusty::Arc<rrr::PollThread> poll_thread_worker) {
  return Frame::CreateRpcServices(site_id, sched, poll_thread_worker);
}

mdb::Row *FrameRococo::CreateRow(const mdb::Schema *schema,
                              vector<Value> &row_data) {
  mdb::Row *r = RccRow::create(schema, row_data);
  return r;
}

shared_ptr<Tx> FrameRococo::CreateTx(epoch_t epoch, txnid_t tid,
                                     bool ro, TxLogServer *mgr) {
  shared_ptr<Tx> sp_tx(new RccTx(epoch, tid, mgr, ro));
  return sp_tx;
}

Communicator *FrameRococo::CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll) {
  return new RccCommo(std::move(poll));
}

} // namespace janus
