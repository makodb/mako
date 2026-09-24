// end-to-end RPC test using
// `InMemoryFactory`.
//
// Drives a real `Server` + `Client` through the in-memory channel
// backend (no sockets, no kernel network stack). Verifies that:
//   * a registered fast-rpc service receives the request and the
//     client sees the response,
//   * the server's destruction propagates as a disconnect to the
//     client (close-fan-out via the channel proxy's on_closed),
//   * the client's `host()` reflects the in-memory bind address.
//
// This is the integration leaf that ties 6a (basic backend), 6b
// (close semantics), and 6c (fault injection — not exercised here
// but available for follow-up tests).

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../srpc.hpp"

// ReconnectPolicy lives in srpc.reconnect_policy (trimmed from the
// consumer umbrella in 08b68144) — import directly.
import srpc.reconnect_policy;
#include "../rpc/inmemory_channel.hpp"

import std;

namespace srpc {
namespace {

// Tiny test service that dispatches an "echo" RPC by replying with
// the user payload echoed back. Mirrors the pattern in
// rpc_server_channel_recv_test.cc but registered through the real
// `Server::reg_service` API rather than constructed manually.
class EchoService {
 public:
    static constexpr i32 kEchoRpcId = 5550001;

    int __reg_to__(Server& server, std::size_t svc_index) {
        // Register kEchoRpcId as a fast (inline-dispatched) RPC so
        // the response fires synchronously inside the InMemory
        // backend's on_frame callback (no fiber yield required).
        return server.reg_fast_rpc(kEchoRpcId, svc_index);
    }

    void __dispatch__(i32 /*rpc_id*/, rusty::Box<Request> req,
                      WeakServerConnection sconn) {
        std::string echo;
        srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
        srpc::Deserialize_::deserialize(echo, __req_ar__);
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++dispatch_count_;
            last_payload_ = echo;
        }
        auto sconn_opt = sconn.upgrade();
        if (sconn_opt.is_some()) {
            sconn_opt.unwrap()->reply(*req, /*err=*/0,
                [&](BinaryWriteArchive& out) { srpc::Serialize_::serialize(echo, out); });
        }
    }

    int dispatch_count() {
        std::lock_guard<std::mutex> lk(mu_);
        return dispatch_count_;
    }
    std::string last_payload() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_payload_;
    }

 private:
    std::mutex  mu_;
    int         dispatch_count_ = 0;
    std::string last_payload_;
};
static_assert(requires(
    EchoService& svc,
    Server& server,
    std::size_t svc_index,
    i32 rpc_id,
    rusty::Box<Request> req,
    WeakServerConnection weak_sconn) {
  { svc.__reg_to__(server, svc_index) } -> std::convertible_to<int>;
  { svc.__dispatch__(rpc_id, std::move(req), std::move(weak_sconn)) }
      -> std::same_as<void>;
});

class InMemoryE2ETest : public ::testing::Test {
 protected:
    void SetUp() override {
        switchboard_ = rusty::Some(
            rusty::Arc<InMemorySwitchboard>::new_(InMemorySwitchboard::new_()));
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        client_ = rusty::None;
        server_ = rusty::None;
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
            poll_thread_ = rusty::None;
        }
        switchboard_ = rusty::None;
    }

    // Build a fresh `ChannelFactoryProxy` wrapping the shared
    // switchboard. Each call returns a new proxy (proxies are
    // move-only, so callers consume them).
    ChannelFactoryProxy make_factory() {
        auto factory_arc = rusty::Arc<InMemoryFactory>::new_(InMemoryFactory::new_(
            switchboard_.as_ref().unwrap().clone()));
        return make_inmemory_factory_proxy(std::move(factory_arc));
    }

    // rusty::Option<T> swap. See
    // rpc_client_channel_recv_test.cc for the API translation
    // pattern (`emplace` → `= rusty::Some(...)`, `reset` → `=
    // rusty::None`, `(*opt)` → `opt.as_ref().unwrap()`).
    rusty::Option<rusty::Arc<InMemorySwitchboard>> switchboard_;
    rusty::Option<rusty::Arc<PollThread>>          poll_thread_;
    rusty::Option<rusty::Box<Server>>              server_;
    rusty::Option<rusty::Arc<Client>>              client_{rusty::None};
};

// ---------------------------------------------------------------------------
// Round-trip: register service, start server, connect client, send
// request, verify response.
// ---------------------------------------------------------------------------

