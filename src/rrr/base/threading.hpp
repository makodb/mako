#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>

#include <pthread.h>


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




#include "basetypes.hpp"
#include "debugging.hpp"
#include "misc.hpp"

namespace rrr {

inline void Pthread_spin_init(pthread_spinlock_t* lock, int pshared) {
    verify(pthread_spin_init(lock, pshared) == 0);
}

inline void Pthread_spin_lock(pthread_spinlock_t* lock) {
    verify(pthread_spin_lock(lock) == 0);
}

inline void Pthread_spin_unlock(pthread_spinlock_t* lock) {
    verify(pthread_spin_unlock(lock) == 0);
}

inline void Pthread_spin_destroy(pthread_spinlock_t* lock) {
    verify(pthread_spin_destroy(lock) == 0);
}

inline void Pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    verify(pthread_mutex_init(mutex, attr) == 0);
}

inline void Pthread_mutex_lock(pthread_mutex_t* mutex) {
    verify(pthread_mutex_lock(mutex) == 0);
}

inline void Pthread_mutex_unlock(pthread_mutex_t* mutex) {
    verify(pthread_mutex_unlock(mutex) == 0);
}

inline void Pthread_mutex_destroy(pthread_mutex_t* mutex) {
    verify(pthread_mutex_destroy(mutex) == 0);
}

inline void Pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    verify(pthread_cond_init(cond, attr) == 0);
}

inline void Pthread_cond_destroy(pthread_cond_t* cond) {
    verify(pthread_cond_destroy(cond) == 0);
}

inline void Pthread_cond_signal(pthread_cond_t* cond) {
    verify(pthread_cond_signal(cond) == 0);
}

inline void Pthread_cond_broadcast(pthread_cond_t* cond) {
    verify(pthread_cond_broadcast(cond) == 0);
}

inline void Pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    verify(pthread_cond_wait(cond, mutex) == 0);
}

inline void Pthread_create(pthread_t* thread,
                           const pthread_attr_t* attr,
                           void* (*func)(void*),
                           void* arg) {
    verify(pthread_create(thread, attr, func, arg) == 0);
}

inline void Pthread_join(pthread_t thread, void** value_ptr) {
    verify(pthread_join(thread, value_ptr) == 0);
}

class Lockable: public NoCopy {
public:
    enum type {MUTEX, SPINLOCK, EMPTY};

    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual ~Lockable() = default;
//    virtual Lockable::type whatami() = 0;
};

// @unsafe - Used with mutable for interior mutability
class SpinLock: public Lockable {
public:
    // @safe - Initializes to unlocked state
    SpinLock(): locked_(false) { }
    ~SpinLock() override = default;

