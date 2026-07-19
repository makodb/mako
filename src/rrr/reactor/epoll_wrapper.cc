module;

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>
#include <rusty/slice.hpp>
#include <rusty/os/fd.hpp>

#include <unistd.h>
#include <strings.h>

#ifdef __APPLE__
#define USE_KQUEUE
#endif

#ifdef USE_KQUEUE
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

export module rrr.epoll_wrapper;

import std;
import rusty;
import rrr.debugging;

// @safe - kqueue/epoll wrapper. The Pollable virtual interface has no
// bodies, PollMode/PollReady are constexpr int sets, and Epoll owns the
// kqueue/epoll poll fd. Epoll is authored as an inline-rust DSL struct
// (impl Drop closes the fd; a `rusty::Cell<bool>` field makes it
// move-only so the fd is never double-closed). Every kqueue/epoll
// syscall body lives in the per-platform module implementation unit
// (epoll_platform_linux.cc, all-DSL / epoll_platform_kqueue.cc, C++),
// selected by CMake — Rust std's sys-module pattern. Only the Wait<F>
// template below keeps an in-interface #ifdef (templates cannot move
// to an implementation unit). The close is the OwnedFd RAII drop.
export namespace rrr {
using std::shared_ptr;

namespace PollMode {
    inline constexpr int READ = 0x1;
    inline constexpr int WRITE = 0x2;
    inline constexpr int NO_CHANGE = -1;
}

namespace PollReady {
    inline constexpr int READABLE = 0x1;
    inline constexpr int WRITABLE = 0x2;
    inline constexpr int ERROR = 0x4;
}

// `Pollable` — abstract base for things that the epoll/kqueue wrapper
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
// DSL emits instance fields only). Read/reset by test_reactor.cc and
// Reactor::get_remove_count().
inline std::atomic<int> epoll_remove_count{0};

// === platform syscall entry points ===
// Declarations only — the bodies live in the CMake-selected platform
// implementation unit (see the file-header note).

// Platform syscall bodies live in the per-platform module
// implementation units (epoll_platform_linux.cc — DSL — or
// epoll_platform_kqueue.cc), selected by CMake. This is Rust std's
// sys-module pattern: no preprocessor split in the shared interface.
int32_t epoll_open();
int epoll_add_impl(int32_t poll_fd, int fd, int poll_mode);
int epoll_remove_impl(int32_t poll_fd, int fd);
int epoll_update_impl(int32_t poll_fd, int fd, int new_mode, int old_mode);

// @unsafe - shared remove-counter bump (test instrumentation), callable
// from the DSL bodies in the implementation units.
inline void epoll_bump_remove_count() { epoll_remove_count++; }





// `Epoll` — owns the kqueue/epoll poll fd. Authored as inline-rust DSL: the
// `#if RUSTYCPP_RUST` block is the source of truth; the transpiler regenerates
// the `RUSTYCPP:GEN-BEGIN ... END` C++ below it. The poll fd is a
// std-faithful rusty::os::fd::OwnedFd — RAII close on drop, move-only —
// which subsumes the former impl Drop and Cell<bool> copy marker. The
// methods delegate to the `@unsafe` syscall free functions above (their
// `#ifdef USE_KQUEUE` bodies aren't DSL-expressible). `Wait<F>` regenerates as
// a real C++ template member. (Dropped vs the old class: the unused
// `volatile bool* pause/stop` back-pointers, the six dead stat counters, and
// the never-called nullary `Wait()` overload.)
// @unsafe - kevent / epoll_wait blocking syscall + raw `evlist[max_nev]`
// stack buffer + dispatch into the caller-supplied handler.
template<typename ReadyHandler>
inline void epoll_wait_impl(int32_t poll_fd, ReadyHandler on_ready) {
    const int max_nev = 100;
#ifdef USE_KQUEUE
    struct kevent evlist[max_nev];
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 1 * 1000 * 1000;

    int nev = kevent(poll_fd, nullptr, 0, evlist, max_nev, &timeout);

    for (int i = 0; i < nev; i++) {
      int ready_events = 0;
      if (evlist[i].filter == EVFILT_READ) {
        ready_events |= PollReady::READABLE;
      }
      if (evlist[i].filter == EVFILT_WRITE) {
        ready_events |= PollReady::WRITABLE;
      }
      if (evlist[i].flags & EV_EOF) {
        ready_events |= PollReady::ERROR;
      }
      if (ready_events != 0) {
        on_ready(static_cast<int>(evlist[i].ident), ready_events);
      }
    }

#else
    struct epoll_event evlist[max_nev];
    int timeout = 1;
    int nev = epoll_wait(poll_fd, evlist, max_nev, timeout);
    for (int i = 0; i < nev; i++) {
      int ready_events = 0;
      if (evlist[i].events & EPOLLIN) {
        ready_events |= PollReady::READABLE;
      }
      if (evlist[i].events & EPOLLOUT) {
        ready_events |= PollReady::WRITABLE;
      }
      if (evlist[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        ready_events |= PollReady::ERROR;
      }
      if (ready_events != 0) {
        on_ready(evlist[i].data.fd, ready_events);
      }
    }
#endif
}

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
