#include <stdint.h>
#include <stddef.h>

#include <rusty/option.hpp>
#include <rusty/box.hpp>
/**
 * Unit tests for RPC Request Timeout and Retry.
 * Tests RequestOptions, TimeoutType, and Future retry support.
 */

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/traits.hpp>
#include "../rrr.hpp"

// Trimmed from the consumer umbrella (08b68144) — import directly.
import rrr.request_options;
#include "rpc_test_ports.h"

import std;

using namespace rrr;
using namespace std::chrono;

namespace {

class TimeoutRetryService : public Service {
public:
    static constexpr i32 kRpcId = 0x58a31f62;

    explicit TimeoutRetryService(int drops_before_reply)
        : drops_before_reply_(drops_before_reply) {}

    std::atomic<int> call_count{0};

    int __reg_to__(Server& svr, size_t svc_index) override {
        return svr.reg_rpc(kRpcId, svc_index);
    }

    void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection weak_sconn) override {
        if (rpc_id != kRpcId) {
            return;
        }

        v32 payload;
        rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));
        rrr::Deserialize_::deserialize(payload, __req_ar__);

        int call = call_count.fetch_add(1) + 1;
        if (call <= drops_before_reply_) {
            return;  // Simulate lost response so client attempt times out.
        }

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_none()) {
            return;
        }

        auto sconn = sconn_opt.unwrap();
        const_cast<ServerConnection&>(*sconn).reply(*req, 0, [payload](BinaryWriteArchive& m) {
            rrr::Serialize_::serialize(payload, m);
        });
    }

private:
    int drops_before_reply_;
};

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

class TimeoutRetryIntegrationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_ = test_ports::get_port();

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    std::string server_addr() const {
        return "127.0.0.1:" + std::to_string(test_port_);
    }
};

}  // namespace

// ============================================================================
// TimeoutType Tests
// ============================================================================

TEST(TimeoutTypeTest, DefaultIsNone) {
    TimeoutType type = TimeoutType::NONE;
    EXPECT_EQ(type, TimeoutType::NONE);
}

TEST(TimeoutTypeTest, StringConversions) {
    EXPECT_EQ(timeout_type_to_string(TimeoutType::NONE), "NONE");
    EXPECT_EQ(timeout_type_to_string(TimeoutType::CONNECT_TIMEOUT), "CONNECT_TIMEOUT");
    EXPECT_EQ(timeout_type_to_string(TimeoutType::REQUEST_TIMEOUT), "REQUEST_TIMEOUT");
    EXPECT_EQ(timeout_type_to_string(TimeoutType::RESPONSE_TIMEOUT), "RESPONSE_TIMEOUT");
    EXPECT_EQ(timeout_type_to_string(TimeoutType::TOTAL_TIMEOUT), "TOTAL_TIMEOUT");
}

TEST(TimeoutTypeTest, UnknownTypeReturnsUnknown) {
    // Cast invalid value
    auto invalid = static_cast<TimeoutType>(255);
    EXPECT_EQ(timeout_type_to_string(invalid), "UNKNOWN");
}

// ============================================================================
// RequestOptions Default Values Tests
// ============================================================================

TEST(RequestOptionsTest, DefaultValues) {
    auto opts = RequestOptions::defaults();
    EXPECT_EQ(opts.timeout_ms, 1000u);
    EXPECT_EQ(opts.total_timeout_ms, 0u);
    EXPECT_EQ(opts.max_retries, 0u);
    EXPECT_EQ(opts.base_delay_ms, 50u);
    EXPECT_EQ(opts.max_delay_ms, 5000u);
    EXPECT_FLOAT_EQ(opts.jitter_factor, 0.1f);
    EXPECT_FALSE(opts.idempotent);
}

TEST(RequestOptionsTest, DefaultsPreset) {
    auto opts = RequestOptions::defaults();
    EXPECT_EQ(opts.timeout_ms, 1000u);
    EXPECT_EQ(opts.max_retries, 0u);
    EXPECT_FALSE(opts.idempotent);
}

