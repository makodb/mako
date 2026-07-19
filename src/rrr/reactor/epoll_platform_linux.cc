// rrr.epoll_wrapper — Linux implementation unit (Rust std's sys-module
// pattern: CMake selects this file on non-Apple platforms; the kqueue
// twin lives in epoll_platform_kqueue.cc). No preprocessor splits —
// every body here is inline-Rust DSL over route-2 unsafe{} libc calls,
// plus the zeroed-event factory kernel.
module;

#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>

#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

module rrr.epoll_wrapper;

import rrr.debugging;

namespace rrr {

// @unsafe - zeroed epoll_event factory for the DSL bodies below
// (struct-fill / memset has no DSL spelling).
inline struct epoll_event epoll_event_zeroed() {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    return ev;
}

// The Linux epoll_ctl(ADD) body — registration flags, EEXIST
// del-then-re-add retry, and the EBADF teardown-race tolerance — as
// DSL over the zeroed-event factory. The DEL retry passes &ev instead
// of the legacy nullptr (the kernel ignores the payload for DEL;
// the DSL has no null-pointer spelling).
#if RUSTYCPP_RUST
fn epoll_add_impl(poll_fd: i32, fd: i32, poll_mode: i32) -> i32 {
    let mut ev = epoll_event_zeroed();
    ev.data.fd = fd;
    ev.events = EPOLLET | EPOLLIN | EPOLLRDHUP;
    if (poll_mode & PollMode::WRITE) != 0 {
        ev.events |= EPOLLOUT;
    }
    let mut result = unsafe { epoll_ctl(poll_fd, EPOLL_CTL_ADD, fd, &mut ev) };
    if result != 0 && errno == EEXIST {
        unsafe { epoll_ctl(poll_fd, EPOLL_CTL_DEL, fd, &mut ev); }
        result = unsafe { epoll_ctl(poll_fd, EPOLL_CTL_ADD, fd, &mut ev) };
    }
    if result != 0 && errno == EBADF {
        // The fd closed between the registration request and this
        // epoll_ctl (teardown racing an accept/connect registration) —
        // report failure so the caller drops the pollable.
        return -1;
    }
    verify(result == 0);
    0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll.add_impl version=1 rust_sha256=b20e5efeefea7bb5cc54005d442f48ce5122f245f51d413b0c42c754c53878fa*/
int32_t epoll_add_impl(int32_t poll_fd, int32_t fd, int32_t poll_mode);

int32_t epoll_add_impl(int32_t poll_fd, int32_t fd, int32_t poll_mode) {
    auto ev = epoll_event_zeroed();
    ev.data.fd = std::move(fd);
    ev.events = (rusty::detail::deref_if_pointer_like(EPOLLET) | rusty::detail::deref_if_pointer_like(EPOLLIN)) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP);
    if (((rusty::detail::deref_if_pointer_like(poll_mode) & PollMode::WRITE)) != static_cast<int32_t>(0)) {
        rusty::detail::deref_if_pointer_like(ev.events) |= EPOLLOUT;
    }
    auto result = epoll_ctl(std::move(poll_fd), EPOLL_CTL_ADD, std::move(fd), &ev);
    if ((rusty::detail::deref_if_pointer_like(result) != 0) && (rusty::detail::deref_if_pointer_like(errno) == rusty::detail::deref_if_pointer_like(EEXIST))) {
        // @unsafe
        {
            epoll_ctl(std::move(poll_fd), EPOLL_CTL_DEL, std::move(fd), &ev);
        }
        result = epoll_ctl(std::move(poll_fd), EPOLL_CTL_ADD, std::move(fd), &ev);
    }
    if ((rusty::detail::deref_if_pointer_like(result) != 0) && (rusty::detail::deref_if_pointer_like(errno) == rusty::detail::deref_if_pointer_like(EBADF))) {
        return -1;
    }
    verify(rusty::detail::deref_if_pointer_like(result) == 0);
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=epoll.add_impl*/


// The Linux epoll_ctl(DEL) body, authored in the DSL as a route-2
// unsafe{} libc call over the zeroed-event factory.
#if RUSTYCPP_RUST
fn epoll_remove_impl_body(poll_fd: i32, fd: i32) {
    let mut ev = epoll_event_zeroed();
    unsafe { epoll_ctl(poll_fd, EPOLL_CTL_DEL, fd, &mut ev); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll.remove_body version=1 rust_sha256=307cef2652fbaa1fd3e98fc1580c9526bb2dda228fef521509aa92cdbb6fb90e*/
void epoll_remove_impl_body(int32_t poll_fd, int32_t fd);

void epoll_remove_impl_body(int32_t poll_fd, int32_t fd) {
    auto ev = epoll_event_zeroed();
    // @unsafe
    {
        epoll_ctl(std::move(poll_fd), EPOLL_CTL_DEL, std::move(fd), &ev);
    }
}
/*RUSTYCPP:GEN-END id=epoll.remove_body*/


// The Linux epoll_ctl(MOD) body — interest recompute + ENOENT/EBADF
// tolerance (racing close/remove) — as DSL over the zeroed factory.
#if RUSTYCPP_RUST
fn epoll_update_impl_body(poll_fd: i32, fd: i32, new_mode: i32) -> i32 {
    let mut ev = epoll_event_zeroed();
    ev.data.fd = fd;
    ev.events = EPOLLET | EPOLLRDHUP;
    if (new_mode & PollMode::READ) != 0 {
        ev.events |= EPOLLIN;
    }
    if (new_mode & PollMode::WRITE) != 0 {
        ev.events |= EPOLLOUT;
    }
    let rc = unsafe { epoll_ctl(poll_fd, EPOLL_CTL_MOD, fd, &mut ev) };
    if rc != 0 {
        let err: i32 = errno;
        if err == ENOENT || err == EBADF {
            return 0;
        }
        verify(rc == 0);
    }
    0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll.update_body version=1 rust_sha256=60c57347f674a1d43765fe1cbdd1c021debe74f16b3dfb4ce169b51fc51edf3f*/
int32_t epoll_update_impl_body(int32_t poll_fd, int32_t fd, int32_t new_mode);

int32_t epoll_update_impl_body(int32_t poll_fd, int32_t fd, int32_t new_mode) {
    auto ev = epoll_event_zeroed();
    ev.data.fd = std::move(fd);
    ev.events = rusty::detail::deref_if_pointer_like(EPOLLET) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP);
    if (((rusty::detail::deref_if_pointer_like(new_mode) & PollMode::READ)) != static_cast<int32_t>(0)) {
        rusty::detail::deref_if_pointer_like(ev.events) |= EPOLLIN;
    }
    if (((rusty::detail::deref_if_pointer_like(new_mode) & PollMode::WRITE)) != static_cast<int32_t>(0)) {
        rusty::detail::deref_if_pointer_like(ev.events) |= EPOLLOUT;
    }
    const auto rc = epoll_ctl(std::move(poll_fd), EPOLL_CTL_MOD, std::move(fd), &ev);
    if (rusty::detail::deref_if_pointer_like(rc) != 0) {
        const int32_t err = errno;
        if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENOENT)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EBADF))) {
            return static_cast<int32_t>(0);
        }
        verify(rusty::detail::deref_if_pointer_like(rc) == 0);
    }
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=epoll.update_body*/


// Declared-in-interface entry points. add lowers directly; remove and
// update wrap their DSL bodies to keep the interface signatures
// (instrumentation bump / dropped legacy old_mode param).
int32_t epoll_open() {
    int32_t fd = epoll_create(10);
    verify(fd != -1);
    return fd;
}

int epoll_remove_impl(int32_t poll_fd, int fd) {
    epoll_bump_remove_count();
    epoll_remove_impl_body(poll_fd, fd);
    return 0;
}

int epoll_update_impl(int32_t poll_fd, int fd, int new_mode, int old_mode) {
    (void)old_mode;
    return epoll_update_impl_body(poll_fd, fd, new_mode);
}

}  // namespace rrr
