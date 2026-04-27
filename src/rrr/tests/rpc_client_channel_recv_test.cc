// Channel-mode response demux test for `ClientConnection`
// (Workstream K, sub-leaf 4c2).
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

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef RR
#pragma push_macro("RR")
#undef RR
#define RRR_TESTS_RESTORE_RR_MACRO 1
#endif
#include <proxy/proxy.h>
#include <proxy/proxy_macros.h>
#ifdef RRR_TESTS_RESTORE_RR_MACRO
#pragma pop_macro("RR")
#undef RRR_TESTS_RESTORE_RR_MACRO
#endif

#include <rusty/arc.hpp>

#include "../rrr.hpp"

namespace rrr {
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
        if (on_frame_) {
            ChannelFrame f{bytes.data(), bytes.size()};
            on_frame_(f);
        }
    }
    void deliver_closed(ChannelError reason = ChannelError::None) {
        closed_ = true;
        if (on_closed_) on_closed_(reason);
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

class RecvDriverChannelStubAdapter {
 public:
    explicit RecvDriverChannelStubAdapter(std::shared_ptr<RecvDriverChannelStub> p)
        : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) { return stub_->send_frame(f); }
    void   flush()                   { stub_->flush(); }
    void   close()                   { stub_->close(); }
    bool   is_closed() const         { return stub_->is_closed(); }
    std::string peer_address() const { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<RecvDriverChannelStub> stub_;
};

inline ChannelConnectionProxy make_recv_driver_proxy(
    std::shared_ptr<RecvDriverChannelStub> p) {
    return pro::make_proxy<ChannelConnectionFacade,
                           RecvDriverChannelStubAdapter>(std::move(p));
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
    Marshal m;
    m << v64(xid);
    m << v32(error_code);
    m << v64(server_instance_id);
    if (!reply_payload.empty()) {
        m.write(reply_payload.data(), reply_payload.size());
    }
    std::vector<std::uint8_t> bytes(m.content_size());
    if (!bytes.empty()) {
        verify(m.read(bytes.data(), bytes.size()) == bytes.size());
    }
    return bytes;
}

// Decode the xid from a captured outbound frame body laid out as
// `[v64 xid][i32 rpc_id][user-marshaled args]`. Returns the xid;
// asserts on failure.
i64 decode_outbound_xid(const std::vector<std::uint8_t>& body) {
    Marshal m;
    m.write(body.data(), body.size());
    v64 v_xid;
    m >> v_xid;
    return v_xid.get();
}

// Pump the test-thread reactor until `pred` returns true, with a
// generous bound to fail loudly if the recv-loop fiber is wedged.
template <typename Pred>
void pump_until(Pred&& pred, int max_iterations = 1000) {
    auto reactor = Reactor::get_reactor();
    for (int i = 0; i < max_iterations; ++i) {
        if (pred()) return;
        reactor->loop();
    }
    FAIL() << "pump_until: predicate never satisfied (recv-loop fiber wedged?)";
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ClientChannelRecvTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_.emplace(PollThread::create());
        conn_.emplace(rusty::Arc<ClientConnection>::make((*poll_thread_).clone()));
        // Production goes through `Client::connect` which wires the
        // weak self-pointer; the test constructs the connection
        // directly so we install it here. Without this, the recv-loop
        // fiber's lambda would fail its `Weak::upgrade` and exit
        // before parking on the FiberChannel.
        mut_conn().install_self_weak_for_testing(
            WeakClientConnection{rusty::sync::downgrade(*conn_)});
        stub_ = std::make_shared<RecvDriverChannelStub>();
        mut_conn().bind_channel(make_recv_driver_proxy(stub_));
    }

    void TearDown() override {
        // Shut the recv-loop fiber down by closing the channel; pump
        // the reactor so the close propagates through the fiber.
        if (stub_) {
            stub_->deliver_closed();
            (void)Reactor::get_reactor()->loop();
        }
        conn_.reset();
        if (poll_thread_) {
            (*poll_thread_)->shutdown();
            poll_thread_.reset();
        }
    }

    ClientConnection& mut_conn() {
        return const_cast<ClientConnection&>(*(*conn_).get());
    }

    std::optional<rusty::Arc<PollThread>>      poll_thread_;
    std::optional<rusty::Arc<ClientConnection>> conn_;
    std::shared_ptr<RecvDriverChannelStub>     stub_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(ClientChannelRecvTest, ResponseFrameResolvesPendingFuture) {
    // Issue a request — captures the xid so we can synthesize a matching
    // response.
    constexpr i32 kRpcId = 0x55;
    auto fr = mut_conn().request(kRpcId, FutureAttr{}, [](Marshal& m) {
        m << static_cast<i32>(0xCAFEBABE);
    });
    ASSERT_TRUE(fr.is_ok());
    auto fu = fr.unwrap();

    ASSERT_EQ(stub_->send_count(), 1u);
    auto frames = stub_->sent();
    const i64 xid = decode_outbound_xid(frames[0]);

    // Synthesize and deliver the response. Reply payload is a single
    // i32 = 0x12345678.
    Marshal payload_marshal;
    payload_marshal << static_cast<i32>(0x12345678);
    std::vector<std::uint8_t> reply_payload(payload_marshal.content_size());
    if (!reply_payload.empty()) {
        verify(payload_marshal.read(reply_payload.data(), reply_payload.size())
                   == reply_payload.size());
    }
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
    *reply_guard >> got;
    EXPECT_EQ(static_cast<std::uint32_t>(got), 0x12345678u);
}

TEST_F(ClientChannelRecvTest, ResponseSurfacesNonZeroErrorCode) {
    auto fr = mut_conn().request(0x66, FutureAttr{}, [](Marshal&) {});
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
        auto fr = mut_conn().request(0x80 + i, FutureAttr{}, [i](Marshal& m) {
            m << i;
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
        Marshal payload_marshal;
        payload_marshal << static_cast<i32>(0xA000 + i);
        std::vector<std::uint8_t> reply_payload(payload_marshal.content_size());
        if (!reply_payload.empty()) {
            verify(payload_marshal.read(reply_payload.data(),
                                        reply_payload.size())
                   == reply_payload.size());
        }
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
        *reply >> got;
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
    for (int i = 0; i < 10; ++i) reactor->loop();

    // No crash, no future to verify — just make sure the recv-loop is
    // still parked and ready for more (we exercise the "drain payload"
    // branch in `decode_response_and_notify`).
    SUCCEED();
}

TEST_F(ClientChannelRecvTest, RecvLoopExitsCleanlyOnChannelClose) {
    // Close the channel before any frames are delivered — the parked
    // recv-loop fiber should wake and exit, leaving the connection in
    // a stable state. We verify "no crash" plus the channel-mode
    // latch is still set.
    stub_->deliver_closed();
    auto reactor = Reactor::get_reactor();
    for (int i = 0; i < 10; ++i) reactor->loop();

    EXPECT_TRUE(mut_conn().is_channel_mode());
    // A subsequent request fails with ENOTCONN because the
    // FiberChannel reports closed.
    auto fr = mut_conn().request(0x99, FutureAttr{}, [](Marshal&) {});
    EXPECT_TRUE(fr.is_err());
    EXPECT_EQ(fr.unwrap_err(), ENOTCONN);
}

}  // namespace
}  // namespace rrr