// ============================================================================
// RequestOptions Preset Tests
// ============================================================================

TEST(RequestOptionsTest, WithRetryPreset) {
    auto opts = RequestOptions::with_retry(3, 2000);
    EXPECT_EQ(opts.timeout_ms, 2000u);
    EXPECT_EQ(opts.max_retries, 3u);
    EXPECT_TRUE(opts.idempotent);  // with_retry implies idempotent
}

TEST(RequestOptionsTest, WithRetryDefaultTimeout) {
    auto opts = RequestOptions::with_retry(5, 1000);
    EXPECT_EQ(opts.timeout_ms, 1000u);  // Default
    EXPECT_EQ(opts.max_retries, 5u);
    EXPECT_TRUE(opts.idempotent);
}

TEST(RequestOptionsTest, IdempotentRetryPreset) {
    auto opts = RequestOptions::idempotent_retry(3);  // Default 3 retries
    EXPECT_EQ(opts.max_retries, 3u);
    EXPECT_TRUE(opts.idempotent);
}

TEST(RequestOptionsTest, IdempotentRetryCustom) {
    auto opts = RequestOptions::idempotent_retry(10);
    EXPECT_EQ(opts.max_retries, 10u);
    EXPECT_TRUE(opts.idempotent);
}

TEST(RequestOptionsTest, NoTimeoutPreset) {
    auto opts = RequestOptions::no_timeout();
    EXPECT_EQ(opts.timeout_ms, 0u);
    EXPECT_EQ(opts.total_timeout_ms, 0u);
}

TEST(RequestOptionsTest, FastPreset) {
    auto opts = RequestOptions::fast();
    EXPECT_EQ(opts.timeout_ms, 100u);
    EXPECT_EQ(opts.max_retries, 2u);
    EXPECT_EQ(opts.base_delay_ms, 10u);
    EXPECT_EQ(opts.max_delay_ms, 100u);
    EXPECT_TRUE(opts.idempotent);
}

TEST(RequestOptionsTest, PatientPreset) {
    auto opts = RequestOptions::patient();
    EXPECT_EQ(opts.timeout_ms, 10000u);
    EXPECT_EQ(opts.total_timeout_ms, 60000u);
    EXPECT_EQ(opts.max_retries, 5u);
    EXPECT_EQ(opts.base_delay_ms, 500u);
    EXPECT_EQ(opts.max_delay_ms, 10000u);
    EXPECT_TRUE(opts.idempotent);
}

// ============================================================================
// can_retry Tests
// ============================================================================

TEST(RequestOptionsTest, CanRetryWhenIdempotentAndUnderLimit) {
    auto opts = RequestOptions::defaults();
    opts.idempotent = true;
    opts.max_retries = 3;

    EXPECT_TRUE(opts.can_retry(0));
    EXPECT_TRUE(opts.can_retry(1));
    EXPECT_TRUE(opts.can_retry(2));
    EXPECT_FALSE(opts.can_retry(3));  // At limit
    EXPECT_FALSE(opts.can_retry(4));  // Over limit
}

TEST(RequestOptionsTest, CannotRetryWhenNotIdempotent) {
    auto opts = RequestOptions::defaults();
    opts.idempotent = false;
    opts.max_retries = 3;

    EXPECT_FALSE(opts.can_retry(0));
    EXPECT_FALSE(opts.can_retry(1));
}

TEST(RequestOptionsTest, CannotRetryWhenNoRetries) {
    auto opts = RequestOptions::defaults();
    opts.idempotent = true;
    opts.max_retries = 0;

    EXPECT_FALSE(opts.can_retry(0));
}

// ============================================================================
// calculate_delay_ms Tests
// ============================================================================

