#pragma once

#include "graph.h"
#include "tx.h"
#include "marshal-value.h"
#include "command.h"
#include "command_marshaler.h"
#include "__dep__.h"
// migrated from closed-set TypeList discriminants to
// open-set `AnyMessage` envelope. Pull in the bridge header (for
// `wrap_serializable_aliased`, `marshallable_cast<T>` overloads) and
// `any_message.hpp` for the envelope type.
#include "srpc/misc/serializable.hpp"
#include "srpc/misc/any_message.hpp"

/**
 * This is NOT thread safe!!!
 */
namespace janus {

//typedef RccDTxn RccDTxn;
typedef vector<RccTx*> RccScc;

// graph payloads (`EmptyGraph`, `RccGraph`) moved
// from the closed-set TypeList discriminant pattern to the open-set
// `AnyMessage` envelope.  No central TypeList — each type registers
// under a stable string name and the envelope dispatches by that
// name on the wire.  The Rust analogue is `typetag` (open set,
// versioned string tags) rather than `enum Foo + bincode` (closed
// set, declaration-order discriminants), and the protobuf analogue
// is `google.protobuf.Any` (type-URL) rather than `oneof` (field
// numbers).  See `docs/dev/srpc-book.md` for the design rationale.

// AnyMessage-wrapped Serializable. No `kind()` discriminant —
// AnyMessage's `type_name_` string carries the type identity on the
// wire.  The `kind()` method below is a stub required by the
// `SerializableBase` contract; its return value is never used
// because the surrounding AnyMessage's kind always wins.
class EmptyGraph {
 public:
  EmptyGraph() = default;

  void save(BinaryWriteArchive&) const {}
  void load(BinaryReadArchive&) {}
  int32_t kind() const noexcept { return 0; }
};

class RccServer;
// AnyMessage-wrapped Serializable.  `Graph<RccTx>`
// carries a `to_marshal`/`from_marshal` pair (graph.h:966) that emits
// `uint64_t size` followed by per-vertex (id, *vertex) pairs. RccTx's
// `operator<<`/`operator>>` are `verify(0)` stubs (tx.h:353-365), so
// in practice only the empty graph (size==0) ever round-trips on the
// wire.  Migration preserves that behavior: `save`/`load` write
// `uint64_t 0` and `verify(n == 0)` on read, byte-for-byte identical
// to the legacy encoding for the only inputs that ever worked.
class RccGraph : public Graph<RccTx> {
 public:
//    Graph<PieInfo> pie_gra_;
//  Graph <TxnInfo> txn_gra_;
  RccServer* sched_{nullptr};
  bool empty_{false};
//  parid_t partition_id_ = 0; // TODO
//  std::vector<srpc::Client *> rpc_clients_;
//  std::vector<RococoProxy *> rpc_proxies_;
//  std::vector<std::string> server_addrs_;

  RccGraph() : Graph<RccTx>() {}

  virtual ~RccGraph() {
    // XXX hopefully some memory leak here does not hurt. :(
  }

  // Stub for the SerializableBase contract; AnyMessage carries
  // the type identity on the wire, so this value is never consulted.
  int32_t kind() const noexcept { return 0; }

  void save(BinaryWriteArchive& ar) const {
    uint64_t n = size();
    verify(n == 0);  // RccTx archive operators are not implemented;
                     // only empty graphs ever round-trip in practice.
    srpc::Serialize_::serialize(n, ar);
  }

  void load(BinaryReadArchive& ar) {
    verify(size() == 0);
    uint64_t n;
    srpc::Deserialize_::deserialize(n, ar);
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
