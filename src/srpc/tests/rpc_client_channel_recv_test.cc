// Channel-mode response demux test for `ClientConnection`
//.
//
// Verifies that when a `ChannelConnectionProxy` has been bound via
// `bind_channel`, inbound response frames delivered through the
// proxy's `on_frame` callback are decoded by the recv-loop fiber and
// resolve the matching pending future.
//
// Strategy: a `RecvDriverChannelStub` lets the test drive `on_frame`
// / `on_closed` directly. The test is single-threaded — `bind_channel`,
// `request(...)`, frame delivery, and reactor pumping all happen on
// the same thread. This matches the channel-layer threading contract
// (callbacks fire on the reactor that owns the recv-loop fiber).
//
// Each test:
//   1. Builds a `ClientConnection` with a `PollThread` (for legacy
//      construction) and binds a `RecvDriverChannelStub`.
//   2. Issues `request(...)` to register a pending future and grab
//      its xid (via the captured outbound frame).
//   3. Synthesizes a response frame (see `make_response_body`) with
//      the same xid and pushes it through `stub_->deliver(...)`.
//   4. Pumps the reactor until the recv-loop fiber decodes the
//      response and resolves the future.
//   5. Asserts the future's error_code / reply payload match.

#include <stdlib.h>

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/sync/weak.hpp>  // rusty::sync::downgrade
#include <rusty/box.hpp>

#include "../srpc.hpp"

import std;
import rusty;

namespace srpc {
namespace {

// ---------------------------------------------------------------------------
// RecvDriverChannelStub — captures outbound frames AND drives inbound.
// ---------------------------------------------------------------------------

class RecvDriverChannelStub {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        std::lock_guard<std::mutex> g(mu_);
        sent_.emplace_back(f.payload, f.payload + f.size);
        return next_send_result_;
    }
    void   flush()                                   {}
    void   close()                                   { closed_ = true; }
    bool   is_closed() const                         { return closed_; }
    std::string peer_address() const                 { return "recv-driver"; }
    void set_on_frame (OnFrameCallback  cb) { on_frame_  = std::move(cb); }
    void set_on_closed(OnClosedCallback cb) { on_closed_ = std::move(cb); }
    void set_on_error (OnErrorCallback  cb) { on_error_  = std::move(cb); }

    // Test helpers — synchronous; both this and the recv fiber run
    // on the test thread.
    void deliver(const std::vector<std::uint8_t>& bytes) {
        if (on_frame_.has_value()) {
            ChannelFrame f{bytes.data(), bytes.size()};
            on_frame_.callable()(f);
        }
    }
    void deliver_closed(ChannelError reason = ChannelError::None) {
        closed_ = true;
        if (on_closed_.has_value()) on_closed_.callable()(reason);
    }

    std::vector<std::vector<std::uint8_t>> sent() {
        std::lock_guard<std::mutex> g(mu_);
        return sent_;
    }
    std::size_t send_count() {
        std::lock_guard<std::mutex> g(mu_);
        return sent_.size();
    }
    void set_send_result(ChannelError e) { next_send_result_ = e; }

