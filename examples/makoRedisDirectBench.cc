#include <stddef.h>
#include <time.h>

#include <mako.hh>
#include "rocks_interface/db.hh"
#include "storage/mbta_sharded_ordered_index.hh"
#include <examples/common.h>

import std;

namespace {

using Clock = std::chrono::steady_clock;

enum class Workload {
    Get,
    Set,
    Mixed,
    Stop,
};

struct Args {
    uint64_t keys = 1'000'000;
    int value_size = 8;
    int threads = 1;
    int duration_sec = 20;
    int warmup_sec = 2;
    int repeats = 3;
    int read_percent = 80;
    std::vector<Workload> workloads{Workload::Get, Workload::Set, Workload::Mixed};
    std::string out_csv{"mako_direct_scalability.csv"};
};

struct WorkerResult {
    uint64_t operations = 0;
    uint64_t aborts = 0;
};

struct PhaseResult {
    uint64_t operations = 0;
    uint64_t aborts = 0;
    double duration_sec = 0;
    double ops_per_sec = 0;
    double process_cpu_cores = 0;
};

const char* workload_name(Workload workload) {
    switch (workload) {
        case Workload::Get:
            return "get";
        case Workload::Set:
            return "set";
        case Workload::Mixed:
            return "mixed";
        case Workload::Stop:
            return "stop";
    }
    return "unknown";
}

uint64_t xorshift64(uint64_t& state) {
    uint64_t x = state;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    state = x;
    return x;
}

double process_cpu_seconds() {
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed");
    }
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string value;
    while (std::getline(stream, value, delimiter)) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }
    return values;
}

std::vector<Workload> parse_workloads(const std::string& text) {
    std::vector<Workload> workloads;
    for (const auto& value : split(text, ',')) {
        if (value == "get") {
            workloads.push_back(Workload::Get);
        } else if (value == "set" || value == "put") {
            workloads.push_back(Workload::Set);
        } else if (value == "mixed") {
            workloads.push_back(Workload::Mixed);
        } else {
            throw std::runtime_error("unknown workload: " + value);
        }
    }
    if (workloads.empty()) {
        throw std::runtime_error("--workloads must not be empty");
    }
    return workloads;
}

void parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };

        if (arg == "--keys") {
            args.keys = std::stoull(value());
        } else if (arg == "--value-size") {
            args.value_size = std::stoi(value());
        } else if (arg == "--threads") {
            args.threads = std::stoi(value());
        } else if (arg == "--duration") {
            args.duration_sec = std::stoi(value());
        } else if (arg == "--warmup") {
            args.warmup_sec = std::stoi(value());
        } else if (arg == "--repeats") {
            args.repeats = std::stoi(value());
        } else if (arg == "--read-percent") {
            args.read_percent = std::stoi(value());
        } else if (arg == "--workloads") {
            args.workloads = parse_workloads(value());
        } else if (arg == "--out") {
            args.out_csv = value();
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (args.keys == 0 || args.value_size < 0 || args.threads < 1 || args.threads > 32
        || args.duration_sec < 1 || args.warmup_sec < 0 || args.repeats < 1
        || args.read_percent < 0 || args.read_percent > 100) {
        throw std::runtime_error("invalid benchmark arguments");
    }
}

class DirectBenchmark {
public:
    DirectBenchmark(const Args& args, mako::DB* db, mbta_sharded_ordered_index* table)
        : args_(args),
          storage_(db->GetDB()),
          table_(table),
          phase_start_(args.threads + 1),
          phase_done_(args.threads + 1),
          preload_done_(args.threads + 1),
          results_(args.threads) {
        keys_.reserve(static_cast<size_t>(args_.keys));
        for (uint64_t i = 0; i < args_.keys; ++i) {
            keys_.push_back("table_key_" + std::to_string(i));
        }
    }

    void start() {
        workers_.reserve(args_.threads);
        for (int worker_id = 0; worker_id < args_.threads; ++worker_id) {
            workers_.emplace_back([this, worker_id]() { worker_main(worker_id); });
        }
        preload_done_.arrive_and_wait();
    }

