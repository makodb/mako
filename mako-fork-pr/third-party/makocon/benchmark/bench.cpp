// // bench.cpp
// // Build: g++ bench.cpp -O3 -std=gnu++17 -lhiredis -lpthread -o bench
// //
// // Example:
// //   ./bench --name redis --host 127.0.0.1 --port 6378 --out redis.csv
// //   ./bench --name mako  --host 127.0.0.1 --port 6380 --out mako.csv
// //
// // Summary:
// //   - Preload keys with the largest value size
// //   - Warm up with GETs
// //   - Run a matrix over (clients × value_size) for GET, then SET
// //   - Write CSV for this single target only

// #include <hiredis/hiredis.h>
// #include <algorithm>
// #include <atomic>
// #include <chrono>
// #include <csignal>
// #include <cstdint>
// #include <cstring>
// #include <fstream>
// #include <iostream>
// #include <numeric>
// #include <random>
// #include <sstream>
// #include <stdexcept>
// #include <string>
// #include <thread>
// #include <vector>

// using Clock = std::chrono::steady_clock;
// using ns = std::chrono::nanoseconds;

// struct Target {
//     std::string name{"redis"};
//     std::string host{"127.0.0.1"};
//     int port{6379};
// };

// struct Args {
//     Target t;                           // single target
//     uint64_t keys{1000000};
//     int warmup_sec{10};
//     // std::vector<int> clients{16, 32, 64};
//     std::vector<int> clients{64};
//     // std::vector<int> values{64, 256, 1024};
//     std::vector<int> values{1024};
//     int duration{150};
//     std::string out_csv{"results.csv"};
// };

// struct BenchRow {
//     Target t;
//     std::string op;       // "get" or "set"
//     int clients;
//     int value_size;
//     int seconds;
//     uint64_t ops;         // total ops completed
//     double ops_sec;       // ops/second
//     double p50_us;
//     double p95_us;
//     double p99_us;
// };

// struct CsvWriter {
//     explicit CsvWriter(const std::string &path) : ofs(path) {
//         if (!ofs) throw std::runtime_error("Cannot open CSV: " + path);
//     }
//     void write_header() {
//         ofs << "server,host,port,op,clients,value_size,seconds,ops,ops_per_sec,p50_us,p95_us,p99_us\n";
//     }
//     void write(const std::vector<BenchRow> &rows) {
//         for (const auto &r : rows) {
//             ofs << r.t.name << ','
//                 << r.t.host << ','
//                 << r.t.port << ','
//                 << r.op << ','
//                 << r.clients << ','
//                 << r.value_size << ','
//                 << r.seconds << ','
//                 << r.ops << ','
//                 << std::fixed << r.ops_sec << ','
//                 << r.p50_us << ','
//                 << r.p95_us << ','
//                 << r.p99_us << '\n';
//         }
//         ofs.flush();
//     }
// private:
//     std::ofstream ofs;
// };

// static std::atomic<bool> g_stop{false};

// static redisContext* connect_retry(const std::string &host, int port, int tries = 20, int ms = 200) {
//     for (int i = 0; i < tries; i++) {
//         timeval tv{2, 0}; // 2s connect timeout
//         redisContext *c = redisConnectWithTimeout(host.c_str(), port, tv);
//         if (c != nullptr) {
//             if (!c->err) return c;
//             redisFree(c);
//         }
//         std::this_thread::sleep_for(std::chrono::milliseconds(ms));
//     }
//     return nullptr;
// }

// static void ping_or_exit(const Target &t) {
//     redisContext *c = connect_retry(t.host, t.port);
//     if (c == nullptr) {
//         throw std::runtime_error("Connect failed: " + t.host + ":" + std::to_string(t.port));
//     }
//     redisReply *r = (redisReply *)redisCommand(c, "PING");
//     if (r == nullptr) {
//         redisFree(c);
//         throw std::runtime_error("PING failed: " + t.host + ":" + std::to_string(t.port));
//     }
//     freeReplyObject(r);
//     redisFree(c);
// }

