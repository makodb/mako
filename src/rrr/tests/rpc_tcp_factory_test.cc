// End-to-end integration tests for the TCP channel backend's
// `TcpFactory`.
//
// Strategy: create a real `PollThread`, build a `TcpFactory` against
// it, and exercise the full connect / listen / accept / send / recv /
// close flow over `127.0.0.1` loopback. The factory auto-registers
// connections (from `connect`) and listeners (from `make_listener`,
// on a successful `listen`) with the poll thread, so the data path
// runs without manual fd plumbing.
//
// What's covered:
//   - factory.backend_name() reports "tcp".
//   - factory.connect to an unbound localhost port fails with
//     ConnectionRefused.
//   - factory.connect to an invalid address fails with AddressInvalid.
//   - End-to-end: factory.make_listener() + listen("127.0.0.1:0") +
//     factory.connect(local_address) -> on_accept fires + bidirectional
//     frame exchange round-trips.
//   - Closing the client side fires on_closed on the server side.
//   - Multiple sequential connect+frame exchanges through the same
//     listener.
//   - Factory channel proxy (`rusty::Box<ChannelFactoryBase>`)
//     forwards all ops to the underlying TcpFactory.

#include <gtest/gtest.h>


#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rusty/arc.hpp>

#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

using namespace std::chrono_literals;

constexpr auto kStepWait = 5ms;
constexpr auto kMaxWait  = 5s;

// Spin-wait for a predicate to become true, up to a wall-clock cap.
// Returns true on success, false on timeout. Used everywhere because
// the poll thread is asynchronous: registrations, frame deliveries,
// and close callbacks all flow through it on its own schedule.
template <typename F>
bool wait_for(F&& predicate, std::chrono::milliseconds max = kMaxWait) {
    auto deadline = std::chrono::steady_clock::now() + max;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(kStepWait);
    }
    return true;
}

class TcpFactoryTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
        factory_arc_ = rusty::Some(rusty::Arc<TcpFactory>::make(poll_thread_.as_ref().unwrap().clone()));
    }

    void TearDown() override {
        // Drop owned proxies / Arcs in inverse order. The poll thread
        // tears down its registered pollables on shutdown.
        factory_arc_ = rusty::None;
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
            poll_thread_ = rusty::None;
        }
    }

    TcpFactory& mut_factory() {
        return const_cast<TcpFactory&>(*factory_arc_.as_ref().unwrap().get());
    }
    const TcpFactory& factory() const {
        return *factory_arc_.as_ref().unwrap().get();
    }

    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    rusty::Option<rusty::Arc<TcpFactory>> factory_arc_;
};

// ---------------------------------------------------------------------------
// Trivial properties
// ---------------------------------------------------------------------------

TEST_F(TcpFactoryTest, BackendNameIsTcp) {
    EXPECT_STREQ(factory().backend_name(), "tcp");
}

TEST_F(TcpFactoryTest, ConnectInvalidAddressFails) {
    auto r = mut_factory().connect("not-an-address");
    EXPECT_EQ(r.error, ChannelError::AddressInvalid);
    EXPECT_FALSE(static_cast<bool>(r.connection));
}

TEST_F(TcpFactoryTest, ConnectUnboundPortFailsConnectionRefused) {
    // Bind a socket to grab a port, close it, then immediately try
    // to connect — the kernel should respond with ECONNREFUSED on
    // the localhost loopback.
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(probe, 0);
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    ASSERT_EQ(0, ::bind(probe, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)));
    socklen_t slen = sizeof(sa);
    ASSERT_EQ(0, ::getsockname(probe, reinterpret_cast<sockaddr*>(&sa), &slen));
    int port = ntohs(sa.sin_port);
    ::close(probe);

    char addr[64];
    std::snprintf(addr, sizeof(addr), "127.0.0.1:%d", port);

    auto r = mut_factory().connect(addr);
    // We accept either ConnectionRefused or, if a TIME_WAIT shadow
    // intercepts, ConnectionReset / Timeout. Localhost loopback in
    // a quiet test process should produce ConnectionRefused, but
    // the test stays robust against a spuriously-bound TIME_WAIT.
    EXPECT_FALSE(static_cast<bool>(r.connection));
    EXPECT_TRUE(r.error == ChannelError::ConnectionRefused
                || r.error == ChannelError::ConnectionReset
                || r.error == ChannelError::Timeout)
        << "got error " << channel_error_to_string(r.error);
}

