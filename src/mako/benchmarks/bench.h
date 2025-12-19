#ifndef _NDB_BENCH_H_
#define _NDB_BENCH_H_

#include <stdint.h>

#include <map>
#include <vector>
#include <utility>
#include <string>

#include "abstract_db.h"
#include "../macros.h"
#include "../thread.h"
#include "../util.h"
#include "../spinbarrier.h"
#include "../rcu.h"
#ifdef MAKO_USE_RAFT
#include "deptran/raft_main_helper.h"
#else
#include "deptran/s_main.h"
#endif
#include "lib/configuration.h"
#include "benchmark_config.h"

class bench_runner;

extern void ycsb_do_test(abstract_db *db, int argc, char **argv);
extern bench_runner* tpcc_do_test(abstract_db *db, int argc, char **argv, int, bench_runner *);
extern bench_runner* tpcc_do_test(abstract_db *db, int argc, char **argv, int, bench_runner *, int shard_index);
extern void tpcc_simple_do_test(abstract_db *db, int argc, char **argv);
extern void queue_do_test(abstract_db *db, int argc, char **argv);
extern void encstress_do_test(abstract_db *db, int argc, char **argv);
extern void bid_do_test(abstract_db *db, int argc, char **argv);

// Multi-shard mode: wire up cross-shard table references between runners
// target_runner receives tables from source_runner (for source_shard_idx's partitions)
extern void wireup_cross_shard_tables_tpcc(bench_runner* target_runner, int source_shard_idx, bench_runner* source_runner);

class scoped_db_thread_ctx {
public:
  scoped_db_thread_ctx(const scoped_db_thread_ctx &) = delete;
  scoped_db_thread_ctx(scoped_db_thread_ctx &&) = delete;
  scoped_db_thread_ctx &operator=(const scoped_db_thread_ctx &) = delete;

  // source: 0 by default, 1 helper_server
  scoped_db_thread_ctx(abstract_db *db, bool loader, int source=0)
    : db(db)
  {
    db->thread_init(loader, source);
  }
  ~scoped_db_thread_ctx()
  {
    db->thread_end();
  }
private:
  abstract_db *const db;
};

class bench_loader : public ndb_thread {
public:
  bench_loader(unsigned long seed, abstract_db *db,
               const std::map<std::string, abstract_ordered_index *> &open_tables,
               int shard_index = -1)  // -1 means use default from BenchmarkConfig
    : r(seed), db(db), open_tables(open_tables), b(0), shard_index_(shard_index)
  {
    txn_obj_buf.reserve(str_arena::MinStrReserveLength);
    txn_obj_buf.resize(db->sizeof_txn_object(BenchmarkConfig::getInstance().getTxnFlags()));
  }
  inline void
  set_barrier(spin_barrier &b)
  {
    ALWAYS_ASSERT(!this->b);
    this->b = &b;
  }

  // Get the shard index for this loader
  int get_shard_index() const { return shard_index_; }

  virtual void
  run()
  {
    { // XXX(stephentu): this is a hack
      scoped_rcu_region r; // register this thread in rcu region
    }

    // Set thread-local shard index and bind to correct SiloRuntime in multi-shard mode
    auto& benchConfig = BenchmarkConfig::getInstance();
    if (benchConfig.getConfig() && benchConfig.getConfig()->multi_shard_mode) {
      int shard_idx = shard_index_;
      if (shard_idx < 0 && !benchConfig.getConfig()->local_shard_indices.empty()) {
        shard_idx = benchConfig.getConfig()->local_shard_indices[0];
      }
      // Set thread-local shard index BEFORE thread_init() is called
      BenchmarkConfig::setThreadLocalShardIndex(shard_idx);

      ShardContext* shard_ctx = benchConfig.getShardContext(shard_idx);
      if (shard_ctx && shard_ctx->runtime.get()) {
        // Use get() and const_cast because get_mut() returns null with shared ownership
        const_cast<SiloRuntime*>(shard_ctx->runtime.get())->BindToCurrentThread();
      }
    } else {
      // Single-shard mode: use global default runtime
      SiloRuntime::Current()->BindToCurrentThread();
    }

    // ALWAYS_ASSERT(b);
    // b->count_down();
    // b->wait_for();
    scoped_db_thread_ctx ctx(db, true);
    load();
  }
protected:
  inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

