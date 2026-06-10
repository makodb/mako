// Channel-factory connect/reconnect test for `ClientConnection`
//.
//
// Verifies that when a `ChannelFactoryProxy` has been bound via
// `bind_factory(...)`, the connection routes its `connect(addr)`
// (and the close fan-out's reconnect spawn) through
// `factory->connect(addr)` instead of the legacy fd path. The
// returned proxy is automatically handed to `bind_channel(...)`,
// so the connection enters channel mode and the recv-loop fiber
// is spawned without any caller-driven setup.
//
// Strategy: a `FakeChannelFactory` stub implements
// `ChannelFactoryBase` and returns a `ChannelConnectionProxy`
// backed by a `FakeChannelStub` (the same kind used by the 4c1 /
// 4c2 / 4d tests). The fixture observes:
//   - `connect_count` to verify the factory was invoked exactly
//     once on `Client::connect`.
//   - `bind_channel` side-effects: `is_channel_mode()` flips true
//     and a `request(...)` produces a captured outbound frame on
//     the stub.
//   - Reconnect: `stub->deliver_closed(...)` triggers the close
//     fan-out, which (with auto-reconnect on + reconnect_address_
//     non-empty) spawns a thread that re-calls
//     `factory->connect(addr)`. The counter bumps by 1.

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

// ---------------------------------------------------------------------------
// FakeChannelStub — minimal ChannelConnectionBase implementation.
// (Same shape as the 4c1/4c2 tests: captures outbound, drives close.)
// ---------------------------------------------------------------------------

class FakeChannelStub {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        std::lock_guard<std::mutex> g(mu_);
        sent_.emplace_back(f.payload, f.payload + f.size);
        return ChannelError::None;
    }
    void   flush()                     {}
    void   close()                     { closed_ = true; }
    bool   is_closed() const           { return closed_; }
    std::string peer_address() const   { return "fake"; }
    void set_on_frame (OnFrameCallback  cb) { on_frame_  = std::move(cb); }
    void set_on_closed(OnClosedCallback cb) { on_closed_ = std::move(cb); }
    void set_on_error (OnErrorCallback  cb) { on_error_  = std::move(cb); }

    void deliver_closed(ChannelError reason = ChannelError::ConnectionReset) {
        closed_ = true;
        if (on_closed_) on_closed_(reason);
    }
    std::size_t send_count() {
        std::lock_guard<std::mutex> g(mu_);
        return sent_.size();
    }

 private:
    std::mutex mu_;
    std::vector<std::vector<std::uint8_t>> sent_;
    OnFrameCallback  on_frame_;
    OnClosedCallback on_closed_;
    OnErrorCallback  on_error_;
    bool closed_ = false;
};

class FakeChannelStubAdapter : public ChannelConnectionBase {
 public:
    explicit FakeChannelStubAdapter(std::shared_ptr<FakeChannelStub> p)
        : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) override { return stub_->send_frame(f); }
    void   flush() override                     { stub_->flush(); }
    void   close() override                     { stub_->close(); }
    bool   is_closed() const override           { return stub_->is_closed(); }
    std::string peer_address() const override   { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) override { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<FakeChannelStub> stub_;
};

inline ChannelConnectionProxy make_fake_channel_proxy(
    std::shared_ptr<FakeChannelStub> stub) {
    return rusty::make_box<FakeChannelStubAdapter>(std::move(stub));
}

// ---------------------------------------------------------------------------
// FakeChannelFactory — minimal ChannelFactoryBase implementation.
// ---------------------------------------------------------------------------
//
// Each `connect(addr)` call records the address, returns a fresh
// `FakeChannelStub`-backed `ChannelConnectionProxy`, and increments
// `connect_count`. Tests can preset the next-error to simulate a
// failed connect (e.g. ConnectionRefused).

