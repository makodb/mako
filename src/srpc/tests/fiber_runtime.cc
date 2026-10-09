#include <stdint.h>
#include <stdlib.h>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
#include <gtest/gtest.h>


#include "../srpc.hpp"

using namespace std;
using namespace srpc;

//TEST(fiber, hello) {
//  ASSERT_EQ(1, 1);
//  Fiber::Create([] () {ASSERT_EQ(1, 1);});
////  Fiber::Create([] () {ASSERT_NE(1, 1);});
//}

#include "gtest/gtest.h"
// the variadic Log_* wrappers now live outside src/srpc
#include "srpc_log.h"

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

// xxx() returns while its fiber is still paused, so the fiber must not
// capture a reference to xxx()'s locals. It holds its own share of `x`.
rusty::Rc<Fiber> xxx(std::shared_ptr<int> x) {
    auto fiber1 = Fiber::create_run([x] () {
        *x = 1;
        Fiber::current_fiber().unwrap()->yield_();
    });
    return fiber1;
}

// A fiber outlives the frame that created it and can then run to completion.
TEST(FiberRuntimeTest, destruct) {
    auto x = std::make_shared<int>(0);
    rusty::Rc<Fiber> c = xxx(x);
    ASSERT_EQ(*x, 1);             // ran up to its yield inside create_run
    ASSERT_FALSE(c->finished());  // and is paused now that xxx() has returned
    c->continue_();
    ASSERT_TRUE(c->finished());
}

// Test dropping the only caller handle to a paused fiber (one that has
// yielded but not finished). Dropping the handle must not resume or finish
// the fiber. The fiber is not destroyed either: the reactor's fibers_
// registry also owns it (docs/srpc-book.md, "Abandoning a paused fiber").
// The fiber therefore outlives this test, so its state is shared and
// captured by value, never by reference to this frame.
TEST(FiberRuntimeTest, destroy_paused_fiber) {
    std::cout << "=== Testing destruction of paused fiber ===" << std::endl;

    struct State {
        int destructor_called = 0;
        int step = 0;
    };
    auto state = std::make_shared<State>();

    {
        auto fiber = Fiber::create_run([state] () {
            std::cout << "Fiber: Starting execution, step=" << state->step << std::endl;
            state->step = 1;

            std::cout << "Fiber: About to yield (step=1)" << std::endl;
            Fiber::current_fiber().unwrap()->yield_();

            // This should NOT be reached: nothing resumes the fiber
            std::cout << "Fiber: Resumed after first yield, step=" << state->step << std::endl;
            state->step = 2;

            std::cout << "Fiber: About to yield again (step=2)" << std::endl;
            Fiber::current_fiber().unwrap()->yield_();

            // This should definitely NOT be reached
            std::cout << "Fiber: Final execution, step=" << state->step << std::endl;
            state->step = 3;
            state->destructor_called = 1;
        });

        ASSERT_EQ(state->step, 1);  // Fiber should have run until first yield
        std::cout << "Main: Fiber yielded with step=" << state->step << std::endl;

        // Now we exit the scope WITHOUT calling Continue()
        // The fiber is still paused (has not finished execution)
        std::cout << "Main: About to drop the handle to the paused fiber" << std::endl;
    }

    // After scope exit, the caller's Rc<Fiber> handle is destroyed
    std::cout << "Main: Handle dropped, step=" << state->step << std::endl;
    std::cout << "Main: destructor_called=" << state->destructor_called << std::endl;

    // Dropping the handle neither resumed nor finished the paused fiber
    ASSERT_EQ(state->step, 1);  // Should still be 1, never reached step 2 or 3
    ASSERT_EQ(state->destructor_called, 0);  // Destructor logic never ran
    // The registry still owns the paused fiber, and so its closure still
    // holds the second share of `state`.
    EXPECT_EQ(state.use_count(), 2);

    std::cout << "=== Test completed successfully ===" << std::endl;
}

// Test dropping the handle to a paused fiber that allocates resources
TEST(FiberRuntimeTest, destroy_paused_fiber_with_cleanup) {
    std::cout << "=== Testing destruction of paused fiber with cleanup ===" << std::endl;

    // Shared rather than a raw new/delete pair: the paused fiber outlives
    // this test and keeps its share of both values.
    auto heap_flag = std::make_shared<bool>(false);
    auto cleanup_step = std::make_shared<int>(0);

    {
        auto fiber = Fiber::create_run([cleanup_step, heap_flag] () {
            std::cout << "Fiber: Allocating local resource" << std::endl;
            int local_var = 42;
            *cleanup_step = 1;

            std::cout << "Fiber: local_var=" << local_var << ", yielding..." << std::endl;
            Fiber::current_fiber().unwrap()->yield_();

            // If this runs, it means the fiber was properly resumed
            std::cout << "Fiber: Resumed! Setting heap flag" << std::endl;
            *heap_flag = true;
            *cleanup_step = 2;
        });

        ASSERT_EQ(*cleanup_step, 1);
        ASSERT_FALSE(*heap_flag);
        std::cout << "Main: Dropping handle to paused fiber with local_var still on stack" << std::endl;
    }

    std::cout << "Main: After handle drop, cleanup_step=" << *cleanup_step << std::endl;
    ASSERT_EQ(*cleanup_step, 1);  // Should not have progressed
    ASSERT_FALSE(*heap_flag);     // Should not have been set
    // The paused fiber, still owned by the registry, holds its shares.
    EXPECT_EQ(cleanup_step.use_count(), 2);
    EXPECT_EQ(heap_flag.use_count(), 2);

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

// A WaitAny over a 10s timer and an IntEvent resolves through the IntEvent,
// long before the timer. Both fibers capture `inte` by value. Before this
// fix they held a reference into this frame, and nothing checked that
// fiber1 finished. It finished only because fiber2's create_run happens to
// make a reactor pass.
TEST(FiberRuntimeTest, orevent) {
  auto inte = create_sp_int_event(1);
  auto done = std::make_shared<bool>(false);
  auto fiber1 = Fiber::create_run([inte, done]() mutable {
    auto t1 = Time::now(true);
    auto timeout = 10 * 1000000;
    auto sp_e1 = create_sp_timeout_event(timeout);
    auto sp_e2 = create_sp_waitany(sp_e1, inte);
    sp_e2->wait();
    auto t2 = Time::now(true);
    ASSERT_GT(t1 + timeout, t2);
    *done = true;
  });
  auto fiber2 = Fiber::create_run([inte](){
    inte->set(1);
  });
  auto reactor = Reactor::get_reactor();
  const uint64_t deadline = Time::now(true) + 5 * 1000000;
  while (!*done && Time::now(true) < deadline) {
    reactor->run_loop(false, true);
    Time::sleep(100);
  }
  ASSERT_TRUE(*done);
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