  virtual void load() = 0;

  util::fast_random r;
  abstract_db *const db;
  std::map<std::string, abstract_ordered_index *> open_tables;
  spin_barrier *b;
  int shard_index_;  // Shard index for multi-shard mode (-1 = use default)
  std::string txn_obj_buf;
  str_arena arena;
};

class bench_worker : public ndb_thread {
public:

  bench_worker(unsigned int worker_id,
               bool set_core_id,
               unsigned long seed, abstract_db *db,
               const std::map<std::string, abstract_ordered_index *> &open_tables,
               spin_barrier *barrier_a, spin_barrier *barrier_b,
               int shard_index = -1)  // -1 means use default from BenchmarkConfig
    : worker_id(worker_id), set_core_id(set_core_id),
      r(seed), db(db), open_tables(open_tables),
      barrier_a(barrier_a), barrier_b(barrier_b),
      shard_index_(shard_index),
      // the ntxn_* numbers are per worker
      ntxn_commits(0), ntxn_aborts(0),
      latency_numer_us(0),
      latency_numer_us_remote(0),
      backoff_shifts(0), // spin between [0, 2^backoff_shifts) times before retry
      size_delta(0)
  {
    txn_obj_buf.reserve(str_arena::MinStrReserveLength);
    txn_obj_buf.resize(db->sizeof_txn_object(BenchmarkConfig::getInstance().getTxnFlags()));
    r = util::fast_random(worker_id);
  }

  // Get the shard index for this worker
  int get_shard_index() const { return shard_index_; }

  virtual ~bench_worker() {}

  // returns [did_commit?, size_increase_bytes*10+{is_local}?0:1]
  typedef std::pair<bool, ssize_t> txn_result;
  typedef txn_result (*txn_fn_t)(bench_worker *);

  struct workload_desc {
    workload_desc() {}
    workload_desc(const std::string &name, double frequency, txn_fn_t fn)
      : name(name), frequency(frequency), fn(fn)
    {
      ALWAYS_ASSERT(frequency > 0.0);
      ALWAYS_ASSERT(frequency <= 1.0);
    }
    std::string name;
    double frequency;
    txn_fn_t fn;
  };
  typedef std::vector<workload_desc> workload_desc_vec;
  virtual workload_desc_vec get_workload() const = 0;

  virtual void run();

  inline size_t get_ntxn_commits() const { return ntxn_commits; }
  inline size_t get_ntxn_aborts() const { return ntxn_aborts; }

  inline uint64_t get_latency_numer_us() const { return latency_numer_us; }
  inline uint64_t get_latency_numer_us_remote() const { return latency_numer_us_remote; }

  inline double
  get_avg_latency_us() const
  {
    return double(latency_numer_us) / double(ntxn_commits);
  }

  std::map<std::string, size_t> get_txn_counts() const;

  void print_stats() const;

  typedef abstract_db::counter_map counter_map;
  typedef abstract_db::txn_counter_map txn_counter_map;

#ifdef ENABLE_BENCH_TXN_COUNTERS
  inline txn_counter_map
  get_local_txn_counters() const
  {
    return local_txn_counters;
  }
#endif

  inline ssize_t get_size_delta() const { return size_delta; }

protected:

  virtual void on_run_setup() {}

  inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

  unsigned int worker_id;
  bool set_core_id;
  util::fast_random r;
  abstract_db *const db;
  std::map<std::string, abstract_ordered_index *> open_tables;
  spin_barrier *const barrier_a;
  spin_barrier *const barrier_b;
  int shard_index_;  // Shard index for multi-shard mode (-1 = use default)

private:
  size_t ntxn_commits;
  size_t ntxn_aborts;
  uint64_t latency_numer_us;  // for all transactions
  uint64_t latency_numer_us_remote; // only for remote
  unsigned backoff_shifts;

protected:

#ifdef ENABLE_BENCH_TXN_COUNTERS
  txn_counter_map local_txn_counters;
  void measure_txn_counters(void *txn, const char *txn_name);
#else
  inline ALWAYS_INLINE void measure_txn_counters(void *txn, const char *txn_name) {}
#endif

