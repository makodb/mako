module;

#include <rusty/arc.hpp>
#include <rusty/rc.hpp>
#include <rusty/rc/weak.hpp>
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
import rrr.debugging;
import rrr.logging;

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

class Pollable {
public:
    virtual ~Pollable() = default;

    virtual int fd() const = 0;
    virtual int poll_mode() const = 0;
    virtual size_t content_size() = 0;
    virtual bool handle_read() = 0;
    virtual int handle_write() = 0;
    virtual void handle_error() = 0;
    virtual void close() = 0;
    virtual bool check_pending_write_update() const = 0;
    virtual bool is_closed() const = 0;
};


class Epoll {
 private:
    int zero_count = 0;
    long have_count = 0;
  long no_count = 0;
    int first = 0;
  long total_have_time = 0;
  long total_no_time = 0;
 public:
  static inline std::atomic<int> remove_count_{0};

  volatile bool* pause;
  volatile bool* stop;

  Epoll() {
#ifdef USE_KQUEUE
    poll_fd_ = kqueue();
#else
    poll_fd_ = epoll_create(10);
#endif
    verify(poll_fd_ != -1);
  }

  Epoll(Epoll&& other) noexcept : poll_fd_(other.poll_fd_) {
    other.poll_fd_ = -1;
  }

  Epoll& operator=(Epoll&& other) noexcept {
    if (this != &other) {
      if (poll_fd_ != -1) {
        ::close(poll_fd_);
      }
      poll_fd_ = other.poll_fd_;
      other.poll_fd_ = -1;
    }
    return *this;
  }

  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  int Add(int fd, int poll_mode) {
#ifdef USE_KQUEUE
    struct kevent ev;
    if (poll_mode & PollMode::READ) {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = EV_ADD;
      ev.filter = EVFILT_READ;
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }
    if (poll_mode & PollMode::WRITE) {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = EV_ADD;
      ev.filter = EVFILT_WRITE;
      verify(kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0);
    }

#else
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.data.fd = fd;
    ev.events = EPOLLET | EPOLLIN | EPOLLRDHUP;

    if (poll_mode & PollMode::WRITE) {
        ev.events |= EPOLLOUT;
    }

    int result = epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
    if (result != 0 && errno == EEXIST) {
        (void)epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        result = epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }
    verify(result == 0);
#endif
    return 0;
  }


  int Remove(int fd) {
    remove_count_++;
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


  int Update(int fd, int new_mode, int old_mode) {
#ifdef USE_KQUEUE
    struct kevent ev;
    auto kqueue_update = [&](int flags, int filter) -> bool {
      bzero(&ev, sizeof(ev));
      ev.ident = fd;
      ev.flags = flags;
      ev.filter = filter;
      if (kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) == 0) return true;
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
    int rc = epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev);
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

  void Wait();

  template<typename ReadyHandler>
  void Wait(ReadyHandler&& on_ready) {
    const int max_nev = 100;
#ifdef USE_KQUEUE
    struct kevent evlist[max_nev];
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 1 * 1000 * 1000;

    int nev = kevent(poll_fd_, nullptr, 0, evlist, max_nev, &timeout);

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
    int nev = epoll_wait(poll_fd_, evlist, max_nev, timeout);
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

  ~Epoll() {
    if (poll_fd_ != -1) {
      ::close(poll_fd_);
    }
  }

  int fd() const { return poll_fd_; }

 private:
  int poll_fd_;
};

} // export namespace rrr

namespace rrr {

void Epoll::Wait() {
    Wait([](int /*fd*/, int /*ready_events*/) {});
}

} // namespace rrr
