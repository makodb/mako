#include <gtest/gtest.h>

import rrr.basetypes;

using namespace rrr;

TEST(Time, ClocksAndSleepAreLive) {
    const auto monotonic_before = Time::now(true);
    Time::sleep(1'000);
    const auto monotonic_after = Time::now(true);

    EXPECT_GE(monotonic_after, monotonic_before);
    EXPECT_GT(Time::now(false), 0u);
}

TEST(Timer, StartStopAndReset) {
    auto timer = Timer::new_();
    EXPECT_EQ(timer.begin_us, 0u);
    EXPECT_EQ(timer.end_us, 0u);

    timer.start();
    EXPECT_GT(timer.begin_us, 0u);
    EXPECT_EQ(timer.end_us, 0u);

    Time::sleep(1'000);
    EXPECT_GE(timer.elapsed(), 0.0);

    timer.stop();
    EXPECT_GT(timer.end_us, 0u);
    const double stopped_elapsed = timer.elapsed();
    EXPECT_GE(stopped_elapsed, 0.0);
    EXPECT_EQ(timer.elapsed(), stopped_elapsed);

    timer.reset();
    EXPECT_EQ(timer.begin_us, 0u);
    EXPECT_EQ(timer.end_us, 0u);
}
