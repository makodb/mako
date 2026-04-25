#include "replay_pool.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "benchmarks/benchmark_config.h"
#include "benchmarks/sto/ReplayDB.h"
#include "benchmarks/sto/ThreadPool.h"
#include "benchmarks/sto/Transaction.hh"
#include "benchmarks/sto/sync_util.hh"

#include <cstdio>
// Simple logging to stderr to avoid dragging in rrr/mako macros into this file.
#define RP_LOG(fmt, ...) fprintf(stderr, "[replay_pool] " fmt "\n", ##__VA_ARGS__)

namespace mako {

std::unique_ptr<ReplayPool> g_replay_pool;

ReplayPool::ReplayPool(TSharedThreadPoolMbta* replicated_db)
    : replicated_db_(replicated_db) {}

ReplayPool::~ReplayPool() { Stop(); }

void ReplayPool::Start(int n) {
  if (running_.load()) return;

  if (n <= 0) {
    const char* env = std::getenv("MAKO_REPLAY_THREADS");
    n = (env && *env) ? std::atoi(env) : 1;
    if (n < 1) n = 1;
  }
  n_ = n;
  running_.store(true);

  mtxes_          = std::vector<std::mutex>(n_);
  queues_         = std::vector<std::deque<ReplayTask>>(n_);
  un_replay_      = std::vector<std::unordered_map<uint32_t, UnReplayQueue>>(n_);
  replay_us_sum_  = std::vector<uint64_t>(n_, 0);
  replay_us_min_  = std::vector<uint64_t>(n_, UINT64_MAX);
  replay_us_max_  = std::vector<uint64_t>(n_, 0);
  replay_count_   = std::vector<uint64_t>(n_, 0);
  replay_queue_peak_ = std::vector<size_t>(n_, 0);

  RP_LOG("[REPLAY-POOL] Starting with %d worker thread(s) (MAKO_REPLAY_THREADS=%s)",
         n_,
         std::getenv("MAKO_REPLAY_THREADS") ? std::getenv("MAKO_REPLAY_THREADS") : "<unset>");

  workers_.reserve(n_);
  for (int i = 0; i < n_; i++) {
    workers_.emplace_back([this, i]() { WorkerLoop(i); });
  }
}

void ReplayPool::Stop() {
  if (!running_.exchange(false)) return;
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();

  // Free any remaining task buffers.
  for (auto& q : queues_) {
    while (!q.empty()) {
      free(q.front().log_copy);
      q.pop_front();
    }
  }
  // Free any remaining un_replay buffers.
  for (auto& m : un_replay_) {
    for (auto& kv : m) {
      auto& q = kv.second;
      while (!q.empty()) {
        auto& it = q.front();
        free(const_cast<char*>(std::get<4>(it)));
        q.pop();
      }
    }
  }
}

void ReplayPool::Enqueue(uint32_t par_id, char* log_copy, int len,
                         uint32_t slot_id, uint32_t timestamp) {
  if (!running_.load()) { free(log_copy); return; }
  int idx = static_cast<int>(par_id) % n_;
  ReplayTask t{par_id, slot_id, timestamp, len, log_copy};
  {
    std::lock_guard<std::mutex> lock(mtxes_[idx]);
    queues_[idx].push_back(t);
  }
}

void ReplayPool::WorkerLoop(int idx) {
  auto& benchConfig = BenchmarkConfig::getInstance();
  const int nshards = benchConfig.getNthreads();

  // Per-thread Sto init: the pool worker is a fresh OS thread, so it needs its
  // own set_id/disable_multiversion/thread_init. The shared `is_init` flag in
  // ThreadDBWrapperMbta only covers the one thread that first calls getDB,
  // so we bypass that and initialize this thread directly.
  TThread::set_id(idx);
  TThread::disable_multiversion();
  actual_directs::thread_init();

  auto window_start = std::chrono::steady_clock::now();
  uint64_t window_count = 0;
  auto last_idle_log = std::chrono::steady_clock::now();

  while (running_.load()) {
    ReplayTask task;
    bool got = false;
    size_t qsize = 0;
    {
      std::lock_guard<std::mutex> lock(mtxes_[idx]);
      qsize = queues_[idx].size();
      if (!queues_[idx].empty()) {
        task = queues_[idx].front();
        queues_[idx].pop_front();
        got = true;
      }
    }
    if (qsize > replay_queue_peak_[idx]) replay_queue_peak_[idx] = qsize;

    if (!got) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - last_idle_log).count() >= 5) {
        RP_LOG("[REPLAY-POOL] tid=%d: IDLE queue_size=%zu applied_total=%lu",
               idx, qsize, replay_count_[idx]);
        last_idle_log = now;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Do the Masstree work for this entry. Set the STO thread-id to match the
    // partition so per-partition caches route correctly. We don't call getDB()
    // here because its internal `is_init` flag is shared across threads and
    // won't re-run per-thread Sto initialization; we grab the shared db ptr
    // directly and ensure this thread's TThread/Sto state is current.
    TThread::set_id(task.par_id);
    Sto::update_threadid();
    abstract_db* db = ThreadDBWrapperMbta::replay_thread_wrapper_db;
    auto& un_replay = un_replay_[idx][task.par_id];

    auto t0 = std::chrono::steady_clock::now();

    uint32_t w = sync_util::sync_logger::retrieveW();
    bool loading = sync_util::sync_logger::noops_cnt.load(std::memory_order_acquire) == 0;
    if (loading || sync_util::sync_logger::safety_check(task.timestamp, w)) {
      benchConfig.incrementReplayBatch();
      treplay_in_same_thread_opt_mbta_v2(task.par_id, task.log_copy, task.len,
                                         db, nshards);
      free(task.log_copy);
    } else {
      // Defer: keep the buffer alive in un_replay; freed on drain.
      un_replay.push(std::make_tuple<int, int, int, int, const char*>(
          (int)task.timestamp, (int)task.slot_id, /*status*/ 2,  // STATUS_SAFETY_FAIL
          (int)task.len, static_cast<const char*>(task.log_copy)));
    }

    // Drain previously-deferred entries that may now be safe.
    while (!un_replay.empty()) {
      auto& it = un_replay.front();
      if (sync_util::sync_logger::safety_check(std::get<0>(it), w)) {
        benchConfig.incrementReplayBatch();
        treplay_in_same_thread_opt_mbta_v2(
            task.par_id, const_cast<char*>(std::get<4>(it)),
            std::get<3>(it), db, nshards);
        free(const_cast<char*>(std::get<4>(it)));
        un_replay.pop();
      } else {
        break;
      }
    }

    auto dt_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    replay_us_sum_[idx] += dt_us;
    if ((uint64_t)dt_us < replay_us_min_[idx]) replay_us_min_[idx] = dt_us;
    if ((uint64_t)dt_us > replay_us_max_[idx]) replay_us_max_[idx] = dt_us;
    replay_count_[idx]++;
    window_count++;

    if (replay_count_[idx] % 500 == 0) {
      auto now = std::chrono::steady_clock::now();
      double ws = std::chrono::duration_cast<std::chrono::microseconds>(
          now - window_start).count() / 1e6;
      double eps = ws > 0 ? window_count / ws : 0.0;
      uint64_t mean_us = replay_count_[idx] > 0
          ? replay_us_sum_[idx] / replay_count_[idx] : 0;
      RP_LOG("[REPLAY-TIMING] tid=%d count=%lu mean_us=%lu min_us=%lu "
             "max_us=%lu peak_queue=%zu window_eps=%.1f",
             idx, replay_count_[idx], mean_us,
             replay_us_min_[idx] == UINT64_MAX ? 0ul : replay_us_min_[idx],
             replay_us_max_[idx], replay_queue_peak_[idx], eps);
      window_start = now;
      window_count = 0;
      replay_queue_peak_[idx] = 0;
    }
  }
  RP_LOG("[REPLAY-POOL] tid=%d: worker exiting (applied %lu)",
         idx, replay_count_[idx]);
}

}  // namespace mako
