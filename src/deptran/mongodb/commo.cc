#include "commo.h"

namespace janus {

MongodbCommo::MongodbCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) : Communicator(poll_thread_worker) {
//  verify(poll != nullptr);
}

void MongodbCommo::BroadcastCommit(const parid_t par_id,
                                    const shared_ptr<Marshallable> cmd) {
  auto proxies = rpc_par_proxies_[par_id];
  int n = proxies.size();
  
  for (auto& p : proxies) {
    auto proxy = (MongodbProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](rusty::Arc<Future> fu) {};
    MongodbProxy::RpcCommitRequest req;
    req.cmd = MarshallDeputy(cmd);
    auto f = proxy->async_Commit(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
}

}