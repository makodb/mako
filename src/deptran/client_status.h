#ifndef CLIENT_STATUS_H_
#define CLIENT_STATUS_H_

#include <rusty/mutex.hpp>
#include <rusty/condvar.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/function.hpp>
#include <map>
#include <vector>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#include "srpc/srpc.hpp"
#ifdef __APPLE__ // for OS X
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "constants.h"

namespace janus {

using srpc::verify;

// Forward declarations
void clock_gettime(struct timespec *time);
bool operator<(const struct timespec &lhs, const struct timespec &rhs);

/**
 * Shared client status that can be held by both ClientControlServiceImpl
 * and external callers (ClientWorker, Coordinators). This decouples status
 * management and statistics from the RPC service.
 *
 * Similar to ServerStatus, but with additional statistics tracking.
 * All methods are const because Arc<T> dereferences to const T;
 * interior mutability is provided by Mutex/locks.
 */
class ClientStatus {
 public:
  enum class LatencyCollectionStatus { LAST_PERIOD, THIS_PERIOD, IGNORE };
  enum class Status { INIT, READY, RUN, FINISH, STOP };

  struct txn_info_t {
    int32_t txn_type;
    int32_t commit_txn;
    int32_t start_txn;
    int32_t total_txn;
    int32_t total_try;
    int32_t retries_exhausted;
    std::vector<double> *interval_latencies[2];
    std::vector<double> *last_interval_latencies[2];
    std::vector<double> *interval_attempt_latencies[2];
    std::vector<int32_t> *num_try[2];
    std::vector<double> interval_latency;

    txn_info_t() {
      txn_type = -1;
      start_txn = 0;
      commit_txn = 0;
      total_txn = 0;
      total_try = 0;
      retries_exhausted = 0;
      num_try[0] = new std::vector<int32_t>();
      num_try[1] = new std::vector<int32_t>();

      interval_latencies[0] = new std::vector<double>();
      interval_latencies[1] = new std::vector<double>();

      last_interval_latencies[0] = new std::vector<double>();
      last_interval_latencies[1] = new std::vector<double>();

      interval_attempt_latencies[0] = new std::vector<double>();
      interval_attempt_latencies[1] = new std::vector<double>();
    }

    void init(int32_t _txn_type) {
      txn_type = _txn_type;
    }

    void destroy() {
      delete interval_latencies[0];
      delete interval_latencies[1];
      delete last_interval_latencies[0];
      delete last_interval_latencies[1];
      delete interval_attempt_latencies[0];
      delete interval_attempt_latencies[1];
      delete num_try[0];
      delete num_try[1];
    }

    void start(bool switzh) {
      start_txn++;
    }

    void give_up() {
      retries_exhausted++;
    }

    void retry(bool switzh, double attempt_latency) {
      total_try++;
      if (switzh)
        interval_attempt_latencies[0]->push_back(attempt_latency);
      else
        interval_attempt_latencies[1]->push_back(attempt_latency);
    }

    void succ(bool switzh, LatencyCollectionStatus lcs, double latency, double attempt_latency, int32_t tried) {
      total_txn++;
      total_try++;
      commit_txn++;
      int use = 1;
      if (switzh)
        use = 0;
      num_try[use]->push_back(tried);
      switch (lcs) {
        case LatencyCollectionStatus::THIS_PERIOD:
          interval_latencies[use]->push_back(latency);
          break;
        case LatencyCollectionStatus::LAST_PERIOD:
          last_interval_latencies[use]->push_back(latency);
          break;
        case LatencyCollectionStatus::IGNORE:
        default:
          break;
      }
      interval_attempt_latencies[use]->push_back(attempt_latency);
      interval_latency.push_back(latency);
    }

    void rej(bool switzh, LatencyCollectionStatus lcs, double latency, double attempt_latency, int32_t tried) {
      total_txn++;
      total_try++;
      if (switzh)
        interval_attempt_latencies[0]->push_back(attempt_latency);
      else
        interval_attempt_latencies[1]->push_back(attempt_latency);
    }
  };

 private:
  // Synchronization state
  struct SyncState {
    Status status = Status::INIT;
    unsigned int num_ready = 0;
    unsigned int num_finish = 0;
    std::vector<rusty::Function<void()>> ready_block_defers;
  };

  mutable rusty::Box<rusty::Mutex<SyncState>> sync_state_;
  mutable rusty::Box<rusty::Condvar> sync_cond_;

