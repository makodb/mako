#pragma once

#include "graph.h"
#include "tx.h"
#include "marshal-value.h"
#include "command.h"
#include "command_marshaler.h"
#include "__dep__.h"

/**
 * This is NOT thread safe!!!
 */
namespace janus {

//typedef RccDTxn RccDTxn;
typedef vector<RccTx*> RccScc;

class EmptyGraph {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::EMPTY_GRAPH;

  EmptyGraph() = default;
  Marshal& to_marshal(Marshal& m) const { return m; }
  Marshal& from_marshal(Marshal& m) { return m; }
};

class RccServer;
class RccGraph : public Graph<RccTx> {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::RCC_GRAPH;
//    Graph<PieInfo> pie_gra_;
//  Graph <TxnInfo> txn_gra_;
  RccServer* sched_{nullptr};
  bool empty_{false};
//  parid_t partition_id_ = 0; // TODO
//  std::vector<rrr::Client *> rpc_clients_;
//  std::vector<RococoProxy *> rpc_proxies_;
//  std::vector<std::string> server_addrs_;

  RccGraph() : Graph<RccTx>() {}

  virtual ~RccGraph() {
    // XXX hopefully some memory leak here does not hurt. :(
  }

  /** on start_req */
  shared_ptr<RccTx> FindOrCreateRccVertex(txnid_t txn_id,
                                          RccServer* sched);
  void RemoveVertex(txnid_t txn_id);
  void RebuildEdgePointer(map<txnid_t, shared_ptr<RccTx>>& index);
  shared_ptr<RccTx> AggregateVertex(shared_ptr<RccTx> rhs_dtxn);
  void UpgradeStatus(RccTx& v, int rank, int8_t status);

  virtual map<txnid_t, shared_ptr<RccTx>> Aggregate(epoch_t epoch, RccGraph& graph);
  void SelectGraphCmtUkn(RccTx& dtxn, shared_ptr<RccGraph> new_graph);
  void SelectGraph(set<shared_ptr<RccTx>> vertexes, RccGraph* new_graph);
//  RccScc& FindSCC(RccDTxn *vertex) override;
  bool AllAncCmt(shared_ptr<RccTx> vertex);

  bool operator== (RccGraph& rhs) const;

  bool operator!= (RccGraph& rhs) const {
    // TODO
    return !(*this == rhs);
  }

  uint64_t MinItfrGraph(RccTx& dtxn,
                        shared_ptr<RccGraph> gra_m,
                        bool quick = false,
                        int depth = -1);

  bool HasICycle(const RccScc& scc);


//  Marshal& to_marshal(Marshal& m) const override;
//  Marshal& from_marshal(Marshal& m) override;

};
} // namespace janus

namespace rrr {

template <>
struct TypedMarshallableAdapterTraits<janus::EmptyGraph> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::EmptyGraph, MarshallDeputy::EMPTY_GRAPH>;
};

template <>
struct TypedMarshallableAdapterTraits<janus::RccGraph> {
  static constexpr bool kEnabled = true;
  using Adapter =
      TypedMarshallableAdapter<janus::RccGraph, MarshallDeputy::RCC_GRAPH>;
};

}  // namespace rrr
