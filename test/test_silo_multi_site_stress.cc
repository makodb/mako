/**
 * test_silo_multi_site_stress.cc
 *
 * Comprehensive stress tests for running multiple Silo sites
 * in a single process. Tests isolation, concurrency, and correctness
 * under heavy load.
 * Uses RustyCpp smart pointers for memory safety.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <random>
#include <chrono>
#include <set>
#include <map>
#include <mutex>

#include "silo_runtime.h"
#include "masstree/masstree_context.h"
#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"

// Provide globalepoch definition for this test file
volatile mrcu_epoch_type globalepoch = 1;

using TestTree = single_threaded_btree;


class SiloMultiSiteStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset thread-local state
        tl_silo_runtime = nullptr;
    }

    void TearDown() override {
        // Cleanup
    }

    // Helper to create a value pointer
    static TestTree::value_type MakeValue(
            std::vector<std::unique_ptr<uint64_t>>& storage, uint64_t v) {
        storage.emplace_back(std::make_unique<uint64_t>(v));
        return reinterpret_cast<TestTree::value_type>(storage.back().get());
    }
};

// Test 1: Four sites running concurrently with independent trees

TEST_F(SiloMultiSiteStressTest, FourSitesConcurrentTrees) {
    const int NUM_SITES = 4;
    const int NUM_KEYS = 10000;

    // Create sites using Arc smart pointers
    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    // Verify all sites are unique
    std::set<int> site_ids;
    for (const auto& site : sites) {
        ASSERT_TRUE(site_ids.insert(site->id()).second)
            << "Duplicate site ID: " << site->id();
    }

    // Per-site trees and storage
    std::vector<std::unique_ptr<TestTree>> trees;
    std::vector<std::vector<std::unique_ptr<uint64_t>>> value_storage(NUM_SITES);
    std::vector<std::vector<u64_varkey>> keys(NUM_SITES);

    for (int s = 0; s < NUM_SITES; ++s) {
        trees.emplace_back(std::make_unique<TestTree>());
        keys[s].reserve(NUM_KEYS);
        value_storage[s].reserve(NUM_KEYS);
        for (int i = 0; i < NUM_KEYS; ++i) {
            // Each site has unique key space: site_id * 1000000 + key
            keys[s].emplace_back(static_cast<uint64_t>(s * 1000000 + i));
        }
    }

    std::atomic<int> sites_completed{0};
    std::atomic<bool> stop_epochs{false};

    // Worker for each site
    // Get raw pointers for thread use (Arc ensures lifetime)
    auto site_worker = [&](int site_idx) {
        SiloRuntime* site = sites[site_idx].get_mut();
        site->BindToCurrentThread();

        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, site_idx * 100);
        ti->rcu_start();

        // Insert all keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            trees[site_idx]->insert(keys[site_idx][i],
                MakeValue(value_storage[site_idx], site_idx * 1000000 + i));
            if (i % 100 == 0) {
                ti->rcu_quiesce();
            }
        }

        // Verify all keys
        int found_count = 0;
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            if (trees[site_idx]->search(keys[site_idx][i], found)) {
                found_count++;
            }
        }
        EXPECT_EQ(found_count, NUM_KEYS) << "Site " << site_idx << " missing keys";

        ti->rcu_stop();
        sites_completed++;
    };

    // Epoch advancement threads (one per site)
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop_epochs.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Run site workers
    std::vector<std::thread> workers;
    for (int s = 0; s < NUM_SITES; ++s) {
        workers.emplace_back(site_worker, s);
    }

    for (auto& w : workers) w.join();

    stop_epochs = true;
    for (auto& e : epoch_threads) e.join();

    EXPECT_EQ(sites_completed.load(), NUM_SITES);

    // Cross-site isolation check: no tree should have another site's keys
    for (int s = 0; s < NUM_SITES; ++s) {
        for (int other = 0; other < NUM_SITES; ++other) {
            if (other == s) continue;
            for (int i = 0; i < 100; ++i) {
                TestTree::value_type found{};
                EXPECT_FALSE(trees[s]->search(keys[other][i], found))
                    << "Site " << s << " has site " << other << "'s key " << i;
            }
        }
    }
}

// Test 2: High concurrency - multiple threads per site

TEST_F(SiloMultiSiteStressTest, HighConcurrencyMultipleThreadsPerSite) {
    const int NUM_SITES = 3;
    const int THREADS_PER_SITE = 8;
    const int OPS_PER_THREAD = 500;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    std::atomic<int> total_ops{0};
    std::atomic<bool> stop_epochs{false};
    std::vector<std::atomic<int>> site_ops(NUM_SITES);
    for (auto& op : site_ops) op.store(0);

    // Worker that does RCU operations
    auto worker = [&](int site_idx, int thread_idx) {
        SiloRuntime* site = sites[site_idx].get_mut();
        site->BindToCurrentThread();

        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS,
            site_idx * 1000 + thread_idx);

        std::mt19937 rng(site_idx * 1000 + thread_idx);
        std::uniform_int_distribution<int> size_dist(32, 256);

        ti->rcu_start();

        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            int size = size_dist(rng);
            void* p = ti->allocate(size, memtag_value);
            ASSERT_NE(p, nullptr);

            // Mix of immediate and RCU deallocations
            if (i % 3 == 0) {
                ti->deallocate(p, size, memtag_value);
            } else {
                ti->deallocate_rcu(p, size, memtag_value);
            }

            if (i % 20 == 0) {
                ti->rcu_quiesce();
            }

            site_ops[site_idx]++;
            total_ops++;
        }

        ti->rcu_stop();
    };

    // Epoch threads
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop_epochs.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }

    // Worker threads
    std::vector<std::thread> workers;
    for (int s = 0; s < NUM_SITES; ++s) {
        for (int t = 0; t < THREADS_PER_SITE; ++t) {
            workers.emplace_back(worker, s, t);
        }
    }

    for (auto& w : workers) w.join();

    stop_epochs = true;
    for (auto& e : epoch_threads) e.join();

    EXPECT_EQ(total_ops.load(), NUM_SITES * THREADS_PER_SITE * OPS_PER_THREAD);

    // Each site should have equal ops
    for (int s = 0; s < NUM_SITES; ++s) {
        EXPECT_EQ(site_ops[s].load(), THREADS_PER_SITE * OPS_PER_THREAD)
            << "Site " << s << " ops mismatch";
    }
}

// Test 3: Epoch isolation under stress

TEST_F(SiloMultiSiteStressTest, EpochIsolationUnderStress) {
    const int NUM_SITES = 4;
    const int DURATION_MS = 200;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    // Record initial epochs
    std::vector<mrcu_epoch_type> initial_epochs;
    for (const auto& site : sites) {
        initial_epochs.push_back(site->masstree_context()->get_epoch());
    }

    std::atomic<bool> stop{false};
    std::vector<std::atomic<uint64_t>> epoch_increments(NUM_SITES);
    for (auto& inc : epoch_increments) inc.store(0);

    // Each site advances epochs at different rates
    std::vector<std::thread> threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        threads.emplace_back([&, s]() {
            // Different increment amounts per site
            int increment = (s + 1) * 2;
            int delay_us = 100 * (s + 1);

            while (!stop.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(increment);
                epoch_increments[s] += increment;
                std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    stop = true;

    for (auto& t : threads) t.join();

    // Verify epochs advanced independently
    for (int s = 0; s < NUM_SITES; ++s) {
        mrcu_epoch_type final_epoch = sites[s].get_mut()->masstree_context()->get_epoch();
        mrcu_epoch_type expected_delta = epoch_increments[s].load();

        EXPECT_EQ(final_epoch, initial_epochs[s] + expected_delta)
            << "Site " << s << " epoch mismatch";

        // Each site should have different final epochs (due to different rates)
        for (int other = s + 1; other < NUM_SITES; ++other) {
            mrcu_epoch_type other_epoch = sites[other].get_mut()->masstree_context()->get_epoch();
            // Epochs likely differ (not guaranteed but very likely with different rates)
            // Just verify they're all positive
            EXPECT_GT(final_epoch, 1u);
            EXPECT_GT(other_epoch, 1u);
        }
    }
}

// Test 4: Thread registry isolation

TEST_F(SiloMultiSiteStressTest, ThreadRegistryIsolation) {
    const int NUM_SITES = 3;
    const int THREADS_PER_SITE = 5;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    std::vector<std::vector<threadinfo*>> site_threadinfos(NUM_SITES);
    std::mutex mtx;

    // Create threads for each site
    std::vector<std::thread> threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        for (int t = 0; t < THREADS_PER_SITE; ++t) {
            threads.emplace_back([&, s, t]() {
                sites[s].get_mut()->BindToCurrentThread();
                threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS,
                    s * 100 + t);
                ASSERT_NE(ti, nullptr);
                EXPECT_EQ(ti->context(), sites[s].get_mut()->masstree_context());

                std::lock_guard<std::mutex> lock(mtx);
                site_threadinfos[s].push_back(ti);
            });
        }
    }

    for (auto& t : threads) t.join();

    // Verify each site's thread list
    for (int s = 0; s < NUM_SITES; ++s) {
        std::set<threadinfo*> site_set;
        for (threadinfo* ti = sites[s].get_mut()->masstree_context()->get_allthreads();
             ti; ti = ti->next()) {
            site_set.insert(ti);
            EXPECT_EQ(ti->context(), sites[s].get_mut()->masstree_context())
                << "Thread belongs to wrong context";
        }

        // All created threads should be in the list
        for (auto* ti : site_threadinfos[s]) {
            EXPECT_TRUE(site_set.count(ti) > 0)
                << "Site " << s << " missing threadinfo";
        }

        // No other site's threads should be in this list
        for (int other = 0; other < NUM_SITES; ++other) {
            if (other == s) continue;
            for (auto* ti : site_threadinfos[other]) {
                EXPECT_TRUE(site_set.count(ti) == 0)
                    << "Site " << s << " has site " << other << "'s threadinfo";
            }
        }
    }
}

// Test 5: Heavy insert/search workload across sites

TEST_F(SiloMultiSiteStressTest, HeavyInsertSearchWorkload) {
    const int NUM_SITES = 2;
    const int NUM_KEYS = 50000;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    std::vector<std::unique_ptr<TestTree>> trees;
    std::vector<std::vector<std::unique_ptr<uint64_t>>> value_storage(NUM_SITES);
    std::vector<std::vector<u64_varkey>> keys(NUM_SITES);

    for (int s = 0; s < NUM_SITES; ++s) {
        sites.push_back(SiloRuntime::Create());
        trees.emplace_back(std::make_unique<TestTree>());
        keys[s].reserve(NUM_KEYS);
        value_storage[s].reserve(NUM_KEYS);
        for (int i = 0; i < NUM_KEYS; ++i) {
            keys[s].emplace_back(static_cast<uint64_t>(s * 10000000 + i));
        }
    }

    std::atomic<bool> stop_epochs{false};
    std::atomic<int> sites_done{0};

    auto heavy_worker = [&](int site_idx) {
        SiloRuntime* site = sites[site_idx].get_mut();
        site->BindToCurrentThread();

        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, site_idx);
        ti->rcu_start();

        auto start = std::chrono::high_resolution_clock::now();

        // Insert phase
        for (int i = 0; i < NUM_KEYS; ++i) {
            trees[site_idx]->insert(keys[site_idx][i],
                MakeValue(value_storage[site_idx], site_idx * 10000000 + i));
            if (i % 500 == 0) {
                ti->rcu_quiesce();
            }
        }

        // Search phase - verify all keys
        int found = 0;
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type val{};
            if (trees[site_idx]->search(keys[site_idx][i], val)) {
                uint64_t expected = site_idx * 10000000 + i;
                uint64_t actual = *reinterpret_cast<uint64_t*>(val);
                EXPECT_EQ(actual, expected);
                found++;
            }
        }
        EXPECT_EQ(found, NUM_KEYS);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        ti->rcu_stop();
        sites_done++;
    };

    // Epoch threads
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop_epochs.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Workers
    std::vector<std::thread> workers;
    for (int s = 0; s < NUM_SITES; ++s) {
        workers.emplace_back(heavy_worker, s);
    }

    for (auto& w : workers) w.join();

    stop_epochs = true;
    for (auto& e : epoch_threads) e.join();

    EXPECT_EQ(sites_done.load(), NUM_SITES);
}

// Test 6: Many sites stress test

TEST_F(SiloMultiSiteStressTest, ManySitesStress) {
    const int NUM_SITES = 8;
    const int OPS_PER_SITE = 1000;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    // Verify all unique
    std::set<int> ids;
    std::set<const MasstreeContext*> ctxs;
    for (const auto& site : sites) {
        ASSERT_TRUE(ids.insert(site->id()).second);
        ASSERT_TRUE(ctxs.insert(site->masstree_context()).second);
    }

    std::atomic<int> total_completed{0};
    std::atomic<bool> stop{false};

    auto worker = [&](int site_idx) {
        sites[site_idx].get_mut()->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, site_idx);

        ti->rcu_start();

        for (int i = 0; i < OPS_PER_SITE; ++i) {
            void* p = ti->allocate(64, memtag_value);
            ASSERT_NE(p, nullptr);
            ti->deallocate_rcu(p, 64, memtag_value);

            if (i % 50 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
        total_completed++;
    };

    // Epoch threads
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }

    // Workers
    std::vector<std::thread> workers;
    for (int s = 0; s < NUM_SITES; ++s) {
        workers.emplace_back(worker, s);
    }

    for (auto& w : workers) w.join();

    stop = true;
    for (auto& e : epoch_threads) e.join();

    EXPECT_EQ(total_completed.load(), NUM_SITES);
}

// Test 7: Interleaved operations across sites

TEST_F(SiloMultiSiteStressTest, InterleavedOperations) {
    const int NUM_SITES = 4;
    const int NUM_ROUNDS = 100;
    const int OPS_PER_ROUND = 50;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    std::atomic<bool> stop{false};
    std::atomic<int> rounds_completed{0};

    // Worker that switches between sites
    auto interleaved_worker = [&](int worker_id) {
        std::mt19937 rng(worker_id);
        std::uniform_int_distribution<int> site_dist(0, NUM_SITES - 1);

        for (int round = 0; round < NUM_ROUNDS; ++round) {
            // Pick a random site
            int site_idx = site_dist(rng);
            SiloRuntime* site = sites[site_idx].get_mut();
            site->BindToCurrentThread();

            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS,
                worker_id * 10000 + round);

            ti->rcu_start();

            for (int op = 0; op < OPS_PER_ROUND; ++op) {
                void* p = ti->allocate(64, memtag_value);
                ASSERT_NE(p, nullptr);
                ti->deallocate_rcu(p, 64, memtag_value);
            }

            ti->rcu_quiesce();
            ti->rcu_stop();

            rounds_completed++;
        }
    };

    // Epoch threads
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Run multiple workers
    const int NUM_WORKERS = 4;
    std::vector<std::thread> workers;
    for (int w = 0; w < NUM_WORKERS; ++w) {
        workers.emplace_back(interleaved_worker, w);
    }

    for (auto& w : workers) w.join();

    stop = true;
    for (auto& e : epoch_threads) e.join();

    EXPECT_EQ(rounds_completed.load(), NUM_WORKERS * NUM_ROUNDS);
}

// Test 8: Long-running concurrent test

TEST_F(SiloMultiSiteStressTest, LongRunningConcurrent) {
    const int NUM_SITES = 3;
    const int DURATION_MS = 500;
    const int THREADS_PER_SITE = 4;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < NUM_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    std::atomic<bool> stop{false};
    std::vector<std::atomic<uint64_t>> ops_count(NUM_SITES);
    for (auto& op : ops_count) op.store(0);

    auto continuous_worker = [&](int site_idx, int thread_idx) {
        sites[site_idx].get_mut()->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS,
            site_idx * 1000 + thread_idx);

        ti->rcu_start();

        while (!stop.load()) {
            void* p = ti->allocate(64, memtag_value);
            if (p) {
                ti->deallocate_rcu(p, 64, memtag_value);
                ops_count[site_idx]++;
            }

            if (ops_count[site_idx] % 100 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
    };

    // Epoch threads
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }

    // Workers
    std::vector<std::thread> workers;
    for (int s = 0; s < NUM_SITES; ++s) {
        for (int t = 0; t < THREADS_PER_SITE; ++t) {
            workers.emplace_back(continuous_worker, s, t);
        }
    }

    // Run for duration
    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    stop = true;

    for (auto& w : workers) w.join();
    for (auto& e : epoch_threads) e.join();

    // All sites should have performed operations
    uint64_t total_ops = 0;
    for (int s = 0; s < NUM_SITES; ++s) {
        EXPECT_GT(ops_count[s].load(), 0u) << "Site " << s << " had no ops";
        total_ops += ops_count[s].load();
    }
    EXPECT_GT(total_ops, 1000u) << "Expected significant total ops";
}

// Test 9: Site creation under load

TEST_F(SiloMultiSiteStressTest, SiteCreationUnderLoad) {
    const int INITIAL_SITES = 2;
    const int ADDITIONAL_SITES = 4;
    const int OPS_PER_SITE = 500;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    for (int i = 0; i < INITIAL_SITES; ++i) {
        sites.push_back(SiloRuntime::Create());
    }

    std::atomic<bool> stop{false};
    std::atomic<int> ops_completed{0};
    std::mutex sites_mutex;

    // Worker that uses existing sites
    auto worker = [&](int worker_id) {
        for (int op = 0; op < OPS_PER_SITE; ++op) {
            SiloRuntime* site;
            {
                std::lock_guard<std::mutex> lock(sites_mutex);
                site = sites[worker_id % sites.size()].get_mut();
            }

            site->BindToCurrentThread();
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS,
                worker_id * 10000 + op);

            ti->rcu_start();
            void* p = ti->allocate(64, memtag_value);
            ASSERT_NE(p, nullptr);
            ti->deallocate_rcu(p, 64, memtag_value);
            ti->rcu_quiesce();
            ti->rcu_stop();

            ops_completed++;
        }
    };

    // Thread that creates new sites
    std::thread site_creator([&]() {
        for (int i = 0; i < ADDITIONAL_SITES; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            rusty::Arc<SiloRuntime> new_site = SiloRuntime::Create();
            {
                std::lock_guard<std::mutex> lock(sites_mutex);
                sites.push_back(std::move(new_site));
            }
        }
    });

    // Epoch threads for initial sites
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < INITIAL_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Workers
    const int NUM_WORKERS = 4;
    std::vector<std::thread> workers;
    for (int w = 0; w < NUM_WORKERS; ++w) {
        workers.emplace_back(worker, w);
    }

    site_creator.join();
    for (auto& w : workers) w.join();

    stop = true;
    for (auto& e : epoch_threads) e.join();

    EXPECT_EQ(ops_completed.load(), NUM_WORKERS * OPS_PER_SITE);
    EXPECT_EQ(sites.size(), INITIAL_SITES + ADDITIONAL_SITES);
}

// Test 10: Stress test with mixed tree operations

TEST_F(SiloMultiSiteStressTest, MixedTreeOperationsStress) {
    const int NUM_SITES = 3;
    const int KEYS_PER_SITE = 5000;

    std::vector<rusty::Arc<SiloRuntime>> sites;
    std::vector<std::unique_ptr<TestTree>> trees;
    std::vector<std::vector<std::unique_ptr<uint64_t>>> value_storage(NUM_SITES);

    for (int s = 0; s < NUM_SITES; ++s) {
        sites.push_back(SiloRuntime::Create());
        trees.emplace_back(std::make_unique<TestTree>());
        value_storage[s].reserve(KEYS_PER_SITE * 2);
    }

    std::atomic<bool> stop{false};
    std::vector<std::atomic<int>> insert_counts(NUM_SITES);
    std::vector<std::atomic<int>> search_counts(NUM_SITES);
    for (int i = 0; i < NUM_SITES; ++i) {
        insert_counts[i] = 0;
        search_counts[i] = 0;
    }

    auto mixed_worker = [&](int site_idx) {
        sites[site_idx].get_mut()->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, site_idx);

        std::mt19937 rng(site_idx * 1000);
        std::uniform_int_distribution<int> op_dist(0, 2);  // 0=insert, 1=search, 2=search

        ti->rcu_start();

        // Store raw key values (not u64_varkey copies which may have pointer issues)
        std::vector<uint64_t> inserted_key_values;
        inserted_key_values.reserve(KEYS_PER_SITE);

        for (int i = 0; i < KEYS_PER_SITE; ++i) {
            int op = op_dist(rng);

            if (op == 0 || inserted_key_values.empty()) {
                // Insert
                uint64_t key_val = site_idx * 10000000 + i;
                u64_varkey key(key_val);
                trees[site_idx]->insert(key,
                    MakeValue(value_storage[site_idx], key_val));
                inserted_key_values.push_back(key_val);
                insert_counts[site_idx]++;
            } else {
                // Search existing key - recreate u64_varkey from stored value
                std::uniform_int_distribution<size_t> key_dist(0, inserted_key_values.size() - 1);
                size_t idx = key_dist(rng);
                u64_varkey search_key(inserted_key_values[idx]);
                TestTree::value_type found{};
                bool result = trees[site_idx]->search(search_key, found);
                EXPECT_TRUE(result) << "Missing key " << inserted_key_values[idx]
                    << " at site " << site_idx;
                search_counts[site_idx]++;
            }

            if (i % 100 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
    };

    // Epoch threads
    std::vector<std::thread> epoch_threads;
    for (int s = 0; s < NUM_SITES; ++s) {
        epoch_threads.emplace_back([&, s]() {
            while (!stop.load()) {
                sites[s].get_mut()->masstree_context()->increment_epoch(2);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    // Workers
    std::vector<std::thread> workers;
    for (int s = 0; s < NUM_SITES; ++s) {
        workers.emplace_back(mixed_worker, s);
    }

    for (auto& w : workers) w.join();

    stop = true;
    for (auto& e : epoch_threads) e.join();

    // Verify each site processed all operations
    for (int s = 0; s < NUM_SITES; ++s) {
        EXPECT_EQ(insert_counts[s].load() + search_counts[s].load(), KEYS_PER_SITE)
            << "Site " << s << " operation count mismatch";
        EXPECT_GT(insert_counts[s].load(), 0) << "Site " << s << " had no inserts";
    }
}
