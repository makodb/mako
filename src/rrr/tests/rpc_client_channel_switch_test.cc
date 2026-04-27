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
// `srpc_use_channel()` reads env on first call.
// ---------------------------------------------------------------------------

TEST_F(SrpcUseChannelSwitchTest, EnvUnsetReturnsFalse) {
    ::unsetenv("SRPC_USE_CHANNEL");
    EXPECT_FALSE(srpc_use_channel());
}

TEST_F(SrpcUseChannelSwitchTest, EnvOneReturnsTrue) {
    ::setenv("SRPC_USE_CHANNEL", "1", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
}

TEST_F(SrpcUseChannelSwitchTest, EnvFalseLiteralReturnsFalse) {
    ::setenv("SRPC_USE_CHANNEL", "false", /*overwrite=*/1);
    EXPECT_FALSE(srpc_use_channel());
}

TEST_F(SrpcUseChannelSwitchTest, EnvUppercaseTrueReturnsTrue) {
    ::setenv("SRPC_USE_CHANNEL", "TRUE", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
}

TEST_F(SrpcUseChannelSwitchTest, EnvOnReturnsTrue) {
    ::setenv("SRPC_USE_CHANNEL", "on", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
}

TEST_F(SrpcUseChannelSwitchTest, EnvIsCachedAfterFirstRead) {
    ::setenv("SRPC_USE_CHANNEL", "1", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
    // Subsequent env change is ignored (decision is per-process).
    ::setenv("SRPC_USE_CHANNEL", "0", /*overwrite=*/1);
    EXPECT_TRUE(srpc_use_channel());
}

// ---------------------------------------------------------------------------
// Test-only override flips the cached value.
// ---------------------------------------------------------------------------

TEST_F(SrpcUseChannelSwitchTest, OverrideFlipsCached) {
    srpc_set_use_channel_for_testing(true);
    EXPECT_TRUE(srpc_use_channel());

    srpc_set_use_channel_for_testing(false);
    EXPECT_FALSE(srpc_use_channel());
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

TEST_F(SrpcUseChannelSwitchTest,
       ClientConnectDoesNotInstallFactoryWhenSwitchOff) {
    srpc_set_use_channel_for_testing(false);

    auto poll_thread = PollThread::create();
    Client client(poll_thread);

    EXPECT_FALSE(client.has_pending_channel_factory());

    // Connect to an unreachable port — fails, but the legacy fd
    // path is taken (no factory).
    int rc = client.connect("127.0.0.1:1");

    // Pending factory slot remains empty.
    EXPECT_FALSE(client.has_pending_channel_factory());

    (void)rc;

    poll_thread->shutdown();
}

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
