#pragma once
#include "../__dep__.h"
#include "graph.h"
#include "dep_graph.h"

namespace janus {
// Phase 8 batch 4 exemplar: the ADL serialize free function owns the wire
// format; the operator below is a forwarder kept only until the operator
// layer is deleted (the catch-alls still bridge through operators).
template<typename T>
inline void serialize(const Vertex<T> *&v, rrr::Marshal &m) {
  verify(0);
  int64_t u = std::uintptr_t(v);
  rrr::Serialize_::serialize(u, m);
}

template<typename T>
inline rrr::Marshal &operator<<(rrr::Marshal &m, const Vertex<T> *&v) { serialize(v, m); return m; }

} // namespace janus
