#include <stddef.h>

#include <rusty/option.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>


#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

class CountingService : public Service {
 public:
  static constexpr i32 RPC_ID = 0x7A01;

  CountingService(std::atomic<int>* reg_calls,
                  std::atomic<int>* dispatch_calls,
                  size_t* last_svc_index,
                  i32* last_rpc_id,
                  i64* last_xid)
      : reg_calls_(reg_calls),
        dispatch_calls_(dispatch_calls),
        last_svc_index_(last_svc_index),
        last_rpc_id_(last_rpc_id),
        last_xid_(last_xid) {}

  int __reg_to__(Server& svr, size_t svc_index) override {
    if (reg_calls_ != nullptr) {
      reg_calls_->fetch_add(1, std::memory_order_relaxed);
    }
    if (last_svc_index_ != nullptr) {
      *last_svc_index_ = svc_index;
    }
    return svr.reg_rpc(RPC_ID, svc_index);
  }

  void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection) override {
    if (dispatch_calls_ != nullptr) {
      dispatch_calls_->fetch_add(1, std::memory_order_relaxed);
    }
    if (last_rpc_id_ != nullptr) {
      *last_rpc_id_ = rpc_id;
    }
    if (last_xid_ != nullptr && req) {
      *last_xid_ = req->xid;
    }
  }

 private:
  std::atomic<int>* reg_calls_;
  std::atomic<int>* dispatch_calls_;
  size_t* last_svc_index_;
  i32* last_rpc_id_;
  i64* last_xid_;
};

class TypedCountingService {
 public:
  static constexpr i32 RPC_ID = 0x7A02;

  TypedCountingService(std::atomic<int>* reg_calls,
                       std::atomic<int>* dispatch_calls,
                       size_t* last_svc_index,
                       i32* last_rpc_id,
                       i64* last_xid)
      : reg_calls_(reg_calls),
        dispatch_calls_(dispatch_calls),
        last_svc_index_(last_svc_index),
        last_rpc_id_(last_rpc_id),
        last_xid_(last_xid) {}

  int __reg_to__(Server& svr, size_t svc_index) {
    if (reg_calls_ != nullptr) {
      reg_calls_->fetch_add(1, std::memory_order_relaxed);
    }
    if (last_svc_index_ != nullptr) {
      *last_svc_index_ = svc_index;
    }
    return svr.reg_rpc(RPC_ID, svc_index);
  }

  void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection) {
    if (dispatch_calls_ != nullptr) {
      dispatch_calls_->fetch_add(1, std::memory_order_relaxed);
    }
    if (last_rpc_id_ != nullptr) {
      *last_rpc_id_ = rpc_id;
    }
    if (last_xid_ != nullptr && req) {
      *last_xid_ = req->xid;
    }
  }

 private:
  std::atomic<int>* reg_calls_;
  std::atomic<int>* dispatch_calls_;
  size_t* last_svc_index_;
  i32* last_rpc_id_;
  i64* last_xid_;
};

TEST(RpcServiceProxyFacadeTest, AdapterForwardsRegistrationAndDispatch) {
  std::atomic<int> reg_calls{0};
  std::atomic<int> dispatch_calls{0};
  size_t last_svc_index = static_cast<size_t>(-1);
  i32 last_rpc_id = -1;
  i64 last_xid = -1;

  auto proxy = make_service_proxy_from_box(rusty::make_box<CountingService>(
      &reg_calls, &dispatch_calls, &last_svc_index, &last_rpc_id, &last_xid));
  Server server(rusty::None);

  EXPECT_EQ(proxy->__reg_to__(server, 3), 0);
  EXPECT_EQ(reg_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_svc_index, 3u);

  auto req = rusty::make_box<Request>();
  req->xid = 42;
  proxy->__dispatch__(CountingService::RPC_ID, std::move(req), WeakServerConnection{});

  EXPECT_EQ(dispatch_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_rpc_id, CountingService::RPC_ID);
  EXPECT_EQ(last_xid, 42);
}

TEST(RpcServiceProxyFacadeTest, TypedBoxAdapterForwardsRegistrationAndDispatch) {
  std::atomic<int> reg_calls{0};
  std::atomic<int> dispatch_calls{0};
  size_t last_svc_index = static_cast<size_t>(-1);
  i32 last_rpc_id = -1;
  i64 last_xid = -1;

  auto proxy = make_service_proxy_from_typed_box(rusty::make_box<TypedCountingService>(
      &reg_calls, &dispatch_calls, &last_svc_index, &last_rpc_id, &last_xid));
  Server server(rusty::None);

  EXPECT_EQ(proxy->__reg_to__(server, 7), 0);
  EXPECT_EQ(reg_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_svc_index, 7u);

  auto req = rusty::make_box<Request>();
  req->xid = 66;
  proxy->__dispatch__(TypedCountingService::RPC_ID, std::move(req), WeakServerConnection{});

  EXPECT_EQ(dispatch_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_rpc_id, TypedCountingService::RPC_ID);
  EXPECT_EQ(last_xid, 66);
}

TEST(RpcServiceProxyFacadeTest, ServerRegistrationUsesProxyBackedPendingStorage) {
  std::atomic<int> reg_calls{0};
  std::atomic<int> dispatch_calls{0};
  size_t last_svc_index = static_cast<size_t>(-1);
  i32 last_rpc_id = -1;
  i64 last_xid = -1;

  Server server(rusty::None);
  server.reg_service(rusty::make_box<CountingService>(
      &reg_calls, &dispatch_calls, &last_svc_index, &last_rpc_id, &last_xid));

  EXPECT_EQ(server.pending_services_field.size(), 1u);
  EXPECT_EQ(reg_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_svc_index, 0u);

  auto rpc_it = server.pending_rpc_to_service_field.get(CountingService::RPC_ID);
  ASSERT_TRUE(rpc_it.is_some());
  EXPECT_EQ(rpc_it.unwrap(), 0u);

  auto req = rusty::make_box<Request>();
  req->xid = 88;
  server.pending_services_field[0]->__dispatch__(
      CountingService::RPC_ID, std::move(req), WeakServerConnection{});

  EXPECT_EQ(dispatch_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_rpc_id, CountingService::RPC_ID);
  EXPECT_EQ(last_xid, 88);
}

TEST(RpcServiceProxyFacadeTest, ServerRegistrationAcceptsTypedServiceWithoutInheritance) {
  std::atomic<int> reg_calls{0};
  std::atomic<int> dispatch_calls{0};
  size_t last_svc_index = static_cast<size_t>(-1);
  i32 last_rpc_id = -1;
  i64 last_xid = -1;

  Server server(rusty::None);
  server.reg_service_typed(rusty::make_box<TypedCountingService>(
      &reg_calls, &dispatch_calls, &last_svc_index, &last_rpc_id, &last_xid));

  EXPECT_EQ(server.pending_services_field.size(), 1u);
  EXPECT_EQ(reg_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_svc_index, 0u);

  auto rpc_it = server.pending_rpc_to_service_field.get(TypedCountingService::RPC_ID);
  ASSERT_TRUE(rpc_it.is_some());
  EXPECT_EQ(rpc_it.unwrap(), 0u);

  auto req = rusty::make_box<Request>();
  req->xid = 99;
  server.pending_services_field[0]->__dispatch__(
      TypedCountingService::RPC_ID, std::move(req), WeakServerConnection{});

  EXPECT_EQ(dispatch_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(last_rpc_id, TypedCountingService::RPC_ID);
  EXPECT_EQ(last_xid, 99);
}

}  // namespace
}  // namespace rrr
