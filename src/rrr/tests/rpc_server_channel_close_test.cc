// Channel-mode close/error fan-out test for `ServerConnection`
//.
//
// Verifies that when a `ChannelConnectionProxy` fires `on_closed`
// or `on_error`, the bound `ServerConnection` transitions to the
// CLOSED state via the existing `close()` path. Mirrors the client-
// side leaf 4d's `on_channel_closed_fan_out` test in spirit, minus
// the reconnect machinery (server-side has no reconnect — the peer
// reconnects via a fresh `accept()`).

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

// Stub that exposes the installed callbacks so tests can fire them
// directly (mirrors the 5c test's StubChannel + delivery helpers).
class StubChannel {
 public:
    ChannelError send_frame(const ChannelFrame&) {
        if (closed_) return ChannelError::ConnectionReset;
        return ChannelError::None;
    }
    void   flush()              {}
    void   close()              { closed_ = true; }
    bool   is_closed() const    { return closed_; }
    std::string peer_address() const { return "stub"; }
    void set_on_frame (OnFrameCallback  cb) { on_frame_  = std::move(cb); }
    void set_on_closed(OnClosedCallback cb) { on_closed_ = std::move(cb); }
    void set_on_error (OnErrorCallback  cb) { on_error_  = std::move(cb); }

    void deliver_closed(ChannelError reason) {
        if (on_closed_) on_closed_(reason);
    }
    void deliver_error(ChannelError err, std::string_view msg) {
        if (on_error_) on_error_(err, msg);
    }

 private:
    OnFrameCallback  on_frame_;
    OnClosedCallback on_closed_;
    OnErrorCallback  on_error_;
    bool closed_ = false;
};

class StubChannelAdapter : public ChannelConnectionBase {
 public:
    explicit StubChannelAdapter(std::shared_ptr<StubChannel> p)
        : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) override { return stub_->send_frame(f); }
    void   flush() override              { stub_->flush(); }
    void   close() override              { stub_->close(); }
    bool   is_closed() const override    { return stub_->is_closed(); }
    std::string peer_address() const override { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) override { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<StubChannel> stub_;
};

inline ChannelConnectionProxy make_stub_proxy(
        std::shared_ptr<StubChannel> stub) {
    return std::make_unique<StubChannelAdapter>(std::move(stub));
}

constexpr uint64_t kFakeServerInstanceId = 0xfeedface00abcdefULL;

inline rusty::Arc<RpcServiceContext> make_test_ctx() {
    rusty::HashMap<i32, std::size_t> rpc_to_service;
    rusty::HashSet<i32> fast_rpc_ids;
    rusty::Vec<rusty::RefCell<ServiceProxy>> services;
    auto pending = rusty::Arc<std::atomic<int>>::make(0);
    auto drop = rusty::Arc<std::atomic<bool>>::make(false);
    return rusty::Arc<RpcServiceContext>::make(
        std::move(rpc_to_service),
        std::move(fast_rpc_ids),
        std::move(services),
        std::string("0.0.0.0:0"),
        std::move(pending),
        std::move(drop),
        kFakeServerInstanceId);
}

class ServerChannelCloseTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ctx_ = rusty::Some(make_test_ctx());
        sconn_ = rusty::Some(rusty::Arc<ServerConnection>::make(
            ctx_.as_ref().unwrap().clone(), /*socket=*/-1));
        const_cast<ServerConnection&>(*sconn_.as_ref().unwrap().get())
            .install_self_weak_for_testing(rusty::sync::downgrade(sconn_.as_ref().unwrap()));
    }

    void TearDown() override {
        sconn_ = rusty::None;
        ctx_ = rusty::None;
    }

    ServerConnection& mut_sconn() {
        return const_cast<ServerConnection&>(*sconn_.as_ref().unwrap().get());
    }
    const ServerConnection& sconn() const {
        return *sconn_.as_ref().unwrap().get();
    }

    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_;
    rusty::Option<rusty::Arc<ServerConnection>>  sconn_;
};

// ---------------------------------------------------------------------------
// on_closed callback transitions the connection to CLOSED.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelCloseTest, OnClosedTransitionsToClosed) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    EXPECT_FALSE(sconn().is_closed());
    EXPECT_TRUE(mut_sconn().connected());

    stub->deliver_closed(ChannelError::None);

    EXPECT_TRUE(sconn().is_closed());
    EXPECT_FALSE(mut_sconn().connected());
}

// ---------------------------------------------------------------------------
// on_error callback transitions the connection to CLOSED.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelCloseTest, OnErrorTransitionsToClosed) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    EXPECT_FALSE(sconn().is_closed());

    stub->deliver_error(ChannelError::ConnectionReset, "peer reset");

    EXPECT_TRUE(sconn().is_closed());
}

// ---------------------------------------------------------------------------
// Multiple on_closed callbacks are idempotent (close() is itself
// idempotent: status_ == CLOSED short-circuits).
// ---------------------------------------------------------------------------

TEST_F(ServerChannelCloseTest, OnClosedIsIdempotent) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    stub->deliver_closed(ChannelError::None);
    EXPECT_TRUE(sconn().is_closed());
    // Second invocation is a no-op (no crash, state stays CLOSED).
    stub->deliver_closed(ChannelError::ConnectionReset);
    EXPECT_TRUE(sconn().is_closed());
}

// ---------------------------------------------------------------------------
// on_closed after the ServerConnection is destroyed is a no-op
// (the captured Weak<ServerConnection> upgrade fails and the
// callback short-circuits).
// ---------------------------------------------------------------------------

TEST_F(ServerChannelCloseTest, OnClosedAfterDestroyIsNoop) {
    auto stub = std::make_shared<StubChannel>();
    mut_sconn().bind_channel(make_stub_proxy(stub));

    // Drop the ServerConnection; the stub's installed callback's
    // weak should now upgrade to None.
    sconn_ = rusty::None;

    // Should not crash, even though the server connection no longer
    // exists. We can't observe state directly anymore, but the
    // absence of a crash is the assertion.
    stub->deliver_closed(ChannelError::None);
    SUCCEED();
}

}  // namespace
}  // namespace rrr
