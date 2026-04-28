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

// ---------------------------------------------------------------------------
// 6b — Close semantics
// ---------------------------------------------------------------------------

namespace close_test_helpers {
// Convenience: build a connected pair of channel proxies. The
// caller can then drive `send_frame` and `close()` directly. The
// fixture re-uses the same listener & address each call.
struct ConnectedPair {
    ChannelConnectionProxy client;
    ChannelConnectionProxy server;
};

inline ConnectedPair make_connected_pair(
        ChannelFactoryProxy& factory,
        ChannelListenerProxy& listener,
        std::string_view addr) {
    ConnectedPair pair;
    listener->set_on_accept([&pair](ChannelConnectionProxy peer) {
        pair.server = std::move(peer);
    });
    auto result = factory->connect(addr);
    if (result.error == ChannelError::None) {
        pair.client = std::move(result.connection);
    }
    return pair;
}
}  // namespace close_test_helpers

// ---------------------------------------------------------------------------
// close() on one side delivers on_closed(None) to the peer.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, ClientCloseFiresServerOnClosed) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-1"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-1");
    ASSERT_TRUE(pair.client.has_value());
    ASSERT_TRUE(pair.server.has_value());

    int server_on_closed_calls = 0;
    ChannelError observed_reason = ChannelError::Internal;
    pair.server->set_on_closed([&](ChannelError r) {
        ++server_on_closed_calls;
        observed_reason = r;
    });
    EXPECT_EQ(server_on_closed_calls, 0);

    pair.client->close();

    EXPECT_EQ(server_on_closed_calls, 1);
    EXPECT_EQ(observed_reason, ChannelError::None);
}

TEST_F(InMemoryChannelTest, ServerCloseFiresClientOnClosed) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-2"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-2");

    int client_on_closed_calls = 0;
    pair.client->set_on_closed([&](ChannelError) {
        ++client_on_closed_calls;
    });

    pair.server->close();

    EXPECT_EQ(client_on_closed_calls, 1);
}

// ---------------------------------------------------------------------------
// close() is idempotent — multiple calls don't re-fire the peer's
// on_closed callback.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, CloseIsIdempotent) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-idem"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-idem");

    int server_on_closed_calls = 0;
    pair.server->set_on_closed([&](ChannelError) {
        ++server_on_closed_calls;
    });

    pair.client->close();
    pair.client->close();
    pair.client->close();

    EXPECT_EQ(server_on_closed_calls, 1);
}

// ---------------------------------------------------------------------------
// is_closed() reflects either-side-closed (both halves observe true
// once one side closes).
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, IsClosedReflectsEitherSide) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-isclosed"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-isclosed");

    EXPECT_FALSE(pair.client->is_closed());
    EXPECT_FALSE(pair.server->is_closed());

    pair.client->close();

    EXPECT_TRUE(pair.client->is_closed());
    EXPECT_TRUE(pair.server->is_closed());
}

// ---------------------------------------------------------------------------
// send_frame after self.close() returns ConnectionReset.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, SendFrameAfterSelfCloseReturnsReset) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-send-self"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-send-self");

    pair.client->close();

    std::vector<std::uint8_t> bytes = {1, 2, 3};
    ChannelFrame f{bytes.data(), bytes.size()};
    EXPECT_EQ(pair.client->send_frame(f), ChannelError::ConnectionReset);
}

// ---------------------------------------------------------------------------
// send_frame after peer.close() returns ConnectionReset.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, SendFrameAfterPeerCloseReturnsReset) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-send-peer"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-send-peer");

    pair.client->close();

    std::vector<std::uint8_t> bytes = {1, 2, 3};
    ChannelFrame f{bytes.data(), bytes.size()};
    // Server still has its own closed_ flag at false, but the peer
    // (client) is closed → send_frame surfaces ConnectionReset.
    EXPECT_EQ(pair.server->send_frame(f), ChannelError::ConnectionReset);
}

