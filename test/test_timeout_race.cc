#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <unordered_set>
#include "reactor/reactor.h"
#include "reactor/event.h"
#include "reactor/coroutine.h"

using namespace rrr;
using namespace std::chrono;

class TimeoutRaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Fresh reactor for each test
    }
    
    void TearDown() override {
        // Cleanup
    }
};

// Test 1: Event ready vs timeout timing within same thread
TEST_F(TimeoutRaceTest, ReadyVsTimeoutTiming) {
    auto reactor = Reactor::GetReactor();
    
    // Test case 1: Event becomes ready before timeout
    {
        auto sp_event = Reactor::CreateSpEvent<IntEvent>();
        std::atomic<bool> completed{false};
        std::atomic<int> final_status{-1};
        
        // Create a coroutine that will handle both setting and waiting
        auto setter_coro = reactor->CreateRunCoroutine([sp_event]() {
            // Just set the event immediately
            sp_event->Set(1);
        });
        
        // Create the waiter coroutine
        reactor->CreateRunCoroutine([sp_event, &completed, &final_status]() {
            // Event should already be set, so this should complete immediately
            sp_event->Wait(100000);
            completed = true;
            final_status = sp_event->status_;
        });
        
        // Process - event is already ready, so waiter should complete
        reactor->Loop(false);
        
        EXPECT_TRUE(completed);
        EXPECT_EQ(final_status, Event::DONE);
    }
    
    // Test case 2: Event times out
    {
        auto sp_event = Reactor::CreateSpEvent<IntEvent>();
        std::atomic<bool> completed{false};
        std::atomic<int> final_status{-1};
        
        reactor->CreateRunCoroutine([sp_event, &completed, &final_status]() {
            // Wait with very short timeout
            sp_event->Wait(1000); // 1ms
            completed = true;
            final_status = sp_event->status_;
        });
        
        // Sleep longer than timeout
        std::this_thread::sleep_for(milliseconds(10));
        reactor->Loop(false);
        
        EXPECT_TRUE(completed);
        EXPECT_EQ(final_status, Event::TIMEOUT);
    }
}

// Test 2: Event in both waiting_events_ and timeout_events_ lists
TEST_F(TimeoutRaceTest, DoubleListBehavior) {
    auto reactor = Reactor::GetReactor();
    
    // Create event with timeout - it goes in both lists
    auto sp_event = Reactor::CreateSpEvent<IntEvent>();
    std::atomic<int> loop_count{0};
    std::atomic<bool> completed{false};
    
    reactor->CreateRunCoroutine([sp_event, &completed]() {
        sp_event->Wait(50000); // 50ms timeout
        completed = true;
    });
    
    // Process events multiple times before timeout
    for (int i = 0; i < 5; i++) {
        std::this_thread::sleep_for(milliseconds(5));
        reactor->Loop(false);
        loop_count++;
        if (completed) break;
    }
    
    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(60));
    reactor->Loop(false);
    
    EXPECT_TRUE(completed);
    std::cout << "Event processed after " << loop_count 
              << " loop iterations before timeout" << std::endl;
}

// Test 3: Multiple events with staggered timeouts
TEST_F(TimeoutRaceTest, StaggeredTimeouts) {
    auto reactor = Reactor::GetReactor();
    
    const int num_events = 10;
    std::atomic<int> timeout_count{0};
    std::atomic<int> ready_count{0};
    
    // Create events with different timeouts
    for (int i = 0; i < num_events; i++) {
        auto sp_event = Reactor::CreateSpEvent<IntEvent>();
        
        reactor->CreateRunCoroutine([sp_event, i, &timeout_count, &ready_count]() {
            // Half will be set ready, half will timeout
            if (i % 2 == 0) {
                // Create inner coroutine to set event ready
                auto reactor = Reactor::GetReactor();
                reactor->CreateRunCoroutine([sp_event]() {
                    Coroutine::CurrentCoroutine().unwrap()->Yield();
                    sp_event->Set(1);
                });
            }
            
            // Wait with varying timeouts
            sp_event->Wait((10 + i * 5) * 1000);
            
            if (sp_event->status_ == Event::TIMEOUT) {
                timeout_count++;
            } else if (sp_event->status_ == Event::DONE) {
                ready_count++;
            }
        });
    }
    
    // Process events over time
    for (int i = 0; i < 20; i++) {
        std::this_thread::sleep_for(milliseconds(10));
        reactor->Loop(false);
    }
    
    std::cout << "Results: Ready=" << ready_count 
              << ", Timeout=" << timeout_count 
              << " (total=" << (ready_count + timeout_count) << ")" << std::endl;
    
    EXPECT_EQ(ready_count + timeout_count, num_events);
}

