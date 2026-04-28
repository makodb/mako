// Workstream K, leaf 6a — basic tests for `InMemoryFactory` /
// `InMemoryListener` / `InMemoryChannel` (the in-memory channel
// backend).
//
// This suite exercises the connect/listen/send flow without any
// real sockets:
//   * factory.make_listener()  → produces a listener proxy
//   * listener.listen(addr)    → registers in the switchboard
//   * factory.connect(addr)    → finds the listener, fires
//                                on_accept, returns the client side
//   * send_frame on either side → fires on_frame on the peer
//
// Future leaves (6b/6c/6d) add close semantics, fault injection,
// and an end-to-end RPC test.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>

#include "../rrr.hpp"
#include "../rpc/inmemory_channel.hpp"

namespace rrr {
namespace {

class InMemoryChannelTest : public ::testing::Test {
 protected:
    void SetUp() override {
        switchboard_.emplace(rusty::Arc<InMemorySwitchboard>::make());
        factory_arc_.emplace(
            rusty::Arc<InMemoryFactory>::make((*switchboard_).clone()));
        factory_ = make_inmemory_factory_proxy((*factory_arc_).clone());
    }

    void TearDown() override {
        factory_     = ChannelFactoryProxy{};
        factory_arc_.reset();
        switchboard_.reset();
    }

    std::optional<rusty::Arc<InMemorySwitchboard>>  switchboard_;
    std::optional<rusty::Arc<InMemoryFactory>>      factory_arc_;
    ChannelFactoryProxy                             factory_;
};

// ---------------------------------------------------------------------------
// Factory: backend_name + connect-to-unbound-addr.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, BackendName) {
    EXPECT_STREQ(factory_->backend_name(), "inmemory");
}

TEST_F(InMemoryChannelTest, ConnectToUnboundAddrReturnsRefused) {
    auto result = factory_->connect("inmemory://nobody-listening");
    EXPECT_EQ(result.error, ChannelError::ConnectionRefused);
    EXPECT_FALSE(result.connection.has_value());
}

// ---------------------------------------------------------------------------
// Listener: listen + close lifecycle.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, ListenerLifecycle) {
    auto listener = factory_->make_listener();
    EXPECT_FALSE(listener->is_closed());
    EXPECT_TRUE(listener->local_address().empty());

    EXPECT_EQ(listener->listen("inmemory://service-A"), ChannelError::None);
    EXPECT_EQ(listener->local_address(), "inmemory://service-A");
    EXPECT_FALSE(listener->is_closed());

    listener->close();
    EXPECT_TRUE(listener->is_closed());
    // Idempotent.
    listener->close();
    EXPECT_TRUE(listener->is_closed());
}

TEST_F(InMemoryChannelTest, ListenerAddressInUse) {
    auto a = factory_->make_listener();
    auto b = factory_->make_listener();
    ASSERT_EQ(a->listen("inmemory://service-X"), ChannelError::None);
    EXPECT_EQ(b->listen("inmemory://service-X"), ChannelError::AddressInUse);
}

// ---------------------------------------------------------------------------
// Connect-then-frame: client → server.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, ConnectAndSendFrameClientToServer) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://service-1"), ChannelError::None);

    // Server-side state captured by the on_accept callback.
    std::vector<std::vector<std::uint8_t>> server_received;
    ChannelConnectionProxy server_side_proxy;
    listener->set_on_accept([&](ChannelConnectionProxy peer) {
        server_side_proxy = std::move(peer);
        server_side_proxy->set_on_frame([&](const ChannelFrame& f) {
            server_received.emplace_back(f.payload, f.payload + f.size);
        });
    });

    auto result = factory_->connect("inmemory://service-1");
    ASSERT_EQ(result.error, ChannelError::None);
    ASSERT_TRUE(result.connection.has_value());
    ASSERT_TRUE(server_side_proxy.has_value());

    // Send a frame from client → server.
    std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    ChannelFrame f{payload.data(), payload.size()};
    EXPECT_EQ(result.connection->send_frame(f), ChannelError::None);

    ASSERT_EQ(server_received.size(), 1u);
    EXPECT_EQ(server_received.front(), payload);
}

