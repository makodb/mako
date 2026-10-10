// raft_bench_jetpack.h -- Jetpack's performance-test client, workload and
// report, as a mode of raft_bench (enabled by JETPACK_N=<n_concurrent>).
//
// The same file is used unchanged by the current tree and by the C++ baseline
// (412c225a) worktree, so the two arms differ only in the Raft underneath.
// Include it after `import std;` and the C headers; it includes nothing.
//
// Copied from Jetpack (github.com/stonysystems/jetpack, commit c03e318e):
//   ZipfDist             <- src/bench/tpca/zipf.h            (verbatim logic)
//   JpDistribution       <- src/deptran/scheduler.h:38-126    (pct/ave/statistics verbatim)
//   the client loop      <- src/deptran/client_worker.cc:313-483 (open-loop branch)
//   the latency window   <- src/deptran/none/coordinator.cc:46-80
//   the .res and CSV     <- src/deptran/s_main.cc:1013-1049
//
// Jetpack is MIT licensed:
//   MIT License. Copyright (c) 2019 Shuai Mu. Permission is hereby granted,
//   free of charge, to any person obtaining a copy of this software and
//   associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use,
//   copy, modify, merge, publish, distribute, sublicense, and/or sell copies
//   of the Software, and to permit persons to whom the Software is furnished
//   to do so, subject to the following conditions: The above copyright notice
//   and this permission notice shall be included in all copies or substantial
//   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//   WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// What differs from Jetpack, deliberately and by necessity:
//   * The virtual clients run inside the leader process and submit through
//     add_log_to_nc (RaftWorker), so there is no client->leader RPC hop.
//   * A request is one Raft entry carrying (site, op, key, value, dispatch
//     time); "apply" is a read or write on an in-memory table on every
//     replica, and the leader's reply is its own apply callback.
//   * The clock is monotonic (now_us from raft_bench) instead of gettimeofday.
#pragma once

namespace jetpack {

// @unsafe - copied C++; plain std containers, single-threaded use per object
class ZipfDist {
 public:
  struct probvals {
    double prob;
    double cum_prob;
  };
  std::vector<probvals> zdist = {};

  void get_zipf(double theta, int N) {
    double sum = 0.0;
    double c = 0.0;
    double sumc = 0.0;
    int i;
    for (i = 1; i <= N; i++) {
      sum += 1.0 / (double) std::pow((double) i, (double) (theta));
    }
    c = 1.0 / sum;
    for (i = 0; i < N; i++) {
      zdist[i].prob = c / (double) std::pow((double) (i + 1), (double) (theta));
      sumc += zdist[i].prob;
      zdist[i].cum_prob = sumc;
    }
    zdist[N - 1].cum_prob = 1.0;
  }

  ZipfDist(double theta, int N) {
    if (N <= 0 || theta < 0.0 || theta > 1.0) {
      std::fprintf(stderr, "wrong arguments for zipf\n");
      std::abort();
    }
    zdist.resize(N);
    get_zipf(theta, N);
  }

