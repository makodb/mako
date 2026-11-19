// @unsafe - Test file exercising the RPC round trip over multiple transports.
// @unsafe {
//
// Tier 2.3 of docs/testing-plan.md: one request/reply body run across more
// than one transport (gRPC fixture-matrix style), so a divergence in the
// shared server-dispatch / client-demux path shows up regardless of which
// channel carried it. This is the C++ lane's home for the matrix because TCP
// and the Rust lane both link the same native C kernels. The Rust counterpart
// also checks cooperative dispatch in tests/rpc_runtime_rust.rs.
//
// The service echoes an i64 doubled, over a fast RPC (inline dispatch, no
// fiber -- keeps the test synchronous and deterministic). It is run over:
//   * the in-memory switchboard (set_channel_factory before start/connect);
//   * TCP loopback (auto-installed; bind to port 0, read the real port back).
//
// TCP also carries the shared-client test: the owning thread and the
// client's poll thread (reply callbacks) enter one rusty::Arc<Client> at
// once, as rpcbench does. Its Rust counterpart is
// tests/client_shared_handle_rust.rs.

#include <stddef.h>
#include <string.h>

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/function.hpp>
#include "../srpc.hpp"

// Serialization and the in-memory transport are trimmed from the umbrella.
import srpc.serializable;
import srpc.inmemory_channel;

import std;

using namespace srpc;

namespace {

constexpr int32_t ECHO_DOUBLE_RPC_ID = 0x00E0'1001;

// A raw Service (the shape a `raw` IDL method hands a handler, and the whole
// Rust service API): read an i64, reply with twice its value, on the fast
// path so dispatch is inline.
class EchoDoubleService : public Service {
 public:
    int32_t __reg_to__(Server& server, size_t svc_index) override {
        return server.reg_fast_rpc(ECHO_DOUBLE_RPC_ID, svc_index);
    }

    void __dispatch__(int32_t rpc_id, rusty::Box<Request> req,
                      WeakServerConnection weak_sconn) const override {
        if (rpc_id != ECHO_DOUBLE_RPC_ID) {
            return;
        }
        int64_t value = 0;
        {
            BinaryReadArchive ar{.source_ = make_source_proxy_buffer(&req->src)};
            Deserialize_::deserialize(value, ar);
        }
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const int64_t doubled = value * 2;
            const_cast<ServerConnection&>(*sconn).reply(
                *req, 0, ServerReplyFn{[doubled](BinaryWriteArchive& out) {
                    Serialize_::serialize(doubled, out);
                }});
        }
    }
};

// Issue one echo request and return the doubled reply, asserting success.
int64_t echo_once(const rusty::Arc<Client>& client, int64_t arg) {
    auto fu_result = client->request(
        ECHO_DOUBLE_RPC_ID, FutureAttr{},
        [arg](BinaryWriteArchive& m) { Serialize_::serialize(arg, m); });
    EXPECT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();
    EXPECT_EQ(fu->get_error_code(), 0);
    int64_t back = 0;
    deserialize_from(fu->get_reply(), back);
    return back;
}

TEST(RpcTransportMatrix, InMemoryRoundTrip) {
    auto switchboard = rusty::Arc<InMemorySwitchboard>::make(InMemorySwitchboard::new_());
    const char* addr = "inmemory://matrix";

    auto server = Server::new_(rusty::Some(PollThread::create()));
    server.set_channel_factory(make_inmemory_factory_proxy(
        rusty::Arc<InMemoryFactory>::make(InMemoryFactory::new_(switchboard.clone()))));
    server.reg_service_typed(rusty::make_box<EchoDoubleService>());
    ASSERT_EQ(server.start(reinterpret_cast<const int8_t*>(addr)), 0);

    auto client = Client::create(PollThread::create());
    client->set_channel_factory(make_inmemory_factory_proxy(
        rusty::Arc<InMemoryFactory>::make(InMemoryFactory::new_(switchboard.clone()))));
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(addr), true), 0);

    EXPECT_EQ(echo_once(client, 21), 42);
    EXPECT_EQ(echo_once(client, -5), -10);

    client->close();
}

