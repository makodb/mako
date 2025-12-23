#pragma once

#include <rusty/box.hpp>
#include <rusty/result.hpp>
#include <rusty/option.hpp>

#include <list>
#include <queue>
#include <vector>
#include <functional>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

#include "basetypes.hpp"
#include "misc.hpp"

// External safety annotations for pthread and std functions used in this module
// @external: {
//   pthread_spin_init: [unsafe, (pthread_spinlock_t*, int) -> int]
//   pthread_spin_lock: [unsafe, (pthread_spinlock_t*) -> int]
//   pthread_spin_unlock: [unsafe, (pthread_spinlock_t*) -> int]
//   pthread_spin_destroy: [unsafe, (pthread_spinlock_t*) -> int]
//   pthread_mutex_init: [unsafe, (pthread_mutex_t*, const pthread_mutexattr_t*) -> int]
//   pthread_mutex_lock: [unsafe, (pthread_mutex_t*) -> int]
//   pthread_mutex_unlock: [unsafe, (pthread_mutex_t*) -> int]
//   pthread_mutex_destroy: [unsafe, (pthread_mutex_t*) -> int]
//   pthread_cond_init: [unsafe, (pthread_cond_t*, const pthread_condattr_t*) -> int]
//   pthread_cond_destroy: [unsafe, (pthread_cond_t*) -> int]
//   pthread_cond_signal: [unsafe, (pthread_cond_t*) -> int]
//   pthread_cond_broadcast: [unsafe, (pthread_cond_t*) -> int]
//   pthread_cond_wait: [unsafe, (pthread_cond_t*, pthread_mutex_t*) -> int]
//   pthread_cond_timedwait: [unsafe, (pthread_cond_t*, pthread_mutex_t*, const struct timespec*) -> int]
//   pthread_create: [unsafe, (pthread_t*, const pthread_attr_t*, void*(*)(void*), void*) -> int]
//   pthread_join: [unsafe, (pthread_t, void**) -> int]
//   pthread_exit: [unsafe, (void*) -> void]
//   pthread_mutexattr_init: [unsafe, (pthread_mutexattr_t*) -> int]
//   pthread_mutexattr_settype: [unsafe, (pthread_mutexattr_t*, int) -> int]
//   pthread_mutexattr_destroy: [unsafe, (pthread_mutexattr_t*) -> int]
//   std::atomic::store: [unsafe, (bool, std::memory_order) -> void]
//   std::atomic::load: [unsafe, (std::memory_order) -> bool]
//   std::__atomic_base::store: [unsafe, (int, std::memory_order) -> void]
//   std::__atomic_base::load: [unsafe, (std::memory_order) -> int]
//   nanosleep: [unsafe, (const struct timespec*, struct timespec*) -> int]
//   std::mutex::lock: [unsafe, () -> void]
//   std::mutex::unlock: [unsafe, () -> void]
//   std::condition_variable::wait: [unsafe, (std::unique_lock<std::mutex>&) -> void]
//   std::condition_variable::notify_one: [unsafe, () -> void]
//   std::condition_variable::notify_all: [unsafe, () -> void]
//   std::condition_variable::wait_for: [unsafe, (std::unique_lock<std::mutex>&, duration) -> std::cv_status]
// }