    // Delete copy and move constructors (atomic is not copyable)
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    // @unsafe - Uses address-of operator for nanosleep call
    // SAFETY: Only takes address of stack-allocated timespec which remains valid throughout nanosleep
    void lock() override {
        // Fast path: try to acquire lock immediately
        bool expected = false;
        if (locked_.compare_exchange_strong(expected, true,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            return;
        }

        // Spin for a short while before sleeping
        int wait = 1000;
        while ((wait-- > 0) && locked_.load(std::memory_order_relaxed)) {
            // CPU-specific pause instruction to reduce contention
#if defined(__i386__) || defined(__x86_64__)
            asm volatile("pause");
#endif
        }

        // Fall back to sleeping if still contended
        struct timespec t;
        t.tv_sec = 0;
        t.tv_nsec = 50000;  // 50 microseconds

        expected = false;
        while (!locked_.compare_exchange_weak(expected, true,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            nanosleep(&t, nullptr);
            expected = false;
        }
    }

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

// @safe - SpinMutexGuard - RAII lock guard for SpinMutex<T>
// This is safe because:
// 1. The guard can only be created by SpinMutex::lock() which acquires the lock
// 2. Data access goes through UnsafeCell which has internal @unsafe blocks
// 3. The destructor releases the lock automatically (RAII)
// 4. Non-copyable ensures single ownership of the lock
template<typename T>
class SpinMutexGuard {
private:
    SpinLock* lock_;
    rusty::UnsafeCell<T>* data_;  // Uses UnsafeCell for interior mutability
    bool owns_lock_;

    friend class SpinMutex<T>;

    // @safe - Private constructor only callable by SpinMutex::lock()
    SpinMutexGuard(SpinLock* lock, rusty::UnsafeCell<T>* data)
        : lock_(lock), data_(data), owns_lock_(true) {}

public:
    // @safe - Access to data through UnsafeCell
    // @lifetime: (&'a) -> &'a
    T& operator*() {
        // @unsafe
        {
            T& ref = data_->as_mut_unchecked();
            return ref;
        }
    }

    // @safe - Const access to data through UnsafeCell
    // @lifetime: (&'a) -> &'a
    const T& operator*() const {
        // @unsafe
        {
            const T& ref = data_->as_ref_unchecked();
            return ref;
        }
    }

    // @safe - Pointer access through UnsafeCell
    // @lifetime: (&'a) -> &'a
    T* operator->() {
        // @unsafe
        {
            return data_->get();
        }
    }

    // @safe - Const pointer access
    // @lifetime: (&'a) -> &'a
    const T* operator->() const {
        // @unsafe
        {
            return data_->get_const();
        }
    }

    // @safe - Get raw pointer through UnsafeCell
    // @lifetime: (&'a) -> &'a
    T* get() {
        // @unsafe
        {
            return data_->get();
        }
    }

    // @safe - Get const raw pointer
    // @lifetime: (&'a) -> &'a
    const T* get() const {
        // @unsafe
        {
            return data_->get_const();
        }
    }

    // @safe - Get mutable reference through UnsafeCell
    // @lifetime: (&'a) -> &'a
    T& get_mut() {
        // @unsafe
        {
            T& ref = data_->as_mut_unchecked();
            return ref;
        }
    }

    // Non-copyable (enforces single ownership of lock)
    SpinMutexGuard(const SpinMutexGuard&) = delete;
    SpinMutexGuard& operator=(const SpinMutexGuard&) = delete;

    // @safe - Movable (transfers ownership)
    SpinMutexGuard(SpinMutexGuard&& other) noexcept
        : lock_(other.lock_), data_(other.data_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    // @safe - Move assignment
    // @lifetime: (&'a, SpinMutexGuard<T>) -> &'a
    SpinMutexGuard& operator=(SpinMutexGuard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_ && lock_) {
                // @unsafe
                { lock_->unlock(); }
            }
            lock_ = other.lock_;
            data_ = other.data_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    // @safe - Destructor unlocks automatically (RAII)
    ~SpinMutexGuard() {
        if (owns_lock_ && lock_) {
            // @unsafe
            { lock_->unlock(); }
        }
    }
};

// @safe - SpinMutex<T> - Thread-safe interior mutability with spinlock
// Similar to Rust's Mutex<T> but uses SpinLock for low-latency locking.
//
// This is safe because:
// 1. Data is stored in UnsafeCell which provides interior mutability
// 2. The lock() method acquires the spinlock before returning the guard
// 3. The guard releases the lock in its destructor (RAII)
// 4. All unsafe operations are encapsulated in internal @unsafe blocks
//
// Usage:
//   SpinMutex<int> counter(0);
//   {
//       auto guard = counter.lock().unwrap();
//       *guard += 1;
//   }  // Lock released here
//
// @safe
template<typename T>
class SpinMutex {
private:
    mutable SpinLock lock_;
    mutable rusty::UnsafeCell<T> data_;  // UnsafeCell for interior mutability

public:
    // Type alias for the guard type
    using Guard = SpinMutexGuard<T>;

    // @safe - Default constructor for default-constructible types
    SpinMutex() : data_() {}

    // @safe - Constructor initializes data
    explicit SpinMutex(T value) : data_(std::move(value)) {}

    // @safe - Acquires lock and returns LockResult
    [[nodiscard]] SpinLockResult<T> lock() {
        // @unsafe
        {
            lock_.lock();
            SpinLock* lock_ptr = &lock_;
            rusty::UnsafeCell<T>* data_ptr = &data_;
            return SpinLockResult<T>::Ok(SpinMutexGuard<T>(lock_ptr, data_ptr));
        }
    }

    // @safe - Acquires lock with const access (interior mutability via UnsafeCell)
    [[nodiscard]] SpinLockResult<T> lock() const {
        // @unsafe
        {
            const_cast<SpinLock&>(lock_).lock();
            SpinLock* lock_ptr = const_cast<SpinLock*>(&lock_);
            rusty::UnsafeCell<T>* data_ptr = const_cast<rusty::UnsafeCell<T>*>(&data_);
            return SpinLockResult<T>::Ok(SpinMutexGuard<T>(lock_ptr, data_ptr));
        }
    }

    // @safe - Attempts to acquire lock without blocking
    // Note: Currently always locks since SpinLock doesn't have try_lock
    [[nodiscard]] rusty::Option<SpinMutexGuard<T>> try_lock() {
        // @unsafe
        {
            lock_.lock();
            SpinLock* lock_ptr = &lock_;
            rusty::UnsafeCell<T>* data_ptr = &data_;
            return rusty::Some(SpinMutexGuard<T>(lock_ptr, data_ptr));
        }
    }

    // @safe - Attempts to acquire lock with const access (interior mutability)
    [[nodiscard]] rusty::Option<SpinMutexGuard<T>> try_lock() const {
        // @unsafe
        {
            const_cast<SpinLock&>(lock_).lock();
            SpinLock* lock_ptr = const_cast<SpinLock*>(&lock_);
            rusty::UnsafeCell<T>* data_ptr = const_cast<rusty::UnsafeCell<T>*>(&data_);
            return rusty::Some(SpinMutexGuard<T>(lock_ptr, data_ptr));
        }
    }
};

// @unsafe - Helper function to create SpinMutex
template<typename T>
auto make_spin_mutex(T value) {
    return SpinMutex<T>(std::move(value));
}

// @safe - Spin-based condition variable using atomic flag
class SpinCondVar {
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

class ThreadPool: public NoCopy {
    int n_;
    Counter round_robin_;
    rusty::Vec<pthread_t> th_;
    // Queue owns pthread primitives (mutex/cond) with stable addresses, so it
    // is not move-constructible. rusty::Vec needs a moveable T for push(),
    // so use std::vector here (resize() constructs in place).
    std::vector<Queue<rusty::Box<std::function<void()>>>> q_;
    bool should_stop_{false};

    static void* start_thread_pool(void*);
    void run_thread(int id_in_pool);

public:
    ~ThreadPool() noexcept;

public:
    ThreadPool(int n = 1 /*get_ncpu() * 2*/);
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // return 0 when queuing ok, otherwise EPERM
    int run_async(const std::function<void()>&);

    // @unsafe - Factory uses rusty::Arc::make (non-borrow-checked)
    template<typename... Args>
    static rusty::Arc<ThreadPool> make(Args&&... args) {
        // @unsafe { rusty::Arc::make is not borrow-checked }
        return rusty::Arc<ThreadPool>::make(std::forward<Args>(args)...);
    }
};

class RunLater: public NoCopy {
    typedef std::pair<double, std::function<void()>*> job_t;

    pthread_t th_;
    pthread_mutex_t m_;
    pthread_cond_t cv_;
    bool should_stop_{};

    SpinLock latest_l_{};
    double latest_{};

    rusty::Vec<job_t> jobs_{};

    static void* start_run_later(void*);
    void run_later_loop();
    void try_one_job();
public:
    RunLater();
    ~RunLater() noexcept;

    // return 0 when queuing ok, otherwise EPERM
    int run_later(double sec, const std::function<void()>&);

    double max_wait() const;

    // @unsafe - Factory uses rusty::Arc::make (non-borrow-checked)
    template<typename... Args>
    static rusty::Arc<RunLater> make(Args&&... args) {
        // @unsafe { rusty::Arc::make is not borrow-checked }
        return rusty::Arc<RunLater>::make(std::forward<Args>(args)...);
    }
};

} // namespace base