// ---------------------------------------------------------------------------
// close() with no on_closed callback installed on the peer is still
// safe: state still flips, no callback fires, send_frame still
// reports ConnectionReset.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, CloseWithoutPeerCallbackIsSafe) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-no-cb"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-no-cb");

    // Deliberately do NOT install on_closed on the server side.
    pair.client->close();

    EXPECT_TRUE(pair.client->is_closed());
    EXPECT_TRUE(pair.server->is_closed());

    std::vector<std::uint8_t> bytes = {0xff};
    ChannelFrame f{bytes.data(), bytes.size()};
    EXPECT_EQ(pair.server->send_frame(f), ChannelError::ConnectionReset);
}

// ---------------------------------------------------------------------------
// Both sides close: each side's on_closed fires at most once. The
// second-to-close side does NOT fire the first side's on_closed
// (since the first side already observed close locally).
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, BothSidesCloseFiresOnClosedOnce) {
    auto listener = factory_->make_listener();
    ASSERT_EQ(listener->listen("inmemory://close-both"), ChannelError::None);
    auto pair = close_test_helpers::make_connected_pair(
        factory_, listener, "inmemory://close-both");

    int client_on_closed_calls = 0;
    int server_on_closed_calls = 0;
    pair.client->set_on_closed([&](ChannelError) { ++client_on_closed_calls; });
    pair.server->set_on_closed([&](ChannelError) { ++server_on_closed_calls; });

    pair.client->close();  // fires server's on_closed
    EXPECT_EQ(client_on_closed_calls, 0);
    EXPECT_EQ(server_on_closed_calls, 1);

    pair.server->close();  // does NOT fire client's on_closed (peer
                           // already closed; server merely flips its
                           // own closed flag).
    EXPECT_EQ(client_on_closed_calls, 0);
    EXPECT_EQ(server_on_closed_calls, 1);
}

// ---------------------------------------------------------------------------
// 6c — Fault injection
// ---------------------------------------------------------------------------

namespace fault_test_helpers {
struct PairAndProxies {
    std::optional<rusty::Arc<InMemoryChannel>> a;
    std::optional<rusty::Arc<InMemoryChannel>> b;
    ChannelConnectionProxy a_proxy;
    ChannelConnectionProxy b_proxy;
    std::vector<std::vector<std::uint8_t>> a_received;
    std::vector<std::vector<std::uint8_t>> b_received;

    InMemoryChannel& mut_a() {
        return const_cast<InMemoryChannel&>(*(*a).get());
    }
    InMemoryChannel& mut_b() {
        return const_cast<InMemoryChannel&>(*(*b).get());
    }
};

inline std::unique_ptr<PairAndProxies> make_pair_with_capture(
        std::string a_addr, std::string b_addr) {
    auto out = std::make_unique<PairAndProxies>();
    auto pair = make_channel_pair_for_testing(std::move(a_addr),
                                              std::move(b_addr));
    out->a.emplace(std::move(pair.first));
    out->b.emplace(std::move(pair.second));
    out->a_proxy = make_inmemory_channel_proxy((*out->a).clone());
    out->b_proxy = make_inmemory_channel_proxy((*out->b).clone());

    auto* a_received_ptr = &out->a_received;
    auto* b_received_ptr = &out->b_received;
    out->a_proxy->set_on_frame([a_received_ptr](const ChannelFrame& f) {
        a_received_ptr->emplace_back(f.payload, f.payload + f.size);
    });
    out->b_proxy->set_on_frame([b_received_ptr](const ChannelFrame& f) {
        b_received_ptr->emplace_back(f.payload, f.payload + f.size);
    });
    return out;
}

inline void send_byte(ChannelConnectionProxy& proxy, std::uint8_t b) {
    ChannelFrame f{&b, 1};
    proxy->send_frame(f);
}
}  // namespace fault_test_helpers

// ---------------------------------------------------------------------------
// inject_drop_next_sends(N): N silent drops from this side, then
// normal delivery resumes.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, InjectDropNextSendsDropsThenResumes) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_drop_next_sends(3);

    // First 3 sends from A → silently dropped.
    fault_test_helpers::send_byte(p->a_proxy, 1);
    fault_test_helpers::send_byte(p->a_proxy, 2);
    fault_test_helpers::send_byte(p->a_proxy, 3);
    EXPECT_EQ(p->b_received.size(), 0u);

    // 4th send → delivered.
    fault_test_helpers::send_byte(p->a_proxy, 4);
    ASSERT_EQ(p->b_received.size(), 1u);
    EXPECT_EQ(p->b_received.front().front(), static_cast<std::uint8_t>(4));

    // 5th send → also delivered (counter is back at zero).
    fault_test_helpers::send_byte(p->a_proxy, 5);
    EXPECT_EQ(p->b_received.size(), 2u);
}

