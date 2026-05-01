// Channel-mode outbound-send test for `ClientConnection`
// (Workstream K, sub-leaf 4b).
//
// Verifies that when a `ChannelConnectionProxy` has been bound via
// `bind_channel`, outbound frames produced by `ClientConnection`'s
// `request` method are routed through the channel's `send_frame`
// rather than the legacy `out_` Marshal / `update_mode(WRITE)` path.
//
// Strategy: a `CapturingChannelStub` records every `send_frame` call
// (a copy of the body bytes). The test:
//   1. Builds a `ClientConnection` with a `PollThread`.
//   2. Calls `bind_channel` with a proxy over the capturing stub.
//   3. Calls `request(...)` with a small write_fn, verifies the
//      stub recorded one frame containing `[v64 xid][i32 rpc_id]
//      [user-marshaled args]`.
//
// In channel mode, `request` does not consult `state_machine_` —
// channel-bound connection health is owned by the proxy (see the
// `is_closed()` path in `request_via_channel`). The test exploits
// that: the stub's `is_closed()` defaults to false, so the request
// proceeds without the test having to drive the legacy state
// machine. (Sub-leaf 4d / 4e re-introduce reconnect / buffering
// hooks through channel callbacks.)
//
// The test does NOT exercise the legacy fd path; that's what the
// existing `test_rpc` / `test_rpc_extended` suites cover (they
// continue to pass because the `is_channel_mode()` branch is
// strictly opt-in).

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
// CapturingChannelStub — record everything send_frame ever sees.
// ---------------------------------------------------------------------------

class CapturingChannelStub {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        std::lock_guard<std::mutex> g(mu_);
        captured_.emplace_back(f.payload, f.payload + f.size);
        return next_send_result_;
    }
    void   flush()                   {}
    void   close()                   { closed_ = true; }
    bool   is_closed() const         { return closed_; }
    std::string peer_address() const { return "capture-stub"; }
    void set_on_frame (OnFrameCallback)  {}
    void set_on_closed(OnClosedCallback) {}
    void set_on_error (OnErrorCallback)  {}

    // Test helpers.
    std::vector<std::vector<std::uint8_t>> captured() {
        std::lock_guard<std::mutex> g(mu_);
        return captured_;
    }
    std::size_t capture_count() {
        std::lock_guard<std::mutex> g(mu_);
        return captured_.size();
    }
    void set_send_result(ChannelError e) { next_send_result_ = e; }
    void mark_closed_for_test() { closed_ = true; }

 private:
    std::mutex mu_;
    std::vector<std::vector<std::uint8_t>> captured_;
    bool closed_ = false;
    ChannelError next_send_result_ = ChannelError::None;
};

class CapturingChannelStubAdapter {
 public:
    explicit CapturingChannelStubAdapter(std::shared_ptr<CapturingChannelStub> p)
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
    std::shared_ptr<CapturingChannelStub> stub_;
};

inline ChannelConnectionProxy make_capture_proxy(
    std::shared_ptr<CapturingChannelStub> p) {
    return pro::make_proxy<ChannelConnectionFacade,
                           CapturingChannelStubAdapter>(std::move(p));
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ClientChannelSendTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
        conn_ = rusty::Some(rusty::Arc<ClientConnection>::make(poll_thread_.as_ref().unwrap().clone()));
        stub_ = std::make_shared<CapturingChannelStub>();
        mut_conn().bind_channel(make_capture_proxy(stub_));
    }

    void TearDown() override {
        conn_ = rusty::None;
        if (poll_thread_.is_some()) {
            poll_thread_.as_ref().unwrap()->shutdown();
            poll_thread_ = rusty::None;
        }
    }

    ClientConnection& mut_conn() {
        return const_cast<ClientConnection&>(*conn_.as_ref().unwrap().get());
    }
    const ClientConnection& conn() const {
        return *conn_.as_ref().unwrap().get();
    }

    rusty::Option<rusty::Arc<PollThread>>      poll_thread_;
    rusty::Option<rusty::Arc<ClientConnection>> conn_;
    std::shared_ptr<CapturingChannelStub>      stub_;
};

// ---------------------------------------------------------------------------
// request -> capture path
// ---------------------------------------------------------------------------

