// FiberChannel guard test.
//
// Verifies that the fiber-blocking wrapper around a
// `ChannelConnectionProxy` correctly:
//   1. Returns enqueued frames in arrival order.
//   2. Suspends the calling fiber until a frame arrives.
//   3. Wakes the parked recv fiber when `on_closed` fires.
//   4. Returns `None` after close + drain.
//   5. Forwards `send_frame` to the underlying proxy.
//
// Strategy: a `FakeChannelStub` exposes the channel-facade methods
// and lets the test drive `on_frame` / `on_closed` callbacks
// directly. The recv fiber is created on the reactor thread via
// `Reactor::get_reactor()->create_run_fiber(...)`.
//
// Because the FiberChannel and the recv fiber both run on the
// reactor thread, the test drives callbacks on the same thread —
// matching the production path where TcpConnection's `handle_read`
// fires `on_frame` synchronously on the poll thread.

#include <stdlib.h>

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

// ---------------------------------------------------------------------------
// FakeChannelStub — lets the test drive on_frame / on_closed callbacks.
// ---------------------------------------------------------------------------

class FakeChannelStub {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        sent_.emplace_back(f.payload, f.payload + f.size);
        return next_send_result_;
    }
    void   flush()                   {}
    void   close()                   { closed_ = true; }
    bool   is_closed() const         { return closed_; }
    std::string peer_address() const { return "fake"; }
    void set_on_frame (OnFrameCallback  cb) { on_frame_  = std::move(cb); }
    void set_on_closed(OnClosedCallback cb) { on_closed_ = std::move(cb); }
    void set_on_error (OnErrorCallback  cb) { on_error_  = std::move(cb); }

    // Test helpers — synchronous; both this and the recv fiber run
    // on the reactor thread.
    void deliver(const std::vector<std::uint8_t>& bytes) {
        if (on_frame_) {
            ChannelFrame f{bytes.data(), bytes.size()};
            on_frame_(f);
        }
    }
    void deliver_closed(ChannelError reason = ChannelError::None) {
        if (on_closed_) on_closed_(reason);
    }
    void deliver_error(ChannelError e, std::string_view m) {
        if (on_error_) on_error_(e, m);
    }
    void set_send_result(ChannelError e) { next_send_result_ = e; }
    const std::vector<std::vector<std::uint8_t>>& sent() const { return sent_; }

 private:
    OnFrameCallback  on_frame_;
    OnClosedCallback on_closed_;
    OnErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> sent_;
    bool closed_ = false;
    ChannelError next_send_result_ = ChannelError::None;
};

class FakeChannelStubAdapter : public ChannelConnectionBase {
 public:
    explicit FakeChannelStubAdapter(std::shared_ptr<FakeChannelStub> p)
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
    std::shared_ptr<FakeChannelStub> stub_;
};

inline ChannelConnectionProxy make_fake_proxy(
    std::shared_ptr<FakeChannelStub> p) {
    return rusty::make_box<FakeChannelStubAdapter>(std::move(p));
}

// ---------------------------------------------------------------------------
// Helper: run a body on the reactor thread inside a fiber, return when done.
// ---------------------------------------------------------------------------

template <typename F>
void run_in_fiber(F&& body) {
    auto reactor = Reactor::get_reactor();
    auto done = create_sp_int_event(1);
    reactor->create_run_fiber([done, body = std::forward<F>(body)]() mutable {
        body();
        done->set(1);
    });
    // The outer caller is also on the reactor thread (test runs
    // entirely on a single thread), so we wait synchronously by
    // pumping the reactor until the event fires. The reactor's
    // `loop()` runs ready fibers; once the body's fiber completes,
    // `done` is set and we exit.
    while (done->value_.get() < 1) {
        reactor->run_loop(false, true);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(FiberChannelTest, SendFrameForwardsToProxy) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    const std::uint8_t bytes[] = {0x01, 0x02, 0x03};
    EXPECT_EQ(fc.send_frame(ChannelFrame{bytes, sizeof(bytes)}),
              ChannelError::None);

    ASSERT_EQ(stub->sent().size(), 1u);
    ASSERT_EQ(stub->sent()[0].size(), sizeof(bytes));
    EXPECT_EQ(0, std::memcmp(stub->sent()[0].data(), bytes, sizeof(bytes)));
}

TEST(FiberChannelTest, SendFramePropagatesError) {
    auto stub = std::make_shared<FakeChannelStub>();
    stub->set_send_result(ChannelError::ConnectionReset);
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    const std::uint8_t b = 0xAB;
    EXPECT_EQ(fc.send_frame(ChannelFrame{&b, 1}),
              ChannelError::ConnectionReset);
}

TEST(FiberChannelTest, RecvFrameReturnsEnqueuedFrameImmediately) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    // Deliver a frame BEFORE the fiber waits — recv should return
    // it without suspending.
    const std::vector<std::uint8_t> payload{0xDE, 0xAD};
    stub->deliver(payload);

    bool got = false;
    run_in_fiber([&]() {
        auto opt = fc.recv_frame();
        ASSERT_TRUE(opt.is_some());
        auto frame = std::move(opt).unwrap();
        // frame.bytes is rusty::Vec<uint8_t> (the rustc port); compare to the
        // std::vector payload by size + bytes.
        EXPECT_EQ(frame.bytes.size(), payload.size());
        EXPECT_EQ(0, std::memcmp(frame.bytes.data(), payload.data(), payload.size()));
        got = true;
    });
    EXPECT_TRUE(got);
}

