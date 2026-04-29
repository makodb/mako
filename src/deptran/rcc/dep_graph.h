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

// Workstream N Phase 4d-1: migrated from Marshallable to Serializable.
// Stateless tag — no fields, save/load are no-ops. Construction
// continues to use the existing MarshallDeputy(shared_ptr<T>) ctor
// and set_marshallable<T> templates (Phase 4d-prep relaxed their
// requires clauses to dispatch via wrap_typed_marshallable's bridge
// overload for SerializableConcept T).
class EmptyGraph {
 public:
  static constexpr int32_t kMarshallKind = MarshallDeputy::EMPTY_GRAPH;

  EmptyGraph() = default;

  int32_t kind() const { return kMarshallKind; }
  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
};

class RccServer;
// Workstream N Phase 4d-5: migrated from Marshallable to Serializable.
// The base `Graph<RccTx>` carries a `to_marshal`/`from_marshal` pair
// (graph.h:966) that emits `uint64_t size` followed by per-vertex
// (id, *vertex) pairs. RccTx's `operator<<`/`operator>>` are
// `verify(0)` stubs (tx.h:353-365), so in practice only the empty
// graph (size==0) ever round-trips on the wire — confirmed by
// `RccGraphRoundTripUsesTypedAdapter` in
// `rpc_marshallable_proxy_test.cc`. Migration preserves that
// behavior: `save`/`load` write `uint64_t 0` and `verify(n == 0)` on
// read, byte-for-byte identical to the legacy encoding for the
// only inputs that ever worked.
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

  int32_t kind() const { return kMarshallKind; }

  void save(BinaryWriteArchive& ar) const {
    uint64_t n = size();
    verify(n == 0);  // RccTx archive operators are not implemented;
                     // only empty graphs ever round-trip in practice.
    ar << n;
  }

  void load(BinaryReadArchive& ar) {
    verify(size() == 0);
    uint64_t n;
    ar >> n;
    verify(n == 0);  // matches the legacy `m >> *v` verify(0) stub
                     // for non-empty graphs.
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

// (EmptyGraph and RccGraph are both Serializables now — no
// TypedMarshallableAdapter traits. Construction sites continue to
// use `MarshallDeputy(make_shared<T>())` / `set_marshallable<T>` /
// `wrap_typed_marshallable<T>` transparently via the Phase 4d-prep
// bridge dispatch. Cast sites continue to use
// `marshallable_cast<T>` transparently via the bridge.)
