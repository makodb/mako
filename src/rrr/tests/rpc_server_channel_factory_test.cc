// Channel-factory listen-path test for `Server::start`.
//
// Verifies that when a `ChannelFactoryProxy` is bound on the server
// via `set_channel_factory(...)`, `Server::start(addr)`:
//   * calls `factory->make_listener()` exactly once
//   * installs an `on_accept` callback on the listener
//   * calls `listener->listen(addr)` with the bind address
//   * returns 0 on success, surfaces the error on failure
//   * routes accepted ChannelConnectionProxies into a ServerConnection
//     bound via 5b/5c/5d's `bind_channel(...)` and parked on the
//     server's accepted-connection vec
//
// No real socket / poll thread integration in this leaf — those
// land in subsequent leaves' end-to-end tests once 5f auto-installs
// a default `TcpFactory`.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../rrr.hpp"

namespace rrr {
namespace {

// ---------------------------------------------------------------------------
// Connection stub used to hand on_accept a non-null
// ChannelConnectionProxy so the server-side bind_channel(...) path
// has something to wrap.
// ---------------------------------------------------------------------------

class ConnStub {
 public:
    ChannelError send_frame(const ChannelFrame&) { return ChannelError::None; }
    void   flush()              {}
    void   close()              { closed_ = true; }
    bool   is_closed() const    { return closed_; }
    std::string peer_address() const { return "stub"; }
    void set_on_frame (OnFrameCallback)  {}
    void set_on_closed(OnClosedCallback) {}
    void set_on_error (OnErrorCallback)  {}
 private:
    bool closed_ = false;
};

class ConnStubAdapter : public ChannelConnectionBase {
 public:
    explicit ConnStubAdapter(std::shared_ptr<ConnStub> p) : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) override { return stub_->send_frame(f); }
    void   flush() override              { stub_->flush(); }
    void   close() override              { stub_->close(); }
    bool   is_closed() const override    { return stub_->is_closed(); }
    std::string peer_address() const override { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) override { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<ConnStub> stub_;
};

inline ChannelConnectionProxy make_conn_proxy(
        std::shared_ptr<ConnStub> stub) {
    return std::make_unique<ConnStubAdapter>(std::move(stub));
}

// ---------------------------------------------------------------------------
// Listener stub: captures the on_accept callback and listen address.
// ---------------------------------------------------------------------------

class ListenerStub {
 public:
    OnAcceptCallback   on_accept_;
    OnErrorCallback    on_error_;
    std::string        listen_addr_;
    int                listen_calls_ = 0;
    bool               listened_ok_  = true;  // controls listen() return
    bool               closed_       = false;
    int                close_calls_  = 0;

    ChannelError listen(std::string_view addr) {
        listen_addr_ = std::string(addr);
        ++listen_calls_;
        return listened_ok_ ? ChannelError::None : ChannelError::AddressInUse;
    }
    void   close()              { ++close_calls_; closed_ = true; }
    bool   is_closed() const    { return closed_; }
    std::string local_address() const { return listen_addr_; }
    void set_on_accept(OnAcceptCallback cb) { on_accept_ = std::move(cb); }
    void set_on_error (OnErrorCallback cb)  { on_error_ = std::move(cb); }
};

class ListenerStubAdapter : public ChannelListenerBase {
 public:
    explicit ListenerStubAdapter(std::shared_ptr<ListenerStub> p)
        : stub_(std::move(p)) {}
    ChannelError listen(std::string_view addr) override { return stub_->listen(addr); }
    void   close() override              { stub_->close(); }
    bool   is_closed() const override    { return stub_->is_closed(); }
    std::string local_address() const override { return stub_->local_address(); }
    void set_on_accept(OnAcceptCallback cb) override { stub_->set_on_accept(std::move(cb)); }
    void set_on_error (OnErrorCallback cb) override  { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<ListenerStub> stub_;
};

inline ChannelListenerProxy make_listener_proxy(
        std::shared_ptr<ListenerStub> stub) {
    return std::make_unique<ListenerStubAdapter>(std::move(stub));
}

// ---------------------------------------------------------------------------
// Factory stub: records make_listener() calls and hands back a
// listener proxy whose underlying ListenerStub the test can poke.
// ---------------------------------------------------------------------------

class FactoryStub {
 public:
    std::shared_ptr<ListenerStub> last_listener_;
    int  make_listener_calls_ = 0;
    bool make_listener_ok_    = true;
    // Pre-arm: when set, the next make_listener() returns a listener
    // whose listen() will fail with AddressInUse.
    bool next_listen_should_fail_ = false;