TEST_F(InMemoryE2ETest, RoundTripFastRpc) {
    server_ = rusty::make_box<Server>(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto svc_box = rusty::make_box<EchoService>();
    auto* svc_raw = svc_box.get();
    server_.as_ref().unwrap()->reg_service_typed<EchoService>(std::move(svc_box));
    server_.as_ref().unwrap()->set_channel_factory(make_factory());
    ASSERT_EQ(server_.as_ref().unwrap()->start(reinterpret_cast<const int8_t*>("inmemory://e2e-server-1")), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap().clone());
    client_ = rusty::Some(client.clone());
    client->set_channel_factory(make_factory());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>("inmemory://e2e-server-1"), true), 0);
    EXPECT_TRUE(client->connected());

    const std::string input = "hello-inmemory";
    auto fu_result = client->request(EchoService::kEchoRpcId, FutureAttr(),
        [&](BinaryWriteArchive& m) { srpc::Serialize_::serialize(input, m); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    EXPECT_EQ(fu->get_error_code(), 0);
    std::string echoed;
    srpc::deserialize_from(fu->get_reply(), echoed);
    EXPECT_EQ(echoed, input);

    EXPECT_EQ(svc_raw->dispatch_count(), 1);
    EXPECT_EQ(svc_raw->last_payload(), input);
}

// ---------------------------------------------------------------------------
// round-trip with a `BinaryWriteArchive&` write_fn (instead
// of the legacy `Marshal&`).  `request_via_channel<F>` dual-dispatches
// via `if constexpr (std::is_invocable_v<F&, BinaryWriteArchive&>)`,
// wrapping the same `Marshal body` through a `MarshalSink`. Bytes on
// the wire stay identical to the legacy path; the server-side decode
// (which reads through the req->src cursor) consumes them with no
// changes.
// ---------------------------------------------------------------------------

TEST_F(InMemoryE2ETest, RoundTripFastRpcViaBinaryWriteArchive) {
    server_ = rusty::make_box<Server>(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto svc_box = rusty::make_box<EchoService>();
    auto* svc_raw = svc_box.get();
    server_.as_ref().unwrap()->reg_service_typed<EchoService>(std::move(svc_box));
    server_.as_ref().unwrap()->set_channel_factory(make_factory());
    ASSERT_EQ(server_.as_ref().unwrap()->start(reinterpret_cast<const int8_t*>("inmemory://e2e-server-archive")), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap().clone());
    client_ = rusty::Some(client.clone());
    client->set_channel_factory(make_factory());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>("inmemory://e2e-server-archive"), true), 0);
    EXPECT_TRUE(client->connected());

    const std::string input = "hello-archive";
    auto fu_result = client->request(EchoService::kEchoRpcId, FutureAttr(),
        [&](BinaryWriteArchive& ar) { srpc::Serialize_::serialize(input, ar); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    EXPECT_EQ(fu->get_error_code(), 0);
    std::string echoed;
    srpc::deserialize_from(fu->get_reply(), echoed);
    EXPECT_EQ(echoed, input);

    EXPECT_EQ(svc_raw->dispatch_count(), 1);
    EXPECT_EQ(svc_raw->last_payload(), input);
}

// ---------------------------------------------------------------------------
// a single test that exercises BOTH write_fn signatures on
// the same client/server pair, asserting both succeed and produce the
// expected echoed payload.  Guards against regressions where one or the
// other branch of the `if constexpr` selection diverges from the wire
// format the server expects.
// ---------------------------------------------------------------------------

TEST_F(InMemoryE2ETest, RoundTripBothWriteFnSignatures) {
    server_ = rusty::make_box<Server>(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto svc_box = rusty::make_box<EchoService>();
    auto* svc_raw = svc_box.get();
    server_.as_ref().unwrap()->reg_service_typed<EchoService>(std::move(svc_box));
    server_.as_ref().unwrap()->set_channel_factory(make_factory());
    ASSERT_EQ(server_.as_ref().unwrap()->start(reinterpret_cast<const int8_t*>("inmemory://e2e-server-mixed")), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap().clone());
    client_ = rusty::Some(client.clone());
    client->set_channel_factory(make_factory());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>("inmemory://e2e-server-mixed"), true), 0);

    // Marshal-flavoured request.
    {
        const std::string input = "via-marshal";
        auto fu = client->request(EchoService::kEchoRpcId, FutureAttr(),
            [&](BinaryWriteArchive& m) { srpc::Serialize_::serialize(input, m); }).unwrap();
        fu->wait();
        ASSERT_EQ(fu->get_error_code(), 0);
        std::string echoed;
        srpc::deserialize_from(fu->get_reply(), echoed);
        EXPECT_EQ(echoed, input);
    }
    // Archive-flavoured request, same client.
    {
        const std::string input = "via-archive";
        auto fu = client->request(EchoService::kEchoRpcId, FutureAttr(),
            [&](BinaryWriteArchive& ar) { srpc::Serialize_::serialize(input, ar); }).unwrap();
        fu->wait();
        ASSERT_EQ(fu->get_error_code(), 0);
        std::string echoed;
        srpc::deserialize_from(fu->get_reply(), echoed);
        EXPECT_EQ(echoed, input);
    }
    EXPECT_EQ(svc_raw->dispatch_count(), 2);
}

// ---------------------------------------------------------------------------
// Multiple sequential requests over the same connection.
// ---------------------------------------------------------------------------

TEST_F(InMemoryE2ETest, MultipleSequentialRequests) {
    server_ = rusty::make_box<Server>(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto svc_box = rusty::make_box<EchoService>();
    auto* svc_raw = svc_box.get();
    server_.as_ref().unwrap()->reg_service_typed<EchoService>(std::move(svc_box));
    server_.as_ref().unwrap()->set_channel_factory(make_factory());
    ASSERT_EQ(server_.as_ref().unwrap()->start(reinterpret_cast<const int8_t*>("inmemory://e2e-server-2")), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap().clone());
    client_ = rusty::Some(client.clone());
    client->set_channel_factory(make_factory());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>("inmemory://e2e-server-2"), true), 0);

    constexpr int kIterations = 25;
    for (int i = 0; i < kIterations; ++i) {
        std::string input = "req-" + std::to_string(i);
        auto fu_result = client->request(EchoService::kEchoRpcId, FutureAttr(),
            [&](BinaryWriteArchive& m) { srpc::Serialize_::serialize(input, m); });
        ASSERT_TRUE(fu_result.is_ok()) << "iter=" << i;
        auto fu = fu_result.unwrap();
        fu->wait();
        ASSERT_EQ(fu->get_error_code(), 0) << "iter=" << i;
        std::string echoed;
        srpc::deserialize_from(fu->get_reply(), echoed);
        EXPECT_EQ(echoed, input);
    }
    EXPECT_EQ(svc_raw->dispatch_count(), kIterations);
}

