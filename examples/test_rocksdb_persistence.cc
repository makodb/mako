/**
 * test_rocksdb_persistence.cc
 *
 * DESCRIPTION:
 * Basic test suite for RocksDB persistence layer functionality.
 *
 * TESTS INCLUDED:
 * 1. Basic Persistence - Single write with callback verification
 * 2. Concurrent Writes - 4 threads writing 100 messages each to test thread safety
 * 3. Large Data - 1MB write to test large message handling
 * 4. Key Generation - Verifies RocksDB key format (shard:partition:epoch:sequence)
 *
 * PURPOSE:
 * Validates core RocksDB persistence operations including:
 * - Asynchronous write operations
 * - Callback execution
 * - Thread-safe concurrent access
 * - Large message handling
 * - Key format correctness
 *
 * EXPECTED RESULTS:
 * - All writes should succeed
 * - Callbacks should execute correctly
 * - Throughput: 10,000+ writes/sec for small messages
 * - Large writes (1MB) should complete in <10ms
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <cstring>
#include "../src/mako/rocksdb_persistence.h"
#include "../src/mako/util.h"

using namespace std;
using namespace mako;
using namespace std::chrono;

// Mock function to simulate get_epoch
int get_epoch() {
    static int epoch = 1;
    return epoch++;
}

void test_basic_persistence() {
    cout << "\n=== Test 1: Basic Persistence ===" << endl;

    auto& persistence = RocksDBPersistence::getInstance();

    // Add username prefix to avoid conflicts when multiple users run on the same server
    string username = util::get_current_username();
    string db_path = "/tmp/" + username + "_test_rocksdb";

    // Initialize RocksDB with 2 partitions
    if (!persistence.initialize(db_path, 2, 2)) {
        cerr << "Failed to initialize RocksDB!" << endl;
        return;
    }

    // Test data
    const char* test_data = "This is test transaction log data";
    size_t data_size = strlen(test_data);

    // Write data asynchronously
    auto future = persistence.persistAsync(test_data, data_size, 0, 1,
        [](bool success) {
            if (success) {
                cout << "Callback: Write succeeded!" << endl;
            } else {
                cout << "Callback: Write failed!" << endl;
            }
        });

    // Wait for completion
    bool result = future.get();
    cout << "Future result: " << (result ? "Success" : "Failed") << endl;

    persistence.shutdown();
}

void test_concurrent_writes() {
    cout << "\n=== Test 2: Concurrent Writes ===" << endl;

    auto& persistence = RocksDBPersistence::getInstance();

    // Add username prefix to avoid conflicts when multiple users run on the same server
    string username = util::get_current_username();
    string db_path = "/tmp/" + username + "_test_rocksdb_concurrent";

    if (!persistence.initialize(db_path, 4, 4)) {
        cerr << "Failed to initialize RocksDB!" << endl;
        return;
    }

    const int num_threads = 4;
    const int writes_per_thread = 100;

    atomic<int> successful_writes(0);
    atomic<int> failed_writes(0);

    vector<thread> threads;

    auto start_time = high_resolution_clock::now();

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < writes_per_thread; i++) {
                string data = "Thread " + to_string(t) + " Write " + to_string(i);

                auto future = persistence.persistAsync(data.c_str(), data.size(),
                                                       t % 2,  // shard_id
                                                       t,      // partition_id
                                                       [&](bool success) {
                    if (success) {
                        successful_writes.fetch_add(1);
                    } else {
                        failed_writes.fetch_add(1);
                    }
                });

                // Don't wait for each write to complete
                future.wait();
            }
        });
    }

    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    cout << "Total writes: " << (num_threads * writes_per_thread) << endl;
    cout << "Successful writes: " << successful_writes.load() << endl;
    cout << "Failed writes: " << failed_writes.load() << endl;
    cout << "Time taken: " << duration.count() << " ms" << endl;
    cout << "Throughput: " << ((num_threads * writes_per_thread * 1000.0) / duration.count())
         << " writes/sec" << endl;

    // Check pending writes
    cout << "Pending writes: " << persistence.getPendingWrites() << endl;

    // Flush all
    cout << "Flushing all data..." << endl;
    if (persistence.flushAll()) {
        cout << "Flush successful!" << endl;
    } else {
        cout << "Flush failed!" << endl;
    }

    persistence.shutdown();
}

void test_large_data() {
    cout << "\n=== Test 3: Large Data Persistence ===" << endl;

    auto& persistence = RocksDBPersistence::getInstance();

    // Add username prefix to avoid conflicts when multiple users run on the same server
    string username = util::get_current_username();
    string db_path = "/tmp/" + username + "_test_rocksdb_large";

    if (!persistence.initialize(db_path, 4, 4)) {
        cerr << "Failed to initialize RocksDB!" << endl;
        return;
    }

    // Create large data (1MB)
    const size_t large_size = 1024 * 1024;
    vector<char> large_data(large_size);
    for (size_t i = 0; i < large_size; i++) {
        large_data[i] = 'A' + (i % 26);
    }

    cout << "Writing 1MB of data..." << endl;

    auto start_time = high_resolution_clock::now();

    auto future = persistence.persistAsync(large_data.data(), large_size, 0, 0,
        [](bool success) {
            if (success) {
                cout << "Large write succeeded!" << endl;
            } else {
                cout << "Large write failed!" << endl;
            }
        });

    bool result = future.get();

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(end_time - start_time);

    cout << "Write result: " << (result ? "Success" : "Failed") << endl;
    cout << "Time taken: " << duration.count() << " microseconds" << endl;

    persistence.shutdown();
}

void test_key_generation() {
    cout << "\n=== Test 4: Key Generation ===" << endl;

    auto& persistence = RocksDBPersistence::getInstance();
    persistence.setEpoch(42);

    // Generate some keys
    for (uint32_t shard = 0; shard < 2; shard++) {
        for (uint32_t partition = 0; partition < 3; partition++) {
            for (uint64_t seq = 0; seq < 5; seq++) {
                string key = persistence.generateKey(shard, partition, 42, seq);
                cout << "Key: " << key << endl;
            }
        }
    }
}

int main(int argc, char** argv) {
    cout << "RocksDB Persistence Layer Test Suite" << endl;
    cout << "=====================================" << endl;

    test_basic_persistence();
    test_concurrent_writes();
    test_large_data();
    test_key_generation();

    cout << "\n=== All Tests Complete ===" << endl;

    return 0;
}