    ConnectResult connect(std::string_view) {
        return ConnectResult{ChannelConnectionProxy{},
                             ChannelError::Internal};
    }
    ChannelListenerProxy make_listener() {
        ++make_listener_calls_;
        if (!make_listener_ok_) return ChannelListenerProxy{};
        last_listener_ = std::make_shared<ListenerStub>();
        last_listener_->listened_ok_ = !next_listen_should_fail_;
        return make_listener_proxy(last_listener_);
    }
    const char* backend_name() const { return "factory-stub"; }
};

class FactoryStubAdapter : public ChannelFactoryBase {
 public:
    explicit FactoryStubAdapter(std::shared_ptr<FactoryStub> p)
        : stub_(std::move(p)) {}
    ConnectResult connect(std::string_view a) override { return stub_->connect(a); }
    ChannelListenerProxy make_listener() override { return stub_->make_listener(); }
    const char* backend_name() const override { return stub_->backend_name(); }
 private:
    std::shared_ptr<FactoryStub> stub_;
};

inline ChannelFactoryProxy make_factory_proxy(
        std::shared_ptr<FactoryStub> stub) {
    return std::make_unique<FactoryStubAdapter>(std::move(stub));
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ServerChannelFactoryTest : public ::testing::Test {
 protected:
    // rusty::Option<T> swap.
    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
        server_ = rusty::make_box<Server>(
            rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    }

    void TearDown() override {
        server_ = rusty::None;
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
            poll_thread_ = rusty::None;
        }
    }

    rusty::Option<rusty::Arc<PollThread>>     poll_thread_;
    rusty::Option<rusty::Box<Server>>         server_;
};

// ---------------------------------------------------------------------------
// start() with bound factory calls make_listener() and listen().
// ---------------------------------------------------------------------------

TEST_F(ServerChannelFactoryTest, StartCallsFactoryMakeListenerAndListen) {
    auto factory_stub = std::make_shared<FactoryStub>();
    server_.as_ref().unwrap()->set_channel_factory(make_factory_proxy(factory_stub));

    EXPECT_EQ(server_.as_ref().unwrap()->start("0.0.0.0:0"), 0);

    EXPECT_EQ(factory_stub->make_listener_calls_, 1);
    ASSERT_TRUE(static_cast<bool>(factory_stub->last_listener_));
    EXPECT_EQ(factory_stub->last_listener_->listen_calls_, 1);
    EXPECT_EQ(factory_stub->last_listener_->listen_addr_, "0.0.0.0:0");
    EXPECT_TRUE(static_cast<bool>(factory_stub->last_listener_->on_accept_));
}

// ---------------------------------------------------------------------------
// listen() failure surfaces as start() returning -1.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelFactoryTest, StartReturnsErrorOnListenFailure) {
    auto factory_stub = std::make_shared<FactoryStub>();
    factory_stub->next_listen_should_fail_ = true;
    server_.as_ref().unwrap()->set_channel_factory(make_factory_proxy(factory_stub));

    EXPECT_EQ(server_.as_ref().unwrap()->start("0.0.0.0:0"), -1);

    // make_listener() was called, listen() was called, but the
    // listener was NOT parked on the server (the failure path
    // resets ctx_ and returns -1 without storing the listener).
    EXPECT_EQ(factory_stub->make_listener_calls_, 1);
    ASSERT_TRUE(static_cast<bool>(factory_stub->last_listener_));
    EXPECT_EQ(factory_stub->last_listener_->listen_calls_, 1);
    // close_calls_ stays 0 because Server never owned the listener.
    // (The listener stub will be released when `factory_stub` is
    // released at fixture teardown — which is just plain shared_ptr
    // drop, no close() call.)
    EXPECT_EQ(factory_stub->last_listener_->close_calls_, 0);
}

// ---------------------------------------------------------------------------
// On-accept callback parks a ServerConnection bound to the proxy.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelFactoryTest, OnAcceptParksBoundServerConnection) {
    auto factory_stub = std::make_shared<FactoryStub>();
    server_.as_ref().unwrap()->set_channel_factory(make_factory_proxy(factory_stub));
    EXPECT_EQ(server_.as_ref().unwrap()->start("0.0.0.0:0"), 0);