// static void preload(const Target &t, uint64_t keys, int vsize) {
//     redisContext *c = connect_retry(t.host, t.port);
//     if (c == nullptr) {
//         throw std::runtime_error("Preload connect failed: " + t.host + ":" + std::to_string(t.port));
//     }
//     std::string val(vsize, 'X');
//     for (uint64_t k = 1; k <= keys; k++) {
//         std::string key = "key:" + std::to_string(k);
//         redisReply *r = (redisReply *)redisCommand(c, "SET %s %b", key.c_str(), val.data(), (size_t)vsize);
//         if (r != nullptr) freeReplyObject(r);
//     }
//     redisFree(c);
// }

// static void warmup(const Target &t, uint64_t keys, int sec) {
//     std::vector<std::thread> th;
//     int clients = 16; // light warmup fanout
//     for (int i = 0; i < clients; i++) {
//         th.emplace_back([&, i]() {
//             redisContext *c = connect_retry(t.host, t.port);
//             if (c == nullptr) return;
//             std::mt19937_64 rng(0xBEEF + i);
//             std::uniform_int_distribution<uint64_t> d(1, keys);
//             auto end = Clock::now() + std::chrono::seconds(sec);
//             while (Clock::now() < end) {
//                 std::string key = "key:" + std::to_string(d(rng));
//                 redisReply *r = (redisReply *)redisCommand(c, "GET %s", key.c_str());
//                 if (r != nullptr) freeReplyObject(r);
//             }
//             redisFree(c);
//         });
//     }
//     for (auto &x : th) x.join();
// }

// static inline double pct_us(std::vector<uint64_t> &ns_sorted, double p) {
//     if (ns_sorted.empty()) return 0.0;
//     size_t idx = static_cast<size_t>(p * (ns_sorted.size() - 1));
//     double ns_val = static_cast<double>(ns_sorted[idx]);
//     return ns_val / 1000.0;
// }

// struct WorkerCfg {
//     Target t;
//     std::string op;   // "get" or "set"
//     uint64_t keys;
//     int value_size;
//     int seconds;
//     uint64_t seed;
// };

// struct Stats {
//     uint64_t ops{0};
//     std::vector<uint64_t> lats;
// };

// static Stats run_workers_no_pipeline(const WorkerCfg &cfg, int clients) {
//     std::vector<std::thread> th;
//     std::vector<Stats> per(clients);
//     std::atomic<bool> start{false};

//     for (int i = 0; i < clients; i++) {
//         th.emplace_back([&, i]() {
//             redisContext *c = connect_retry(cfg.t.host, cfg.t.port);
//             if (c == nullptr) return;

//             std::string val(cfg.value_size, 'Y');
//             std::mt19937_64 rng(cfg.seed + i * 1337ULL);
//             std::uniform_int_distribution<uint64_t> d(1, cfg.keys);
//             per[i].lats.reserve(2000);

//             while (!start.load()) std::this_thread::yield();

//             auto end = Clock::now() + std::chrono::seconds(cfg.seconds);
//             while (Clock::now() < end && !g_stop.load()) {
//                 std::string key = "key:" + std::to_string(d(rng));
//                 auto t0 = Clock::now();
//                 redisReply *r = nullptr;
//                 if (cfg.op == "get") {
//                     r = (redisReply *)redisCommand(c, "GET %s", key.c_str());
//                 } else {
//                     r = (redisReply *)redisCommand(c, "SET %s %b", key.c_str(), val.data(), (size_t)cfg.value_size);
//                 }
//                 auto t1 = Clock::now();
//                 if (r != nullptr) {
//                     freeReplyObject(r);
//                     per[i].ops++;
//                 }
//                 if (per[i].lats.size() < per[i].lats.capacity()) {
//                     per[i].lats.push_back(std::chrono::duration_cast<ns>(t1 - t0).count());
//                 }
//             }
//             redisFree(c);
//         });
//     }

