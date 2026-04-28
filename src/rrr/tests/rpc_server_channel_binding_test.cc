// Scaffolding guard test for `Server::set_channel_factory` /
// `is_channel_factory_bound` (Workstream K, server sub-leaf 5a).
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

#include <rusty/box.hpp>

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

// Tiny fake conforming to ChannelFactoryFacade. We only need a
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

class NullFactoryStubAdapter {
 public:
    explicit NullFactoryStubAdapter(std::shared_ptr<NullFactoryStub> p)
        : stub_(std::move(p)) {}
    ConnectResult connect(std::string_view addr) { return stub_->connect(addr); }
    ChannelListenerProxy make_listener() { return stub_->make_listener(); }
    const char* backend_name() const { return stub_->backend_name(); }

 private:
    std::shared_ptr<NullFactoryStub> stub_;
};

inline ChannelFactoryProxy make_stub_factory_proxy() {
    return pro::make_proxy<ChannelFactoryFacade,
                           NullFactoryStubAdapter>(
        std::make_shared<NullFactoryStub>());
}

class ServerChannelBindingTest : public ::testing::Test {
 protected:
    void SetUp() override {
        poll_thread_.emplace(PollThread::create());
        server_ = rusty::make_box<Server>(rusty::Some((*poll_thread_).clone()));
    }

    void TearDown() override {
        server_.reset();
        if (poll_thread_) {
            (*poll_thread_)->shutdown();
            poll_thread_.reset();
        }
    }

    Server& server() { return **server_; }

    std::optional<rusty::Arc<PollThread>>     poll_thread_;
    std::optional<rusty::Box<Server>>         server_;
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
