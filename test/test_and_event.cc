#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include "reactor/reactor.h"
#include "reactor/event.h"
#include "reactor/coroutine.h"

using namespace rrr;
using namespace std::chrono;

TEST(AndEventTest, BasicAndEvent) {
    auto reactor = Reactor::GetReactor();
    
    // Create two events that must both be ready
    auto event1 = Reactor::CreateSpEvent<IntEvent>();
    auto event2 = Reactor::CreateSpEvent<IntEvent>();
    
    // Create AndEvent that waits for both
    std::vector<std::shared_ptr<Event>> events = {event1, event2};
    auto and_event = Reactor::CreateSpEvent<AndEvent>(events);
    
    std::atomic<bool> and_triggered{false};
    
    reactor->CreateRunCoroutine([and_event, &and_triggered]() {
        and_event->Wait();
        and_triggered = true;
    });
    
    // Set only first event - AndEvent should NOT trigger
    event1->Set(1);
    reactor->Loop(false);
    EXPECT_FALSE(and_triggered);
    
    // Set second event - now AndEvent should trigger (use target value)
    event2->Set(1);
    reactor->Loop(false);
    EXPECT_TRUE(and_triggered);
}

TEST(AndEventTest, ThreeEventAnd) {
    auto reactor = Reactor::GetReactor();
    
    auto event1 = Reactor::CreateSpEvent<IntEvent>();
    auto event2 = Reactor::CreateSpEvent<IntEvent>();
    auto event3 = Reactor::CreateSpEvent<IntEvent>();
    
    std::vector<std::shared_ptr<Event>> events = {event1, event2, event3};
    auto and_event = Reactor::CreateSpEvent<AndEvent>(events);
    
    std::atomic<int> completion_value{0};
    
    reactor->CreateRunCoroutine([and_event, event1, event2, event3, &completion_value]() {
        and_event->Wait();
        // All three events should have their values set
        completion_value = event1->value_ + event2->value_ + event3->value_;
    });
    
    // Set events in different order
    event2->Set(1);
    reactor->Loop(false);
    EXPECT_EQ(completion_value, 0); // Not ready yet
    
    event3->Set(1);
    reactor->Loop(false);
    EXPECT_EQ(completion_value, 0); // Still not ready
    
    event1->Set(1);
    reactor->Loop(false);
    EXPECT_EQ(completion_value, 3); // Now all are ready: 1+1+1
}

TEST(AndEventTest, AndWithTimeout) {
    auto reactor = Reactor::GetReactor();
    
    auto event1 = Reactor::CreateSpEvent<IntEvent>();
    auto event2 = Reactor::CreateSpEvent<IntEvent>();
    
    std::vector<std::shared_ptr<Event>> events = {event1, event2};
    auto and_event = Reactor::CreateSpEvent<AndEvent>(events);
    
    std::atomic<bool> timed_out{false};
    std::atomic<bool> completed{false};
    
    reactor->CreateRunCoroutine([and_event, &timed_out, &completed]() {
        // Wait with 50ms timeout
        and_event->Wait(50000);
        completed = true;
        if (and_event->status_ == Event::TIMEOUT) {
            timed_out = true;
        }
    });
    
    // Set only one event
    event1->Set(1);
    
    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(100));
    reactor->Loop(false);
    
    EXPECT_TRUE(completed);
    // Should have timed out since event2 was never set
    EXPECT_TRUE(timed_out || and_event->status_ == Event::TIMEOUT);
}

TEST(AndEventTest, VariadicConstructor) {
    auto reactor = Reactor::GetReactor();
    
    auto event1 = Reactor::CreateSpEvent<IntEvent>();
    auto event2 = Reactor::CreateSpEvent<IntEvent>();
    auto event3 = Reactor::CreateSpEvent<IntEvent>();
    
    // Test variadic constructor
    auto and_event = Reactor::CreateSpEvent<AndEvent>(event1, event2, event3);
    
    std::atomic<bool> completed{false};
    
    reactor->CreateRunCoroutine([and_event, &completed]() {
        and_event->Wait();
        completed = true;
    });
    
    // Set all events
    event1->Set(1);
    event2->Set(1);
    event3->Set(1);
    
    reactor->Loop(false);
    EXPECT_TRUE(completed);
}

TEST(AndEventTest, MixedEventTypes) {
    auto reactor = Reactor::GetReactor();
    
    // Mix different event types
    auto int_event = Reactor::CreateSpEvent<IntEvent>();
    auto timeout_event = Reactor::CreateSpEvent<TimeoutEvent>(100000); // 100ms
    
    std::vector<std::shared_ptr<Event>> events = {int_event, timeout_event};
    auto and_event = Reactor::CreateSpEvent<AndEvent>(events);
    
    std::atomic<bool> completed{false};
    
    reactor->CreateRunCoroutine([and_event, &completed]() {
        and_event->Wait();
        completed = true;
    });
    
    // Set the int event
    int_event->Set(1);
    
    // Wait for timeout event to become ready
    std::this_thread::sleep_for(milliseconds(150));
    reactor->Loop(false);
    
    EXPECT_TRUE(completed);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}