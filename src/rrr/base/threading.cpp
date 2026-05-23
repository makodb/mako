module;

#include <rusty/rusty.hpp>
#include <rusty/result.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>

#include <pthread.h>

export module rrr.threading;

import std;
import rrr.basetypes;  // for NoCopy
import rrr.debugging;  // for verify

// @safe
export namespace rrr {


// The Pthread_* wrappers below pass through a raw pointer (provided
// by the caller) to a libc pthread_* function and `verify()` the
// return code. The libc call itself isn't borrow-checked, so each
// wrapper is `@safe` with the single libc call wrapped in an inline
// `@unsafe` block.

// @safe
inline void Pthread_spin_init(pthread_spinlock_t* lock, int pshared) {
    // @unsafe { libc pthread_spin_init }
    { verify(pthread_spin_init(lock, pshared) == 0); }
}

// @safe
inline void Pthread_spin_lock(pthread_spinlock_t* lock) {
    // @unsafe { libc pthread_spin_lock }
    { verify(pthread_spin_lock(lock) == 0); }
}

// @safe
inline void Pthread_spin_unlock(pthread_spinlock_t* lock) {
    // @unsafe { libc pthread_spin_unlock }
    { verify(pthread_spin_unlock(lock) == 0); }
}

// @safe
inline void Pthread_spin_destroy(pthread_spinlock_t* lock) {
    // @unsafe { libc pthread_spin_destroy }
    { verify(pthread_spin_destroy(lock) == 0); }
}

// @safe
inline void Pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    // @unsafe { libc pthread_mutex_init }
    { verify(pthread_mutex_init(mutex, attr) == 0); }
}

// @safe
inline void Pthread_mutex_lock(pthread_mutex_t* mutex) {
    // @unsafe { libc pthread_mutex_lock }
    { verify(pthread_mutex_lock(mutex) == 0); }
}

// @safe
inline void Pthread_mutex_unlock(pthread_mutex_t* mutex) {
    // @unsafe { libc pthread_mutex_unlock }
    { verify(pthread_mutex_unlock(mutex) == 0); }
}

// @safe
inline void Pthread_mutex_destroy(pthread_mutex_t* mutex) {
    // @unsafe { libc pthread_mutex_destroy }
    { verify(pthread_mutex_destroy(mutex) == 0); }
}

// @safe
inline void Pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    // @unsafe { libc pthread_cond_init }
    { verify(pthread_cond_init(cond, attr) == 0); }
}

// @safe
inline void Pthread_cond_destroy(pthread_cond_t* cond) {
    // @unsafe { libc pthread_cond_destroy }
    { verify(pthread_cond_destroy(cond) == 0); }
}

// @safe
inline void Pthread_cond_signal(pthread_cond_t* cond) {
    // @unsafe { libc pthread_cond_signal }
    { verify(pthread_cond_signal(cond) == 0); }
}

// @safe
inline void Pthread_cond_broadcast(pthread_cond_t* cond) {
    // @unsafe { libc pthread_cond_broadcast }
    { verify(pthread_cond_broadcast(cond) == 0); }
}

// @safe
inline void Pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    // @unsafe { libc pthread_cond_wait }
    { verify(pthread_cond_wait(cond, mutex) == 0); }
}

// @safe
inline void Pthread_create(pthread_t* thread,
                           const pthread_attr_t* attr,
                           void* (*func)(void*),
                           void* arg) {
    // @unsafe { libc pthread_create + raw function pointer }
    { verify(pthread_create(thread, attr, func, arg) == 0); }
}

// @safe
inline void Pthread_join(pthread_t thread, void** value_ptr) {
    // @unsafe { libc pthread_join + void** out-parameter }
    { verify(pthread_join(thread, value_ptr) == 0); }
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

    // @safe - parity with Rust's `Mutex::lock`. The atomic compare/exchange
    // and load operations are memory-safe; the sleeping fallback path
    // delegates to `rusty::sys::time::sleep_us` (itself @safe with an
    // inner @unsafe block around nanosleep).
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

        // Fall back to sleeping if still contended.
        expected = false;
        while (!locked_.compare_exchange_weak(expected, true,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            rusty::sys::time::sleep_us(50);  // 50 microseconds
            expected = false;
        }
    }

    // @safe - parity with Rust's `Mutex` drop / `unlock`. `std::atomic::store`
    // is memory-safe; the prior `@unsafe` annotation was over-conservative.
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

} // export namespace rrr
