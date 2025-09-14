#pragma once

#include <list>
#include <queue>
#include <functional>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

#include "basetypes.hpp"
#include "misc.hpp"

// External safety annotations for pthread functions used in this module
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
//   nanosleep: [safe, (const struct timespec*, struct timespec*) -> int]
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

// @safe - Thread-safe spinlock using atomic operations
class SpinLock: public Lockable {
public:
    // @safe - Initializes to unlocked state
    SpinLock(): locked_(false) { }
    
    // @unsafe - Uses address-of operator for nanosleep call
    // SAFETY: Only takes address of stack-allocated timespec which remains valid
    void lock();
    
    // @safe - Releases lock atomically
    void unlock() {
        locked_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> locked_ alignas(64);  // Cache-line aligned to prevent false sharing
};

// @safe - Spin-based condition variable using atomic flag
class SpinCondVar: public NoCopy {
private:
    std::atomic<int> flag_{0};

public:
    // @safe - Default constructor
    SpinCondVar() = default;
    
    // @safe - Default destructor
    ~SpinCondVar() = default;

    // @safe - Wait on condition, releasing and reacquiring lock
    void wait(SpinLock& sl) {
        flag_.store(0, std::memory_order_relaxed);
        sl.unlock(); 

        while(flag_.load(std::memory_order_acquire) == 0) {
            Time::sleep(10);
        }
        sl.lock();
    }

    // @safe - Timed wait with timeout
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

    // @safe - Signal one waiter
    void signal() {
        flag_.store(1, std::memory_order_release);
    }

    // @safe - Broadcast to all waiters
    void bcast() {
        flag_.store(1, std::memory_order_release);
    }
};

//#define ALL_SPIN_LOCK

#ifdef ALL_SPIN_LOCK

#define Mutex SpinLock
#define CondVar SpinCondVar

#else

// @unsafe - Uses raw pthread mutex operations for performance
// SAFETY: All operations are thread-safe when used correctly
class Mutex: public Lockable {
public:
    // @unsafe - Initializes pthread mutex
    Mutex() : m_() {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
        Pthread_mutex_init(&m_, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    
    // @unsafe - Destroys pthread mutex
    ~Mutex() {
        Pthread_mutex_destroy(&m_);
    }

    // @unsafe - Acquires mutex lock
    void lock() {
        Pthread_mutex_lock(&m_);
    }
    
    // @unsafe - Releases mutex lock
    void unlock() {
        Pthread_mutex_unlock(&m_);
    }

private:
    friend class CondVar;
    pthread_mutex_t m_;
};

// choice between spinlock & mutex:
// * when n_thread > n_core, use mutex
// * on virtual machines, use mutex

// @unsafe - Uses raw pthread condition variable for performance
// SAFETY: All operations are thread-safe when used with proper mutex
class CondVar: public NoCopy {
public:
    // @unsafe - Initializes pthread condition variable
    CondVar() : cv_() {
        Pthread_cond_init(&cv_, nullptr);
    }
    
    // @unsafe - Destroys pthread condition variable
    ~CondVar() {
        Pthread_cond_destroy(&cv_);
    }

    // @unsafe - Waits on condition variable with mutex
    void wait(Mutex& m) {
        Pthread_cond_wait(&cv_, &m.m_);
    }
    
    // @unsafe - Signals one waiting thread
    void signal() {
        Pthread_cond_signal(&cv_);
    }
    
    // @unsafe - Broadcasts to all waiting threads
    void bcast() {
        Pthread_cond_broadcast(&cv_);
    }

    // @unsafe - Timed wait with timeout
    int timed_wait(Mutex& m, double sec);

private:
    pthread_cond_t cv_;
};

#endif // ALL_SPIN_LOCK

// @safe - RAII lock guard that ensures proper lock/unlock
class ScopedLock: public NoCopy {
public:
    // @safe - Acquires lock on construction
    explicit ScopedLock(Lockable* lock): m_(lock) { 
        if (m_) m_->lock(); 
    }
    
    // @safe - Acquires lock on construction (reference version)
    explicit ScopedLock(Lockable& lock): m_(&lock) { 
        m_->lock(); 
    }
    
    // @safe - Releases lock on destruction
    ~ScopedLock() { 
        if (m_) m_->unlock(); 
    }
    
private:
    Lockable* m_;
};


/**
 * Thread safe queue using unique_ptr for automatic memory management.
 * @unsafe - Uses raw pthread primitives for performance
 * SAFETY: All public methods are thread-safe through mutex protection
 */
template<class T>
class Queue: public NoCopy {
    std::unique_ptr<std::list<T>> q_;
    pthread_cond_t not_empty_;
    pthread_mutex_t m_;

public:
    // @unsafe - Initializes pthread primitives
    Queue(): q_(std::make_unique<std::list<T>>()), not_empty_(), m_() {
        Pthread_mutex_init(&m_, nullptr);
        Pthread_cond_init(&not_empty_, nullptr);
    }

    // @unsafe - Destroys pthread primitives
    ~Queue() {
        Pthread_cond_destroy(&not_empty_);
        Pthread_mutex_destroy(&m_);
        // q_ automatically deleted by unique_ptr
    }

    // @unsafe - Thread-safe push with mutex protection
    void push(const T& e) {
        Pthread_mutex_lock(&m_);
        q_->push_back(e);
        Pthread_cond_signal(&not_empty_);
        Pthread_mutex_unlock(&m_);
    }

    // @unsafe - Thread-safe try_pop with mutex protection
    bool try_pop(T* t) {
        bool ret = false;
        Pthread_mutex_lock(&m_);
        if (!q_->empty()) {
            ret = true;
            *t = q_->front();
            q_->pop_front();
        }
        Pthread_mutex_unlock(&m_);
        return ret;
    }

    // @unsafe - Thread-safe try_pop with ignore value
    bool try_pop_but_ignore(T* t, const T& ignore) {
        bool ret = false;
        Pthread_mutex_lock(&m_);
        if (!q_->empty() && q_->front() != ignore) {
            ret = true;
            *t = q_->front();
            q_->pop_front();
        }
        Pthread_mutex_unlock(&m_);
        return ret;
    }

    // @unsafe - Thread-safe blocking pop
    T pop() {
        Pthread_mutex_lock(&m_);
        while (q_->empty()) {
            Pthread_cond_wait(&not_empty_, &m_);
        }
        T e = q_->front();
        q_->pop_front();
        Pthread_mutex_unlock(&m_);
        return e;
    }
};

class ThreadPool: public RefCounted {
    int n_;
    Counter round_robin_;
    pthread_t* th_;
    Queue<std::function<void()>*>* q_;
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
