// Channel-mode close fan-out test for `ClientConnection`
//.
//
// Verifies that when the bound `ChannelConnectionProxy` reports
// closed (`on_closed` callback fires through `FiberChannel`), the
// recv-loop fiber drives the close-side reliability fan-out:
//   - error callback is invoked with ECONNRESET
//   - state transitions to FAILED
//   - all pending futures are canceled with ENOTCONN
//   - disconnected callback fires
//   - auto-reconnect is *attempted* when policy allows (observable
//     via `channel_reconnect_attempts_count()`); the spawn is
//     short-circuited inside its body when `reconnect_address_` is
//     empty so the test stays unit-scope.
//
// Test surface mirrors the 4c2 recv-test fixture: single-threaded
// `RecvDriverChannelStub` driven from the test thread; reactor
// pumped manually so the recv-loop fiber observes the synthetic
// close.

#include <stdlib.h>

#include <gtest/gtest.h>


#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../rrr.hpp"

import std;

namespace rrr {
namespace {

// ---------------------------------------------------------------------------
// CloseDriverChannelStub — captures outbound, drives on_closed.
// ---------------------------------------------------------------------------

class CloseDriverChannelStub {
 public:
    ChannelError send_frame(const ChannelFrame& f) {
        std::lock_guard<std::mutex> g(mu_);
        sent_.emplace_back(f.payload, f.payload + f.size);
        return ChannelError::None;
    }
    void   flush()                     {}
    void   close()                     { closed_ = true; }
    bool   is_closed() const           { return closed_; }
    std::string peer_address() const   { return "close-driver"; }
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

class CloseDriverChannelStubAdapter : public ChannelConnectionBase {
 public:
    explicit CloseDriverChannelStubAdapter(std::shared_ptr<CloseDriverChannelStub> p)
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
    std::shared_ptr<CloseDriverChannelStub> stub_;
};

inline ChannelConnectionProxy make_close_driver_proxy(
    std::shared_ptr<CloseDriverChannelStub> p) {
    return std::make_unique<CloseDriverChannelStubAdapter>(std::move(p));
}

// ---------------------------------------------------------------------------
// Reactor pumping helper — drives the recv-loop fiber forward.
// ---------------------------------------------------------------------------

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

class ClientChannelCloseTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
        conn_ = rusty::Some(rusty::Arc<ClientConnection>::make(poll_thread_.as_ref().unwrap().clone()));
        mut_conn().install_self_weak_for_testing(
            WeakClientConnection{rusty::sync::downgrade(conn_.as_ref().unwrap())});

        // Install a fresh CallbackManager so the test can observe
        // error / disconnected callbacks. The constructor's default
        // would also work, but sharing an Arc lets the test (a)
        // register before bind_channel and (b) read the registered
        // callbacks back if needed.
        callback_manager_ = rusty::Some(rusty::Arc<CallbackManager>::make());
        mut_conn().set_callback_manager(callback_manager_.as_ref().unwrap());

        callback_manager_.as_ref().unwrap()->add_on_error([this](RpcError e, const std::string&) {
            error_callbacks_.fetch_add(1, std::memory_order_acq_rel);
            last_error_ = e;
        });
        callback_manager_.as_ref().unwrap()->add_on_disconnected([this]() {
            disconnected_callbacks_.fetch_add(1, std::memory_order_acq_rel);
        });

        stub_ = std::make_shared<CloseDriverChannelStub>();
        mut_conn().bind_channel(make_close_driver_proxy(stub_));
        // request_via_channel rejects when state != CONNECTED; force it.
        mut_conn().force_connected_for_testing();
    }

