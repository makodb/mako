// Channel-mode reply test for `ServerConnection::reply`.
//
// Exercises the new `bind_channel(...)` / `is_channel_mode()` /
// `reply<F>` channel-mode path. A `CapturingChannelStub` records
// every `send_frame` payload into a vector; the test drives
// `ServerConnection::reply(req, error_code, write_fn)` directly and
// asserts the captured frame body decodes to the expected
// `[xid:v64][error_code:v32][server_instance_id:v64][user-data]`
// wire layout (channel mode always emits the extended-header form;
// the size prefix is owned by the channel layer, not the body).

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/hashmap.hpp>
#include <rusty/hashset.hpp>
#include <rusty/refcell.hpp>
#include <rusty/vec.hpp>

#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

// Captures every send_frame payload into a vector for assertion.
class CapturingChannelStub {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        if (closed_) return ChannelError::ConnectionReset;
        std::vector<std::uint8_t> bytes(f.payload, f.payload + f.size);
        captured_.push_back(std::move(bytes));
        return ChannelError::None;
    }
    void   flush()              {}
    void   close()              { closed_ = true; }
    bool   is_closed() const    { return closed_; }
    std::string peer_address() const { return "stub"; }
    void set_on_frame (OnFrameCallback)  {}
    void set_on_closed(OnClosedCallback) {}
    void set_on_error (OnErrorCallback)  {}

    const std::vector<std::vector<std::uint8_t>>& captured() const { return captured_; }
    std::size_t count() const { return captured_.size(); }

 private:
    std::vector<std::vector<std::uint8_t>> captured_;
    bool closed_ = false;
};

class CapturingChannelStubAdapter : public ChannelConnectionBase {
 public:
    explicit CapturingChannelStubAdapter(std::shared_ptr<CapturingChannelStub> p)
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
    std::shared_ptr<CapturingChannelStub> stub_;
};

inline ChannelConnectionProxy make_capturing_channel_proxy(
        std::shared_ptr<CapturingChannelStub> stub) {
    return std::make_unique<CapturingChannelStubAdapter>(std::move(stub));
}

// Empty service used only to provide a valid `RpcServiceContext`
// (the production constructor needs registered services + an
// instance id).
constexpr uint64_t kFakeServerInstanceId = 0xfeedface00abcdefULL;

inline rusty::Arc<RpcServiceContext> make_test_ctx() {
    rusty::HashMap<i32, size_t> rpc_to_service;
    rusty::HashSet<i32> fast_rpc_ids;
    rusty::Vec<rusty::RefCell<ServiceProxy>> services;
    auto pending = rusty::Arc<std::atomic<int>>::make(0);
    auto drop_heartbeats = rusty::Arc<std::atomic<bool>>::make(false);
    return rusty::Arc<RpcServiceContext>::make(
        std::move(rpc_to_service),
        std::move(fast_rpc_ids),
        std::move(services),
        std::string("0.0.0.0:0"),
        std::move(pending),
        std::move(drop_heartbeats),
        kFakeServerInstanceId);
}

class ServerChannelSendTest : public ::testing::Test {
 protected:
    // rusty::Option<T> swap. See
    // rpc_client_channel_recv_test.cc for the API translation
    // pattern (`emplace` → `= rusty::Some(...)`, `reset` → `=
    // rusty::None`, `(*opt)` → `opt.as_ref().unwrap()`).
    void SetUp() override {
        ctx_ = rusty::Some(make_test_ctx());
        // socket fd = -1: legacy path is unreachable (we never call
        // handle_read / handle_write); we only exercise reply().
        sconn_ = rusty::Some(rusty::Arc<ServerConnection>::make(
            ctx_.as_ref().unwrap().clone(), /*socket=*/-1));
    }

    void TearDown() override {
        sconn_ = rusty::None;
        ctx_ = rusty::None;
    }

    ServerConnection& mut_sconn() {
        return const_cast<ServerConnection&>(
            *sconn_.as_ref().unwrap().get());
    }
    const ServerConnection& sconn() const {
        return *sconn_.as_ref().unwrap().get();
    }

    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_;
    rusty::Option<rusty::Arc<ServerConnection>>  sconn_;
};

