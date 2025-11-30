// Test to investigate Arc<Mutex<>> behavior across threads
// This replicates the pattern that caused hangs in PollThread::create()

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include <rusty/thread.hpp>
#include <rusty/sync/mpsc.hpp>
#include <thread>
#include <chrono>
#include <atomic>

// Mark std::thread::id as Send+Sync (it's a simple value type)
namespace rusty {
template<> struct is_send<std::thread::id> : std::true_type {};
template<> struct is_sync<std::thread::id> : std::true_type {};
// Explicitly mark Mutex<std::thread::id> as Send+Sync
template<> struct is_send<rusty::Mutex<std::thread::id>> : std::true_type {};
template<> struct is_sync<rusty::Mutex<std::thread::id>> : std::true_type {};
}

// Test 1: Basic Arc<Mutex<T>> access from spawned thread
TEST(ArcMutexThread, BasicCrossThreadAccess) {
    auto mutex = rusty::Arc<rusty::Mutex<int>>::make(0);

    // Clone for the spawned thread
    auto mutex_clone = mutex.clone();

    auto handle = rusty::thread::spawn([](rusty::Arc<rusty::Mutex<int>> m) {
        auto guard = m->lock();
        *guard = 42;
    }, std::move(mutex_clone));

    handle.join();

    auto guard = mutex->lock();
    EXPECT_EQ(*guard, 42);
}

// Test 2: Arc<Mutex<std::thread::id>> - the exact type that caused issues
TEST(ArcMutexThread, ThreadIdType) {
    auto thread_id_mutex = rusty::Arc<rusty::Mutex<std::thread::id>>::make(std::thread::id{});

    auto mutex_clone = thread_id_mutex.clone();

    auto handle = rusty::thread::spawn([](rusty::Arc<rusty::Mutex<std::thread::id>> m) {
        auto guard = m->lock();
        *guard = std::this_thread::get_id();
    }, std::move(mutex_clone));

    handle.join();

    auto guard = thread_id_mutex->lock();
    EXPECT_NE(*guard, std::thread::id{});  // Should be set to spawned thread's ID
}

// Test 3: Pattern closer to PollThread::create() - Arc to outer struct containing mutex
struct TestStruct {
    rusty::Mutex<std::thread::id> thread_id{std::thread::id{}};
    mutable std::atomic<bool> done{false};  // mutable for const access via Arc
};

// Mark TestStruct as Send+Sync (it's thread-safe due to Mutex)
namespace rusty {
template<> struct is_send<TestStruct> : std::true_type {};
template<> struct is_sync<TestStruct> : std::true_type {};
}

TEST(ArcMutexThread, ArcToStructWithMutex) {
    auto test_struct = rusty::Arc<TestStruct>::make();

    auto struct_clone = test_struct.clone();

    auto handle = rusty::thread::spawn([](rusty::Arc<TestStruct> s) {
        auto guard = s->thread_id.lock();
        *guard = std::this_thread::get_id();
        s->done.store(true, std::memory_order_release);
    }, std::move(struct_clone));

    handle.join();

    EXPECT_TRUE(test_struct->done.load(std::memory_order_acquire));
    auto guard = test_struct->thread_id.lock();
    EXPECT_NE(*guard, std::thread::id{});
}

// Test 4: The EXACT pattern from PollThread::create() that caused hangs
// - Create Arc
// - Clone Arc for capture
// - Spawn thread with mpsc::Receiver
// - Thread sets thread_id via captured Arc<Mutex<>>
// - Parent stores JoinHandle after spawn
struct PollThreadLike {
    rusty::Mutex<std::thread::id> poll_thread_id{std::thread::id{}};
    rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<void>>> join_handle{rusty::None};
    mutable std::atomic<bool> worker_started{false};  // mutable for const access via Arc
};

// Mark PollThreadLike as Send+Sync
namespace rusty {
template<> struct is_send<PollThreadLike> : std::true_type {};
template<> struct is_sync<PollThreadLike> : std::true_type {};
}