// ---------------------------------------------------------------------------
// inject_drop_next_sends affects only the side it was invoked on.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, InjectDropNextSendsIsPerSide) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_drop_next_sends(2);

    fault_test_helpers::send_byte(p->a_proxy, 1);  // dropped
    fault_test_helpers::send_byte(p->b_proxy, 2);  // delivered (B-side has no drop)
    fault_test_helpers::send_byte(p->a_proxy, 3);  // dropped
    fault_test_helpers::send_byte(p->b_proxy, 4);  // delivered

    ASSERT_EQ(p->b_received.size(), 0u);
    ASSERT_EQ(p->a_received.size(), 2u);
    EXPECT_EQ(p->a_received[0].front(), static_cast<std::uint8_t>(2));
    EXPECT_EQ(p->a_received[1].front(), static_cast<std::uint8_t>(4));
}

// ---------------------------------------------------------------------------
// inject_send_error(err, N): N sends return the given error, then
// normal delivery resumes.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, InjectSendErrorReturnsErrThenResumes) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_send_error(ChannelError::WouldBlock, 2);

    std::uint8_t b = 0;
    ChannelFrame f{&b, 1};
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::WouldBlock);
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::WouldBlock);
    // 3rd send → success.
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::None);

    // First two were rejected (returned error, not delivered).
    // Only the third one reaches B.
    ASSERT_EQ(p->b_received.size(), 1u);
}

// ---------------------------------------------------------------------------
// Drop counter takes precedence over error counter.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, DropTakesPrecedenceOverError) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_drop_next_sends(2);
    p->mut_a().inject_send_error(ChannelError::ConnectionReset, 2);

    std::uint8_t b = 0;
    ChannelFrame f{&b, 1};
    // First two: drop (return None, no delivery).
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::None);
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::None);
    // Drop counter exhausted; next two pick up the error.
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::ConnectionReset);
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::ConnectionReset);
    // Both counters exhausted; next is normal.
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::None);

    // Only the last one reached B.
    ASSERT_EQ(p->b_received.size(), 1u);
}

// ---------------------------------------------------------------------------
// clear_fault_injection resets all knobs.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, ClearFaultInjectionResets) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_drop_next_sends(5);
    p->mut_a().inject_send_error(ChannelError::WouldBlock, 5);
    p->mut_a().clear_fault_injection();

    fault_test_helpers::send_byte(p->a_proxy, 7);
    ASSERT_EQ(p->b_received.size(), 1u);
    EXPECT_EQ(p->b_received.front().front(), static_cast<std::uint8_t>(7));
}

// ---------------------------------------------------------------------------
// Fault injection respects close: send_frame after close still
// returns ConnectionReset regardless of injection state.
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, FaultInjectionRespectsClose) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_drop_next_sends(10);
    p->mut_a().close();

    std::uint8_t b = 0;
    ChannelFrame f{&b, 1};
    // Closed state takes precedence: ConnectionReset, not None.
    EXPECT_EQ(p->a_proxy->send_frame(f), ChannelError::ConnectionReset);
}

// ---------------------------------------------------------------------------
// Calling inject_drop_next_sends with 0 effectively clears the
// drop counter (does not add).
// ---------------------------------------------------------------------------

TEST_F(InMemoryChannelTest, InjectDropZeroClears) {
    auto p = fault_test_helpers::make_pair_with_capture("addr-A", "addr-B");

    p->mut_a().inject_drop_next_sends(3);
    p->mut_a().inject_drop_next_sends(0);  // clears the counter

    fault_test_helpers::send_byte(p->a_proxy, 9);
    ASSERT_EQ(p->b_received.size(), 1u);
    EXPECT_EQ(p->b_received.front().front(), static_cast<std::uint8_t>(9));
}

}  // namespace
}  // namespace rrr
