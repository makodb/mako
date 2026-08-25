/**
 * @file fiber_test.cc
 * @brief Unit tests for the Fiber API (this_fiber namespace).
 */

#include <gtest/gtest.h>
#include <rusty/option.hpp>
#include "../rrr.hpp"

namespace rrr {

class FiberTest : public ::testing::Test {
protected:
    void SetUp() override {
        // No persistent state needed - get reactor locally in each test
    }

    void TearDown() override {
        // Drop the thread-local reactor + any running-fiber slot between
        // tests so each test gets a fresh scheduler. Without this, fibers
        // suspended in one test (e.g. after a yield/sleep) stay registered
        // in the next test's `reactor->run_loop(false, true)` call and block it forever.
        // The Rc<Reactor> goes out of scope on assignment, running ~Reactor.
        *rrr::sp_running_fiber_th_.borrow_mut() = rusty::None;
        rrr::sp_reactor_th_ = rusty::None;
    }

    // Drive the reactor until `done` holds, or give up after timeout_us.
    //
    // A single `run_loop(false, true)` pass returns long before a
    // millisecond-scale timeout expires, so a sleeping fiber is still
    // PARKED when the pass returns. Nothing then resumes it: it stays
    // registered in `reactor->fibers_`, its stack is never unwound, and
    // the `Rc<Reactor>` living in that suspended frame keeps the reactor
    // alive past TearDown's drop -- which is the 1,088-byte "leak"
    // LeakSanitizer reports for this file.
    //
    // It also made the sleep assertions VACUOUS: with the fiber never
    // finishing, `end_time` stayed 0 and `end_time - start_time` wrapped
    // to ~1.8e19, satisfying any EXPECT_GE. Callers must ASSERT_TRUE on
    // the result before treating that subtraction as meaningful.
    // (The reactor handle is a template parameter so this file does not
    // have to name rusty::Rc, which it never imports.)
    template <typename ReactorHandle, typename Pred>
    static bool DriveUntil(const ReactorHandle& reactor, Pred done,
                           uint64_t timeout_us) {
        const uint64_t deadline = rrr::Time::now(true) + timeout_us;
        while (!done()) {
            if (rrr::Time::now(true) >= deadline) {
                return false;
            }
            reactor->run_loop(false, true);
            rrr::Time::sleep(100);
        }
        return true;
    }
};

// =============================================================================
// Type Alias Tests
// =============================================================================

TEST_F(FiberTest, FiberIsDefined) {
    // Verify Fiber class is defined and usable
    static_assert(sizeof(Fiber) > 0, "Fiber must be a defined class");
}

TEST_F(FiberTest, WaitAllIsDefined) {
    // Verify WaitAll class is defined and usable
    static_assert(sizeof(WaitAll) > 0, "WaitAll must be a defined class");
}

TEST_F(FiberTest, WaitAnyIsDefined) {
    // Verify WaitAny class is defined and usable
    static_assert(sizeof(WaitAny) > 0, "WaitAny must be a defined class");
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

    reactor->run_loop(false, true);

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

    reactor->run_loop(false, true);

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

    reactor->run_loop(false, true);

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

    reactor->run_loop(false, true);

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
    reactor->continue_fiber(fiber);

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

    reactor->run_loop(false, true);

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

    ASSERT_TRUE(DriveUntil(reactor, [&] { return end_time != 0; }, 1000000))
        << "fiber never completed its sleep; the assertion below would be vacuous";

    // Should have slept at least the specified duration
    ASSERT_GE(end_time, start_time);
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

    ASSERT_TRUE(DriveUntil(reactor, [&] { return end_time != 0; }, 1000000))
        << "fiber never completed its sleep; the assertion below would be vacuous";

    // Should have slept at least 5ms = 5000us
    ASSERT_GE(end_time, start_time);
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
    static_assert(rrr::RRR_USEC_PER_SEC == 1000000,
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

    // A past deadline should not park the fiber, so this returns on the
    // first pass; assert that explicitly instead of relying on the
    // subtraction below to notice.
    ASSERT_TRUE(DriveUntil(reactor, [&] { return end_time != 0; }, 1000000))
        << "sleep_until_us(past) never resumed the fiber";

    // Should return immediately (within a few hundred microseconds)
    ASSERT_GE(end_time, start_time);
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

// =============================================================================
// Future/Promise Tests
// =============================================================================

TEST_F(FiberTest, PromiseGetFutureOnce) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    EXPECT_TRUE(future.valid());
}

TEST_F(FiberTest, PromiseGetFutureTwiceThrows) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    EXPECT_THROW(promise.get_future(), std::logic_error);
}

TEST_F(FiberTest, PromiseSetValueOnce) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    promise.set_value(42);
    EXPECT_TRUE(promise.is_ready());
}

