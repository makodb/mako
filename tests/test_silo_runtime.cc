/**
 * test_silo_runtime.cc
 *
 * Tests for SiloRuntime - per-site runtime context.
 * Verifies that multiple sites can run independently in a single process.
 * Uses RustyCpp smart pointers for memory safety.
 */

#include <stdint.h>

#include <gtest/gtest.h>


#include "silo_runtime.h"
#include "core.h"
#include "macros.h"
#include "spinbarrier.h"
#include "masstree/masstree_context.h"
#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"
#include "sto/MassTrans.hh"
#include "sto/TBox.hh"
#include "sto/ThreadPool.h"
#include "sto/Transaction.hh"
#include "storage/mbta_wrapper.hh"

import std;

// Provide globalepoch definition for this test file
volatile mrcu_epoch_type globalepoch = 1;

// Production benchmark binaries provide this once from common2.h. This
// focused unit-test binary does not link that benchmark-only definition.
abstract_db* ThreadDBWrapperMbta::replay_thread_wrapper_db = nullptr;

using TestTree = single_threaded_btree;

class LocalTimestampBox final : public TBox<uint64_t> {
public:
    using TBox<uint64_t>::TBox;

    bool get_is_remote() const override { return false; }
};

// Models MassTrans insert ownership: staging creates an invalid physical row,
// install makes it visible while retaining the item lock, abort cleanup removes
// it, and committed cleanup preserves it.
class ParticipantInsertObject final : public TObject {
public:
    void stage_insert() {
        present_ = true;
        Sto::item(this, 0).add_write<uint8_t>(1);
    }

    void stage_fresh_inserts(size_t count) {
        present_ = count != 0;
        for (size_t index = 0; index != count; ++index)
            Sto::fresh_item(this, index).add_write<uint8_t>(1);
    }

    bool lock(TransItem& item, Transaction& transaction) override {
        return transaction.try_lock(item, version_);
    }
    bool check(TransItem&, Transaction&) override { return true; }
    void install(TransItem&, Transaction&) override {
        installed_ = true;
        ++install_count_;
    }
    void unlock(TransItem&) override {
        version_.unlock();
        ++unlock_count_;
    }
    void cleanup(TransItem&, bool committed) override {
        if (committed) {
            ++committed_cleanup_count_;
        } else {
            present_ = false;
            ++abort_cleanup_count_;
        }
    }
    bool get_is_remote() const override { return false; }

    bool present() const { return present_; }
    bool installed() const { return installed_; }
    size_t install_count() const { return install_count_; }
    size_t unlock_count() const { return unlock_count_; }
    size_t committed_cleanup_count() const {
        return committed_cleanup_count_;
    }
    size_t abort_cleanup_count() const { return abort_cleanup_count_; }

private:
    TVersion version_;
    bool present_ = false;
    bool installed_ = false;
    size_t install_count_ = 0;
    size_t unlock_count_ = 0;
    size_t committed_cleanup_count_ = 0;
    size_t abort_cleanup_count_ = 0;
};

class ThrowingInstallObject final : public TObject {
public:
    void stage_write() {
        Sto::item(this, 0).add_write<uint8_t>(1);
    }

    bool lock(TransItem& item, Transaction& transaction) override {
        return transaction.try_lock(item, version_);
    }
    bool check(TransItem&, Transaction&) override { return true; }
    void install(TransItem&, Transaction&) override {
        throw std::runtime_error("synthetic install failure");
    }
    void unlock(TransItem&) override { version_.unlock(); }
    void cleanup(TransItem&, bool) override {}
    bool get_is_remote() const override { return false; }

private:
    TVersion version_;
};

// Exercises transaction-set slot reuse without involving Masstree's
// process-lifetime allocations. The exact ASan CTest keeps leak detection on,
// so replacing a live TransItem and orphaning its non-SSO extra string is a
// test failure rather than a bounded-retention qualification.
class TransItemExtraObject final : public TObject {
public:
    void stage_with_extra(std::string extra) {
        Sto::item(this, 0)
            .add_extra(std::move(extra))
            .add_write<uint8_t>(1);
    }

    void stage_without_extra() {
        Sto::item(this, 0).add_write<uint8_t>(1);
    }

    bool lock(TransItem& item, Transaction& transaction) override {
        return transaction.try_lock(item, version_);
    }
    bool check(TransItem&, Transaction&) override { return true; }
    void install(TransItem& item, Transaction&) override {
        installed_extra_ = item.get_extra();
        version_.inc_nonopaque_version();
    }
    void unlock(TransItem&) override { version_.unlock(); }
    void cleanup(TransItem&, bool) override {}
    bool get_is_remote() const override { return false; }

    const std::string& installed_extra() const { return installed_extra_; }

private:
    TVersion version_;
    std::string installed_extra_;
};

static void configure_standalone_mbta_thread_test() {
    auto& config = BenchmarkConfig::getInstance();
    config.setConfig(nullptr);
    config.setNthreads(1);
    config.setNshards(1);
    config.setShardIndex(0);
    config.setIsReplicated(0);
    config.setPaxosProcName(mako::LOCALHOST_CENTER);
}

class SiloRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create two runtimes for two sites using Arc smart pointers
        // Use rusty::Some() to wrap Arc in Option
        site1_ = rusty::Some(SiloRuntime::Create());
        site2_ = rusty::Some(SiloRuntime::Create());

        ASSERT_TRUE(site1_.is_some());
        ASSERT_TRUE(site2_.is_some());
        ASSERT_NE(site1()->id(), site2()->id());
        ASSERT_NE(site1()->masstree_context(), site2()->masstree_context());
    }

    void TearDown() override {
        SiloRuntime::BindCurrentThread(nullptr);
        // Reset to None, Arc handles cleanup automatically
        site1_ = rusty::None;
        site2_ = rusty::None;
    }

    // Helper to get the runtime pointer (use as_ref to avoid moving the Arc)
    SiloRuntime* site1() { return site1_.as_ref().unwrap().as_ptr(); }
    SiloRuntime* site2() { return site2_.as_ref().unwrap().as_ptr(); }

    // Use Option<Arc> for nullable Arc members (gtest requires default-constructible fixtures)
    rusty::Option<rusty::Arc<SiloRuntime>> site1_;
    rusty::Option<rusty::Arc<SiloRuntime>> site2_;
};

// Test 1: Basic runtime creation and isolation

TEST_F(SiloRuntimeTest, BasicCreationAndIsolation) {
    // Each runtime should have its own MasstreeContext
    EXPECT_NE(site1()->masstree_context()->id(), site2()->masstree_context()->id());

    // Initial epochs should both be 1
    EXPECT_EQ(site1()->masstree_context()->get_epoch(), 1u);
    EXPECT_EQ(site2()->masstree_context()->get_epoch(), 1u);
}

// Test 2: Thread binding and Current() accessor

