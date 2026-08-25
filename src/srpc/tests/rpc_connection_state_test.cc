/**
 * Unit tests for ConnectionStateMachine
 * Tests state transitions, callbacks, and thread-safe state access.
 */

#include <gtest/gtest.h>
#include "../srpc.hpp"

import std;

using namespace srpc;

// ============================================================================
// Basic State Tests
// ============================================================================

TEST(ConnectionStateTest, InitialState) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_EQ(sm.state(), ConnectionState::NEW);
    EXPECT_FALSE(sm.is_connected());
    EXPECT_FALSE(sm.is_terminal());
    EXPECT_TRUE(sm.can_connect());
}

TEST(ConnectionStateTest, StateToString) {
    EXPECT_EQ(connection_state_to_string(ConnectionState::NEW), "NEW");
    EXPECT_EQ(connection_state_to_string(ConnectionState::CONNECTING), "CONNECTING");
    EXPECT_EQ(connection_state_to_string(ConnectionState::CONNECTED), "CONNECTED");
    EXPECT_EQ(connection_state_to_string(ConnectionState::DISCONNECTING), "DISCONNECTING");
    EXPECT_EQ(connection_state_to_string(ConnectionState::DISCONNECTED), "DISCONNECTED");
    EXPECT_EQ(connection_state_to_string(ConnectionState::FAILED), "FAILED");
}

// ============================================================================
// Valid State Transition Tests
// ============================================================================

TEST(ConnectionStateTest, NewToConnecting) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_TRUE(sm.transition_to(ConnectionState::CONNECTING));
    EXPECT_EQ(sm.state(), ConnectionState::CONNECTING);
    EXPECT_FALSE(sm.is_connected());
    EXPECT_FALSE(sm.can_connect());
}

TEST(ConnectionStateTest, ConnectingToConnected) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_TRUE(sm.transition_to(ConnectionState::CONNECTED));
    EXPECT_EQ(sm.state(), ConnectionState::CONNECTED);
    EXPECT_TRUE(sm.is_connected());
}

TEST(ConnectionStateTest, ConnectedToDisconnecting) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_TRUE(sm.transition_to(ConnectionState::DISCONNECTING));
    EXPECT_EQ(sm.state(), ConnectionState::DISCONNECTING);
    EXPECT_FALSE(sm.is_connected());
}

TEST(ConnectionStateTest, DisconnectingToDisconnected) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);
    sm.transition_to(ConnectionState::DISCONNECTING);
    EXPECT_TRUE(sm.transition_to(ConnectionState::DISCONNECTED));
    EXPECT_EQ(sm.state(), ConnectionState::DISCONNECTED);
    EXPECT_TRUE(sm.is_terminal());
}

TEST(ConnectionStateTest, ConnectingToFailed) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_TRUE(sm.transition_to(ConnectionState::FAILED));
    EXPECT_EQ(sm.state(), ConnectionState::FAILED);
    EXPECT_TRUE(sm.is_terminal());
}

TEST(ConnectionStateTest, ConnectedToFailed) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_TRUE(sm.transition_to(ConnectionState::FAILED));
    EXPECT_EQ(sm.state(), ConnectionState::FAILED);
}

TEST(ConnectionStateTest, DisconnectedToConnecting) {
    // From DISCONNECTED, you CAN transition to CONNECTING (reconnect)
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);
    sm.transition_to(ConnectionState::DISCONNECTING);
    sm.transition_to(ConnectionState::DISCONNECTED);

    EXPECT_TRUE(sm.transition_to(ConnectionState::CONNECTING));
    EXPECT_EQ(sm.state(), ConnectionState::CONNECTING);
}

TEST(ConnectionStateTest, FailedToConnecting) {
    // From FAILED, you CAN transition to CONNECTING (reconnect attempt)
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::FAILED);

    EXPECT_TRUE(sm.transition_to(ConnectionState::CONNECTING));
    EXPECT_EQ(sm.state(), ConnectionState::CONNECTING);
}

// ============================================================================
// Invalid State Transition Tests
// ============================================================================

