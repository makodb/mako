/**
 * Unit tests for Phase 3.2: Connection Health Metrics
 * Tests ConnectionMetrics tracking of requests, bytes, latency, and connection lifecycle.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <rusty/arc.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "rpc/connection_metrics.hpp"
#include "misc/marshal.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Helper to get current time in milliseconds
static uint64_t current_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}

// ============================================================================
// ConnectionMetrics Unit Tests
// ============================================================================

TEST(ConnectionMetricsTest, InitialValuesZero) {
    ConnectionMetrics metrics;
    EXPECT_EQ(metrics.requests_sent(), 0u);
    EXPECT_EQ(metrics.requests_completed(), 0u);
    EXPECT_EQ(metrics.requests_failed(), 0u);
    EXPECT_EQ(metrics.requests_timed_out(), 0u);
    EXPECT_EQ(metrics.bytes_sent(), 0u);
    EXPECT_EQ(metrics.bytes_received(), 0u);
    EXPECT_EQ(metrics.reconnect_count(), 0u);
    EXPECT_EQ(metrics.min_latency_us(), 0u);  // Returns 0 when no data
    EXPECT_EQ(metrics.max_latency_us(), 0u);
}

TEST(ConnectionMetricsTest, RequestSentIncrement) {
    ConnectionMetrics metrics;

    metrics.record_request_sent();
    EXPECT_EQ(metrics.requests_sent(), 1u);

    metrics.record_request_sent();
    metrics.record_request_sent();
    EXPECT_EQ(metrics.requests_sent(), 3u);
}

TEST(ConnectionMetricsTest, RequestCompletedWithLatency) {
    ConnectionMetrics metrics;

    metrics.record_request_sent();
    metrics.record_request_completed(1000);  // 1000 microseconds

    EXPECT_EQ(metrics.requests_completed(), 1u);
    EXPECT_EQ(metrics.avg_latency_us(), 1000u);
    EXPECT_EQ(metrics.min_latency_us(), 1000u);
    EXPECT_EQ(metrics.max_latency_us(), 1000u);
}

TEST(ConnectionMetricsTest, RequestCompletedWithoutLatency) {
    ConnectionMetrics metrics;

    metrics.record_request_sent();
    metrics.record_request_completed();

    EXPECT_EQ(metrics.requests_completed(), 1u);
    EXPECT_EQ(metrics.avg_latency_us(), 0u);  // No latency recorded
}

TEST(ConnectionMetricsTest, RequestFailedIncrement) {
    ConnectionMetrics metrics;

    metrics.record_request_sent();
    metrics.record_request_failed();

    EXPECT_EQ(metrics.requests_failed(), 1u);
}

TEST(ConnectionMetricsTest, RequestTimeoutIncrement) {
    ConnectionMetrics metrics;

    metrics.record_request_sent();
    metrics.record_request_timeout();

    EXPECT_EQ(metrics.requests_timed_out(), 1u);
}

TEST(ConnectionMetricsTest, ByteCountersAccumulate) {
    ConnectionMetrics metrics;

    metrics.record_bytes_sent(100);
    metrics.record_bytes_sent(200);
    EXPECT_EQ(metrics.bytes_sent(), 300u);

    metrics.record_bytes_received(50);
    metrics.record_bytes_received(75);
    EXPECT_EQ(metrics.bytes_received(), 125u);
}

TEST(ConnectionMetricsTest, ReconnectCountIncrement) {
    ConnectionMetrics metrics;

    metrics.record_reconnect();
    EXPECT_EQ(metrics.reconnect_count(), 1u);

    metrics.record_reconnect();
    metrics.record_reconnect();
    EXPECT_EQ(metrics.reconnect_count(), 3u);
}

TEST(ConnectionMetricsTest, SuccessRateCalculation) {
    ConnectionMetrics metrics;

    // No requests = 100% success
    EXPECT_EQ(metrics.success_rate_percent(), 100u);

    // 3 sent, 3 completed = 100%
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_completed();
    metrics.record_request_completed();
    metrics.record_request_completed();
    EXPECT_EQ(metrics.success_rate_percent(), 100u);

    // 4 sent, 3 completed = 75%
    metrics.record_request_sent();
    EXPECT_EQ(metrics.success_rate_percent(), 75u);
}

TEST(ConnectionMetricsTest, AverageLatencyCalculation) {
    ConnectionMetrics metrics;

    // No completions = 0 avg
    EXPECT_EQ(metrics.avg_latency_us(), 0u);

    // Single completion
    metrics.record_request_completed(1000);
    EXPECT_EQ(metrics.avg_latency_us(), 1000u);

    // Average of 1000 and 3000 = 2000
    metrics.record_request_completed(3000);
    EXPECT_EQ(metrics.avg_latency_us(), 2000u);
}

TEST(ConnectionMetricsTest, MinMaxLatencyTracking) {
    ConnectionMetrics metrics;

    metrics.record_request_completed(500);
    EXPECT_EQ(metrics.min_latency_us(), 500u);
    EXPECT_EQ(metrics.max_latency_us(), 500u);

    metrics.record_request_completed(1000);
    EXPECT_EQ(metrics.min_latency_us(), 500u);
    EXPECT_EQ(metrics.max_latency_us(), 1000u);

    metrics.record_request_completed(200);
    EXPECT_EQ(metrics.min_latency_us(), 200u);
    EXPECT_EQ(metrics.max_latency_us(), 1000u);
}

TEST(ConnectionMetricsTest, Reset) {
    ConnectionMetrics metrics;

    metrics.record_request_sent();
    metrics.record_request_completed(1000);
    metrics.record_bytes_sent(100);
    metrics.record_bytes_received(50);
    metrics.record_reconnect();
    metrics.record_connect(current_time_ms());

    EXPECT_GT(metrics.requests_sent(), 0u);

    metrics.reset();

    EXPECT_EQ(metrics.requests_sent(), 0u);
    EXPECT_EQ(metrics.requests_completed(), 0u);
    EXPECT_EQ(metrics.bytes_sent(), 0u);
    EXPECT_EQ(metrics.bytes_received(), 0u);
    EXPECT_EQ(metrics.reconnect_count(), 0u);
    EXPECT_EQ(metrics.connect_time_ms(), 0u);
}

TEST(ConnectionMetricsTest, ConnectTimeRecorded) {
    ConnectionMetrics metrics;

    EXPECT_EQ(metrics.connect_time_ms(), 0u);

    auto now_ms = current_time_ms();
    metrics.record_connect(now_ms);

    EXPECT_EQ(metrics.connect_time_ms(), now_ms);
    EXPECT_GE(metrics.uptime_ms(current_time_ms()), 0u);  // Should be >= 0
}

TEST(ConnectionMetricsTest, ThreadSafety) {
    ConnectionMetrics metrics;
    std::vector<std::thread> threads;
    const int num_threads = 4;
    const int ops_per_thread = 100;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&metrics, ops_per_thread]() {
            for (int j = 0; j < ops_per_thread; j++) {
                metrics.record_request_sent();
                metrics.record_request_completed();
                metrics.record_bytes_sent(10);
                metrics.record_bytes_received(10);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(metrics.requests_sent(), num_threads * ops_per_thread);
    EXPECT_EQ(metrics.requests_completed(), num_threads * ops_per_thread);
    EXPECT_EQ(metrics.bytes_sent(), num_threads * ops_per_thread * 10);
    EXPECT_EQ(metrics.bytes_received(), num_threads * ops_per_thread * 10);
}

// ============================================================================
// Integration Tests with Real Connection
// ============================================================================

class MetricsTestService : public benchmark::BenchmarkService {
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

class ConnectionMetricsIntegrationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    ConnectionMetricsIntegrationTest() : test_port_(test_ports::get_port()) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    Server* start_server() {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<MetricsTestService>();
        server->reg_service(std::move(service_box));
        if (server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()) != 0) {
            delete server;
            return nullptr;
        }
        return server;
    }

    std::string server_addr() {
        return "127.0.0.1:" + std::to_string(test_port_);
    }
};

TEST_F(ConnectionMetricsIntegrationTest, MetricsUpdatedOnRealRequests) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());

    // Get initial metrics (reference API)
    const auto& metrics = client->metrics();

    // Connect time should be recorded
    EXPECT_GT(metrics.connect_time_ms(), 0u);

    // Make several requests
    for (int i = 0; i < 5; i++) {
        std::string input = "test_" + std::to_string(i);
        auto fu_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
    }

    // Verify metrics were updated
    EXPECT_EQ(metrics.requests_sent(), 5u);
    EXPECT_EQ(metrics.requests_completed(), 5u);
    EXPECT_EQ(metrics.requests_failed(), 0u);
    EXPECT_GT(metrics.bytes_sent(), 0u);
    EXPECT_GT(metrics.bytes_received(), 0u);
    EXPECT_EQ(metrics.success_rate_percent(), 100u);

    client->close();
    delete server;
}

TEST_F(ConnectionMetricsIntegrationTest, MetricsAfterReconnect) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    const auto& metrics = client->metrics();

    // Make some initial requests
    for (int i = 0; i < 3; i++) {
        std::string input = "before_" + std::to_string(i);
        auto fu_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        if (fu_result.is_ok()) {
            fu_result.unwrap()->wait();
        }
    }

    EXPECT_EQ(metrics.requests_sent(), 3u);
    EXPECT_EQ(metrics.reconnect_count(), 0u);

    // Disconnect and reconnect
    client->close();
    std::this_thread::sleep_for(milliseconds(50));

    std::atomic<bool> reconnect_done{false};
    client->reconnect([&](bool success) {
        reconnect_done = true;
    });

    for (int i = 0; i < 50 && !reconnect_done; i++) {
        std::this_thread::sleep_for(milliseconds(20));
    }

    if (reconnect_done && client->connected()) {
        // Reconnect count should have increased
        EXPECT_EQ(metrics.reconnect_count(), 1u);

        // Make more requests after reconnect
        for (int i = 0; i < 2; i++) {
            std::string input = "after_" + std::to_string(i);
            auto fu_result = client->request(
                benchmark::BenchmarkService::FAST_NOP,
                [&](Marshal& m) { m << input; }
            );
            if (fu_result.is_ok()) {
                fu_result.unwrap()->wait();
            }
        }

        EXPECT_EQ(metrics.requests_sent(), 5u);
    }

    client->close();
    delete server;
}

TEST_F(ConnectionMetricsIntegrationTest, ByteCounterAccuracy) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    const auto& metrics = client->metrics();

    uint64_t bytes_sent_before = metrics.bytes_sent();
    uint64_t bytes_received_before = metrics.bytes_received();

    // Make a request
    std::string input = "test_data";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    fu_result.unwrap()->wait();

    // Bytes should have increased
    EXPECT_GT(metrics.bytes_sent(), bytes_sent_before);
    EXPECT_GT(metrics.bytes_received(), bytes_received_before);

    client->close();
    delete server;
}

TEST_F(ConnectionMetricsIntegrationTest, ClientWithoutConnection) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // No connection yet - has_connection() should be false, metrics returns empty
    EXPECT_FALSE(client->has_connection());
    const auto& metrics = client->metrics();
    // Empty metrics should have all zeros
    EXPECT_EQ(metrics.requests_sent(), 0u);
    EXPECT_EQ(metrics.bytes_sent(), 0u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
