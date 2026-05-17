module;

#include <rusty/rusty.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/result.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>
#include <rusty/vecdeque.hpp>

#include <pthread.h>
#include <sys/time.h>

export module rrr.threading;

import std;
import rrr.basetypes;
import rrr.debugging;
import rrr.misc;

export namespace rrr {


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
    rusty::Box<rusty::VecDeque<T>> q_;
    pthread_cond_t not_empty_;
    pthread_mutex_t m_;

public:
    // @unsafe - Initializes pthread primitives
    Queue(): q_(rusty::Box<rusty::VecDeque<T>>::make(rusty::VecDeque<T>())), not_empty_(), m_() {
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
        if (!q_->is_empty()) {
            ret = true;
            *t = q_->pop_front();
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
        if (!q_->is_empty() && q_->front().is_valid()) {
            ret = true;
            *t = q_->pop_front();
        }
        Pthread_mutex_unlock(&m_);
        return ret;
    }

    // @unsafe - Thread-safe blocking pop
    // SAFETY: Returns by value (move), not by reference. Borrow checker false positive.
    T pop() {
        Pthread_mutex_lock(&m_);
        while (q_->is_empty()) {
            Pthread_cond_wait(&not_empty_, &m_);
        }
        auto result = q_->pop_front();
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
    std::vector<Queue<rusty::Box<rusty::Function<void()>>>> q_;
    bool should_stop_{false};

    static void* start_thread_pool(void*);
    void run_thread(int id_in_pool);

public:
    ~ThreadPool() noexcept;

public:
    ThreadPool(int n = 1 /*get_ncpu() * 2*/);
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // return 0 when queuing ok, otherwise EPERM. Takes ownership of the
    // callable; rusty::Function is move-only so callers pass a lambda
    // (which converts implicitly) or std::move an existing Function.
    int run_async(rusty::Function<void()> f);

    // @unsafe - Factory uses rusty::Arc::make (non-borrow-checked)
    template<typename... Args>
    static rusty::Arc<ThreadPool> make(Args&&... args) {
        // @unsafe { rusty::Arc::make is not borrow-checked }
        return rusty::Arc<ThreadPool>::make(std::forward<Args>(args)...);
    }
};

class RunLater: public NoCopy {
    // The Option<Box<Function>> payload is None for the death-pill
    // (former `nullptr`) and Some(box) for real jobs. Box owns the
    // heap-allocated rusty::Function (former `new std::function(f)`).
    typedef std::pair<double,
                      rusty::Option<rusty::Box<rusty::Function<void()>>>> job_t;

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

    // return 0 when queuing ok, otherwise EPERM. Takes ownership of the
    // callable; rusty::Function is move-only so callers pass a lambda
    // (which converts implicitly) or std::move an existing Function.
    int run_later(double sec, rusty::Function<void()> f);

    double max_wait() const;

    // @unsafe - Factory uses rusty::Arc::make (non-borrow-checked)
    template<typename... Args>
    static rusty::Arc<RunLater> make(Args&&... args) {
        // @unsafe { rusty::Arc::make is not borrow-checked }
        return rusty::Arc<RunLater>::make(std::forward<Args>(args)...);
    }
};


} // export namespace rrr

namespace rrr {

struct start_thread_pool_args {
    ThreadPool* thrpool;
    int id_in_pool;
};

void* ThreadPool::start_thread_pool(void* args) {
    start_thread_pool_args* t_args = (start_thread_pool_args *) args;
    t_args->thrpool->run_thread(t_args->id_in_pool);
    delete t_args;
    pthread_exit(nullptr);
    return nullptr;
}

ThreadPool::ThreadPool(int n /* =... */)
    : n_(n), round_robin_(), th_(n), q_(n) {
    verify(n_ >= 0);

    // rusty::Vec(size_t) only reserves capacity, it does NOT populate the
    // vector. Grow th_ to n elements before indexed assignment. q_ is a
    // std::vector (see threading.hpp) and the constructor already sized it.
    for (int i = 0; i < n_; i++) {
        th_.push(pthread_t{});
    }

    for (int i = 0; i < n_; i++) {
        start_thread_pool_args* args = new start_thread_pool_args();
        args->thrpool = this;
        args->id_in_pool = i;
        Pthread_create(&th_[i], nullptr, ThreadPool::start_thread_pool, args);
    }
}

ThreadPool::~ThreadPool() noexcept {
    should_stop_ = true;
    for (int i = 0; i < n_; i++) {
        q_[i].push(rusty::Box<rusty::Function<void()>>(nullptr));  // death pill
    }
    for (int i = 0; i < n_; i++) {
        Pthread_join(th_[i], nullptr);
    }
    // check if there's left over jobs
    for (int i = 0; i < n_; i++) {
        rusty::Box<rusty::Function<void()>> job(nullptr);
        while (q_[i].try_pop(&job)) {
            if (job.is_valid()) {
                (*job)();
            }
        }
    }
    // th_ and q_ are now std::vector, automatically cleaned up
}

int ThreadPool::run_async(rusty::Function<void()> f) {
    if (should_stop_) {
        return EPERM;
    }
    int queue_id = round_robin_.next() % n_;
    q_[queue_id].push(rusty::make_box<rusty::Function<void()>>(std::move(f)));
    return 0;
}

void ThreadPool::run_thread(int id_in_pool) {
    struct timespec sleep_req;
    const int min_sleep_nsec = 1000;  // 1us
    const int max_sleep_nsec = 50 * 1000;  // 50us
    sleep_req.tv_nsec = 1000;  // 1us
    sleep_req.tv_sec = 0;
    int stage = 0;

    // randomized stealing order
    rusty::Vec<int> steal_order(n_);
    for (int i = 0; i < n_; i++) {
        steal_order.push(i);
    }
    Rand r;
    for (int i = 0; i < n_ - 1; i++) {
        int j = r.next(i, n_);
        if (j != i) {
            std::swap(steal_order[j], steal_order[i]);
        }
    }

    // fallback stages: try_pop -> sleep -> try_pop -> steal -> pop
    // succeed: sleep - 1
    // failure: sleep + 10
    for (;;) {
        rusty::Box<rusty::Function<void()>> job(nullptr);

        switch(stage) {
        case 0:
        case 2:
            if (q_[id_in_pool].try_pop(&job)) {
                stage = 0;
            } else {
                stage++;
            }
            break;
        case 1:
            nanosleep(&sleep_req, nullptr);
            stage++;
            break;
        case 3:
            for (int i = 0; i < n_; i++) {
                if (steal_order[i] != id_in_pool) {
                    // just don't steal other thread's death pill (null Box), otherwise they won't die
                    if (q_[steal_order[i]].try_pop_but_ignore_invalid(&job)) {
                        stage = 0;
                        break;
                    }
                }
            }
            if (stage != 0) {
                stage++;
            }
            break;
        case 4:
            job = q_[id_in_pool].pop();
            stage = 0;
            break;
        }

        if (stage == 0) {
            if (!job.is_valid()) {
                break;
            }
            (*job)();
            // job is automatically cleaned up when it goes out of scope
            sleep_req.tv_nsec = clamp(sleep_req.tv_nsec - 1000, min_sleep_nsec, max_sleep_nsec);
        } else {
            sleep_req.tv_nsec = clamp(sleep_req.tv_nsec + 1000, min_sleep_nsec, max_sleep_nsec);
        }
    }
    // steal_order is automatically cleaned up (std::vector)
}

// Min-heap comparator over the wall-clock timestamp `pair.first`.
// Replaces the prior `std::greater<job_t>{}`, which transitively required
// `operator<` on the second element — now `Option<Box<Function>>`, which
// does not provide one. We only need to order on time anyway.
struct GreaterByJobTime {
    template <typename Pair>
    bool operator()(const Pair& a, const Pair& b) const noexcept {
        return a.first > b.first;
    }
};

void* RunLater::start_run_later(void* thiz) {
    RunLater* rl = (RunLater *) thiz;
    rl->run_later_loop();
    pthread_exit(nullptr);
    return nullptr;
}

RunLater::RunLater() :
    th_(), m_(), cv_() {
    should_stop_ = false;
    latest_ = 0.0;
    Pthread_mutex_init(&m_, nullptr);
    Pthread_cond_init(&cv_, nullptr);
    Pthread_create(&th_, nullptr, RunLater::start_run_later, this);
}

RunLater::~RunLater() noexcept {
    should_stop_ = true;

    Pthread_mutex_lock(&m_);
    // death pill: None payload (former nullptr) signals run_later_loop
    // to exit on dequeue.
    jobs_.push(job_t(0.0, rusty::None));
    std::push_heap(jobs_.begin(), jobs_.end(), GreaterByJobTime{});
    Pthread_cond_signal(&cv_);
    Pthread_mutex_unlock(&m_);

    Pthread_join(th_, nullptr);
    Pthread_mutex_destroy(&m_);
    Pthread_cond_destroy(&cv_);
}

// @unsafe - rusty-cpp false positives: now_f is initialized, job_func is moved out before dereference
void RunLater::try_one_job() {
    // @unsafe - pthread mutex operations
    { Pthread_mutex_lock(&m_); }
    if (!jobs_.is_empty()) {
        // Peek the time without copying the (move-only) Option payload.
        auto job_time = jobs_.front().first;

        struct timeval now;
        // @unsafe - gettimeofday uses address-of
        { gettimeofday(&now, nullptr); }
        double now_f = now.tv_sec + now.tv_usec / 1000.0 / 1000.0;
        double wait = job_time - now_f;
        if (wait < 0.0) {
            // Move the function out before pop_heap shuffles the vector;
            // pop_heap then leaves the (now-empty) Option at the back, and
            // jobs_.pop() removes that back slot.
            auto job_func = std::move(jobs_.front().second);
            // @unsafe - heap operations over internal job vector
            {
                std::pop_heap(jobs_.begin(), jobs_.end(), GreaterByJobTime{});
                (void)jobs_.pop();
            }
            if (job_func.is_none()) {
                // death pill
                // @unsafe
                { Pthread_mutex_unlock(&m_); }
                return;
            } else {
                // @unsafe - move Box out of Option and invoke
                {
                    auto box = std::move(job_func).unwrap();
                    (*box)();
                    // box drops at end of scope, freeing the Function
                }
            }
        } else {
            // @unsafe - wait for the time to execute a job (C-style casts, pthread calls)
            {
                struct timespec abstime;
                int wait_sec = (int) wait;
                int wait_nsec = (int) ((wait - wait_sec) * 1000.0 * 1000.0 * 1000.0);
                abstime.tv_sec = now.tv_sec;
                abstime.tv_nsec = now.tv_usec * 1000 + wait_nsec;
                if (abstime.tv_nsec > 1000 * 1000 * 1000) {
                    abstime.tv_sec += 1;
                    abstime.tv_nsec -= 1000 * 1000 * 1000;
                }
                int ret = pthread_cond_timedwait(&cv_, &m_, &abstime);
                verify(ret == ETIMEDOUT || ret == 0);
            }
        }
    } else {
        // wait for inserting a new job
        // @unsafe
        { Pthread_cond_wait(&cv_, &m_); }
    }
    // @unsafe
    { Pthread_mutex_unlock(&m_); }
}

void RunLater::run_later_loop() {
    while (!should_stop_) {
        try_one_job();
    }

    bool done = false;
    while (!done) {
        Pthread_mutex_lock(&m_);
        if (jobs_.is_empty()) {
            done = true;
        }
        Pthread_mutex_unlock(&m_);
        if (!done) {
            try_one_job();
        }
    }
}

int RunLater::run_later(double sec, rusty::Function<void()> f) {
    if (should_stop_) {
        return EPERM;
    }

    struct timeval now;
    gettimeofday(&now, nullptr);
    double later = now.tv_sec + now.tv_usec / 1000.0 / 1000.0;
    if (sec > 0.0) {
        later += sec;
    }

    latest_l_.lock();
    if (later > latest_) {
        latest_ = later;
    }
    latest_l_.unlock();

    Pthread_mutex_lock(&m_);
    jobs_.push(job_t(later, rusty::Some(rusty::make_box<rusty::Function<void()>>(std::move(f)))));
    std::push_heap(jobs_.begin(), jobs_.end(), GreaterByJobTime{});
    Pthread_cond_signal(&cv_);
    Pthread_mutex_unlock(&m_);

    return 0;
}

double RunLater::max_wait() const {
    struct timeval now;
    gettimeofday(&now, nullptr);
    double now_f = now.tv_sec + now.tv_usec / 1000.0 / 1000.0;
    return std::max(0.0, latest_ - now_f);
}


} // namespace rrr