TEST(ConnectionStateTest, NewToFailed_Invalid) {
    // From NEW, you can only go to CONNECTING, not directly to FAILED
    auto sm = ConnectionStateMachine::new_();
    EXPECT_FALSE(sm.transition_to(ConnectionState::FAILED));
    EXPECT_EQ(sm.state(), ConnectionState::NEW);  // Unchanged
}

TEST(ConnectionStateTest, NewToConnected_Invalid) {
    // Cannot skip CONNECTING state
    auto sm = ConnectionStateMachine::new_();
    EXPECT_FALSE(sm.transition_to(ConnectionState::CONNECTED));
    EXPECT_EQ(sm.state(), ConnectionState::NEW);
}

TEST(ConnectionStateTest, ConnectedToNew_Invalid) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_FALSE(sm.transition_to(ConnectionState::NEW));
    EXPECT_EQ(sm.state(), ConnectionState::CONNECTED);  // Unchanged
}

TEST(ConnectionStateTest, FailedToConnected_Invalid) {
    // From FAILED, must go through CONNECTING first
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::FAILED);
    EXPECT_FALSE(sm.transition_to(ConnectionState::CONNECTED));
    EXPECT_EQ(sm.state(), ConnectionState::FAILED);
}

TEST(ConnectionStateTest, SameStateTransition) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_FALSE(sm.transition_to(ConnectionState::NEW));  // Already NEW
    EXPECT_EQ(sm.state(), ConnectionState::NEW);
}

// ============================================================================
// Force State Tests
// ============================================================================

TEST(ConnectionStateTest, ForceStateBypassesValidation) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);

    // Force back to NEW (normally invalid)
    sm.force_state(ConnectionState::NEW);
    EXPECT_EQ(sm.state(), ConnectionState::NEW);
    EXPECT_TRUE(sm.can_connect());
}

TEST(ConnectionStateTest, ForceStateToFailed) {
    // Force to FAILED from any state
    auto sm = ConnectionStateMachine::new_();
    sm.force_state(ConnectionState::FAILED);
    EXPECT_EQ(sm.state(), ConnectionState::FAILED);
    EXPECT_TRUE(sm.is_terminal());
    EXPECT_TRUE(sm.can_connect());  // Can reconnect from FAILED
}

TEST(ConnectionStateTest, ForceStateFromTerminal) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::FAILED);
    EXPECT_TRUE(sm.is_terminal());

    // Force back to NEW for reconnection
    sm.force_state(ConnectionState::NEW);
    EXPECT_FALSE(sm.is_terminal());
    EXPECT_TRUE(sm.can_connect());
}

// ============================================================================
// State Query Tests
// ============================================================================

TEST(ConnectionStateTest, IsConnectedOnlyInConnectedState) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_FALSE(sm.is_connected());  // NEW

    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_FALSE(sm.is_connected());

    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_TRUE(sm.is_connected());

    sm.transition_to(ConnectionState::DISCONNECTING);
    EXPECT_FALSE(sm.is_connected());
}

TEST(ConnectionStateTest, IsTerminalOnlyInTerminalStates) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_FALSE(sm.is_terminal());  // NEW

    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_FALSE(sm.is_terminal());

    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_FALSE(sm.is_terminal());

    sm.transition_to(ConnectionState::DISCONNECTING);
    EXPECT_FALSE(sm.is_terminal());

    sm.transition_to(ConnectionState::DISCONNECTED);
    EXPECT_TRUE(sm.is_terminal());
}

TEST(ConnectionStateTest, IsFailedTerminal) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::FAILED);
    EXPECT_TRUE(sm.is_terminal());
    EXPECT_TRUE(sm.is_failed());
}

TEST(ConnectionStateTest, CanConnectFromReconnectableStates) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_TRUE(sm.can_connect());  // NEW

    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_FALSE(sm.can_connect());  // Already connecting

    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_FALSE(sm.can_connect());  // Already connected

    sm.transition_to(ConnectionState::DISCONNECTING);
    EXPECT_FALSE(sm.can_connect());  // Disconnecting

    sm.transition_to(ConnectionState::DISCONNECTED);
    EXPECT_TRUE(sm.can_connect());  // Can reconnect from DISCONNECTED
}

