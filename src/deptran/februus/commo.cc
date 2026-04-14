#include "commo.h"

using namespace janus;

// TODO change the event reference to weak ptr? because the event could be
// destroyed before accessed if a quorum is satisfied early.
void CommoFebruus::BroadcastPreAccept(QuorumEvent& e,
                                      parid_t par_id,
                                      txid_t tx_id) {
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());

  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [&e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      uint64_t timestamp;
      fu->get_reply() >> res >> timestamp;
      e.vec_timestamp_.push_back(timestamp);
      e.vote_yes();
    };
    verify(tx_id > 0);
    ClassicProxy::RpcPreAcceptFebruusRequest req;
    req.tx_id = tx_id;
    auto fu_result = proxy->async_PreAcceptFebruus(req, fuattr);
    // Arc auto-released
  }
}

void CommoFebruus::BroadcastAccept(QuorumEvent& e,
                                   parid_t par_id,
                                   txid_t tx_id,
                                   ballot_t ballot,
                                   uint64_t timestamp) {
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());

  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [&e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      fu->get_reply() >> res;
      e.vote_yes();
    };
    verify(tx_id > 0);
    ClassicProxy::RpcAcceptFebruusRequest req;
    req.tx_id = tx_id;
    req.ballot = ballot;
    req.timestamp = timestamp;
    auto fu_result = proxy->async_AcceptFebruus(req, fuattr);
    // Arc auto-released
  }
}

void CommoFebruus::BroadcastCommit(const set<parid_t>& set_par_id,
                                   txid_t tx_id,
                                   uint64_t timestamp) {
  for (auto par_id : set_par_id) {
    verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
    for (auto& p : rpc_par_proxies_[par_id]) {
      auto proxy = (p.second);
      verify(proxy != nullptr);
      FutureAttr fuattr;
      fuattr.callback = [](rusty::Arc<Future> fu) {
        int32_t res;
        fu->get_reply() >> res;
      };
      verify(tx_id > 0);

      ClassicProxy::RpcCommitFebruusRequest req;
      req.tx_id = tx_id;
      req.timestamp = timestamp;
      auto fu_result = proxy->async_CommitFebruus(req, fuattr);
      // Arc auto-released
    }
  }
}
