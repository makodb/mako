module;

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>

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
// syscall (kevent, epoll_create/ctl/wait, ::close) carries a `#ifdef
// USE_KQUEUE` platform split that the DSL can't express inline, so the
// syscall bodies live in `// @unsafe` free functions (epoll_open /
// epoll_close / epoll_*_impl) that the DSL methods delegate to.
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

// === @unsafe kqueue/epoll syscall bodies (free functions) ===
// Each carries the `#ifdef USE_KQUEUE` platform split — which the Rust DSL
// cannot express inline — so the syscall bodies live here and the DSL Epoll
// methods below delegate to them (the AddrInfo free-function pattern).

// @unsafe - kqueue() / epoll_create syscall to allocate the poll fd.
inline int32_t epoll_open() {
    int32_t fd;
#ifdef USE_KQUEUE
    fd = kqueue();
#else
    fd = epoll_create(10);
#endif
    verify(fd != -1);
    return fd;
}

// @unsafe - ::close syscall on the owned poll fd (the Drop body).
inline void epoll_close(int32_t poll_fd) {
    if (poll_fd != -1) {
        ::close(poll_fd);
    }
}

// @unsafe - kevent / epoll_ctl(ADD) plumbing with bzero/memset and EEXIST retry.
inline int epoll_add_impl(int32_t poll_fd, int fd, int poll_mode) {
#ifdef USE_KQUEUE
    struct kevent ev;
    if (poll_mode & PollMode::READ) {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = EV_ADD;
      ev.filter = EVFILT_READ;
      verify(kevent(poll_fd, &ev, 1, nullptr, 0, nullptr) == 0);
    }
    if (poll_mode & PollMode::WRITE) {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = EV_ADD;
      ev.filter = EVFILT_WRITE;
      verify(kevent(poll_fd, &ev, 1, nullptr, 0, nullptr) == 0);
    }

#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.fd = fd;
    ev.events = EPOLLET | EPOLLIN | EPOLLRDHUP;

    if (poll_mode & PollMode::WRITE) {
        ev.events |= EPOLLOUT;
    }

    int result = epoll_ctl(poll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (result != 0 && errno == EEXIST) {
        (void)epoll_ctl(poll_fd, EPOLL_CTL_DEL, fd, nullptr);
        result = epoll_ctl(poll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
    if (result != 0 && errno == EBADF) {
        // The fd was closed between the registration request and this
        // epoll_ctl (teardown racing an accept/connect registration).
        // A closed fd can never produce events — report failure so the
        // caller can drop the pollable instead of aborting the process.
        return -1;
    }
    verify(result == 0);
#endif
    return 0;
}

// @unsafe - kevent / epoll_ctl(DEL) syscall + bzero/memset.
inline int epoll_remove_impl(int32_t poll_fd, int fd) {
    epoll_remove_count++;
#ifdef USE_KQUEUE
    struct kevent ev;

    bzero(&ev, sizeof(ev));
    ev.ident = fd;
    ev.flags = EV_DELETE;
    ev.filter = EVFILT_READ;
    kevent(poll_fd, &ev, 1, nullptr, 0, nullptr);
    bzero(&ev, sizeof(ev));
    ev.ident = fd;
    ev.flags = EV_DELETE;
    ev.filter = EVFILT_WRITE;
    kevent(poll_fd, &ev, 1, nullptr, 0, nullptr);

#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    epoll_ctl(poll_fd, EPOLL_CTL_DEL, fd, &ev);
#endif
    return 0;
}

// @unsafe - kevent / epoll_ctl(MOD) syscall, bzero/memset, EBADF/ENOENT tolerance.
inline int epoll_update_impl(int32_t poll_fd, int fd, int new_mode, int old_mode) {
#ifdef USE_KQUEUE
    struct kevent ev;
    auto kqueue_update = [&](int flags, int filter) -> bool {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = flags;
      ev.filter = filter;
      if (kevent(poll_fd, &ev, 1, nullptr, 0, nullptr) == 0) return true;
      int err = errno;
      return (err == EBADF || err == ENOENT);
    };
    if ((new_mode & PollMode::READ) && !(old_mode & PollMode::READ)) {
      verify(kqueue_update(EV_ADD, EVFILT_READ));
    }
    if (!(new_mode & PollMode::READ) && (old_mode & PollMode::READ)) {
      verify(kqueue_update(EV_DELETE, EVFILT_READ));
    }
    if ((new_mode & PollMode::WRITE) && !(old_mode & PollMode::WRITE)) {
      verify(kqueue_update(EV_ADD, EVFILT_WRITE));
    }
    if (!(new_mode & PollMode::WRITE) && (old_mode & PollMode::WRITE)) {
      verify(kqueue_update(EV_DELETE, EVFILT_WRITE));
    }
#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.fd = fd;
    ev.events = EPOLLET | EPOLLRDHUP;
    if (new_mode & PollMode::READ) {
        ev.events |= EPOLLIN;
    }
    if (new_mode & PollMode::WRITE) {
        ev.events |= EPOLLOUT;
    }
    int rc = epoll_ctl(poll_fd, EPOLL_CTL_MOD, fd, &ev);
    if (rc != 0) {
      int err = errno;
      if (err == ENOENT || err == EBADF) {
        return 0;
      }
      verify(rc == 0);
    }
#endif
    return 0;
}

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

// `Epoll` — owns the kqueue/epoll poll fd. Authored as inline-rust DSL: the
// `#if RUSTYCPP_RUST` block is the source of truth; the transpiler regenerates
// the `RUSTYCPP:GEN-BEGIN ... END` C++ below it. `impl Drop` closes the fd; the
// move ctor's `_rusty_forgotten` flag subsumes the old `-1` sentinel and
// prevents a double close. A `rusty::Cell<bool>` field (`owned_`) forces
// copy-`=delete` — load-bearing, since copying the fd would double-close. The
// methods delegate to the `@unsafe` syscall free functions above (their
// `#ifdef USE_KQUEUE` bodies aren't DSL-expressible). `Wait<F>` regenerates as
// a real C++ template member. (Dropped vs the old class: the unused
// `volatile bool* pause/stop` back-pointers, the six dead stat counters, and
// the never-called nullary `Wait()` overload.)
//
// @safe - see comment above.
#if RUSTYCPP_RUST
struct Epoll {
    poll_fd_: i32,
    owned_: rusty::Cell<bool>,
}

impl Epoll {
    // Allocates the poll fd up front (matches the old default ctor).
    #[cpp_ctor]
    fn new() -> Epoll {
        Epoll { poll_fd_: epoll_open(), owned_: rusty::Cell::new(true) }
    }
    fn fd(&self) -> i32 {
        self.poll_fd_
    }
    fn Add(&mut self, fd: i32, poll_mode: i32) -> i32 {
        epoll_add_impl(self.poll_fd_, fd, poll_mode)
    }
    fn Remove(&mut self, fd: i32) -> i32 {
        epoll_remove_impl(self.poll_fd_, fd)
    }
    fn Update(&mut self, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
        epoll_update_impl(self.poll_fd_, fd, new_mode, old_mode)
    }
    fn Wait<F>(&mut self, on_ready: F) {
        epoll_wait_impl(self.poll_fd_, on_ready);
    }
}

impl Drop for Epoll {
    fn drop(&mut self) {
        epoll_close(self.poll_fd_);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_wrapper.epoll version=1 rust_sha256=4786d660ca962a0de6163657556db003ddc3702a7aaf0b96a09aba905dc01ec4*/
struct Epoll;

struct Epoll {
    int32_t poll_fd_;
    rusty::Cell<bool> owned_;
    mutable bool _rusty_forgotten = false;
    Epoll(int32_t poll_fd__init, rusty::Cell<bool> owned__init) : poll_fd_(std::move(poll_fd__init)), owned_(std::move(owned__init)) {}
    Epoll(const Epoll&) = delete;
    Epoll(Epoll&& other) noexcept : poll_fd_(std::move(other.poll_fd_)), owned_(std::move(other.owned_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    Epoll& operator=(const Epoll&) = delete;
    Epoll& operator=(Epoll&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~Epoll();
        new (this) Epoll(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; }


    Epoll();
    int32_t fd() const;
    int32_t Add(int32_t fd, int32_t poll_mode);
    int32_t Remove(int32_t fd);
    int32_t Update(int32_t fd, int32_t new_mode, int32_t old_mode);
    template<typename F>
    void Wait(F on_ready);
    ~Epoll() noexcept(false);
};


Epoll::Epoll()
    : poll_fd_(epoll_open())
    , owned_(rusty::Cell<bool>::new_(true))
{}

int32_t Epoll::fd() const {
    return this->poll_fd_;
}

int32_t Epoll::Add(int32_t fd, int32_t poll_mode) {
    return epoll_add_impl(this->poll_fd_, std::move(fd), std::move(poll_mode));
}

int32_t Epoll::Remove(int32_t fd) {
    return epoll_remove_impl(this->poll_fd_, std::move(fd));
}

int32_t Epoll::Update(int32_t fd, int32_t new_mode, int32_t old_mode) {
    return epoll_update_impl(this->poll_fd_, std::move(fd), std::move(new_mode), std::move(old_mode));
}

template<typename F>
void Epoll::Wait(F on_ready) {
    epoll_wait_impl(this->poll_fd_, std::move(on_ready));
}

Epoll::~Epoll() noexcept(false) {
    if (_rusty_forgotten) { return; }
    epoll_close(this->poll_fd_);
}
/*RUSTYCPP:GEN-END id=epoll_wrapper.epoll*/

} // export namespace rrr
