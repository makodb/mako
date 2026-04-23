// Compile-only smoke test for the Phase 2 RrrTransportAdapter. We do not
// construct RaftCommo here (that would drag in the whole communicator
// stack); instead we take the address of make_rrr_transport and verify
// the adapter's class template instantiations compile. The
// verify(0)-stubbed quorum RPCs are never called at runtime.

#include <gtest/gtest.h>

#include "deptran/raft/rrr_transport.hpp"

using namespace janus::raft;

TEST(RaftRrrTransportCompileTest, FactorySymbolLinks) {
  // Force the compiler to emit make_rrr_transport's instantiation by
  // taking its address. If RrrTransportAdapter fails to satisfy
  // TransportFacade, pro::make_proxy won't compile and this test file
  // won't link.
  auto* factory = &make_rrr_transport;
  EXPECT_NE(reinterpret_cast<void*>(factory), nullptr);
}