TEST(ArcMutexThread, ExactPollThreadPattern) {
    auto [sender, receiver] = rusty::sync::mpsc::channel<int>();
    auto arc = rusty::Arc<PollThreadLike>::make();

    // Clone Arc for the thread to capture
    auto arc_for_thread = arc.clone();

    auto handle = rusty::thread::spawn(
        [](rusty::Arc<PollThreadLike> a, rusty::sync::mpsc::Receiver<int> rx) {
            // This is what caused the hang - locking mutex from spawned thread
            {
                auto guard = a->poll_thread_id.lock();
                *guard = std::this_thread::get_id();
            }
            a->worker_started.store(true, std::memory_order_release);

            // Simulate worker loop - wait for shutdown signal
            while (true) {
                auto result = rx.recv();
                if (result.is_err()) break;  // Channel closed
                if (result.unwrap() == -1) break;  // Shutdown signal
            }
        },
        std::move(arc_for_thread),
        std::move(receiver)
    );

    // Store join handle (like PollThread::create() does)
    {
        auto guard = arc->join_handle.lock();
        *guard = rusty::Some(std::move(handle));
    }

    // Wait for worker to start
    while (!arc->worker_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Verify thread_id was set
    {
        auto guard = arc->poll_thread_id.lock();
        EXPECT_NE(*guard, std::thread::id{});
    }

    // Send shutdown signal
    sender.send(-1);

    // Join the thread
    {
        auto guard = arc->join_handle.lock();
        if (guard->is_some()) {
            guard->take().unwrap().join();
        }
    }
}

// Test 5: Multiple rapid lock/unlock cycles across threads
TEST(ArcMutexThread, RapidLockUnlock) {
    auto counter = rusty::Arc<rusty::Mutex<int>>::make(0);

    std::vector<rusty::thread::JoinHandle<void>> handles;

    for (int i = 0; i < 10; i++) {
        auto counter_clone = counter.clone();
        handles.push_back(rusty::thread::spawn(
            [](rusty::Arc<rusty::Mutex<int>> c) {
                for (int j = 0; j < 100; j++) {
                    auto guard = c->lock();
                    (*guard)++;
                }
            },
            std::move(counter_clone)
        ));
    }

    for (auto& h : handles) {
        h.join();
    }

    auto guard = counter->lock();
    EXPECT_EQ(*guard, 1000);
}

// Test 6: Using std::thread directly (bypasses rusty::thread::spawn)
// This tests if the issue is in rusty::thread::spawn or in Arc<Mutex<>>
TEST(ArcMutexThread, StdThreadWithArcMutex) {
    auto thread_id_mutex = rusty::Arc<rusty::Mutex<std::thread::id>>::make(std::thread::id{});

    auto mutex_clone = thread_id_mutex.clone();

    std::thread t([m = std::move(mutex_clone)]() {
        auto guard = m->lock();
        *guard = std::this_thread::get_id();
    });

    t.join();

    auto guard = thread_id_mutex->lock();
    EXPECT_NE(*guard, std::thread::id{});
}

// Test 7: std::thread with the full PollThread pattern
TEST(ArcMutexThread, StdThreadPollThreadPattern) {
    auto [sender, receiver] = rusty::sync::mpsc::channel<int>();
    auto arc = rusty::Arc<PollThreadLike>::make();

    auto arc_for_thread = arc.clone();
    auto rx = std::move(receiver);

    std::thread t([a = std::move(arc_for_thread), rx = std::move(rx)]() mutable {
        {
            auto guard = a->poll_thread_id.lock();
            *guard = std::this_thread::get_id();
        }
        a->worker_started.store(true, std::memory_order_release);

        while (true) {
            auto result = rx.recv();
            if (result.is_err()) break;
            if (result.unwrap() == -1) break;
        }
    });

    // Wait for worker to start
    while (!arc->worker_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Verify thread_id was set
    {
        auto guard = arc->poll_thread_id.lock();
        EXPECT_NE(*guard, std::thread::id{});
    }

    sender.send(-1);
    t.join();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
