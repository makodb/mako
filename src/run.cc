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

int main(int argc, char* argv[]) {
  fiber_basic();
  fiber_yield();
  fiber_yield_2();
}