TEST(FiberChannelTest, RecvFrameSuspendsThenWakesOnDelivery) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    int frames_received = 0;
    std::vector<std::uint8_t> last_frame;

    run_in_fiber([&]() {
        auto reactor = Reactor::get_reactor();
        auto recv_done = create_sp_int_event(1);

        // Spawn the recv fiber. `create_run_fiber` runs the fiber
        // synchronously up to its first yield (the wait() inside
        // recv_frame), then returns control here.
        reactor->create_run_fiber([&, recv_done]() {
            auto opt = fc.recv_frame();
            if (opt.is_some()) {
                ++frames_received;
                // OwnedFrame::bytes is rusty::Vec<uint8_t> (the rustc port);
                // copy it into the std::vector capture.
                auto owned = std::move(opt).unwrap();
                last_frame.assign(owned.bytes.data(),
                                  owned.bytes.data() + owned.bytes.size());
            }
            recv_done->set(1);
        });

        // Recv fiber is now parked. Deliver a frame — the on_frame
        // callback fires synchronously and signals the recv fiber's
        // IntEvent.
        const std::vector<std::uint8_t> payload{0x11, 0x22, 0x33};
        stub->deliver(payload);

        // Yield so the reactor schedules the woken recv fiber.
        recv_done->wait();
    });

    EXPECT_EQ(frames_received, 1);
    ASSERT_EQ(last_frame.size(), 3u);
    EXPECT_EQ(last_frame[0], 0x11);
    EXPECT_EQ(last_frame[2], 0x33);
}

TEST(FiberChannelTest, RecvFrameReturnsNoneAfterClose) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    // Pre-close: the fiber should observe None on first recv.
    stub->deliver_closed(ChannelError::None);

    bool got_none = false;
    run_in_fiber([&]() {
        auto opt = fc.recv_frame();
        got_none = opt.is_none();
    });
    EXPECT_TRUE(got_none);
    EXPECT_TRUE(fc.is_closed());
}

TEST(FiberChannelTest, RecvDrainsQueuedFramesBeforeReturningNone) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    // Pre-deliver three frames, then close. Recv should return all
    // three before returning None.
    stub->deliver({0xA0});
    stub->deliver({0xA1, 0xA2});
    stub->deliver({0xA3, 0xA4, 0xA5});
    stub->deliver_closed(ChannelError::None);

    std::vector<std::vector<std::uint8_t>> got;
    bool saw_none = false;
    run_in_fiber([&]() {
        for (int i = 0; i < 4; ++i) {
            auto opt = fc.recv_frame();
            if (opt.is_some()) {
                // OwnedFrame::bytes is rusty::Vec<uint8_t>; copy into std::vector.
                auto owned = std::move(opt).unwrap();
                got.push_back(std::vector<std::uint8_t>(
                    owned.bytes.data(), owned.bytes.data() + owned.bytes.size()));
            } else {
                saw_none = true;
                break;
            }
        }
    });

    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].size(), 1u);
    EXPECT_EQ(got[1].size(), 2u);
    EXPECT_EQ(got[2].size(), 3u);
    EXPECT_EQ(got[2][2], 0xA5);
    EXPECT_TRUE(saw_none);
}

TEST(FiberChannelTest, ParkedRecvWakesOnClose) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    bool got_none = false;
    run_in_fiber([&]() {
        auto reactor = Reactor::get_reactor();
        auto done = create_sp_int_event(1);

        // Spawn recv fiber — it parks on IntEvent inside recv_frame.
        reactor->create_run_fiber([&, done]() {
            auto opt = fc.recv_frame();
            got_none = opt.is_none();
            done->set(1);
        });

        // Recv fiber is parked. Deliver close — the on_closed
        // callback flips `closed_` and signals the recv fiber's
        // IntEvent.
        stub->deliver_closed(ChannelError::None);

        done->wait();
    });

    EXPECT_TRUE(got_none);
    EXPECT_TRUE(fc.is_closed());
}

TEST(FiberChannelTest, MultipleSequentialFramesCaptureInOrder) {
    auto stub = std::make_shared<FakeChannelStub>();
    FiberChannel fc(make_fake_proxy(stub));
    fc.bind_callbacks();  // install on_frame/on_closed/on_error (required post-ctor)

    constexpr int kCount = 10;
    std::vector<std::uint8_t> got_first_bytes;

    run_in_fiber([&]() {
        for (int i = 0; i < kCount; ++i) {
            stub->deliver({static_cast<std::uint8_t>(i)});
        }
        for (int i = 0; i < kCount; ++i) {
            auto opt = fc.recv_frame();
            ASSERT_TRUE(opt.is_some()) << "iteration " << i;
            auto frame = std::move(opt).unwrap();
            ASSERT_EQ(frame.bytes.size(), 1u);
            got_first_bytes.push_back(frame.bytes[0]);
        }
    });

    ASSERT_EQ(got_first_bytes.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(got_first_bytes[i], static_cast<std::uint8_t>(i));
    }
}

}  // namespace
}  // namespace rrr