  // Statistics state - mutable for interior mutability
  mutable pthread_t** coo_threads_;
  mutable std::map<int32_t, txn_info_t>* txn_info_;
  mutable bool txn_info_switch_;
  mutable std::recursive_mutex mtx_;
  mutable pthread_rwlock_t collect_lock_;
  unsigned int num_threads_;
  mutable struct timespec start_time_;
  mutable struct timespec last_time_;
  mutable struct timespec before_last_time_;
  std::map<int32_t, std::string> txn_names_;

 public:
  ClientStatus(unsigned int num_threads, const std::map<int32_t, std::string>& txn_types);
  ~ClientStatus();

  // Non-copyable, non-movable (shared via Arc)
  ClientStatus(const ClientStatus&) = delete;
  ClientStatus& operator=(const ClientStatus&) = delete;
  ClientStatus(ClientStatus&&) = delete;
  ClientStatus& operator=(ClientStatus&&) = delete;

  // =========================================================================
  // Synchronization methods (for ClientWorker)
  // All methods are const because Arc<T> dereferences to const T
  // =========================================================================

  void wait_for_start(unsigned int id) const;
  void wait_for_shutdown() const;

  // =========================================================================
  // Statistics methods (for Coordinators)
  // =========================================================================

  void txn_give_up_one(txnid_t id, int32_t txn_type) const {
    std::lock_guard<std::recursive_mutex> guard(mtx_);
    pthread_rwlock_rdlock(&collect_lock_);
    verify(id >= 0 && id < num_threads_);
    verify(txn_info_[id].find(txn_type) != txn_info_[id].end());
    txn_info_[id][txn_type].give_up();
    pthread_rwlock_unlock(&collect_lock_);
  }

  void txn_start_one(unsigned int id, int32_t txn_type) const {
    std::lock_guard<std::recursive_mutex> guard(mtx_);
    pthread_rwlock_rdlock(&collect_lock_);
    verify(id >= 0 && id < num_threads_);
    verify(txn_info_[id].find(txn_type) != txn_info_[id].end());
    txn_info_[id][txn_type].start(txn_info_switch_);
    pthread_rwlock_unlock(&collect_lock_);
  }

  void txn_retry_one(unsigned int id, int32_t txn_type, double attempt_latency) const {
    std::lock_guard<std::recursive_mutex> guard(mtx_);
    pthread_rwlock_rdlock(&collect_lock_);
    txn_info_[id][txn_type].retry(txn_info_switch_, attempt_latency);
    pthread_rwlock_unlock(&collect_lock_);
  }

  void txn_success_one(unsigned int id,
                       int32_t txn_type,
                       struct timespec start_time,
                       double latency,
                       double attempt_latency,
                       int32_t tried) const {
    std::lock_guard<std::recursive_mutex> guard(mtx_);
    pthread_rwlock_rdlock(&collect_lock_);
    auto lcs = LatencyCollectionStatus::IGNORE;
    if (last_time_ < start_time)
      lcs = LatencyCollectionStatus::THIS_PERIOD;
    else if (before_last_time_ < start_time)
      lcs = LatencyCollectionStatus::LAST_PERIOD;
    txn_info_[id][txn_type].succ(txn_info_switch_, lcs, latency, attempt_latency, tried);
    pthread_rwlock_unlock(&collect_lock_);
  }

  void txn_reject_one(unsigned int id,
                      int32_t txn_type,
                      struct timespec start_time,
                      double latency,
                      double attempt_latency,
                      int32_t tried) const {
    std::lock_guard<std::recursive_mutex> guard(mtx_);
    pthread_rwlock_rdlock(&collect_lock_);
    auto lcs = LatencyCollectionStatus::IGNORE;
    if (last_time_ < start_time)
      lcs = LatencyCollectionStatus::THIS_PERIOD;
    else if (before_last_time_ < start_time)
      lcs = LatencyCollectionStatus::LAST_PERIOD;
    txn_info_[id][txn_type].rej(txn_info_switch_, lcs, latency, attempt_latency, tried);
    pthread_rwlock_unlock(&collect_lock_);
  }

  // =========================================================================
  // Methods for RPC service to access/modify state
  // =========================================================================

  Status get_status() const {
    auto guard = sync_state_->lock().unwrap();
    return guard->status;
  }

  void set_status(Status s) const {
    {
      auto guard = sync_state_->lock().unwrap();
      guard->status = s;
    }
    sync_cond_->notify_all();
  }

