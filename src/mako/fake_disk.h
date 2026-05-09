#pragma once

// FakeDisk: shared simulator used by both persistence layers.
//   Layer 2 (Mako per-txn KV)  → src/mako/rocksdb_persistence.cc
//   Layer 1 (Raft consensus log) → src/rrr/rpc/rocksdb_log_storage.hpp
//
// When MAKO_PERSIST_FAKE_DISK=1, all calls to FakeDisk::write() /
// sleep_for() share one global queued device. Each caller reserves service
// time from the same next_available_ timestamp, then sleeps on the calling
// thread until its own completion time. Service time is modeled as fixed
// latency plus transfer time, which better matches a shared SSD request path
// than independent per-request sleeps.
//
// The function fake_disk() returns that singleton via a Meyers static
// inside an inline function, which C++17 guarantees is unique across all
// translation units.

#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <inttypes.h>

namespace mako {

enum class FakeDiskSource : int {
    MakoData = 0,
    RaftLog = 1,
    RaftMetadata = 2
};

class FakeDisk {
public:
    FakeDisk() {
        const char* en = std::getenv("MAKO_PERSIST_FAKE_DISK");
        enabled_ = (en != nullptr && std::atoi(en) != 0);
        if (!enabled_) return;
        const char* bw  = std::getenv("MAKO_PERSIST_BW_MBPS");
        const char* lat = std::getenv("MAKO_PERSIST_LATENCY_US");
        bw_mbps_    = (bw  != nullptr) ? std::atoll(bw)  : 0;
        latency_us_ = (lat != nullptr) ? std::atoll(lat) : 0;
        buffer_.resize(64 * 1024 * 1024);
        std::fprintf(stderr,
            "[FakeDisk] active: bw=%lld MB/s, latency=%lld us "
            "(shared queued device for Mako persist + Raft-log sync; "
            "sleep is on commit/append-entries path)\n",
            static_cast<long long>(bw_mbps_),
            static_cast<long long>(latency_us_));
    }

    bool enabled() const { return enabled_; }

    // Layer 2 entry: real memcpy into rolling buffer + sleep. Used when we
    // bypass RocksDB completely so the simulator pays the byte-copy cost.
    void write(FakeDiskSource source, const char* data, size_t size) {
        if (!buffer_.empty() && data != nullptr && size > 0) {
            const size_t cap = buffer_.size();
            const size_t copy_size = std::min(size, cap);
            const size_t off = offset_.fetch_add(copy_size, std::memory_order_relaxed) % cap;
            const size_t first = std::min(copy_size, cap - off);
            std::memcpy(buffer_.data() + off, data, first);
            if (copy_size > first) {
                std::memcpy(buffer_.data(), data + first, copy_size - first);
            }
        }
        sleep_for(source, size);
    }

    // Layer 1 entry: sleep only. Used after the underlying rocksdb_flush
    // (which we keep so consensus reads still work). The flush already moved
    // the bytes; we just add the simulated disk latency on top.
    void sleep_for(FakeDiskSource source, size_t bytes) {
        const auto reservation = reserve_delay_us(bytes);
        record(source, bytes, reservation.service_us, reservation.wait_us);
        const int64_t wait_us = reservation.wait_us;
        if (wait_us <= 0) return;
        std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
    }

