//
// Created by shuai on 8/22/18.
//
#pragma once

#include "base/all.hpp"
#include <rusty/arc.hpp>
#include <rusty/rc.hpp>
#include <rusty/rc/weak.hpp>
#include <rusty/refcell.hpp>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <memory>
#include <vector>
#include <cerrno>

#ifdef __APPLE__
#define USE_KQUEUE
#endif

#ifdef USE_KQUEUE
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif


namespace rrr {
using std::shared_ptr;

// Forward declaration
class PollThreadWorker;

// Pollable mode constants (moved outside interface for @interface compliance)
namespace PollMode {
    static constexpr int READ = 0x1;
    static constexpr int WRITE = 0x2;
    // Special return value for handle_write() indicating no mode change needed
    static constexpr int NO_CHANGE = -1;
}

// @interface
class Pollable {
public:
    virtual ~Pollable() = default;

    // @safe
    virtual int fd() const = 0;
    // @safe
    virtual int poll_mode() const = 0;
    // @safe
    virtual size_t content_size() = 0;
    // @safe
    virtual bool handle_read() = 0;
    // @safe
    virtual int handle_write() = 0;
    // @safe
    virtual void handle_error() = 0;
    // @safe
    virtual void close() = 0;
    // @safe
    virtual bool check_pending_write_update() const = 0;
    // @safe
    virtual bool is_closed() const = 0;
};


// @unsafe - Wrapper for epoll/kqueue system calls
// SAFETY: Proper file descriptor management and error checking
class Epoll {
 private:
	int zero_count = 0;
	long have_count = 0;
  long no_count = 0;
	int first = 0;
  long total_have_time = 0;
  long total_no_time = 0;
 public:
  // For testing: track number of Remove() calls (static for persistence)
  static inline std::atomic<int> remove_count_{0};

  // Control flags for pause/stop functionality (jetpack)
  volatile bool* pause;
  volatile bool* stop;

  // @unsafe - Creates epoll/kqueue file descriptor
  // SAFETY: Verifies creation succeeded
  Epoll() {
#ifdef USE_KQUEUE
    poll_fd_ = kqueue();
#else
    poll_fd_ = epoll_create(10);    // arg ignored, any value > 0 will do
#endif
    verify(poll_fd_ != -1);
  }

  // Move constructor - transfers ownership of poll_fd
  Epoll(Epoll&& other) noexcept : poll_fd_(other.poll_fd_) {
    other.poll_fd_ = -1;  // Prevent double-close
  }

  // Move assignment - transfers ownership of poll_fd
  Epoll& operator=(Epoll&& other) noexcept {
    if (this != &other) {
      if (poll_fd_ != -1) {
        close(poll_fd_);
      }
      poll_fd_ = other.poll_fd_;
      other.poll_fd_ = -1;  // Prevent double-close
    }
    return *this;
  }

  // Delete copy constructor and copy assignment
  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  // @unsafe - Adds file descriptor to epoll/kqueue
  // SAFETY: Uses system calls with proper error checking, Arc for polymorphism
  // userdata is raw Pollable* for lookup
  int Add(const rusty::Arc<Pollable>& poll, void* userdata) {
    auto poll_mode = poll->poll_mode();
    auto fd = poll->fd();
#ifdef USE_KQUEUE
    struct kevent ev;
    if (poll_mode & PollMode::READ) {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = EV_ADD;
      ev.filter = EVFILT_READ;
      ev.udata = userdata;  // Store slot index instead of raw pointer
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }
    if (poll_mode & PollMode::WRITE) {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = EV_ADD;
      ev.filter = EVFILT_WRITE;
      ev.udata = userdata;  // Store slot index instead of raw pointer
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }

#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.ptr = userdata;  // Store slot index instead of raw pointer
    ev.events = EPOLLET | EPOLLIN | EPOLLRDHUP; // EPOLLERR and EPOLLHUP are included by default

    if (poll_mode & PollMode::WRITE) {
        ev.events |= EPOLLOUT;
    }

    // Try to add the fd to epoll
    // If it already exists (fd reuse after reconnection), remove first then add
    int result = epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
    if (result != 0 && errno == EEXIST) {
        // fd already registered (possible due to fd reuse after close+reconnect)
        // Remove it first, then add again
        (void)epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        result = epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }
    verify(result == 0);
#endif
    return 0;
  }


  // @unsafe - Removes file descriptor from epoll/kqueue
  // SAFETY: Uses system calls, ignores errors for already removed fds, Arc for polymorphism
  int Remove(const rusty::Arc<Pollable>& poll) {
    remove_count_++;  // Track Remove() calls for testing
    auto fd = poll->fd();
#ifdef USE_KQUEUE
    struct kevent ev;

    bzero(&ev, sizeof(ev));
    ev.ident = fd;
    ev.flags = EV_DELETE;
    ev.filter = EVFILT_READ;
    kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
    bzero(&ev, sizeof(ev));
    ev.ident = fd;
    ev.flags = EV_DELETE;
    ev.filter = EVFILT_WRITE;
    kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);

#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, &ev);
#endif
    return 0;
  }


