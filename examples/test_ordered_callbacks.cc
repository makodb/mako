/**
 * test_ordered_callbacks.cc
 *
 * DESCRIPTION:
 * Tests ordered callback execution guarantees for RocksDB persistence layer.
 *
 * TESTS INCLUDED:
 * 1. Ordered Callbacks - 100 logs submitted in random order, callbacks must execute in submission order
 * 2. Unordered Callbacks - 50 logs without ordering requirement (for comparison)
 * 3. Multiple Partitions - 3 partitions with independent ordering (20 logs each)
 *
 * PURPOSE:
 * Validates that the persistence layer maintains Paxos-like ordering guarantees:
 * - Callbacks execute in the order messages were submitted to each partition
 * - Each partition maintains independent ordering (no cross-partition blocking)
 * - Ordering is preserved even when messages arrive out-of-order
 * - Supports both ordered and unordered operations on same partition
 *
 * KEY FEATURES TESTED:
 * - Submission order tracking and verification
 * - Ordered callback execution per partition
 * - Independence between partitions
 * - Mixed ordered/unordered operations
 * - Sequence number management
 *
 * EXPECTED RESULTS:
 * - Ordered test: All 100 callbacks execute in submission order
 * - Unordered test: All 50 callbacks execute (order not guaranteed)
 * - Multiple partitions: All 3 partitions process independently and correctly (20/20 each)
 * - No callback order violations within a partition
 */

#include "../src/mako/rocksdb_persistence.h"
#include "../src/mako/util.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <random>
#include <cassert>
#include <unistd.h>

using namespace mako;

std::atomic<int> callback_order{0};
std::vector<int> callback_sequence;
std::mutex sequence_mutex;