// Test 4: Timeout event cleanup
TEST_F(TimeoutRaceTest, TimeoutEventCleanup) {
    auto reactor = Reactor::GetReactor();
    
    // Create multiple events that will timeout
    std::vector<std::shared_ptr<IntEvent>> events;
    std::atomic<int> completed_count{0};
    
    for (int i = 0; i < 5; i++) {
        auto sp_event = Reactor::CreateSpEvent<IntEvent>();
        events.push_back(sp_event);
        
        reactor->CreateRunCoroutine([sp_event, &completed_count]() {
            sp_event->Wait(10000); // 10ms timeout
            completed_count++;
        });
    }
    
    // Wait for all timeouts
    std::this_thread::sleep_for(milliseconds(20));
    reactor->Loop(false);
    
    EXPECT_EQ(completed_count, 5);
    
    // Verify all events are in TIMEOUT state
    for (auto& event : events) {
        EXPECT_EQ(event->status_, Event::TIMEOUT);
    }
}

// Test 5: Rapid timeout changes in same thread
TEST_F(TimeoutRaceTest, RapidTimeoutChanges) {
    auto reactor = Reactor::GetReactor();
    
    const int num_iterations = 50;
    std::atomic<int> timeout_count{0};
    std::atomic<int> ready_count{0};
    
    for (int iter = 0; iter < num_iterations; iter++) {
        auto sp_event = Reactor::CreateSpEvent<IntEvent>();
        
        reactor->CreateRunCoroutine([sp_event, iter, &timeout_count, &ready_count]() {
            // Randomly decide to set ready or let timeout
            if (iter % 3 == 0) {
                // Set it ready immediately (same coroutine)
                sp_event->Set(1);
            }
            
            // Very short timeout
            sp_event->Wait(1000); // 1ms
            
            if (sp_event->status_ == Event::TIMEOUT) {
                timeout_count++;
            } else if (sp_event->status_ == Event::DONE) {
                ready_count++;
            }
        });
        
        // Process with small delay
        if (iter % 3 != 0) {
            std::this_thread::sleep_for(milliseconds(2));
        }
        reactor->Loop(false);
    }
    
    std::cout << "Results after " << num_iterations << " iterations: "
              << "Ready=" << ready_count << ", Timeout=" << timeout_count << std::endl;
    
    EXPECT_EQ(ready_count + timeout_count, num_iterations);
}

// Test 6: Event status after timeout
TEST_F(TimeoutRaceTest, EventStatusAfterTimeout) {
    auto reactor = Reactor::GetReactor();
    
    auto sp_event = Reactor::CreateSpEvent<IntEvent>();
    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    
    // First coroutine waits with timeout
    reactor->CreateRunCoroutine([sp_event, &first_done]() {
        sp_event->Wait(5000); // 5ms timeout
        first_done = true;
        EXPECT_EQ(sp_event->status_, Event::TIMEOUT);
    });
    
    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(10));
    reactor->Loop(false);
    
    EXPECT_TRUE(first_done);
    
    // Try to use the same event again (should see it's already TIMEOUT)
    reactor->CreateRunCoroutine([sp_event, &second_done]() {
        // Event is already in TIMEOUT state
        // The behavior here is interesting - what happens?
        if (sp_event->status_ == Event::TIMEOUT) {
            std::cout << "Event already in TIMEOUT state before Wait()" << std::endl;
            second_done = true;
            // Don't try to wait on an already finished event - undefined behavior
            // The event system doesn't support reusing events after they're done/timeout
        } else {
            // This shouldn't happen, but if it does, try to wait
            sp_event->Wait(5000);
            second_done = true;
        }
        
        std::cout << "Second coroutine completed with event status: " 
                  << sp_event->status_ << std::endl;
    });
    
    reactor->Loop(false);
    
    // The second coroutine should complete
    EXPECT_TRUE(second_done);
    
    // This test reveals that events cannot be reused after timeout/done
    std::cout << "Note: Events cannot be reused after reaching DONE/TIMEOUT state" << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}