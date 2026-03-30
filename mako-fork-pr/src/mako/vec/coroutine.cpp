#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <coroutine>
#include <queue>
#include <chrono>

using namespace std;
using namespace std::chrono;

atomic<bool> stopFlag(false); // Flag to stop threads

// Forward declaration of Scheduler
struct Scheduler;

// Task type representing a coroutine
struct Task {
    struct promise_type {
        Scheduler* scheduler;

        Task get_return_object() {
            return Task{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_always initial_suspend() { return {}; }
        struct final_awaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<>) noexcept {}
            void await_resume() noexcept {}
        };
        final_awaiter final_suspend() noexcept { return {}; }
        void unhandled_exception() {}
        void return_void() {}
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() {
        if (handle) handle.destroy();
    }

    void start(Scheduler& scheduler);
};

// Scheduler to manage coroutines within a thread
struct Scheduler {
    using TaskEntry = pair<steady_clock::time_point, std::coroutine_handle<>>;
    priority_queue<TaskEntry, vector<TaskEntry>, greater<>> tasks;

    void schedule(std::coroutine_handle<> handle) {
        tasks.emplace(steady_clock::now(), handle);
    }

    void schedule_at(std::coroutine_handle<> handle, steady_clock::time_point time) {
        tasks.emplace(time, handle);
    }

    void run() {
        while (!stopFlag.load()) {
            if (!tasks.empty()) {
                auto [time, task] = tasks.top();
                if (steady_clock::now() >= time) {
                    tasks.pop();
                    if (!task.done()) {
                        task.resume();
                    }
                } else {
                    auto sleep_duration = time - steady_clock::now();
                    if (sleep_duration > milliseconds(1)) {
                        std::this_thread::sleep_for(milliseconds(1));
                    } else {
                        std::this_thread::yield();
                    }
                }
            } else {
                std::this_thread::yield();
            }
        }
    }
};

void Task::start(Scheduler& scheduler) {
    handle.promise().scheduler = &scheduler;
    scheduler.schedule(handle);
}

// Awaitable for coroutine sleep
struct SleepAwaitable {
    Scheduler& scheduler;
    steady_clock::time_point wake_up_time;

    SleepAwaitable(Scheduler& sched, microseconds dur)
        : scheduler(sched), wake_up_time(steady_clock::now() + dur) {}

    bool await_ready() {
        return steady_clock::now() >= wake_up_time;
    }
    void await_suspend(std::coroutine_handle<> h) {
        scheduler.schedule_at(h, wake_up_time);
    }
    void await_resume() {}
};

// Coroutine function simulating transaction processing
Task coroutine_function(Scheduler& scheduler, int& local_throughput) {
    while (!stopFlag.load()) {
        local_throughput++;
        co_await SleepAwaitable(scheduler, microseconds(15));
    }
}

// Worker thread function running two coroutines
void workerThread(int thread_id, int& local_throughput) {
    Scheduler scheduler;

    Task t1 = coroutine_function(scheduler, local_throughput);
    Task t2 = coroutine_function(scheduler, local_throughput);

    t1.start(scheduler);
    t2.start(scheduler);

    scheduler.run();
}

int main(int argc, char* argv[]) {
    int num_threads = 4;
    vector<thread> threads;
    vector<int> thread_throughput(num_threads, 0); // Per-thread throughput counters

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(workerThread, i, ref(thread_throughput[i]));
    }

    int run_time = 10;
    this_thread::sleep_for(seconds(run_time));
    stopFlag.store(true);

    // Join threads
    for (auto& t : threads) {
        t.join();
    }

    // Sum up throughput from all threads
    int total_throughput = 0;
    for (int i = 0; i < num_threads; ++i) {
        total_throughput += thread_throughput[i];
    }

    // Output throughput
    cout << "Throughput: " << total_throughput / run_time << " txn/sec" << endl;

    return 0;
}
