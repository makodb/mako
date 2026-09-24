// @unsafe - Test file: raw threads, signals and deliberate teardown races.
// @unsafe {
/**
 * @file reactor_stackless_battery.cc
 * @brief Native promotion battery for the stackless waker (plan section 2.2,
 *        items 1-9).  Gates G6 (runtime), G7 (ASan) and G8 (TSan) run this.
 *
 * These tests exist because the Cargo lane CANNOT prove any of this.  Cargo
 * sees the carrier's `#[cfg_attr(any(), thread_local)]` markers as ordinary
 * process-global statics, so the per-thread reactor/registry semantics that
 * the whole waker design rests on simply are not present under rustc.  The
 * Cargo tests pin source shape; correctness is decided here, on the generated
 * C++, or it is not decided at all.
 *
 * Design under test (plan section 2.1, shape (ii) -- lifetime-tracked
 * thread-safe wake ingress):
 *   - a foreign thread may hold a copied `rusty::Waker` and fire it at any
 *     time, including during teardown;
 *   - the wake crosses threads through the ingress only (2 atomics + 1 mutex);
 *     the owner-only queues are never touched off-owner;
 *   - completion always runs on the owning thread;
 *   - teardown owes every stranded waiter an error, never silence.
 *
 * EVERY test body opens with SRPC_TEST_WATCHDOG.  See reactor_watchdog.h for
 * why that is not optional here.
 *
 * NOTE ON RUNNABILITY: authored ahead of the compiling provider, per the plan
 * ("author now, run later").  This file cannot build until compiler tuple V11
 * clears the H2/H3/H4/H5 clusters and C6 (the boxed-callable coercion) -- C6
 * in particular is what makes `*ctx->waker` a copyable `std::function<void()>`,
 * which items 1-9 all depend on.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <rusty/async.hpp>
#include <rusty/option.hpp>

#include "../srpc.hpp"
#include "reactor_watchdog.h"

import std;

using namespace srpc;

namespace {

long current_tid() { return static_cast<long>(::syscall(SYS_gettid)); }

// ---------------------------------------------------------------------------
// ManualGate: the test-side future the reactor polls.
// ---------------------------------------------------------------------------
//
// On first poll it copies the Context's Waker so a foreign thread can fire it.
// Copying is the point: `rusty::Waker` is a copyable Send+Sync std::function
// struct (async.hpp), and the whole cross-thread design depends on a copy
// outliving the poll that produced it.
struct ManualGate {
    std::mutex mu;
    rusty::Waker waker;
    bool have_waker = false;
    std::atomic<bool> ready{false};
    std::atomic<long> completed_on_tid{0};
    std::atomic<int> poll_count{0};

    // Returns a copy of the waker, or nothing if the task has not been polled
    // yet.  Foreign threads call this.
    bool try_copy_waker(rusty::Waker* out) {
        std::lock_guard<std::mutex> guard(mu);
        if (!have_waker) {
            return false;
        }
        *out = waker;
        return true;
    }

    void wait_for_waker() {
        while (true) {
            {
                std::lock_guard<std::mutex> guard(mu);
                if (have_waker) {
                    return;
                }
            }
            std::this_thread::yield();
        }
    }

    void open_and_wake() {
        ready.store(true, std::memory_order_release);
        rusty::Waker copy;
        if (try_copy_waker(&copy)) {
            copy.wake();
        }
    }
};

struct GateAwaiter {
    ManualGate* gate;

    bool await_ready() const noexcept {
        return gate->ready.load(std::memory_order_acquire);
    }

    void await_suspend(std::coroutine_handle<>) const noexcept {
        rusty::Context* ctx = rusty::current_context();
        if (ctx != nullptr && ctx->waker != nullptr) {
            std::lock_guard<std::mutex> guard(gate->mu);
            // Copy, do not alias: the Context/Waker binding belongs to the
            // reactor and may be retired the moment this task completes.
            gate->waker = *ctx->waker;
            gate->have_waker = true;
        }
    }

    void await_resume() const noexcept {}
};

// The loop is required, not decorative: a spurious or coalesced wake resumes
// the coroutine without `ready` being set, and a task that treated that as
// completion would make the coalescing test (item 3) vacuous.
rusty::Task<int> gate_task(ManualGate* gate, int value) {
    while (!gate->ready.load(std::memory_order_acquire)) {
        gate->poll_count.fetch_add(1, std::memory_order_relaxed);
        co_await GateAwaiter{gate};
    }
    co_return value;
}

// The void lane has no on_ready callback, so it reports completion from inside
// the coroutine.  It is worth its own coverage because
// reactor_spawn_stackless_task_impl(const Reactor&, Task<void>) is one of the
// 300 owned strong symbols -- its signature is frozen, unlike the generic
// with_result spawn.
rusty::Task<void> gate_task_void(ManualGate* gate, std::atomic<long>* done_tid) {
    while (!gate->ready.load(std::memory_order_acquire)) {
        co_await GateAwaiter{gate};
    }
    done_tid->store(current_tid(), std::memory_order_release);
    co_return;
}

// Drives the owner reactor until `done` or `budget` expires.  Mirrors the
// pollworker idle cycle: drain, then wait a bounded moment.  Returns false on
// budget exhaustion so a caller can turn a hang into an assertion instead of
// leaving it to the watchdog.
// The reactor handle is a template parameter so this file never has to name
// rusty::Rc, which has no header form here -- the same dodge fiber_test.cc uses.
template <typename ReactorHandle, typename Pred>
bool drive_until(const ReactorHandle& reactor, Pred done,
                 std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        (*reactor).process_stackless_tasks();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (*reactor).process_stackless_tasks();
    return done();
}

class StacklessBatteryTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Drop the thread-local reactor between tests so each one gets a fresh
        // owner thread_id_, registry entry and slot space.
        *srpc::sp_running_fiber_th_.borrow_mut() = rusty::None;
        srpc::sp_reactor_th_ = rusty::None;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. Foreign-thread wake completes on the ORIGINAL OWNER TID.
// ---------------------------------------------------------------------------
// This is the test the checkpoint requires and the one shape (i) (owner-only
// wakers) cannot satisfy even in principle -- under (i) there is no defined
// cross-thread wake to observe.  It is the direct proof of shape (ii).
TEST_F(StacklessBatteryTest, stackless_foreign_wake_completes_on_owner_tid) {
    SRPC_TEST_WATCHDOG("stackless_foreign_wake_completes_on_owner_tid");

    const long owner_tid = current_tid();
    auto reactor = Reactor::get_reactor();

    ManualGate gate;
    std::atomic<int> result{0};

    reactor_spawn_stackless_task_with_result<int>(
        *reactor, gate_task(&gate, 4242),
        [&gate, &result](int v) {
            gate.completed_on_tid.store(current_tid(), std::memory_order_release);
            result.store(v, std::memory_order_release);
        });

    // The task must actually be pending, or the test proves nothing.
    ASSERT_FALSE(gate.ready.load(std::memory_order_acquire));
    gate.wait_for_waker();

    std::thread foreign([&gate]() { gate.open_and_wake(); });

    const bool completed = drive_until(reactor, [&result]() {
        return result.load(std::memory_order_acquire) != 0;
    });
    foreign.join();

    ASSERT_TRUE(completed) << "foreign wake never completed the task";
    EXPECT_EQ(4242, result.load(std::memory_order_acquire));
    EXPECT_EQ(owner_tid, gate.completed_on_tid.load(std::memory_order_acquire))
        << "completion ran off the owning thread; the owner-only queues are "
           "not owner-only";
}

// ---------------------------------------------------------------------------
// 2. Wake racing the INITIAL poll.
// ---------------------------------------------------------------------------
// Pins the early-ticket store plus the take_pending backstop: the waker handed
// out during the first poll(ectx) exists before register_stackless_poller has
// published a slot, so a wake landing in that window must not be lost.
TEST_F(StacklessBatteryTest, stackless_wake_during_initial_poll) {
    SRPC_TEST_WATCHDOG("stackless_wake_during_initial_poll");

    auto reactor = Reactor::get_reactor();

    for (int iteration = 0; iteration < 10000; ++iteration) {
        ManualGate gate;
        std::atomic<bool> done{false};

        // Racer fires as soon as a waker appears -- which is during the very
        // first poll, inside the spawn call itself.
        std::thread racer([&gate]() {
            gate.wait_for_waker();
            gate.open_and_wake();
        });

        reactor_spawn_stackless_task_with_result<int>(
            *reactor, gate_task(&gate, 1),
            [&done](int) { done.store(true, std::memory_order_release); });

        const bool completed = drive_until(reactor, [&done]() {
            return done.load(std::memory_order_acquire);
        }, std::chrono::milliseconds(2000));
        racer.join();

        ASSERT_TRUE(completed) << "lost wake at iteration " << iteration
                               << " -- a wake during the initial poll was "
                                  "dropped instead of replayed by take_pending";
    }
}

// ---------------------------------------------------------------------------
// 3. Duplicate + concurrent wake coalescing.
// ---------------------------------------------------------------------------
// 8 threads x 100k wakes on one ticket.  Exactly-once completion, at most one
// pending entry per ticket, bounded memory.  This is what the ticket's atomic
// `enqueued` bit is for; without it the pending queue grows without bound.
TEST_F(StacklessBatteryTest, stackless_duplicate_and_concurrent_wake_coalescing) {
    SRPC_TEST_WATCHDOG("stackless_duplicate_and_concurrent_wake_coalescing");

    auto reactor = Reactor::get_reactor();

    ManualGate gate;
    std::atomic<int> completions{0};

    reactor_spawn_stackless_task_with_result<int>(
        *reactor, gate_task(&gate, 7),
        [&completions](int) { completions.fetch_add(1, std::memory_order_acq_rel); });

    gate.wait_for_waker();

    constexpr int kThreads = 8;
    constexpr int kWakesPerThread = 100000;
    std::atomic<bool> go{false};
    std::vector<std::thread> wakers;
    wakers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        wakers.emplace_back([&gate, &go]() {
            rusty::Waker copy;
            ASSERT_TRUE(gate.try_copy_waker(&copy));
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kWakesPerThread; ++i) {
                copy.wake();
            }
        });
    }
    go.store(true, std::memory_order_release);

    // Keep draining while the storm runs; the drain is what proves the queue
    // stays bounded rather than accumulating 800k entries.
    for (int i = 0; i < 200; ++i) {
        (*reactor).process_stackless_tasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& w : wakers) {
        w.join();
    }

    gate.open_and_wake();
    const bool completed = drive_until(reactor, [&completions]() {
        return completions.load(std::memory_order_acquire) > 0;
    });

    ASSERT_TRUE(completed);
    EXPECT_EQ(1, completions.load(std::memory_order_acquire))
        << "completion fired more than once under concurrent wakes";
}

// ---------------------------------------------------------------------------
// 4. Completion racing a FORCED slot reuse.
// ---------------------------------------------------------------------------
// An old waker, retained past its task's completion, is fired after the slot
// has been handed to a new task.  The stale ticket must read as a tombstone at
// drain and must not complete the new occupant.
TEST_F(StacklessBatteryTest, stackless_completion_races_forced_slot_reuse) {
    SRPC_TEST_WATCHDOG("stackless_completion_races_forced_slot_reuse");

    auto reactor = Reactor::get_reactor();

    ManualGate first;
    std::atomic<int> first_done{0};
    reactor_spawn_stackless_task_with_result<int>(
        *reactor, gate_task(&first, 1),
        [&first_done](int) { first_done.fetch_add(1, std::memory_order_acq_rel); });
    first.wait_for_waker();

    // Retain the first task's waker past its completion.
    rusty::Waker stale;
    ASSERT_TRUE(first.try_copy_waker(&stale));

    first.open_and_wake();
    ASSERT_TRUE(drive_until(reactor, [&first_done]() {
        return first_done.load(std::memory_order_acquire) == 1;
    }));

    // The freed slot is now on the free-slot stack, so this task takes it.
    ManualGate second;
    std::atomic<int> second_done{0};
    reactor_spawn_stackless_task_with_result<int>(
        *reactor, gate_task(&second, 2),
        [&second_done](int) { second_done.fetch_add(1, std::memory_order_acq_rel); });
    second.wait_for_waker();

    // Fire the stale waker from a foreign thread, repeatedly.
    std::thread foreign([&stale]() {
        for (int i = 0; i < 10000; ++i) {
            stale.wake();
        }
    });
    for (int i = 0; i < 50; ++i) {
        (*reactor).process_stackless_tasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    foreign.join();

    EXPECT_EQ(0, second_done.load(std::memory_order_acquire))
        << "a stale waker completed the slot's new occupant";
    EXPECT_EQ(1, first_done.load(std::memory_order_acquire))
        << "the retired task completed twice";

    second.open_and_wake();
    ASSERT_TRUE(drive_until(reactor, [&second_done]() {
        return second_done.load(std::memory_order_acquire) == 1;
    }));
}

// ---------------------------------------------------------------------------
// 5. Reactor destruction racing a retained waker.   [ASan]
// ---------------------------------------------------------------------------
// A foreign thread spins wake() straight through ~Reactor.  No crash, wakes
// become defined no-ops, nothing is resurrected in the registry, and no
// allocation outlives the last waker Arc.
TEST_F(StacklessBatteryTest, stackless_reactor_destruction_races_retained_waker) {
    SRPC_TEST_WATCHDOG("stackless_reactor_destruction_races_retained_waker");

    ManualGate gate;
    rusty::Waker retained;
    std::atomic<bool> stop{false};

    {
        auto reactor = Reactor::get_reactor();
        reactor_spawn_stackless_task_with_result<int>(
            *reactor, gate_task(&gate, 5), [](int) {});
        gate.wait_for_waker();
        ASSERT_TRUE(gate.try_copy_waker(&retained));
    }

    std::thread foreign([&retained, &stop]() {
        while (!stop.load(std::memory_order_acquire)) {
            retained.wake();
        }
    });

    // Destroy the reactor underneath the spinning waker.
    *srpc::sp_running_fiber_th_.borrow_mut() = rusty::None;
    srpc::sp_reactor_th_ = rusty::None;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_release);
    foreign.join();

    // Dropping the last waker copy must release the ingress and every ticket
    // it still held.  ASan is the real assertion here.
    retained = rusty::Waker{};
    SUCCEED();
}

// ---------------------------------------------------------------------------
// 6. PollThread shutdown racing a waker.   [ASan]
// ---------------------------------------------------------------------------
TEST_F(StacklessBatteryTest, stackless_pollthread_shutdown_races_waker) {
    SRPC_TEST_WATCHDOG("stackless_pollthread_shutdown_races_waker");

    ManualGate gate;
    rusty::Waker retained;
    std::atomic<bool> stop{false};
    std::atomic<bool> spawned{false};

    // A PollThread owns its own reactor on its own thread; the task must be
    // registered from that thread, which is what makes this different from
    // item 5 rather than a duplicate of it.
    std::thread owner([&gate, &retained, &spawned]() {
        auto reactor = Reactor::get_reactor();
        reactor_spawn_stackless_task_with_result<int>(
            *reactor, gate_task(&gate, 6), [](int) {});
        spawned.store(true, std::memory_order_release);
        // Drain briefly, then let the thread exit so the TLS reactor and the
        // wake registry are torn down in thread-exit order.
        for (int i = 0; i < 50; ++i) {
            (*reactor).process_stackless_tasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        *srpc::sp_running_fiber_th_.borrow_mut() = rusty::None;
        srpc::sp_reactor_th_ = rusty::None;
    });

    while (!spawned.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    gate.wait_for_waker();
    ASSERT_TRUE(gate.try_copy_waker(&retained));

    std::thread foreign([&retained, &stop]() {
        while (!stop.load(std::memory_order_acquire)) {
            retained.wake();
        }
    });

    owner.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true, std::memory_order_release);
    foreign.join();

    retained = rusty::Waker{};
    SUCCEED();
}

// ---------------------------------------------------------------------------
// 7. Direct-construction and non-PollThread reactors.   [ASan]
// ---------------------------------------------------------------------------
// The ctor seeds thread_id_ with the creating thread, so a directly built
// Reactor on a plain thread has to satisfy the same owner-thread contract as
// the two TLS factories.  Disk TLS is a third, independent registry entry.
TEST_F(StacklessBatteryTest, stackless_direct_and_nonpollthread_reactors) {
    SRPC_TEST_WATCHDOG("stackless_direct_and_nonpollthread_reactors");

    std::atomic<int> completed{0};

    std::thread plain([&completed]() {
        // Normal TLS reactor on a thread that is not a PollThread.
        auto reactor = Reactor::get_reactor();
        ManualGate gate;
        reactor_spawn_stackless_task_with_result<int>(
            *reactor, gate_task(&gate, 1),
            [&completed](int) { completed.fetch_add(1, std::memory_order_acq_rel); });
        gate.wait_for_waker();

        std::thread foreign([&gate]() { gate.open_and_wake(); });
        drive_until(reactor, [&completed]() {
            return completed.load(std::memory_order_acquire) >= 1;
        });
        foreign.join();

        // Disk TLS reactor: a second, independent owner entry on this thread.
        auto disk = Reactor::get_disk_reactor();
        ManualGate disk_gate;
        reactor_spawn_stackless_task_with_result<int>(
            *disk, gate_task(&disk_gate, 2),
            [&completed](int) { completed.fetch_add(1, std::memory_order_acq_rel); });
        disk_gate.wait_for_waker();
        std::thread disk_foreign([&disk_gate]() { disk_gate.open_and_wake(); });
        drive_until(disk, [&completed]() {
            return completed.load(std::memory_order_acquire) >= 2;
        });
        disk_foreign.join();

        *srpc::sp_running_fiber_th_.borrow_mut() = rusty::None;
        srpc::sp_reactor_th_ = rusty::None;
    });
    plain.join();

    EXPECT_EQ(2, completed.load(std::memory_order_acquire))
        << "a non-PollThread or disk-TLS reactor did not complete its task";
}

// ---------------------------------------------------------------------------
// 7b. The void spawn lane, on the same owner-thread contract.
// ---------------------------------------------------------------------------
// reactor_spawn_stackless_task_impl is a frozen owned symbol, and it has no
// on_ready callback at all, so a foreign wake there has to be proven through
// the coroutine's own completion.
TEST_F(StacklessBatteryTest, stackless_void_spawn_completes_on_owner_tid) {
    SRPC_TEST_WATCHDOG("stackless_void_spawn_completes_on_owner_tid");

    const long owner_tid = current_tid();
    auto reactor = Reactor::get_reactor();

    ManualGate gate;
    std::atomic<long> done_tid{0};

    reactor_spawn_stackless_task_impl(*reactor, gate_task_void(&gate, &done_tid));
    gate.wait_for_waker();

    std::thread foreign([&gate]() { gate.open_and_wake(); });
    const bool completed = drive_until(reactor, [&done_tid]() {
        return done_tid.load(std::memory_order_acquire) != 0;
    });
    foreign.join();

    ASSERT_TRUE(completed) << "the void spawn lane never completed";
    EXPECT_EQ(owner_tid, done_tid.load(std::memory_order_acquire))
        << "a Task<void> resumed off the owning thread";
}

// ---------------------------------------------------------------------------
// 8. Wake latency bound.
// ---------------------------------------------------------------------------
// Turns the drain-latency ASSUMPTION into a contract.  Owners never park: the
// pollworker idles in epoll_wait with a 1 ms timeout and drains every cycle,
// so a foreign wake must be observed within roughly one idle period.  If a
// future change lets an owner block indefinitely, this test is what fails, and
// the fix is a doorbell in stackless_wake_request -- not a longer bound here.
TEST_F(StacklessBatteryTest, stackless_wake_latency_bound) {
    SRPC_TEST_WATCHDOG("stackless_wake_latency_bound");

    auto reactor = Reactor::get_reactor();

    ManualGate gate;
    std::atomic<bool> done{false};
    reactor_spawn_stackless_task_with_result<int>(
        *reactor, gate_task(&gate, 8),
        [&done](int) { done.store(true, std::memory_order_release); });
    gate.wait_for_waker();

    const auto fired_at = std::chrono::steady_clock::now();
    std::thread foreign([&gate]() { gate.open_and_wake(); });

    ASSERT_TRUE(drive_until(reactor, [&done]() {
        return done.load(std::memory_order_acquire);
    }));
    const auto observed_at = std::chrono::steady_clock::now();
    foreign.join();

    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        observed_at - fired_at);
    // One idle period is 1 ms; allow generous scheduling slack while still
    // failing loudly if the wake needed a park/unpark that does not exist.
    EXPECT_LE(latency.count(), 250)
        << "foreign wake took " << latency.count()
        << " ms -- longer than an owner idle period, so the drain-latency "
           "contract no longer holds and a doorbell is required";
}

// ---------------------------------------------------------------------------
// 9. CLIENT-HANG REGRESSION.   [TSan]
// ---------------------------------------------------------------------------
// Shaped like `external_close_waits_for_an_admitted_response_callback`, the
// test that sat for 15h42m with two threads in futex_do_wait.
//
// Thread A blocks in a close-style wait whose ONLY release is the on_ready
// callback of a stackless task owned by O.  The response is admitted on O and
// a foreign thread fires the wake.  A must come back.
//
// Variant (b) is the half that the memory-safety argument does not cover:
// teardown begins between admission and wake.  A must still come back, and it
// must come back WITH AN ERROR.  Silence there is the bug; a fast error is the
// contract.  This test turns a 15h42m silence into a 30 s red.
namespace {

// A close-style waiter with a cancellation-safe release.  The release is in
// the destructor as well as the success path, which is exactly the contract
// the carrier's W2 comment states callers must honour: teardown destroys the
// callback and its captures, and THAT is what unblocks the waiter with an
// error when the completion can never arrive.
struct CloseWaiter {
    std::mutex mu;
    std::condition_variable cv;
    bool released = false;
    bool errored = false;

    void release_ok() {
        {
            std::lock_guard<std::mutex> guard(mu);
            released = true;
        }
        cv.notify_all();
    }

    void release_error() {
        {
            std::lock_guard<std::mutex> guard(mu);
            released = true;
            errored = true;
        }
        cv.notify_all();
    }

    // Returns false if the wait timed out, i.e. the hang reproduced.
    bool wait_for(std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, budget, [this]() { return released; });
    }
};

// Held by the completion callback.  If the callback runs, the waiter is
// released normally.  If the callback is destroyed without running -- which is
// precisely what teardown does -- the destructor releases it with an error.
struct CompletionHandle {
    CloseWaiter* waiter;
    bool fired = false;

    explicit CompletionHandle(CloseWaiter* w) : waiter(w) {}
    CompletionHandle(CompletionHandle&& o) noexcept
        : waiter(o.waiter), fired(o.fired) {
        o.waiter = nullptr;
    }
    CompletionHandle(const CompletionHandle&) = delete;

    ~CompletionHandle() {
        if (waiter != nullptr && !fired) {
            waiter->release_error();
        }
    }

    void fire() {
        fired = true;
        if (waiter != nullptr) {
            waiter->release_ok();
        }
    }
};

}  // namespace

TEST_F(StacklessBatteryTest, stackless_client_hang_regression) {
    SRPC_TEST_WATCHDOG("stackless_client_hang_regression");

    // ---- variant (a): admitted response, foreign wake, A unblocks OK ----
    {
        auto reactor = Reactor::get_reactor();
        CloseWaiter waiter;
        ManualGate gate;

        auto handle = std::make_shared<CompletionHandle>(&waiter);
        reactor_spawn_stackless_task_with_result<int>(
            *reactor, gate_task(&gate, 1),
            [handle](int) { handle->fire(); });
        gate.wait_for_waker();

        // Thread A: the close-style wait.
        std::atomic<bool> a_returned{false};
        std::atomic<bool> a_errored{false};
        std::thread a([&waiter, &a_returned, &a_errored]() {
            const bool ok = waiter.wait_for(std::chrono::milliseconds(20000));
            a_errored.store(waiter.errored, std::memory_order_release);
            a_returned.store(ok, std::memory_order_release);
        });

        // Response admitted on O, wake fired from F.
        std::thread f([&gate]() { gate.open_and_wake(); });
        drive_until(reactor, [&a_returned]() {
            return a_returned.load(std::memory_order_acquire);
        });
        f.join();
        a.join();

        EXPECT_TRUE(a_returned.load(std::memory_order_acquire))
            << "variant (a): the close wait never returned -- this is the "
               "15h42m hang";
        EXPECT_FALSE(a_errored.load(std::memory_order_acquire))
            << "variant (a): a normal completion reported an error";

        *srpc::sp_running_fiber_th_.borrow_mut() = rusty::None;
        srpc::sp_reactor_th_ = rusty::None;
    }

    // ---- variant (b): teardown between admission and wake -> ERROR, not hang
    {
        CloseWaiter waiter;
        ManualGate gate;
        std::atomic<bool> a_returned{false};
        std::atomic<bool> a_errored{false};

        const StacklessCancelReport before = stackless_cancel_report<rusty::Unit>();

        {
            auto reactor = Reactor::get_reactor();
            auto handle = std::make_shared<CompletionHandle>(&waiter);
            reactor_spawn_stackless_task_with_result<int>(
                *reactor, gate_task(&gate, 2),
                [handle](int) { handle->fire(); });
            gate.wait_for_waker();

            std::thread a([&waiter, &a_returned, &a_errored]() {
                const bool ok = waiter.wait_for(std::chrono::milliseconds(20000));
                a_errored.store(waiter.errored, std::memory_order_release);
                a_returned.store(ok, std::memory_order_release);
            });

            // Admit the completion, but tear the reactor down before it is
            // ever drained.  This is the exact window the plan names.
            gate.ready.store(true, std::memory_order_release);
            rusty::Waker copy;
            if (gate.try_copy_waker(&copy)) {
                copy.wake();
            }

            *srpc::sp_running_fiber_th_.borrow_mut() = rusty::None;
            srpc::sp_reactor_th_ = rusty::None;  // ~Reactor runs here

            a.join();
        }

        EXPECT_TRUE(a_returned.load(std::memory_order_acquire))
            << "variant (b): teardown left the close wait blocked forever -- "
               "this is the silent cancel the W2 audit exists to prevent";
        EXPECT_TRUE(a_errored.load(std::memory_order_acquire))
            << "variant (b): the waiter was released without an error; "
               "cancellation must propagate an error, never silence";

        // The carrier must also have RECORDED the cancellation, not just
        // happened to release the waiter through a destructor.
        const StacklessCancelReport after = stackless_cancel_report<rusty::Unit>();
        EXPECT_GT(after.teardown_tasks + after.admitted_completions +
                      after.pending_wakes + after.rejected_spawns,
                  before.teardown_tasks + before.admitted_completions +
                      before.pending_wakes + before.rejected_spawns)
            << "teardown cancelled a waiter without recording it";
    }
}
// @unsafe }
