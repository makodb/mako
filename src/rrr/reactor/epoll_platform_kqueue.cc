// rrr.epoll_wrapper — kqueue (Apple) implementation unit. Selected by
// CMake on APPLE; not compiled (or verified) on Linux CI — same status
// as the former #ifdef branches. Bodies are hand-written C++ (kevent
// struct-fill).
module;

#include <string.h>
#include <strings.h>
#include <sys/event.h>
#include <unistd.h>

module rrr.epoll_wrapper;

import rrr.debugging;

namespace rrr {

int32_t epoll_open() {
    int32_t fd = kqueue();
    verify(fd != -1);
    return fd;
}

int epoll_add_impl(int32_t poll_fd, int fd, int poll_mode) {
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

    return 0;
}

int epoll_remove_impl(int32_t poll_fd, int fd) {
    epoll_bump_remove_count();
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
    return 0;
}

int epoll_update_impl(int32_t poll_fd, int fd, int new_mode, int old_mode) {
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
    return 0;
}

}  // namespace rrr
