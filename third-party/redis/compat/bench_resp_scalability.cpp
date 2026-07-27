#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

enum class Workload {
    Get,
    Set,
    Mixed,
};

struct Target {
    std::string name{"redis-over-mako"};
    std::string host{"127.0.0.1"};
    int port = 6380;
};

struct Args {
    Target target;
    uint64_t keys = 1'000'000;
    int value_size = 8;
    int threads = 1;
    int duration_sec = 20;
    int read_percent = 80;
    int preload_threads = 1;
    std::string key_prefix;
    std::string out_csv{"redis_scalability.csv"};
    std::vector<Workload> workloads{Workload::Get, Workload::Set, Workload::Mixed};
    bool skip_preload = false;
    bool preload_only = false;
};

struct BenchRow {
    std::string workload;
    double duration_sec = 0;
    uint64_t total_ops = 0;
    double ops_per_sec = 0;
    double p50_us = 0;
    double p95_us = 0;
    double p99_us = 0;
};

const char* workload_name(Workload workload) {
    switch (workload) {
        case Workload::Get:
            return "get";
        case Workload::Set:
            return "set";
        case Workload::Mixed:
            return "mixed";
    }
    return "unknown";
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

        if (arg == "--name") {
            args.target.name = value();
        } else if (arg == "--host") {
            args.target.host = value();
        } else if (arg == "--port") {
            args.target.port = std::stoi(value());
        } else if (arg == "--keys") {
            args.keys = std::stoull(value());
        } else if (arg == "--value-size") {
            args.value_size = std::stoi(value());
        } else if (arg == "--threads") {
            args.threads = std::stoi(value());
        } else if (arg == "--duration") {
            args.duration_sec = std::stoi(value());
        } else if (arg == "--read-percent") {
            args.read_percent = std::stoi(value());
        } else if (arg == "--preload-threads") {
            args.preload_threads = std::stoi(value());
        } else if (arg == "--key-prefix") {
            args.key_prefix = value();
        } else if (arg == "--workloads") {
            args.workloads = parse_workloads(value());
        } else if (arg == "--out") {
            args.out_csv = value();
        } else if (arg == "--skip-preload") {
            args.skip_preload = true;
        } else if (arg == "--preload-only") {
            args.preload_only = true;
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (args.keys == 0 || args.value_size < 0 || args.threads < 1
        || args.duration_sec < 1 || args.preload_threads < 1
        || args.read_percent < 0 || args.read_percent > 100) {
        throw std::runtime_error("invalid benchmark arguments");
    }
}

std::vector<std::string> build_keys(uint64_t total_keys, const std::string& prefix) {
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(total_keys));
    for (uint64_t i = 0; i < total_keys; ++i) {
        keys.push_back(prefix + std::to_string(i));
    }
    return keys;
}

class Connection {
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : fd_(other.fd_), error_(std::move(other.error_)) {
        other.fd_ = -1;
    }

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            close_fd();
            fd_ = other.fd_;
            error_ = std::move(other.error_);
            other.fd_ = -1;
        }
        return *this;
    }

    ~Connection() {
        close_fd();
    }

    bool connect_to(const Target& target) {
        close_fd();
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            error_ = std::strerror(errno);
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(target.port));
        if (inet_pton(AF_INET, target.host.c_str(), &address.sin_addr) != 1) {
            error_ = "inet_pton failed";
            return false;
        }
        if (connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error_ = std::strerror(errno);
            return false;
        }
        return true;
    }

    bool send_all(const std::string& data) {
        const char* cursor = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
            ssize_t sent = send(fd_, cursor, remaining, 0);
            if (sent <= 0) {
                error_ = std::strerror(errno);
                return false;
            }
            cursor += sent;
            remaining -= static_cast<size_t>(sent);
        }
        return true;
    }

    bool read_reply() {
        std::string line;
        if (!read_line(line) || line.empty()) {
            return false;
        }
        if (line.front() == '-') {
            error_ = line;
            return false;
        }
        if (line.front() == '$') {
            long length = std::stol(line.substr(1));
            return length < 0 || read_exact(static_cast<size_t>(length) + 2);
        }
        if (line.front() == '*') {
            long count = std::stol(line.substr(1));
            for (long i = 0; i < count; ++i) {
                if (!read_reply()) {
                    return false;
                }
            }
        }
        return true;
    }

    const std::string& error() const {
        return error_;
    }

