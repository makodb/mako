/**
 * test_callback_demo.cc
 *
 * DESCRIPTION:
 * Demonstrates callback functionality for asynchronous RocksDB persistence operations.
 *
 * TESTS INCLUDED:
 * 1. Basic Callbacks - 10 async writes with success/failure callback tracking
 * 2. Transaction Simulation - Simulates Transaction.hh usage pattern with atomic counters
 *
 * PURPOSE:
 * Shows how callbacks can be used to:
 * - Track successful vs. failed persistence operations
 * - Update shared atomic counters from worker threads
 * - Monitor progress in production code
 * - Integrate with existing transaction logging
 *
 * KEY FEATURES DEMONSTRATED:
 * - Callback execution in worker thread context
 * - Atomic counter updates (safe for concurrent access)
 * - Success/failure reporting
 * - Integration pattern for Transaction.hh
 *
 * EXPECTED RESULTS:
 * - All 10 basic writes should succeed (10/10)
 * - All 5 transaction logs should succeed (5/5)
 * - No failures reported
 * - Callbacks execute promptly after persistence
 */

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "../src/mako/rocksdb_persistence.h"
#include "../src/mako/util.h"

// Mock get_epoch for testing
int get_epoch() {
    return 1;
}

int main() {
    std::cout << "=== RocksDB Callback Demonstration ===" << std::endl;

    // Main thread variables that callbacks will update
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};
    std::atomic<bool> all_done{false};

    // Initialize RocksDB
    // Add username prefix to avoid conflicts when multiple users run on the same server
    std::string username = util::get_current_username();
    std::string db_path = "/tmp/" + username + "_callback_demo_db";

    auto& persistence = mako::RocksDBPersistence::getInstance();
    if (!persistence.initialize(db_path, 2, 2)) {
        std::cerr << "Failed to initialize RocksDB!" << std::endl;
        return 1;
    }

    const int NUM_WRITES = 10;
    std::cout << "\nStarting " << NUM_WRITES << " async writes with callbacks..." << std::endl;

    // Issue multiple async writes
    for (int i = 0; i < NUM_WRITES; i++) {
        std::string data = "Test data " + std::to_string(i);

        // Capture references to main thread variables in the callback
        auto future = persistence.persistAsync(
            data.c_str(), data.size(), 0, 0,
            [&success_count, &failure_count, i](bool success) {
                if (success) {
                    int current = success_count.fetch_add(1) + 1;
                    std::cout << "  ✓ Callback " << i << ": Write successful! "
                              << "(Total successes: " << current << ")" << std::endl;
                } else {
                    int current = failure_count.fetch_add(1) + 1;
                    std::cerr << "  ✗ Callback " << i << ": Write failed! "
                              << "(Total failures: " << current << ")" << std::endl;
                }
            });

        // Don't wait for the future immediately - let it be async
    }

    // Wait a bit for callbacks to complete
    std::cout << "\nWaiting for callbacks to complete..." << std::endl;
    for (int wait = 0; wait < 10; wait++) {
        if (success_count + failure_count >= NUM_WRITES) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Final statistics
    std::cout << "\n=== Final Statistics ===" << std::endl;
    std::cout << "Successful writes: " << success_count.load() << std::endl;
    std::cout << "Failed writes: " << failure_count.load() << std::endl;
    std::cout << "Total callbacks executed: " << (success_count + failure_count) << "/" << NUM_WRITES << std::endl;

    // Demonstrate how this would work in Transaction.hh context
    std::cout << "\n=== Simulating Transaction.hh Usage ===" << std::endl;

    // Static variables that persist across function calls (like in Transaction.hh)
    static std::atomic<uint64_t> transaction_persist_count{0};
    static std::atomic<uint64_t> transaction_fail_count{0};

    // Simulate multiple transaction log writes
    for (int txn = 0; txn < 5; txn++) {
        std::string txn_log = "Transaction log entry " + std::to_string(txn);

        persistence.persistAsync(
            txn_log.c_str(), txn_log.size(), 0, txn % 2,  // Vary partition_id (0 or 1)
            [](bool success) {
                if (success) {
                    uint64_t count = transaction_persist_count.fetch_add(1) + 1;
                    // Print every 1000th success in production, but here print all
                    std::cout << "  [Transaction] Persisted log #" << count
                              << " to RocksDB" << std::endl;
                } else {
                    uint64_t fails = transaction_fail_count.fetch_add(1) + 1;
                    std::cerr << "  [Transaction] Failed to persist (total failures: "
                              << fails << ")" << std::endl;
                }
            });
    }

    // Wait for transaction callbacks
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "\nTransaction persistence stats:" << std::endl;
    std::cout << "  Persisted: " << transaction_persist_count.load() << " transaction logs" << std::endl;
    std::cout << "  Failed: " << transaction_fail_count.load() << " transaction logs" << std::endl;

    persistence.shutdown();
    std::cout << "\n=== Demo Complete ===" << std::endl;

    return 0;
}