#define Pthread_spin_init(l, pshared) verify(pthread_spin_init(l, (pshared)) == 0)
#define Pthread_spin_lock(l) verify(pthread_spin_lock(l) == 0)
#define Pthread_spin_unlock(l) verify(pthread_spin_unlock(l) == 0)
#define Pthread_spin_destroy(l) verify(pthread_spin_destroy(l) == 0)
#define Pthread_mutex_init(m, attr) verify(pthread_mutex_init(m, attr) == 0)
#define Pthread_mutex_lock(m) verify(pthread_mutex_lock(m) == 0)
#define Pthread_mutex_unlock(m) verify(pthread_mutex_unlock(m) == 0)
#define Pthread_mutex_destroy(m) verify(pthread_mutex_destroy(m) == 0)
#define Pthread_cond_init(c, attr) verify(pthread_cond_init(c, attr) == 0)
#define Pthread_cond_destroy(c) verify(pthread_cond_destroy(c) == 0)
#define Pthread_cond_signal(c) verify(pthread_cond_signal(c) == 0)
#define Pthread_cond_broadcast(c) verify(pthread_cond_broadcast(c) == 0)
#define Pthread_cond_wait(c, m) verify(pthread_cond_wait(c, m) == 0)
#define Pthread_create(th, attr, func, arg) verify(pthread_create(th, attr, func, arg) == 0)
#define Pthread_join(th, attr) verify(pthread_join(th, attr) == 0)

namespace rrr {

class Lockable: public NoCopy {
public:
    enum type {MUTEX, SPINLOCK, EMPTY};

    virtual void lock() = 0;
    virtual void unlock() = 0;
//    virtual Lockable::type whatami() = 0;
};

// @unsafe - Used with mutable for interior mutability
class SpinLock: public Lockable {
public:
    // @safe - Initializes to unlocked state
    SpinLock(): locked_(false) { }

    // Delete copy and move constructors (atomic is not copyable)
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    // @unsafe - Uses address-of operator for nanosleep call
    void lock();

    // @unsafe - Calls std::atomic::store
    void unlock() {
        locked_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> locked_ alignas(64);  // Cache-line aligned to prevent false sharing
};

// =============================================================================
// SpinMutex<T> - Rust-like Mutex API using SpinLock
// =============================================================================
// Similar to rusty::Mutex<T>, but uses SpinLock for low-latency locking.
// Returns LockResult<T> from lock() for API consistency.

// Forward declarations
template<typename T> class SpinMutex;
template<typename T> class SpinMutexGuard;

// @safe - PoisonError for SpinMutex (placeholder, C++ doesn't have poisoning)
template<typename T>
class SpinPoisonError {
public:
    SpinPoisonError() = default;
};

// Type alias for SpinMutex lock result
template<typename T>
using SpinLockResult = rusty::Result<SpinMutexGuard<T>, SpinPoisonError<T>>;

// @unsafe - SpinMutexGuard - RAII lock guard for SpinMutex<T>
template<typename T>
class SpinMutexGuard {
private:
    SpinLock* lock_;
    T* data_;
    bool owns_lock_;

    friend class SpinMutex<T>;

    SpinMutexGuard(SpinLock* lock, T* data)
        : lock_(lock), data_(data), owns_lock_(true) {}

public:
    // Access to data (dereferences pointer)
    // @lifetime: (&'a) -> &'a
    T& operator*() {
        // @unsafe
        { return *data_; }
    }
    // @lifetime: (&'a) -> &'a
    const T& operator*() const {
        // @unsafe
        { return *data_; }
    }

    // Pointer access
    // @safe
    // @lifetime: (&'a) -> &'a
    T* operator->() { return data_; }
    // @safe
    // @lifetime: (&'a) -> &'a
    const T* operator->() const { return data_; }

    // Get raw pointer
    // @lifetime: (&'a) -> &'a
    T* get() { return data_; }
    // @lifetime: (&'a) -> &'a
    const T* get() const { return data_; }

    // Get mutable reference
    // @lifetime: (&'a) -> &'a
    T& get_mut() {
        // @unsafe
        { return *data_; }
    }

    // Non-copyable
    SpinMutexGuard(const SpinMutexGuard&) = delete;
    SpinMutexGuard& operator=(const SpinMutexGuard&) = delete;