// ---------------------------------------------------------------------------
// reply() in channel mode emits one frame whose body parses to
// [xid][error][instance_id][user-data].
// ---------------------------------------------------------------------------

TEST_F(ServerChannelSendTest, ReplyCapturesFrameWithExpectedBody) {
    auto stub = std::make_shared<CapturingChannelStub>();
    mut_sconn().bind_channel(make_capturing_channel_proxy(stub));
    EXPECT_TRUE(sconn().is_channel_mode());

    Request req;
    req.xid = 42;

    const std::string user_payload = "hello";
    sconn().reply(req, /*error_code=*/0, [&](BinaryWriteArchive& out) {
        out << user_payload;
    });

    ASSERT_EQ(stub->count(), 1u);
    const auto& bytes = stub->captured().front();

    Marshal body;
    body.write(bytes.data(), bytes.size());

    v64 v_xid;
    v32 v_err;
    v64 v_instance;
    body >> v_xid >> v_err >> v_instance;
    EXPECT_EQ(static_cast<i64>(v_xid.get()), 42);
    EXPECT_EQ(v_err.get(), 0);
    EXPECT_EQ(static_cast<uint64_t>(v_instance.get()), kFakeServerInstanceId);

    std::string decoded;
    body >> decoded;
    EXPECT_EQ(decoded, user_payload);
}

// ---------------------------------------------------------------------------
// Non-zero error code propagates into the wire-format body.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelSendTest, ReplyPropagatesErrorCode) {
    auto stub = std::make_shared<CapturingChannelStub>();
    mut_sconn().bind_channel(make_capturing_channel_proxy(stub));

    Request req;
    req.xid = 7;
    sconn().reply(req, /*error_code=*/ENOENT, [](BinaryWriteArchive&) {});

    ASSERT_EQ(stub->count(), 1u);
    Marshal body;
    body.write(stub->captured().front().data(),
               stub->captured().front().size());
    v64 v_xid;
    v32 v_err;
    v64 v_instance;
    body >> v_xid >> v_err >> v_instance;
    EXPECT_EQ(static_cast<i64>(v_xid.get()), 7);
    EXPECT_EQ(v_err.get(), ENOENT);
    EXPECT_EQ(static_cast<uint64_t>(v_instance.get()), kFakeServerInstanceId);
}

// ---------------------------------------------------------------------------
// Multiple sequential replies capture in order with distinct xids.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelSendTest, MultipleSequentialRepliesCaptureInOrder) {
    auto stub = std::make_shared<CapturingChannelStub>();
    mut_sconn().bind_channel(make_capturing_channel_proxy(stub));

    for (i64 xid = 1; xid <= 5; ++xid) {
        Request req;
        req.xid = xid;
        sconn().reply(req, 0, [&](BinaryWriteArchive& out) {
            out << static_cast<i64>(xid * 10);
        });
    }
    ASSERT_EQ(stub->count(), 5u);
    for (std::size_t i = 0; i < 5u; ++i) {
        Marshal body;
        body.write(stub->captured()[i].data(),
                   stub->captured()[i].size());
        v64 v_xid;
        v32 v_err;
        v64 v_instance;
        body >> v_xid >> v_err >> v_instance;
        EXPECT_EQ(static_cast<i64>(v_xid.get()), static_cast<i64>(i + 1));
        EXPECT_EQ(v_err.get(), 0);
        i64 user_value;
        body >> user_value;
        EXPECT_EQ(user_value, static_cast<i64>((i + 1) * 10));
    }
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_F(ServerChannelSendTest, ChannelModeStartsFalse) {
    EXPECT_FALSE(sconn().is_channel_mode());
}

TEST_F(ServerChannelSendTest, BindChannelWithNullProxyIsNoop) {
    EXPECT_FALSE(sconn().is_channel_mode());
    mut_sconn().bind_channel(ChannelConnectionProxy{});
    EXPECT_FALSE(sconn().is_channel_mode());
}

}  // namespace
}  // namespace rrr
