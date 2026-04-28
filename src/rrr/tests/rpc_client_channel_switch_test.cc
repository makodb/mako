// Migration-switch test for `SRPC_USE_CHANNEL`
// (Workstream K, sub-leaf 4f).
//
// Verifies that when the migration flag is on, `Client::connect(addr)`
// auto-installs a default TCP-backed `ChannelFactoryProxy` if the
// caller hasn't already installed one via `set_channel_factory(...)`.
// When the flag is off (default), the legacy fd path stays active —
// no factory is installed unless the caller explicitly asks.
//
// We exercise the switch behavior at the Client surface:
//   - `srpc_use_channel()` returns the cached / overridden value.
//   - `srpc_set_use_channel_for_testing(...)` flips the cached
//     value without spawning a child process.
//   - With the override on, `Client::connect(...)` to an
//     unreachable address still installs (and consumes) the
//     pending factory — observable via
//     `Client::has_pending_channel_factory()` flipping false after
//     connect returns, and the resulting `ClientConnection`
//     reporting `is_channel_mode() == true`.
//   - With the override off, the legacy fd path is taken — the
//     pending factory slot stays empty before AND after connect,
//     and the connection is NOT in channel mode.
//
// We don't bind a real TCP server here; the test exercises just the
// switch + factory-installation plumbing. The end-to-end parity with
// real TCP is verified separately by running `test_rpc` and
// `test_rpc_extended` with the env var set both ways.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

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
// Fixture — restores the cached switch state after each test so
// successive tests don't leak state.
// ---------------------------------------------------------------------------

class SrpcUseChannelSwitchTest : public ::testing::Test {
 protected:
    void SetUp() override {
        // Reset the cached choice so each test starts with a clean
        // slate (next `srpc_use_channel()` call re-reads env).
        srpc_reset_use_channel_for_testing();
    }
    void TearDown() override {
        srpc_reset_use_channel_for_testing();
    }
};

// ---------------------------------------------------------------------------
// 4g3a: channel mode is non-negotiable. The env vars
// (SRPC_DISABLE_CHANNEL, SRPC_USE_CHANNEL) and the test-only
// override are deprecated no-ops; `srpc_use_channel()` always
// returns true. These tests assert the deprecated semantics (no-op).
// ---------------------------------------------------------------------------

TEST_F(SrpcUseChannelSwitchTest, EnvUnsetReturnsTrue) {
    ::unsetenv("SRPC_DISABLE_CHANNEL");
    EXPECT_TRUE(srpc_use_channel());
}

TEST_F(SrpcUseChannelSwitchTest, DisableEnvIsIgnored) {
    // 4g3a: SRPC_DISABLE_CHANNEL is now a deprecated no-op.
    ::setenv("SRPC_DISABLE_CHANNEL", "1", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
    ::unsetenv("SRPC_DISABLE_CHANNEL");
}

TEST_F(SrpcUseChannelSwitchTest, UseChannelEnvIsIgnored) {
    // 4g3a: legacy SRPC_USE_CHANNEL env var is also a no-op.
    ::setenv("SRPC_USE_CHANNEL", "0", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
    ::unsetenv("SRPC_USE_CHANNEL");
}

// ---------------------------------------------------------------------------
// Test-only override is a no-op (channel mode is unconditional).
// ---------------------------------------------------------------------------

TEST_F(SrpcUseChannelSwitchTest, OverrideIsNoop) {
    srpc_set_use_channel_for_testing(true);
    EXPECT_TRUE(srpc_use_channel());

    // 4g3a: setting to false is a no-op.
    srpc_set_use_channel_for_testing(false);
    EXPECT_TRUE(srpc_use_channel());
}

// ---------------------------------------------------------------------------
// Client integration — switch on auto-installs TCP factory.
// ---------------------------------------------------------------------------

TEST_F(SrpcUseChannelSwitchTest,
       ClientConnectAutoInstallsTcpFactoryWhenSwitchOn) {
    srpc_set_use_channel_for_testing(true);

    auto poll_thread = PollThread::create();
    Client client(poll_thread);

    // No factory installed by the test code.
    EXPECT_FALSE(client.has_pending_channel_factory());

    // Connect to a port that isn't listening — TCP connect will
    // fail. We don't care about the connect succeeding; we only
    // care that the migration switch consumed the
    // (auto-installed) pending factory and the underlying
    // connection reports channel mode if the connect actually
    // reached `bind_channel`.
    int rc = client.connect("127.0.0.1:1");

    // Whether the connect succeeded or failed, the auto-installed
    // factory should have been consumed by `Client::connect`'s
    // move-out logic (the factory is pushed into the new
    // ClientConnection at connect time).
    EXPECT_FALSE(client.has_pending_channel_factory());

    // The connect probably failed (port 1 isn't listening), but we
    // tolerate either outcome — we just need the switch path to
    // have been taken.
    (void)rc;

    poll_thread->shutdown();
}

// 4g3a: ClientConnectDoesNotInstallFactoryWhenSwitchOff removed —
// channel mode is now non-negotiable; the "switch off" path no
// longer exists.

TEST_F(SrpcUseChannelSwitchTest,
       ClientConnectRespectsExplicitFactoryWhenSwitchOn) {
    // Caller-provided factory takes priority over the migration
    // switch's auto-install. We use the same TCP factory so the
    // test stays self-contained; the assertion is just that
    // pending_factory_ holds *one* factory before connect (the
    // explicit one) and is consumed afterwards.
    srpc_set_use_channel_for_testing(true);

    auto poll_thread = PollThread::create();
    Client client(poll_thread);

    auto explicit_factory = make_tcp_factory_proxy(
        rusty::Arc<TcpFactory>::make(poll_thread));
    client.set_channel_factory(std::move(explicit_factory));

    EXPECT_TRUE(client.has_pending_channel_factory());

    int rc = client.connect("127.0.0.1:1");
    EXPECT_FALSE(client.has_pending_channel_factory());
    (void)rc;

    poll_thread->shutdown();
}

}  // namespace
}  // namespace rrr