private:
    void close_fd() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    bool read_exact(size_t length) {
        char buffer[4096];
        while (length > 0) {
            size_t requested = std::min(length, sizeof(buffer));
            ssize_t received = recv(fd_, buffer, requested, 0);
            if (received <= 0) {
                error_ = received == 0 ? "connection closed" : std::strerror(errno);
                return false;
            }
            length -= static_cast<size_t>(received);
        }
        return true;
    }

    bool read_line(std::string& line) {
        line.clear();
        char value = '\0';
        while (true) {
            ssize_t received = recv(fd_, &value, 1, 0);
            if (received <= 0) {
                error_ = received == 0 ? "connection closed" : std::strerror(errno);
                return false;
            }
            if (value == '\r') {
                received = recv(fd_, &value, 1, 0);
                if (received <= 0 || value != '\n') {
                    error_ = "invalid RESP line ending";
                    return false;
                }
                return true;
            }
            line.push_back(value);
        }
    }

    int fd_ = -1;
    std::string error_;
};

Connection connect_retry(const Target& target) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        Connection connection;
        if (connection.connect_to(target)) {
            return connection;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error(
        "connect failed to " + target.host + ":" + std::to_string(target.port));
}

std::string command_ping() {
    return "*1\r\n$4\r\nPING\r\n";
}

std::string command_get(const std::string& key) {
    return "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key
        + "\r\n";
}

