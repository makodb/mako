/**
 * Unit tests for Graceful Server Shutdown
 *
 * Tests the shutdown phase tracking, hooks, request tracking,
 * drain functionality, and full graceful shutdown sequence.
 */
#include <rusty/option.hpp>

#include "gtest/gtest.h"
#include "../rrr.hpp"

import std;

namespace rrr {

class GracefulShutdownTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ========== ShutdownPhase Tests ==========

TEST_F(GracefulShutdownTest, ShutdownPhaseToString) {
    EXPECT_STREQ("RUNNING", shutdown_phase_to_string(ShutdownPhase::RUNNING));
    EXPECT_STREQ("STOP_ACCEPTING", shutdown_phase_to_string(ShutdownPhase::STOP_ACCEPTING));
    EXPECT_STREQ("DRAINING", shutdown_phase_to_string(ShutdownPhase::DRAINING));
    EXPECT_STREQ("CLOSING", shutdown_phase_to_string(ShutdownPhase::CLOSING));
    EXPECT_STREQ("STOPPED", shutdown_phase_to_string(ShutdownPhase::STOPPED));
}

TEST_F(GracefulShutdownTest, InitialPhaseIsRunning) {
    auto server = Server::new_(rusty::None);
    EXPECT_EQ(ShutdownPhase::RUNNING, server.phase());
}

TEST_F(GracefulShutdownTest, InitialPendingRequestCountIsZero) {
    auto server = Server::new_(rusty::None);
    EXPECT_EQ(0, server.pending_request_count());
}

// ========== Request Tracking Tests ==========

TEST_F(GracefulShutdownTest, IncrementPendingRequests) {
    auto server = Server::new_(rusty::None);
    EXPECT_EQ(0, server.pending_request_count());

    server.increment_pending();
    EXPECT_EQ(1, server.pending_request_count());

    server.increment_pending();
    EXPECT_EQ(2, server.pending_request_count());
}

TEST_F(GracefulShutdownTest, DecrementPendingRequests) {
    auto server = Server::new_(rusty::None);
    server.increment_pending();
    server.increment_pending();
    EXPECT_EQ(2, server.pending_request_count());

    server.decrement_pending();
    EXPECT_EQ(1, server.pending_request_count());

    server.decrement_pending();
    EXPECT_EQ(0, server.pending_request_count());
}

TEST_F(GracefulShutdownTest, ConcurrentRequestTracking) {
    auto server = Server::new_(rusty::None);
    const int num_threads = 10;
    const int increments_per_thread = 100;
    std::vector<std::thread> threads;

    // Spawn threads that increment
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&server, increments_per_thread]() {
            for (int j = 0; j < increments_per_thread; ++j) {
                server.increment_pending();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(num_threads * increments_per_thread, server.pending_request_count());

    // Now decrement all
    threads.clear();
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&server, increments_per_thread]() {
            for (int j = 0; j < increments_per_thread; ++j) {
                server.decrement_pending();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(0, server.pending_request_count());
}

// ========== Shutdown Hook Tests ==========

TEST_F(GracefulShutdownTest, ShutdownHookCalled) {
    auto server = Server::new_(rusty::None);
    bool hook_called = false;

    server.add_shutdown_hook([&hook_called]() {
        hook_called = true;
    });

    // Graceful shutdown should call the hook
    server.graceful_shutdown(100);  // Short timeout

    EXPECT_TRUE(hook_called);
}

TEST_F(GracefulShutdownTest, MultipleHooksCalledInOrder) {
    auto server = Server::new_(rusty::None);
    std::vector<int> call_order;

    server.add_shutdown_hook([&call_order]() {
        call_order.push_back(1);
    });
    server.add_shutdown_hook([&call_order]() {
        call_order.push_back(2);
    });
    server.add_shutdown_hook([&call_order]() {
        call_order.push_back(3);
    });

    server.graceful_shutdown(100);

    ASSERT_EQ(3u, call_order.size());
    EXPECT_EQ(1, call_order[0]);
    EXPECT_EQ(2, call_order[1]);
    EXPECT_EQ(3, call_order[2]);
}

TEST_F(GracefulShutdownTest, HookExceptionDoesNotStopOthers) {
    auto server = Server::new_(rusty::None);
    std::vector<int> call_order;

    server.add_shutdown_hook([&call_order]() {
        call_order.push_back(1);
    });
    server.add_shutdown_hook([]() {
        throw std::runtime_error("Hook error");
    });
    server.add_shutdown_hook([&call_order]() {
        call_order.push_back(3);
    });

    // Should not throw, and all hooks should be attempted
    EXPECT_NO_THROW(server.graceful_shutdown(100));

    ASSERT_EQ(2u, call_order.size());
    EXPECT_EQ(1, call_order[0]);
    EXPECT_EQ(3, call_order[1]);
}

// ========== Stop Accepting Tests ==========

TEST_F(GracefulShutdownTest, StopAcceptingTransitionsPhase) {
    auto server = Server::new_(rusty::None);
    EXPECT_EQ(ShutdownPhase::RUNNING, server.phase());

    server.stop_accepting();
    EXPECT_EQ(ShutdownPhase::STOP_ACCEPTING, server.phase());
}

TEST_F(GracefulShutdownTest, StopAcceptingIdempotent) {
    auto server = Server::new_(rusty::None);
    server.stop_accepting();
    EXPECT_EQ(ShutdownPhase::STOP_ACCEPTING, server.phase());

    // Second call should be a no-op
    server.stop_accepting();
    EXPECT_EQ(ShutdownPhase::STOP_ACCEPTING, server.phase());
}

// ========== Drain Tests ==========

TEST_F(GracefulShutdownTest, DrainImmediateWhenNoPendingRequests) {
    auto server = Server::new_(rusty::None);
    server.stop_accepting();

    auto start = std::chrono::steady_clock::now();
    bool result = server.drain(1000);  // 1 second timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result);
    EXPECT_LT(elapsed, std::chrono::milliseconds(100));  // Should be immediate
    EXPECT_EQ(ShutdownPhase::DRAINING, server.phase());
}

TEST_F(GracefulShutdownTest, DrainWaitsForPendingRequests) {
    auto server = Server::new_(rusty::None);
    server.stop_accepting();
    server.increment_pending();

    // Start a thread that will decrement after a delay
    std::thread worker([&server]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.decrement_pending();
    });

    auto start = std::chrono::steady_clock::now();
    bool result = server.drain(1000);  // 1 second timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    worker.join();

    EXPECT_TRUE(result);
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));  // Should have waited
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));  // But not too long
}