  void set_status_run_and_start_timer() const {
    {
      auto guard = sync_state_->lock().unwrap();
      guard->status = Status::RUN;
    }
    sync_cond_->notify_all();
    clock_gettime(&start_time_);
    last_time_ = start_time_;
    before_last_time_ = start_time_;
  }

  void add_ready_block_defer(rusty::Function<void()> cb) const {
    auto guard = sync_state_->lock().unwrap();
    guard->ready_block_defers.emplace_back(std::move(cb));
  }

  void wait_until_ready_or_stop() const {
    auto guard = sync_state_->lock().unwrap();
    guard = sync_cond_->wait_while(
        std::move(guard),
        [](SyncState& s) {
          return s.status != Status::READY && s.status != Status::STOP;
        }).unwrap();
  }

  unsigned int num_threads() const { return num_threads_; }
  pthread_t** coo_threads() const { return coo_threads_; }
  const std::map<int32_t, std::string>& txn_names() const { return txn_names_; }
  std::map<int32_t, txn_info_t>* txn_info() const { return txn_info_; }
  bool txn_info_switch() const { return txn_info_switch_; }

  // For client_response RPC - collect and return statistics
  // Returns is_finish flag
  template<typename ClientResponseT>
  bool collect_response(ClientResponseT* res) const {
    std::lock_guard<std::recursive_mutex> guard(mtx_);

    bool is_finish;
    {
      auto status_guard = sync_state_->lock().unwrap();
      is_finish = (status_guard->status == Status::FINISH);
    }

    pthread_rwlock_wrlock(&collect_lock_);
    before_last_time_ = last_time_;
    clock_gettime(&last_time_);
    res->run_sec = (int64_t)(last_time_.tv_sec - start_time_.tv_sec);
    res->run_nsec = (int64_t)(last_time_.tv_nsec - start_time_.tv_nsec);

    res->period_sec = (int64_t)(last_time_.tv_sec - before_last_time_.tv_sec);
    res->period_nsec = (int64_t)(last_time_.tv_nsec - before_last_time_.tv_nsec);

    txn_info_switch_ = !txn_info_switch_;

    for (unsigned int i = 0; i < num_threads_; i++) {
      for (auto it = txn_info_[i].begin(); it != txn_info_[i].end(); it++) {
        res->txn_info[it->first].start_txn += it->second.start_txn;
        res->txn_info[it->first].total_txn += it->second.total_txn;
        res->txn_info[it->first].total_try += it->second.total_try;
        res->txn_info[it->first].commit_txn += it->second.commit_txn;
        res->txn_info[it->first].num_exhausted += it->second.retries_exhausted;
        res->txn_info[it->first].interval_latency.insert(
            res->txn_info[it->first].interval_latency.end(),
            it->second.interval_latency.begin(),
            it->second.interval_latency.end());
        it->second.interval_latency.clear();
      }
    }
    pthread_rwlock_unlock(&collect_lock_);

    int use = 0;
    if (txn_info_switch_)
      use = 1;
    for (unsigned int i = 0; i < num_threads_; i++) {
      for (auto it = txn_info_[i].begin(); it != txn_info_[i].end(); it++) {
        res->txn_info[it->first].this_latency.insert(
            res->txn_info[it->first].this_latency.end(),
            it->second.interval_latencies[use]->begin(),
            it->second.interval_latencies[use]->end());
        res->txn_info[it->first].last_latency.insert(
            res->txn_info[it->first].last_latency.end(),
            it->second.last_interval_latencies[use]->begin(),
            it->second.last_interval_latencies[use]->end());
        res->txn_info[it->first].attempt_latency.insert(
            res->txn_info[it->first].attempt_latency.end(),
            it->second.interval_attempt_latencies[use]->begin(),
            it->second.interval_attempt_latencies[use]->end());

        res->txn_info[it->first].num_try.insert(
            res->txn_info[it->first].num_try.end(),
            it->second.num_try[use]->begin(),
            it->second.num_try[use]->end());

        it->second.retries_exhausted = 0;
        it->second.num_try[use]->clear();
        it->second.interval_latencies[use]->clear();
        it->second.last_interval_latencies[use]->clear();
        it->second.interval_attempt_latencies[use]->clear();
      }
    }

    return is_finish;
  }
};

}  // namespace janus

#endif  // CLIENT_STATUS_H_
