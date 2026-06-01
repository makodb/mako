/**
 * Unit tests for RPC callback system.
 * Tests CallbackManager for connection lifecycle events.
 */

#include <stdlib.h>

#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;

using namespace rrr;

// ============================================================================
// Basic Registration Tests
// ============================================================================

TEST(CallbackManagerTest, InitiallyEmpty) {
    auto mgr = CallbackManager::new_();
    EXPECT_EQ(mgr.callback_count(), 0u);
    EXPECT_FALSE(mgr.has_callbacks());
}

TEST(CallbackManagerTest, AddOnConnected) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_connected([]() {});
    EXPECT_EQ(mgr.on_connected_count(), 1u);
    EXPECT_EQ(mgr.callback_count(), 1u);
    EXPECT_TRUE(mgr.has_callbacks());
}

TEST(CallbackManagerTest, AddOnDisconnected) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_disconnected([]() {});
    EXPECT_EQ(mgr.on_disconnected_count(), 1u);
    EXPECT_EQ(mgr.callback_count(), 1u);
}

TEST(CallbackManagerTest, AddOnError) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_error([](RpcError, const std::string&) {});
    EXPECT_EQ(mgr.on_error_count(), 1u);
    EXPECT_EQ(mgr.callback_count(), 1u);
}

TEST(CallbackManagerTest, AddOnReconnecting) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_reconnecting([]() {});
    EXPECT_EQ(mgr.on_reconnecting_count(), 1u);
    EXPECT_EQ(mgr.callback_count(), 1u);
}

TEST(CallbackManagerTest, AddOnReconnected) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_reconnected([](bool) {});
    EXPECT_EQ(mgr.on_reconnected_count(), 1u);
    EXPECT_EQ(mgr.callback_count(), 1u);
}

TEST(CallbackManagerTest, MultipleCallbacksPerEvent) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_connected([]() {});
    mgr.add_on_connected([]() {});
    mgr.add_on_connected([]() {});
    EXPECT_EQ(mgr.on_connected_count(), 3u);
    EXPECT_EQ(mgr.callback_count(), 3u);
}

TEST(CallbackManagerTest, MultipleEventTypes) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_connected([]() {});
    mgr.add_on_disconnected([]() {});
    mgr.add_on_error([](RpcError, const std::string&) {});
    mgr.add_on_reconnecting([]() {});
    mgr.add_on_reconnected([](bool) {});
    EXPECT_EQ(mgr.callback_count(), 5u);
}

// ============================================================================
// Invocation Tests
// ============================================================================

TEST(CallbackManagerTest, InvokeOnConnected) {
    auto mgr = CallbackManager::new_();
    int call_count = 0;
    mgr.add_on_connected([&call_count]() { call_count++; });

    EXPECT_EQ(call_count, 0);
    mgr.invoke_on_connected();
    EXPECT_EQ(call_count, 1);
    mgr.invoke_on_connected();
    EXPECT_EQ(call_count, 2);
}

TEST(CallbackManagerTest, InvokeOnDisconnected) {
    auto mgr = CallbackManager::new_();
    int call_count = 0;
    mgr.add_on_disconnected([&call_count]() { call_count++; });

    mgr.invoke_on_disconnected();
    EXPECT_EQ(call_count, 1);
}

TEST(CallbackManagerTest, InvokeOnErrorWithParams) {
    auto mgr = CallbackManager::new_();
    RpcError received_error = RpcError::OK;
    std::string received_message;

    mgr.add_on_error([&](RpcError error, const std::string& msg) {
        received_error = error;
        received_message = msg;
    });

    mgr.invoke_on_error(RpcError::CONNECTION_RESET, "Connection lost");
    EXPECT_EQ(received_error, RpcError::CONNECTION_RESET);
    EXPECT_EQ(received_message, "Connection lost");
}

TEST(CallbackManagerTest, InvokeOnReconnecting) {
    auto mgr = CallbackManager::new_();
    int call_count = 0;
    mgr.add_on_reconnecting([&call_count]() { call_count++; });

    mgr.invoke_on_reconnecting();
    EXPECT_EQ(call_count, 1);
}

TEST(CallbackManagerTest, InvokeOnReconnectedSuccess) {
    auto mgr = CallbackManager::new_();
    bool received_success = false;

    mgr.add_on_reconnected([&received_success](bool success) {
        received_success = success;
    });

    mgr.invoke_on_reconnected(true);
    EXPECT_TRUE(received_success);
}

TEST(CallbackManagerTest, InvokeOnReconnectedFailure) {
    auto mgr = CallbackManager::new_();
    bool received_success = true;

    mgr.add_on_reconnected([&received_success](bool success) {
        received_success = success;
    });

    mgr.invoke_on_reconnected(false);
    EXPECT_FALSE(received_success);
}

TEST(CallbackManagerTest, InvokeMultipleCallbacks) {
    auto mgr = CallbackManager::new_();
    std::vector<int> call_order;

    mgr.add_on_connected([&call_order]() { call_order.push_back(1); });
    mgr.add_on_connected([&call_order]() { call_order.push_back(2); });
    mgr.add_on_connected([&call_order]() { call_order.push_back(3); });

    mgr.invoke_on_connected();

    ASSERT_EQ(call_order.size(), 3u);
    EXPECT_EQ(call_order[0], 1);
    EXPECT_EQ(call_order[1], 2);
    EXPECT_EQ(call_order[2], 3);
}