  int operator()(std::mt19937& rand_gen) {
    auto x = rand_gen();
    double xx = (x - rand_gen.min()) / (double) (rand_gen.max() - rand_gen.min());
    int start_search = 0;
    int end_search = (int) zdist.size() - 1;
    int middle = 0;
    while (start_search <= end_search) {
      if (start_search == end_search) {
        return start_search;
      }
      middle = (start_search + end_search) / 2;
      if (middle == 0) {
        return middle;
      }
      auto& z = zdist[middle - 1];
      auto& zz = zdist[middle];
      if (xx > z.cum_prob && xx <= zz.cum_prob) {
        return middle;
      } else if (xx < zz.cum_prob) {
        end_search = middle - 1;
      } else if (xx > zz.cum_prob) {
        start_search = middle + 1;
      } else {
        std::abort();
      }
    }
    return start_search < (int) zdist.size() ? start_search : (int) zdist.size() - 1;
  }
};

// Jetpack's Distribution (scheduler.h:38): pct() sorts in place and picks
// floor(n * p); statistics() is the exact line format its scripts parse.
// @unsafe - copied C++
class JpDistribution {
 public:
  std::vector<double> data_;
  void append(double x) { data_.push_back(x); }
  size_t count() { return data_.size(); }
  double pct(double pct) {
    if (data_.size() == 0) return -1;
    std::sort(data_.begin(), data_.end());
    size_t pick = (size_t) std::floor(data_.size() * pct);
    if (pick == data_.size()) pick -= 1;
    return data_[pick];
  }
  double ave() {
    if (data_.size() == 0) return -1;
    double sum = 0;
    for (size_t i = 0; i < data_.size(); i++) sum += data_[i];
    return sum / data_.size();
  }
  // Byte-identical to Jetpack's setw(7)/setw(9) + fixed/setprecision(2)
  // stream (the count is printed before std::fixed takes effect).
  std::string statistics() {
    char buf[160];
    const size_t n = count();
    const double p0 = pct(0.0), p50 = pct(0.5), p90 = pct(0.9), p99 = pct(0.99), av = ave();
    std::snprintf(buf, sizeof buf, "%7s%9zu%7s%9.2f%7s%9.2f%7s%9.2f%7s%9.2f%7s%9.2f", "count", n,
                  " 0pct", p0, "50pct", p50, "90pct", p90, "99pct", p99, "  ave", av);
    return buf;
  }
};

// Entry layout after raft_bench's 40-byte header (magic, seq, stamp):
//   [40]     'J'
//   [41..43) site (u16)      [43] op (0 read, 1 write)
//   [44..48) key (u32)       [48..52) value (u32)
//   [52..60) dispatch time, us since the driver started (u64)
constexpr int kJpHeader = 40;
constexpr int kJpBytes = 64;

struct Config {
  bool enabled = false;
  int sites = 60;             // 60c1s5r10p: c01..c60
  int n_concurrent = 1;       // config/concurrent_<N>.yml
  int max_undone = 300;       // config/client_open_raft.yml
  long key_range = 1000000;   // config/rw_1000000.yml
  double zipf_theta = 0.0;    // 0 = uniform; rw_zipf_<theta>.yml otherwise
  double read_ratio = 0.5;    // config/YCSB_A.yml
  int hosts = 10;             // 60c1s5r10p: c<k> runs on server<(k-1) % 10>
  std::string out_dir;        // where <prefix>-server<i>.res/.csv go
  std::string prefix;         // <protocol>-<site>-<workload>-concurrent_<N>-<mode>-<ycsb>
};

// @safe - reads the environment once
inline Config& config() {
  static Config c = [] {
    Config k;
    auto env = [](const char* n) -> const char* {
      const char* v = std::getenv(n);
      return (v && *v) ? v : nullptr;
    };
    if (const char* v = env("JETPACK_N")) {
      k.enabled = true;
      k.n_concurrent = std::atoi(v);
    }
    if (const char* v = env("JETPACK_SITES")) k.sites = std::atoi(v);
    if (const char* v = env("JETPACK_MAX_UNDONE")) k.max_undone = std::atoi(v);
    if (const char* v = env("JETPACK_KEYS")) k.key_range = std::atol(v);
    if (const char* v = env("JETPACK_ZIPF")) k.zipf_theta = std::atof(v);
    if (const char* v = env("JETPACK_READ_RATIO")) k.read_ratio = std::atof(v);
    if (const char* v = env("JETPACK_HOSTS")) k.hosts = std::max(1, std::atoi(v));
    if (const char* v = env("JETPACK_OUT_DIR")) k.out_dir = v;
    if (const char* v = env("JETPACK_PREFIX")) k.prefix = v;
    return k;
  }();
  return c;
}

inline bool enabled() { return config().enabled; }

// State shared between the driver thread (leader) and the apply thread.
struct State {
  std::atomic<bool> driving{false};
  uint64_t start_us = 0;      // driver start, microseconds (raft_bench now_us)
  double duration_s = 30.0;
  std::vector<std::atomic<long long>> issued;
  std::vector<std::atomic<long long>> done;
  std::atomic<long long> issued_total{0};
  std::atomic<long long> done_total{0};
  std::mutex mu;              // guards the per-host statistics and kv
  // One per Jetpack host process: the client sites that host would run
  // (site k on host k % hosts, as 60c1s5r10p.yml's process map).
  struct Host {
    JpDistribution all;       // cli2cli[5] == [3] == [4] for vanilla Raft
    JpDistribution reads;     // cli2cli[6]
    JpDistribution writes;    // cli2cli[7]
    JpDistribution dispatch;  // dispatch_time_distribution
    std::vector<std::pair<double, double>> commit_time;  // (dispatch ms, latency ms)
  };
  std::vector<Host> hosts;
  std::unordered_map<uint32_t, uint32_t> kv;          // the state machine
  std::atomic<long long> kv_reads{0};
  std::atomic<long long> kv_writes{0};
};

inline State& state() {
  static State s;
  return s;
}

inline void put16(char* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void put32(char* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void put64(char* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline uint16_t get16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t get32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t get64(const char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// Apply one entry on any replica. Returns true if it was a Jetpack request.
// Called first in raft_bench's apply callback; the callback's own bookkeeping
// (integrity, its own latency sample) still runs afterwards.
// @unsafe - reads raw entry bytes
inline bool on_apply(const char* log, int len, uint64_t apply_us) {
  if (len < kJpBytes || log == nullptr || log[kJpHeader] != 'J') {
    return false;
  }
  State& s = state();
  const uint16_t site = get16(log + 41);
  const bool is_write = log[43] == 1;
  const uint32_t key = get32(log + 44);
  const uint32_t value = get32(log + 48);
  const uint64_t dispatch_rel_us = get64(log + 52);
  // The state machine: every replica executes every request (Jetpack
  // replicates reads too; none/scheduler.cc -> OnCommit).
  {
    std::lock_guard<std::mutex> g(s.mu);
    if (is_write) {
      s.kv[key] = value;
      s.kv_writes.fetch_add(1, std::memory_order_relaxed);
    } else {
      auto it = s.kv.find(key);
      (void) it;
      s.kv_reads.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (!s.driving.load(std::memory_order_acquire)) {
    return true;  // a follower, or the leader before/after driving
  }
  // The leader's own apply is the reply (Jetpack: the dispatch reply leaves
  // after the leader's CommitReplicated sets commit_result).
  const double latency_ms =
      (double) (apply_us - (s.start_us + dispatch_rel_us)) / 1000.0;
  const double dispatch_ms = (double) dispatch_rel_us / 1000.0;
  // latency_window (none/coordinator.cc:46): 3 x dispatch-since-start in
  // (duration, 2 x duration), all in ms.
  const double d3 = dispatch_ms * 3;
  const bool in_window =
      d3 > s.duration_s * 1000 && d3 < s.duration_s * 2 * 1000;
  {
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.hosts.empty()) {
      State::Host& h = s.hosts[site % s.hosts.size()];
      h.dispatch.append(dispatch_ms);
      h.commit_time.emplace_back(dispatch_ms, latency_ms);
      if (in_window) {
        h.all.append(latency_ms);
        (is_write ? h.writes : h.reads).append(latency_ms);
      }
    }
  }
  if (site < s.done.size()) {
    s.done[site].fetch_add(1, std::memory_order_relaxed);
  }
  s.done_total.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// The driver: Config.sites client workers x n_concurrent open-loop virtual
// clients, in one thread, ordered by next send time. Per virtual client, as
// client_worker.cc:329-452 (open-loop branch):
//   loop: wait U(0, 1e6 / n_concurrent) us; stop if past the end;
//         while the worker's undone >= max_undone: wait 10 ms;
//         dispatch; wait U(0.5, 1.5) s.
// `submit` sends one entry (add_log_to_nc on partition 0); `header` writes
// raft_bench's 40-byte header for (seq, stamp); `now_us` is raft_bench's clock.
// @unsafe - drives the replication helper through the caller's callbacks
template <typename SubmitFn, typename HeaderFn, typename NowFn, typename SubmittedFn>
void run_leader(double duration_s, SubmitFn submit, HeaderFn header, NowFn now_us,
                SubmittedFn on_submitted) {
  Config& c = config();
  State& s = state();
  s.duration_s = duration_s;
  s.issued = std::vector<std::atomic<long long>>(c.sites);
  s.done = std::vector<std::atomic<long long>>(c.sites);
  {
    std::lock_guard<std::mutex> g(s.mu);
    s.hosts = std::vector<State::Host>(c.hosts);
  }
  s.start_us = now_us();
  s.driving.store(true, std::memory_order_release);
  const uint64_t end_us = s.start_us + (uint64_t) (duration_s * 1e6);

  std::mt19937 gen(std::random_device{}());
  std::unique_ptr<ZipfDist> zipf;
  if (c.zipf_theta > 0) zipf = std::make_unique<ZipfDist>(c.zipf_theta, (int) c.key_range);
  std::uniform_int_distribution<long> uni_key(0, c.key_range - 1);
  std::bernoulli_distribution is_read(c.read_ratio);
  std::uniform_int_distribution<int> pre_wait(0, 1000000 / std::max(1, c.n_concurrent));
  std::uniform_int_distribution<int> post_wait(500000, 1500000);

  using Ev = std::pair<uint64_t, uint32_t>;  // (next time, site)
  std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> heap;
  for (int site = 0; site < c.sites; ++site) {
    for (int v = 0; v < c.n_concurrent; ++v) {
      heap.emplace(s.start_us + (uint64_t) pre_wait(gen), (uint32_t) site);
    }
  }
  std::vector<char> buf(kJpBytes, 'x');
  uint64_t seq = 0;
  while (!heap.empty()) {
    Ev ev = heap.top();
    heap.pop();
    // Wait for the event's time: sleep when far, spin when close.
    for (;;) {
      const uint64_t t = now_us();
      if (t >= ev.first) break;
      const uint64_t gap = ev.first - t;
      if (gap > 300) std::this_thread::sleep_for(std::chrono::microseconds(gap - 200));
      else std::this_thread::yield();
    }
    const uint64_t t = now_us();
    if (t > end_us) {
      continue;  // this virtual client stops issuing; drain the rest
    }
    const uint32_t site = ev.second;
    const long long undone = s.issued[site].load(std::memory_order_relaxed) -
                             s.done[site].load(std::memory_order_relaxed);
    if (c.max_undone > 0 && undone >= c.max_undone) {
      heap.emplace(t + 10000, site);  // Wait(10^4 us), client_worker.cc:386
      continue;
    }
    ++seq;
    header(buf.data(), seq, t);
    buf[kJpHeader] = 'J';
    put16(buf.data() + 41, (uint16_t) site);
    const bool read = is_read(gen);
    buf[43] = read ? 0 : 1;
    const uint32_t key = (uint32_t) (zipf ? (*zipf)(gen) : uni_key(gen));
    put32(buf.data() + 44, key);
    put32(buf.data() + 48, (uint32_t) seq);
    put64(buf.data() + 52, t - s.start_us);
    s.issued[site].fetch_add(1, std::memory_order_relaxed);
    s.issued_total.fetch_add(1, std::memory_order_relaxed);
    on_submitted(t);
    if (!submit(buf.data(), kJpBytes)) {
      s.issued[site].fetch_sub(1, std::memory_order_relaxed);
      s.issued_total.fetch_sub(1, std::memory_order_relaxed);
      break;  // lost leadership
    }
    // Next dispatch: the post-dispatch sleep, then the next iteration's
    // pre-dispatch wait.
    heap.emplace(t + (uint64_t) post_wait(gen) + (uint64_t) pre_wait(gen), site);
  }
  // Like Jetpack, wait for all outstanding requests (bounded here, 30 s).
  const uint64_t drain_deadline = now_us() + 30000000ULL;
  while (s.done_total.load() < s.issued_total.load() && now_us() < drain_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  s.driving.store(false, std::memory_order_release);
}

// Writes, for every host group i, <out_dir>/<prefix>-server<i>.res (the lines
// s_main.cc:1013-1016 prints, which is what Jetpack's .res files are) and
// <prefix>-server<i>.csv (the 11-column dump, s_main.cc:1019-1049). Each
// .res also carries a "[jetpack] ..." line recording the run's settings,
// including the injected network delay.
// @unsafe - C stdio
inline void report() {
  Config& c = config();
  State& s = state();
  std::lock_guard<std::mutex> g(s.mu);
  const char* wan = std::getenv("WAN_DELAY_MS");
  long long total_mid_count = 0;
  for (size_t i = 0; i < s.hosts.size(); ++i) {
    State::Host& h = s.hosts[i];
    const double mid = (double) h.all.count() / (s.duration_s / 3.0);
    total_mid_count += (long long) h.all.count();
    std::string all = h.all.statistics();
    std::string reads = h.reads.statistics();
    std::string writes = h.writes.statistics();
    std::string base = c.out_dir.empty() ? std::string() : c.out_dir + "/" + c.prefix +
                                                              "-server" + std::to_string(i);
    char line[640];
    std::string out;
    std::snprintf(line, sizeof line, "All-original-path-attempts       statistics %s\n", all.c_str());
    out += line;
    std::snprintf(line, sizeof line, "Efficient-original-path-attempts statistics %s\n", all.c_str());
    out += line;
    std::snprintf(line, sizeof line, "All-efficient-attempts           statistics %s\n", all.c_str());
    out += line;
    std::snprintf(line, sizeof line, "Read-mid-10s                     statistics %s\n", reads.c_str());
    out += line;
    std::snprintf(line, sizeof line, "Write-mid-10s                    statistics %s\n", writes.c_str());
    out += line;
    std::snprintf(line, sizeof line, "Mid throughput is %.2f\n", mid);
    out += line;
    std::snprintf(line, sizeof line,
                  "[jetpack] host=server%zu of %d (a group of client sites in the leader process) "
                  "sites=%d n_concurrent=%d max_undone=%d keys=%ld zipf=%.2f read_ratio=%.2f "
                  "duration_s=%.1f wan_delay_ms=%s issued_total=%lld done_total=%lld\n",
                  i, c.hosts, c.sites, c.n_concurrent, c.max_undone, c.key_range, c.zipf_theta,
                  c.read_ratio, s.duration_s, wan ? wan : "0", s.issued_total.load(),
                  s.done_total.load());
    out += line;
    if (!base.empty()) {
      const std::string csv = base + ".csv";
      if (std::FILE* f = std::fopen(csv.c_str(), "w")) {
        std::fprintf(f, "All-fast-path-attempts,Success-fast-path-attempts,Efficient-fast-path-attempts,"
                        "All-original-path-attempts,Efficient-original-path-attempts,All-efficient-attempts,"
                        "Start-Time,End2End-Latency,Dispatch-Time,Mid-Read-Lat,Mid-Write-Lat\n");
        std::sort(h.commit_time.begin(), h.commit_time.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        size_t rows = std::max({h.commit_time.size(), h.all.count(), h.dispatch.count()});
        for (size_t r = 0; r < rows; ++r) {
          std::fprintf(f, ",,,");
          for (int k = 0; k < 3; ++k) {
            if (r < h.all.count()) std::fprintf(f, "%f", h.all.data_[r]);
            std::fprintf(f, ",");
          }
          if (r < h.commit_time.size()) std::fprintf(f, "%f", h.commit_time[r].first);
          std::fprintf(f, ",");
          if (r < h.commit_time.size()) std::fprintf(f, "%f", h.commit_time[r].second);
          std::fprintf(f, ",");
          if (r < h.dispatch.count()) std::fprintf(f, "%f", h.dispatch.data_[r]);
          std::fprintf(f, ",");
          if (r < h.reads.count()) std::fprintf(f, "%f", h.reads.data_[r]);
          std::fprintf(f, ",");
          if (r < h.writes.count()) std::fprintf(f, "%f", h.writes.data_[r]);
          std::fprintf(f, "\n");
        }
        std::fclose(f);
        std::snprintf(line, sizeof line, "Dumped to %s with %zu lines data\n", csv.c_str(), rows);
        out += line;
      }
      if (std::FILE* f = std::fopen((base + ".res").c_str(), "w")) {
        std::fputs(out.c_str(), f);
        std::fclose(f);
      }
    }
  }
  std::printf("[jetpack] total Mid throughput is %.2f (sum over %zu host groups); wan_delay_ms=%s\n",
              (double) total_mid_count / (s.duration_s / 3.0), s.hosts.size(), wan ? wan : "0");
  std::fflush(stdout);
}

}  // namespace jetpack