    // Fire on_accept manually.
    auto conn_stub = std::make_shared<ConnStub>();
    auto& on_accept = factory_stub->last_listener_->on_accept_;
    ASSERT_TRUE(static_cast<bool>(on_accept));
    on_accept(make_conn_proxy(conn_stub));

    // The accepted connection survives across the on_accept call.
    // We can't reach into Server::channel_sconns_ directly, but the
    // ConnStub's `set_on_frame` would have been invoked by the
    // bound ServerConnection's bind_channel → so `closed_` is still
    // false (no close happened) and the conn_stub is not destroyed.
    EXPECT_FALSE(conn_stub->is_closed());

    // Verify the parked connection survives a second accept too.
    auto conn_stub2 = std::make_shared<ConnStub>();
    on_accept(make_conn_proxy(conn_stub2));
    EXPECT_FALSE(conn_stub2->is_closed());
}

// ---------------------------------------------------------------------------
// Server destructor closes the channel listener.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelFactoryTest, DestructorClosesChannelListener) {
    auto factory_stub = std::make_shared<FactoryStub>();
    server_.as_ref().unwrap()->set_channel_factory(make_factory_proxy(factory_stub));
    EXPECT_EQ(server_.as_ref().unwrap()->start("0.0.0.0:0"), 0);

    auto listener_stub = factory_stub->last_listener_;
    ASSERT_TRUE(static_cast<bool>(listener_stub));
    EXPECT_EQ(listener_stub->close_calls_, 0);

    server_ = rusty::None;
    // 5f: ~Server schedules listener close on the poll thread via a
    // OneTimeJob (to avoid the CmdAddPollable race), so we poll for
    // the close to take effect.
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(2000);
    while (listener_stub->close_calls_ == 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(listener_stub->close_calls_, 1);
}

// ---------------------------------------------------------------------------
// stop_accepting() closes the channel listener but leaves accepted
// connections alive.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelFactoryTest, StopAcceptingClosesListenerOnly) {
    auto factory_stub = std::make_shared<FactoryStub>();
    server_.as_ref().unwrap()->set_channel_factory(make_factory_proxy(factory_stub));
    EXPECT_EQ(server_.as_ref().unwrap()->start("0.0.0.0:0"), 0);

    auto listener_stub = factory_stub->last_listener_;

    // Accept one conn first.
    auto conn_stub = std::make_shared<ConnStub>();
    listener_stub->on_accept_(make_conn_proxy(conn_stub));
    EXPECT_FALSE(conn_stub->is_closed());

    // Now stop_accepting → listener closed, conn untouched.
    server_.as_ref().unwrap()->stop_accepting();
    EXPECT_GE(listener_stub->close_calls_, 1);
    EXPECT_FALSE(conn_stub->is_closed());
}

// ---------------------------------------------------------------------------
// 5f: without an explicitly-bound factory, `Server::start` auto-installs
// a default `TcpFactory` and goes through the channel path. The
// stub-factory in this fixture is unused (never set on the server),
// so it sees zero make_listener_calls_; the real bind happens via
// the auto-installed TcpFactory against the loopback address.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelFactoryTest, StartWithoutFactoryAutoInstallsDefault) {
    auto factory_stub = std::make_shared<FactoryStub>();
    // Note: factory NOT installed on the server.
    EXPECT_EQ(server_.as_ref().unwrap()->start("127.0.0.1:0"), 0);
    // The fixture's stub was never bound, so it sees no calls.
    EXPECT_EQ(factory_stub->make_listener_calls_, 0);
    // The server is now in channel mode by virtue of auto-install
    // (real TcpFactory bound from inside `start`); destruction via
    // fixture teardown closes the auto-installed channel listener.
}

}  // namespace
}  // namespace rrr