TEST(RequestOptionsTest, CalculateDelayExponentialBackoff) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 10000;
    opts.jitter_factor = 0.0f;  // Disable jitter for deterministic test

    // Exponential: base * 2^attempt
    EXPECT_EQ(opts.calculate_delay_ms(0), 100u);   // 100 * 2^0 = 100
    EXPECT_EQ(opts.calculate_delay_ms(1), 200u);   // 100 * 2^1 = 200
    EXPECT_EQ(opts.calculate_delay_ms(2), 400u);   // 100 * 2^2 = 400
    EXPECT_EQ(opts.calculate_delay_ms(3), 800u);   // 100 * 2^3 = 800
}

TEST(RequestOptionsTest, CalculateDelayCappedAtMax) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 500;
    opts.jitter_factor = 0.0f;

    EXPECT_EQ(opts.calculate_delay_ms(0), 100u);
    EXPECT_EQ(opts.calculate_delay_ms(1), 200u);
    EXPECT_EQ(opts.calculate_delay_ms(2), 400u);
    EXPECT_EQ(opts.calculate_delay_ms(3), 500u);  // Capped at max
    EXPECT_EQ(opts.calculate_delay_ms(10), 500u); // Still capped
}

TEST(RequestOptionsTest, CalculateDelayWithJitter) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 10000;
    opts.jitter_factor = 0.2f;  // 20% jitter

    // With jitter, delays should vary
    std::vector<uint64_t> delays;
    for (int i = 0; i < 100; i++) {
        delays.push_back(opts.calculate_delay_ms(0));
    }

    // Check that not all delays are the same (jitter adds variation)
    bool all_same = true;
    for (size_t i = 1; i < delays.size(); i++) {
        if (delays[i] != delays[0]) {
            all_same = false;
            break;
        }
    }
    EXPECT_FALSE(all_same) << "Jitter should cause variation in delays";

    // The f32 jitter factor promotes slightly above 0.2; at the minimum draw,
    // truncation therefore permits 89.  The maximum still truncates to 110.
    for (uint64_t delay : delays) {
        EXPECT_GE(delay, 89u);
        EXPECT_LE(delay, 110u);
    }
}

// ============================================================================
// Total Timeout Tests
// ============================================================================

TEST(RequestOptionsTest, TotalTimeoutExceeded) {
    auto opts = RequestOptions::defaults();
    opts.total_timeout_ms = 5000;

    EXPECT_FALSE(opts.is_total_timeout_exceeded(0));
    EXPECT_FALSE(opts.is_total_timeout_exceeded(4999));
    EXPECT_TRUE(opts.is_total_timeout_exceeded(5000));
    EXPECT_TRUE(opts.is_total_timeout_exceeded(10000));
}

TEST(RequestOptionsTest, TotalTimeoutNotSetNeverExceeds) {
    auto opts = RequestOptions::defaults();
    opts.total_timeout_ms = 0;  // Disabled

    EXPECT_FALSE(opts.is_total_timeout_exceeded(0));
    EXPECT_FALSE(opts.is_total_timeout_exceeded(1000000));
}

TEST(RequestOptionsTest, RemainingTimeCalculation) {
    auto opts = RequestOptions::defaults();
    opts.total_timeout_ms = 5000;

    EXPECT_EQ(opts.remaining_time_ms(0), 5000u);
    EXPECT_EQ(opts.remaining_time_ms(1000), 4000u);
    EXPECT_EQ(opts.remaining_time_ms(4999), 1u);
    EXPECT_EQ(opts.remaining_time_ms(5000), 0u);
    EXPECT_EQ(opts.remaining_time_ms(6000), 0u);
}

