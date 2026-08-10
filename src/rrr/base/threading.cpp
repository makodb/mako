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

// Pthread_create/Pthread_join shims deleted (syscall plan item 3):
// every call site now uses the std-faithful rusty::thread::spawn /
// JoinHandle (which detaches on drop, join().unwrap() aborts on
// error — matching the old verify()==0 contract).

// Bring `Ordering` and `AtomicBool` into the `rrr` namespace so DSL
// bodies can write `Ordering::Acquire` / `AtomicBool::new(...)` (Rust
// idiom) and the emitted C++ resolves via these using-decls.
//
// Authored as DSL: a `use rusty::…;` used to be silently dropped (the
// transpiler classified the runtime as an unmapped external crate), so
// these had to be hand-written. Fixed upstream (rusty-cpp 3f7ca481).
// Explicit block id — auto-numbering would collide with `threading.1`.
#if RUSTYCPP_RUST
use rusty::sync::atomic::Ordering;
use rusty::sync::atomic::AtomicBool;
#endif
/*RUSTYCPP:GEN-BEGIN id=threading.atomic_usings version=1 rust_sha256=7c543022e137f9870611065121ad3bb2a8c4b9002bb2c45f359e223bc74aafc7*/
using rusty::sync::atomic::Ordering;

using rusty::sync::atomic::AtomicBool;
/*RUSTYCPP:GEN-END id=threading.atomic_usings*/

// @safe - architecture-specific pause hint for spin loops. The
// pause/yield instruction itself lives in srpc_timing.c (plain C,
// Goal-0 C demotion -- inline asm will never be Rust DSL); this is the
// one-line passthrough over it, authored as inline Rust DSL exactly
// like `get_ncpu` in base/misc.cpp. The `extern "C"` declaration
// stays hand-written C-bridge scaffolding and must precede the block.
//
// Two C++ qualifiers are dropped vs. the pre-DSL form: `inline` and
// `noexcept`. Neither costs anything here -- the only callers are
// SpinLock::lock/unlock below in this same TU, so the definition is
// still visible for inlining, and `srpc_cpu_pause` is C and cannot
// throw.
extern "C" void srpc_cpu_pause(void);

#if RUSTYCPP_RUST
fn cpu_pause() {
    unsafe { srpc_cpu_pause(); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=threading.4 version=1 rust_sha256=d81887ce42eb48b40001f9a99c4f48ab58a7ec9065063918af176390def6dbe1*/
void cpu_pause();

void cpu_pause() {
    // @unsafe
    {
        srpc_cpu_pause();
    }
}
/*RUSTYCPP:GEN-END id=threading.4*/

// `SpinLock` — atomic-flag busy-wait lock. The previous `Lockable`
// abstract base was deleted (no polymorphic callers in the tree);
// SpinLock is now a standalone DSL struct. The atomic flag's interior
// mutability lets `lock` / `unlock` take `&self`, so the emitted C++
// methods are `const`-qualified. (The former `SpinMutex<T>` wrapper was
// retired in favor of `rusty::Mutex<T>`; SpinLock remains as the raw
// low-latency primitive used directly by a few debug locks.)
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. `fn new()` lowers to a static
// `SpinLock::new_()` factory.
//
// One C++ feature deliberately dropped vs. the pre-DSL form: the
// `alignas(64)` cache-line alignment on `locked_field`. The DSL does
// not yet emit field-level alignment attributes. Fixable by adding
// alignas support to the transpiler (preferred — it is a translator
// gap, not a design choice) or by wrapping in a CacheAligned<T>.
//
// MEASURED EXPOSURE (2026-08-01): currently nil. SpinLock has exactly
// two live instances in the whole tree — RccTx::__debug_parents_lock_
// and RccTx::__debug_scc_lock_ (src/deptran/rcc/tx.h:26-27), both
// STATIC DEBUG locks, neither a field inside a hot struct. The
// false-sharing scenario this note warns about does not occur today,
// and no CacheAligned<T> helper exists anywhere in the tree.
//
// So: do not treat this as an outstanding performance bug. DO re-check
// it before putting a SpinLock inside a struct alongside
// heavily-written fields — that is the case where the lost alignment
// starts to cost something, and nothing will warn you.
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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

} // export namespace rrr
