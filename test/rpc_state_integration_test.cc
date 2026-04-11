/**
 * Integration tests for ConnectionStateMachine with actual RPC operations.
 * Tests state transitions during connect, disconnect, and error scenarios.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <rusty/arc.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "rpc/connection_state.hpp"
#include "misc/marshal.hpp"
#include "benchmark_service.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Atomic counter for dynamic port allocation
static std::atomic<int> g_state_test_port{11000};

namespace {

int count_open_fds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }

    int count = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ++count;
    }
    ::closedir(dir);

    // /proc/self/fd enumeration includes the directory descriptor itself.
    return count > 0 ? count - 1 : 0;
}

template <typename Predicate>
bool wait_for_condition(Predicate&& predicate, milliseconds timeout) {
    auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return predicate();
}

bool wait_for_fd_close(int fd, milliseconds timeout) {
    auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        errno = 0;
        if (::fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }

    errno = 0;
    return (::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

class ClosedFlagPollable : public Pollable {
public:
    explicit ClosedFlagPollable(int fd)
        : fd_(fd) {}

    int fd() const override {
        return fd_.load();
    }

    int poll_mode() const override {
        return PollMode::READ;
    }

    size_t content_size() override {
        return 0;
    }

    bool handle_read() override {
        return true;
    }

    int handle_write() override {
        return PollMode::NO_CHANGE;
    }

    void handle_error() override {}

    void close() override {
        close_calls_.fetch_add(1);
        int fd = fd_.exchange(-1);
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool check_pending_write_update() const override {
        return false;
    }

    bool is_closed() const override {
        return closed_.load();
    }

    void set_closed(bool closed) const {
        closed_.store(closed);
    }

    int close_calls() const {
        return close_calls_.load();
    }

private:
    std::atomic<int> fd_;
    mutable std::atomic<bool> closed_{false};
    std::atomic<int> close_calls_{0};
};

}  // namespace

// Simple test service for integration tests
class StateTestService : public benchmark::BenchmarkService {
public:
    std::atomic<int> call_count{0};

    void fast_nop(const std::string& input) override {
        call_count++;
    }

    void nop(const std::string& input) override {
        call_count++;
    }

    void fast_prime(const i32& n, i8* flag) override {
        *flag = 1;
    }

    void fast_vec(const i32& n, std::vector<i64>* v) override {
        for (i32 i = 0; i < n; i++) v->push_back(i);
    }

    void sleep(const double& sec) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(sec));
    }
};

// ============================================================================
// Basic State Transition Tests
// ============================================================================

class StateIntegrationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    StateIntegrationTest() : test_port_(g_state_test_port.fetch_add(1)) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }
};

TEST_F(StateIntegrationTest, InitialStateNotConnected) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    EXPECT_FALSE(client->connected());
}

TEST_F(StateIntegrationTest, StateAfterSuccessfulConnect) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    // Connect client
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    EXPECT_FALSE(client->connected());

    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());

    // Cleanup
    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, StateAfterDisconnect) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    // Connect client
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // Disconnect
    client->close();
    std::this_thread::sleep_for(milliseconds(50));

    // After close, client should not be connected
    EXPECT_FALSE(client->connected());

    delete server;
}

TEST_F(StateIntegrationTest, StateAfterConnectionRefused) {
    // Don't start server - port should be closed
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Try to connect to non-existent server
    int result = client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str());
    EXPECT_NE(result, 0);

    // Client should not be connected
    EXPECT_FALSE(client->connected());

    client->close();
}

TEST_F(StateIntegrationTest, StateDuringActiveRequest) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    auto service = service_box.get();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    // Connect client
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Should be connected during request
    EXPECT_TRUE(client->connected());

    // Make request
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(fu_result.is_ok());

    // Still connected during request
    EXPECT_TRUE(client->connected());

    auto fu = fu_result.unwrap();
    fu->wait();

    // Still connected after request
    EXPECT_TRUE(client->connected());
    EXPECT_EQ(service->call_count, 1);

    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, PendingRequestCountTracksInFlightSleepRequest) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    ASSERT_TRUE(client->connected());
    ASSERT_EQ(server->pending_request_count(), 0);

    constexpr double kSleepSeconds = 0.3;
    auto fu_result = client->request(
        benchmark::BenchmarkService::SLEEP,
        [&](Marshal& m) { m << kSleepSeconds; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() {
        return server->pending_request_count() > 0;
    }, milliseconds(600)));
    EXPECT_GT(server->pending_request_count(), 0);

    fu->wait();
    ASSERT_TRUE(wait_for_condition([&]() {
        return server->pending_request_count() == 0;
    }, milliseconds(1000)));

    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, DrainTimeoutReflectsRealInFlightRequest) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    ASSERT_TRUE(client->connected());

    constexpr double kSleepSeconds = 0.4;
    auto fu_result = client->request(
        benchmark::BenchmarkService::SLEEP,
        [&](Marshal& m) { m << kSleepSeconds; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() {
        return server->pending_request_count() > 0;
    }, milliseconds(600)));

    server->stop_accepting();
    auto start = steady_clock::now();
    bool drained = server->drain(50);
    auto elapsed = steady_clock::now() - start;

    EXPECT_FALSE(drained);
    EXPECT_GE(elapsed, milliseconds(40));
    EXPECT_GT(server->pending_request_count(), 0);

    fu->wait();
    ASSERT_TRUE(wait_for_condition([&]() {
        return server->pending_request_count() == 0;
    }, milliseconds(1000)));

    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, GracefulShutdownWaitsForInFlightRequest) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    ASSERT_TRUE(client->connected());

    constexpr double kSleepSeconds = 0.5;
    auto fu_result = client->request(
        benchmark::BenchmarkService::SLEEP,
        [&](Marshal& m) { m << kSleepSeconds; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() {
        return server->pending_request_count() > 0;
    }, milliseconds(600)));

    auto start = steady_clock::now();
    server->graceful_shutdown(1000);
    auto elapsed = steady_clock::now() - start;

    EXPECT_GE(elapsed, milliseconds(200));
    EXPECT_EQ(server->phase(), ShutdownPhase::STOPPED);
    EXPECT_EQ(server->pending_request_count(), 0);

    fu->wait();
    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, HeartbeatTimeoutTriggersReconnectRecovery) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ReconnectPolicy policy;
    policy.auto_reconnect = true;
    policy.max_retries = 200;
    policy.initial_delay_ms = 20;
    policy.max_delay_ms = 20;
    policy.backoff_multiplier = 1.0;
    policy.jitter_enabled = false;
    client->set_reconnect_policy(policy);

    HeartbeatConfig heartbeat;
    heartbeat.enabled = true;
    heartbeat.interval_ms = 20;
    heartbeat.timeout_ms = 20;
    heartbeat.max_missed = 1;
    client->set_heartbeat(heartbeat);

    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    ASSERT_TRUE(client->connected());
    ASSERT_EQ(client->metrics().reconnect_count(), 0u);

    std::string input = "heartbeat_warmup";
    auto warmup = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(warmup.is_ok());
    auto warmup_fu = warmup.unwrap();
    warmup_fu->wait();
    ASSERT_EQ(warmup_fu->get_error_code(), 0);

    // Simulate an unresponsive peer without dropping transport: heartbeat probes
    // are accepted but replies are suppressed.
    server->set_drop_heartbeat_replies(true);
    ASSERT_TRUE(wait_for_condition([&]() {
        return client->metrics().reconnect_count() >= 1u;
    }, milliseconds(4000)));

    // Restore heartbeat replies and verify the connection recovers.
    server->set_drop_heartbeat_replies(false);
    ASSERT_TRUE(wait_for_condition([&]() {
        return client->connected();
    }, milliseconds(4000)));

    auto after = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(after.is_ok());
    auto after_fu = after.unwrap();
    after_fu->wait();
    EXPECT_EQ(after_fu->get_error_code(), 0);

    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, CircuitOpenFailFastThenHalfOpenRecovery) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    ReconnectPolicy reconnect_policy;
    reconnect_policy.auto_reconnect = false;
    client->set_reconnect_policy(reconnect_policy);

    // Disable request buffering so connection failures surface immediately.
    client->set_buffering_config(BufferingConfig::disabled());

    CircuitBreakerConfig cb_config;
    cb_config.enabled = true;
    cb_config.failure_threshold = 1;
    cb_config.success_threshold = 1;
    cb_config.timeout_ms = 400;
    client->set_circuit_breaker(cb_config);
    HeartbeatConfig heartbeat;
    heartbeat.enabled = true;
    heartbeat.interval_ms = 20;
    heartbeat.timeout_ms = 20;
    heartbeat.max_missed = 1;
    client->set_heartbeat(heartbeat);

    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    ASSERT_TRUE(wait_for_condition([&]() { return client->connected(); }, milliseconds(1000)));
    client->set_reconnect_policy(reconnect_policy);
    client->set_buffering_config(BufferingConfig::disabled());

    std::string input = "cb-warmup";
    auto warmup = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(warmup.is_ok());
    auto warmup_fu = warmup.unwrap();
    warmup_fu->wait();
    ASSERT_EQ(warmup_fu->get_error_code(), 0);

    // Drop server to generate transport failures.
    delete server;
    server = nullptr;

    ASSERT_TRUE(wait_for_condition([&]() {
        return !client->connected();
    }, milliseconds(5000)));

    // First disconnected request records a transport failure in the circuit.
    auto first_failure = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(first_failure.is_err());
    ASSERT_EQ(first_failure.unwrap_err(), ENOTCONN);

    // Circuit should now fail fast.
    auto fail_fast = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(fail_fast.is_err());
    ASSERT_EQ(fail_fast.unwrap_err(), EBUSY);

    // Bring server back and reconnect transport.
    server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box2 = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box2));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    std::atomic<bool> reconnect_done{false};
    std::atomic<bool> reconnect_success{false};
    ASSERT_EQ(client->reconnect([&](bool success) {
        reconnect_success.store(success);
        reconnect_done.store(true);
    }), 0);
    ASSERT_TRUE(wait_for_condition([&]() { return reconnect_done.load(); }, milliseconds(4000)));
    ASSERT_TRUE(reconnect_success.load());
    ASSERT_TRUE(client->connected());

    // Still open before timeout expires.
    auto still_open = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(still_open.is_err());
    EXPECT_EQ(still_open.unwrap_err(), EBUSY);

    // After open timeout, one half-open probe should be allowed and succeed.
    std::this_thread::sleep_for(milliseconds(500));

    auto probe = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(probe.is_ok());
    auto probe_fu = probe.unwrap();
    probe_fu->wait();
    ASSERT_EQ(probe_fu->get_error_code(), 0);

    // success_threshold=1 should close the circuit for subsequent traffic.
    auto after = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(after.is_ok());
    auto after_fu = after.unwrap();
    after_fu->wait();
    EXPECT_EQ(after_fu->get_error_code(), 0);

    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, StateAfterServerShutdown) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    // Connect client
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // Shutdown server
    delete server;
    std::this_thread::sleep_for(milliseconds(100));

    // Try to make a request - should fail
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );

    // Either request fails immediately or times out
    if (fu_result.is_ok()) {
        auto fu = fu_result.unwrap();
        fu->timed_wait(0.5);  // Short timeout
    }

    // Give some time for state to update
    std::this_thread::sleep_for(milliseconds(100));

    client->close();
}

TEST_F(StateIntegrationTest, ErrorPathClosesSocketFd) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    ASSERT_TRUE(client->connected());

    const int initial_fd = client->fd();
    ASSERT_GE(initial_fd, 0);
    ASSERT_NE(::fcntl(initial_fd, F_GETFD), -1);

    // Force remote close so the poll thread enters handle_error().
    delete server;

    std::string input = "probe";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    if (fu_result.is_ok()) {
        auto fu = fu_result.unwrap();
        fu->timed_wait(0.2);
    }

    auto disconnect_deadline = steady_clock::now() + milliseconds(1000);
    while (client->connected() && steady_clock::now() < disconnect_deadline) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    EXPECT_FALSE(client->connected());
    EXPECT_TRUE(wait_for_fd_close(initial_fd, milliseconds(1000)));

    client->close();
}

TEST_F(StateIntegrationTest, MarkClosingStaysNonTerminalUntilPollClose) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    ASSERT_TRUE(client->connected());

    auto conn_opt = client->connection();
    ASSERT_TRUE(conn_opt.is_some());
    auto conn = conn_opt.unwrap();
    auto& mut_conn = const_cast<ClientConnection&>(*conn);
    const int fd = conn->fd();
    ASSERT_GE(fd, 0);
    ASSERT_NE(::fcntl(fd, F_GETFD), -1);

    mut_conn.mark_closing();
    EXPECT_EQ(mut_conn.connection_state(), ConnectionState::DISCONNECTING);
    EXPECT_FALSE(mut_conn.is_closed());
    ASSERT_NE(::fcntl(fd, F_GETFD), -1);

    // Complete close through the poll-thread close callback.
    poll_thread_.as_ref().unwrap()->request_close(fd);
    EXPECT_TRUE(wait_for_fd_close(fd, milliseconds(1000)));

    auto state_deadline = steady_clock::now() + milliseconds(1000);
    while (conn->connection_state() != ConnectionState::DISCONNECTED &&
           steady_clock::now() < state_deadline) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    EXPECT_EQ(conn->connection_state(), ConnectionState::DISCONNECTED);

    client->close();
    delete server;
}

TEST_F(StateIntegrationTest, ClosedFdCleanupInvokesCloseCallbackBeforeErase) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    auto pollable = rusty::Arc<ClosedFlagPollable>::make(sv[0]);
    poll_thread_.as_ref().unwrap()->add(pollable);
    std::this_thread::sleep_for(milliseconds(50));

    ASSERT_EQ(pollable->close_calls(), 0);
    ASSERT_NE(::fcntl(sv[0], F_GETFD), -1);

    // Mark closed so poll loop takes the closed-fd cleanup branch.
    pollable->set_closed(true);

    auto close_deadline = steady_clock::now() + milliseconds(1000);
    while (pollable->close_calls() == 0 && steady_clock::now() < close_deadline) {
        std::this_thread::sleep_for(milliseconds(10));
    }

    EXPECT_EQ(pollable->close_calls(), 1);
    EXPECT_TRUE(wait_for_fd_close(sv[0], milliseconds(1000)));

    // Ensure no descriptor leak if callback was not invoked.
    if (::fcntl(sv[0], F_GETFD) != -1) {
        ::close(sv[0]);
    }
    ::close(sv[1]);
}

TEST_F(StateIntegrationTest, RepeatedErrorReconnectCyclesDoNotIncreaseFdCount) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    const std::string server_addr = "127.0.0.1:" + std::to_string(test_port_);
    client->set_heartbeat(HeartbeatConfig(true, 30, 60, 1));

    const int baseline_fd_count = count_open_fds();
    ASSERT_GE(baseline_fd_count, 0);

    constexpr int kCycles = 20;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<StateTestService>();
        server->reg_service(std::move(service_box));
        ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

        if (cycle == 0) {
            ASSERT_EQ(client->connect(server_addr.c_str()), 0);
        } else {
            std::atomic<bool> reconnect_done{false};
            std::atomic<bool> reconnect_success{false};
            ASSERT_EQ(client->reconnect([&](bool success) {
                reconnect_success = success;
                reconnect_done = true;
            }), 0);
            ASSERT_TRUE(wait_for_condition([&]() { return reconnect_done.load(); }, milliseconds(2000)));
            ASSERT_TRUE(reconnect_success.load());
        }

        ASSERT_TRUE(wait_for_condition([&]() { return client->connected(); }, milliseconds(1000)));
        const int cycle_fd = client->fd();
        ASSERT_GE(cycle_fd, 0);
        ASSERT_NE(::fcntl(cycle_fd, F_GETFD), -1);

        std::string input = "cycle-" + std::to_string(cycle);
        auto ok_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        if (ok_result.is_ok()) {
            ok_result.unwrap()->timed_wait(0.2);
        }

        // Force disconnection path and handle_error() driven cleanup.
        delete server;
        server = nullptr;

        auto fail_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        if (fail_result.is_ok()) {
            fail_result.unwrap()->timed_wait(0.2);
        }

        // Drive transport error detection until state flips to disconnected.
        auto disconnect_deadline = steady_clock::now() + milliseconds(4000);
        while (client->connected() && steady_clock::now() < disconnect_deadline) {
            auto probe_result = client->request(
                benchmark::BenchmarkService::FAST_NOP,
                [&](Marshal& m) { m << input; }
            );
            if (probe_result.is_ok()) {
                probe_result.unwrap()->timed_wait(0.05);
            }
            std::this_thread::sleep_for(milliseconds(20));
        }

        ASSERT_FALSE(client->connected());
        EXPECT_TRUE(wait_for_fd_close(cycle_fd, milliseconds(2000)));

        const int cycle_fd_count = count_open_fds();
        ASSERT_GE(cycle_fd_count, 0);
        EXPECT_LE(cycle_fd_count, baseline_fd_count + 2);
    }

    client->close();
    std::this_thread::sleep_for(milliseconds(100));

    const int final_fd_count = count_open_fds();
    ASSERT_GE(final_fd_count, 0);
    EXPECT_LE(final_fd_count, baseline_fd_count + 1);
}

TEST_F(StateIntegrationTest, StressFastConnectCloseCyclesDoNotIncreaseFdCount) {
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    const std::string server_addr = "127.0.0.1:" + std::to_string(test_port_);
    const int baseline_fd_count = count_open_fds();
    ASSERT_GE(baseline_fd_count, 0);

    constexpr int kCycles = 1000;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        auto client = Client::create(poll_thread_.as_ref().unwrap());
        ASSERT_EQ(client->connect(server_addr.c_str()), 0) << "cycle=" << cycle;
        ASSERT_TRUE(wait_for_condition([&]() { return client->connected(); }, milliseconds(1000)))
            << "cycle=" << cycle;

        const int fd = client->fd();
        ASSERT_GE(fd, 0) << "cycle=" << cycle;
        ASSERT_NE(::fcntl(fd, F_GETFD), -1) << "cycle=" << cycle;

        client->close();
        ASSERT_TRUE(wait_for_condition([&]() { return !client->connected(); }, milliseconds(1500)))
            << "cycle=" << cycle;
        ASSERT_TRUE(wait_for_fd_close(fd, milliseconds(1500))) << "cycle=" << cycle;

        if ((cycle + 1) % 100 == 0) {
            const int cycle_fd_count = count_open_fds();
            ASSERT_GE(cycle_fd_count, 0);
            EXPECT_LE(cycle_fd_count, baseline_fd_count + 3) << "cycle=" << cycle;
        }
    }

    std::this_thread::sleep_for(milliseconds(100));
    const int final_fd_count = count_open_fds();
    ASSERT_GE(final_fd_count, 0);
    EXPECT_LE(final_fd_count, baseline_fd_count + 2);

    delete server;
}

TEST_F(StateIntegrationTest, MultipleClientsIndependentState) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    // Create two clients
    auto client1 = Client::create(poll_thread_.as_ref().unwrap());
    auto client2 = Client::create(poll_thread_.as_ref().unwrap());

    // Both start not connected
    EXPECT_FALSE(client1->connected());
    EXPECT_FALSE(client2->connected());

    // Connect client1
    ASSERT_EQ(client1->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // client1 connected, client2 still not connected
    EXPECT_TRUE(client1->connected());
    EXPECT_FALSE(client2->connected());

    // Connect client2
    ASSERT_EQ(client2->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Both connected
    EXPECT_TRUE(client1->connected());
    EXPECT_TRUE(client2->connected());

    // Disconnect client1
    client1->close();
    std::this_thread::sleep_for(milliseconds(50));

    // client1 disconnected, client2 still connected
    EXPECT_FALSE(client1->connected());
    EXPECT_TRUE(client2->connected());

    client2->close();
    delete server;
}

// ============================================================================
// Connection Lifecycle Tests
// ============================================================================

TEST_F(StateIntegrationTest, RapidConnectDisconnectCycles) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    for (int i = 0; i < 5; i++) {
        auto client = Client::create(poll_thread_.as_ref().unwrap());
        EXPECT_FALSE(client->connected());

        ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
        std::this_thread::sleep_for(milliseconds(20));
        EXPECT_TRUE(client->connected());

        client->close();
        std::this_thread::sleep_for(milliseconds(20));
        EXPECT_FALSE(client->connected());
    }

    delete server;
}

TEST_F(StateIntegrationTest, ConnectionObjectAccessible) {
    // Start server
    auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
    auto service_box = rusty::make_box<StateTestService>();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Before connect, connection() should return None
    auto conn_before = client->connection();
    EXPECT_TRUE(conn_before.is_none());

    // Connect
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(test_port_)).c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // After connect, connection() should return Some
    auto conn_after = client->connection();
    EXPECT_TRUE(conn_after.is_some());

    // Can query state through connection
    if (conn_after.is_some()) {
        auto conn = conn_after.unwrap();
        EXPECT_TRUE(conn->connected());
        // Can access connection_state() on ClientConnection
        EXPECT_EQ(conn->connection_state(), ConnectionState::CONNECTED);
    }

    client->close();
    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