    void print_stats(const char* label) const {
        const uint64_t mako_data_writes = source_writes_[idx(FakeDiskSource::MakoData)].load();
        const uint64_t mako_data_bytes = source_bytes_[idx(FakeDiskSource::MakoData)].load();
        const uint64_t raft_log_writes = source_writes_[idx(FakeDiskSource::RaftLog)].load();
        const uint64_t raft_log_bytes = source_bytes_[idx(FakeDiskSource::RaftLog)].load();
        const uint64_t raft_metadata_writes = source_writes_[idx(FakeDiskSource::RaftMetadata)].load();
        const uint64_t raft_metadata_bytes = source_bytes_[idx(FakeDiskSource::RaftMetadata)].load();
        std::fprintf(stderr,
            "[FakeDiskStats] label=%s enabled=%d total_writes=%" PRIu64
            " total_bytes=%" PRIu64 " total_service_us=%" PRIu64
            " total_wait_us=%" PRIu64 " max_wait_us=%" PRIu64
            " mako_data_writes=%" PRIu64 " mako_data_bytes=%" PRIu64
            " raft_log_writes=%" PRIu64 " raft_log_bytes=%" PRIu64
            " raft_metadata_writes=%" PRIu64 " raft_metadata_bytes=%" PRIu64 "\n",
            label ? label : "unknown",
            enabled_ ? 1 : 0,
            total_writes_.load(),
            total_bytes_.load(),
            total_service_us_.load(),
            total_wait_us_.load(),
            max_wait_us_.load(),
            mako_data_writes,
            mako_data_bytes,
            raft_log_writes,
            raft_log_bytes,
            raft_metadata_writes,
            raft_metadata_bytes);
    }

private:
    static constexpr size_t kSourceCount = 3;

    struct Reservation {
        int64_t service_us{0};
        int64_t wait_us{0};
    };

    static constexpr size_t idx(FakeDiskSource source) {
        return static_cast<size_t>(source);
    }

    int64_t service_time_us(size_t bytes) const {
        int64_t target_us = latency_us_;
        if (bw_mbps_ > 0) {
            const uint64_t numerator =
                static_cast<uint64_t>(bytes) * 1000000ULL;
            const uint64_t denominator =
                static_cast<uint64_t>(bw_mbps_) * 1048576ULL;
            const int64_t bw_us = static_cast<int64_t>(
                denominator == 0 ? 0 : (numerator + denominator - 1) / denominator);
            target_us += bw_us;
        }
        return target_us;
    }

    Reservation reserve_delay_us(size_t bytes) {
        const int64_t target_us = service_time_us(bytes);
        if (target_us <= 0) return Reservation{target_us, 0};

        using clock = std::chrono::steady_clock;
        const auto now = clock::now();

        std::lock_guard<std::mutex> lock(schedule_mu_);
        if (next_available_ < now) {
            next_available_ = now;
        }
        next_available_ += std::chrono::microseconds(target_us);
        const int64_t wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
            next_available_ - now).count();
        return Reservation{target_us, wait_us};
    }

    void record(FakeDiskSource source, size_t bytes, int64_t service_us, int64_t wait_us) {
        const size_t source_idx = idx(source);
        if (source_idx >= kSourceCount) return;

        total_writes_.fetch_add(1, std::memory_order_relaxed);
        total_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        total_service_us_.fetch_add(service_us > 0 ? static_cast<uint64_t>(service_us) : 0,
                                    std::memory_order_relaxed);
        total_wait_us_.fetch_add(wait_us > 0 ? static_cast<uint64_t>(wait_us) : 0,
                                 std::memory_order_relaxed);
        source_writes_[source_idx].fetch_add(1, std::memory_order_relaxed);
        source_bytes_[source_idx].fetch_add(bytes, std::memory_order_relaxed);

        uint64_t observed = wait_us > 0 ? static_cast<uint64_t>(wait_us) : 0;
        uint64_t current = max_wait_us_.load(std::memory_order_relaxed);
        while (observed > current &&
               !max_wait_us_.compare_exchange_weak(current, observed,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {}
    }

    bool enabled_{false};
    int64_t bw_mbps_{0};
    int64_t latency_us_{0};
    std::vector<char> buffer_;
    std::atomic<size_t> offset_{0};
    std::mutex schedule_mu_;
    std::chrono::steady_clock::time_point next_available_{};
    std::atomic<uint64_t> total_writes_{0};
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<uint64_t> total_service_us_{0};
    std::atomic<uint64_t> total_wait_us_{0};
    std::atomic<uint64_t> max_wait_us_{0};
    std::atomic<uint64_t> source_writes_[kSourceCount]{};
    std::atomic<uint64_t> source_bytes_[kSourceCount]{};
};

inline FakeDisk& fake_disk() {
    static FakeDisk inst;
    return inst;
}

}  // namespace mako