//     start.store(true);
//     for (auto &x : th) x.join();

//     Stats out;
//     for (auto &s : per) out.ops += s.ops;

//     std::vector<uint64_t> all;
//     size_t n = 0;
//     for (auto &s : per) n += s.lats.size();
//     all.reserve(n);
//     for (auto &s : per) all.insert(all.end(), s.lats.begin(), s.lats.end());
//     std::sort(all.begin(), all.end());
//     out.lats = std::move(all);
//     return out;
// }

// struct BenchEngine {
//     void prepare(const Args &a) {
//         ping_or_exit(a.t);
//         int max_v = *std::max_element(a.values.begin(), a.values.end());
//         preload(a.t, a.keys, max_v);
//         warmup(a.t, a.keys, a.warmup_sec);
//     }

//     std::vector<BenchRow> run_matrix(const Args &a) {
//         std::vector<BenchRow> rows;
//         rows.reserve(a.values.size() * a.clients.size() * 2 /*get+set*/);

//         for (int v : a.values) {
//             for (int c : a.clients) {
//                 std::cout << "[RUN] target=" << a.t.name
//                           << " clients=" << c
//                           << " value=" << v
//                           << "s=" << a.duration
//                           << " (GET then SET)" << std::endl;

//                 // GET
//                 // {
//                 //     WorkerCfg cfg{a.t, "get", a.keys, v, a.duration, 0xC0FFEE};
//                 //     Stats s = run_workers_no_pipeline(cfg, c);

//                 //     BenchRow r;
//                 //     r.t = a.t; r.op = "get"; r.clients = c; r.value_size = v; r.seconds = a.duration;
//                 //     r.ops = s.ops; r.ops_sec = static_cast<double>(s.ops) / static_cast<double>(a.duration);
//                 //     r.p50_us = pct_us(s.lats, 0.50);
//                 //     r.p95_us = pct_us(s.lats, 0.95);
//                 //     r.p99_us = pct_us(s.lats, 0.99);
//                 //     rows.push_back(std::move(r));
//                 // }

//                 // SET
//                 {
//                     WorkerCfg cfg{a.t, "set", a.keys, v, a.duration, 0xC0FFEE ^ 0x1234};
//                     Stats s = run_workers_no_pipeline(cfg, c);

//                     BenchRow r;
//                     r.t = a.t; r.op = "set"; r.clients = c; r.value_size = v; r.seconds = a.duration;
//                     r.ops = s.ops; r.ops_sec = static_cast<double>(s.ops) / static_cast<double>(a.duration);
//                     r.p50_us = pct_us(s.lats, 0.50);
//                     r.p95_us = pct_us(s.lats, 0.95);
//                     r.p99_us = pct_us(s.lats, 0.99);
//                     rows.push_back(std::move(r));
//                 }
//             }
//         }
//         return rows;
//     }
// };

// static void on_sigint(int) { g_stop.store(true); }

// static void usage(const char *p) {
//     std::cerr
//         << "Usage: " << p << " [options]\n"
//         << "  --name N      (default redis)\n"
//         << "  --host H      (default 127.0.0.1)\n"
//         << "  --port P      (default 6379)\n"
//         << "  --keys N            (default 1000000)\n"
//         << "  --warmup-sec S      (default 10)\n"
//         << "  --clients list      (default 16,32,64)\n"
//         << "  --values list       (bytes; default 64,256,1024)\n"
//         << "  --duration S        (default 30)\n"
//         << "  --out file.csv      (default results.csv)\n";
// }

// static std::vector<int> parse_list(std::string s) {
//     std::vector<int> v;
//     std::stringstream ss(std::move(s));
//     std::string t;
//     while (std::getline(ss, t, ',')) {
//         if (!t.empty()) v.push_back(std::stoi(t));
//     }
//     return v;
// }