std::string command_set(const std::string& key, const std::string& value) {
    return "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key
        + "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

void require_ping(const Target& target) {
    Connection connection = connect_retry(target);
    if (!connection.send_all(command_ping()) || !connection.read_reply()) {
        throw std::runtime_error("PING failed: " + connection.error());
    }
}

void preload(
    const Target& target,
    const std::vector<std::string>& keys,
    int value_size,
    int thread_count) {
    const std::string value(static_cast<size_t>(value_size), 'X');
    std::atomic<uint64_t> next_index{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::string first_error;
    const auto start = Clock::now();

    auto loader = [&]() {
        Connection connection = connect_retry(target);
        while (!failed.load(std::memory_order_relaxed)) {
            uint64_t index = next_index.fetch_add(1, std::memory_order_relaxed);
            if (index >= keys.size()) {
                break;
            }
            if (!connection.send_all(command_set(keys[static_cast<size_t>(index)], value))
                || !connection.read_reply()) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (first_error.empty()) {
                    first_error = connection.error();
                }
                failed.store(true, std::memory_order_relaxed);
                break;
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> loaders;
    loaders.reserve(static_cast<size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
        loaders.emplace_back(loader);
    }
    for (auto& thread : loaders) {
        thread.join();
    }
    if (failed.load()) {
        throw std::runtime_error("preload failed: " + first_error);
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Preloaded " << completed.load() << " keys in " << std::fixed
              << std::setprecision(2) << elapsed << " seconds ("
              << static_cast<double>(completed.load()) / elapsed << " SET/s)\n";
}

uint64_t xorshift64(uint64_t& state) {
    uint64_t x = state;
    x ^= x << 7;
    x ^= x >> 9;
    x ^= x << 8;
    state = x;
    return x;
}

struct WorkerStats {
    uint64_t operations = 0;
    std::vector<uint32_t> latencies_us;
    std::string error;
};

double percentile(std::vector<uint32_t>& values, double quantile) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(
        quantile * static_cast<double>(values.size() - 1));
    return static_cast<double>(values[index]);
}

WorkerStats run_worker(
    const Target& target,
    const std::vector<std::string>& keys,
    Workload workload,
    int value_size,
    int read_percent,
    int duration_sec,
    uint64_t seed,
    std::barrier<>& start_barrier) {
    WorkerStats stats;
    Connection connection = connect_retry(target);
    const std::string value(static_cast<size_t>(value_size), 'Y');
    uint64_t rng = seed;
    start_barrier.arrive_and_wait();
    const auto deadline = Clock::now() + std::chrono::seconds(duration_sec);

    while (Clock::now() < deadline) {
        const auto& key = keys[static_cast<size_t>(xorshift64(rng) % keys.size())];
        const bool is_get = workload == Workload::Get
            || (workload == Workload::Mixed
                && static_cast<int>(xorshift64(rng) % 100) < read_percent);
        const std::string command = is_get ? command_get(key) : command_set(key, value);
        const auto operation_start = Clock::now();
        if (!connection.send_all(command) || !connection.read_reply()) {
            stats.error = std::string(is_get ? "GET: " : "SET: ")
                + connection.error();
            break;
        }
        const auto operation_end = Clock::now();
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            operation_end - operation_start);
        stats.latencies_us.push_back(static_cast<uint32_t>(latency.count()));
        ++stats.operations;
    }
    return stats;
}

BenchRow run_workload(
    const Args& args,
    const std::vector<std::string>& keys,
    Workload workload) {
    std::vector<std::thread> workers;
    std::vector<WorkerStats> stats(static_cast<size_t>(args.threads));
    std::barrier<> start_barrier(args.threads + 1);
    workers.reserve(static_cast<size_t>(args.threads));

    for (int i = 0; i < args.threads; ++i) {
        workers.emplace_back([&, i]() {
            const uint64_t seed = 0xc0ffeeULL
                + static_cast<uint64_t>(i + 1) * 0x9e3779b97f4a7c15ULL
                + static_cast<uint64_t>(workload);
            stats[static_cast<size_t>(i)] = run_worker(
                args.target,
                keys,
                workload,
                args.value_size,
                args.read_percent,
                args.duration_sec,
                seed,
                start_barrier);
        });
    }

    const auto start = Clock::now();
    start_barrier.arrive_and_wait();
    for (auto& thread : workers) {
        thread.join();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    BenchRow row;
    row.workload = workload_name(workload);
    row.duration_sec = elapsed;
    std::vector<uint32_t> latencies;
    for (const auto& worker : stats) {
        if (!worker.error.empty()) {
            throw std::runtime_error(
                row.workload + " worker failed: " + worker.error);
        }
        row.total_ops += worker.operations;
        latencies.insert(
            latencies.end(), worker.latencies_us.begin(), worker.latencies_us.end());
    }
    row.ops_per_sec = static_cast<double>(row.total_ops) / row.duration_sec;
    row.p50_us = percentile(latencies, 0.50);
    row.p95_us = percentile(latencies, 0.95);
    row.p99_us = percentile(latencies, 0.99);
    return row;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args;
        parse_args(argc, argv, args);
        require_ping(args.target);
        std::vector<std::string> keys = build_keys(args.keys, args.key_prefix);

        if (!args.skip_preload) {
            preload(
                args.target, keys, args.value_size, args.preload_threads);
        }
        if (args.preload_only) {
            return 0;
        }

        std::ofstream out(args.out_csv);
        if (!out) {
            throw std::runtime_error("failed to open output CSV: " + args.out_csv);
        }
        out << "server,host,port,workload,key_dist,threads,value_size,read_percent,"
               "duration_sec,total_ops,ops_per_sec,ops_per_sec_per_thread,"
               "p50_us,p95_us,p99_us\n";

        for (Workload workload : args.workloads) {
            BenchRow row = run_workload(args, keys, workload);
            std::cout << row.workload << " threads=" << args.threads
                      << " throughput=" << std::fixed << std::setprecision(2)
                      << row.ops_per_sec << " ops/s p99=" << row.p99_us << " us\n";
            out << args.target.name << ',' << args.target.host << ','
                << args.target.port << ',' << row.workload
                << ",uniform-decimal," << args.threads << ',' << args.value_size
                << ',' << args.read_percent << ',' << std::fixed
                << std::setprecision(6) << row.duration_sec << ','
                << row.total_ops << ',' << row.ops_per_sec << ','
                << row.ops_per_sec / static_cast<double>(args.threads) << ','
                << row.p50_us << ',' << row.p95_us << ',' << row.p99_us << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bench_resp_scalability: " << error.what() << '\n';
        return 1;
    }
}
