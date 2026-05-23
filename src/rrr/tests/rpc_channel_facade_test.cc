// Guard test for the SRPC channel core interfaces.
//
// Validates that the virtual base contracts in `rpc/channel.hpp` are
// coherent: a fake implementation can satisfy each base, and dispatch
// through the Box<Base> reaches the implementation. No real network I/O.
//
// The fakes are exposed via thin adapters that hold `std::shared_ptr<Fake>`
// and forward every base method to the underlying fake, letting tests
// retain a handle to the fake state through their own shared_ptr.

#include <gtest/gtest.h>


#include <rusty/box.hpp>
#include <rusty/option.hpp>

#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

// ---------------------------------------------------------------------------
// FakeConnection: records every dispatched call, drives callbacks via
// helper methods that the test invokes.
// ---------------------------------------------------------------------------
class FakeConnection {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        last_send_size_ = f.size;
        ++send_calls_;
        return send_result_;
    }
    void   flush()              { ++flush_calls_; }
    void   close()              { closed_ = true; ++close_calls_; }
    bool   is_closed() const    { return closed_; }
    std::string peer_address() const { return peer_; }

    void set_on_frame(OnFrameCallback cb)   { on_frame_ = std::move(cb); }
    void set_on_closed(OnClosedCallback cb) { on_closed_ = std::move(cb); }
    void set_on_error(OnErrorCallback cb)   { on_error_ = std::move(cb); }

    // Test helpers to drive callbacks as a real backend would.
    void deliver(const ChannelFrame& f)               { if (on_frame_) on_frame_(f); }
    void deliver_closed(ChannelError reason)          { if (on_closed_) on_closed_(reason); }
    void deliver_error(ChannelError e, std::string_view m) { if (on_error_) on_error_(e, m); }

    void set_send_result(ChannelError e) { send_result_ = e; }
    int  send_calls() const  { return send_calls_; }
    int  flush_calls() const { return flush_calls_; }
    int  close_calls() const { return close_calls_; }
    std::size_t last_send_size() const { return last_send_size_; }

 private:
    OnFrameCallback  on_frame_;
    OnClosedCallback on_closed_;
    OnErrorCallback  on_error_;

    bool closed_ = false;
    ChannelError send_result_ = ChannelError::None;
    int send_calls_  = 0;
    int flush_calls_ = 0;
    int close_calls_ = 0;
    std::size_t last_send_size_ = 0;
    std::string peer_ = "127.0.0.1:9000";
};

// Adapter wraps a `shared_ptr<FakeConnection>` and exposes the
// ChannelConnectionBase member surface. Lets the proxy own the adapter
// while the test keeps the underlying fake reachable through its own
// shared_ptr.
class FakeConnectionAdapter : public ChannelConnectionBase {
 public:
    explicit FakeConnectionAdapter(std::shared_ptr<FakeConnection> conn)
        : conn_(std::move(conn)) {}