// ---------------------------------------------------------------------------
// Bidirectional frame exchange.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, BidirectionalSendFrame) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://bidir"), ChannelError::None);

    std::vector<std::vector<std::uint8_t>> server_received;
    std::vector<std::vector<std::uint8_t>> client_received;
    ChannelConnectionProxy server_side_proxy;

    listener->set_on_accept([&](ChannelConnectionProxy peer) {
        server_side_proxy = std::move(peer);
        server_side_proxy->set_on_frame([&](const ChannelFrame& f) {
            server_received.emplace_back(f.payload, f.payload + f.size);
        });
    });

    auto result = factory_->connect("inmemory://bidir");
    ASSERT_EQ(result.error, ChannelError::None);
    auto& client_proxy = result.connection;
    client_proxy->set_on_frame([&](const ChannelFrame& f) {
        client_received.emplace_back(f.payload, f.payload + f.size);
    });

    // Client → server.
    std::vector<std::uint8_t> req = {0xA, 0xB, 0xC};
    ChannelFrame fr{req.data(), req.size()};
    EXPECT_EQ(client_proxy->send_frame(fr), ChannelError::None);
    ASSERT_EQ(server_received.size(), 1u);
    EXPECT_EQ(server_received.front(), req);

    // Server → client.
    std::vector<std::uint8_t> resp = {0x1, 0x2, 0x3, 0x4};
    ChannelFrame fr2{resp.data(), resp.size()};
    EXPECT_EQ(server_side_proxy->send_frame(fr2), ChannelError::None);
    ASSERT_EQ(client_received.size(), 1u);
    EXPECT_EQ(client_received.front(), resp);

    // Multiple frames in order.
    for (int i = 0; i < 10; ++i) {
        std::vector<std::uint8_t> p = {static_cast<std::uint8_t>(i)};
        ChannelFrame f3{p.data(), p.size()};
        EXPECT_EQ(client_proxy->send_frame(f3), ChannelError::None);
    }
    ASSERT_EQ(server_received.size(), 11u);  // 1 prior + 10 new
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(server_received[i + 1].front(), static_cast<std::uint8_t>(i));
    }
}

// ---------------------------------------------------------------------------
// Multiple connections to the same listener.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, MultipleConnections) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://multi"), ChannelError::None);

    int accept_count = 0;
    std::vector<ChannelConnectionProxy> server_proxies;
    listener->set_on_accept([&](ChannelConnectionProxy peer) {
        ++accept_count;
        server_proxies.push_back(std::move(peer));
    });

    auto c1 = factory_->connect("inmemory://multi");
    auto c2 = factory_->connect("inmemory://multi");
    auto c3 = factory_->connect("inmemory://multi");
    EXPECT_EQ(c1.error, ChannelError::None);
    EXPECT_EQ(c2.error, ChannelError::None);
    EXPECT_EQ(c3.error, ChannelError::None);

    EXPECT_EQ(accept_count, 3);
    EXPECT_EQ(server_proxies.size(), 3u);
}

// ---------------------------------------------------------------------------
// Connect after listener close → ConnectionRefused.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, ConnectAfterListenerCloseRefused) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://going-away"), ChannelError::None);
    listener->close();

    auto result = factory_->connect("inmemory://going-away");
    EXPECT_EQ(result.error, ChannelError::ConnectionRefused);
}

// ---------------------------------------------------------------------------
// peer_address() reflects the addresses set up at accept time.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, PeerAddress) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://peer-addr-test"), ChannelError::None);

    ChannelConnectionProxy server_side_proxy;
    listener->set_on_accept([&](ChannelConnectionProxy peer) {
        server_side_proxy = std::move(peer);
    });

    auto result = factory_->connect("inmemory://peer-addr-test");
    ASSERT_EQ(result.error, ChannelError::None);

    // From the client's perspective, the peer (server) is at the
    // listener's address.
    EXPECT_EQ(result.connection->peer_address(), "inmemory://peer-addr-test");
    // From the server's perspective, the peer is the synthesized
    // client address (factory-generated, starts with "inmemory://client-").
    EXPECT_NE(server_side_proxy->peer_address().find("inmemory://client-"),
              std::string::npos);
}

}  // namespace
}  // namespace rrr
