#include <gtest/gtest.h>

#include <boost/coroutine/all.hpp>
#include <stdexcept>
#include <iostream>

#include "rrr/rrr.hpp"

using namespace std;
using namespace rrr;

//using boost::coroutines::coroutine;
//typedef coroutine<void>::pull_type boost_coro_task_t;
//typedef coroutine<void>::push_type boost_coro_yield_t;

//TEST(coroutine, hello) {
//  ASSERT_EQ(1, 1);
//  Coroutine::Create([] () {ASSERT_EQ(1, 1);});
////  Coroutine::Create([] () {ASSERT_NE(1, 1);});
//}
//
//void cooperative(boost_coro_yield_t &yield)
//{
//  yield();
//}

//class Cooperative {
// public:
//  void work(boost_coro_yield_t &yield) {
//    yield();
//  }
//};
//
//class CoroutineWrapper {
// public:
//  boost_coro_task_t *boost_coro_task_{nullptr};
//
//  void Work(boost_coro_yield_t &yield) {
//    yield();
//  }
//
//  CoroutineWrapper() {
//    boost_coro_task_ = new boost_coro_task_t(
//        std::bind(&CoroutineWrapper::Work, this, std::placeholders::_1));
//  }
//
//  void Run() {
//    (*boost_coro_task_)();
//  }
//
//  ~CoroutineWrapper() {
//    delete boost_coro_task_;
//  }
//};

//TEST(coroutine, boost) {
//  boost_coro_task_t source{cooperative};
//  source();
//  Cooperative o;
//  boost_coro_task_t* task = new boost_coro_task_t(
//      std::bind(&Cooperative::work, o, std::placeholders::_1));
//  (*task)();
//  delete task;
//  CoroutineWrapper* cw = new CoroutineWrapper();
//  cw->Run();
//  delete cw;
//
//}

#include "gtest/gtest.h"

TEST(CoroutineTest, helloworld) {
  Coroutine::CreateRun([] () {ASSERT_EQ(1, 1);});
  Coroutine::CreateRun([] () {ASSERT_NE(1, 2);});
}

TEST(CoroutineTest, yield) {
  int x = 0;
  auto coro1 = Coroutine::CreateRun([&x] () {
    x = 1;
    Coroutine::CurrentCoroutine().unwrap()->Yield();
    x = 2;
    Coroutine::CurrentCoroutine().unwrap()->Yield();
    x = 3;
  });
  ASSERT_EQ(x, 1);
  Reactor::GetReactor()->ContinueCoro(coro1);
  ASSERT_EQ(x, 2);
  Reactor::GetReactor()->ContinueCoro(coro1);
  ASSERT_EQ(x, 3);
}

rusty::Rc<Coroutine> xxx() {
    int x;
    auto coro1 = Coroutine::CreateRun([&x] () {
        x = 1;
        Coroutine::CurrentCoroutine().unwrap()->Yield();
    });
    return coro1;
}

TEST(CoroutineTest, destruct) {
    rusty::Rc<Coroutine> c = xxx();
    c->Continue();
}

// Test destroying a paused coroutine (one that has yielded but not finished)
TEST(CoroutineTest, destroy_paused_coroutine) {
    std::cout << "=== Testing destruction of paused coroutine ===" << std::endl;

    int destructor_called = 0;
    int step = 0;

    {
        auto coro = Coroutine::CreateRun([&step, &destructor_called] () {
            std::cout << "Coroutine: Starting execution, step=" << step << std::endl;
            step = 1;

            std::cout << "Coroutine: About to yield (step=1)" << std::endl;
            Coroutine::CurrentCoroutine().unwrap()->Yield();

            // This should NOT be reached if we destroy the coroutine
            std::cout << "Coroutine: Resumed after first yield, step=" << step << std::endl;
            step = 2;

            std::cout << "Coroutine: About to yield again (step=2)" << std::endl;
            Coroutine::CurrentCoroutine().unwrap()->Yield();

            // This should definitely NOT be reached
            std::cout << "Coroutine: Final execution, step=" << step << std::endl;
            step = 3;
            destructor_called = 1;
        });

        ASSERT_EQ(step, 1);  // Coroutine should have run until first yield
        std::cout << "Main: Coroutine yielded with step=" << step << std::endl;

        // Now we exit the scope WITHOUT calling Continue()
        // The coroutine is still paused (has not finished execution)
        std::cout << "Main: About to destroy paused coroutine" << std::endl;
    }

    // After scope exit, the Rc<Coroutine> is destroyed
    std::cout << "Main: Coroutine destroyed, step=" << step << std::endl;
    std::cout << "Main: destructor_called=" << destructor_called << std::endl;

    // The coroutine should have been destroyed while paused
    ASSERT_EQ(step, 1);  // Should still be 1, never reached step 2 or 3
    ASSERT_EQ(destructor_called, 0);  // Destructor logic never ran

    std::cout << "=== Test completed successfully ===" << std::endl;
}