  std::vector<size_t> txn_counts; // breakdown of txns
  std::vector<size_t> sampling_remote_calls; // Debugging: the invoke time for remote calls

  ssize_t size_delta; // how many logical bytes (of values) did the worker add to the DB

  std::string txn_obj_buf;
  str_arena arena;
};

class bench_runner {
public:
  bench_runner(const bench_runner &) = delete;
  bench_runner(bench_runner &&) = delete;
  bench_runner &operator=(const bench_runner &) = delete;

  bench_runner(abstract_db *db)
    : db(db), shard_index_(-1), barrier_a(BenchmarkConfig::getInstance().getNthreads()), barrier_b(1) {}

  // Constructor with shard index for multi-shard mode
  bench_runner(abstract_db *db, int shard_index)
    : db(db), shard_index_(shard_index), barrier_a(BenchmarkConfig::getInstance().getNthreads()), barrier_b(1) {}

  virtual ~bench_runner() {}
  void run();
  void stop();
  int f_mode;  // failure mode: default 0, 1 => without load phase(failover)

  // Get shard index for this runner
  int get_shard_index() const { return shard_index_; }

protected:
  // only called once
  virtual std::vector<bench_loader*> make_loaders() = 0;

  // only called once
  virtual std::vector<bench_worker*> make_workers() = 0;

  std::map<std::string, abstract_ordered_index *> get_open_tables();
  abstract_db *const db;
  int shard_index_;  // Shard index for multi-shard mode (-1 = use default)
  std::map<std::string, abstract_ordered_index *> open_tables;

  // barriers for actual benchmark execution
  spin_barrier barrier_a;
  spin_barrier barrier_b;
};

// XXX(stephentu): limit_callback is not optimal, should use
// static_limit_callback if possible
class limit_callback : public abstract_ordered_index::scan_callback {
public:
  limit_callback(ssize_t limit = -1)
    : limit(limit), n(0)
  {
    ALWAYS_ASSERT(limit == -1 || limit > 0);
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const std::string &value)
  {
    INVARIANT(limit == -1 || n < size_t(limit));
    values.emplace_back(std::string(keyp, keylen), value);
    return (limit == -1) || (++n < size_t(limit));
  }

  typedef std::pair<std::string, std::string> kv_pair;
  std::vector<kv_pair> values;

  const ssize_t limit;
private:
  size_t n;
};


class latest_key_callback : public abstract_ordered_index::scan_callback {
public:
  latest_key_callback(std::string &k, ssize_t limit = -1)
    : limit(limit), n(0), k(&k)
  {
    ALWAYS_ASSERT(limit == -1 || limit > 0);
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const std::string &value)
  {
    INVARIANT(limit == -1 || n < size_t(limit));
    k->assign(keyp, keylen);
    ++n;
    return (limit == -1) || (n < size_t(limit));
  }

  inline size_t size() const { return n; }
  inline std::string &kstr() { return *k; }

private:
  ssize_t limit;
  size_t n;
  std::string *k;
};

// explicitly copies keys, because btree::search_range_call() interally
// re-uses a single string to pass keys (so using standard string assignment
// will force a re-allocation b/c of shared ref-counting)
//
// this isn't done for values, because each value has a distinct string from
// the string allocator, so there are no mutations while holding > 1 ref-count
template <size_t N>
class static_limit_callback : public abstract_ordered_index::scan_callback {
public:
  // XXX: push ignore_key into lower layer
  static_limit_callback(str_arena *arena, bool ignore_key)
    : n(0), arena(arena), ignore_key(ignore_key)
  {
    static_assert(N > 0, "xx");
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const std::string &value)
  {
    INVARIANT(n < N);
    //    INVARIANT(arena->manages(&value));
    if (ignore_key) {
      values.emplace_back(nullptr, &value);
    } else {
      std::string * const s_px = arena->next();
      INVARIANT(s_px && s_px->empty());
      s_px->assign(keyp, keylen);
      values.emplace_back(s_px, &value);
    }
    return ++n < N;
  }

  inline size_t
  size() const
  {
    return values.size();
  }

  typedef std::pair<const std::string *, const std::string *> kv_pair;
  typename util::vec<kv_pair, N>::type values;

private:
  size_t n;
  str_arena *arena;
  bool ignore_key;
};

#endif /* _NDB_BENCH_H_ */
