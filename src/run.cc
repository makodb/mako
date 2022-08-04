#include "deptran/s_main.h"
#include <assert.h>
#include <iostream>
#include <boost/coroutine/all.hpp>
#include "rrr/rrr.hpp"

using namespace std;
using namespace rrr;

// copy from ./test/coroutine.cc
void ASSERT_EQ(bool a) { if (!a) throw; }

void coroutine_basic() {
  int x = 0;
  Coroutine::CreateRun([&x] () {
    x = 1;
    sleep(2);
    x = 2;
  });
  ASSERT_EQ(x == 2);
}
void coroutine_yield() {
  int x = 0;
  auto coro1 = Coroutine::CreateRun([&x] () {
    x = 1;
    Coroutine::CurrentCoroutine()->Yield();
    x = 2;
    Coroutine::CurrentCoroutine()->Yield();
    x = 3;
  });
  ASSERT_EQ(x == 1);
  Reactor::GetReactor()->ContinueCoro(coro1);
  ASSERT_EQ(x == 2);
  Reactor::GetReactor()->ContinueCoro(coro1);
  ASSERT_EQ(x == 3);
}
shared_ptr<Coroutine> coroutine_yield_2_sub() {
  int x;
  auto coro1 = Coroutine::CreateRun([&x] () {
      x = 1;
      Coroutine::CurrentCoroutine()->Yield();
  });
  return coro1;
}
void coroutine_yield_2() {
  shared_ptr<Coroutine> c = coroutine_yield_2_sub();
  c->Continue();
}
void coroutine_wait_die_lock() {
  WaitDieALock a;
  auto coro1 = Coroutine::CreateRun([&a] () {
    uint64_t req_id = a.Lock(0, ALock::WLOCK, 10);
    ASSERT_EQ(req_id == true);
    Coroutine::CurrentCoroutine()->Yield();
    Log_info("aborting lock from coroutine 1.");
    a.abort(req_id);
  });

  int x = 0;
  auto coro2 = Coroutine::CreateRun([&] () {
    uint64_t req_id = a.Lock(0, ALock::WLOCK, 11);
    ASSERT_EQ(req_id == false);
    x = 1;
  });
  ASSERT_EQ(x == 1);

  int y = 0;
  auto coro3 = Coroutine::CreateRun([&] () {
    uint64_t req_id = a.Lock(0, ALock::WLOCK, 8);  // yield
    ASSERT_EQ(req_id > 0);
    Log_info("acquired lock from coroutine 3.");
    y = 1;
  });
  ASSERT_EQ(y == 0);
  coro1->Continue();
  Reactor::GetReactor()->Loop();
  ASSERT_EQ(y == 1);
}

int main(int argc, char* argv[]) {
  coroutine_basic();
  coroutine_yield();
  coroutine_yield_2();
  coroutine_wait_die_lock();
}
