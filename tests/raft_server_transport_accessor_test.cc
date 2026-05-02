#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "deptran/raft/server.h"

TEST(RaftServerTransportAccessorTest, AccessorReturnsTransportProxyReference) {
  using AccessorReturn = decltype(std::declval<janus::RaftServer&>().transport());
  static_assert(std::is_same_v<AccessorReturn, janus::raft::TransportProxy&>,
                "RaftServer::transport() must return TransportProxy&");
  SUCCEED();
}
