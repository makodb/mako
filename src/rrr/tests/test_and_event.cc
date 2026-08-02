#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;
import rusty;

using namespace rrr;
using namespace std::chrono;

TEST(AndEventTest, BasicAndEvent) {
    auto reactor = Reactor::get_reactor();
    
    // Create two events that must both be ready
    auto event1 = reactor_create_sp_event<IntEvent>();
    auto event2 = reactor_create_sp_event<IntEvent>();
    
    // Create WaitAll that waits for both
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2};
    auto and_event = reactor_create_sp_event<WaitAll>(events);
    
    std::atomic<bool> and_triggered{false};
    
    reactor->create_run_fiber([and_event, &and_triggered]() {
        and_event->wait();
        and_triggered = true;
    });
    
    // Set only first event - WaitAll should NOT trigger
    event1->set(1);
    reactor->run_loop(false, true);
    EXPECT_FALSE(and_triggered);
    
    // Set second event - now WaitAll should trigger (use target value)
    event2->set(1);
    reactor->run_loop(false, true);
    EXPECT_TRUE(and_triggered);
}

TEST(AndEventTest, ThreeEventAnd) {
    auto reactor = Reactor::get_reactor();
    
    auto event1 = reactor_create_sp_event<IntEvent>();
    auto event2 = reactor_create_sp_event<IntEvent>();
    auto event3 = reactor_create_sp_event<IntEvent>();
    
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2, event3};
    auto and_event = reactor_create_sp_event<WaitAll>(events);
    
    std::atomic<int> completion_value{0};
    
    reactor->create_run_fiber([and_event, event1, event2, event3, &completion_value]() {
        and_event->wait();
        // All three events should have their values set
        completion_value = event1->value_.get() + event2->value_.get() + event3->value_.get();
    });
    
    // Set events in different order
    event2->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(completion_value, 0); // Not ready yet
    
    event3->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(completion_value, 0); // Still not ready
    
    event1->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(completion_value, 3); // Now all are ready: 1+1+1
}

TEST(AndEventTest, AndWithTimeout) {
    auto reactor = Reactor::get_reactor();
    
    auto event1 = reactor_create_sp_event<IntEvent>();
    auto event2 = reactor_create_sp_event<IntEvent>();
    
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2};
    auto and_event = reactor_create_sp_event<WaitAll>(events);
    
    std::atomic<bool> timed_out{false};
    std::atomic<bool> completed{false};
    
    reactor->create_run_fiber([and_event, &timed_out, &completed]() {
        // Wait with 50ms timeout
        and_event->wait_timeout(50000);
        completed = true;
        if (and_event->status_.get() == EventStatus::TIMEOUT) {
            timed_out = true;
        }
    });

    // Set only one event
    event1->set(1);

    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(100));
    reactor->run_loop(false, true);

    EXPECT_TRUE(completed);
    // Should have timed out since event2 was never set
    EXPECT_TRUE(timed_out || and_event->status_.get() == EventStatus::TIMEOUT);
}

TEST(AndEventTest, VariadicConstructor) {
    auto reactor = Reactor::get_reactor();
    
    auto event1 = reactor_create_sp_event<IntEvent>();
    auto event2 = reactor_create_sp_event<IntEvent>();
    auto event3 = reactor_create_sp_event<IntEvent>();
    
    // Test vector constructor (the 3-arg variadic ctor was dropped when WaitAll
    // was flattened to a DSL struct)
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2, event3};
    auto and_event = reactor_create_sp_event<WaitAll>(events);
    
    std::atomic<bool> completed{false};
    
    reactor->create_run_fiber([and_event, &completed]() {
        and_event->wait();
        completed = true;
    });
    
    // Set all events
    event1->set(1);
    event2->set(1);
    event3->set(1);
    
    reactor->run_loop(false, true);
    EXPECT_TRUE(completed);
}

TEST(AndEventTest, MixedEventTypes) {
    auto reactor = Reactor::get_reactor();
    
    // Mix different event types
    auto int_event = reactor_create_sp_event<IntEvent>();
    auto timeout_event = reactor_create_sp_event<TimeoutEvent>(100000); // 100ms
    
    rusty::Vec<rusty::Arc<EventPollable>> events = {int_event, timeout_event};
    auto and_event = reactor_create_sp_event<WaitAll>(events);
    
    std::atomic<bool> completed{false};
    
    reactor->create_run_fiber([and_event, &completed]() {
        and_event->wait();
        completed = true;
    });
    
    // Set the int event
    int_event->set(1);
    
    // Wait for timeout event to become ready
    std::this_thread::sleep_for(milliseconds(150));
    reactor->run_loop(false, true);
    
    EXPECT_TRUE(completed);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}