class FakeChannelFactory {
 public:
    ConnectResult connect(std::string_view addr) {
        std::lock_guard<std::mutex> g(mu_);
        connect_addrs_.push_back(std::string(addr));
        if (next_error_ != ChannelError::None) {
            ChannelError e = next_error_;
            next_error_ = ChannelError::None;
            return ConnectResult{rusty::None, e};
        }
        auto stub = std::make_shared<FakeChannelStub>();
        produced_stubs_.push_back(stub);
        return ConnectResult{rusty::Some(make_fake_channel_proxy(stub)),
                             ChannelError::None};
    }
    rusty::Option<ChannelListenerProxy> make_listener() {
        // Not exercised by these tests.
        return rusty::None;
    }
    std::string backend_name() const { return "fake"; }

    // Test introspection.
    std::size_t connect_count() {
        std::lock_guard<std::mutex> g(mu_);
        return connect_addrs_.size();
    }
    std::vector<std::string> connect_addrs() {
        std::lock_guard<std::mutex> g(mu_);
        return connect_addrs_;
    }
    std::shared_ptr<FakeChannelStub> last_stub() {
        std::lock_guard<std::mutex> g(mu_);
        return produced_stubs_.empty() ? nullptr : produced_stubs_.back();
    }
    std::size_t stub_count() {
        std::lock_guard<std::mutex> g(mu_);
        return produced_stubs_.size();
    }
    void set_next_error(ChannelError e) {
        std::lock_guard<std::mutex> g(mu_);
        next_error_ = e;
    }

 private:
    std::mutex mu_;
    std::vector<std::string> connect_addrs_;
    std::vector<std::shared_ptr<FakeChannelStub>> produced_stubs_;
    ChannelError next_error_ = ChannelError::None;
};

class FakeChannelFactoryAdapter : public ChannelFactoryBase {
 public:
    explicit FakeChannelFactoryAdapter(std::shared_ptr<FakeChannelFactory> p)
        : f_(std::move(p)) {}
    ConnectResult                       connect(std::string_view a) override { return f_->connect(a); }
    rusty::Option<ChannelListenerProxy> make_listener() override             { return f_->make_listener(); }
    std::string                         backend_name() const override        { return f_->backend_name(); }
 private:
    std::shared_ptr<FakeChannelFactory> f_;
};

inline ChannelFactoryProxy make_fake_factory_proxy(
    std::shared_ptr<FakeChannelFactory> f) {
    return rusty::make_box<FakeChannelFactoryAdapter>(std::move(f));
}

// ---------------------------------------------------------------------------
// Fixture — direct-Arc ClientConnection (no Client facade). Mirrors
// the 4c2/4d test fixtures because we want to exercise the
// connection's factory branch without going through Client::connect's
// full setup dance.
// ---------------------------------------------------------------------------

class ClientChannelFactoryTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
        conn_ = rusty::Some(rusty::Arc<ClientConnection>::make(poll_thread_.as_ref().unwrap().clone()));
        mut_conn().install_self_weak_for_testing(
            WeakClientConnection{rusty::sync::downgrade(conn_.as_ref().unwrap())});

        factory_ = std::make_shared<FakeChannelFactory>();
        mut_conn().bind_factory(make_fake_factory_proxy(factory_));
    }

    void TearDown() override {
        // Drop conn first; the recv-loop fiber holds a Weak so the
        // teardown is straightforward.
        conn_ = rusty::None;
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
            poll_thread_ = rusty::None;
        }
    }

    ClientConnection& mut_conn() {
        return const_cast<ClientConnection&>(*conn_.as_ref().unwrap().get());
    }

    rusty::Option<rusty::Arc<PollThread>>       poll_thread_;
    rusty::Option<rusty::Arc<ClientConnection>> conn_;
    std::shared_ptr<FakeChannelFactory>         factory_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(ClientChannelFactoryTest, ConnectRoutesThroughFactory) {
    EXPECT_FALSE(mut_conn().is_channel_mode());
    EXPECT_EQ(factory_->connect_count(), 0u);

    int rc = mut_conn().connect("fake-addr:0");

    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(mut_conn().is_channel_mode());
    ASSERT_EQ(factory_->connect_count(), 1u);
    EXPECT_EQ(factory_->connect_addrs()[0], "fake-addr:0");
    EXPECT_EQ(mut_conn().connection_state(), ConnectionState::CONNECTED);
}

