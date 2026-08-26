#include <stdlib.h>

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include "../rrr.hpp"

import std;
import rusty;

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
    auto reactor = Reactor::get_reactor();
    
    // Test case 1: Event becomes ready before timeout
    {
        auto sp_event = create_sp_int_event(1);
        std::atomic<bool> completed{false};
        std::atomic<int> final_status{-1};
        
        // Create a fiber that will handle both setting and waiting
        auto setter_fiber = reactor->create_run_fiber([sp_event]() {
            // Just set the event immediately
            sp_event->set(1);
        });
        
        // Create the waiter fiber
        reactor->create_run_fiber([sp_event, &completed, &final_status]() {
            // Event should already be set, so this should complete immediately
            sp_event->wait_timeout(100000);
            completed = true;
            final_status = static_cast<int>(sp_event->status_.get());
        });
        
        // Process - event is already ready, so waiter should complete
        reactor->run_loop(false, true);
        
        EXPECT_TRUE(completed);
        EXPECT_EQ(final_status.load(), static_cast<int>(EventStatus::DONE));
    }
    
    // Test case 2: Event times out
    {
        auto sp_event = create_sp_int_event(1);
        std::atomic<bool> completed{false};
        std::atomic<int> final_status{-1};
        
        reactor->create_run_fiber([sp_event, &completed, &final_status]() {
            // Wait with very short timeout
            sp_event->wait_timeout(1000); // 1ms
            completed = true;
            final_status = static_cast<int>(sp_event->status_.get());
        });
        
        // Sleep longer than timeout
        std::this_thread::sleep_for(milliseconds(10));
        reactor->run_loop(false, true);
        
        EXPECT_TRUE(completed);
        EXPECT_EQ(final_status.load(), static_cast<int>(EventStatus::TIMEOUT));
    }
}

// Test 2: Event in both waiting_events_ and timeout_events_ lists
TEST_F(TimeoutRaceTest, DoubleListBehavior) {
    auto reactor = Reactor::get_reactor();
    
    // Create event with timeout - it goes in both lists
    auto sp_event = create_sp_int_event(1);
    std::atomic<int> loop_count{0};
    std::atomic<bool> completed{false};
    
    reactor->create_run_fiber([sp_event, &completed]() {
        sp_event->wait_timeout(50000); // 50ms timeout
        completed = true;
    });
    
    // Process events multiple times before timeout
    for (int i = 0; i < 5; i++) {
        std::this_thread::sleep_for(milliseconds(5));
        reactor->run_loop(false, true);
        loop_count++;
        if (completed) break;
    }
    
    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(60));
    reactor->run_loop(false, true);
    
    EXPECT_TRUE(completed);
    std::cout << "Event processed after " << loop_count 
              << " loop iterations before timeout" << std::endl;
}

// Test 3: Multiple events with staggered timeouts
TEST_F(TimeoutRaceTest, StaggeredTimeouts) {
    auto reactor = Reactor::get_reactor();
    
    const int num_events = 10;
    std::atomic<int> timeout_count{0};
    std::atomic<int> ready_count{0};
    
    // Create events with different timeouts
    for (int i = 0; i < num_events; i++) {
        auto sp_event = create_sp_int_event(1);
        
        reactor->create_run_fiber([sp_event, i, &timeout_count, &ready_count]() {
            // Half will be set ready, half will timeout
            if (i % 2 == 0) {
                // Create inner fiber to set event ready
                auto reactor = Reactor::get_reactor();
                reactor->create_run_fiber([sp_event]() {
                    Fiber::current_fiber().unwrap()->yield_();
                    sp_event->set(1);
                });
            }
            
            // Wait with varying timeouts
            sp_event->wait_timeout((10 + i * 5) * 1000);
            
            if (sp_event->status_.get() == EventStatus::TIMEOUT) {
                timeout_count++;
            } else if (sp_event->status_.get() == EventStatus::DONE) {
                ready_count++;
            }
        });
    }
    
    // Process events over time
    for (int i = 0; i < 20; i++) {
        std::this_thread::sleep_for(milliseconds(10));
        reactor->run_loop(false, true);
    }
    
    std::cout << "Results: Ready=" << ready_count 
              << ", Timeout=" << timeout_count 
              << " (total=" << (ready_count + timeout_count) << ")" << std::endl;
    
    EXPECT_EQ(ready_count + timeout_count, num_events);
}