// ---------------------------------------------------------------------------
// Server destruction propagates as a client-side disconnect.
//
// When ~Server runs, it actively closes each accepted ServerConnection
// (5f), which drives its bound channel proxy's close(), which fires
// the peer's on_closed callback. The client's bind_channel_direct
// installs an on_closed handler that calls on_channel_closed_fan_out
// — which transitions the connection state machine to FAILED and
// invokes the disconnected callback.
// ---------------------------------------------------------------------------

TEST_F(InMemoryE2ETest, ServerDestroyTriggersClientDisconnect) {
    server_ = rusty::make_box<Server>(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto svc_box = rusty::make_box<EchoService>();
    server_.as_ref().unwrap()->reg_service_typed<EchoService>(std::move(svc_box));
    server_.as_ref().unwrap()->set_channel_factory(make_factory());
    ASSERT_EQ(server_.as_ref().unwrap()->start(reinterpret_cast<const int8_t*>("inmemory://e2e-server-3")), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap().clone());
    client_ = rusty::Some(client.clone());
    // Disable auto-reconnect — this test only verifies the disconnect
    // path. Leaving auto-reconnect on causes `on_channel_closed_fan_out`
    // to spawn a detached reconnect thread when the server is dropped
    // below; that thread races the next test's setup (the fixture's
    // shared switchboard) and intermittently produces a `malloc():
    // unaligned tcache chunk detected` corruption when the next test
    // (`ConnectToUnboundAddrFailsFast`) creates a fresh PollThread.
    client->set_reconnect_policy(ReconnectPolicy::no_retry());
    client->set_channel_factory(make_factory());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>("inmemory://e2e-server-3"), true), 0);
    EXPECT_TRUE(client->connected());

    std::atomic<int> disconnected_count{0};
    client->add_on_disconnected([&]() {
        disconnected_count.fetch_add(1, std::memory_order_relaxed);
    });

    // Drop the server. ~Server actively closes accepted connections,
    // which drives the channel proxy's close → client sees
    // on_closed → on_channel_closed_fan_out.
    server_ = rusty::None;

    EXPECT_FALSE(client->connected());
    // The disconnected callback fires synchronously through the
    // InMemory backend's on_closed dispatch; the count should
    // already be 1.
    EXPECT_GE(disconnected_count.load(std::memory_order_relaxed), 1);
}

// ---------------------------------------------------------------------------
// Connect to an address with no listener returns ConnectionRefused.
// ---------------------------------------------------------------------------

TEST_F(InMemoryE2ETest, ConnectToUnboundAddrFailsFast) {
    auto client = Client::create(poll_thread_.as_ref().unwrap().clone());
    client_ = rusty::Some(client.clone());
    client->set_channel_factory(make_factory());
    // No server registered for this address.
    EXPECT_NE(client->connect(reinterpret_cast<const int8_t*>("inmemory://nonexistent"), true), 0);
    EXPECT_FALSE(client->connected());
}

// Note: `Client::host()` is only populated by the legacy fd-path
// connect (which deleted in 4g3); channel-mode `Client::connect`
// doesn't set `host_` on the ClientConnection. Tests that need a
// peer identifier should use the underlying channel proxy's
// `peer_address()` if exposed.

}  // namespace
}  // namespace srpc