// ---------------------------------------------------------------------------
// End-to-end: listen + connect + bidirectional frame exchange
// ---------------------------------------------------------------------------

TEST_F(TcpFactoryTest, EndToEndFrameRoundTrip) {
    // Server side: factory-built listener.
    auto listener = mut_factory().make_listener();
    ASSERT_EQ(listener->listen("127.0.0.1:0"), ChannelError::None);
    const std::string local_addr = listener->local_address();
    ASSERT_FALSE(local_addr.empty());

    // Track the accepted connection on the server side. We pin the
    // proxy in shared state because `on_accept` is called on the
    // poll thread.
    std::mutex accept_mu;
    rusty::Option<ChannelConnectionProxy> server_side_conn;
    std::vector<std::vector<std::uint8_t>> server_received;

    listener->set_on_accept([&](ChannelConnectionProxy proxy) {
        std::lock_guard<std::mutex> g(accept_mu);
        proxy->set_on_frame([&](const ChannelFrame& f) {
            std::lock_guard<std::mutex> g2(accept_mu);
            server_received.emplace_back(f.payload, f.payload + f.size);
        });
        server_side_conn = rusty::Some(std::move(proxy));
    });

    // Client side: factory.connect().
    auto cresult = mut_factory().connect(local_addr);
    ASSERT_EQ(cresult.error, ChannelError::None);
    ASSERT_TRUE(static_cast<bool>(cresult.connection));

    std::mutex client_mu;
    std::vector<std::vector<std::uint8_t>> client_received;
    cresult.connection->set_on_frame([&](const ChannelFrame& f) {
        std::lock_guard<std::mutex> g(client_mu);
        client_received.emplace_back(f.payload, f.payload + f.size);
    });

    // Wait for accept to surface on the poll thread.
    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(accept_mu);
        return server_side_conn.is_some();
    })) << "accept did not fire within deadline";

    // Send a frame client → server.
    const std::uint8_t c2s[] = {0xC1, 0xC2, 0xC3, 0xC4};
    EXPECT_EQ(cresult.connection->send_frame({c2s, sizeof(c2s)}),
              ChannelError::None);

    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(accept_mu);
        return !server_received.empty();
    })) << "server did not receive frame";

    {
        std::lock_guard<std::mutex> g(accept_mu);
        ASSERT_EQ(server_received.size(), 1u);
        ASSERT_EQ(server_received[0].size(), sizeof(c2s));
        EXPECT_EQ(0, std::memcmp(server_received[0].data(), c2s, sizeof(c2s)));
    }

    // Send a frame server → client.
    const std::uint8_t s2c[] = {0xD1, 0xD2};
    {
        std::lock_guard<std::mutex> g(accept_mu);
        ASSERT_TRUE(server_side_conn.is_some());
        EXPECT_EQ(server_side_conn.as_ref().unwrap()->send_frame({s2c, sizeof(s2c)}),
                  ChannelError::None);
    }

    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(client_mu);
        return !client_received.empty();
    })) << "client did not receive frame";

    {
        std::lock_guard<std::mutex> g(client_mu);
        ASSERT_EQ(client_received.size(), 1u);
        ASSERT_EQ(client_received[0].size(), sizeof(s2c));
        EXPECT_EQ(0, std::memcmp(client_received[0].data(), s2c, sizeof(s2c)));
    }

    // Tidy up.
    cresult.connection->close();
    {
        std::lock_guard<std::mutex> g(accept_mu);
        if (server_side_conn) server_side_conn.as_ref().unwrap()->close();
    }
    listener->close();
}

// ---------------------------------------------------------------------------
// Disconnect propagation: closing one side fires on_closed on the other
// ---------------------------------------------------------------------------