// Test 4: Timeout event cleanup
TEST_F(TimeoutRaceTest, TimeoutEventCleanup) {
    auto reactor = Reactor::get_reactor();
    const size_t waiting_before = reactor->waiting_events_.borrow()->size();
    const size_t composite_before = reactor->composite_events_.borrow()->size();
    
    // Create multiple events that will timeout
    std::vector<rusty::Arc<IntEvent>> events;
    std::atomic<int> completed_count{0};
    
    for (int i = 0; i < 5; i++) {
        auto sp_event = create_sp_int_event(1);
        events.push_back(sp_event);
        
        reactor->create_run_fiber([sp_event, &completed_count]() {
            sp_event->wait_timeout(10000); // 10ms timeout
            completed_count++;
        });
    }

    // Raft's submission loop uses TimeoutEvent::wait() rather than adding a
    // timeout to an IntEvent. Cover that real primitive too; depending on the
    // exact scan/deadline interleaving it legitimately finishes as DONE or
    // TIMEOUT, but either terminal state must leave the polling queue.
    std::vector<rusty::Arc<TimeoutEvent>> timer_events;
    std::atomic<int> timer_completed_count{0};
    for (int i = 0; i < 5; ++i) {
        auto timer_event = create_sp_timeout_event(10000);
        timer_events.push_back(timer_event);
        reactor->create_run_fiber([timer_event, &timer_completed_count]() {
            timer_event->wait();
            timer_completed_count++;
        });
    }

    // Exercise the second polling queue too. Neither child becomes ready, so
    // the WaitAll itself times out while registered in both waiting_events_
    // and composite_events_.
    auto child_a = create_sp_int_event(1);
    auto child_b = create_sp_int_event(1);
    rusty::Vec<rusty::Arc<EventPollable>> children = {child_a, child_b};
    auto composite = create_sp_waitall_from(children);
    std::atomic<bool> composite_completed{false};
    reactor->create_run_fiber([composite, &composite_completed]() {
        composite->wait_timeout(10000); // 10ms timeout
        composite_completed = true;
    });
    
    // Wait for all timeouts
    std::this_thread::sleep_for(milliseconds(20));
    reactor->run_loop(false, true);
    
    EXPECT_EQ(completed_count, 5);
    EXPECT_EQ(timer_completed_count, 5);
    EXPECT_TRUE(composite_completed);
    
    // Verify all events are in TIMEOUT state
    for (auto& event : events) {
        EXPECT_EQ(event->status_.get(), EventStatus::TIMEOUT);
    }
    for (auto& event : timer_events) {
        EXPECT_TRUE(event->status_.get() == EventStatus::DONE ||
                    event->status_.get() == EventStatus::TIMEOUT);
    }
    EXPECT_EQ(composite->status_.get(), EventStatus::TIMEOUT);

    // TIMEOUT remains observable on the event, but it is terminal for reactor
    // scheduling and must not stay in either polling queue.  A second pass
    // performs the post-resume cleanup in the same way as the production loop.
    reactor->run_loop(false, true);
    EXPECT_LE(reactor->waiting_events_.borrow()->size(), waiting_before);
    EXPECT_LE(reactor->composite_events_.borrow()->size(), composite_before);

    // Aggregate sizes can shrink because of unrelated cleanup, so also assert
    // the scheduling invariant directly: neither polling queue may retain a
    // terminal TIMEOUT event.
    {
        auto waiting = reactor->waiting_events_.borrow();
        for (size_t i = 0; i < waiting->size(); ++i) {
            EXPECT_NE((*waiting)[i]->status(), EventStatus::TIMEOUT);
        }
    }
    {
        auto composite_queue = reactor->composite_events_.borrow();
        for (size_t i = 0; i < composite_queue->size(); ++i) {
            EXPECT_NE((*composite_queue)[i]->status(), EventStatus::TIMEOUT);
        }
    }
}

// Test 5: Rapid timeout changes in same thread
TEST_F(TimeoutRaceTest, RapidTimeoutChanges) {
    auto reactor = Reactor::get_reactor();
    
    const int num_iterations = 50;
    std::atomic<int> timeout_count{0};
    std::atomic<int> ready_count{0};
    
    for (int iter = 0; iter < num_iterations; iter++) {
        auto sp_event = create_sp_int_event(1);
        
        reactor->create_run_fiber([sp_event, iter, &timeout_count, &ready_count]() {
            // Randomly decide to set ready or let timeout
            if (iter % 3 == 0) {
                // Set it ready immediately (same fiber)
                sp_event->set(1);
            }
            
            // Very short timeout
            sp_event->wait_timeout(1000); // 1ms
            
            if (sp_event->status_.get() == EventStatus::TIMEOUT) {
                timeout_count++;
            } else if (sp_event->status_.get() == EventStatus::DONE) {
                ready_count++;
            }
        });
        
        // Process with small delay
        if (iter % 3 != 0) {
            std::this_thread::sleep_for(milliseconds(2));
        }
        reactor->run_loop(false, true);
    }
    
    std::cout << "Results after " << num_iterations << " iterations: "
              << "Ready=" << ready_count << ", Timeout=" << timeout_count << std::endl;
    
    EXPECT_EQ(ready_count + timeout_count, num_iterations);
}

// Test 6: Event status after timeout
TEST_F(TimeoutRaceTest, EventStatusAfterTimeout) {
    auto reactor = Reactor::get_reactor();
    
    auto sp_event = create_sp_int_event(1);
    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    
    // First fiber waits with timeout
    reactor->create_run_fiber([sp_event, &first_done]() {
        sp_event->wait_timeout(5000); // 5ms timeout
        first_done = true;
        EXPECT_EQ(sp_event->status_.get(), EventStatus::TIMEOUT);
    });
    
    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(10));
    reactor->run_loop(false, true);
    
    EXPECT_TRUE(first_done);
    
    // Try to use the same event again (should see it's already TIMEOUT)
    reactor->create_run_fiber([sp_event, &second_done]() {
        // Event is already in TIMEOUT state
        // The behavior here is interesting - what happens?
        if (sp_event->status_.get() == EventStatus::TIMEOUT) {
            std::cout << "Event already in TIMEOUT state before Wait()" << std::endl;
            second_done = true;
            // Don't try to wait on an already finished event - undefined behavior
            // The event system doesn't support reusing events after they're done/timeout
        } else {
            // This shouldn't happen, but if it does, try to wait
            sp_event->wait_timeout(5000);
            second_done = true;
        }

        std::cout << "Second fiber completed with event status: "
                  << static_cast<int>(sp_event->status_.get()) << std::endl;
    });
    
    reactor->run_loop(false, true);
    
    // The second fiber should complete
    EXPECT_TRUE(second_done);
    
    // This test reveals that events cannot be reused after timeout/done
    std::cout << "Note: Events cannot be reused after reaching DONE/TIMEOUT state" << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
