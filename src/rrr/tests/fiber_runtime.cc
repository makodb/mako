#include <stdint.h>
#include <stdlib.h>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>


#include "../rrr.hpp"

using namespace std;
using namespace rrr;

//TEST(fiber, hello) {
//  ASSERT_EQ(1, 1);
//  Fiber::Create([] () {ASSERT_EQ(1, 1);});
////  Fiber::Create([] () {ASSERT_NE(1, 1);});
//}

#include "gtest/gtest.h"

import std;
import rusty;

TEST(FiberRuntimeTest, helloworld) {
  Fiber::create_run([] () {ASSERT_EQ(1, 1);});
  Fiber::create_run([] () {ASSERT_NE(1, 2);});
}

TEST(FiberRuntimeTest, yield) {
  int x = 0;
  auto fiber1 = Fiber::create_run([&x] () {
    x = 1;
    Fiber::current_fiber().unwrap()->yield_();
    x = 2;
    Fiber::current_fiber().unwrap()->yield_();
    x = 3;
  });
  ASSERT_EQ(x, 1);
  Reactor::get_reactor()->continue_fiber(fiber1);
  ASSERT_EQ(x, 2);
  Reactor::get_reactor()->continue_fiber(fiber1);
  ASSERT_EQ(x, 3);
}

rusty::Rc<Fiber> xxx() {
    int x;
    auto fiber1 = Fiber::create_run([&x] () {
        x = 1;
        Fiber::current_fiber().unwrap()->yield_();
    });
    return fiber1;
}

TEST(FiberRuntimeTest, destruct) {
    rusty::Rc<Fiber> c = xxx();
    c->continue_();
}

// Test destroying a paused fiber (one that has yielded but not finished)
TEST(FiberRuntimeTest, destroy_paused_fiber) {
    std::cout << "=== Testing destruction of paused fiber ===" << std::endl;

    int destructor_called = 0;
    int step = 0;

    {
        auto fiber = Fiber::create_run([&step, &destructor_called] () {
            std::cout << "Fiber: Starting execution, step=" << step << std::endl;
            step = 1;

            std::cout << "Fiber: About to yield (step=1)" << std::endl;
            Fiber::current_fiber().unwrap()->yield_();

            // This should NOT be reached if we destroy the fiber
            std::cout << "Fiber: Resumed after first yield, step=" << step << std::endl;
            step = 2;

            std::cout << "Fiber: About to yield again (step=2)" << std::endl;
            Fiber::current_fiber().unwrap()->yield_();

            // This should definitely NOT be reached
            std::cout << "Fiber: Final execution, step=" << step << std::endl;
            step = 3;
            destructor_called = 1;
        });

        ASSERT_EQ(step, 1);  // Fiber should have run until first yield
        std::cout << "Main: Fiber yielded with step=" << step << std::endl;

        // Now we exit the scope WITHOUT calling Continue()
        // The fiber is still paused (has not finished execution)
        std::cout << "Main: About to destroy paused fiber" << std::endl;
    }

    // After scope exit, the Rc<Fiber> is destroyed
    std::cout << "Main: Fiber destroyed, step=" << step << std::endl;
    std::cout << "Main: destructor_called=" << destructor_called << std::endl;

    // The fiber should have been destroyed while paused
    ASSERT_EQ(step, 1);  // Should still be 1, never reached step 2 or 3
    ASSERT_EQ(destructor_called, 0);  // Destructor logic never ran

    std::cout << "=== Test completed successfully ===" << std::endl;
}

// Test destroying a paused fiber that allocates resources
TEST(FiberRuntimeTest, destroy_paused_fiber_with_cleanup) {
    std::cout << "=== Testing destruction of paused fiber with cleanup ===" << std::endl;

    bool* heap_flag = new bool(false);
    int cleanup_step = 0;

    {
        auto fiber = Fiber::create_run([&cleanup_step, heap_flag] () {
            std::cout << "Fiber: Allocating local resource" << std::endl;
            int local_var = 42;
            cleanup_step = 1;

            std::cout << "Fiber: local_var=" << local_var << ", yielding..." << std::endl;
            Fiber::current_fiber().unwrap()->yield_();

            // If this runs, it means the fiber was properly resumed
            std::cout << "Fiber: Resumed! Setting heap flag" << std::endl;
            *heap_flag = true;
            cleanup_step = 2;
        });

        ASSERT_EQ(cleanup_step, 1);
        ASSERT_FALSE(*heap_flag);
        std::cout << "Main: Destroying paused fiber with local_var still on stack" << std::endl;
    }

    std::cout << "Main: After destruction, cleanup_step=" << cleanup_step << std::endl;
    ASSERT_EQ(cleanup_step, 1);  // Should not have progressed
    ASSERT_FALSE(*heap_flag);     // Should not have been set

    delete heap_flag;
    std::cout << "=== Test completed successfully ===" << std::endl;
}
TEST(FiberRuntimeTest, timeout) {
  auto fiber1 = Fiber::create_run([](){
    auto t1 = Time::now(true);
    auto timeout = 1 * 1000000;
    auto sp_e = create_sp_timeout_event(timeout);
    Log_debug("set timeout, start wait");
    sp_e->wait();
    auto t2 = Time::now(true);
    ASSERT_GT(t2, t1 + timeout);
    Log_debug("end timeout, end wait");
    Reactor::get_reactor()->looping_.set(false);
  });
  Reactor::get_reactor()->run_loop(true, true);
}

TEST(FiberRuntimeTest, orevent) {
  auto inte = create_sp_int_event(1);
  auto fiber1 = Fiber::create_run([&inte](){
    auto t1 = Time::now(true);
    auto timeout = 10 * 1000000;
    auto sp_e1 = create_sp_timeout_event(timeout);
    auto sp_e2 = reactor_create_sp_event<WaitAny>(sp_e1, inte);
    sp_e2->wait();
    auto t2 = Time::now(true);
    ASSERT_GT(t1 + timeout, t2);
  });
  auto fiber2 = Fiber::create_run([&inte](){
    inte->set(1);
  });
}

TEST(SquareRootTest, PositiveNos) {
//  EXPECT_EQ (18.0, square-root (324.0));
//  EXPECT_EQ (25.4, square-root (645.16));
//  EXPECT_EQ (50.3321, square-root (2533.310224));
}

TEST (SquareRootTest, ZeroAndNegativeNos) {
//  ASSERT_EQ (0.0, square-root (0.0));
//  ASSERT_EQ (-1, square-root (-22.0));
}


int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
