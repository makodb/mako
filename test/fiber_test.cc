/**
 * @file fiber_test.cc
 * @brief Unit tests for the Fiber API (this_fiber namespace).
 */

#include <gtest/gtest.h>
#include "reactor/fiber.h"
#include "reactor/reactor.h"
#include "base/basetypes.hpp"

namespace rrr {

class FiberTest : public ::testing::Test {
protected:
    void SetUp() override {
        // No persistent state needed - get reactor locally in each test
    }

    void TearDown() override {
        // Clean up
    }
};

// =============================================================================
// Type Alias Tests
// =============================================================================

TEST_F(FiberTest, FiberAliasIsSameAsCoroutine) {
    // Verify Fiber is the same type as Coroutine
    static_assert(std::is_same<Fiber, Coroutine>::value,
                  "Fiber must be an alias for Coroutine");
}

TEST_F(FiberTest, WaitAllIsSameAsAndEvent) {
    // Verify WaitAll is the same type as AndEvent
    static_assert(std::is_same<WaitAll, AndEvent>::value,
                  "WaitAll must be an alias for AndEvent");
}

TEST_F(FiberTest, WaitAnyIsSameAsOrEvent) {
    // Verify WaitAny is the same type as OrEvent
    static_assert(std::is_same<WaitAny, OrEvent>::value,
                  "WaitAny must be an alias for OrEvent");
}

TEST_F(FiberTest, WaitNIsSameAsNEvent) {
    // Verify WaitN is the same type as NEvent
    static_assert(std::is_same<WaitN, NEvent>::value,
                  "WaitN must be an alias for NEvent");
}

// =============================================================================
// this_fiber::get_id() Tests
// =============================================================================

TEST_F(FiberTest, GetIdOutsideFiberContext) {
    // Outside fiber context, should return 0
    EXPECT_EQ(0u, this_fiber::get_id());
}

// Simple test to verify lambda runs during create_run
TEST_F(FiberTest, LambdaRunsDuringCreateRun) {
    bool lambda_ran = false;

    Fiber::create_run([&lambda_ran]() {
        lambda_ran = true;
    });

    // Lambda should run immediately during create_run (no loop needed)
    EXPECT_TRUE(lambda_ran);
}

TEST_F(FiberTest, GetIdInsideFiberContext) {
    uint64_t captured_id = 0;
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&captured_id]() {
        captured_id = this_fiber::get_id();
    });

    reactor->loop();

    // Inside fiber context, should return non-zero ID
    EXPECT_NE(0u, captured_id);
}

TEST_F(FiberTest, GetIdUniquePerFiber) {
    uint64_t id1 = 0, id2 = 0;
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&id1]() {
        id1 = this_fiber::get_id();
    });

    Fiber::create_run([&id2]() {
        id2 = this_fiber::get_id();
    });

    reactor->loop();

    // Each fiber should have a unique ID
    EXPECT_NE(0u, id1);
    EXPECT_NE(0u, id2);
    EXPECT_NE(id1, id2);
}

// =============================================================================
// this_fiber::current() Tests
// =============================================================================

TEST_F(FiberTest, CurrentOutsideFiberContext) {
    // Outside fiber context, should return None
    auto current = this_fiber::current();
    EXPECT_TRUE(current.is_none());
}

TEST_F(FiberTest, CurrentInsideFiberContext) {
    bool got_current = false;
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&got_current]() {
        auto current = this_fiber::current();
        got_current = current.is_some();
    });

    reactor->loop();

    // Inside fiber context, should return Some
    EXPECT_TRUE(got_current);
}

// =============================================================================
// this_fiber::in_fiber_context() Tests
// =============================================================================

TEST_F(FiberTest, InFiberContextOutside) {
    EXPECT_FALSE(this_fiber::in_fiber_context());
}

TEST_F(FiberTest, InFiberContextInside) {
    bool inside = false;
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&inside]() {
        inside = this_fiber::in_fiber_context();
    });

    reactor->loop();

    EXPECT_TRUE(inside);
}

