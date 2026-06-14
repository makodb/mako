module;

#include <rusty/rusty.hpp>
#include <rusty/result.hpp>
#include <rusty/option.hpp>
#include <rusty/sync/atomic.hpp>
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

// Pthread_spin_* wrappers around libc pthread spin primitives. Each
// passes the caller-owned `pthread_spinlock_t*` through to the matching
// libc call and verifies the return code.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn Pthread_spin_init(lock: *mut pthread_spinlock_t, pshared: i32) {
    // @unsafe { libc pthread_spin_init }
    { verify(pthread_spin_init(lock, pshared) == 0); }
}

fn Pthread_spin_lock(lock: *mut pthread_spinlock_t) {
    // @unsafe { libc pthread_spin_lock }
    { verify(pthread_spin_lock(lock) == 0); }
}

fn Pthread_spin_unlock(lock: *mut pthread_spinlock_t) {
    // @unsafe { libc pthread_spin_unlock }
    { verify(pthread_spin_unlock(lock) == 0); }
}

fn Pthread_spin_destroy(lock: *mut pthread_spinlock_t) {
    // @unsafe { libc pthread_spin_destroy }
    { verify(pthread_spin_destroy(lock) == 0); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=threading.pthread_spin version=1 rust_sha256=2beea7b64b35e14d0fc3301e572e477dc6f5792d62795118122329b24336b3be*/
void Pthread_spin_init(pthread_spinlock_t* lock, int32_t pshared);
void Pthread_spin_lock(pthread_spinlock_t* lock);
void Pthread_spin_unlock(pthread_spinlock_t* lock);
void Pthread_spin_destroy(pthread_spinlock_t* lock);

void Pthread_spin_init(pthread_spinlock_t* lock, int32_t pshared) {
    {
        verify(pthread_spin_init(lock, std::move(pshared)) == 0);
    }
}

void Pthread_spin_lock(pthread_spinlock_t* lock) {
    {
        verify(pthread_spin_lock(lock) == 0);
    }
}

void Pthread_spin_unlock(pthread_spinlock_t* lock) {
    {
        verify(pthread_spin_unlock(lock) == 0);
    }
}

void Pthread_spin_destroy(pthread_spinlock_t* lock) {
    {
        verify(pthread_spin_destroy(lock) == 0);
    }
}
/*RUSTYCPP:GEN-END id=threading.pthread_spin*/

// Pthread_mutex_* / Pthread_cond_* wrappers around libc pthread mutex
// + condvar primitives. Each forwards the caller-owned raw pointers
// through to the matching libc call and verifies the return code.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn Pthread_mutex_init(mutex: *mut pthread_mutex_t, attr: *const pthread_mutexattr_t) {
    // @unsafe { libc pthread_mutex_init }
    { verify(pthread_mutex_init(mutex, attr) == 0); }
}

fn Pthread_mutex_lock(mutex: *mut pthread_mutex_t) {
    // @unsafe { libc pthread_mutex_lock }
    { verify(pthread_mutex_lock(mutex) == 0); }
}

fn Pthread_mutex_unlock(mutex: *mut pthread_mutex_t) {
    // @unsafe { libc pthread_mutex_unlock }
    { verify(pthread_mutex_unlock(mutex) == 0); }
}

fn Pthread_mutex_destroy(mutex: *mut pthread_mutex_t) {
    // @unsafe { libc pthread_mutex_destroy }
    { verify(pthread_mutex_destroy(mutex) == 0); }
}

fn Pthread_cond_init(cond: *mut pthread_cond_t, attr: *const pthread_condattr_t) {
    // @unsafe { libc pthread_cond_init }
    { verify(pthread_cond_init(cond, attr) == 0); }
}

fn Pthread_cond_destroy(cond: *mut pthread_cond_t) {
    // @unsafe { libc pthread_cond_destroy }
    { verify(pthread_cond_destroy(cond) == 0); }
}

fn Pthread_cond_signal(cond: *mut pthread_cond_t) {
    // @unsafe { libc pthread_cond_signal }
    { verify(pthread_cond_signal(cond) == 0); }
}

fn Pthread_cond_broadcast(cond: *mut pthread_cond_t) {
    // @unsafe { libc pthread_cond_broadcast }
    { verify(pthread_cond_broadcast(cond) == 0); }
}

fn Pthread_cond_wait(cond: *mut pthread_cond_t, mutex: *mut pthread_mutex_t) {
    // @unsafe { libc pthread_cond_wait }
    { verify(pthread_cond_wait(cond, mutex) == 0); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=threading.pthread_mutex_cond version=1 rust_sha256=33a2388103ed79a770c9bb55bd8bac55a1e4472d1112dcae41064362726c8467*/
void Pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
void Pthread_mutex_lock(pthread_mutex_t* mutex);
void Pthread_mutex_unlock(pthread_mutex_t* mutex);
void Pthread_mutex_destroy(pthread_mutex_t* mutex);
void Pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
void Pthread_cond_destroy(pthread_cond_t* cond);
void Pthread_cond_signal(pthread_cond_t* cond);
void Pthread_cond_broadcast(pthread_cond_t* cond);
void Pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);

void Pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    {
        verify(pthread_mutex_init(mutex, attr) == 0);
    }
}

void Pthread_mutex_lock(pthread_mutex_t* mutex) {
    {
        verify(pthread_mutex_lock(mutex) == 0);
    }
}

void Pthread_mutex_unlock(pthread_mutex_t* mutex) {
    {
        verify(pthread_mutex_unlock(mutex) == 0);
    }
}

void Pthread_mutex_destroy(pthread_mutex_t* mutex) {
    {
        verify(pthread_mutex_destroy(mutex) == 0);
    }
}

void Pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    {
        verify(pthread_cond_init(cond, attr) == 0);
    }
}

