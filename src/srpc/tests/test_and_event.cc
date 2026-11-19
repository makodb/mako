#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>
#include "../srpc.hpp"

import std;
import rusty;

using namespace srpc;
using namespace std::chrono;

TEST(AndEventTest, BasicAndEvent) {
    auto reactor = Reactor::get_reactor();
    
    // Create two events that must both be ready
    auto event1 = create_sp_int_event(1);
    auto event2 = create_sp_int_event(1);
    
    // Create WaitAll that waits for both
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2};
    auto and_event = create_sp_waitall_from(events);
    
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
    
    auto event1 = create_sp_int_event(1);
    auto event2 = create_sp_int_event(1);
    auto event3 = create_sp_int_event(1);
    
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2, event3};
    auto and_event = create_sp_waitall_from(events);
    
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
    
    auto event1 = create_sp_int_event(1);
    auto event2 = create_sp_int_event(1);
    
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2};
    auto and_event = create_sp_waitall_from(events);
    
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
    
    auto event1 = create_sp_int_event(1);
    auto event2 = create_sp_int_event(1);
    auto event3 = create_sp_int_event(1);
    
    // Test vector constructor (the 3-arg variadic ctor was dropped when WaitAll
    // was flattened to a DSL struct)
    rusty::Vec<rusty::Arc<EventPollable>> events = {event1, event2, event3};
    auto and_event = create_sp_waitall_from(events);
    
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
    auto int_event = create_sp_int_event(1);
    auto timeout_event = create_sp_timeout_event(100000); // 100ms
    
    rusty::Vec<rusty::Arc<EventPollable>> events = {int_event, timeout_event};
    auto and_event = create_sp_waitall_from(events);
    
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

// S4 step 4 of docs/dev/lion-runtime-plan.md: composites wake on change.
// A child that tests ready tests its waiting parents, whose WAIT->READY edge
// queues them; nothing re-tests a composite per pass. These run the generated
// C++ parent links; the Rust lane's versions are in
// tests/reactor_composite_rust.rs. Captures are shared and every fiber
// finishes inside its test (the S4 step 0 rule).

TEST(AndEventTest, CompositeIsNotRetestedPerPass) {
    auto reactor = Reactor::get_reactor();
    const size_t waiting_before = reactor->waiting_events_.borrow()->len();
    const size_t composite_before = reactor->composite_events_.borrow()->len();

    auto probes = std::make_shared<int>(0);
    auto answer = std::make_shared<bool>(false);
    auto child = create_sp_int_event(1);
    *child->state_.test_.borrow_mut() = [probes, answer](int32_t) {
        ++*probes;
        return *answer;
    };
    rusty::Vec<rusty::Arc<EventPollable>> children = {child};
    auto and_event = create_sp_waitall_from(children);
    auto resumed = std::make_shared<int>(0);
    reactor->create_run_fiber([and_event, resumed]() {
        and_event->wait();
        ++*resumed;
    });
    EXPECT_EQ(reactor->waiting_events_.borrow()->len(), waiting_before);
    EXPECT_EQ(reactor->composite_events_.borrow()->len(), composite_before);

    const int parked_probes = *probes;
    for (int i = 0; i < 16; i++) {
        reactor->run_loop(false, true);
    }
    EXPECT_EQ(*probes, parked_probes) << "run_loop evaluated a waiting composite";

    *answer = true;
    EXPECT_TRUE(child->test());
    EXPECT_EQ(and_event->status_.get(), EventStatus::READY);
    EXPECT_EQ(*resumed, 0) << "a child test resumed its parent inline";
    reactor->run_loop(false, true);
    EXPECT_EQ(*resumed, 1);
    EXPECT_EQ(and_event->status_.get(), EventStatus::DONE);
}

TEST(AndEventTest, SharedChildWakesBothParents) {
    auto reactor = Reactor::get_reactor();
    auto shared = create_sp_int_event(1);
    auto other = create_sp_int_event(1);
    rusty::Vec<rusty::Arc<EventPollable>> children = {shared, other};
    auto all = create_sp_waitall_from(children);
    auto any = create_sp_waitany(create_sp_never_event(), shared);
    auto all_resumed = std::make_shared<int>(0);
    auto any_resumed = std::make_shared<int>(0);
    reactor->create_run_fiber([all, all_resumed]() {
        all->wait();
        ++*all_resumed;
    });
    reactor->create_run_fiber([any, any_resumed]() {
        any->wait();
        ++*any_resumed;
    });
    other->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*all_resumed + *any_resumed, 0);

    shared->set(1);
    EXPECT_EQ(all->status_.get(), EventStatus::READY);
    EXPECT_EQ(any->status_.get(), EventStatus::READY);
    reactor->run_loop(false, true);
    EXPECT_EQ(*all_resumed, 1);
    EXPECT_EQ(*any_resumed, 1);
}

TEST(AndEventTest, NestedCompositePropagatesThroughUnwaitedMiddle) {
    auto reactor = Reactor::get_reactor();
    auto a = create_sp_int_event(1);
    auto b = create_sp_int_event(1);
    auto c = create_sp_int_event(1);
    auto inner = create_sp_waitany(a, b);
    rusty::Vec<rusty::Arc<EventPollable>> children = {inner, c};
    auto outer = create_sp_waitall_from(children);
    auto resumed = std::make_shared<int>(0);
    reactor->create_run_fiber([outer, resumed]() {
        outer->wait();
        ++*resumed;
    });
    c->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*resumed, 0);
    b->set(1);
    EXPECT_EQ(inner->status_.get(), EventStatus::DONE);
    EXPECT_EQ(outer->status_.get(), EventStatus::READY);
    reactor->run_loop(false, true);
    EXPECT_EQ(*resumed, 1);
}

TEST(AndEventTest, AddEventLinksTheNewChild) {
    auto reactor = Reactor::get_reactor();
    auto all = create_sp_waitall();
    auto a = create_sp_int_event(1);
    auto b = create_sp_int_event(1);
    all->add_event(a);
    all->add_event(b);
    auto resumed = std::make_shared<int>(0);
    reactor->create_run_fiber([all, resumed]() {
        all->wait();
        ++*resumed;
    });
    a->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*resumed, 0);
    b->set(1);
    reactor->run_loop(false, true);
    EXPECT_EQ(*resumed, 1);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}