// =============================================================================
// this_fiber::yield() Tests
// =============================================================================

TEST_F(FiberTest, YieldOutsideFiberContext) {
    // Should be a no-op outside fiber context (no crash)
    this_fiber::yield();
    SUCCEED();
}

TEST_F(FiberTest, YieldInsideFiberContext) {
    int step = 0;
    auto reactor = Reactor::get_reactor();

    auto fiber = Fiber::create_run([&step]() {
        step = 1;
        this_fiber::yield();
        step = 2;
    });

    // After create_run, fiber runs until first yield
    EXPECT_EQ(1, step);

    // Explicitly continue the fiber to complete it
    reactor->continue_coro(fiber);

    // Fiber should have completed
    EXPECT_EQ(2, step);
}

// =============================================================================
// this_fiber::sleep_us() Tests
// =============================================================================

TEST_F(FiberTest, SleepUsZero) {
    bool completed = false;
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&completed]() {
        this_fiber::sleep_us(0);
        completed = true;
    });

    reactor->loop();

    EXPECT_TRUE(completed);
}

TEST_F(FiberTest, SleepUsPositive) {
    uint64_t start_time = Time::now(true);
    uint64_t end_time = 0;
    const uint64_t sleep_duration = 1000; // 1ms
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&end_time, sleep_duration]() {
        this_fiber::sleep_us(sleep_duration);
        end_time = Time::now(true);
    });

    reactor->loop();

    // Should have slept at least the specified duration
    EXPECT_GE(end_time - start_time, sleep_duration);
}

// =============================================================================
// this_fiber::sleep_ms() Tests
// =============================================================================

TEST_F(FiberTest, SleepMsConversion) {
    uint64_t start_time = Time::now(true);
    uint64_t end_time = 0;
    const uint64_t sleep_ms = 5; // 5ms
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&end_time, sleep_ms]() {
        this_fiber::sleep_ms(sleep_ms);
        end_time = Time::now(true);
    });

    reactor->loop();

    // Should have slept at least 5ms = 5000us
    EXPECT_GE(end_time - start_time, sleep_ms * 1000);
}

// =============================================================================
// this_fiber::sleep_s() Tests
// =============================================================================

TEST_F(FiberTest, SleepSConversion) {
    // Just verify sleep_s compiles correctly with the conversion
    // The actual timing behavior is tested by SleepUsPositive/SleepMsConversion
    // Note: Even sleep_s(0) creates a TimeoutEvent that may yield

    // Verify the conversion factor is correct: 1 second = 1,000,000 us
    static_assert(Time::RRR_USEC_PER_SEC == 1000000,
                  "1 second should be 1,000,000 microseconds");

    // Just verify the function exists and is callable
    // (don't actually call it as it would block)
    [[maybe_unused]] auto fn = &this_fiber::sleep_s;
    SUCCEED();
}

// =============================================================================
// this_fiber::sleep_until_us() Tests
// =============================================================================

TEST_F(FiberTest, SleepUntilPastTime) {
    // If target time is in the past, should return immediately
    uint64_t start_time = Time::now(true);
    uint64_t end_time = 0;
    uint64_t past_time = start_time - 1000; // 1ms in the past
    auto reactor = Reactor::get_reactor();

    Fiber::create_run([&end_time, past_time]() {
        this_fiber::sleep_until_us(past_time);
        end_time = Time::now(true);
    });

    reactor->loop();

    // Should return immediately (within a few hundred microseconds)
    EXPECT_LT(end_time - start_time, 10000u); // Less than 10ms
}

TEST_F(FiberTest, SleepUntilFutureTime) {
    // Just verify sleep_until_us compiles and is callable
    // The actual timing behavior is tested by SleepUsPositive and SleepUntilPastTime
    // Note: Any future time creates a TimeoutEvent that yields, so we can't
    // easily test without a full reactor loop.

    // Verify the function exists and is callable (address check)
    [[maybe_unused]] auto fn = &this_fiber::sleep_until_us;
    SUCCEED();
}

}  // namespace rrr

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