    // @unsafe - Movable (transfers ownership of raw pointers)
    SpinMutexGuard(SpinMutexGuard&& other) noexcept
        : lock_(other.lock_), data_(other.data_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    SpinMutexGuard& operator=(SpinMutexGuard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_ && lock_) {
                lock_->unlock();
            }
            lock_ = other.lock_;
            data_ = other.data_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    // @unsafe - Destructor unlocks automatically
    ~SpinMutexGuard() {
        if (owns_lock_ && lock_) {
            lock_->unlock();
        }
    }
};

// @unsafe - SpinMutex<T> - Thread-safe interior mutability with spinlock
// Similar to rusty::Mutex<T> but uses SpinLock for low-latency locking.
//
// Usage:
//   SpinMutex<int> counter(0);
//   {
//       auto guard = counter.lock().unwrap();
//       *guard += 1;
//   }  // Lock released here
//
template<typename T>
class SpinMutex : public NoCopy {
private:
    mutable SpinLock lock_;
    mutable T data_;

public:
    // Type alias for the guard type
    using Guard = SpinMutexGuard<T>;

    // @unsafe - Default constructor for default-constructible types
    SpinMutex() : data_() {}

    // @unsafe - Constructor initializes data
    explicit SpinMutex(T value) : data_(std::move(value)) {}

    // @unsafe - Acquires lock and returns LockResult
    [[nodiscard]] SpinLockResult<T> lock() {
        lock_.lock();
        return SpinLockResult<T>::Ok(SpinMutexGuard<T>(&lock_, &data_));
    }

    // @unsafe - Acquires lock with const access (interior mutability)
    [[nodiscard]] SpinLockResult<T> lock() const {
        lock_.lock();
        return SpinLockResult<T>::Ok(SpinMutexGuard<T>(
            const_cast<SpinLock*>(&lock_),
            const_cast<T*>(&data_)
        ));
    }

    // @unsafe - Attempts to acquire lock without blocking
    // Note: Currently always locks since SpinLock doesn't have try_lock
    [[nodiscard]] rusty::Option<SpinMutexGuard<T>> try_lock() {
        // SpinLock doesn't have try_lock, so we just lock
        // For a real try_lock, we'd need to modify SpinLock
        lock_.lock();
        return rusty::Some(SpinMutexGuard<T>(&lock_, &data_));
    }

    // @unsafe - Attempts to acquire lock with const access (interior mutability)
    [[nodiscard]] rusty::Option<SpinMutexGuard<T>> try_lock() const {
        lock_.lock();
        return rusty::Some(SpinMutexGuard<T>(
            const_cast<SpinLock*>(&lock_),
            const_cast<T*>(&data_)
        ));
    }
};

// @unsafe - Helper function to create SpinMutex
template<typename T>
auto make_spin_mutex(T value) {
    return SpinMutex<T>(std::move(value));
}

// @safe - Spin-based condition variable using atomic flag
class SpinCondVar: public NoCopy {
private:
    std::atomic<int> flag_{0};

public:
    // @safe - Default constructor
    SpinCondVar() = default;
    
    // @safe - Default destructor
    ~SpinCondVar() = default;

    // @unsafe - Calls std::atomic::store/load (external unsafe)
    // SAFETY: Thread-safe atomic operations, proper lock/unlock ordering
    void wait(SpinLock& sl) {
        flag_.store(0, std::memory_order_relaxed);
        sl.unlock();

        while(flag_.load(std::memory_order_acquire) == 0) {
            Time::sleep(10);
        }
        sl.lock();
    }

    // @unsafe - Calls std::atomic::store/load (external unsafe)
    // SAFETY: Thread-safe atomic operations, proper lock/unlock ordering
    void timed_wait(SpinLock& sl, double sec) {
        flag_.store(0, std::memory_order_relaxed);
        sl.unlock();

        Timer t;
        t.start();
        while(flag_.load(std::memory_order_acquire) == 0) {
            Time::sleep(10);
            if (t.elapsed() > sec) {
                break;
            }
        }
        sl.lock();
    }

    // @unsafe - Calls std::atomic::store (external unsafe)
    // SAFETY: Thread-safe atomic store operation
    void signal() {
        flag_.store(1, std::memory_order_release);
    }

