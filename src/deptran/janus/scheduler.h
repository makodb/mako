
#pragma once
#include <rusty/arc.hpp>
#include "../rcc/server.h"

namespace janus {

class RccGraph;
class JanusCommo;
class SchedulerJanus : public RccServer {
 public:
  using RccServer::RccServer;

  map<txnid_t, shared_ptr<RccTx>> Aggregate(RccGraph& graph) override;

  virtual int OnPreAccept(txnid_t txnid,
                          rank_t rank,
                          const vector<SimpleCommand> &cmds,
                          rusty::Arc<RccGraph> sp_graph,
                          rusty::Arc<RccGraph> res_graph);

  void OnAccept(txnid_t txn_id,
                int rank,
                const ballot_t& ballot,
                rusty::Arc<RccGraph> graph,
                int32_t* res);

//  int OnCommit(txnid_t txn_id,
//               rank_t rank,
//               bool need_validation,
//               shared_ptr<RccGraph> sp_graph,
//               TxnOutput *output) override;

  JanusCommo* commo();

};
} // namespace janus