TEST_F(ClientChannelSendTest, RequestRoutesFrameThroughChannel) {
    EXPECT_TRUE(conn().is_channel_mode());

    constexpr i32 kRpcId = 0x42;
    auto fr = mut_conn().request(kRpcId, FutureAttr{}, [](BinaryWriteArchive& m) {
        m << static_cast<i32>(0xDEADBEEF);
    });
    ASSERT_TRUE(fr.is_ok());

    ASSERT_EQ(stub_->capture_count(), 1u);
    auto frames = stub_->captured();
    ASSERT_EQ(frames.size(), 1u);

    // Decode the captured body: [v64 xid][i32 rpc_id][i32 0xDEADBEEF].
    Marshal m;
    m.write(frames[0].data(), frames[0].size());

    v64 v_xid;
    i32 rpc_id;
    i32 user_arg;
    m >> v_xid >> rpc_id >> user_arg;

    EXPECT_EQ(rpc_id, kRpcId);
    EXPECT_EQ(static_cast<std::uint32_t>(user_arg), 0xDEADBEEFu);
    // xid is opaque — just verify it's present (Marshal would fail
    // on read otherwise, asserted by the >> chain above).
}

TEST_F(ClientChannelSendTest, RequestSurfacesChannelErrorAsEIO) {
    stub_->set_send_result(ChannelError::ConnectionReset);
    auto fr = mut_conn().request(0x11, FutureAttr{}, [](BinaryWriteArchive&) {});
    ASSERT_TRUE(fr.is_err());
    EXPECT_EQ(fr.unwrap_err(), EIO);
    // The frame still went through the stub (send_frame ran before
    // returning the error), but pending_fu was cleaned up.
    EXPECT_EQ(stub_->capture_count(), 1u);
}

TEST_F(ClientChannelSendTest, RequestFailsWithENOTCONNWhenChannelClosed) {
    stub_->mark_closed_for_test();
    auto fr = mut_conn().request(0x22, FutureAttr{}, [](BinaryWriteArchive&) {});
    ASSERT_TRUE(fr.is_err());
    EXPECT_EQ(fr.unwrap_err(), ENOTCONN);
    // No frame should have been sent — early-exit on closed channel.
    EXPECT_EQ(stub_->capture_count(), 0u);
}

// ---------------------------------------------------------------------------
// Multiple requests produce multiple captured frames in order.
// ---------------------------------------------------------------------------

TEST_F(ClientChannelSendTest, MultipleRequestsCaptureInOrder) {
    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i) {
        auto fr = mut_conn().request(0x100 + i, FutureAttr{}, [i](BinaryWriteArchive& m) {
            m << i;
        });
        ASSERT_TRUE(fr.is_ok()) << "iteration " << i;
    }

    ASSERT_EQ(stub_->capture_count(), static_cast<std::size_t>(kCount));
    auto frames = stub_->captured();
    for (int i = 0; i < kCount; ++i) {
        Marshal m;
        m.write(frames[i].data(), frames[i].size());
        v64 v_xid;
        i32 rpc_id;
        i32 user_arg;
        m >> v_xid >> rpc_id >> user_arg;
        EXPECT_EQ(rpc_id, 0x100 + i) << "iteration " << i;
        EXPECT_EQ(user_arg, i)       << "iteration " << i;
    }
}

// ---------------------------------------------------------------------------
// Concurrent dispatch — sub-leaf 4g1a regression guard.
//
// Pre-fix: `fiber_channel_` was a `rusty::RefCell<Option<Box<…>>>`. With many
// user threads concurrently calling `request(...)` (which calls
// `dispatch_frame_via_channel` → `borrow_mut()`), the non-atomic
// `borrow_state` int races and either throws `std::runtime_error` from
// `add_writer` (best case — observable) or silently corrupts state.
//
// Post-fix: `fiber_channel_` is `SpinMutex<Option<Box<…>>>`. The lock
// serialises concurrent dispatchers; every request must succeed and every
// frame must reach the stub.
// ---------------------------------------------------------------------------

TEST_F(ClientChannelSendTest, ConcurrentDispatchIsThreadSafe) {
    constexpr int kThreads             = 100;
    constexpr int kRequestsPerThread   = 10;

    std::atomic<int> ok_count{0};
    std::atomic<int> err_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, t, &ok_count, &err_count]() {
            for (int i = 0; i < kRequestsPerThread; ++i) {
                const i32 rpc_id = (t << 8) | i;
                auto fr = mut_conn().request(rpc_id, FutureAttr{},
                                             [t, i](BinaryWriteArchive& m) {
                                                 m << static_cast<i32>(t);
                                                 m << static_cast<i32>(i);
                                             });
                if (fr.is_ok()) {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    err_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    constexpr int kExpected = kThreads * kRequestsPerThread;
    EXPECT_EQ(ok_count.load(),  kExpected);
    EXPECT_EQ(err_count.load(), 0);
    EXPECT_EQ(stub_->capture_count(), static_cast<std::size_t>(kExpected));
}

}  // namespace
}  // namespace rrr