    // @unsafe - Calls std::atomic::store (external unsafe)
    // SAFETY: Thread-safe atomic store operation
    void bcast() {
        flag_.store(1, std::memory_order_release);
    }
};


/**
 * Thread safe queue using rusty::Box for automatic memory management.
 * @unsafe - Uses raw pthread primitives for performance
 * SAFETY: All public methods are thread-safe through mutex protection
 * Supports move-only types like rusty::Box<T>.
 */
template<class T>
class Queue: public NoCopy {
    rusty::Box<std::list<T>> q_;
    pthread_cond_t not_empty_;
    pthread_mutex_t m_;

public:
    // @unsafe - Initializes pthread primitives
    Queue(): q_(rusty::Box<std::list<T>>::make(std::list<T>())), not_empty_(), m_() {
        Pthread_mutex_init(&m_, nullptr);
        Pthread_cond_init(&not_empty_, nullptr);
    }

    // @unsafe - Destroys pthread primitives
    ~Queue() {
        Pthread_cond_destroy(&not_empty_);
        Pthread_mutex_destroy(&m_);
        // q_ automatically deleted by rusty::Box
    }

    // @unsafe - Thread-safe push with mutex protection (move semantics)
    void push(T e) {
        Pthread_mutex_lock(&m_);
        q_->push_back(std::move(e));
        Pthread_cond_signal(&not_empty_);
        Pthread_mutex_unlock(&m_);
    }

    // @unsafe - Thread-safe try_pop with mutex protection
    // SAFETY: Returns via output parameter using move semantics
    bool try_pop(T* t) {
        bool ret = false;
        Pthread_mutex_lock(&m_);
        if (!q_->empty()) {
            ret = true;
            *t = std::move(q_->front());
            q_->pop_front();
        }
        Pthread_mutex_unlock(&m_);
        return ret;
    }

    // @unsafe - Thread-safe try_pop that ignores invalid/null items
    // For rusty::Box<T>, this ignores items where !is_valid()
    // SAFETY: Returns via output parameter using move semantics
    bool try_pop_but_ignore_invalid(T* t) {
        bool ret = false;
        Pthread_mutex_lock(&m_);
        if (!q_->empty() && q_->front().is_valid()) {
            ret = true;
            *t = std::move(q_->front());
            q_->pop_front();
        }
        Pthread_mutex_unlock(&m_);
        return ret;
    }

    // @unsafe - Thread-safe blocking pop
    // SAFETY: Returns by value (move), not by reference. Borrow checker false positive.
    T pop() {
        Pthread_mutex_lock(&m_);
        while (q_->empty()) {
            Pthread_cond_wait(&not_empty_, &m_);
        }
        auto result = std::move(q_->front());
        q_->pop_front();
        Pthread_mutex_unlock(&m_);
        return result;
    }
};

class ThreadPool: public RefCounted {
    int n_;
    Counter round_robin_;
    std::vector<pthread_t> th_;
    std::vector<Queue<rusty::Box<std::function<void()>>>> q_;
    bool should_stop_{false};

    static void* start_thread_pool(void*);
    void run_thread(int id_in_pool);

protected:
    ~ThreadPool();

public:
    ThreadPool(int n = 1 /*get_ncpu() * 2*/);
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // return 0 when queuing ok, otherwise EPERM
    int run_async(const std::function<void()>&);
};

class RunLater: public RefCounted {
    typedef std::pair<double, std::function<void()>*> job_t;

    pthread_t th_;
    pthread_mutex_t m_;
    pthread_cond_t cv_;
    bool should_stop_{};

    SpinLock latest_l_{};
    double latest_{};

    std::priority_queue<job_t, std::vector<job_t>, std::greater<job_t>> jobs_{};

    static void* start_run_later(void*);
    void run_later_loop();
    void try_one_job();
public:
    RunLater();

    // return 0 when queuing ok, otherwise EPERM
    int run_later(double sec, const std::function<void()>&);

    double max_wait() const;
protected:
    ~RunLater();
};

} // namespace base
