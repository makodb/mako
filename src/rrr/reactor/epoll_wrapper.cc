module;

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>
// Reachability: this file's GEN names rusty::as_mut_ptr and
// rusty::sync::atomic::AtomicI32.
#include <rusty/array.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/os/fd.hpp>

#include <unistd.h>
#include <strings.h>

#include <sys/epoll.h>

export module rrr.epoll_wrapper;

import std;
import rusty;
import rrr.debugging;

// @safe - epoll wrapper. The Pollable virtual interface has no
// bodies, PollMode/PollReady are constexpr int sets, and Epoll owns the
// epoll poll fd. Epoll is authored as an inline-rust DSL struct
// (impl Drop closes the fd; a `rusty::Cell<bool>` field makes it
// move-only so the fd is never double-closed). Every epoll
// syscall body lives in the per-platform module implementation unit
// (epoll_platform_linux.cc, all-DSL).
// selected by CMake — Rust std's sys-module pattern. Only the Wait<F>
// template below keeps an in-interface #ifdef (templates cannot move
// to an implementation unit). The close is the OwnedFd RAII drop.
export namespace rrr {

// `PollMode` / `PollReady` — the two constant sets the poll layer speaks
// in. Authored as inline Rust DSL: `pub const NAME: i32 = ..` lowers to
// `constexpr int32_t NAME = ..`. The `namespace X {` braces stay
// hand-written scaffolding — a DSL block emits at the enclosing scope
// and cannot open a namespace.
//
// The emitted constants drop the `inline` the hand-written ones carried.
// That is safe here because a const variable in the purview of a module
// interface unit has MODULE linkage, not internal: the single definition
// below stays ODR-usable from importers (checked with an address-of and
// a const-ref bind from an importing TU — both compile and link).
namespace PollMode {
#if RUSTYCPP_RUST
pub const READ: i32 = 0x1;
pub const WRITE: i32 = 0x2;
pub const NO_CHANGE: i32 = -1;
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.1 version=1 rust_sha256=10300b1d677ba058fca4724f50a836dc6e1c756053b46495708faa947f482fd3*/
constexpr int32_t READ = static_cast<int32_t>(1);
constexpr int32_t WRITE = static_cast<int32_t>(2);
constexpr int32_t NO_CHANGE = -1;
/*RUSTYCPP:GEN-END id=epoll_wrapper.1*/
}

namespace PollReady {
#if RUSTYCPP_RUST
pub const READABLE: i32 = 0x1;
pub const WRITABLE: i32 = 0x2;
pub const ERROR: i32 = 0x4;
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.3 version=1 rust_sha256=176833170f7e300660bf046252bb5f12eb06047679378b3a54ac9c47ac77212a*/
constexpr int32_t READABLE = static_cast<int32_t>(1);
constexpr int32_t WRITABLE = static_cast<int32_t>(2);
constexpr int32_t ERROR = static_cast<int32_t>(4);
/*RUSTYCPP:GEN-END id=epoll_wrapper.3*/
}

// `Pollable` — abstract base for things that the epoll wrapper
// polls (concrete subclasses live in tests; production code uses the
// higher-level `PollableBase` trait). Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block as a virtual `class Pollable`. Same pattern as SinkBase /
// SourceBase / Job / Service.
#if RUSTYCPP_RUST
pub trait Pollable {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&mut self) -> usize;
    fn handle_read(&mut self) -> bool;
    fn handle_write(&mut self) -> i32;
    fn handle_error(&mut self);
    fn close(&mut self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.pollable version=1 rust_sha256=b44240c03518247bd1a865f3626ed5a43ffc80c6c0b604e97608cc215ebbce7c*/
class Pollable;

class Pollable {
public:
    virtual ~Pollable() noexcept(false) {}
    virtual int32_t fd() const = 0;
    virtual int32_t poll_mode() const = 0;
    virtual size_t content_size() = 0;
    virtual bool handle_read() = 0;
    virtual int32_t handle_write() = 0;
    virtual void handle_error() = 0;
    virtual void close() = 0;
    virtual bool check_pending_write_update() const = 0;
    virtual bool is_closed() const = 0;
    Pollable(const Pollable&) = delete;
    Pollable& operator=(const Pollable&) = delete;
    Pollable(Pollable&&) = delete;
    Pollable& operator=(Pollable&&) = delete;
protected:
    Pollable() = default;
};

template <class U> class PollableAdapter;
template <class U> class PollableAdapterRef;
template <class U> class PollableAdapterRefMut;
/*RUSTYCPP:GEN-END id=epoll_wrapper.pollable*/


// Global counter of Epoll::Remove calls — test instrumentation (was the
// static member `Epoll::remove_count_`; hoisted to module scope because the
// DSL emits instance fields only). Reset/read by test_reactor.cc, bumped
// by the platform remove body.
//
// Authored as inline Rust DSL: a module-scope `static NAME: T = ..` lowers
// to an `extern` declaration plus the `inline` definition — the DSL
// spelling of the old `inline std::atomic<int>`. Retyping to
// rusty::sync::atomic::AtomicI32 leaves `.load()` unchanged (its Ordering
// argument defaults to SeqCst) but drops plain assignment, so the reset in
// test_reactor.cc spells `.store(0)`.
#if RUSTYCPP_RUST
static epoll_remove_count: rusty::sync::atomic::AtomicI32 = rusty::sync::atomic::AtomicI32::new(0);
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.4 version=1 rust_sha256=e4c807ae4be400ab2cb3843489c041e410e135dbbb3845bb475dd4c6f9ee01a4*/
extern rusty::sync::atomic::AtomicI32 epoll_remove_count;

inline rusty::sync::atomic::AtomicI32 epoll_remove_count = rusty::sync::atomic::AtomicI32::new_(0);
/*RUSTYCPP:GEN-END id=epoll_wrapper.4*/

// === platform syscall entry points ===
// Declarations only — the bodies live in the CMake-selected platform
// implementation unit (see the file-header note).

// Platform syscall bodies live in the per-platform module
// implementation units (epoll_platform_linux.cc — DSL — or
// epoll_platform_linux.cc), selected by CMake. This is Rust std's
// sys-module pattern: no preprocessor split in the shared interface.
int32_t epoll_open();
int epoll_add_impl(int32_t poll_fd, int fd, int poll_mode);
int epoll_remove_impl(int32_t poll_fd, int fd);
int epoll_update_impl(int32_t poll_fd, int fd, int new_mode, int old_mode);

// @unsafe - shared remove-counter bump (test instrumentation), callable
// from the DSL bodies in the implementation units. No longer `inline`:
// a DSL fn emits an ordinary module-attached definition here, which the
// platform implementation unit calls across the TU boundary (Remove is a
// teardown path, not a hot one).
#if RUSTYCPP_RUST
fn epoll_bump_remove_count() {
    epoll_remove_count.fetch_add(1, rusty::sync::atomic::Ordering::SeqCst);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.5 version=1 rust_sha256=d04e5fafad102048d61ab66e46867c90a80863cea61db3c2c110c3061e4e73a0*/
void epoll_bump_remove_count();

void epoll_bump_remove_count() {
    epoll_remove_count.fetch_add(1, rusty::sync::atomic::Ordering::SeqCst);
}
/*RUSTYCPP:GEN-END id=epoll_wrapper.5*/





// `Epoll` — owns the epoll poll fd. Authored as inline-rust DSL: the
// `#if RUSTYCPP_RUST` block is the source of truth; the transpiler regenerates
// the `RUSTYCPP:GEN-BEGIN ... END` C++ below it. The poll fd is a
// std-faithful rusty::os::fd::OwnedFd — RAII close on drop, move-only —
// which subsumes the former impl Drop and Cell<bool> copy marker. The
// methods delegate to the `@unsafe` syscall free functions above.
// `Wait<F>` regenerates as a real C++ template member. (Dropped vs the old class: the unused
// `volatile bool* pause/stop` back-pointers, the six dead stat counters, and
// the never-called nullary `Wait()` overload.)

// `epoll_wait_impl<F>` — one poll pass: drain the ready set and hand each
// (fd, ready_events) pair to the caller's handler. It has to stay in the
// module INTERFACE — it is a template, and templates cannot move to an
// implementation unit — so this is the one place where the platform
// #ifdef survives the sys-module split.
//
// The Linux branch is inline-Rust DSL. The old "KERNEL: platform #ifdef +
// syscall + hot-path template dispatch" verdict was wrong on all three
// counts: only ONE branch is platform-specific (so the #ifdef moves out
// of the body and wraps two definitions); a syscall is an ordinary C call
// from a DSL body, exactly as every body in epoll_platform_linux.cc
// already does; and a DSL `fn f<F>(..)` emits a REAL `template<typename
// F>` — no rusty::Function, no type erasure, no indirect call, so the
// single hot call site (reactor.cpp's poll loop) still inlines the
// handler.
//
// @unsafe - epoll_wait blocking syscall, called route-2 (`unsafe { .. }`)
// from the DSL body below.
//
// Lowering notes (probed, then runtime-checked against a real epoll fd):
//   * `[epoll_event; 100]` lowers to `std::array<epoll_event, 100>`, and
//     `Default::default()` in typed-let position to
//     `rusty::default_like<..>()` — so the 100-entry event buffer needs
//     no zeroing kernel. `evlist.as_mut_ptr()` lowers to the free fn
//     `rusty::as_mut_ptr(evlist)`, which is why the GMF includes
//     <rusty/array.hpp> (a GMF must include what its own GEN names).
//     Indexing lowers to bounds-checked `.at(idx)`: one compare per ready
//     event, against a blocking syscall — not a hot-path concern.
//   * the loop counter MUST stay `i32`, matching epoll_wait's SIGNED
//     return. With a `usize` counter and `nev as usize`, the error return
//     (-1 on EINTR/EBADF) becomes SIZE_MAX and the loop spins forever
//     over uninitialised events; the signed form reproduces the old
//     `for (int i = 0; i < nev; i++)` exactly. Verified: nev < 0 returns
//     with zero handler calls.
//   * 100 is spelled twice (array extent and epoll_wait's maxevents)
//     because a DSL array extent must be a literal; it replaces the old
//     `const int max_nev = 100`.
#if RUSTYCPP_RUST
fn epoll_wait_impl<F>(poll_fd: i32, on_ready: F) {
    let mut evlist: [epoll_event; 100] = Default::default();
    let nev: i32 = unsafe { epoll_wait(poll_fd, evlist.as_mut_ptr(), 100, 1) };
    let mut i: i32 = 0;
    while i < nev {
        let idx: usize = i as usize;
        let events: u32 = evlist[idx].events;
        let mut ready_events: i32 = 0;
        if (events & EPOLLIN) != 0 {
            ready_events |= PollReady::READABLE;
        }
        if (events & EPOLLOUT) != 0 {
            ready_events |= PollReady::WRITABLE;
        }
        if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0 {
            ready_events |= PollReady::ERROR;
        }
        if ready_events != 0 {
            on_ready(evlist[idx].data.fd, ready_events);
        }
        i += 1;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.2 version=1 rust_sha256=fd694ed9769ec0f5b512fde118db7233d7049837c483c8e66256eac9a4a328d9*/
template<typename F>
void epoll_wait_impl(int32_t poll_fd, F on_ready);

template<typename F>
void epoll_wait_impl(int32_t poll_fd, F on_ready) {
    std::array<epoll_event, 100> evlist = rusty::default_like<std::array<epoll_event, 100>>();
    const int32_t nev = epoll_wait(std::move(poll_fd), rusty::as_mut_ptr(evlist), 100, 1);
    int32_t i = static_cast<int32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(nev)) {
        const size_t idx = static_cast<size_t>(i);
        const uint32_t events = evlist.at(idx).events;
        int32_t ready_events = static_cast<int32_t>(0);
        if (((rusty::detail::deref_if_pointer_like(events) & rusty::detail::deref_if_pointer_like(EPOLLIN))) != static_cast<uint32_t>(0)) {
            ready_events |= PollReady::READABLE;
        }
        if (((rusty::detail::deref_if_pointer_like(events) & rusty::detail::deref_if_pointer_like(EPOLLOUT))) != static_cast<uint32_t>(0)) {
            ready_events |= PollReady::WRITABLE;
        }
        if (((rusty::detail::deref_if_pointer_like(events) & (((rusty::detail::deref_if_pointer_like(EPOLLERR) | rusty::detail::deref_if_pointer_like(EPOLLHUP)) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP))))) != static_cast<uint32_t>(0)) {
            ready_events |= PollReady::ERROR;
        }
        if (rusty::detail::deref_if_pointer_like(ready_events) != static_cast<int32_t>(0)) {
            on_ready([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.fd); }) { return (__r.fd); } else if constexpr (requires { (__r.fd_field); }) { return (__r.fd_field); } else if constexpr (requires { ((*__r).fd); }) { return ((*__r).fd); } else { return ((*__r).fd_field); } }(evlist.at(idx).data), std::move(ready_events));
        }
        i += 1;
    }
}
/*RUSTYCPP:GEN-END id=epoll_wrapper.2*/