 private:
    std::mutex mu_;
    std::vector<std::vector<std::uint8_t>> sent_;
    OnFrameCallback  on_frame_;
    OnClosedCallback on_closed_;
    OnErrorCallback  on_error_;
    bool closed_ = false;
    ChannelError next_send_result_ = ChannelError::None;
};

class RecvDriverChannelStubAdapter : public ChannelConnectionBase {
 public:
    explicit RecvDriverChannelStubAdapter(std::shared_ptr<RecvDriverChannelStub> p)
        : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) override { return stub_->send_frame(f); }
    void   flush() override                   { stub_->flush(); }
    void   close() override                   { stub_->close(); }
    bool   is_closed() const override         { return stub_->is_closed(); }
    std::string peer_address() const override { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) override { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) override { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) override { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<RecvDriverChannelStub> stub_;
};

inline ChannelConnectionProxy make_recv_driver_proxy(
    std::shared_ptr<RecvDriverChannelStub> p) {
    return rusty::make_box<RecvDriverChannelStubAdapter>(std::move(p));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Synthesize a server-style response body matching what
// `ClientConnection::decode_response_and_notify` expects:
//
//     [v64 reply_xid][v32 error_code][v64 server_instance_id][reply payload]
//
// The 4-byte size prefix is not included — that's added by the
// channel layer before reaching the recv-loop. Returns the wire bytes.
std::vector<std::uint8_t> make_response_body(
    i64 xid,
    i32 error_code,
    i64 server_instance_id,
    const std::vector<std::uint8_t>& reply_payload) {
    srpc::BufferSink sink;
    srpc::BinaryWriteArchive war(srpc::make_sink_proxy_buffer(&sink));
    srpc::Serialize_::serialize(v64(xid), war);
    srpc::Serialize_::serialize(v32(error_code), war);
    srpc::Serialize_::serialize(v64(server_instance_id), war);
    if (!reply_payload.empty()) {
        sink.write_bytes(reply_payload.data(), reply_payload.size());
    }
    return std::vector<std::uint8_t>(
        sink.bytes.data(), sink.bytes.data() + sink.bytes.len());
}

// Decode the xid from a captured outbound frame body laid out as
// `[v64 xid][i32 rpc_id][user-marshaled args]`. Returns the xid;
// asserts on failure.
i64 decode_outbound_xid(const std::vector<std::uint8_t>& body) {
    srpc::BufferSource src(body.data(), body.size());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    v64 v_xid;
    srpc::Deserialize_::deserialize(v_xid, rar);
    return v_xid.get();
}

// Pump the test-thread reactor until `pred` returns true, with a
// generous bound to fail loudly if the recv-loop fiber is wedged.
template <typename Pred>
void pump_until(Pred&& pred, int max_iterations = 1000) {
    auto reactor = Reactor::get_reactor();
    for (int i = 0; i < max_iterations; ++i) {
        if (pred()) return;
        reactor->run_loop(false, true);
    }
    FAIL() << "pump_until: predicate never satisfied (recv-loop fiber wedged?)";
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ClientChannelRecvTest : public ::testing::Test {
 protected:
    void SetUp() override {
        // rusty::Option<T> swap. `(*opt)` no longer
        // returns the inner T (it returns the Option itself);
        // use `opt.as_ref().unwrap()` for borrowed access.
        // `emplace`/`reset` aren't on rusty::Option — assign
        // `rusty::Some(...)` / `rusty::None`.
        poll_thread_ = rusty::Some(PollThread::create());
        conn_ = rusty::Some(rusty::Arc<ClientConnection>::new_(ClientConnection::new_(
            poll_thread_.as_ref().unwrap().clone())));
        // Production goes through `Client::connect` which wires the
        // weak self-pointer; the test constructs the connection
        // directly so we install it here. Without this, the recv-loop
        // fiber's lambda would fail its `Weak::upgrade` and exit
        // before parking on the FiberChannel.
        mut_conn().install_self_weak_for_testing(
            WeakClientConnection{
                rusty::sync::downgrade(conn_.as_ref().unwrap())});
        stub_ = std::make_shared<RecvDriverChannelStub>();
        mut_conn().bind_channel(make_recv_driver_proxy(stub_));
        // request_via_channel rejects when state != CONNECTED; force it.
        mut_conn().force_connected_for_testing();
    }

    void TearDown() override {
        // Shut the recv-loop fiber down by closing the channel; pump
        // the reactor so the close propagates through the fiber.
        if (stub_) {
            stub_->deliver_closed();
            (void)Reactor::get_reactor()->run_loop(false, true);
        }
        conn_ = rusty::None;
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
            poll_thread_ = rusty::None;
        }
    }

    ClientConnection& mut_conn() {
        return const_cast<ClientConnection&>(
            *conn_.as_ref().unwrap().get());
    }

    rusty::Option<rusty::Arc<PollThread>>      poll_thread_;
    rusty::Option<rusty::Arc<ClientConnection>> conn_;
    std::shared_ptr<RecvDriverChannelStub>     stub_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(ClientChannelRecvTest, ResponseFrameResolvesPendingFuture) {
    // Issue a request — captures the xid so we can synthesize a matching
    // response.
    constexpr i32 kRpcId = 0x55;
    auto fr = mut_conn().request(kRpcId, FutureAttr{}, [](BinaryWriteArchive& m) {
        srpc::Serialize_::serialize(static_cast<i32>(0xCAFEBABE), m);
    });
    ASSERT_TRUE(fr.is_ok());
    auto fu = fr.unwrap();

    ASSERT_EQ(stub_->send_count(), 1u);
    auto frames = stub_->sent();
    const i64 xid = decode_outbound_xid(frames[0]);

    // Synthesize and deliver the response. Reply payload is a single
    // i32 = 0x12345678.
    srpc::BufferSink payload_sink;
    srpc::BinaryWriteArchive payload_war(srpc::make_sink_proxy_buffer(&payload_sink));
    srpc::Serialize_::serialize(static_cast<i32>(0x12345678), payload_war);
    std::vector<std::uint8_t> reply_payload(
        payload_sink.bytes.data(),
        payload_sink.bytes.data() + payload_sink.bytes.len());
    auto body = make_response_body(xid,
                                   /*error_code=*/0,
                                   /*server_instance_id=*/42,
                                   reply_payload);
    stub_->deliver(body);

    // Pump the reactor until the recv-loop fiber notifies the future.
    pump_until([&]() { return fu->ready(); });

    EXPECT_TRUE(fu->ready());
    EXPECT_EQ(fu->get_error_code(), 0);

    // Check the reply payload survived.
    auto reply_guard = fu->get_reply();
    i32 got = 0;
    srpc::deserialize_from(std::move(reply_guard), got);
    EXPECT_EQ(static_cast<std::uint32_t>(got), 0x12345678u);
}

TEST_F(ClientChannelRecvTest, ResponseSurfacesNonZeroErrorCode) {
    auto fr = mut_conn().request(0x66, FutureAttr{}, [](BinaryWriteArchive&) {});
    ASSERT_TRUE(fr.is_ok());
    auto fu = fr.unwrap();

    auto frames = stub_->sent();
    ASSERT_EQ(frames.size(), 1u);
    const i64 xid = decode_outbound_xid(frames[0]);

    auto body = make_response_body(xid,
                                   /*error_code=*/EINVAL,
                                   /*server_instance_id=*/7,
                                   /*reply_payload=*/{});
    stub_->deliver(body);

    pump_until([&]() { return fu->ready(); });

    EXPECT_EQ(fu->get_error_code(), EINVAL);
}

TEST_F(ClientChannelRecvTest, MultipleResponsesResolveFuturesInOrder) {
    constexpr int kCount = 4;
    std::vector<rusty::Arc<Future>> futures;
    futures.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        auto fr = mut_conn().request(0x80 + i, FutureAttr{}, [i](BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(i, m);
        });
        ASSERT_TRUE(fr.is_ok());
        futures.push_back(fr.unwrap());
    }

    auto frames = stub_->sent();
    ASSERT_EQ(frames.size(), static_cast<std::size_t>(kCount));

    // Synthesize and deliver responses in order. The deliveries are
    // synchronous — each `deliver(...)` queues a frame, and the
    // recv-loop fiber drains as we pump.
    for (int i = 0; i < kCount; ++i) {
        const i64 xid = decode_outbound_xid(frames[i]);
        srpc::BufferSink payload_sink;
        srpc::BinaryWriteArchive payload_war(srpc::make_sink_proxy_buffer(&payload_sink));
        srpc::Serialize_::serialize(static_cast<i32>(0xA000 + i), payload_war);
        std::vector<std::uint8_t> reply_payload(
            payload_sink.bytes.data(),
            payload_sink.bytes.data() + payload_sink.bytes.len());
        auto body = make_response_body(xid, 0, 1, reply_payload);
        stub_->deliver(body);
    }

    // All futures should resolve.
    pump_until([&]() {
        for (auto& f : futures) {
            if (!f->ready()) return false;
        }
        return true;
    });

    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(futures[i]->ready()) << "future " << i;
        EXPECT_EQ(futures[i]->get_error_code(), 0);
        auto reply = futures[i]->get_reply();
        i32 got = 0;
        srpc::deserialize_from(std::move(reply), got);
        EXPECT_EQ(static_cast<std::uint32_t>(got),
                  static_cast<std::uint32_t>(0xA000 + i)) << "future " << i;
    }
}

TEST_F(ClientChannelRecvTest, ResponseForUnknownXidIsDroppedSilently) {
    // Synthesize a response for an xid the client never registered.
    auto body = make_response_body(/*xid=*/0xDEADC0DE,
                                   /*error_code=*/0,
                                   /*server_instance_id=*/1,
                                   /*reply_payload=*/{0xFE, 0xED});
    stub_->deliver(body);

    // Pump a few iterations so the recv-loop has a chance to drain.
    auto reactor = Reactor::get_reactor();
    for (int i = 0; i < 10; ++i) reactor->run_loop(false, true);

    // No crash, no future to verify — just make sure the recv-loop is
    // still parked and ready for more (we exercise the "drain payload"
    // branch in `decode_response_and_notify`).
    SUCCEED();
}

TEST_F(ClientChannelRecvTest, RecvLoopExitsCleanlyOnChannelClose) {
    // Default buffering is QUEUE — a request while disconnected
    // would land in `pending_queue_` instead of returning ENOTCONN.
    // This test is about the immediate-rejection path, so pick
    // FAIL_FAST.
    BufferingConfig cfg = BufferingConfig::disabled();
    mut_conn().set_buffering_config(cfg);

    // Close the channel before any frames are delivered — the parked
    // recv-loop fiber should wake and exit, leaving the connection in
    // a stable state. We verify "no crash" plus the channel-mode
    // latch is still set.
    stub_->deliver_closed();
    auto reactor = Reactor::get_reactor();
    for (int i = 0; i < 10; ++i) reactor->run_loop(false, true);

    EXPECT_TRUE(mut_conn().is_channel_mode());
    // A subsequent request fails with ENOTCONN because the
    // FiberChannel reports closed.
    auto fr = mut_conn().request(0x99, FutureAttr{}, [](BinaryWriteArchive&) {});
    EXPECT_TRUE(fr.is_err());
    EXPECT_EQ(fr.unwrap_err(), ENOTCONN);
}

}  // namespace
}  // namespace srpc
