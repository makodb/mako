// Scaffolding guard test for `Server::set_channel_factory` /
// `is_channel_factory_bound`.
//
// Verifies that the new channel-factory binding entry points exist
// with the expected signatures and that the latch flips as
// documented. There is **no behavior change** in this leaf —
// `Server::start()` does not yet read the bound factory; the legacy
// `ServerListener` socket path is still the only active code path.
//
// Subsequent leaves (5b–5g) will wire frame send / recv / close /
// listen routing through the channel; their tests will replace this
// trivial one with end-to-end coverage.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../rrr.hpp"

namespace rrr {
namespace {

// Tiny fake conforming to ChannelFactoryBase. We only need a
// non-null proxy to flip the channel-factory latch; the proxy
// methods don't have to do anything meaningful for this scaffolding
// test.
class NullFactoryStub {
 public:
    ConnectResult connect(std::string_view) {
        return ConnectResult{ChannelConnectionProxy{},
                             ChannelError::Internal};
    }
    ChannelListenerProxy make_listener() {
        return ChannelListenerProxy{};
    }
    const char* backend_name() const { return "null-stub"; }
};

class NullFactoryStubAdapter : public ChannelFactoryBase {
 public:
    explicit NullFactoryStubAdapter(std::shared_ptr<NullFactoryStub> p)
        : stub_(std::move(p)) {}
    ConnectResult connect(std::string_view addr) override { return stub_->connect(addr); }
    ChannelListenerProxy make_listener() override { return stub_->make_listener(); }
    const char* backend_name() const override { return stub_->backend_name(); }

 private:
    std::shared_ptr<NullFactoryStub> stub_;
};

inline ChannelFactoryProxy make_stub_factory_proxy() {
    return rusty::make_box<NullFactoryStubAdapter>(
        std::make_shared<NullFactoryStub>());
}

class ServerChannelBindingTest : public ::testing::Test {
 protected:
    // rusty::Option<T> swap. See
    // rpc_client_channel_recv_test.cc for the API translation.
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

    Server& server() { return *server_.as_ref().unwrap(); }

    rusty::Option<rusty::Arc<PollThread>>     poll_thread_;
    rusty::Option<rusty::Box<Server>>         server_;
};

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_F(ServerChannelBindingTest, FactoryUnboundByDefault) {
    EXPECT_FALSE(server().is_channel_factory_bound());
}

// ---------------------------------------------------------------------------
// set_channel_factory with a null proxy is a no-op.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelBindingTest, SetChannelFactoryWithNullProxyIsNoop) {
    EXPECT_FALSE(server().is_channel_factory_bound());
    server().set_channel_factory(ChannelFactoryProxy{});
    EXPECT_FALSE(server().is_channel_factory_bound());
}

// ---------------------------------------------------------------------------
// set_channel_factory with a non-null proxy flips the latch.
// ---------------------------------------------------------------------------

TEST_F(ServerChannelBindingTest, SetChannelFactoryWithStubFlipsLatch) {
    EXPECT_FALSE(server().is_channel_factory_bound());
    server().set_channel_factory(make_stub_factory_proxy());
    EXPECT_TRUE(server().is_channel_factory_bound());
}

// ---------------------------------------------------------------------------
// Re-binding replaces the previously-bound factory and the latch
// stays true. (No behavior change — there's no observable state
// beyond the latch in this leaf, but rebind must not de-bind.)
// ---------------------------------------------------------------------------

TEST_F(ServerChannelBindingTest, SetChannelFactoryRebindKeepsLatch) {
    server().set_channel_factory(make_stub_factory_proxy());
    EXPECT_TRUE(server().is_channel_factory_bound());
    server().set_channel_factory(make_stub_factory_proxy());
    EXPECT_TRUE(server().is_channel_factory_bound());
}

}  // namespace
}  // namespace rrr