// Test destroying a paused coroutine that allocates resources
TEST(CoroutineTest, destroy_paused_coroutine_with_cleanup) {
    std::cout << "=== Testing destruction of paused coroutine with cleanup ===" << std::endl;

    bool* heap_flag = new bool(false);
    int cleanup_step = 0;

    {
        auto coro = Coroutine::CreateRun([&cleanup_step, heap_flag] () {
            std::cout << "Coroutine: Allocating local resource" << std::endl;
            int local_var = 42;
            cleanup_step = 1;

            std::cout << "Coroutine: local_var=" << local_var << ", yielding..." << std::endl;
            Coroutine::CurrentCoroutine().unwrap()->Yield();

            // If this runs, it means the coroutine was properly resumed
            std::cout << "Coroutine: Resumed! Setting heap flag" << std::endl;
            *heap_flag = true;
            cleanup_step = 2;
        });

        ASSERT_EQ(cleanup_step, 1);
        ASSERT_FALSE(*heap_flag);
        std::cout << "Main: Destroying paused coroutine with local_var still on stack" << std::endl;
    }

    std::cout << "Main: After destruction, cleanup_step=" << cleanup_step << std::endl;
    ASSERT_EQ(cleanup_step, 1);  // Should not have progressed
    ASSERT_FALSE(*heap_flag);     // Should not have been set

    delete heap_flag;
    std::cout << "=== Test completed successfully ===" << std::endl;
}

TEST(CoroutineTest, wait_die_lock) {
  WaitDieALock a;
  auto coro1 = Coroutine::CreateRun([&a] () {
    uint64_t req_id = a.Lock(0, ALock::WLOCK, 10);
    ASSERT_EQ(req_id, true);
    Coroutine::CurrentCoroutine().unwrap()->Yield();
    Log_debug("aborting lock from coroutine 1.");
    a.abort(req_id);
  });

  int x = 0;
  auto coro2 = Coroutine::CreateRun([&] () {
    uint64_t req_id = a.Lock(0, ALock::WLOCK, 11);
    ASSERT_EQ(req_id, false);
    x = 1;
  });
  ASSERT_EQ(x, 1);

  int y = 0;
  auto coro3 = Coroutine::CreateRun([&] () {
    uint64_t req_id = a.Lock(0, ALock::WLOCK, 8);
    ASSERT_GT(req_id, 0);
    Log_debug("acquired lock from coroutine 3.");
    y = 1;
  });
  ASSERT_EQ(y, 0);
  coro1->Continue();
  Reactor::GetReactor()->Loop();
  ASSERT_EQ(y, 1);
}

TEST(CoroutineTest, timeout) {
  auto coro1 = Coroutine::CreateRun([](){
    auto t1 = Time::now();
    auto timeout = 1 * 1000000;
    auto sp_e = Reactor::CreateSpEvent<TimeoutEvent>(timeout);
    Log_debug("set timeout, start wait");
    sp_e->Wait();
    auto t2 = Time::now();
    ASSERT_GT(t2, t1 + timeout);
    Log_debug("end timeout, end wait");
    Reactor::GetReactor()->looping_ = false;
  });
  Reactor::GetReactor()->Loop(true);
}

TEST(CoroutineTest, orevent) {
  auto inte = Reactor::CreateSpEvent<IntEvent>();
  auto coro1 = Coroutine::CreateRun([&inte](){
    auto t1 = Time::now();
    auto timeout = 10 * 1000000;
    auto sp_e1 = Reactor::CreateSpEvent<TimeoutEvent>(timeout);
    auto sp_e2 = Reactor::CreateSpEvent<OrEvent>(sp_e1, inte);
    sp_e2->Wait();
    auto t2 = Time::now();
    ASSERT_GT(t1 + timeout, t2);
  });
  auto coro2 = Coroutine::CreateRun([&inte](){
    inte->Set(1);
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