TEST(ConnectionStateTest, CanConnectFromFailed) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::FAILED);
    EXPECT_TRUE(sm.can_connect());  // Can reconnect from FAILED
}

TEST(ConnectionStateTest, IsUsable) {
    auto sm = ConnectionStateMachine::new_();
    EXPECT_FALSE(sm.is_usable());  // NEW

    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_TRUE(sm.is_usable());  // CONNECTING is usable

    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_TRUE(sm.is_usable());  // CONNECTED is usable

    sm.transition_to(ConnectionState::DISCONNECTING);
    EXPECT_FALSE(sm.is_usable());  // DISCONNECTING not usable
}

// ============================================================================
// Callback Tests
// ============================================================================

TEST(ConnectionStateTest, CallbackOnStateChange) {
    auto sm = ConnectionStateMachine::new_();

    std::atomic<int> callback_count{0};
    ConnectionState last_from = ConnectionState::NEW;
    ConnectionState last_to = ConnectionState::NEW;

    sm.set_on_state_change([&](ConnectionState from, ConnectionState to) {
        callback_count++;
        last_from = from;
        last_to = to;
    });

    sm.transition_to(ConnectionState::CONNECTING);
    EXPECT_EQ(callback_count.load(), 1);
    EXPECT_EQ(last_from, ConnectionState::NEW);
    EXPECT_EQ(last_to, ConnectionState::CONNECTING);

    sm.transition_to(ConnectionState::CONNECTED);
    EXPECT_EQ(callback_count.load(), 2);
    EXPECT_EQ(last_from, ConnectionState::CONNECTING);
    EXPECT_EQ(last_to, ConnectionState::CONNECTED);
}

TEST(ConnectionStateTest, NoCallbackOnInvalidTransition) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);

    std::atomic<int> callback_count{0};
    sm.set_on_state_change([&](ConnectionState, ConnectionState) {
        callback_count++;
    });

    // This should fail and not trigger callback
    sm.transition_to(ConnectionState::NEW);
    EXPECT_EQ(callback_count.load(), 0);
}

TEST(ConnectionStateTest, CallbackOnForceState) {
    auto sm = ConnectionStateMachine::new_();

    std::atomic<int> callback_count{0};
    sm.set_on_state_change([&](ConnectionState, ConnectionState) {
        callback_count++;
    });

    sm.force_state(ConnectionState::CONNECTED);
    EXPECT_EQ(callback_count.load(), 1);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST(ConnectionStateTest, ConcurrentReads) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);

    std::atomic<int> read_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 100; j++) {
                auto state = sm.state();
                EXPECT_EQ(state, ConnectionState::CONNECTED);
                read_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(read_count.load(), 1000);
}

TEST(ConnectionStateTest, ConcurrentStateQueries) {
    auto sm = ConnectionStateMachine::new_();
    sm.transition_to(ConnectionState::CONNECTING);
    sm.transition_to(ConnectionState::CONNECTED);

    std::vector<std::thread> threads;
    std::atomic<bool> all_ok{true};

    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 100; j++) {
                if (!sm.is_connected()) {
                    all_ok = false;
                }
                if (sm.is_terminal()) {
                    all_ok = false;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(all_ok.load());
}

// ============================================================================
// Can Transition Tests
// ============================================================================

TEST(ConnectionStateTest, CanTransitionTo) {
    auto sm = ConnectionStateMachine::new_();

    // From NEW
    EXPECT_TRUE(sm.can_transition_to(ConnectionState::CONNECTING));
    EXPECT_FALSE(sm.can_transition_to(ConnectionState::CONNECTED));
    EXPECT_FALSE(sm.can_transition_to(ConnectionState::FAILED));

    sm.transition_to(ConnectionState::CONNECTING);

    // From CONNECTING
    EXPECT_TRUE(sm.can_transition_to(ConnectionState::CONNECTED));
    EXPECT_TRUE(sm.can_transition_to(ConnectionState::FAILED));
    EXPECT_TRUE(sm.can_transition_to(ConnectionState::DISCONNECTED));
    EXPECT_FALSE(sm.can_transition_to(ConnectionState::NEW));
}