TEST_F(GracefulShutdownTest, DrainTimesOut) {
    auto server = Server::new_(rusty::None);
    server.stop_accepting();
    server.increment_pending();  // This won't be decremented

    auto start = std::chrono::steady_clock::now();
    bool result = server.drain(100);  // 100ms timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result);  // Should timeout
    EXPECT_GE(elapsed, std::chrono::milliseconds(90));  // Should have waited
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));

    // Cleanup
    server.decrement_pending();
}

// ========== Graceful Shutdown Tests ==========

TEST_F(GracefulShutdownTest, GracefulShutdownTransitionsAllPhases) {
    auto server = Server::new_(rusty::None);
    std::vector<ShutdownPhase> observed_phases;

    // Track phase transitions via hooks
    server.add_shutdown_hook([&server, &observed_phases]() {
        observed_phases.push_back(server.phase());
    });

    EXPECT_EQ(ShutdownPhase::RUNNING, server.phase());

    server.graceful_shutdown(100);

    // During hook execution, phase should be CLOSING
    ASSERT_EQ(1u, observed_phases.size());
    EXPECT_EQ(ShutdownPhase::CLOSING, observed_phases[0]);

    // Final phase should be STOPPED
    EXPECT_EQ(ShutdownPhase::STOPPED, server.phase());
}

TEST_F(GracefulShutdownTest, GracefulShutdownDrainsRequests) {
    auto server = Server::new_(rusty::None);
    server.increment_pending();
    server.increment_pending();

    std::thread worker([&server]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        server.decrement_pending();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        server.decrement_pending();
    });

    auto start = std::chrono::steady_clock::now();
    server.graceful_shutdown(1000);  // 1 second timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    worker.join();

    EXPECT_EQ(ShutdownPhase::STOPPED, server.phase());
    EXPECT_EQ(0, server.pending_request_count());
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));  // Should have waited
}

TEST_F(GracefulShutdownTest, GracefulShutdownProceedsOnDrainTimeout) {
    auto server = Server::new_(rusty::None);
    server.increment_pending();  // Won't be decremented

    auto start = std::chrono::steady_clock::now();
    server.graceful_shutdown(50);  // Short timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Should complete despite timeout
    EXPECT_EQ(ShutdownPhase::STOPPED, server.phase());
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));

    // Cleanup
    server.decrement_pending();
}

} // namespace rrr