// ============================================================================
// Exception Safety Tests
// ============================================================================

TEST(CallbackManagerTest, ExceptionInCallbackDoesNotPropagate) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_connected([]() { throw std::runtime_error("Test error"); });

    // Should not throw
    EXPECT_NO_THROW(mgr.invoke_on_connected());
}

TEST(CallbackManagerTest, ExceptionInOneCallbackDoesNotAffectOthers) {
    auto mgr = CallbackManager::new_();
    int call_count = 0;

    mgr.add_on_connected([&call_count]() { call_count++; });
    mgr.add_on_connected([]() { throw std::runtime_error("Test error"); });
    mgr.add_on_connected([&call_count]() { call_count++; });

    mgr.invoke_on_connected();
    EXPECT_EQ(call_count, 2);  // Both non-throwing callbacks should be called
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST(CallbackManagerTest, ClearAll) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_connected([]() {});
    mgr.add_on_disconnected([]() {});
    mgr.add_on_error([](RpcError, const std::string&) {});
    mgr.add_on_reconnecting([]() {});
    mgr.add_on_reconnected([](bool) {});

    EXPECT_EQ(mgr.callback_count(), 5u);

    mgr.clear_all();
    EXPECT_EQ(mgr.callback_count(), 0u);
    EXPECT_FALSE(mgr.has_callbacks());
}

TEST(CallbackManagerTest, ClearAllClearsAllTypes) {
    auto mgr = CallbackManager::new_();
    mgr.add_on_connected([]() {});
    mgr.add_on_connected([]() {});
    mgr.add_on_error([](RpcError, const std::string&) {});

    mgr.clear_all();

    EXPECT_EQ(mgr.on_connected_count(), 0u);
    EXPECT_EQ(mgr.on_error_count(), 0u);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST(CallbackManagerTest, ConcurrentRegistration) {
    auto mgr = CallbackManager::new_();
    std::atomic<int> total_registered{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&mgr, &total_registered]() {
            for (int j = 0; j < 100; j++) {
                mgr.add_on_connected([]() {});
                total_registered++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(total_registered, 1000);
    EXPECT_EQ(mgr.on_connected_count(), 1000u);
}

TEST(CallbackManagerTest, ConcurrentInvocation) {
    auto mgr = CallbackManager::new_();
    std::atomic<int> call_count{0};

    mgr.add_on_connected([&call_count]() { call_count++; });

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&mgr]() {
            for (int j = 0; j < 100; j++) {
                mgr.invoke_on_connected();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(call_count, 1000);
}

TEST(CallbackManagerTest, RegistrationDuringInvocation) {
    auto mgr = CallbackManager::new_();
    std::atomic<int> call_count{0};

    mgr.add_on_connected([&mgr, &call_count]() {
        call_count++;
        // Try to add a new callback during invocation
        // This should not deadlock (callbacks are copied before invocation)
        mgr.add_on_connected([]() {});
    });

    mgr.invoke_on_connected();
    EXPECT_EQ(call_count, 1);
    // New callback was added during invocation
    EXPECT_EQ(mgr.on_connected_count(), 2u);
}

// ============================================================================
// Integration-style Tests
// ============================================================================

TEST(CallbackManagerTest, TypicalUsagePattern) {
    auto mgr = CallbackManager::new_();
    std::vector<std::string> events;

    mgr.add_on_connected([&events]() { events.push_back("connected"); });
    mgr.add_on_disconnected([&events]() { events.push_back("disconnected"); });
    mgr.add_on_error([&events](RpcError, const std::string&) {
        events.push_back("error");
    });
    mgr.add_on_reconnecting([&events]() { events.push_back("reconnecting"); });
    mgr.add_on_reconnected([&events](bool success) {
        events.push_back(success ? "reconnected_success" : "reconnected_failure");
    });

    // Simulate connection lifecycle
    mgr.invoke_on_connected();
    mgr.invoke_on_error(RpcError::CONNECTION_RESET, "Lost connection");
    mgr.invoke_on_disconnected();
    mgr.invoke_on_reconnecting();
    mgr.invoke_on_reconnected(true);

    ASSERT_EQ(events.size(), 5u);
    EXPECT_EQ(events[0], "connected");
    EXPECT_EQ(events[1], "error");
    EXPECT_EQ(events[2], "disconnected");
    EXPECT_EQ(events[3], "reconnecting");
    EXPECT_EQ(events[4], "reconnected_success");
}

TEST(CallbackManagerTest, NoCallbacksInvocationIsSafe) {
    auto mgr = CallbackManager::new_();

    // Invoking without any registered callbacks should be safe
    EXPECT_NO_THROW(mgr.invoke_on_connected());
    EXPECT_NO_THROW(mgr.invoke_on_disconnected());
    EXPECT_NO_THROW(mgr.invoke_on_error(RpcError::OK, ""));
    EXPECT_NO_THROW(mgr.invoke_on_reconnecting());
    EXPECT_NO_THROW(mgr.invoke_on_reconnected(true));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
