#include <stddef.h>

#include "macros.h"
#include "thread.h"
#include "lib/fasttransport.h"
#include "benchmarks/benchmark_config.h"

import std;

using namespace std;

// Match Linux's 8 MB default on macOS (macOS std::thread defaults to 512 KB).
static constexpr size_t kThreadStackBytes = 8u * 1024u * 1024u;

// @unsafe - pthread trampoline; raw pointer cast
static void *ndb_thread_trampoline(void *arg) {
  auto *self = static_cast<ndb_thread *>(arg);
  // Inherit the spawner's explicit shard binding (multi-shard
  // single-process: without this, workers race on the shared global
  // shard index and two shards' workers bind identical client ports).
  if (self->inherited_shard_index() >= 0) {
    BenchmarkConfig::setThreadLocalShardIndex(self->inherited_shard_index());
  }
  self->run();
  return nullptr;
}

static pthread_t start_with_stack(ndb_thread *self) {
  pthread_attr_t attr;
  ALWAYS_ASSERT(pthread_attr_init(&attr) == 0);
  ALWAYS_ASSERT(pthread_attr_setstacksize(&attr, kThreadStackBytes) == 0);
  pthread_t thd;
  ALWAYS_ASSERT(pthread_create(&thd, &attr, ndb_thread_trampoline, self) == 0);
  pthread_attr_destroy(&attr);
  return thd;
}

// @safe
ndb_thread::~ndb_thread()
{
}

// @unsafe - creates thread via pthread with 8 MB stack
void
ndb_thread::start()
{
  inherited_shard_index_ = BenchmarkConfig::getThreadLocalShardIndex();
  thd_ = start_with_stack(this);
  if (daemon_)
    pthread_detach(thd_);
}

// @unsafe: uses pthread_setname_np
void
ndb_thread::startBind(int core_id)
{
  inherited_shard_index_ = BenchmarkConfig::getThreadLocalShardIndex();
  thd_ = start_with_stack(this);
#if defined(__APPLE__)
  (void)core_id;
#else
  pthread_setname_np(thd_, ("worker_" + to_string(core_id)).c_str());
#endif
  if (daemon_)
    pthread_detach(thd_);
}

// @unsafe: uses pthread_join
void
ndb_thread::join()
{
  ALWAYS_ASSERT(!daemon_);
  pthread_join(thd_, nullptr);
}

// @unsafe
// can be overloaded by subclasses
void
ndb_thread::run()
{
  ALWAYS_ASSERT(body_);
  body_();
}