TEST_F(FiberTest, PromiseSetValueTwiceThrows) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    promise.set_value(42);
    EXPECT_THROW(promise.set_value(100), std::logic_error);
}

TEST_F(FiberTest, FutureIsReadyAfterSet) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    EXPECT_FALSE(future.is_ready());
    promise.set_value(42);
    EXPECT_TRUE(future.is_ready());
}

TEST_F(FiberTest, FutureGetValueImmediate) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    promise.set_value(42);

    // Value already set, get() returns immediately (no blocking)
    EXPECT_EQ(42, future.get());
}

TEST_F(FiberTest, FutureGetValueMultipleTimes) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    promise.set_value(42);

    // Can call get() multiple times
    EXPECT_EQ(42, future.get());
    EXPECT_EQ(42, future.get());
    EXPECT_EQ(42, future.get());
}

TEST_F(FiberTest, FutureGetValueInFiber) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    int received_value = 0;
    auto reactor = Reactor::get_reactor();

    // Consumer fiber waits for value
    Fiber::create_run([&future, &received_value]() {
        received_value = future.get();
    });

    // Producer sets value
    promise.set_value(42);

    // Run reactor to let consumer fiber complete
    reactor->run_loop(false, true);

    EXPECT_EQ(42, received_value);
}

TEST_F(FiberTest, FutureWaitForTimeout) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    bool ready = false;
    auto reactor = Reactor::get_reactor();

    // Run wait_for inside a fiber context
    Fiber::create_run([&future, &ready]() {
        // Wait with timeout should return false if not set
        ready = future.wait_for(1000);  // 1ms timeout
    });

    reactor->run_loop(false, true);

    EXPECT_FALSE(ready);
}

TEST_F(FiberTest, FutureWaitForReady) {
    auto promise = FiberPromise<int>::default_();
    auto future = promise.get_future();
    promise.set_value(42);

    // Wait should return immediately if already set
    bool ready = future.wait_for(1000);
    EXPECT_TRUE(ready);
}

TEST_F(FiberTest, DefaultFutureIsInvalid) {
    auto future = FiberFuture<int>::default_();
    EXPECT_FALSE(future.valid());
    EXPECT_FALSE(future.is_ready());
}

TEST_F(FiberTest, MovedFromFutureIsInvalid) {
    auto promise = FiberPromise<int>::default_();
    auto future1 = promise.get_future();
    auto future2 = std::move(future1);

    EXPECT_FALSE(future1.valid());
    EXPECT_TRUE(future2.valid());
}

TEST_F(FiberTest, MovedFromPromiseThrows) {
    auto promise1 = FiberPromise<int>::default_();
    auto future = promise1.get_future();
    FiberPromise<int> promise2 = std::move(promise1);

    // Moved-from promise should throw
    EXPECT_THROW(promise1.set_value(42), std::logic_error);

    // New owner should work
    promise2.set_value(42);
    EXPECT_EQ(42, future.get());
}

TEST_F(FiberTest, MakePromiseConvenience) {
    auto [promise, future] = make_promise<std::string>();
    EXPECT_TRUE(future.valid());
    EXPECT_FALSE(future.is_ready());

    promise.set_value("hello");
    EXPECT_EQ("hello", future.get());
}

TEST_F(FiberTest, MakeReadyFuture) {
    auto future = make_ready_future<int>(42);
    EXPECT_TRUE(future.valid());
    EXPECT_TRUE(future.is_ready());
    EXPECT_EQ(42, future.get());
}

TEST_F(FiberTest, FutureWithStringType) {
    auto promise = FiberPromise<std::string>::default_();
    auto future = promise.get_future();
    promise.set_value("hello world");
    EXPECT_EQ("hello world", future.get());
}

TEST_F(FiberTest, FutureWithVectorType) {
    auto promise = FiberPromise<std::vector<int>>::default_();
    auto future = promise.get_future();
    promise.set_value({1, 2, 3, 4, 5});

    auto result = future.get();
    EXPECT_EQ(5u, result.size());
    EXPECT_EQ(1, result[0]);
    EXPECT_EQ(5, result[4]);
}

}  // namespace rrr

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
