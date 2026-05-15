#include <stdint.h>
#include <stdlib.h>

#include "deptran/s_main.h"
#include <assert.h>
#include "rrr/rrr.hpp"

import std;

using namespace std;
using namespace rrr;

// copy from ./tests/fiber_runtime.cc
void ASSERT_EQ(bool a) { if (!a) throw; }

void fiber_basic() {
  int x = 0;
  Fiber::create_run([&x] () {
    x = 1;
    sleep(2);
    x = 2;
  });
  ASSERT_EQ(x == 2);
}
void fiber_yield() {
  int x = 0;
  auto fiber1 = Fiber::create_run([&x] () {
    x = 1;
    Fiber::current_fiber().unwrap()->yield_();
    x = 2;
    Fiber::current_fiber().unwrap()->yield_();
    x = 3;
  });
  ASSERT_EQ(x == 1);
  Reactor::get_reactor()->continue_fiber(fiber1);
  ASSERT_EQ(x == 2);
  Reactor::get_reactor()->continue_fiber(fiber1);
  ASSERT_EQ(x == 3);
}
rusty::Rc<Fiber> fiber_yield_2_sub() {
  int x;
  auto fiber1 = Fiber::create_run([&x] () {
      x = 1;
      Fiber::current_fiber().unwrap()->yield_();
  });
  return fiber1;
}
void fiber_yield_2() {
  rusty::Rc<Fiber> fiber = fiber_yield_2_sub();
  fiber->continue_();
}
void fiber_wait_die_lock() {
  WaitDieALock a;
  auto fiber1 = Fiber::create_run([&a] () {
    uint64_t req_id = a.lock_sync(0, ALock::WLOCK, 10);
    ASSERT_EQ(req_id == true);
    Fiber::current_fiber().unwrap()->yield_();
    Log_info("aborting lock from fiber 1.");
    a.abort(req_id);
  });

  int x = 0;
  auto fiber2 = Fiber::create_run([&] () {
    uint64_t req_id = a.lock_sync(0, ALock::WLOCK, 11);
    ASSERT_EQ(req_id == false);
    x = 1;
  });
  (void)fiber2;
  ASSERT_EQ(x == 1);

  int y = 0;
  auto fiber3 = Fiber::create_run([&] () {
    uint64_t req_id = a.lock_sync(0, ALock::WLOCK, 8);  // yield
    ASSERT_EQ(req_id > 0);
    Log_info("acquired lock from fiber 3.");
    y = 1;
  });
  (void)fiber3;
  ASSERT_EQ(y == 0);
  fiber1->continue_();
  Reactor::get_reactor()->loop();
  ASSERT_EQ(y == 1);
}

int main(int argc, char* argv[]) {
  fiber_basic();
  fiber_yield();
  fiber_yield_2();
  fiber_wait_die_lock();
}