void Pthread_cond_destroy(pthread_cond_t* cond) {
    {
        verify(pthread_cond_destroy(cond) == 0);
    }
}

void Pthread_cond_signal(pthread_cond_t* cond) {
    {
        verify(pthread_cond_signal(cond) == 0);
    }
}

void Pthread_cond_broadcast(pthread_cond_t* cond) {
    {
        verify(pthread_cond_broadcast(cond) == 0);
    }
}

void Pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    {
        verify(pthread_cond_wait(cond, mutex) == 0);
    }
}
/*RUSTYCPP:GEN-END id=threading.pthread_mutex_cond*/

// @safe
inline void Pthread_create(pthread_t* thread,
                           const pthread_attr_t* attr,
                           void* (*func)(void*),
                           void* arg) {
    // @unsafe { libc pthread_create + raw function pointer }
    { verify(pthread_create(thread, attr, func, arg) == 0); }
}

// @safe — Pthread_join stays outside the DSL because libc spells the
// out-parameter as `void**`, and the DSL has no syntax for `void` /
// `*mut c_void` (using `*mut *mut u8` emits `uint8_t**`, which libc
// rejects).
inline void Pthread_join(pthread_t thread, void** value_ptr) {
    // @unsafe { libc pthread_join + void** out-parameter }
    { verify(pthread_join(thread, value_ptr) == 0); }
}

// Bring `Ordering` and `AtomicBool` into the `rrr` namespace so DSL
// bodies can write `Ordering::Acquire` / `AtomicBool::new(...)` (Rust
// idiom) and the emitted C++ resolves via these using-decls.
using rusty::sync::atomic::Ordering;
using rusty::sync::atomic::AtomicBool;

// @safe - architecture-specific pause hint for spin loops. Defined
// outside the DSL because the DSL has no inline-asm or preprocessor
// support. The inline `@unsafe` block scopes the asm instruction.
inline void cpu_pause() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    // @unsafe { inline asm }
    { asm volatile("pause"); }
#elif defined(__aarch64__)
    // @unsafe { inline asm }
    { asm volatile("yield"); }
#endif
}

// `SpinLock` — atomic-flag busy-wait lock. The previous `Lockable`
// abstract base was deleted (no polymorphic callers in the tree);
// SpinLock is now a standalone DSL struct. The atomic flag's interior
// mutability lets `lock` / `unlock` take `&self`, so the emitted C++
// methods are `const`-qualified — the `mutable` qualifier on SpinLock
// fields in holder classes (SpinMutex<T>::lock_) becomes redundant.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. `fn new()` lowers to a static
// `SpinLock::new_()` factory.
//
// One C++ feature deliberately dropped vs. the pre-DSL form: the
// `alignas(64)` cache-line alignment on `locked_field`. The DSL does
// not yet emit field-level alignment attributes. The performance
// impact is small (false-sharing risk for SpinLocks colocated with
// other heavily-written data); fixable later by adding alignas
// support to the transpiler or wrapping in a CacheAligned<T> helper.
#if RUSTYCPP_RUST
struct SpinLock {
    locked_field: AtomicBool,
}