TEST(RequestOptionsTest, RemainingTimeNoLimit) {
    auto opts = RequestOptions::defaults();
    opts.total_timeout_ms = 0;

    EXPECT_EQ(opts.remaining_time_ms(0), UINT64_MAX);
    EXPECT_EQ(opts.remaining_time_ms(1000000), UINT64_MAX);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST(RequestOptionsTest, ConcurrentDelayCalculation) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 10000;
    opts.jitter_factor = 0.1f;

    std::atomic<int> error_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 8; i++) {
        threads.emplace_back([&opts, &error_count, i]() {
            for (int j = 0; j < 1000; j++) {
                uint64_t delay = opts.calculate_delay_ms(i % 5);
                // Should always be positive and reasonable
                if (delay == 0 || delay > 15000) {
                    error_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(error_count.load(), 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(RequestOptionsTest, ZeroBaseDelay) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 0;
    opts.max_delay_ms = 1000;
    opts.jitter_factor = 0.0f;

    EXPECT_EQ(opts.calculate_delay_ms(0), 0u);
    EXPECT_EQ(opts.calculate_delay_ms(5), 0u);
}

TEST(RequestOptionsTest, VeryLargeAttempt) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 5000;
    opts.jitter_factor = 0.0f;

    // Very large attempt should be capped
    EXPECT_EQ(opts.calculate_delay_ms(100), 5000u);
    EXPECT_EQ(opts.calculate_delay_ms(1000), 5000u);
}

TEST(RequestOptionsTest, MaxDelayLessThanBase) {
    auto opts = RequestOptions::defaults();
    opts.base_delay_ms = 1000;
    opts.max_delay_ms = 500;  // Less than base!
    opts.jitter_factor = 0.0f;

    // Should cap at max even if less than base
    EXPECT_EQ(opts.calculate_delay_ms(0), 500u);
}

// ============================================================================
// RequestOptions Copy/Move Tests
// ============================================================================

TEST(RequestOptionsTest, CopyConstruct) {
    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 5000;
    opts.max_retries = 10;
    opts.idempotent = true;

    RequestOptions copy = opts;
    EXPECT_EQ(copy.timeout_ms, 5000u);
    EXPECT_EQ(copy.max_retries, 10u);
    EXPECT_TRUE(copy.idempotent);
}

TEST(RequestOptionsTest, MoveConstruct) {
    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 5000;
    opts.max_retries = 10;

    RequestOptions moved = std::move(opts);
    EXPECT_EQ(moved.timeout_ms, 5000u);
    EXPECT_EQ(moved.max_retries, 10u);
}

// ============================================================================
// Retry Integration Tests
// ============================================================================

TEST_F(TimeoutRetryIntegrationTest, IdempotentRequestRetriesAfterTimeoutAndThenSucceeds) {
    auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto service_box = rusty::make_box<TimeoutRetryService>(1);  // Drop first response only.
    auto* service = service_box.get();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);

    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 40;
    opts.max_retries = 2;
    opts.base_delay_ms = 80;
    opts.max_delay_ms = 80;
    opts.jitter_factor = 0.0f;
    opts.idempotent = true;

    std::atomic<int> marshal_calls{0};
    auto start = steady_clock::now();
    auto fu_result = client->request_with_options(
        TimeoutRetryService::kRpcId, opts,
        [&](BinaryWriteArchive& m) {
            marshal_calls.fetch_add(1);
            rrr::Serialize_::serialize(v32(123), m);
        });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() { return fu->ready() || fu->timed_out(); }, milliseconds(2000)));
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - start).count();

    EXPECT_TRUE(fu->wait_with_options());
    EXPECT_EQ(fu->get_error_code(), 0);
    EXPECT_EQ(fu->get_retry_count(), 1);
    EXPECT_EQ(fu->get_timeout_type(), TimeoutType::NONE);
    EXPECT_GE(elapsed_ms, 100);  // Timeout + configured deterministic backoff.

    v32 reply_value;
    rrr::deserialize_from(fu->get_reply(), reply_value);
    EXPECT_EQ(reply_value.get(), 123);
    EXPECT_EQ(service->call_count.load(), 2);
    EXPECT_EQ(marshal_calls.load(), 1);  // Request payload serialized once.

    client->close();
    delete server;
}