TEST_F(SiloRuntimeTest, ThreadBindingAndCurrent) {
    // Bind site1 to this thread
    SiloRuntime::BindCurrentThread(site1());
    EXPECT_EQ(SiloRuntime::Current(), site1());
    EXPECT_EQ(MasstreeContext::Current(), site1()->masstree_context());

    // Rebind to site2
    SiloRuntime::BindCurrentThread(site2());
    EXPECT_EQ(SiloRuntime::Current(), site2());
    EXPECT_EQ(MasstreeContext::Current(), site2()->masstree_context());
}

// Test 3: Concurrent operations on different sites

TEST_F(SiloRuntimeTest, ConcurrentSiteOperations) {
    const int NUM_THREADS_PER_SITE = 4;
    const int OPS_PER_THREAD = 100;

    std::atomic<int> site1_completed{0};
    std::atomic<int> site2_completed{0};

    // Get raw pointers for thread use (Arc ensures lifetime)
    SiloRuntime* site1_ptr = site1();
    SiloRuntime* site2_ptr = site2();

    // Worker function
    auto worker = [](SiloRuntime* site, int thread_id,
                     std::atomic<int>& completed, int ops) {
        site->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, thread_id);

        ti->rcu_start();

        for (int i = 0; i < ops; ++i) {
            void* p = ti->allocate(64, memtag_value);
            ASSERT_NE(p, nullptr);
            ti->deallocate_rcu(p, 64, memtag_value);

            if (i % 10 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
        completed++;
    };

    // Spawn threads for site1
    std::vector<std::thread> site1_threads;
    for (int i = 0; i < NUM_THREADS_PER_SITE; ++i) {
        site1_threads.emplace_back(worker, site1_ptr, 1000 + i,
                                   std::ref(site1_completed), OPS_PER_THREAD);
    }

    // Spawn threads for site2
    std::vector<std::thread> site2_threads;
    for (int i = 0; i < NUM_THREADS_PER_SITE; ++i) {
        site2_threads.emplace_back(worker, site2_ptr, 2000 + i,
                                   std::ref(site2_completed), OPS_PER_THREAD);
    }

    // Epoch advancement threads
    std::atomic<bool> stop{false};

    std::thread epoch1([site1_ptr, &stop]() {
        while (!stop.load()) {
            site1_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread epoch2([site2_ptr, &stop]() {
        while (!stop.load()) {
            site2_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Wait for workers
    for (auto& t : site1_threads) t.join();
    for (auto& t : site2_threads) t.join();

    stop = true;
    epoch1.join();
    epoch2.join();

    EXPECT_EQ(site1_completed.load(), NUM_THREADS_PER_SITE);
    EXPECT_EQ(site2_completed.load(), NUM_THREADS_PER_SITE);
}

// Test 4: Two sites with independent Masstree instances

TEST_F(SiloRuntimeTest, IndependentMasstreeInstances) {
    const int NUM_KEYS = 5000;

    TestTree tree1;
    TestTree tree2;

    std::atomic<bool> done1{false};
    std::atomic<bool> done2{false};
    std::atomic<bool> stop_epoch{false};

    // Storage for values and keys
    std::vector<std::unique_ptr<uint64_t>> values1;
    std::vector<std::unique_ptr<uint64_t>> values2;
    std::vector<u64_varkey> keys1;
    std::vector<u64_varkey> keys2;

    values1.reserve(NUM_KEYS);
    values2.reserve(NUM_KEYS);
    keys1.reserve(NUM_KEYS);
    keys2.reserve(NUM_KEYS);

    for (int i = 0; i < NUM_KEYS; ++i) {
        keys1.emplace_back(static_cast<uint64_t>(i));
        keys2.emplace_back(static_cast<uint64_t>(i + 1000000));  // Different key space
    }

    auto make_value = [](std::vector<std::unique_ptr<uint64_t>>& storage, uint64_t v) -> TestTree::value_type {
        storage.emplace_back(std::make_unique<uint64_t>(v));
        return reinterpret_cast<TestTree::value_type>(storage.back().get());
    };

    // Get raw pointers for thread use
    SiloRuntime* site1_ptr = site1();
    SiloRuntime* site2_ptr = site2();

    // Worker for site1
    std::thread worker1([&, site1_ptr]() {
        site1_ptr->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 3000);

        ti->rcu_start();

        // Insert keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            tree1.insert(keys1[i], make_value(values1, i));
            if (i % 100 == 0) ti->rcu_quiesce();
        }

        // Verify keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            EXPECT_TRUE(tree1.search(keys1[i], found)) << "site1 tree missing key " << i;
        }

        ti->rcu_stop();
        done1 = true;
    });

    // Worker for site2
    std::thread worker2([&, site2_ptr]() {
        site2_ptr->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 4000);

        ti->rcu_start();

        // Insert keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            tree2.insert(keys2[i], make_value(values2, i + 1000000));
            if (i % 100 == 0) ti->rcu_quiesce();
        }

        // Verify keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            EXPECT_TRUE(tree2.search(keys2[i], found)) << "site2 tree missing key " << i;
        }

        ti->rcu_stop();
        done2 = true;
    });

    // Epoch threads
    std::thread epoch1([site1_ptr, &stop_epoch]() {
        while (!stop_epoch.load()) {
            site1_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread epoch2([site2_ptr, &stop_epoch]() {
        while (!stop_epoch.load()) {
            site2_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    worker1.join();
    worker2.join();

    stop_epoch = true;
    epoch1.join();
    epoch2.join();

    EXPECT_TRUE(done1.load());
    EXPECT_TRUE(done2.load());

    // Cross-check: tree1 should NOT have tree2 keys and vice versa
    for (int i = 0; i < 100; ++i) {
        TestTree::value_type found{};
        EXPECT_FALSE(tree1.search(keys2[i], found)) << "tree1 should not have tree2 key " << i;
        EXPECT_FALSE(tree2.search(keys1[i], found)) << "tree2 should not have tree1 key " << i;
    }
}

// Test 5: Concurrent global-default initialization publishes one stable owner

TEST_F(SiloRuntimeTest, ConcurrentGlobalDefaultInitialization) {
    constexpr size_t thread_count = 32;
    std::array<SiloRuntime*, thread_count> global_runtimes{};
    std::array<SiloRuntime*, thread_count> current_runtimes{};
    std::array<rcu*, thread_count> rcus{};
    std::array<ticker*, thread_count> tickers{};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            tl_silo_runtime = nullptr;
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            global_runtimes[index] = SiloRuntime::GlobalDefault();
            current_runtimes[index] = SiloRuntime::Current();
            rcus[index] = &global_runtimes[index]->get_rcu();
            tickers[index] = &global_runtimes[index]->get_ticker();
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();

    ASSERT_NE(global_runtimes.front(), nullptr);
    for (size_t index = 0; index < thread_count; ++index) {
        EXPECT_EQ(global_runtimes[index], global_runtimes.front());
        EXPECT_EQ(current_runtimes[index], global_runtimes.front());
        EXPECT_EQ(rcus[index], rcus.front());
        EXPECT_EQ(tickers[index], tickers.front());
    }

    SiloRuntime* late_global = nullptr;
    SiloRuntime* late_current = nullptr;
    std::thread late_reader([&] {
        late_global = SiloRuntime::GlobalDefault();
        late_current = SiloRuntime::Current();
    });
    late_reader.join();
    EXPECT_EQ(late_global, global_runtimes.front());
    EXPECT_EQ(late_current, global_runtimes.front());
}

TEST(SiloRuntimeLifetimeTest, LazyServicesHaveBoundedTeardown) {
    {
        auto runtime = SiloRuntime::Create();
        ASSERT_NE(runtime.as_ptr(), nullptr);
        (void)runtime.as_ptr()->get_rcu();
        (void)runtime.as_ptr()->get_ticker();
    }
}

TEST(CoreStorageTest, LazyPerCoreStorageHonorsOverAlignment) {
    struct alignas(128) OverAlignedValue {
        uint64_t value;
    };

    percore_lazy<OverAlignedValue> values;
    OverAlignedValue& value = values.get(0, OverAlignedValue{42});

    EXPECT_EQ(reinterpret_cast<uintptr_t>(&value) % alignof(OverAlignedValue),
              0u);
    EXPECT_EQ(value.value, 42u);
    EXPECT_EQ(values.view(0), &value);
}

TEST(SpinBarrierTest, WaitingThreadObservesEveryParticipantWrite) {
    constexpr size_t thread_count = 8;
    spin_barrier barrier(thread_count);
    std::array<uint64_t, thread_count> published{};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (size_t index = 0; index < thread_count; ++index) {
        workers.emplace_back([&, index] {
            published[index] = index + 1;
            barrier.count_down();
        });
    }

    barrier.wait_for();
    for (size_t index = 0; index < thread_count; ++index)
        EXPECT_EQ(published[index], index + 1);

    for (auto& worker : workers)
        worker.join();
}

TEST(MasstreeContextEpochTest, ConcurrentAdvanceIsMonotonicAndIndependent) {
    MasstreeContext context1;
    MasstreeContext context2;
    std::array<MasstreeContext*, 2> contexts{&context1, &context2};
    constexpr size_t thread_count = 8;
    constexpr Transaction::epoch_type epoch_step = 1024;
    const Transaction::epoch_type target_epoch =
        Transaction::global_epochs.global_epoch.fetch_add(
            epoch_step, std::memory_order_acq_rel) + epoch_step;
    std::array<Transaction::epoch_type, 2> requested_maxima{
        target_epoch, target_epoch};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (size_t index = 0; index < thread_count; ++index) {
        const size_t context_index = index % contexts.size();
        const Transaction::epoch_type requested_epoch =
            target_epoch + static_cast<Transaction::epoch_type>(index + 1);
        requested_maxima[context_index] =
            std::max(requested_maxima[context_index], requested_epoch);
        workers.emplace_back([&, context_index, requested_epoch] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            // This is the same monotonic operation used by the MassTrans
            // transaction-start callback before entering Masstree RCU.
            contexts[context_index]->advance_epoch_to_at_least(target_epoch);
            contexts[context_index]->advance_epoch_to_at_least(requested_epoch);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& worker : workers)
        worker.join();

    for (size_t index = 0; index < contexts.size(); ++index)
        EXPECT_GE(contexts[index]->get_epoch(), requested_maxima[index]);

    // Concurrent delayed observations must not undo either context's maximum.
    std::array<Transaction::epoch_type, 2> before_stale{
        contexts[0]->get_epoch(), contexts[1]->get_epoch()};
    workers.clear();
    for (size_t index = 0; index < thread_count; ++index) {
        workers.emplace_back([&, index] {
            const size_t context_index = index % contexts.size();
            contexts[context_index]->advance_epoch_to_at_least(
                before_stale[context_index] - 1);
        });
    }
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(contexts[0]->get_epoch(), before_stale[0]);
    EXPECT_EQ(contexts[1]->get_epoch(), before_stale[1]);
}

TEST(MassTransEpochIntegrationTest, TransactionStartAdvancesOwningContext) {
    using TestMassTrans =
        MassTrans<std::string, versioned_str_struct, false>;

    // MassTrans threadinfo records intentionally live for process lifetime.
    // Attach this one to the process-rooted default runtime so the registry
    // and its context cannot dangle when the test returns.
    SiloRuntime* const runtime = SiloRuntime::GlobalDefault();
    runtime->BindToCurrentThread();
    MasstreeContext* const context = runtime->masstree_context();

    constexpr Transaction::epoch_type epoch_step = 1024;
    const Transaction::epoch_type target_epoch =
        Transaction::global_epochs.global_epoch.fetch_add(
            epoch_step, std::memory_order_acq_rel) + epoch_step;
    ASSERT_LT(context->get_epoch(), target_epoch);

    TThread::set_id(MAX_THREADS - 1);
    TThread::set_mode(0);
    TestMassTrans::thread_init();
    Sto::start_transaction();
    EXPECT_GE(context->get_epoch(), target_epoch);
    Sto::silent_abort();
    Transaction::rcu_quiesce();
    SiloRuntime::BindCurrentThread(nullptr);
}

TEST(MassTransEpochIntegrationTest,
     FreshReplayThreadUsesGlobalRuntimeEpochDomain) {
    SiloRuntime* const runtime = SiloRuntime::GlobalDefault();
    runtime->BindToCurrentThread();

    // DB::Open constructs replay tables while bound to GlobalDefault. Model
    // that ownership here, then access the table from a fresh replay OS thread.
    actual_directs table;
    ASSERT_NE(actual_directs::mythreadinfo.ti, nullptr);
    ASSERT_EQ(actual_directs::mythreadinfo.ti->context(),
              runtime->masstree_context());

    const std::string key = "replay-epoch-domain-key";
    const std::string encoded_value = mako::Encode("replay-value");
    SiloRuntime* observed_runtime = nullptr;
    MasstreeContext* observed_context = nullptr;
    std::string observed_value;
    std::string failure;
    bool inserted = false;
    bool found = false;
    bool removed = false;
    std::atomic<bool> reclaimed{false};

    struct reclaim_probe final : public threadinfo::mrcu_callback {
        explicit reclaim_probe(std::atomic<bool>& reclaimed)
            : reclaimed_(reclaimed) {}

        void operator()(threadinfo&) override {
            reclaimed_.store(true, std::memory_order_release);
            delete this;
        }

        std::atomic<bool>& reclaimed_;
    };

    std::thread replay_thread([&] {
        try {
            // Explicitly clear inherited assumptions: std::thread begins with
            // fresh TLS, and getDB() itself must establish both bindings.
            SiloRuntime::BindCurrentThread(nullptr);
            ThreadDBWrapperMbta wrapper(0);
            (void)wrapper.getDB();

            observed_runtime = SiloRuntime::Current();
            threadinfo* const ti = actual_directs::mythreadinfo.ti;
            if (ti == nullptr) {
                throw std::logic_error(
                    "replay initialization did not create threadinfo");
            }
            observed_context = ti->context();

            inserted = table.put(actual_directs::Str(key), encoded_value);
            found = table.get(actual_directs::Str(key), observed_value);

            // erase() runs a one-operation OCC transaction and queues the
            // actual row allocation for RCU. The probe makes successful
            // progress through that same limbo queue observable without
            // exposing Masstree allocator internals.
            removed = table.erase(actual_directs::Str(key));
            ti->rcu_register(new reclaim_probe(reclaimed));
            for (int attempt = 0;
                 attempt != 5 &&
                 !reclaimed.load(std::memory_order_acquire);
                 ++attempt) {
                runtime->masstree_context()->increment_epoch(2);
                ti->rcu_quiesce();
            }
            ti->rcu_stop();
            Transaction::rcu_quiesce();

            delete TThread::txn;
            TThread::txn = nullptr;
            SiloRuntime::BindCurrentThread(nullptr);
        } catch (const std::exception& error) {
            failure = error.what();
        } catch (...) {
            failure = "unknown replay-thread exception";
        }
    });
    replay_thread.join();

    EXPECT_TRUE(failure.empty()) << failure;
    EXPECT_EQ(observed_runtime, runtime);
    EXPECT_EQ(observed_context, runtime->masstree_context());
    EXPECT_TRUE(inserted);
    EXPECT_TRUE(found);
    EXPECT_EQ(observed_value, encoded_value);
    EXPECT_TRUE(removed);
    EXPECT_TRUE(reclaimed.load(std::memory_order_acquire));
    EXPECT_EQ(table.approx_size(), 0U);

    SiloRuntime::BindCurrentThread(nullptr);
}

TEST(StoEpochAdvancerLifecycleTest, ConcurrentStartIsIdempotentAndAdvances) {
    const Transaction::epoch_type initial_epoch =
        Transaction::global_epochs.global_epoch.load(std::memory_order_acquire);
    constexpr size_t thread_count = 32;
    std::atomic<bool> start{false};
    std::vector<std::thread> starters;
    starters.reserve(thread_count);
    for (size_t index = 0; index < thread_count; ++index) {
        starters.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            Transaction::start_epoch_advancer();
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : starters)
        thread.join();

    EXPECT_TRUE(Transaction::global_epochs.run.load(std::memory_order_acquire));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (Transaction::global_epochs.global_epoch.load(
               std::memory_order_acquire) <= initial_epoch &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(Transaction::global_epochs.global_epoch.load(
                  std::memory_order_acquire),
              initial_epoch);
}

TEST(StoThreadIdLifecycleTest,
     StableAllocatorIsUniqueAndRestoresOneIdPerOsThread) {
    constexpr size_t thread_count = 32;
    std::array<int, thread_count> assigned{};
    std::array<int, thread_count> restored{};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            assigned[index] = TThread::assign_stable_id();
            TThread::set_id(MAX_THREADS - 1);
            restored[index] = TThread::assign_stable_id();
        });
    }
    for (auto& thread : threads)
        thread.join();

    auto sorted = assigned;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());
    for (size_t index = 0; index < thread_count; ++index) {
        EXPECT_GE(assigned[index], 0);
        EXPECT_LT(assigned[index], MAX_THREADS);
        EXPECT_EQ(restored[index], assigned[index]);
    }
}

TEST(StoEpochAdvancerLifecycleTest, ThreadEndAbortsActiveTransaction) {
    // This test needs no BenchmarkConfig or ShardClient: a local TBox write is
    // enough to prove that thread teardown resolves an open transaction before
    // retiring its RCU state.
    TThread::set_mode(0);
    if (TThread::txn != nullptr) {
        if (TThread::txn->has_active_state()) {
            TThread::txn->silent_abort();
        }
        delete TThread::txn;
        TThread::txn = nullptr;
    }
    configure_standalone_mbta_thread_test();
    ASSERT_EQ(TThread::sclient, nullptr);
    mbta_wrapper db;
    db.thread_init(false, 0);

    LocalTimestampBox box(0);
    Sto::start_transaction();
    box.write(7);
    EXPECT_TRUE(Sto::in_progress());

    db.thread_end();

    EXPECT_FALSE(Sto::in_progress());
    EXPECT_EQ(box.nontrans_read(), 0U);
    EXPECT_EQ(TThread::sclient, nullptr);

    delete TThread::txn;
    TThread::txn = nullptr;
}

TEST(StoEpochAdvancerLifecycleTest, ThreadEndStopsIdleModeOneMasstreeRcu) {
    // shard_reset() leaves an empty mode-1 participant transaction ready for
    // the next RPC, but start() has still entered the Masstree RCU region.
    SiloRuntime* const runtime = SiloRuntime::GlobalDefault();
    runtime->BindToCurrentThread();
    TThread::set_mode(0);
    if (TThread::txn != nullptr) {
        if (TThread::txn->has_active_state()) {
            TThread::txn->silent_abort();
        }
        delete TThread::txn;
        TThread::txn = nullptr;
    }

    configure_standalone_mbta_thread_test();
    ASSERT_EQ(TThread::sclient, nullptr);
    mbta_wrapper db;
    db.thread_init(false, 0);
    TThread::set_mode(1);

    auto& end_callback =
        Transaction::tinfo[TThread::id()].trans_end_callback;
    struct callback_restore {
        std::function<void(void)>& slot;
        std::function<void(void)> original;
        ~callback_restore() { slot = std::move(original); }
    } restore{end_callback, std::move(end_callback)};
    size_t end_callback_count = 0;
    end_callback = [&] {
        if (restore.original)
            restore.original();
        ++end_callback_count;
    };

    db.shard_reset();
    EXPECT_NE(TThread::txn, nullptr);
    if (TThread::txn != nullptr) {
        EXPECT_TRUE(TThread::txn->has_active_state());
        EXPECT_FALSE(TThread::txn->has_staged_items());
    }
    EXPECT_NE(mbta_table::mythreadinfo.ti, nullptr);
    if (mbta_table::mythreadinfo.ti != nullptr) {
        EXPECT_TRUE(mbta_table::mythreadinfo.ti->rcu_active());
    }

    db.thread_end();

    EXPECT_EQ(end_callback_count, 1U);
    if (TThread::txn != nullptr) {
        EXPECT_FALSE(TThread::txn->has_active_state());
    }
    if (mbta_table::mythreadinfo.ti != nullptr) {
        EXPECT_FALSE(mbta_table::mythreadinfo.ti->rcu_active());
    }
    EXPECT_EQ(Transaction::tinfo[TThread::id()].epoch.load(
                  std::memory_order_acquire),
              0U);

    TThread::set_mode(0);
    delete TThread::txn;
    TThread::txn = nullptr;
    EXPECT_EQ(end_callback_count, 1U);
    SiloRuntime::BindCurrentThread(nullptr);
}

TEST(StoEpochAdvancerLifecycleTest, ThreadEndUnlocksStagedModeOneWrite) {
    TThread::set_mode(0);
    if (TThread::txn != nullptr) {
        if (TThread::txn->has_active_state()) {
            TThread::txn->silent_abort();
        }
        delete TThread::txn;
        TThread::txn = nullptr;
    }

    configure_standalone_mbta_thread_test();
    mbta_wrapper db;
    db.thread_init(false, 0);
    TThread::set_mode(1);
    EXPECT_EQ(TThread::sclient, nullptr);

    LocalTimestampBox box(0);
    Sto::start_transaction();
    box.write(7);
    EXPECT_TRUE(TThread::txn->has_staged_items());
    EXPECT_TRUE(Sto::shard_try_lock_last_writeset());

    db.thread_end();

    EXPECT_FALSE(TThread::txn->has_active_state());
    EXPECT_EQ(box.nontrans_read(), 0U);
    TThread::set_mode(0);
    delete TThread::txn;
    TThread::txn = nullptr;

    std::atomic<bool> committed{false};
    std::thread contender([&] {
        TThread::set_id(MAX_THREADS - 5);
        TThread::set_mode(0);
        Sto::start_transaction();
        box.write(9);
        committed.store(Sto::try_commit(), std::memory_order_release);
        TThread::set_mode(0);
        delete TThread::txn;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    contender.join();

    EXPECT_TRUE(committed.load(std::memory_order_acquire));
    EXPECT_EQ(box.nontrans_read(), 9U);
}

TEST(StoEpochAdvancerLifecycleTest,
     ThreadEndCommitsInstalledModeOneWriteAndUnlocksIt) {
    TThread::set_mode(0);
    if (TThread::txn != nullptr) {
        if (TThread::txn->has_active_state()) {
            TThread::txn->silent_abort();
        }
        delete TThread::txn;
        TThread::txn = nullptr;
    }

    configure_standalone_mbta_thread_test();
    ASSERT_EQ(TThread::sclient, nullptr);
    mbta_wrapper db;
    db.thread_init(false, 0);
    TThread::set_mode(1);

    LocalTimestampBox box(0);
    Sto::start_transaction();
    box.write(7);
    const bool locked = Sto::shard_try_lock_last_writeset();
    EXPECT_TRUE(locked);
    if (!locked) {
        db.thread_end();
        TThread::set_mode(0);
        delete TThread::txn;
        TThread::txn = nullptr;
        return;
    }
    Sto::shard_install(101);
    EXPECT_TRUE(TThread::txn->has_active_state());
    EXPECT_EQ(box.nontrans_read(), 7U);

    // INSTALL is an irreversible 2PC decision. Teardown must finish committed
    // cleanup, not run abort cleanup over the published value.
    db.thread_end();

    EXPECT_FALSE(TThread::txn->has_active_state());
    EXPECT_EQ(box.nontrans_read(), 7U);
    TThread::set_mode(0);
    delete TThread::txn;
    TThread::txn = nullptr;

    std::atomic<bool> committed{false};
    std::thread contender([&] {
        TThread::set_id(MAX_THREADS - 6);
        TThread::set_mode(0);
        Sto::start_transaction();
        box.write(9);
        committed.store(Sto::try_commit(), std::memory_order_release);
        delete TThread::txn;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    contender.join();

    EXPECT_TRUE(committed.load(std::memory_order_acquire));
    EXPECT_EQ(box.nontrans_read(), 9U);
}

TEST(StoEpochAdvancerLifecycleTest,
     ThreadEndPreservesAnInstalledParticipantInsert) {
    TThread::set_mode(0);
    if (TThread::txn != nullptr) {
        if (TThread::txn->has_active_state()) {
            TThread::txn->silent_abort();
        }
        delete TThread::txn;
        TThread::txn = nullptr;
    }

    configure_standalone_mbta_thread_test();
    mbta_wrapper db;
    db.thread_init(false, 0);
    TThread::set_mode(1);

    ParticipantInsertObject object;
    Sto::start_transaction();
    object.stage_insert();
    const bool locked = Sto::shard_try_lock_last_writeset();
    EXPECT_TRUE(locked);
    if (!locked) {
        db.thread_end();
        TThread::set_mode(0);
        delete TThread::txn;
        TThread::txn = nullptr;
        return;
    }
    EXPECT_EQ(Sto::shard_validate(), 0);
    Sto::shard_install(102);

    // MassTrans has the same cleanup contract: abort removes its staged row,
    // whereas teardown after INSTALL must preserve the row and release its
    // item lock exactly once.
    db.thread_end();
    EXPECT_TRUE(object.present());
    EXPECT_TRUE(object.installed());
    EXPECT_EQ(object.install_count(), 1U);
    EXPECT_EQ(object.unlock_count(), 1U);
    EXPECT_EQ(object.committed_cleanup_count(), 1U);
    EXPECT_EQ(object.abort_cleanup_count(), 0U);

    TThread::set_mode(0);
    delete TThread::txn;
    TThread::txn = nullptr;
}

TEST(StoEpochAdvancerLifecycleTest,
     ThreadEndAfterShardUnlockDoesNotCleanUpTwice) {
    TThread::set_mode(0);
    if (TThread::txn != nullptr) {
        if (TThread::txn->has_active_state()) {
            TThread::txn->silent_abort();
        }
        delete TThread::txn;
        TThread::txn = nullptr;
    }

    configure_standalone_mbta_thread_test();
    ASSERT_EQ(TThread::sclient, nullptr);
    mbta_wrapper db;
    db.thread_init(false, 0);
    TThread::set_mode(1);

    auto& end_callback =
        Transaction::tinfo[TThread::id()].trans_end_callback;
    struct callback_restore {
        std::function<void(void)>& slot;
        std::function<void(void)> original;
        ~callback_restore() { slot = std::move(original); }
    } restore{end_callback, std::move(end_callback)};
    size_t end_callback_count = 0;
    end_callback = [&] {
        if (restore.original)
            restore.original();
        ++end_callback_count;
    };

    LocalTimestampBox box(0);
    Sto::start_transaction();
    box.write(7);
    const bool locked = Sto::shard_try_lock_last_writeset();
    EXPECT_TRUE(locked);
    if (!locked) {
        db.thread_end();
        TThread::set_mode(0);
        delete TThread::txn;
        TThread::txn = nullptr;
        return;
    }
    Sto::shard_install(103);
    Sto::shard_unlock(true);

    EXPECT_FALSE(TThread::txn->has_active_state());
    EXPECT_FALSE(TThread::txn->has_staged_items());
    EXPECT_EQ(end_callback_count, 1U);
    EXPECT_EQ(box.nontrans_read(), 7U);

    db.thread_end();

    EXPECT_EQ(end_callback_count, 1U);
    EXPECT_EQ(box.nontrans_read(), 7U);
    TThread::set_mode(0);
    delete TThread::txn;
    TThread::txn = nullptr;
}

TEST(StoEpochAdvancerLifecycleTest, ConcurrentCallbackReplacementIsSafe) {
    Transaction::start_epoch_advancer();
    auto callback_count = std::make_shared<std::atomic<uint32_t>>(0);
    constexpr size_t thread_count = 8;
    constexpr size_t replacements_per_thread = 128;
    std::atomic<bool> start{false};
    std::vector<std::thread> setters;
    setters.reserve(thread_count);

    for (size_t index = 0; index < thread_count; ++index) {
        setters.emplace_back([&, callback_count] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (size_t replacement = 0;
                 replacement < replacements_per_thread; ++replacement) {
                Transaction::set_epoch_advance_callback(
                    [callback_count](Transaction::epoch_type) {
                        callback_count->fetch_add(1,
                                                  std::memory_order_relaxed);
                    });
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& setter : setters)
        setter.join();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callback_count->load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(callback_count->load(std::memory_order_relaxed), 0U);
    Transaction::set_epoch_advance_callback({});
}

TEST(MakoTimestampTest, AllocationObservationAndExhaustionAreMonotonic) {
    auto& clock = sync_util::sync_logger::local_replica_id;
    const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);
    struct restore_clock {
        std::atomic<uint32_t>& clock;
        uint32_t value;
        ~restore_clock() { clock.store(value, std::memory_order_release); }
    } restore{clock, saved};

    uint32_t timestamp = 0;
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
    EXPECT_EQ(timestamp, 1U);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
    EXPECT_EQ(timestamp, 2U);

    Transaction::observe_mako_timestamp(9);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
    EXPECT_EQ(timestamp, 10U);
    Transaction::observe_mako_timestamp(4);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
    EXPECT_EQ(timestamp, 11U);

    clock.store(5, std::memory_order_relaxed);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp_after(4, timestamp));
    EXPECT_EQ(timestamp, 5U);
    clock.store(5, std::memory_order_relaxed);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp_after(5, timestamp));
    EXPECT_EQ(timestamp, 6U);
    clock.store(5, std::memory_order_relaxed);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp_after(9, timestamp));
    EXPECT_EQ(timestamp, 10U);

    clock.store(1, std::memory_order_relaxed);
    EXPECT_FALSE(Transaction::try_allocate_mako_timestamp_after(
        Transaction::max_mako_timestamp, timestamp));
    EXPECT_EQ(timestamp, 0U);

    clock.store(Transaction::max_mako_timestamp, std::memory_order_release);
    ASSERT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
    EXPECT_EQ(timestamp, Transaction::max_mako_timestamp);
    EXPECT_FALSE(Transaction::try_allocate_mako_timestamp(timestamp));
    EXPECT_EQ(timestamp, 0U);
}

TEST(MakoTimestampTest, ConcurrentAllocationsStayUniqueAboveCompletedFloor) {
    auto& clock = sync_util::sync_logger::local_replica_id;
    const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);
    struct restore_clock {
        std::atomic<uint32_t>& clock;
        uint32_t value;
        ~restore_clock() { clock.store(value, std::memory_order_release); }
    } restore{clock, saved};

    constexpr size_t thread_count = 16;
    constexpr size_t allocations_per_thread = 256;
    constexpr uint32_t floor = 10000;

    std::array<std::array<uint32_t, allocations_per_thread>, thread_count>
        timestamps{};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (size_t thread = 0; thread < thread_count; ++thread) {
        threads.emplace_back([&, thread] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (uint32_t& timestamp : timestamps[thread]) {
                if (!Transaction::try_allocate_mako_timestamp_after(
                        floor, timestamp)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();

    ASSERT_FALSE(failed.load(std::memory_order_relaxed));
    std::vector<uint32_t> ordered;
    ordered.reserve(thread_count * allocations_per_thread);
    for (const auto& per_thread : timestamps)
        ordered.insert(ordered.end(), per_thread.begin(), per_thread.end());
    std::sort(ordered.begin(), ordered.end());
    ASSERT_EQ(ordered.size(), thread_count * allocations_per_thread);
    for (size_t index = 0; index < ordered.size(); ++index)
        EXPECT_EQ(ordered[index], floor + 1 + index);
}

TEST(MakoTimestampTest, ConcurrentObservationAndAllocationPreserveFloor) {
    auto& clock = sync_util::sync_logger::local_replica_id;
    const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);
    struct restore_clock {
        std::atomic<uint32_t>& clock;
        uint32_t value;
        ~restore_clock() { clock.store(value, std::memory_order_release); }
    } restore{clock, saved};

    constexpr size_t allocator_count = 8;
    constexpr size_t allocations_per_thread = 1024;
    constexpr uint32_t first_floor = 1000;
    constexpr uint32_t floor_count = 512;
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::atomic<uint32_t> completed_floor{0};
    std::vector<std::thread> allocators;
    allocators.reserve(allocator_count);

    for (size_t index = 0; index < allocator_count; ++index) {
        allocators.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (size_t allocation = 0;
                 allocation < allocations_per_thread; ++allocation) {
                const uint32_t floor_before =
                    completed_floor.load(std::memory_order_acquire);
                uint32_t timestamp = 0;
                if (!Transaction::try_allocate_mako_timestamp(timestamp)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                const uint32_t floor_after =
                    completed_floor.load(std::memory_order_acquire);
                if (floor_before != 0 && floor_before == floor_after &&
                    timestamp <= floor_before) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    std::thread observer([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (uint32_t offset = 0; offset < floor_count; ++offset) {
            const uint32_t floor = first_floor + offset;
            Transaction::observe_mako_timestamp(floor);
            completed_floor.store(floor, std::memory_order_release);
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    observer.join();
    for (auto& allocator : allocators)
        allocator.join();

    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
    EXPECT_GT(clock.load(std::memory_order_relaxed),
              completed_floor.load(std::memory_order_relaxed));
}

TEST(MakoTimestampTest, CommitIsStrictlyNewerThanReadDependency) {
    auto& clock = sync_util::sync_logger::local_replica_id;
    const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);
    struct restore_clock {
        std::atomic<uint32_t>& clock;
        uint32_t value;
        ~restore_clock() { clock.store(value, std::memory_order_release); }
    } restore{clock, saved};

    constexpr uint32_t dependency_timestamp = 1000;
    LocalTimestampBox box(0);
    uint32_t commit_timestamp = 0;
    bool committed = false;
    std::thread worker([&] {
        TThread::set_id(MAX_THREADS - 2);
        TThread::set_mode(0);
        Sto::start_transaction();
        box.write(1);
        Transaction* const transaction = Sto::transaction();
        transaction->maxTimestampReadSet = dependency_timestamp;
        committed = Sto::try_commit();
        commit_timestamp = transaction->tid_unique_;
        delete transaction;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    worker.join();

    ASSERT_TRUE(committed);
    EXPECT_GT(commit_timestamp, dependency_timestamp);
    EXPECT_EQ(box.nontrans_read(), 1U);
}

TEST(MakoTimestampTest, CommitExhaustionIsTerminalAndCleansUp) {
    auto& clock = sync_util::sync_logger::local_replica_id;
    const uint32_t saved = clock.exchange(
        Transaction::max_mako_timestamp + 1, std::memory_order_acq_rel);
    struct restore_clock {
        std::atomic<uint32_t>& clock;
        uint32_t value;
        ~restore_clock() { clock.store(value, std::memory_order_release); }
    } restore{clock, saved};

    LocalTimestampBox box(0);
    std::thread worker([&] {
        TThread::set_id(MAX_THREADS - 3);
        TThread::set_mode(0);
        Sto::start_transaction();
        box.write(1);
        TThread::txn->maxTimestampReadSet = 1;
        EXPECT_THROW(Sto::try_commit(), Transaction::TimestampExhausted);
        ASSERT_NE(TThread::txn, nullptr);
        EXPECT_FALSE(TThread::txn->has_active_state());
        EXPECT_EQ(box.nontrans_read(), 0U);
        delete TThread::txn;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    worker.join();
}

TEST(MakoTimestampTest, DependencyExhaustionPermanentlySaturatesClock) {
    auto& clock = sync_util::sync_logger::local_replica_id;
    const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);
    struct restore_clock {
        std::atomic<uint32_t>& clock;
        uint32_t value;
        ~restore_clock() { clock.store(value, std::memory_order_release); }
    } restore{clock, saved};

    LocalTimestampBox box(0);
    std::thread worker([&] {
        TThread::set_id(MAX_THREADS - 7);
        TThread::set_mode(0);

        Sto::start_transaction();
        box.write(1);
        TThread::txn->maxTimestampReadSet =
            Transaction::max_mako_timestamp;
        EXPECT_THROW(Sto::try_commit(), Transaction::TimestampExhausted);
        EXPECT_EQ(clock.load(std::memory_order_acquire),
                  Transaction::max_mako_timestamp + 1);
        EXPECT_EQ(box.nontrans_read(), 0U);

        Sto::start_transaction();
        box.write(2);
        TThread::txn->maxTimestampReadSet = 1;
        EXPECT_THROW(Sto::try_commit(), Transaction::TimestampExhausted);
        EXPECT_FALSE(TThread::txn->has_active_state());
        EXPECT_EQ(box.nontrans_read(), 0U);

        delete TThread::txn;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    worker.join();
}

TEST(TransactionSetBoundaryTest, ParticipantAbortCleansExactChunkBoundaries) {
    std::thread worker([] {
        TThread::set_id(MAX_THREADS - 4);
        TThread::set_mode(1);

        for (const size_t count : {size_t{512}, size_t{32768}}) {
            ParticipantInsertObject object;
            size_t end_callback_count = 0;
            Transaction::tinfo[TThread::id()].trans_end_callback =
                [&] { ++end_callback_count; };

            Sto::start_transaction();
            object.stage_fresh_inserts(count);
            Sto::silent_abort();

            ASSERT_NE(TThread::txn, nullptr);
            EXPECT_FALSE(TThread::txn->has_active_state());
            EXPECT_EQ(object.abort_cleanup_count(), count);
            EXPECT_EQ(object.install_count(), 0U);
            EXPECT_EQ(object.unlock_count(), 0U);
            EXPECT_EQ(object.committed_cleanup_count(), 0U);
            EXPECT_EQ(end_callback_count, 1U);
        }

        Transaction::tinfo[TThread::id()].trans_end_callback = {};
        TThread::set_mode(0);
        delete TThread::txn;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    worker.join();
}

TEST(TransactionSetBoundaryTest, CapacityExhaustionFailsClosedInRelease) {
    std::thread worker([] {
        TThread::set_id(MAX_THREADS - 9);
        TThread::set_mode(0);

        ParticipantInsertObject overflowing;
        size_t end_callback_count = 0;
        Transaction::tinfo[TThread::id()].trans_end_callback =
            [&] { ++end_callback_count; };

        Sto::start_transaction();
        EXPECT_THROW(overflowing.stage_fresh_inserts(32769),
                     Transaction::CapacityExhausted);
        ASSERT_NE(TThread::txn, nullptr);
        EXPECT_FALSE(TThread::txn->has_active_state());
        EXPECT_EQ(overflowing.abort_cleanup_count(), 32768U);
        EXPECT_EQ(overflowing.install_count(), 0U);
        EXPECT_EQ(overflowing.unlock_count(), 0U);
        EXPECT_EQ(end_callback_count, 1U);

        ParticipantInsertObject recovery;
        Sto::start_transaction();
        recovery.stage_fresh_inserts(1);
        EXPECT_TRUE(Sto::try_commit());
        EXPECT_EQ(recovery.install_count(), 1U);
        EXPECT_EQ(recovery.committed_cleanup_count(), 1U);
        EXPECT_EQ(end_callback_count, 2U);

        Transaction::tinfo[TThread::id()].trans_end_callback = {};
        delete TThread::txn;
        TThread::txn = nullptr;
        Transaction::rcu_quiesce();
    });
    worker.join();
}

TEST(TransactionCommitFailureTest, InstallExceptionIsFailStop) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT(
        {
            std::set_terminate([] { _Exit(86); });
            std::thread worker([] {
                TThread::set_id(MAX_THREADS - 8);
                TThread::set_mode(0);
                ThrowingInstallObject object;
                Sto::start_transaction();
                object.stage_write();
                try {
                    (void)Sto::try_commit();
                } catch (...) {
                    _Exit(42);
                }
                _Exit(0);
            });
            worker.join();
            _Exit(0);
        },
        ::testing::ExitedWithCode(86), "");
}

TEST(TransItemLifetimeTest, ReusedSlotRetainsExactlyOneOwnedExtraString) {
    bool succeeded = true;
    std::thread worker([&] {
        TThread::set_id(MAX_THREADS - 10);
        TThread::set_mode(0);
        TransItemExtraObject object;

        for (size_t index = 0; index != 1024; ++index) {
            const std::string expected =
                "non-sso-transaction-extra-" + std::to_string(index) +
                std::string(96, static_cast<char>('a' + index % 26));
            Sto::start_transaction();
            object.stage_with_extra(expected);
            if (!Sto::try_commit() || object.installed_extra() != expected) {
                succeeded = false;
                break;
            }
        }

        if (succeeded) {
            Sto::start_transaction();
            object.stage_without_extra();
            succeeded = Sto::try_commit() && object.installed_extra().empty();
        }

        if (TThread::txn != nullptr) {
            if (TThread::txn->has_active_state())
                TThread::txn->silent_abort();
            delete TThread::txn;
            TThread::txn = nullptr;
        }
        Transaction::rcu_quiesce();
    });
    worker.join();

    EXPECT_TRUE(succeeded);
}

// Test 6: Global default runtime for backward compatibility

TEST_F(SiloRuntimeTest, GlobalDefaultRuntime) {
    // Without binding, Current() should return the global default
    tl_silo_runtime = nullptr;  // Clear any previous binding

    SiloRuntime* global = SiloRuntime::GlobalDefault();
    ASSERT_NE(global, nullptr);

    // Same global should be returned on subsequent calls
    EXPECT_EQ(SiloRuntime::GlobalDefault(), global);

    // Current() should also return global default when nothing bound
    EXPECT_EQ(SiloRuntime::Current(), global);
}

// Test 7: BindToCurrentThread convenience method

TEST_F(SiloRuntimeTest, BindToCurrentThreadConvenience) {
    // Use convenience method
    site1()->BindToCurrentThread();

    // Both SiloRuntime and MasstreeContext should be bound
    EXPECT_EQ(SiloRuntime::Current(), site1());
    EXPECT_EQ(MasstreeContext::Current(), site1()->masstree_context());

    // Switch to site2
    site2()->BindToCurrentThread();

    EXPECT_EQ(SiloRuntime::Current(), site2());
    EXPECT_EQ(MasstreeContext::Current(), site2()->masstree_context());
}

// Test 8: Per-runtime core ID allocation

TEST_F(SiloRuntimeTest, PerRuntimeCoreIdAllocation) {
    // Each runtime should have its own core ID counter starting at 0
    EXPECT_EQ(site1()->core_count(), 0u);
    EXPECT_EQ(site2()->core_count(), 0u);

    // Allocate core IDs from site1
    site1()->BindToCurrentThread();
    unsigned core1a = coreid::core_id();
    EXPECT_EQ(core1a, 0u);  // First core ID for site1
    EXPECT_EQ(site1()->core_count(), 1u);

    // Allocating again from same thread should return cached value
    unsigned core1b = coreid::core_id();
    EXPECT_EQ(core1b, core1a);
    EXPECT_EQ(site1()->core_count(), 1u);  // No new allocation

    // Switch to site2 - should get a NEW core ID from site2's space
    site2()->BindToCurrentThread();
    coreid::reset_core_id();  // Force re-allocation
    unsigned core2a = coreid::core_id();
    EXPECT_EQ(core2a, 0u);  // First core ID for site2 (starts at 0)
    EXPECT_EQ(site2()->core_count(), 1u);

    // site1 should still have count 1, site2 now has count 1
    EXPECT_EQ(site1()->core_count(), 1u);
    EXPECT_EQ(site2()->core_count(), 1u);
}

// Test 8: Core ID isolation across threads

TEST_F(SiloRuntimeTest, CoreIdIsolationAcrossThreads) {
    const int NUM_THREADS = 4;
    std::atomic<int> site1_count{0};
    std::atomic<int> site2_count{0};

    SiloRuntime* site1_ptr = site1();
    SiloRuntime* site2_ptr = site2();

    std::vector<std::thread> threads;

    // Spawn threads for site1
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([site1_ptr, &site1_count]() {
            site1_ptr->BindToCurrentThread();
            unsigned cid = coreid::core_id();
            EXPECT_LT(cid, NMAXCORES);
            site1_count++;
        });
    }

    // Spawn threads for site2
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([site2_ptr, &site2_count]() {
            site2_ptr->BindToCurrentThread();
            unsigned cid = coreid::core_id();
            EXPECT_LT(cid, NMAXCORES);
            site2_count++;
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(site1_count.load(), NUM_THREADS);
    EXPECT_EQ(site2_count.load(), NUM_THREADS);

    // Each runtime should have allocated NUM_THREADS core IDs
    EXPECT_EQ(site1()->core_count(), NUM_THREADS);
    EXPECT_EQ(site2()->core_count(), NUM_THREADS);
}

// Test (Finding 6): explicit thread registration fails gracefully
// when the per-runtime core-id pool is exhausted, instead of
// aborting the process. This exercises the Mako-side fix; the
// underlying cap lives in SiloRuntime, not in Masstree.
//
// Uses a FRESH SiloRuntime so the test does not poison the
// process-wide default runtime's pool.
TEST(SiloRuntimeThreadCap, TryRegisterCurrentThreadFailsGracefullyAtCap) {
    rusty::Arc<SiloRuntime> rt_arc = SiloRuntime::Create();
    SiloRuntime* rt = rt_arc.as_ptr();
    ASSERT_NE(rt, nullptr);
    EXPECT_EQ(rt->core_count(), 0u);

    constexpr unsigned kOverhead = 8;  // a few extra threads past the cap
    const unsigned cap = SiloRuntime::NMaxCores;
    const unsigned kThreads = cap + kOverhead;

    std::atomic<unsigned> registered{0};
    std::atomic<unsigned> rejected{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (unsigned i = 0; i < kThreads; ++i) {
        workers.emplace_back([rt, &registered, &rejected]() {
            if (rt->try_register_current_thread()) {
                ++registered;
            } else {
                ++rejected;
            }
        });
    }
    for (auto& t : workers) t.join();

    EXPECT_EQ(registered.load(), cap);
    EXPECT_EQ(rejected.load(),   kOverhead);
}

// Test (Finding 6, idempotency): try_register_current_thread is a
// no-op (returns true without consuming a slot) for a thread that
// already registered to the same runtime.
TEST(SiloRuntimeThreadCap, TryRegisterCurrentThreadIsIdempotent) {
    rusty::Arc<SiloRuntime> rt_arc = SiloRuntime::Create();
    SiloRuntime* rt = rt_arc.as_ptr();
    ASSERT_NE(rt, nullptr);

    std::thread t([rt]() {
        EXPECT_TRUE(rt->try_register_current_thread());
        const unsigned core_count_after_first = rt->core_count();
        EXPECT_EQ(core_count_after_first, 1u);

        // Subsequent calls must not consume another slot.
        for (int i = 0; i < 10; ++i) {
            EXPECT_TRUE(rt->try_register_current_thread());
        }
        EXPECT_EQ(rt->core_count(), core_count_after_first);
    });
    t.join();
}
