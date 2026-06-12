#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <numeric>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool writeAll(int fd, const char* data, size_t size) {
    while (size > 0) {
        ssize_t written = ::write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

bool syncFile(int fd, bool use_fdatasync) {
    while (true) {
        int rc = use_fdatasync ? ::fdatasync(fd) : ::fsync(fd);
        if (rc == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

double percentile(const std::vector<double>& values, double pct) {
    if (values.empty()) {
        return 0.0;
    }
    size_t idx = static_cast<size_t>((pct / 100.0) * (values.size() - 1));
    return values[idx];
}

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " <file> [iterations] [write_bytes] [writes_per_sync] [fsync|fdatasync]\n"
        << "Example: " << argv0 << " /tmp/fsync_test.log 1000 4096 1 fsync\n"
        << "Group commit example: " << argv0 << " /tmp/fsync_test.log 1000 4096 64 fsync\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 6) {
        usage(argv[0]);
        return 1;
    }

    const std::string path = argv[1];
    const size_t iterations = argc > 2 ? std::stoull(argv[2]) : 1000;
    const size_t write_bytes = argc > 3 ? std::stoull(argv[3]) : 4096;
    const size_t writes_per_sync = argc > 4 ? std::stoull(argv[4]) : 1;
    const std::string sync_mode = argc > 5 ? argv[5] : "fsync";
    const bool use_fdatasync = sync_mode == "fdatasync";

    if (iterations == 0 || write_bytes == 0 || writes_per_sync == 0 ||
        (sync_mode != "fsync" && sync_mode != "fdatasync")) {
        usage(argv[0]);
        return 1;
    }

    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::cerr << "open failed for " << path << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    std::vector<char> buffer(write_bytes, 'x');
    std::vector<double> lat_us;
    lat_us.reserve(iterations);

    auto start_total = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        for (size_t j = 0; j < writes_per_sync; ++j) {
            buffer[0] = static_cast<char>((i + j) & 0xff);
            if (!writeAll(fd, buffer.data(), buffer.size())) {
                std::cerr << "write failed: " << std::strerror(errno) << "\n";
                ::close(fd);
                return 1;
            }
        }

        auto t0 = std::chrono::steady_clock::now();
        if (!syncFile(fd, use_fdatasync)) {
            std::cerr << sync_mode << " failed: " << std::strerror(errno) << "\n";
            ::close(fd);
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();

        lat_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    auto end_total = std::chrono::steady_clock::now();

    if (::close(fd) != 0) {
        std::cerr << "close failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::sort(lat_us.begin(), lat_us.end());
    double sum = std::accumulate(lat_us.begin(), lat_us.end(), 0.0);
    double total_sec = std::chrono::duration<double>(end_total - start_total).count();
    double mb_written = static_cast<double>(iterations * writes_per_sync * write_bytes) / (1024.0 * 1024.0);

    std::cout << "file: " << path << "\n";
    std::cout << "sync_mode: " << sync_mode << "\n";
    std::cout << "iterations: " << iterations << "\n";
    std::cout << "write_bytes: " << write_bytes << "\n";
    std::cout << "writes_per_sync: " << writes_per_sync << "\n";
    std::cout << "total_written_mb: " << mb_written << "\n";
    std::cout << "total_time_sec: " << total_sec << "\n";
    std::cout << "syncs_per_sec: " << static_cast<double>(iterations) / total_sec << "\n";
    std::cout << "write_mb_per_sec: " << mb_written / total_sec << "\n";
    std::cout << "fsync_latency_us_avg: " << sum / lat_us.size() << "\n";
    std::cout << "fsync_latency_us_p50: " << percentile(lat_us, 50) << "\n";
    std::cout << "fsync_latency_us_p95: " << percentile(lat_us, 95) << "\n";
    std::cout << "fsync_latency_us_p99: " << percentile(lat_us, 99) << "\n";
    std::cout << "fsync_latency_us_max: " << lat_us.back() << "\n";

    return 0;
}