TEST_F(ClientChannelFactoryTest, ConnectViaFactoryEnablesRequestDispatch) {
    ASSERT_EQ(mut_conn().connect("fake-addr:0"), 0);
    auto stub = factory_->last_stub();
    ASSERT_NE(stub, nullptr);

    // Issue a request — it should land on the stub the factory
    // produced (channel-mode dispatch).
    auto fr = mut_conn().request(0x42, FutureAttr{}, [](BinaryWriteArchive& m) {
        m << static_cast<i32>(0xABCD);
    });
    ASSERT_TRUE(fr.is_ok());
    EXPECT_EQ(stub->send_count(), 1u);
}

TEST_F(ClientChannelFactoryTest, ConnectFailureSurfacesAsErrno) {
    factory_->set_next_error(ChannelError::ConnectionRefused);

    int rc = mut_conn().connect("fake-addr:0");

    EXPECT_EQ(rc, ECONNREFUSED);
    EXPECT_FALSE(mut_conn().is_channel_mode());
    EXPECT_EQ(factory_->connect_count(), 1u);
    EXPECT_EQ(mut_conn().connection_state(), ConnectionState::FAILED);
}

TEST_F(ClientChannelFactoryTest,
       OnClosedReconnectReusesFactoryAndProducesNewStub) {
    // Configure auto-reconnect + factory-driven reconnect.
    ReconnectPolicy policy;
    policy.auto_reconnect = true;
    mut_conn().set_reconnect_policy(policy);

    ASSERT_EQ(mut_conn().connect("fake-addr:0"), 0);
    ASSERT_EQ(factory_->connect_count(), 1u);

    auto first_stub = factory_->last_stub();
    ASSERT_NE(first_stub, nullptr);

    // Trigger close — fan-out should invalidate pending futures and
    // (with auto-reconnect on + reconnect_address_ non-empty + factory
    // bound) spawn a thread that re-calls factory->connect.
    first_stub->deliver_closed(ChannelError::ConnectionReset);

    // The reconnect spawn is on a separate thread; poll the factory's
    // connect_count for up to 2 seconds. Don't pump the reactor —
    // the spawn body re-enters connect synchronously on its own
    // thread.
    auto reactor = Reactor::get_reactor();
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           factory_->connect_count() < 2u) {
        // Drive the test-thread reactor too so the recv-loop fiber's
        // close-side fan-out runs.
        reactor->loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_GE(factory_->connect_count(), 2u);
    EXPECT_GE(mut_conn().channel_reconnect_attempts_count(), 1u);

    auto addrs = factory_->connect_addrs();
    ASSERT_GE(addrs.size(), 2u);
    EXPECT_EQ(addrs[0], "fake-addr:0");
    EXPECT_EQ(addrs[1], "fake-addr:0");

    // After the spawn completes, channel_mode_ should be re-enabled
    // with the new stub, and is_channel_mode() should report true
    // again.
    EXPECT_TRUE(mut_conn().is_channel_mode());

    // Stop the spawn thread cleanly: abort_reconnect to short-circuit
    // any further attempts, then close the most recent stub.
    mut_conn().abort_reconnect();
    auto last_stub = factory_->last_stub();
    if (last_stub && !last_stub->is_closed()) {
        last_stub->deliver_closed(ChannelError::None);
        for (int i = 0; i < 5; ++i) reactor->loop();
    }
}

TEST_F(ClientChannelFactoryTest, IsFactoryBoundAccessor) {
    // Sanity: the fixture binds the factory in SetUp.
    EXPECT_TRUE(mut_conn().is_factory_bound());
}

}  // namespace
}  // namespace rrr