TEST(RpcTransportMatrix, TcpLoopbackRoundTrip) {
    auto poll = PollThread::create();
    auto server = Server::new_(rusty::Some(poll.clone()));
    server.reg_service_typed(rusty::make_box<EchoDoubleService>());
    // Bind to port 0; TCP factory is auto-installed.
    ASSERT_EQ(server.start(reinterpret_cast<const int8_t*>("127.0.0.1:0")), 0);
    const int32_t port = server.get_bound_port();
    ASSERT_GT(port, 0);

    std::string addr = "127.0.0.1:" + std::to_string(port);
    auto client = Client::create(poll.clone());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(addr.c_str()), true), 0);

    EXPECT_EQ(echo_once(client, 21), 42);
    EXPECT_EQ(echo_once(client, 1000), 2000);

    client->close();
}

// Both methods share one service object. A suspended stackful handler must
// leave the service available for another request on the same poll thread.
class SuspendingService : public Service {
 public:
    static constexpr int32_t SLOW_RPC_ID = 0x00E0'1002;
    static constexpr int32_t FAST_RPC_ID = 0x00E0'1003;
    mutable std::atomic<int> stage{0};

    int32_t __reg_to__(Server& server, size_t index) override {
        const auto error = server.reg_rpc(SLOW_RPC_ID, index);
        return error == 0 ? server.reg_fast_rpc(FAST_RPC_ID, index) : error;
    }

    void __dispatch__(int32_t rpc_id, rusty::Box<Request> req,
                      WeakServerConnection weak_sconn) const override {
        if (rpc_id == SLOW_RPC_ID) {
            stage.store(1);
            this_fiber::sleep_ms(75);
            EXPECT_EQ(stage.load(), 2);
            stage.store(3);
        } else if (rpc_id == FAST_RPC_ID) {
            EXPECT_EQ(stage.exchange(2), 1);
        } else {
            return;
        }
        auto connection = weak_sconn.upgrade();
        if (connection.is_some()) {
            auto sconn = connection.unwrap();
            const_cast<ServerConnection&>(*sconn).reply(
                *req, 0, ServerReplyFn{[](BinaryWriteArchive& out) {
                    Serialize_::serialize(int64_t{41}, out);
                }});
        }
    }
};

TEST(RpcTransportMatrix, StackfulHandlerAllowsSameServiceDispatch) {
    auto poll = PollThread::create();
    auto server = Server::new_(rusty::Some(poll.clone()));
    auto service = rusty::make_box<SuspendingService>();
    const auto* observer = service.get();
    server.reg_service_typed(std::move(service));
    ASSERT_EQ(server.start(reinterpret_cast<const int8_t*>("127.0.0.1:0")), 0);
    const auto addr = "127.0.0.1:" + std::to_string(server.get_bound_port());
    auto client = Client::create(poll.clone());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(addr.c_str()), true), 0);

    auto slow_result = client->request(SuspendingService::SLOW_RPC_ID, FutureAttr{},
                                     [](BinaryWriteArchive&) {});
    ASSERT_TRUE(slow_result.is_ok());
    auto slow = slow_result.unwrap();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (observer->stage.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(observer->stage.load(), 1);
    auto fast_result = client->request(SuspendingService::FAST_RPC_ID, FutureAttr{},
                                     [](BinaryWriteArchive&) {});
    ASSERT_TRUE(fast_result.is_ok());
    auto fast = fast_result.unwrap();
    ASSERT_TRUE(fast->wait_with_options());
    ASSERT_TRUE(slow->wait_with_options());
    EXPECT_EQ(fast->get_error_code(), 0);
    EXPECT_EQ(slow->get_error_code(), 0);
    EXPECT_EQ(observer->stage.load(), 3);
    int64_t value = 0;
    deserialize_from(slow->get_reply(), value);
    EXPECT_EQ(value, 41);
    client->close();
}