    void TearDown() override {
        // If the test didn't already close the channel, close it now
        // so the recv-loop fiber exits.
        if (stub_ && !stub_->is_closed()) {
            stub_->deliver_closed(ChannelError::None);
            (void)Reactor::get_reactor()->loop();
        }
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

    rusty::Option<rusty::Arc<PollThread>>       poll_thread_;
    rusty::Option<rusty::Arc<ClientConnection>> conn_;
    std::shared_ptr<CloseDriverChannelStub>     stub_;
    rusty::Option<rusty::Arc<CallbackManager>>  callback_manager_;

    std::atomic<int>     error_callbacks_{0};
    std::atomic<int>     disconnected_callbacks_{0};
    std::atomic<RpcError> last_error_{RpcError::OK};
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(ClientChannelCloseTest, OnClosedCancelsPendingFuturesWithENOTCONN) {
    // Issue a couple of pending requests — they should all flip to
    // ENOTCONN once the channel reports closed.
    auto fr1 = mut_conn().request(0x10, FutureAttr{}, [](BinaryWriteArchive&) {});
    auto fr2 = mut_conn().request(0x11, FutureAttr{}, [](BinaryWriteArchive&) {});
    ASSERT_TRUE(fr1.is_ok());
    ASSERT_TRUE(fr2.is_ok());
    auto fu1 = fr1.unwrap();
    auto fu2 = fr2.unwrap();

    EXPECT_FALSE(fu1->ready());
    EXPECT_FALSE(fu2->ready());

    // Trigger close — the recv-loop fiber's `recv_frame()` returns
    // None, and `on_channel_closed_fan_out` cancels pending futures.
    stub_->deliver_closed(ChannelError::ConnectionReset);
    pump_until([&]() { return fu1->ready() && fu2->ready(); });

    EXPECT_TRUE(fu1->ready());
    EXPECT_TRUE(fu2->ready());
    EXPECT_EQ(fu1->get_error_code(), ENOTCONN);
    EXPECT_EQ(fu2->get_error_code(), ENOTCONN);
}

TEST_F(ClientChannelCloseTest, OnClosedFiresErrorAndDisconnectedCallbacks) {
    EXPECT_EQ(error_callbacks_.load(), 0);
    EXPECT_EQ(disconnected_callbacks_.load(), 0);

    stub_->deliver_closed(ChannelError::ConnectionReset);
    pump_until([&]() { return disconnected_callbacks_.load() > 0; });

    EXPECT_EQ(error_callbacks_.load(), 1);
    EXPECT_EQ(disconnected_callbacks_.load(), 1);
}

TEST_F(ClientChannelCloseTest, OnClosedTransitionsStateToFAILED) {
    stub_->deliver_closed(ChannelError::ConnectionReset);
    pump_until([&]() {
        return mut_conn().connection_state() == ConnectionState::FAILED;
    });

    EXPECT_EQ(mut_conn().connection_state(), ConnectionState::FAILED);
}

TEST_F(ClientChannelCloseTest, OnClosedDoesNotAttemptReconnectWhenAddressEmpty) {
    // Default `reconnect_address_` is empty — the fan-out's
    // reconnect-policy branch must short-circuit before the spawn.
    EXPECT_EQ(mut_conn().channel_reconnect_attempts_count(), 0u);
    stub_->deliver_closed(ChannelError::ConnectionReset);
    pump_until([&]() {
        return mut_conn().connection_state() == ConnectionState::FAILED;
    });
    EXPECT_EQ(mut_conn().channel_reconnect_attempts_count(), 0u);
}

TEST_F(ClientChannelCloseTest, OnClosedAttemptsReconnectWhenPolicyAllows) {
    // Configure the policy to attempt reconnection AND set a
    // reconnect_address_ that the fan-out will see — but mock the
    // legacy fd reconnect path by aborting it via reconnect_abort_
    // *inside* the spawned thread (we observe the spawn via the
    // counter, then immediately abort to avoid hitting socket(2)).
    ReconnectPolicy policy;
    policy.auto_reconnect = true;
    mut_conn().set_reconnect_policy(policy);

    // Set the abort flag now so the spawned thread short-circuits
    // before `reconnect()` is called. The counter still increments
    // because it's bumped *before* the spawn.
    mut_conn().abort_reconnect();
    // Also seed reconnect_address_ via a Client-internal test-only
    // setter (added below).
    mut_conn().set_reconnect_address_for_testing("fake-addr:0");

    EXPECT_EQ(mut_conn().channel_reconnect_attempts_count(), 0u);

    stub_->deliver_closed(ChannelError::ConnectionReset);
    pump_until([&]() {
        return mut_conn().channel_reconnect_attempts_count() > 0;
    });

    EXPECT_GE(mut_conn().channel_reconnect_attempts_count(), 1u);
}

TEST_F(ClientChannelCloseTest, RequestAfterCloseFailsFastWithENOTCONN) {
    // Default buffering is QUEUE — a request while disconnected
    // would land in `pending_queue_` (returns Ok with a future
    // pending TTL expiry) instead of returning ENOTCONN. This test
    // is about the immediate-rejection path, so pick FAIL_FAST.
    BufferingConfig cfg = BufferingConfig::disabled();
    mut_conn().set_buffering_config(cfg);

    stub_->deliver_closed(ChannelError::ConnectionReset);
    pump_until([&]() {
        return mut_conn().connection_state() == ConnectionState::FAILED;
    });

    auto fr = mut_conn().request(0x55, FutureAttr{}, [](BinaryWriteArchive&) {});
    EXPECT_TRUE(fr.is_err());
    EXPECT_EQ(fr.unwrap_err(), ENOTCONN);
}

}  // namespace
}  // namespace rrr
