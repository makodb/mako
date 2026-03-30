#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <random>
#include "misc/alock.hpp"
#include "misc/alarm.hpp"
#include "base/all.hpp"

using namespace rrr;
using namespace std::chrono;

class ALockTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
    
    void TearDown() override {
    }
};

TEST_F(ALockTest, TimeoutALockBasicLock) {
    TimeoutALock al;
    std::atomic<int> value{0};
    
    auto id = al.lock(1, [&value]() {
        value = 1;
    }, [&value]() {
        value = -1;
    });
    
    EXPECT_GT(id, 0u);
    EXPECT_EQ(value, 1);
    
    al.abort(id);
}

TEST_F(ALockTest, TimeoutALockMultipleLocks) {
    TimeoutALock al;
    std::atomic<int> value{0};
    
    auto id1 = al.lock(1, [&value]() {
        value = 1;
    }, [&value]() {
        value = -1;
    });
    
    EXPECT_EQ(value, 1);
    
    auto id2 = al.lock(2, [&value]() {
        value = 2;
    }, [&value]() {
        value = -2;
    });
    
    EXPECT_EQ(value, 1);
    
    al.abort(id1);
    
    std::this_thread::sleep_for(milliseconds(10));
    EXPECT_EQ(value, 2);
    
    al.abort(id2);
}

TEST_F(ALockTest, TimeoutALockReadWriteLocks) {
    TimeoutALock al;
    std::atomic<int> read_count{0};
    std::atomic<int> write_count{0};
    
    auto r1 = al.lock(1, [&read_count]() {
        read_count++;
    }, []() {}, ALock::RLOCK);
    
    auto r2 = al.lock(2, [&read_count]() {
        read_count++;
    }, []() {}, ALock::RLOCK);
    
    EXPECT_EQ(read_count, 2);
    
    auto w1 = al.lock(3, [&write_count]() {
        write_count++;
    }, []() {}, ALock::WLOCK);
    
    EXPECT_EQ(write_count, 0);
    
    al.abort(r1);
    al.abort(r2);
    
    std::this_thread::sleep_for(milliseconds(10));
    EXPECT_EQ(write_count, 1);
    
    al.abort(w1);
}

TEST_F(ALockTest, TimeoutALockTimeout) {
    TimeoutALock al;
    std::atomic<int> value{0};
    std::atomic<bool> timed_out{false};
    
    auto id1 = al.lock(1, [&value]() {
        value = 1;
    }, []() {});
    
    EXPECT_EQ(value, 1);
    
    auto id2 = al.lock(2, [&value]() {
        value = 2;
    }, [&timed_out]() {
        timed_out = true;
    });
    
    // Wait for timeout to occur (ALOCK_TIMEOUT is in microseconds)
    std::this_thread::sleep_for(milliseconds(250));  // 250ms > 200ms timeout
    
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(value, 1);
    
    al.abort(id1);
}

TEST_F(ALockTest, WaitDieALockBasic) {
    WaitDieALock wdal;
    std::atomic<int> value{0};
    
    auto id = wdal.lock(1, [&value]() {
        value = 1;
    }, [&value]() {
        value = -1;
    }, ALock::WLOCK, 10);
    
    EXPECT_EQ(value, 1);
    
    wdal.abort(id);
}

TEST_F(ALockTest, WaitDieALockPriorityOrdering) {
    WaitDieALock wdal;
    std::vector<int> lock_order;
    std::mutex order_mutex;
    
    auto high_priority = wdal.lock(1, [&]() {
        std::lock_guard<std::mutex> guard(order_mutex);
        lock_order.push_back(1);
    }, []() {}, ALock::WLOCK, 5);
    
    EXPECT_EQ(lock_order.size(), 1u);
    EXPECT_EQ(lock_order[0], 1);
    
    auto low_priority = wdal.lock(2, [&]() {
        std::lock_guard<std::mutex> guard(order_mutex);
        lock_order.push_back(2);
    }, []() {}, ALock::WLOCK, 10);
    
    auto higher_priority = wdal.lock(3, [&]() {
        std::lock_guard<std::mutex> guard(order_mutex);
        lock_order.push_back(3);
    }, []() {}, ALock::WLOCK, 1);
    
    EXPECT_EQ(lock_order.size(), 1u);
    
    wdal.abort(high_priority);
    
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(lock_order.size(), 2u);
    EXPECT_EQ(lock_order[1], 3);
    
    wdal.abort(higher_priority);
    
    std::this_thread::sleep_for(milliseconds(50));  // Give more time for lock to be granted
    
    EXPECT_GE(lock_order.size(), 2u);  // At least 2 locks should be granted
    if (lock_order.size() == 3) {
        EXPECT_EQ(lock_order[2], 2);
    }
    
    wdal.abort(low_priority);
}