// static void parse_args(int argc, char **argv, Args &a) {
//     auto need = [&](int i) {
//         if (i >= argc) { usage(argv[0]); std::exit(1); }
//     };
//     for (int i = 1; i < argc; i++) {
//         std::string k = argv[i];
//         if (k == "--name") { need(++i); a.t.name = argv[i]; }
//         else if (k == "--host") { need(++i); a.t.host = argv[i]; }
//         else if (k == "--port") { need(++i); a.t.port = std::stoi(argv[i]); }
//         else if (k == "--keys") { need(++i); a.keys = std::stoull(argv[i]); }
//         else if (k == "--warmup-sec") { need(++i); a.warmup_sec = std::stoi(argv[i]); }
//         else if (k == "--clients") { need(++i); a.clients = parse_list(argv[i]); }
//         else if (k == "--values") { need(++i); a.values = parse_list(argv[i]); }
//         else if (k == "--duration") { need(++i); a.duration = std::stoi(argv[i]); }
//         else if (k == "--out") { need(++i); a.out_csv = argv[i]; }
//         else { usage(argv[0]); std::exit(1); }
//     }
// }

// int main(int argc, char **argv) {
//     std::signal(SIGINT, on_sigint);

//     Args a;
//     parse_args(argc, argv, a);

//     std::cout << "Target " << a.t.name << " " << a.t.host << ":" << a.t.port << "\n";

//     try {
//         BenchEngine engine;
//         engine.prepare(a);
//         auto results = engine.run_matrix(a);

//         CsvWriter csv(a.out_csv);
//         csv.write_header();
//         csv.write(results);
//     } catch (const std::exception &e) {
//         std::cerr << "ERROR: " << e.what() << "\n";
//         return 1;
//     }

//     std::cout << "Done -> " << a.out_csv << "\n";
//     return 0;
// }






// bench_masstree_style.cpp
// Build: g++ -O3 -march=native -DNDEBUG -std=c++17 bench.cpp -lhiredis -lpthread -o bench
//
// Masstree Section 7 style benchmark:
//   - Preload 20M keys with 8-byte values
//   - Test uniform distribution with 1-to-10-byte decimal keys
//   - Run GET and PUT workloads for 60 seconds each
//   - Test scalability across different client thread counts
//
// Usage examples:
//   ./bench --name mako --port 6380 --out mako_results.csv
//   ./bench --name redis --port 6378 --out redis_results.csv

#include <hiredis/hiredis.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Target {
    std::string name{"mako"};
    std::string host{"127.0.0.1"};
    int port{6380};
};

struct Args {
    Target t;
    uint64_t keys{1'000'000};                 // 1M keys by default
    int value_size{8};                        // 8-byte values (Masstree Section 7 style)
    std::vector<int> thread_counts{1, 4, 16}; // Client thread scalability test
    int duration_sec{60};                     // 60 seconds per workload
    std::string out_csv{"masstree_style_results.csv"};
    bool skip_preload{false};
    int preload_report_interval{50'000};      // Report every 50k keys
};

struct BenchRow {
    Target t;
    std::string workload;    // "get" or "put"
    std::string key_dist;    // "1-to-10-byte-decimal"
    int threads;
    int value_size;
    double duration_sec;
    uint64_t total_ops;
    double ops_per_sec;
    double ops_per_sec_per_thread;
    double p50_us;  // unused for now (0)
    double p95_us;  // unused for now (0)
    double p99_us;  // unused for now (0)
};

struct CsvWriter {
    explicit CsvWriter(const std::string &path) : ofs(path) {
        if (!ofs) throw std::runtime_error("Cannot open CSV: " + path);
    }
    void write_header() {
        ofs << "server,host,port,workload,key_dist,threads,value_size,duration_sec,"
            << "total_ops,ops_per_sec,ops_per_sec_per_thread,p50_us,p95_us,p99_us\n";
    }
    void write(const BenchRow &r) {
        ofs << r.t.name << ','
            << r.t.host << ','
            << r.t.port << ','
            << r.workload << ','
            << r.key_dist << ','
            << r.threads << ','
            << r.value_size << ','
            << std::fixed << std::setprecision(2) << r.duration_sec << ','
            << r.total_ops << ','
            << std::fixed << std::setprecision(2) << r.ops_per_sec << ','
            << std::fixed << std::setprecision(2) << r.ops_per_sec_per_thread << ','
            << 0.0 << ',' << 0.0 << ',' << 0.0 << '\n';
        ofs.flush();
    }
private:
    std::ofstream ofs;
};

static std::atomic<bool> g_stop{false};

// ===== Key generation: build 1-to-10-byte decimal keyspace once =====
static std::vector<std::string> build_keys(uint64_t total_keys) {
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(total_keys));
    for (uint64_t i = 0; i < total_keys; ++i) {
        uint32_t num = static_cast<uint32_t>(i % 0x80000000ULL);
        keys.emplace_back("key:" + std::to_string(num));
    }
    return keys;
}