void test_ordered_callbacks() {
    std::cout << "\n=== Testing Ordered Callbacks ===" << std::endl;

    auto& persistence = RocksDBPersistence::getInstance();

    const int NUM_LOGS = 100;
    const uint32_t SHARD_ID = 1;
    const uint32_t PARTITION_ID = 0;  // Valid partition ID (0-3)

    callback_order = 0;
    callback_sequence.clear();

    std::vector<std::future<bool>> futures;
    std::vector<int> expected_order;  // Track the order logs were submitted

    // Submit logs in random order to simulate out-of-order network arrival
    std::vector<int> submit_order;
    for (int i = 0; i < NUM_LOGS; i++) {
        submit_order.push_back(i);
    }

    // Shuffle to simulate random arrival
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(submit_order.begin(), submit_order.end(), g);

    std::cout << "Submitting " << NUM_LOGS << " logs in random order..." << std::endl;

    for (int i = 0; i < NUM_LOGS; i++) {
        int idx = submit_order[i];
        expected_order.push_back(idx);  // Track the actual submission order
        std::string data = "Log entry " + std::to_string(idx);

        // Each callback verifies it's called in the submission order
        auto future = persistence.persistAsync(
            data.c_str(), data.size(),
            SHARD_ID, PARTITION_ID,
            [i, idx, &expected_order](bool success) {
                if (!success) {
                    std::cerr << "Failed to persist log " << idx << std::endl;
                    return;
                }

                int callback_pos = callback_order.fetch_add(1);

                {
                    std::lock_guard<std::mutex> lock(sequence_mutex);
                    callback_sequence.push_back(idx);
                }

                // Verify callbacks are called in submission order
                if (callback_pos < expected_order.size() && idx != expected_order[callback_pos]) {
                    std::cerr << "ERROR: Callback order violation! Position " << callback_pos
                              << " expected idx " << expected_order[callback_pos]
                              << " but got idx " << idx << std::endl;
                } else if (callback_pos % 10 == 0) {
                    std::cout << "Callback at position " << callback_pos
                              << " (idx=" << idx << ") executed in correct order" << std::endl;
                }
            });  // Ordering is always enabled now

        futures.push_back(std::move(future));

        // Small delay to spread out submissions
        if (i % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "Waiting for all persistence operations to complete..." << std::endl;

    // Wait for all futures
    int success_count = 0;
    for (auto& future : futures) {
        if (future.get()) {
            success_count++;
        }
    }

    std::cout << "Successfully persisted " << success_count << "/" << NUM_LOGS << " logs" << std::endl;

    // Verify callbacks were called in order
    bool order_correct = true;
    {
        std::lock_guard<std::mutex> lock(sequence_mutex);
        for (size_t i = 0; i < callback_sequence.size(); i++) {
            if (callback_sequence[i] != expected_order[i]) {
                std::cerr << "ERROR: Callback sequence mismatch at position " << i
                          << ": expected idx " << expected_order[i] << " but got idx " << callback_sequence[i] << std::endl;
                order_correct = false;
            }
        }
    }

    if (order_correct && callback_sequence.size() == NUM_LOGS) {
        std::cout << "✓ All callbacks executed in correct order!" << std::endl;
    } else {
        std::cerr << "✗ Callback ordering test FAILED!" << std::endl;
    }
}

void test_unordered_callbacks() {
    std::cout << "\n=== Testing Unordered Callbacks (for comparison) ===" << std::endl;

    auto& persistence = RocksDBPersistence::getInstance();

    const int NUM_LOGS = 50;
    const uint32_t SHARD_ID = 2;
    const uint32_t PARTITION_ID = 1;  // Valid partition ID (0-3)

    std::atomic<int> completed_count{0};
    std::vector<std::future<bool>> futures;

    std::cout << "Submitting " << NUM_LOGS << " logs without ordering requirement..." << std::endl;

    for (int i = 0; i < NUM_LOGS; i++) {
        std::string data = "Unordered log " + std::to_string(i);

        auto future = persistence.persistAsync(
            data.c_str(), data.size(),
            SHARD_ID, PARTITION_ID,
            [i, &completed_count](bool success) {
                if (success) {
                    int count = completed_count.fetch_add(1) + 1;
                    if (count % 10 == 0) {
                        std::cout << "Unordered callback executed (total: " << count << ")" << std::endl;
                    }
                }
            });  // Ordering is always enabled now (was false before)

        futures.push_back(std::move(future));
    }

    // Wait for all
    for (auto& future : futures) {
        future.get();
    }

    std::cout << "Completed " << completed_count.load() << "/" << NUM_LOGS
              << " unordered callbacks" << std::endl;
}

void test_multiple_partitions() {
    std::cout << "\n=== Testing Multiple Partitions (Independence) ===" << std::endl;

    auto& persistence = RocksDBPersistence::getInstance();

    const int LOGS_PER_PARTITION = 20;
    const int NUM_PARTITIONS = 3;
    const uint32_t SHARD_ID = 3;

    std::vector<std::atomic<int>> partition_counters(NUM_PARTITIONS);
    std::vector<std::future<bool>> futures;

    std::cout << "Submitting logs to " << NUM_PARTITIONS << " partitions..." << std::endl;

    // Submit to multiple partitions
    for (int p = 0; p < NUM_PARTITIONS; p++) {
        partition_counters[p] = 0;

        for (int i = 0; i < LOGS_PER_PARTITION; i++) {
            std::string data = "Partition " + std::to_string(p) + " log " + std::to_string(i);

            auto future = persistence.persistAsync(
                data.c_str(), data.size(),
                SHARD_ID, p,  // Different partition IDs
                [p, i, &partition_counters](bool success) {
                    if (success) {
                        int expected = partition_counters[p].fetch_add(1);
                        if (expected != i) {
                            std::cerr << "ERROR: Partition " << p << " callback order violation!" << std::endl;
                        }
                    }
                });  // Ordering is always enabled now

            futures.push_back(std::move(future));
        }
    }

    // Wait for all
    for (auto& future : futures) {
        future.get();
    }

    // Check results
    bool all_correct = true;
    for (int p = 0; p < NUM_PARTITIONS; p++) {
        int count = partition_counters[p].load();
        std::cout << "Partition " << p << ": " << count << "/" << LOGS_PER_PARTITION << " callbacks executed" << std::endl;
        if (count != LOGS_PER_PARTITION) {
            all_correct = false;
        }
    }

    if (all_correct) {
        std::cout << "✓ All partitions processed independently and correctly!" << std::endl;
    } else {
        std::cout << "✗ Some partition callbacks were not executed correctly!" << std::endl;
    }
}

int main() {
    std::cout << "=== RocksDB Ordered Callbacks Test ===" << std::endl;

    auto& persistence = RocksDBPersistence::getInstance();

    // Add username prefix to avoid conflicts when multiple users run on the same server
    std::string username = util::get_current_username();
    std::string db_path = "/tmp/" + username + "_rocksdb_ordered_test_" + std::to_string(getpid());
    if (!persistence.initialize(db_path, 4, 4)) {
        std::cerr << "Failed to initialize RocksDB persistence" << std::endl;
        return 1;
    }

    std::cout << "RocksDB initialized at: " << db_path << std::endl;

    // Run tests
    test_ordered_callbacks();
    test_unordered_callbacks();
    test_multiple_partitions();

    // Cleanup
    persistence.shutdown();
    system(("rm -rf " + db_path).c_str());

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}