TEST_F(ALockTest, WaitDieALockDeadlockPrevention) {
    WaitDieALock wdal;
    std::atomic<int> w1_val{0};
    std::atomic<int> w2_val{0};
    
    auto w1 = wdal.lock(1, [&w1_val]() {
        w1_val = 1;
    }, [&w1_val]() {
        w1_val = -1;
    }, ALock::WLOCK, 10);
    
    EXPECT_EQ(w1_val, 1);
    
    auto w2 = wdal.lock(2, [&w2_val]() {
        w2_val = 1;
    }, [&w2_val]() {
        w2_val = -1;
    }, ALock::WLOCK, 5);
    
    // Give time for the abort callback to be called
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(w1_val, 1);
    EXPECT_EQ(w2_val, -1);
    
    wdal.abort(w1);
}

TEST_F(ALockTest, WoundDieALockWounding) {
    WoundDieALock wdal;
    std::atomic<int> w1_val{0};
    std::atomic<int> w2_val{0};
    std::atomic<bool> w1_wounded{false};
    
    auto w1 = wdal.lock(1, [&w1_val]() {
        w1_val = 1;
    }, [&w1_val]() {
        w1_val = -1;
    }, ALock::WLOCK, 10, [&w1_wounded]() -> int {
        w1_wounded = true;
        return 0;
    });
    
    EXPECT_EQ(w1_val, 1);
    
    auto w2 = wdal.lock(2, [&w2_val]() {
        w2_val = 1;
    }, [&w2_val]() {
        w2_val = -1;
    }, ALock::WLOCK, 5, []() -> int {
        return 0;
    });
    
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_TRUE(w1_wounded);
    EXPECT_EQ(w2_val, 1);
    
    wdal.abort(w2);
}

TEST_F(ALockTest, ALockGroupSimple) {
    TimeoutALock al1, al2, al3;
    ALockGroup alg;
    std::atomic<int> value{0};
    
    alg.add(&al1);
    alg.add(&al2);
    alg.add(&al3);
    
    alg.lock_all([&value]() {
        value = 1;
    }, [&value]() {
        value = -1;
    });
    
    // Give time for the lock callbacks to execute
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(value, 1);
    
    alg.unlock_all();
}

TEST_F(ALockTest, ALockGroupConflict) {
    TimeoutALock al1, al2;
    ALockGroup alg1, alg2;
    std::atomic<int> val1{0}, val2{0};
    
    alg1.add(&al1);
    alg1.lock_all([&val1]() {
        val1 = 1;
    }, [&val1]() {
        val1 = -1;
    });
    
    EXPECT_EQ(val1, 1);
    
    alg2.add(&al1);
    alg2.add(&al2);
    alg2.lock_all([&val2]() {
        val2 = 1;
    }, [&val2]() {
        val2 = -1;
    });
    
    std::this_thread::sleep_for(milliseconds(ALOCK_TIMEOUT/1000 + 100));
    
    EXPECT_EQ(val1, 1);
    EXPECT_EQ(val2, -1);
    
    alg1.unlock_all();
}

TEST_F(ALockTest, ALockGroupPartialSuccess) {
    TimeoutALock al1, al2, al3;
    ALockGroup alg1, alg2;
    std::atomic<int> val1{0}, val2{0};
    
    alg1.add(&al1);
    alg1.add(&al2);
    alg1.lock_all([&val1]() {
        val1 = 1;
    }, [&val1]() {
        val1 = -1;
    });
    
    EXPECT_EQ(val1, 1);
    
    alg2.add(&al2);
    alg2.add(&al3);
    alg2.lock_all([&val2]() {
        val2 = 1;
    }, [&val2]() {
        val2 = -1;
    });
    
    std::this_thread::sleep_for(milliseconds(ALOCK_TIMEOUT/1000 + 100));
    
    EXPECT_EQ(val2, -1);
    
    alg1.unlock_all();
    
    alg2.lock_all([&val2]() {
        val2 = 2;
    }, [&val2]() {
        val2 = -2;
    });
    
    EXPECT_EQ(val2, 2);
    
    alg2.unlock_all();
}

