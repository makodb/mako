// ReplayPool: pool of worker threads that perform the heavy Masstree replay
// step, moved out of the Raft apply callback so the apply thread can stay fast
// and Raft progress is decoupled from DB-apply latency.
//
// One pool instance per process. Workers are sharded by par_id (partition p
// always maps to worker `p % N`), preserving per-partition slot order while
// allowing different partitions to run in parallel.
//
// Pool size is selected via env var `MAKO_REPLAY_THREADS` (default 1).
//
// Thread-safety: Enqueue() takes a short lock on the target worker's queue.
// Each worker owns its own un_replay state; no cross-worker sharing.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid heavy includes here.
class TSharedThreadPoolMbta;

namespace mako {

struct ReplayTask {
  uint32_t par_id;
  uint32_t slot_id;
  uint32_t timestamp;
  int      len;
  char*    log_copy;   // owned; freed by the worker after replay
};

class ReplayPool {
 public:
  explicit ReplayPool(TSharedThreadPoolMbta* replicated_db);
  ~ReplayPool();

  // Start N worker threads. If n<=0, read MAKO_REPLAY_THREADS (default 1).
  void Start(int n);

  // Stop and join all workers.
  void Stop();

  // Enqueue one entry. log_copy must be malloc'd and ownership is transferred
  // to the pool.
  void Enqueue(uint32_t par_id, char* log_copy, int len,
               uint32_t slot_id, uint32_t timestamp);

 private:
  void WorkerLoop(int worker_idx);

  using UnReplayEntry = std::tuple<int, int, int, int, const char*>;
  using UnReplayQueue = std::queue<UnReplayEntry>;

  TSharedThreadPoolMbta* replicated_db_;
  int n_{0};
  std::atomic<bool> running_{false};

  std::vector<std::thread> workers_;
  std::vector<std::mutex>  mtxes_;
  std::vector<std::deque<ReplayTask>> queues_;

  // Per-worker, per-partition un_replay queues. Only touched by the worker
  // that owns the partition, so no extra locking is needed.
  std::vector<std::unordered_map<uint32_t, UnReplayQueue>> un_replay_;

  // Per-worker counters for REPLAY-TIMING logs.
  std::vector<uint64_t> replay_us_sum_;
  std::vector<uint64_t> replay_us_min_;
  std::vector<uint64_t> replay_us_max_;
  std::vector<uint64_t> replay_count_;
  std::vector<size_t>   replay_queue_peak_;
};

// Process-wide pool. Initialized at benchmark startup after the replicated_db
// exists, and torn down at shutdown.
extern std::unique_ptr<ReplayPool> g_replay_pool;

}  // namespace mako
