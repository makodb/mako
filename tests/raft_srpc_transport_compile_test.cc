// Compile-only smoke test for the Phase 2 SrpcTransportAdapter. We do not
// construct RaftCommo here (that would drag in the whole communicator
// stack); instead we take the address of make_srpc_transport and verify
// the adapter's class template instantiations compile. The
// verify(0)-stubbed quorum RPCs are never called at runtime.

#include <gtest/gtest.h>

#include "deptran/raft/srpc_transport.hpp"

using namespace janus::raft;

TEST(RaftSrpcTransportCompileTest, FactorySymbolLinks) {
  // Force the compiler to emit make_srpc_transport's instantiation by
  // taking its address. If SrpcTransportAdapter fails to satisfy
  // TransportBase, this test file won't link.
  auto* factory = &make_srpc_transport;
  EXPECT_NE(reinterpret_cast<void*>(factory), nullptr);
}