// ===== Connection utilities =====
static redisContext* connect_retry(const std::string &host, int port, int tries = 20, int ms = 200) {
    for (int i = 0; i < tries; i++) {
        timeval tv{2, 0};
        redisContext *c = redisConnectWithTimeout(host.c_str(), port, tv);
        if (c != nullptr && !c->err) return c;
        if (c) redisFree(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    return nullptr;
}

static void ping_or_exit(const Target &t) {
    redisContext *c = connect_retry(t.host, t.port);
    if (!c) throw std::runtime_error("Connect failed: " + t.host + ":" + std::to_string(t.port));
    redisReply *r = (redisReply *)redisCommand(c, "PING");
    if (!r) {
        redisFree(c);
        throw std::runtime_error("PING failed");
    }
    freeReplyObject(r);
    redisFree(c);
}

// ===== SINGLE-THREADED Preload phase (uses prebuilt key vector) =====
static void preload(const Target &t,
                    const std::vector<std::string> &keys,
                    int value_size,
                    int report_interval) {
    const uint64_t total_keys = keys.size();

    std::cout << "\n=== Preloading " << total_keys << " keys with "
              << value_size << "-byte values ===" << std::endl;
    std::cout << "Using SINGLE-THREADED preload (optimized for single-threaded server)" << std::endl;

    redisContext *c = connect_retry(t.host, t.port);
    if (!c) {
        throw std::runtime_error("Preload connect failed");
    }

    std::string val(value_size, 'X');
    auto start_time = Clock::now();
    auto last_report = start_time;

    for (uint64_t i = 0; i < total_keys && !g_stop.load(); i++) {
        const std::string &key = keys[static_cast<size_t>(i)];
        redisReply *r = (redisReply *)redisCommand(c, "SET %s %b",
                                                   key.c_str(),
                                                   val.data(),
                                                   (size_t)value_size);
        if (r) {
            freeReplyObject(r);
        } else {
            std::cerr << "\nPreload error at key " << i << ": " << c->errstr << std::endl;
            redisFree(c);
            throw std::runtime_error("Preload failed");
        }

        if ((i + 1) % report_interval == 0 || i + 1 == total_keys) {
            auto now = Clock::now();
            auto elapsed_total = std::chrono::duration_cast<std::chrono::seconds>(
                                     now - start_time)
                                     .count();
            auto elapsed_interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - last_report)
                                        .count();

            double overall_rate = (elapsed_total > 0) ? (double)(i + 1) / elapsed_total : 0.0;
            double interval_rate = (elapsed_interval > 0)
                                       ? (double)report_interval / (elapsed_interval / 1000.0)
                                       : 0.0;

            std::cout << "  Progress: " << (i + 1) << " / " << total_keys
                      << " (" << std::fixed << std::setprecision(1)
                      << (100.0 * (i + 1) / total_keys) << "%) "
                      << "Overall: " << std::fixed << std::setprecision(1)
                      << (overall_rate / 1000.0) << "k ops/sec, "
                      << "Current: " << std::fixed << std::setprecision(1)
                      << (interval_rate / 1000.0) << "k ops/sec\r" << std::flush;

            last_report = now;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       Clock::now() - start_time)
                       .count();

    std::cout << "\n  Preload complete: " << total_keys << " keys in "
              << elapsed << "s (" << std::fixed << std::setprecision(0)
              << (total_keys / (double)elapsed) << " ops/sec)\n";

    redisFree(c);
}

// ===== Worker thread stats (throughput-only) =====
struct WorkerStats {
    uint64_t ops{0};
};

// ===== Lightweight RNG: xorshift64* =====
static inline uint64_t xorshift64(uint64_t &state) {
    // state must be non-zero
    uint64_t x = state;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    state = x;
    return x;
}

// ===== GET workload (no pipelining, throughput-only) =====
static WorkerStats get_worker(const Target &t,
                              const std::vector<std::string> &keys,
                              int duration_sec,
                              uint64_t seed,
                              std::atomic<bool> &start_flag) {
    WorkerStats stats;

    redisContext *c = connect_retry(t.host, t.port);
    if (!c) return stats;

    uint64_t rng_state = seed ? seed : 0x123456789abcdefULL;
    if (rng_state == 0) rng_state = 1;

    const size_t key_count = keys.size();
    auto end_time = Clock::now() + std::chrono::seconds(duration_sec);

    // Wait for start signal
    while (!start_flag.load()) {
        std::this_thread::yield();
    }

    while (Clock::now() < end_time && !g_stop.load()) {
        uint64_t r = xorshift64(rng_state);
        size_t idx = static_cast<size_t>(r % key_count);
        const std::string &key = keys[idx];

        redisReply *reply = (redisReply *)redisCommand(c, "GET %s", key.c_str());
        if (!reply) break;
        freeReplyObject(reply);
        stats.ops++;
    }

    redisFree(c);
    return stats;
}

// ===== PUT workload (no pipelining, throughput-only) =====
static WorkerStats put_worker(const Target &t,
                              const std::vector<std::string> &keys,
                              int duration_sec,
                              int value_size,
                              uint64_t seed,
                              std::atomic<bool> &start_flag) {
    WorkerStats stats;

    redisContext *c = connect_retry(t.host, t.port);
    if (!c) return stats;

    uint64_t rng_state = seed ? seed : 0x9876543210fedcbaULL;
    if (rng_state == 0) rng_state = 1;

    const size_t key_count = keys.size();
    std::string val(value_size, 'Y');
    auto end_time = Clock::now() + std::chrono::seconds(duration_sec);

    // Wait for start signal
    while (!start_flag.load()) {
        std::this_thread::yield();
    }

    while (Clock::now() < end_time && !g_stop.load()) {
        uint64_t r = xorshift64(rng_state);
        size_t idx = static_cast<size_t>(r % key_count);
        const std::string &key = keys[idx];

        redisReply *reply = (redisReply *)redisCommand(
            c, "SET %s %b", key.c_str(), val.data(), (size_t)value_size);
        if (!reply) break;
        freeReplyObject(reply);
        stats.ops++;
    }

    redisFree(c);
    return stats;
}

// ===== Benchmark execution =====
static BenchRow run_get_workload(const Target &t,
                                 const std::vector<std::string> &keys,
                                 int threads,
                                 int value_size,
                                 int duration_sec) {
    std::cout << "\n[GET] threads=" << threads
              << " duration=" << duration_sec << "s" << std::flush;

    std::vector<std::thread> workers;
    std::vector<WorkerStats> stats(threads);
    std::atomic<bool> start_flag{false};

    for (int i = 0; i < threads; i++) {
        uint64_t seed = 0xC0FFEEULL + (uint64_t)i * 1337ULL;
        workers.emplace_back([&, i, seed]() {
            stats[i] = get_worker(t, keys, duration_sec, seed, start_flag);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto start_time = Clock::now();
    start_flag.store(true);

    for (auto &w : workers) w.join();

    auto end_time = Clock::now();
    double actual_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() /
        1000.0;

    uint64_t total_ops = 0;
    for (const auto &s : stats) {
        total_ops += s.ops;
    }

    BenchRow row;
    row.t = t;
    row.workload = "get";
    row.key_dist = "1-to-10-byte-decimal";
    row.threads = threads;
    row.value_size = value_size;
    row.duration_sec = actual_duration;
    row.total_ops = total_ops;
    row.ops_per_sec = total_ops / actual_duration;
    row.ops_per_sec_per_thread = row.ops_per_sec / threads;
    row.p50_us = row.p95_us = row.p99_us = 0.0;

    std::cout << " => " << std::fixed << std::setprecision(2)
              << (row.ops_per_sec / 1'000'000.0) << " Mops/sec\n";

    return row;
}

static BenchRow run_put_workload(const Target &t,
                                 const std::vector<std::string> &keys,
                                 int threads,
                                 int value_size,
                                 int duration_sec) {
    std::cout << "\n[PUT] threads=" << threads
              << " duration=" << duration_sec << "s" << std::flush;

    std::vector<std::thread> workers;
    std::vector<WorkerStats> stats(threads);
    std::atomic<bool> start_flag{false};

    for (int i = 0; i < threads; i++) {
        uint64_t seed = 0xBEEFULL + (uint64_t)i * 1337ULL;
        workers.emplace_back([&, i, seed]() {
            stats[i] = put_worker(t, keys, duration_sec, value_size, seed, start_flag);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto start_time = Clock::now();
    start_flag.store(true);

    for (auto &w : workers) w.join();

    auto end_time = Clock::now();
    double actual_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() /
        1000.0;

    uint64_t total_ops = 0;
    for (const auto &s : stats) {
        total_ops += s.ops;
    }

    BenchRow row;
    row.t = t;
    row.workload = "put";
    row.key_dist = "1-to-10-byte-decimal";
    row.threads = threads;
    row.value_size = value_size;
    row.duration_sec = actual_duration;
    row.total_ops = total_ops;
    row.ops_per_sec = total_ops / actual_duration;
    row.ops_per_sec_per_thread = row.ops_per_sec / threads;
    row.p50_us = row.p95_us = row.p99_us = 0.0;

    std::cout << " => " << std::fixed << std::setprecision(2)
              << (row.ops_per_sec / 1'000'000.0) << " Mops/sec\n";

    return row;
}

// ===== Main benchmark engine =====
struct MasstreeStyleBench {
    void run(const Args &a) {
        ping_or_exit(a.t);

        // Build keyspace once (used for preload + workloads)
        auto keys = build_keys(a.keys);

        // Preload phase
        if (!a.skip_preload) {
            preload(a.t, keys, a.value_size, a.preload_report_interval);
        } else {
            std::cout << "\n=== Skipping preload (--skip-preload) ===" << std::endl;
        }

        std::cout << "\n=== Starting Masstree-style benchmark ===" << std::endl;
        std::cout << "Key distribution: 1-to-10-byte decimal (uniform over preloaded set)" << std::endl;
        std::cout << "Value size: " << a.value_size << " bytes" << std::endl;
        std::cout << "Duration: " << a.duration_sec << " seconds per workload" << std::endl;
        std::cout << "Client thread counts: ";
        for (int t : a.thread_counts) std::cout << t << " ";
        std::cout << "\n" << std::endl;

        CsvWriter csv(a.out_csv);
        csv.write_header();

        std::cout << "\n====== GET WORKLOAD ======" << std::endl;
        for (int tc : a.thread_counts) {
            if (g_stop.load()) break;
            BenchRow row = run_get_workload(a.t, keys, tc, a.value_size, a.duration_sec);
            csv.write(row);
        }

        std::cout << "\n====== PUT WORKLOAD ======" << std::endl;
        for (int tc : a.thread_counts) {
            if (g_stop.load()) break;
            BenchRow row = run_put_workload(a.t, keys, tc, a.value_size, a.duration_sec);
            csv.write(row);
        }

        std::cout << "\n=== Benchmark complete ===" << std::endl;
    }
};

// ===== CLI =====
static void on_sigint(int) {
    g_stop.store(true);
    std::cout << "\n[Interrupted by user]\n";
}

static void usage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Masstree Section 7 style benchmark (optimized for single-threaded servers, no pipelining):\n"
        << "  --name NAME           Server name (default: mako)\n"
        << "  --host HOST           Server host (default: 127.0.0.1)\n"
        << "  --port PORT           Server port (default: 6380)\n"
        << "  --keys N              Total keys to preload (default: 1000000)\n"
        << "  --value-size N        Value size in bytes (default: 8)\n"
        << "  --threads LIST        Comma-separated client thread counts (default: 1,4,16)\n"
        << "  --duration N          Workload duration in seconds (default: 60)\n"
        << "  --out FILE            Output CSV file (default: masstree_style_results.csv)\n"
        << "  --skip-preload        Skip preload phase (assumes data already loaded)\n"
        << "\nExamples:\n"
        << "  # Quick test:\n"
        << "  " << prog << " --name mako --port 6380 --keys 100000 --duration 10\n"
        << "\n"
        << "  # Standard test:\n"
        << "  " << prog << " --name mako --port 6380 --keys 1000000 --duration 60 --out mako_results.csv\n"
        << "\n"
        << "  # Compare with Redis:\n"
        << "  " << prog << " --name redis --port 6378 --out redis_results.csv\n";
}

static std::vector<int> parse_int_list(const std::string &s) {
    std::vector<int> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            result.push_back(std::stoi(item));
        }
    }
    return result;
}

static void parse_args(int argc, char **argv, Args &a) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        auto need_value = [&]() {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a value\n";
                usage(argv[0]);
                std::exit(1);
            }
        };

        if (arg == "--name") {
            need_value(); a.t.name = argv[++i];
        } else if (arg == "--host") {
            need_value(); a.t.host = argv[++i];
        } else if (arg == "--port") {
            need_value(); a.t.port = std::stoi(argv[++i]);
        } else if (arg == "--keys") {
            need_value(); a.keys = std::stoull(argv[++i]);
        } else if (arg == "--value-size") {
            need_value(); a.value_size = std::stoi(argv[++i]);
        } else if (arg == "--threads") {
            need_value(); a.thread_counts = parse_int_list(argv[++i]);
        } else if (arg == "--duration") {
            need_value(); a.duration_sec = std::stoi(argv[++i]);
        } else if (arg == "--out") {
            need_value(); a.out_csv = argv[++i];
        } else if (arg == "--skip-preload") {
            a.skip_preload = true;
        } else {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            usage(argv[0]);
            std::exit(1);
        }
    }
}

int main(int argc, char **argv) {
    std::signal(SIGINT, on_sigint);

    // Speed up iostreams
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Args args;
    parse_args(argc, argv, args);

    std::cout << "========================================\n"
              << "  Masstree Section 7 Style Benchmark\n"
              << "  (Optimized for Single-Threaded Servers, No Pipelining)\n"
              << "========================================\n"
              << "Target: " << args.t.name << " @ "
              << args.t.host << ":" << args.t.port << "\n"
              << "Keys: " << args.keys << "\n"
              << "Value size: " << args.value_size << " bytes\n"
              << "Duration: " << args.duration_sec << " seconds per workload\n"
              << "Preload: Single-threaded (unless --skip-preload)\n"
              << "========================================\n";

    try {
        MasstreeStyleBench bench;
        bench.run(args);
        std::cout << "\nResults written to: " << args.out_csv << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}