  // @unsafe - Updates poll mode for file descriptor
  // SAFETY: Uses system calls with proper event flag handling
  // userdata is raw Pollable* for lookup
  int Update(const Pollable& poll, void* userdata, int new_mode, int old_mode) {
    auto fd = poll.fd();
#ifdef USE_KQUEUE
    struct kevent ev;
    if ((new_mode & PollMode::READ) && !(old_mode & PollMode::READ)) {
      // add READ
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.udata = userdata;  // Store slot index instead of raw pointer
      ev.flags = EV_ADD;
      ev.filter = EVFILT_READ;
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }
    if (!(new_mode & PollMode::READ) && (old_mode & PollMode::READ)) {
      // del READ
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.udata = userdata;  // Store slot index instead of raw pointer
      ev.flags = EV_DELETE;
      ev.filter = EVFILT_READ;
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }
    if ((new_mode & PollMode::WRITE) && !(old_mode & PollMode::WRITE)) {
      // add WRITE
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.udata = userdata;  // Store slot index instead of raw pointer
      ev.flags = EV_ADD;
      ev.filter = EVFILT_WRITE;
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }
    if (!(new_mode & PollMode::WRITE) && (old_mode & PollMode::WRITE)) {
      // del WRITE
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.udata = userdata;  // Store slot index instead of raw pointer
      ev.flags = EV_DELETE;
      ev.filter = EVFILT_WRITE;
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }
#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.ptr = userdata;  // Store slot index instead of raw pointer
    ev.events = EPOLLET | EPOLLRDHUP;
    if (new_mode & PollMode::READ) {
        ev.events |= EPOLLIN;
    }
    if (new_mode & PollMode::WRITE) {
        ev.events |= EPOLLOUT;
    }
    int rc = epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev);
    if (rc != 0) {
      int err = errno;
      // When a transport closes a socket in a different thread (e.g. during shutdown),
      // the poller may still try to update its mode. Treat a missing or already-closed
      // descriptor as a benign race so shutdown can proceed without aborting.
      if (err == ENOENT || err == EBADF) {
        return 0;
      }
      verify(rc == 0);
    }
#endif
    return 0;
  }

  // Jetpack split-phase Wait (declaration - implementation in epoll_wrapper.cc)
  void Wait();

  // @unsafe - Waits for events and dispatches to handlers directly (mako-dev template version)
  // SAFETY: Uses system calls with timeout, raw pointer safe due to deferred removal
  // userdata is Pollable* - safe to use directly because object remains in fd_to_pollable_ map
  // ModeUpdater: callable with signature void(Pollable*, int new_mode)
  template<typename ModeUpdater>
  void Wait(ModeUpdater&& update_mode) {
    const int max_nev = 100;
#ifdef USE_KQUEUE
    struct kevent evlist[max_nev];
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 1 * 1000 * 1000; // 0.001 sec

    int nev = kevent(poll_fd_, nullptr, 0, evlist, max_nev, &timeout);

    for (int i = 0; i < nev; i++) {
      void* userdata = evlist[i].udata;
      Pollable* poll = reinterpret_cast<Pollable*>(userdata);  // Direct cast - safe!

      if (evlist[i].filter == EVFILT_READ) {
        poll->handle_read();
      }
      if (evlist[i].filter == EVFILT_WRITE) {
        int new_mode = poll->handle_write();
        if (new_mode != PollMode::NO_CHANGE) {
          update_mode(poll, new_mode);
        }
      }

      // handle error after handle IO, so that we can at least process something
      if (evlist[i].flags & EV_EOF) {
        poll->handle_error();
      }
    }

#else
    struct epoll_event evlist[max_nev];
    int timeout = 1; // milli, 0.001 sec
//    int timeout = 0; // busy loop
    //Log_info("epoll::wait entering here....");
    int nev = epoll_wait(poll_fd_, evlist, max_nev, timeout);
    //Log_info("epoll::wait exiting here.....");
    //Log_info("number of events are %d", nev);
    for (int i = 0; i < nev; i++) {
      //Log_info("number of events are %d", nev);
      void* userdata = evlist[i].data.ptr;

      // Skip if userdata is nullptr (used for internal wakeup events like channel eventfd)
      if (userdata == nullptr) {
        continue;
      }

      Pollable* poll = reinterpret_cast<Pollable*>(userdata);  // Direct cast - safe!

      if (evlist[i].events & EPOLLIN) {
          poll->handle_read();
      }
      if (evlist[i].events & EPOLLOUT) {
          int new_mode = poll->handle_write();
          if (new_mode != PollMode::NO_CHANGE) {
            update_mode(poll, new_mode);
          }
      }
      // handle error after handle IO, so that we can at least process something
      if (evlist[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          poll->handle_error();
      }
    }
#endif
  }

  ~Epoll() {
    if (poll_fd_ != -1) {
      close(poll_fd_);
    }
  }

  // Public accessor for the epoll/kqueue file descriptor
  int fd() const { return poll_fd_; }

 private:
  int poll_fd_;
};

} // namespace rrr