impl SpinLock {
    fn new() -> SpinLock {
        SpinLock { locked_field: AtomicBool::new(false) }
    }

    fn lock(&self) {
        if self.locked_field.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_ok() {
            return;
        }
        let mut wait: i32 = 1000i32;
        while wait > 0i32 && self.locked_field.load(Ordering::Relaxed) {
            cpu_pause();
            wait -= 1i32;
        }
        while self.locked_field.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
            rusty::sys::time::sleep_us(50u64);
        }
    }

    fn unlock(&self) {
        self.locked_field.store(false, Ordering::Release);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=threading.1 version=1 rust_sha256=8bff8c93678817029a24300bbe1f78054ca17c8c2557b461e0c1fa1e85a9aea4*/
struct SpinLock;

struct SpinLock {
    rusty::sync::atomic::AtomicBool locked_field;

    static SpinLock new_();
    void lock() const;
    void unlock() const;
};


SpinLock SpinLock::new_() {
    return SpinLock{.locked_field = AtomicBool::new_(false)};
}

void SpinLock::lock() const {
    if (this->locked_field.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_ok()) {
        return;
    }
    int32_t wait = static_cast<int32_t>(1000);
    while ((rusty::detail::deref_if_pointer_like(wait) > static_cast<int32_t>(0)) && this->locked_field.load(Ordering::Relaxed)) {
        cpu_pause();
        wait -= static_cast<int32_t>(1);
    }
    while (this->locked_field.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err()) {
        rusty::sys::time::sleep_us(static_cast<uint64_t>(50));
    }
}

void SpinLock::unlock() const {
    this->locked_field.store(false, Ordering::Release);
}
/*RUSTYCPP:GEN-END id=threading.1*/

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

    // Transparent forwarding so the inline-Rust DSL's container operations on a
    // guarded collection resolve through to the inner value: the DSL lowers
    // `guard.len()` -> `rusty::len(guard)`, `guard.contains(k)` ->
    // `rusty::contains(guard, k)`, and `guard[i]` -> `guard[i]`, none of which
    // the bare guard otherwise satisfies. Each is SFINAE-gated on the inner T
    // actually supporting the op, so a SpinMutexGuard over a non-container T is
    // unaffected. (Plain `guard.size()`/`guard.pop_front()`/etc. already work
    // through `operator->`.)
    // @safe
    template<typename U = T>
    auto len() const -> decltype(std::declval<const U&>().len()) { return (**this).len(); }
    // @safe
    template<typename K, typename U = T>
    auto contains(const K& key) const -> decltype(std::declval<const U&>().contains(key)) {
        return (**this).contains(key);
    }
    // @safe
    template<typename I, typename U = T>
    auto operator[](I index) -> decltype(std::declval<U&>()[index]) { return (**this)[index]; }
    // @safe
    template<typename I, typename U = T>
    auto operator[](I index) const -> decltype(std::declval<const U&>()[index]) {
        return (**this)[index];
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

    // @safe - Default constructor for default-constructible types.
    // SpinMutex now has an implicit move ctor (both SpinLock and
    // UnsafeCell<T> are movable as of the SpinLock atomic flip), so
    // value-returning `static new_(...)` factories below compile.
    SpinMutex() : data_() {}

    // @safe - Constructor initializes data
    explicit SpinMutex(T value) : data_(std::move(value)) {}

    // @safe - Rust-style factory matching `fn new() -> Self`. Equivalent
    // to default construction; provided for symmetry with the rest of
    // the rrr `new_()` rollout.
    static SpinMutex new_() {
        return SpinMutex{};
    }

    // @safe - Rust-style factory matching `fn new(value) -> Self`.
    static SpinMutex new_(T value) {
        return SpinMutex{std::move(value)};
    }

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
