#pragma once
#include "../__dep__.h"
#include "../communicator.h"

namespace janus {
class SimpleCommand;
class RccGraph;
class RccCommo : public Communicator {
 public:
  using Communicator::Communicator;
  virtual void SendDispatch(
      vector<SimpleCommand>& cmd,
      const function<void(int res, TxnOutput& cmd, const RccGraph& graph)>&);

  virtual void SendHandoutRo(
      SimpleCommand& cmd,
      const function<void(int res,
                          SimpleCommand& cmd,
                          map<int, mdb::version_t>& vers)>&);

  virtual void SendFinish(
      parid_t pid,
      txnid_t tid,
      shared_ptr<RccGraph> graph,
      const function<void(map<innid_t, map<int32_t, Value>>& output)>&);

  shared_ptr<map<txid_t, parent_set_t>>
  Inquire(parid_t pid, txnid_t tid, rank_t rank);
  void BroadcastValidation(txid_t, set<parid_t>, int validation);
};

} // namespace janus
