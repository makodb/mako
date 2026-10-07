#include <stdlib.h>

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include "../srpc.hpp"

import std;

using namespace srpc;
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
    
    // Wait for all timeouts
    std::this_thread::sleep_for(milliseconds(20));
    reactor->run_loop(false, true);
    
    EXPECT_EQ(completed_count, 5);
    
    // Verify all events are in TIMEOUT state
    for (auto& event : events) {
        EXPECT_EQ(event->status_.get(), EventStatus::TIMEOUT);
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

// Tests 7-10 (S4 step 3 of docs/dev/lion-runtime-plan.md): timers go through a
// per-reactor deadline map served in deadline order, instead of a linear scan
// of every timed wait on every pass. These run the generated C++ map; the
// Rust lane's versions are in tests/reactor_deadline_rust.rs. Every capture is
// shared, and every fiber finishes inside its test, so nothing outlives a
// frame (the S4 step 0 rule).

// Test 7: timers that expire before one pass resume in deadline order.
TEST_F(TimeoutRaceTest, DeadlinesResumeInDeadlineOrder) {
    auto reactor = Reactor::get_reactor();
    auto order = std::make_shared<std::vector<std::string>>();

    auto sleeper = create_sp_timeout_event(300000);
    reactor->create_run_fiber([sleeper, order]() {
        sleeper->wait();
        order->push_back("timeout-event-300ms");
    });
    auto int_event = create_sp_int_event(1);
    reactor->create_run_fiber([int_event, order]() {
        int_event->wait_timeout(100000);
        order->push_back("int-100ms");
    });
    auto never = create_sp_never_event();
    reactor->create_run_fiber([never, order]() {
        never->wait_timeout(200000);
        order->push_back("never-200ms");
    });
    EXPECT_TRUE(order->empty());

    std::this_thread::sleep_for(milliseconds(350));
    reactor->run_loop(false, true);
    ASSERT_EQ(order->size(), 3u);
    EXPECT_EQ((*order)[0], "int-100ms");
    EXPECT_EQ((*order)[1], "never-200ms");
    EXPECT_EQ((*order)[2], "timeout-event-300ms");
    EXPECT_EQ(int_event->status_.get(), EventStatus::TIMEOUT);
    EXPECT_EQ(never->status_.get(), EventStatus::TIMEOUT);
    EXPECT_EQ(sleeper->status_.get(), EventStatus::DONE);
}

// Test 8: a timed wait whose event became ready without an owner-thread
// test() completes at its deadline, READY, and not before. One event has its
// value written directly; the other is also marked READY, the state a
// foreign-thread set() leaves (it queues nothing off the owner thread).
TEST_F(TimeoutRaceTest, ReadyWithoutAnOwnerTestCompletesAtTheDeadline) {
    auto reactor = Reactor::get_reactor();
    auto written = create_sp_int_event(1);
    auto marked = create_sp_int_event(1);
    auto resumed = std::make_shared<int>(0);
    auto early = std::make_shared<int>(0);  // resumed before its own deadline
    auto statuses_done = std::make_shared<int>(0);
    for (auto ev : {written, marked}) {
        reactor->create_run_fiber([ev, resumed, early, statuses_done]() {
            ev->wait_timeout(300000);
            if (ev->status_.get() == EventStatus::DONE) {
                ++*statuses_done;
            }
            if (Time::now(true) < ev->wakeup_time()) {
                ++*early;
            }
            ++*resumed;
        });
    }
    ASSERT_GT(written->wakeup_time(), 0u);
    ASSERT_GT(marked->wakeup_time(), 0u);
    written->value_.set(1);
    marked->value_.set(1);
    marked->set_status(EventStatus::READY);

    const auto limit = steady_clock::now() + seconds(5);
    while (*resumed < 2 && steady_clock::now() < limit) {
        std::this_thread::sleep_for(milliseconds(1));
        reactor->run_loop(false, true);
    }
    ASSERT_EQ(*resumed, 2);
    EXPECT_EQ(*early, 0) << "a waiter resumed before its deadline";
    EXPECT_EQ(*statuses_done, 2);
}

// Test 9: deletion is lazy, so a timed wait that ended early leaves an entry
// behind. It must not end a later, untimed wait of the same event.
TEST_F(TimeoutRaceTest, StaleDeadlineDoesNotEndALaterWait) {
    auto reactor = Reactor::get_reactor();
    auto ev = create_sp_int_event(1);
    auto phase = std::make_shared<int>(0);
    reactor->create_run_fiber([ev, phase]() {
        ev->wait_timeout(100000);
        *phase = 1;
        ev->value_.set(0);
        ev->test();  // not ready any more: DONE -> INIT
        ev->wait();
        *phase = 2;
    });
    ev->set(1);
    reactor->run_loop(false, true);
    ASSERT_EQ(*phase, 1);

    std::this_thread::sleep_for(milliseconds(110));
    reactor->run_loop(false, true);
    reactor->run_loop(false, true);
    EXPECT_EQ(*phase, 1) << "a stale deadline ended an untimed wait";
    EXPECT_EQ(ev->status_.get(), EventStatus::WAIT);

    ev->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*phase, 2);
}

// Test 10: a long timeout on a wait that ends at once does not keep a map
// entry until the timeout; stale entries are swept.
TEST_F(TimeoutRaceTest, EarlyEndingWaitsDoNotAccumulateDeadlines) {
    auto reactor = Reactor::get_reactor();
    const EventWakeReport before = event_wake_report<std::tuple<>>();
    for (int i = 0; i < 1000; i++) {
        auto ev = create_sp_int_event(1);
        reactor->create_run_fiber([ev]() { ev->wait_timeout(60000000); });
        ev->set(1);
        reactor->run_loop(false, true);
        ASSERT_EQ(ev->status_.get(), EventStatus::DONE);
    }
    const EventWakeReport after = event_wake_report<std::tuple<>>();
    EXPECT_EQ(after.live_deadlines, before.live_deadlines);
    EXPECT_LE(after.deadline_entries,
              before.deadline_entries + 2 * before.live_deadlines + 65);
}

// Test 11: one pass can list an event twice, through the ready queue and
// through its deadline. If the first dispatch resumes a waiter that re-arms
// the event and waits on it again at once, the second entry must leave that
// new wait alone rather than resume it (or trip the dispatch's checks).
TEST_F(TimeoutRaceTest, SecondEntryDoesNotEndAnImmediateRewait) {
    auto reactor = Reactor::get_reactor();
    auto ev = create_sp_int_event(1);
    auto phase = std::make_shared<int>(0);
    reactor->create_run_fiber([ev, phase]() {
        ev->wait_timeout(5000);
        *phase = 1;
        ev->value_.set(0);
        ev->test();  // DONE -> INIT
        ev->wait();
        *phase = 2;
    });
    std::this_thread::sleep_for(milliseconds(10));
    ev->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*phase, 1) << "the second wait was resumed by the first wait's entry";
    EXPECT_EQ(ev->status_.get(), EventStatus::WAIT);
    ev->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*phase, 2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}