TEST_F(TcpFactoryTest, ClientCloseFiresServerOnClosed) {
    auto listener = mut_factory().make_listener();
    ASSERT_EQ(listener->listen("127.0.0.1:0"), ChannelError::None);
    const std::string local_addr = listener->local_address();

    std::mutex mu;
    rusty::Option<ChannelConnectionProxy> server_conn;
    int server_closes = 0;
    listener->set_on_accept([&](ChannelConnectionProxy proxy) {
        std::lock_guard<std::mutex> g(mu);
        proxy->set_on_closed([&](ChannelError) {
            std::lock_guard<std::mutex> g2(mu);
            ++server_closes;
        });
        server_conn = rusty::Some(std::move(proxy));
    });

    auto cresult = mut_factory().connect(local_addr);
    ASSERT_EQ(cresult.error, ChannelError::None);

    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(mu);
        return server_conn.is_some();
    }));

    cresult.connection->close();

    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(mu);
        return server_closes >= 1;
    })) << "server-side on_closed did not fire after client close";

    {
        std::lock_guard<std::mutex> g(mu);
        EXPECT_EQ(server_closes, 1);
        if (server_conn) server_conn.as_ref().unwrap()->close();
    }
    listener->close();
}

// ---------------------------------------------------------------------------
// Multiple sequential connections through one listener
// ---------------------------------------------------------------------------

TEST_F(TcpFactoryTest, MultipleSequentialConnects) {
    auto listener = mut_factory().make_listener();
    ASSERT_EQ(listener->listen("127.0.0.1:0"), ChannelError::None);
    const std::string local_addr = listener->local_address();

    std::mutex mu;
    int accepts = 0;
    std::vector<ChannelConnectionProxy> server_conns;
    listener->set_on_accept([&](ChannelConnectionProxy proxy) {
        std::lock_guard<std::mutex> g(mu);
        ++accepts;
        server_conns.push_back(std::move(proxy));
    });

    constexpr int kClients = 5;
    std::vector<ChannelConnectionProxy> client_conns;
    for (int i = 0; i < kClients; ++i) {
        auto r = mut_factory().connect(local_addr);
        ASSERT_EQ(r.error, ChannelError::None) << "client " << i;
        client_conns.push_back(std::move(r.connection));
    }

    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(mu);
        return accepts == kClients;
    })) << "expected " << kClients << " accepts";

    {
        std::lock_guard<std::mutex> g(mu);
        EXPECT_EQ(accepts, kClients);
        EXPECT_EQ(server_conns.size(), static_cast<std::size_t>(kClients));
    }

    for (auto& c : client_conns) c->close();
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& c : server_conns) c->close();
    }
    listener->close();
}

// ---------------------------------------------------------------------------
// Factory channel proxy facade dispatch
// ---------------------------------------------------------------------------

TEST_F(TcpFactoryTest, FactoryChannelProxyForwardsAllOps) {
    auto proxy = make_tcp_factory_proxy(factory_arc_.as_ref().unwrap().clone());

    EXPECT_STREQ(proxy->backend_name(), "tcp");

    auto listener = proxy->make_listener();
    ASSERT_EQ(listener->listen("127.0.0.1:0"), ChannelError::None);
    const std::string local_addr = listener->local_address();
    ASSERT_FALSE(local_addr.empty());

    // Verify that connect through the proxy lands on a usable
    // accepted connection on the listener side.
    std::mutex mu;
    rusty::Option<ChannelConnectionProxy> server_conn;
    listener->set_on_accept([&](ChannelConnectionProxy p) {
        std::lock_guard<std::mutex> g(mu);
        server_conn = rusty::Some(std::move(p));
    });

    auto r = proxy->connect(local_addr);
    ASSERT_EQ(r.error, ChannelError::None);
    ASSERT_TRUE(static_cast<bool>(r.connection));

    EXPECT_TRUE(wait_for([&] {
        std::lock_guard<std::mutex> g(mu);
        return server_conn.is_some();
    }));

    r.connection->close();
    {
        std::lock_guard<std::mutex> g(mu);
        if (server_conn) server_conn.as_ref().unwrap()->close();
    }
    listener->close();
}

}  // namespace
}  // namespace rrr