// One client handle entered from two threads at once: rpcbench's pipeline.
// The owning thread keeps issuing request_async while each reply callback,
// which runs on the client's own poll thread, issues the next request
// through a captured copy of the same rusty::Arc<Client>. Before the client
// state was synchronized, Client::connection() read a RefCell whose borrow
// counter is a plain int; concurrent borrows lost updates, and a later
// borrow panicked "already mutably borrowed". C++ callable erasure lets this
// capture compile regardless, so the contract has to hold at runtime.
struct SharedClientCounters {
    std::atomic<int64_t> chain_hops{0};
    std::atomic<int64_t> chains_finished{0};
    std::atomic<int64_t> caller_completed{0};
    std::atomic<int64_t> errors{0};
};

constexpr int64_t kSharedChains = 32;
constexpr int64_t kSharedHopsPerChain = 1500;
constexpr int64_t kSharedCallerRequests = 20000;
// Bound the owner's in-flight requests far below the 16,384 async slots.
constexpr int64_t kSharedCallerWindow = 256;

void chain_shared_client(rusty::Arc<Client> client,
                         std::shared_ptr<SharedClientCounters> counters,
                         int64_t hops_left) {
    AsyncReplyCallback on_reply{
        [client, counters, hops_left](int32_t error, const uint8_t*, size_t) {
            if (error != 0) {
                counters->errors.fetch_add(1);
                counters->chains_finished.fetch_add(1);
                return;
            }
            counters->chain_hops.fetch_add(1);
            if (hops_left > 1) {
                chain_shared_client(client, counters, hops_left - 1);
            } else {
                counters->chains_finished.fetch_add(1);
            }
        }};
    auto sent = client->request_async(
        ECHO_DOUBLE_RPC_ID,
        [](BinaryWriteArchive& m) { Serialize_::serialize(int64_t{1}, m); },
        std::move(on_reply));
    if (sent.is_err()) {
        counters->errors.fetch_add(1);
        counters->chains_finished.fetch_add(1);
    }
}

TEST(RpcTransportMatrix, TcpReplyCallbacksAndOwnerShareOneClient) {
    auto server_poll = PollThread::create();
    auto client_poll = PollThread::create();
    auto server = Server::new_(rusty::Some(server_poll.clone()));
    server.reg_service_typed(rusty::make_box<EchoDoubleService>());
    ASSERT_EQ(server.start(reinterpret_cast<const int8_t*>("127.0.0.1:0")), 0);
    const auto addr = "127.0.0.1:" + std::to_string(server.get_bound_port());
    auto client = Client::create(client_poll.clone());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(addr.c_str()), true), 0);

    auto counters = std::make_shared<SharedClientCounters>();
    for (int64_t i = 0; i < kSharedChains; ++i) {
        chain_shared_client(client, counters, kSharedHopsPerChain);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int64_t issued = 0;
    while (issued < kSharedCallerRequests) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "caller stalled at " << issued;
        if (issued - counters->caller_completed.load() >= kSharedCallerWindow) {
            std::this_thread::yield();
            continue;
        }
        AsyncReplyCallback on_reply{[counters](int32_t error, const uint8_t*, size_t) {
            if (error != 0) {
                counters->errors.fetch_add(1);
            }
            counters->caller_completed.fetch_add(1);
        }};
        auto sent = client->request_async(
            ECHO_DOUBLE_RPC_ID,
            [](BinaryWriteArchive& m) { Serialize_::serialize(int64_t{1}, m); },
            std::move(on_reply));
        ASSERT_TRUE(sent.is_ok()) << "request " << issued;
        ++issued;
    }
    while ((counters->chains_finished.load() < kSharedChains ||
            counters->caller_completed.load() < kSharedCallerRequests) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(counters->errors.load(), 0);
    EXPECT_EQ(counters->chains_finished.load(), kSharedChains);
    EXPECT_EQ(counters->chain_hops.load(), kSharedChains * kSharedHopsPerChain);
    EXPECT_EQ(counters->caller_completed.load(), kSharedCallerRequests);
    EXPECT_EQ(client->metrics().in_flight_requests(), 0u);

    client->close();
    client_poll->shutdown();
    server_poll->shutdown();
}

}  // namespace
// @unsafe }