TEST_F(TimeoutRetryIntegrationTest, NonIdempotentRequestNeverRetriesOnTimeout) {
    auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto service_box = rusty::make_box<TimeoutRetryService>(1000);  // Never reply in this test.
    auto* service = service_box.get();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);

    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 40;
    opts.max_retries = 5;   // Should be ignored because request is non-idempotent.
    opts.base_delay_ms = 80;
    opts.max_delay_ms = 80;
    opts.jitter_factor = 0.0f;
    opts.idempotent = false;

    auto fu_result = client->request_with_options(
        TimeoutRetryService::kRpcId, opts,
        [](BinaryWriteArchive& m) { rrr::Serialize_::serialize(v32(456), m); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() { return fu->ready() || fu->timed_out(); }, milliseconds(2000)));
    EXPECT_FALSE(fu->wait_with_options());
    EXPECT_TRUE(fu->timed_out());
    EXPECT_EQ(fu->get_error_code(), ETIMEDOUT);
    EXPECT_EQ(fu->get_timeout_type(), TimeoutType::RESPONSE_TIMEOUT);
    EXPECT_EQ(fu->get_retry_count(), 0);
    EXPECT_EQ(service->call_count.load(), 1);  // Initial attempt only.

    client->close();
    delete server;
}

TEST_F(TimeoutRetryIntegrationTest, RetryLoopStopsAtRetryLimitWithPerAttemptTimeout) {
    auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto service_box = rusty::make_box<TimeoutRetryService>(1000);  // Never reply in this test.
    auto* service = service_box.get();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);

    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 30;
    opts.max_retries = 2;
    opts.base_delay_ms = 50;
    opts.max_delay_ms = 50;
    opts.jitter_factor = 0.0f;
    opts.idempotent = true;

    auto start = steady_clock::now();
    auto fu_result = client->request_with_options(
        TimeoutRetryService::kRpcId, opts,
        [](BinaryWriteArchive& m) { rrr::Serialize_::serialize(v32(9), m); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() { return fu->ready() || fu->timed_out(); }, milliseconds(2500)));
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - start).count();

    EXPECT_FALSE(fu->wait_with_options());
    EXPECT_TRUE(fu->timed_out());
    EXPECT_EQ(fu->get_error_code(), ETIMEDOUT);
    EXPECT_EQ(fu->get_timeout_type(), TimeoutType::RESPONSE_TIMEOUT);
    EXPECT_EQ(fu->get_retry_count(), 2);
    EXPECT_EQ(service->call_count.load(), 3);  // Initial + 2 retries.
    EXPECT_GE(elapsed_ms, 170);  // 3 attempts + 2 deterministic backoff delays.

    client->close();
    delete server;
}

TEST_F(TimeoutRetryIntegrationTest, DisconnectedFailFastSetsConnectTimeoutType) {
    auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto service_box = rusty::make_box<TimeoutRetryService>(0);
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);

    BufferingConfig buffering = BufferingConfig::defaults();
    buffering.behavior = DisconnectBehavior::FAIL_FAST;
    client->set_buffering_config(buffering);
    client->close();
    ASSERT_TRUE(wait_for_condition([&]() { return !client->connected(); }, milliseconds(1000)));

    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 50;
    opts.max_retries = 0;
    opts.idempotent = true;

    auto fu_result = client->request_with_options(
        TimeoutRetryService::kRpcId, opts,
        [](BinaryWriteArchive& m) { rrr::Serialize_::serialize(v32(3), m); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() { return fu->ready() || fu->timed_out(); }, milliseconds(1000)));
    EXPECT_FALSE(fu->wait_with_options());
    EXPECT_TRUE(fu->timed_out());
    EXPECT_EQ(fu->get_error_code(), ENOTCONN);
    EXPECT_EQ(fu->get_timeout_type(), TimeoutType::CONNECT_TIMEOUT);
    EXPECT_EQ(fu->get_retry_count(), 0);

    delete server;
}