//
// @safe - see comment above.
#if RUSTYCPP_RUST
struct Epoll {
    // std-faithful RAII fd (rusty::os::fd::OwnedFd, mirroring Rust's
    // std::os::fd::OwnedFd): closes on drop, move-only — this subsumes
    // both the old impl Drop (epoll_close) and the Cell<bool> copy
    // marker.
    poll_fd_: rusty::os::fd::OwnedFd,
}

impl Epoll {
    // Allocates the poll fd up front (matches the old default ctor).
    #[cpp_ctor]
    fn new() -> Epoll {
        Epoll { poll_fd_: rusty::os::fd::OwnedFd::from_raw_fd(epoll_open()) }
    }
    fn fd(&self) -> i32 {
        self.poll_fd_.as_raw_fd()
    }
    fn Add(&mut self, fd: i32, poll_mode: i32) -> i32 {
        epoll_add_impl(self.poll_fd_.as_raw_fd(), fd, poll_mode)
    }
    fn Remove(&mut self, fd: i32) -> i32 {
        epoll_remove_impl(self.poll_fd_.as_raw_fd(), fd)
    }
    fn Update(&mut self, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
        epoll_update_impl(self.poll_fd_.as_raw_fd(), fd, new_mode, old_mode)
    }
    fn Wait<F>(&mut self, on_ready: F) {
        epoll_wait_impl(self.poll_fd_.as_raw_fd(), on_ready);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.epoll version=1 rust_sha256=f4b6bc3a41813a441bf1df8473e6f1e0303aad2ac21c123b2caf993014ad61ac*/
struct Epoll;

struct Epoll {
    rusty::os::fd::OwnedFd poll_fd_;

    Epoll();
    int32_t fd() const;
    int32_t Add(int32_t fd, int32_t poll_mode);
    int32_t Remove(int32_t fd);
    int32_t Update(int32_t fd, int32_t new_mode, int32_t old_mode);
    template<typename F>
    void Wait(F on_ready);
};


Epoll::Epoll()
    : poll_fd_(rusty::os::fd::OwnedFd::from_raw_fd(epoll_open()))
{}

int32_t Epoll::fd() const {
    return this->poll_fd_.as_raw_fd();
}

int32_t Epoll::Add(int32_t fd, int32_t poll_mode) {
    return epoll_add_impl(this->poll_fd_.as_raw_fd(), std::move(fd), std::move(poll_mode));
}

int32_t Epoll::Remove(int32_t fd) {
    return epoll_remove_impl(this->poll_fd_.as_raw_fd(), std::move(fd));
}

int32_t Epoll::Update(int32_t fd, int32_t new_mode, int32_t old_mode) {
    return epoll_update_impl(this->poll_fd_.as_raw_fd(), std::move(fd), std::move(new_mode), std::move(old_mode));
}

template<typename F>
void Epoll::Wait(F on_ready) {
    epoll_wait_impl(this->poll_fd_.as_raw_fd(), std::move(on_ready));
}
/*RUSTYCPP:GEN-END id=epoll_wrapper.epoll*/

} // export namespace rrr