    PhaseResult run(Workload workload, int duration_sec) {
        for (auto& result : results_) {
            result = WorkerResult{};
        }
        workload_.store(workload, std::memory_order_release);
        deadline_ns_.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch() + std::chrono::seconds(duration_sec))
                .count(),
            std::memory_order_release);

        const double cpu_start = process_cpu_seconds();
        const auto wall_start = Clock::now();
        phase_start_.arrive_and_wait();
        phase_done_.arrive_and_wait();
        const auto wall_end = Clock::now();
        const double cpu_end = process_cpu_seconds();

        PhaseResult phase;
        phase.duration_sec = std::chrono::duration<double>(wall_end - wall_start).count();
        for (const auto& result : results_) {
            phase.operations += result.operations;
            phase.aborts += result.aborts;
        }
        phase.ops_per_sec = static_cast<double>(phase.operations) / phase.duration_sec;
        phase.process_cpu_cores = (cpu_end - cpu_start) / phase.duration_sec;
        return phase;
    }

    void stop() {
        workload_.store(Workload::Stop, std::memory_order_release);
        phase_start_.arrive_and_wait();
        phase_done_.arrive_and_wait();
        for (auto& worker : workers_) {
            worker.join();
        }
        workers_.clear();
    }

private:
    bool execute_get(str_arena& arena, std::string& txn_buf, const std::string& key) {
        arena.reset();
        void* txn = storage_->new_txn(
            0, arena, txn_buf.data(), abstract_db::HINT_KV_GET_PUT);
        try {
            std::string value;
            const bool found = tx_get(table_, txn, key, value);
            if (!found) {
                storage_->abort_txn(txn);
                return false;
            }
            return storage_->commit_txn(txn);
        } catch (abstract_db::abstract_abort_exception&) {
            storage_->abort_txn(txn);
            return false;
        } catch (...) {
            storage_->abort_txn(txn);
            throw;
        }
    }

    bool execute_set(
        str_arena& arena,
        std::string& txn_buf,
        const std::string& key,
        const std::string& encoded_value) {
        arena.reset();
        void* txn = storage_->new_txn(
            0, arena, txn_buf.data(), abstract_db::HINT_KV_GET_PUT);
        try {
            tx_put(table_, txn, key, encoded_value);
            return storage_->commit_txn(txn);
        } catch (abstract_db::abstract_abort_exception&) {
            storage_->abort_txn(txn);
            return false;
        } catch (...) {
            storage_->abort_txn(txn);
            throw;
        }
    }

    void worker_main(int worker_id) {
        SiloRuntime::Current()->BindToCurrentThread();
        storage_->thread_init(false, 0);
        str_arena arena;
        std::string txn_buf(storage_->sizeof_txn_object(0), '\0');
        const std::string encoded_value = mako::Encode(
            std::string(static_cast<size_t>(args_.value_size), 'V'));

        for (uint64_t index = static_cast<uint64_t>(worker_id);
             index < args_.keys;
             index += static_cast<uint64_t>(args_.threads)) {
            while (!execute_set(
                arena,
                txn_buf,
                keys_[static_cast<size_t>(index)],
                encoded_value)) {
            }
        }
        preload_done_.arrive_and_wait();

        uint64_t rng = 0x9e3779b97f4a7c15ULL
            ^ (static_cast<uint64_t>(worker_id) + 1) * 0xbf58476d1ce4e5b9ULL;
        while (true) {
            phase_start_.arrive_and_wait();
            const Workload workload = workload_.load(std::memory_order_acquire);
            if (workload == Workload::Stop) {
                phase_done_.arrive_and_wait();
                break;
            }

            WorkerResult result;
            const int64_t deadline_ns = deadline_ns_.load(std::memory_order_acquire);
            while (std::chrono::duration_cast<std::chrono::nanoseconds>(
                       Clock::now().time_since_epoch())
                       .count()
                   < deadline_ns) {
                const uint64_t random = xorshift64(rng);
                const auto& key = keys_[static_cast<size_t>(random % args_.keys)];
                const bool is_get = workload == Workload::Get
                    || (workload == Workload::Mixed
                        && static_cast<int>(xorshift64(rng) % 100)
                            < args_.read_percent);
                const bool committed = is_get
                    ? execute_get(arena, txn_buf, key)
                    : execute_set(arena, txn_buf, key, encoded_value);
                if (committed) {
                    ++result.operations;
                } else {
                    ++result.aborts;
                }
            }
            results_[static_cast<size_t>(worker_id)] = result;
            phase_done_.arrive_and_wait();
        }
        storage_->thread_end();
    }

    const Args& args_;
    abstract_db* storage_;
    mbta_sharded_ordered_index* table_;
    std::vector<std::string> keys_;
    std::barrier<> phase_start_;
    std::barrier<> phase_done_;
    std::barrier<> preload_done_;
    std::atomic<Workload> workload_{Workload::Get};
    std::atomic<int64_t> deadline_ns_{0};
    std::vector<WorkerResult> results_;
    std::vector<std::thread> workers_;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args;
        parse_args(argc, argv, args);

        const std::string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards1-warehouses"
            + std::to_string(args.threads) + ".yml";
        auto transport_config =
            std::make_unique<transport::Configuration>(config_path);

        mako::Options options;
        options.num_threads = args.threads;
        options.num_shards = 1;
        options.shard_index = 0;
        options.paxos_proc_name = "localhost";
        options.replication.enabled = false;
        options.transport_config = transport_config.get();

        mako::DB* db = nullptr;
        mako::Status status = mako::DB::Open(options, "/tmp/mako_redis_direct_bench", &db);
        if (!status.ok() || db == nullptr) {
            std::cerr << "failed to open Mako DB: " << status.ToString() << '\n';
            return 1;
        }

        auto* table = db->GetDB()->open_sharded_index("customer_0");
        if (table == nullptr) {
            std::cerr << "failed to open customer_0\n";
            delete db;
            return 1;
        }

        std::ofstream out(args.out_csv);
        if (!out) {
            throw std::runtime_error("failed to open output CSV: " + args.out_csv);
        }
        out << "benchmark,workload,workers,key_count,value_size,read_percent,repeat,"
               "duration_sec,total_ops,ops_per_sec,ops_per_sec_per_worker,"
               "process_cpu_cores,ops_per_used_core,aborts\n";

        DirectBenchmark benchmark(args, db, table);
        std::cout << "Preloading " << args.keys << " keys with " << args.threads
                  << " direct Mako workers\n";
        benchmark.start();

        for (Workload workload : args.workloads) {
            if (args.warmup_sec > 0) {
                std::cout << "Warmup " << workload_name(workload) << " for "
                          << args.warmup_sec << " seconds\n";
                benchmark.run(workload, args.warmup_sec);
            }
            for (int repeat = 1; repeat <= args.repeats; ++repeat) {
                PhaseResult result = benchmark.run(workload, args.duration_sec);
                std::cout << workload_name(workload) << " workers=" << args.threads
                          << " repeat=" << repeat << " throughput=" << std::fixed
                          << std::setprecision(2) << result.ops_per_sec
                          << " ops/s cpu=" << result.process_cpu_cores << " cores"
                          << " aborts=" << result.aborts << '\n';
                out << "direct-mako," << workload_name(workload) << ','
                    << args.threads << ',' << args.keys << ',' << args.value_size
                    << ',' << args.read_percent << ',' << repeat << ','
                    << std::fixed << std::setprecision(6) << result.duration_sec
                    << ',' << result.operations << ',' << result.ops_per_sec << ','
                    << result.ops_per_sec / static_cast<double>(args.threads) << ','
                    << result.process_cpu_cores << ','
                    << (result.process_cpu_cores > 0
                            ? result.ops_per_sec / result.process_cpu_cores
                            : 0.0)
                    << ',' << result.aborts << '\n';
                out.flush();
            }
        }

        benchmark.stop();
        delete db;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "makoRedisDirectBench: " << error.what() << '\n';
        return 1;
    }
}