    ChannelError send_frame(const ChannelFrame& f) override { return conn_->send_frame(f); }
    void   flush() override                                 { conn_->flush(); }
    void   close() override                                 { conn_->close(); }
    bool   is_closed() const override                       { return conn_->is_closed(); }
    std::string peer_address() const override               { return conn_->peer_address(); }
    void set_on_frame(OnFrameCallback cb) override          { conn_->set_on_frame(std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override        { conn_->set_on_closed(std::move(cb)); }
    void set_on_error(OnErrorCallback cb) override          { conn_->set_on_error(std::move(cb)); }

 private:
    std::shared_ptr<FakeConnection> conn_;
};

inline ChannelConnectionProxy make_fake_conn_proxy(std::shared_ptr<FakeConnection> c) {
    return rusty::make_box<FakeConnectionAdapter>(std::move(c));
}

// ---------------------------------------------------------------------------
// FakeListener and adapter.
// ---------------------------------------------------------------------------
class FakeListener {
 public:
    ChannelError listen(std::string_view addr) {
        bound_ = std::string(addr);
        return listen_result_;
    }
    void close()                    { closed_ = true; }
    bool is_closed() const          { return closed_; }
    std::string local_address() const { return bound_; }

    void set_on_accept(OnAcceptCallback cb) { on_accept_ = std::move(cb); }
    void set_on_error(OnErrorCallback cb)   { on_error_ = std::move(cb); }

    void deliver_accept(ChannelConnectionProxy c) { if (on_accept_) on_accept_(std::move(c)); }
    void set_listen_result(ChannelError e) { listen_result_ = e; }

 private:
    OnAcceptCallback on_accept_;
    OnErrorCallback  on_error_;
    bool closed_ = false;
    std::string bound_;
    ChannelError listen_result_ = ChannelError::None;
};

class FakeListenerAdapter : public ChannelListenerBase {
 public:
    explicit FakeListenerAdapter(std::shared_ptr<FakeListener> l)
        : listener_(std::move(l)) {}

    ChannelError listen(std::string_view addr) override { return listener_->listen(addr); }
    void close() override                               { listener_->close(); }
    bool is_closed() const override                     { return listener_->is_closed(); }
    std::string local_address() const override          { return listener_->local_address(); }
    void set_on_accept(OnAcceptCallback cb) override    { listener_->set_on_accept(std::move(cb)); }
    void set_on_error(OnErrorCallback cb) override      { listener_->set_on_error(std::move(cb)); }

 private:
    std::shared_ptr<FakeListener> listener_;
};

inline ChannelListenerProxy make_fake_listener_proxy(std::shared_ptr<FakeListener> l) {
    return rusty::make_box<FakeListenerAdapter>(std::move(l));
}

// ---------------------------------------------------------------------------
// FakeFactory and adapter.
// ---------------------------------------------------------------------------
class FakeFactory {
 public:
    ConnectResult connect(std::string_view addr) {
        last_connect_addr_ = std::string(addr);
        if (connect_result_ != ChannelError::None) {
            return ConnectResult{rusty::None, connect_result_};
        }
        return ConnectResult{
            rusty::Some(make_fake_conn_proxy(std::make_shared<FakeConnection>())),
            ChannelError::None,
        };
    }
    rusty::Option<ChannelListenerProxy> make_listener() {
        return rusty::Some(make_fake_listener_proxy(std::make_shared<FakeListener>()));
    }
    const char* backend_name() const { return "fake"; }

    void set_connect_result(ChannelError e) { connect_result_ = e; }
    const std::string& last_connect_addr() const { return last_connect_addr_; }

 private:
    ChannelError connect_result_ = ChannelError::None;
    std::string last_connect_addr_;
};

class FakeFactoryAdapter : public ChannelFactoryBase {
 public:
    explicit FakeFactoryAdapter(std::shared_ptr<FakeFactory> f)
        : factory_(std::move(f)) {}

    ConnectResult                       connect(std::string_view addr) override { return factory_->connect(addr); }
    rusty::Option<ChannelListenerProxy> make_listener() override                { return factory_->make_listener(); }
    const char*                         backend_name() const override           { return factory_->backend_name(); }

 private:
    std::shared_ptr<FakeFactory> factory_;
};

inline ChannelFactoryProxy make_fake_factory_proxy(std::shared_ptr<FakeFactory> f) {
    return rusty::make_box<FakeFactoryAdapter>(std::move(f));
}

// ===========================================================================
// Tests
// ===========================================================================

TEST(RpcChannelFacadeTest, ErrorEnumStringification) {
    EXPECT_STREQ("None",              channel_error_to_string(ChannelError::None));
    EXPECT_STREQ("WouldBlock",        channel_error_to_string(ChannelError::WouldBlock));
    EXPECT_STREQ("ConnectionRefused", channel_error_to_string(ChannelError::ConnectionRefused));
    EXPECT_STREQ("ConnectionReset",   channel_error_to_string(ChannelError::ConnectionReset));
    EXPECT_STREQ("Timeout",           channel_error_to_string(ChannelError::Timeout));
    EXPECT_STREQ("Internal",          channel_error_to_string(ChannelError::Internal));
}

TEST(RpcChannelFacadeTest, ConnectionDispatchForwardsAllOps) {
    auto fake = std::make_shared<FakeConnection>();
    auto proxy = make_fake_conn_proxy(fake);

    std::uint8_t buf[3] = {0x01, 0x02, 0x03};
    ChannelFrame f{buf, sizeof(buf)};
    EXPECT_EQ(proxy->send_frame(f), ChannelError::None);
    EXPECT_EQ(fake->send_calls(), 1);
    EXPECT_EQ(fake->last_send_size(), sizeof(buf));

    fake->set_send_result(ChannelError::WouldBlock);
    EXPECT_EQ(proxy->send_frame(f), ChannelError::WouldBlock);
    EXPECT_EQ(fake->send_calls(), 2);

    proxy->flush();
    EXPECT_EQ(fake->flush_calls(), 1);
    EXPECT_FALSE(proxy->is_closed());
    proxy->close();
    EXPECT_TRUE(proxy->is_closed());
    proxy->close();  // idempotent at the facade level
    EXPECT_EQ(fake->close_calls(), 2);

    EXPECT_EQ(proxy->peer_address(), "127.0.0.1:9000");
}

TEST(RpcChannelFacadeTest, ConnectionCallbacksReceiveDeliveredEvents) {
    auto fake = std::make_shared<FakeConnection>();
    auto proxy = make_fake_conn_proxy(fake);

    int frames_seen = 0;
    int closes_seen = 0;
    int errors_seen = 0;
    ChannelError last_close_reason = ChannelError::None;
    ChannelError last_error = ChannelError::None;
    std::string  last_error_msg;

    proxy->set_on_frame ([&](const ChannelFrame& f) {
        ++frames_seen;
        EXPECT_NE(f.payload, nullptr);
    });
    proxy->set_on_closed([&](ChannelError r) {
        ++closes_seen;
        last_close_reason = r;
    });
    proxy->set_on_error ([&](ChannelError e, std::string_view m) {
        ++errors_seen;
        last_error = e;
        last_error_msg = std::string(m);
    });

    std::uint8_t b[2] = {0xAA, 0xBB};
    ChannelFrame f{b, 2};
    fake->deliver(f);
    fake->deliver(f);
    fake->deliver_error(ChannelError::Timeout, "stalled");
    fake->deliver_closed(ChannelError::ConnectionReset);

    EXPECT_EQ(frames_seen, 2);
    EXPECT_EQ(errors_seen, 1);
    EXPECT_EQ(closes_seen, 1);
    EXPECT_EQ(last_error, ChannelError::Timeout);
    EXPECT_EQ(last_error_msg, "stalled");
    EXPECT_EQ(last_close_reason, ChannelError::ConnectionReset);
}

TEST(RpcChannelFacadeTest, ListenerDispatchForwardsAllOps) {
    auto fake = std::make_shared<FakeListener>();
    auto proxy = make_fake_listener_proxy(fake);

    EXPECT_EQ(proxy->listen("0.0.0.0:7000"), ChannelError::None);
    EXPECT_EQ(proxy->local_address(), "0.0.0.0:7000");
    EXPECT_FALSE(proxy->is_closed());

    fake->set_listen_result(ChannelError::AddressInUse);
    EXPECT_EQ(proxy->listen("0.0.0.0:7000"), ChannelError::AddressInUse);

    proxy->close();
    EXPECT_TRUE(proxy->is_closed());
}

TEST(RpcChannelFacadeTest, ListenerDeliversAcceptedConnection) {
    auto fake_listener = std::make_shared<FakeListener>();
    auto listener_proxy = make_fake_listener_proxy(fake_listener);

    auto fake_conn = std::make_shared<FakeConnection>();
    auto conn_proxy = make_fake_conn_proxy(fake_conn);

    int accepted = 0;
    listener_proxy->set_on_accept([&](ChannelConnectionProxy c) {
        ++accepted;
        EXPECT_TRUE(static_cast<bool>(c));
        c->flush();
    });
    fake_listener->deliver_accept(std::move(conn_proxy));

    EXPECT_EQ(accepted, 1);
    EXPECT_EQ(fake_conn->flush_calls(), 1);
}

TEST(RpcChannelFacadeTest, FactoryProducesConnectionsAndListeners) {
    auto fake_factory = std::make_shared<FakeFactory>();
    auto factory = make_fake_factory_proxy(fake_factory);

    EXPECT_STREQ(factory->backend_name(), "fake");

    auto ok = factory->connect("127.0.0.1:1");
    EXPECT_EQ(ok.error, ChannelError::None);
    EXPECT_TRUE(static_cast<bool>(ok.connection));
    EXPECT_EQ(fake_factory->last_connect_addr(), "127.0.0.1:1");

    fake_factory->set_connect_result(ChannelError::ConnectionRefused);
    auto fail = factory->connect("127.0.0.1:9");
    EXPECT_EQ(fail.error, ChannelError::ConnectionRefused);
    EXPECT_FALSE(static_cast<bool>(fail.connection));

    auto listener = factory->make_listener();
    EXPECT_TRUE(static_cast<bool>(listener));
}

}  // namespace
}  // namespace rrr
