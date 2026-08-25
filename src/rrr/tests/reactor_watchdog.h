/**
 * @file reactor_watchdog.h
 * @brief 30-second all-thread watchdog for the Reactor promotion battery.
 *
 * Every test in the stackless-waker battery runs under this watchdog.  The
 * reason is literal: the client-hang evidence
 * (/var/tmp/srpc-client-hang-evidence.1786727169/EVIDENCE.md) is a test that
 * sat in `futex_do_wait` for 15h42m and was only noticed by a human walking
 * past.  Nothing bounded it.  A hang that reports in 30 seconds with the state
 * of every thread is a red test; a hang that reports never is lost work.
 *
 * Dumping "all-thread stacks" has to survive this environment specifically.
 * The evidence run could not get userspace stacks at all -- ptrace returned
 * EPERM under yama, so gdb/eu-stack attach is not a dependable primitive here.
 * The watchdog therefore reports twice over:
 *
 *   1. /proc/self/task/<tid>/{comm,stat,wchan} for every live thread.  This is
 *      exactly the evidence that identified the original hang (`wchan` +
 *      thread count), and it needs no privileges at all.
 *   2. An in-process backtrace from every thread, collected by broadcasting a
 *      signal whose handler runs `backtrace_symbols_fd` on the receiving
 *      thread.  This needs no ptrace either, because each thread walks its own
 *      stack.
 *
 * Then it aborts, so the failure is loud and the core is inspectable.  Abort
 * rather than a graceful gtest failure is deliberate: the process is by
 * definition wedged, so unwinding through it cannot be trusted to work.
 *
 * TSan/ASan note: the watchdog thread only ever reads atomics and a mutex, and
 * on the timeout path the process is ending anyway, so it adds no races of its
 * own to the sanitizer runs (battery items 1-9 under TSan, 5-7 under ASan).
 */

#ifndef RRR_TESTS_REACTOR_WATCHDOG_H
#define RRR_TESTS_REACTOR_WATCHDOG_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace rrr_test {

// Signal used to ask every thread to print its own backtrace.  SIGUSR2 is not
// used by the reactor, the fiber runtime, or gtest.
static constexpr int kWatchdogDumpSignal = SIGUSR2;

inline void watchdog_backtrace_handler(int /*signo*/) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    // Async-signal-safe: write(2) and backtrace_symbols_fd(3) only.  Never
    // printf here -- a wedged process is exactly when a malloc lock is held.
    char header[64];
    const int len = ::snprintf(header, sizeof(header), "\n--- thread %ld ---\n",
                               static_cast<long>(::syscall(SYS_gettid)));
    if (len > 0) {
        ssize_t ignored = ::write(STDERR_FILENO, header, static_cast<size_t>(len));
        (void)ignored;
    }
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

inline void watchdog_install_dump_handler() {
    struct sigaction sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &watchdog_backtrace_handler;
    sa.sa_flags = SA_RESTART;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(kWatchdogDumpSignal, &sa, nullptr);
}

// Reports /proc state for every thread, then asks each to dump its own stack.
inline void watchdog_dump_all_threads(const char* test_name) {
    ::fprintf(stderr, "\n==================== WATCHDOG EXPIRED ====================\n");
    ::fprintf(stderr, "test: %s\n", test_name != nullptr ? test_name : "<unknown>");
    ::fprintf(stderr, "pid : %ld\n", static_cast<long>(::getpid()));

    const long self_tid = static_cast<long>(::syscall(SYS_gettid));
    const pid_t pid = ::getpid();

    // Pass 1: privilege-free per-thread state.  This is the same signal that
    // identified the 15h42m hang (two threads, both futex_do_wait).
    DIR* dir = ::opendir("/proc/self/task");
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            char path[256];
            char buf[512];

            ::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
            buf[0] = '\0';
            FILE* f = ::fopen(path, "r");
            if (f != nullptr) {
                if (::fgets(buf, sizeof(buf), f) == nullptr) {
                    buf[0] = '\0';
                }
                ::fclose(f);
            }
            ::fprintf(stderr, "thread %-8s comm=%-20s", entry->d_name, buf);

            ::snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", entry->d_name);
            buf[0] = '\0';
            f = ::fopen(path, "r");
            if (f != nullptr) {
                if (::fgets(buf, sizeof(buf), f) == nullptr) {
                    buf[0] = '\0';
                }
                ::fclose(f);
            }
            ::fprintf(stderr, " wchan=%s\n", buf);
        }
        ::closedir(dir);
    }
    ::fflush(stderr);

    // Pass 2: each thread walks its own stack, so no ptrace is needed.
    dir = ::opendir("/proc/self/task");
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            const long tid = ::strtol(entry->d_name, nullptr, 10);
            if (tid == self_tid) {
                continue;  // the watchdog's own stack is not interesting
            }
            ::syscall(SYS_tgkill, static_cast<long>(pid), tid,
                      static_cast<long>(kWatchdogDumpSignal));
        }
        ::closedir(dir);
    }
    // Give the signalled threads a moment to write before the process dies.
    ::usleep(300 * 1000);
    ::fprintf(stderr, "==========================================================\n");
    ::fflush(stderr);
}

/**
 * Scoped watchdog.  Construct at the top of a test body; the destructor
 * cancels it.  If the body has not finished within `seconds`, every thread is
 * dumped and the process aborts.
 */
class Watchdog {
public:
    explicit Watchdog(const char* test_name, unsigned seconds = 30)
        : test_name_(test_name), seconds_(seconds) {
        watchdog_install_dump_handler();
        thread_ = std::thread([this]() { this->Run(); });
    }

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            finished_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool finished = cv_.wait_for(lock, std::chrono::seconds(seconds_),
                                           [this]() { return finished_; });
        if (finished) {
            return;
        }
        lock.unlock();
        watchdog_dump_all_threads(test_name_);
        ::fprintf(stderr, "WATCHDOG: '%s' exceeded %us -- aborting.\n",
                  test_name_ != nullptr ? test_name_ : "<unknown>", seconds_);
        ::fflush(stderr);
        ::abort();
    }

    const char* test_name_;
    unsigned seconds_;
    bool finished_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};

}  // namespace rrr_test

// Every battery test starts with this.  Naming the test in the macro keeps the
// watchdog report self-identifying in a parallel ctest run.
#define RRR_TEST_WATCHDOG(name) ::rrr_test::Watchdog rrr_test_watchdog_(name, 30)

#endif  // RRR_TESTS_REACTOR_WATCHDOG_H