TEST_F(TimeoutRetryIntegrationTest, QueueRejectSetsRequestTimeoutType) {
    auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto service_box = rusty::make_box<TimeoutRetryService>(0);
    auto* service = service_box.get();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);

    BufferingConfig buffering = BufferingConfig::defaults();
    buffering.behavior = DisconnectBehavior::QUEUE;
    buffering.max_pending = 0;  // Force immediate queue reject when disconnected.
    buffering.overflow = OverflowStrategy::DROP_NEWEST;
    client->set_buffering_config(buffering);

    client->close();
    ASSERT_TRUE(wait_for_condition([&]() { return !client->connected(); }, milliseconds(1000)));

    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 50;
    opts.max_retries = 0;
    opts.idempotent = true;

    auto fu_result = client->request_with_options(
        TimeoutRetryService::kRpcId, opts,
        [](BinaryWriteArchive& m) { rrr::Serialize_::serialize(v32(5), m); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() { return fu->ready() || fu->timed_out(); }, milliseconds(1000)));
    EXPECT_FALSE(fu->wait_with_options());
    EXPECT_TRUE(fu->timed_out());
    EXPECT_EQ(fu->get_error_code(), EAGAIN);
    EXPECT_EQ(fu->get_timeout_type(), TimeoutType::REQUEST_TIMEOUT);
    EXPECT_EQ(fu->get_retry_count(), 0);
    EXPECT_EQ(service->call_count.load(), 0);  // Request never reached server while disconnected.

    delete server;
}

TEST_F(TimeoutRetryIntegrationTest, TotalTimeoutBudgetCutsOffRetriesBeforeNextAttempt) {
    auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
    auto service_box = rusty::make_box<TimeoutRetryService>(1000);  // Never reply in this test.
    auto* service = service_box.get();
    server->reg_service(std::move(service_box));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())), 0);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);

    auto opts = RequestOptions::defaults();
    opts.timeout_ms = 80;
    opts.total_timeout_ms = 130;
    opts.max_retries = 5;
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 100;
    opts.jitter_factor = 0.0f;
    opts.idempotent = true;

    auto start = steady_clock::now();
    auto fu_result = client->request_with_options(
        TimeoutRetryService::kRpcId, opts,
        [](BinaryWriteArchive& m) { rrr::Serialize_::serialize(v32(77), m); });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    ASSERT_TRUE(wait_for_condition([&]() { return fu->ready() || fu->timed_out(); }, milliseconds(2500)));
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - start).count();

    EXPECT_FALSE(fu->wait_with_options());
    EXPECT_TRUE(fu->timed_out());
    EXPECT_EQ(fu->get_error_code(), ETIMEDOUT);
    EXPECT_EQ(fu->get_timeout_type(), TimeoutType::TOTAL_TIMEOUT);
    EXPECT_EQ(fu->get_retry_count(), 0);  // No retry attempt started within total budget.
    EXPECT_EQ(service->call_count.load(), 1);
    EXPECT_GE(elapsed_ms, 70);
    EXPECT_LT(elapsed_ms, 220);

    client->close();
    delete server;
}

// ============================================================================
// Integration with rusty::Cell
// ============================================================================

TEST(RequestOptionsTest, CellStorageIsExplicitlySingleThreaded) {
    static_assert(!rusty::is_sync<rusty::Cell<RequestOptions>>::value);

    rusty::Cell<RequestOptions> cell{RequestOptions::defaults()};

    auto opts = cell.get();
    EXPECT_EQ(opts.timeout_ms, 1000u);

    RequestOptions new_opts = RequestOptions::patient();
    cell.set(new_opts);

    auto retrieved = cell.get();
    EXPECT_EQ(retrieved.timeout_ms, 10000u);
    EXPECT_EQ(retrieved.max_retries, 5u);
}
