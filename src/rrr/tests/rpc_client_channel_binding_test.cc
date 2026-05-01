// Scaffolding guard test for `ClientConnection::bind_channel` /
// `is_channel_mode` (Workstream K, sub-leaf 4a).
//
// Verifies that the new channel-binding entry points exist with the
// expected signatures and that the latch flips as documented. There
// is **no behavior change** in this leaf — none of the existing
// I/O methods route through the channel yet — so the test only
// exercises the new accessors.
//
// Subsequent leaves (4b/4c/4d) will add real frame send / recv /
// close routing through the channel; their tests will replace this
// trivial one with end-to-end coverage.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

// Tiny fake conforming to ChannelConnectionFacade. We only need a
// non-null proxy to flip the channel-mode latch; the proxy methods
// don't have to do anything meaningful for this scaffolding test.
class NullChannelStub {
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

class NullChannelStubAdapter {
 public:
    explicit NullChannelStubAdapter(std::shared_ptr<NullChannelStub> p)
        : stub_(std::move(p)) {}
    ChannelError send_frame(const ChannelFrame& f) { return stub_->send_frame(f); }
    void   flush()              { stub_->flush(); }
    void   close()              { stub_->close(); }
    bool   is_closed() const    { return stub_->is_closed(); }
    std::string peer_address() const { return stub_->peer_address(); }
    void set_on_frame (OnFrameCallback  cb) { stub_->set_on_frame (std::move(cb)); }
    void set_on_closed(OnClosedCallback cb) { stub_->set_on_closed(std::move(cb)); }
    void set_on_error (OnErrorCallback  cb) { stub_->set_on_error (std::move(cb)); }
 private:
    std::shared_ptr<NullChannelStub> stub_;
};

inline ChannelConnectionProxy make_stub_channel_proxy() {
    return pro::make_proxy<ChannelConnectionFacade,
                           NullChannelStubAdapter>(
        std::make_shared<NullChannelStub>());
}

class ClientChannelBindingTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
        conn_ = rusty::Some(rusty::Arc<ClientConnection>::make(poll_thread_.as_ref().unwrap().clone()));
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
};

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_F(ClientChannelBindingTest, ChannelModeStartsFalse) {
    EXPECT_FALSE(conn().is_channel_mode());
}

// ---------------------------------------------------------------------------
// bind_channel with a null proxy is a no-op.
// ---------------------------------------------------------------------------

TEST_F(ClientChannelBindingTest, BindChannelWithNullProxyIsNoop) {
    EXPECT_FALSE(conn().is_channel_mode());
    mut_conn().bind_channel(ChannelConnectionProxy{});
    EXPECT_FALSE(conn().is_channel_mode());
}

// ---------------------------------------------------------------------------
// bind_channel with a non-null proxy flips the latch.
// ---------------------------------------------------------------------------

TEST_F(ClientChannelBindingTest, BindChannelWithStubFlipsLatch) {
    EXPECT_FALSE(conn().is_channel_mode());
    mut_conn().bind_channel(make_stub_channel_proxy());
    EXPECT_TRUE(conn().is_channel_mode());
}

}  // namespace
}  // namespace rrr