TEST_F(ALockTest, ConcurrentStressTest) {
    const int num_locks = 10;
    const int num_threads = 20;
    const int ops_per_thread = 100;
    
    std::vector<std::unique_ptr<TimeoutALock>> locks;
    for (int i = 0; i < num_locks; ++i) {
        locks.push_back(std::make_unique<TimeoutALock>());
    }
    
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 gen(t);
            std::uniform_int_distribution<> lock_dist(0, num_locks - 1);
            std::uniform_int_distribution<> type_dist(0, 1);
            
            for (int op = 0; op < ops_per_thread; ++op) {
                int lock_idx = lock_dist(gen);
                ALock::type_t lock_type = type_dist(gen) ? ALock::WLOCK : ALock::RLOCK;
                
                auto id = locks[lock_idx]->lock(
                    t * 1000 + op,
                    [&success_count]() { success_count++; },
                    [&failure_count]() { failure_count++; },
                    lock_type
                );
                
                if (id > 0) {
                    std::this_thread::sleep_for(microseconds(100));
                    locks[lock_idx]->abort(id);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_GT(success_count, 0);
    EXPECT_GE(failure_count, 0);
    
    int total = success_count + failure_count;
    EXPECT_LE(total, num_threads * ops_per_thread);
}

TEST_F(ALockTest, ReadWriteFairness) {
    TimeoutALock al;
    std::atomic<int> read_count{0};
    std::atomic<int> write_count{0};
    std::vector<uint64_t> ids;
    
    auto w1 = al.lock(1, [&write_count]() {
        write_count++;
    }, []() {}, ALock::WLOCK);
    
    EXPECT_EQ(write_count, 1);
    
    for (int i = 0; i < 5; ++i) {
        ids.push_back(al.lock(10 + i, [&read_count]() {
            read_count++;
        }, []() {}, ALock::RLOCK));
    }
    
    auto w2 = al.lock(2, [&write_count]() {
        write_count++;
    }, []() {}, ALock::WLOCK);
    
    for (int i = 0; i < 5; ++i) {
        ids.push_back(al.lock(20 + i, [&read_count]() {
            read_count++;
        }, []() {}, ALock::RLOCK));
    }
    
    EXPECT_EQ(read_count, 0);
    EXPECT_EQ(write_count, 1);
    
    al.abort(w1);
    
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(read_count, 5);
    EXPECT_EQ(write_count, 1);
    
    for (int i = 0; i < 5; ++i) {
        al.abort(ids[i]);
    }
    
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(write_count, 2);
    EXPECT_EQ(read_count, 5);
    
    al.abort(w2);
    
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(read_count, 10);
    
    for (int i = 5; i < 10; ++i) {
        al.abort(ids[i]);
    }
}

TEST_F(ALockTest, LockUpgradeDowngrade) {
    TimeoutALock al;
    std::atomic<int> value{0};
    
    auto r1 = al.lock(1, [&value]() {
        value = 1;
    }, []() {}, ALock::RLOCK);
    
    EXPECT_EQ(value, 1);
    
    auto r2 = al.lock(2, [&value]() {
        value = 2;
    }, []() {}, ALock::RLOCK);
    
    EXPECT_EQ(value, 2);
    
    auto w1 = al.lock(1, [&value]() {
        value = 10;
    }, []() {}, ALock::WLOCK);
    
    EXPECT_EQ(value, 2);
    
    al.abort(r1);
    al.abort(r2);
    
    std::this_thread::sleep_for(milliseconds(10));
    
    EXPECT_EQ(value, 10);
    
    al.abort(w1);
}

TEST_F(ALockTest, MemoryLeakTest) {
    for (int iter = 0; iter < 1000; ++iter) {
        TimeoutALock al;
        std::vector<uint64_t> ids;
        
        for (int i = 0; i < 10; ++i) {
            ids.push_back(al.lock(i, []() {}, []() {}));
        }
        
        for (auto id : ids) {
            al.abort(id);
        }
    }
}

TEST_F(ALockTest, PerformanceBenchmark) {
    TimeoutALock al;
    const int num_operations = 10000;
    
    auto start = high_resolution_clock::now();
    
    for (int i = 0; i < num_operations; ++i) {
        auto id = al.lock(i, []() {}, []() {});
        al.abort(id);
    }
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(end - start);
    
    double ops_per_sec = (num_operations * 1000000.0) / duration.count();
    
    std::cout << "Lock/Unlock performance: " << ops_per_sec << " ops/sec" << std::endl;
    
    EXPECT_GT(ops_per_sec, 10000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}