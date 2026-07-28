#pragma once

#include <vector>
#include <memory>
#include <atomic>

#include "rrr/rrr.hpp"

namespace janus {

/**
 * ShardFailureController - Simulates shard failures for testing.
 *
 * This class provides thread-safe failure simulation for individual shards
 * in a multi-shard system. Use for testing transaction timeout and
 * shard failure handling.
 *
 * Usage:
 *   ShardFailureController controller(num_shards);
 *   controller.fail_shard(1);  // Fail shard 1
 *   if (controller.is_shard_failed(1)) {
 *     // Skip RPC to shard 1 or simulate timeout
 *   }
 *   controller.recover_shard(1);  // Recover shard 1
 *
 * Thread-safe: All methods can be called from any thread.
 */
class ShardFailureController {
private:
    // Thread-safe failure flags per shard using std::atomic
    std::vector<std::unique_ptr<std::atomic<bool>>> shard_failed_;
    size_t num_shards_;

public:
    // @unsafe - Constructor uses std::vector and std::unique_ptr
    explicit ShardFailureController(size_t num_shards)
        : num_shards_(num_shards)
    {
        // @unsafe { std::vector and std::unique_ptr operations }
        shard_failed_.reserve(num_shards);
        for (size_t i = 0; i < num_shards; i++) {
            shard_failed_.push_back(std::make_unique<std::atomic<bool>>(false));
        }
    }

    // Disable copy/move
    ShardFailureController(const ShardFailureController&) = delete;
    ShardFailureController& operator=(const ShardFailureController&) = delete;
    ShardFailureController(ShardFailureController&&) = delete;
    ShardFailureController& operator=(ShardFailureController&&) = delete;

    // @safe - Mark a shard as failed
    void fail_shard(size_t shard_idx) {
        if (shard_idx >= num_shards_) {
            rrr::Log_error("ShardFailureController: invalid shard index {} (num_shards={})",
                      shard_idx, num_shards_);
            return;
        }
        shard_failed_[shard_idx]->store(true, std::memory_order_relaxed);
        rrr::Log_info("ShardFailureController: shard {} marked as FAILED", shard_idx);
    }

    // @safe - Mark a shard as recovered (healthy)
    void recover_shard(size_t shard_idx) {
        if (shard_idx >= num_shards_) {
            rrr::Log_error("ShardFailureController: invalid shard index {} (num_shards={})",
                      shard_idx, num_shards_);
            return;
        }
        shard_failed_[shard_idx]->store(false, std::memory_order_relaxed);
        rrr::Log_info("ShardFailureController: shard {} marked as RECOVERED", shard_idx);
    }

    // @safe - Check if a shard is currently failed
    bool is_shard_failed(size_t shard_idx) const {
        if (shard_idx >= num_shards_) {
            rrr::Log_error("ShardFailureController: invalid shard index {} (num_shards={})",
                      shard_idx, num_shards_);
            return false;  // Assume healthy if index invalid
        }
        return shard_failed_[shard_idx]->load(std::memory_order_relaxed);
    }

    // @safe - Get number of shards
    size_t num_shards() const {
        return num_shards_;
    }

    // @safe - Get count of currently failed shards
    size_t failed_shard_count() const {
        size_t count = 0;
        for (size_t i = 0; i < num_shards_; i++) {
            if (shard_failed_[i]->load(std::memory_order_relaxed)) {
                count++;
            }
        }
        return count;
    }

    // @safe - Fail all shards
    void fail_all_shards() {
        for (size_t i = 0; i < num_shards_; i++) {
            shard_failed_[i]->store(true, std::memory_order_relaxed);
        }
        rrr::Log_info("ShardFailureController: all {} shards marked as FAILED", num_shards_);
    }

    // @safe - Recover all shards
    void recover_all_shards() {
        for (size_t i = 0; i < num_shards_; i++) {
            shard_failed_[i]->store(false, std::memory_order_relaxed);
        }
        rrr::Log_info("ShardFailureController: all {} shards marked as RECOVERED", num_shards_);
    }
};

// Global pointer to failure controller (set during test setup)
// @unsafe - Uses global mutable state
inline ShardFailureController* g_shard_failure_controller = nullptr;

// @safe - Convenience function to check if a shard is failed
// Returns false if controller not set up
inline bool is_shard_failed(size_t shard_idx) {
    if (g_shard_failure_controller == nullptr) {
        return false;  // No controller = no simulated failures
    }
    return g_shard_failure_controller->is_shard_failed(shard_idx);
}

} // namespace janus
