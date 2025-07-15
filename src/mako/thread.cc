#include <iostream>
#include "macros.h"
#include "thread.h"
#include "lib/fasttransport.h"

using namespace std;

ndb_thread::~ndb_thread()
{
}

void
ndb_thread::start()
{
  thd_ = std::move(thread(&ndb_thread::run, this));
  if (daemon_)
    thd_.detach();
}

void
ndb_thread::startBind(int core_id)
{
  thd_ = std::move(thread(&ndb_thread::run, this));
  pthread_setname_np(thd_.native_handle(), ("worker_"+std::to_string(core_id)).c_str());
  //cpu_set_t cpuset;
  //CPU_ZERO(&cpuset);
  //CPU_SET(core_id, &cpuset);
  //pthread_setaffinity_np(thd_.native_handle(),sizeof(cpu_set_t), &cpuset);
  if (daemon_)
    thd_.detach();
}

void
ndb_thread::join()
{
  ALWAYS_ASSERT(!daemon_);
  thd_.join();
}

// can be overloaded by subclasses
void
ndb_thread::run()
{
  ALWAYS_ASSERT(body_);
  body_();
}
