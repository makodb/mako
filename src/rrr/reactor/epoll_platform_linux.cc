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

// The zeroed-epoll_event factory lives in srpc_epoll.c now (plain C,
// Goal-0 C demotion): memset-then-fill has no DSL spelling and needs no
// C++ either, and `struct epoll_event` is a libc POD so returning one by
// value is ABI-identical across the boundary.
extern "C" struct epoll_event srpc_epoll_event_zeroed(void);

// @unsafe - thin shim over the C kernel, keeping the name the DSL bodies
// below already call.
inline struct epoll_event epoll_event_zeroed() {
    return srpc_epoll_event_zeroed();
}

// The Linux epoll_ctl(ADD) body — registration flags, EEXIST
// del-then-re-add retry, and the EBADF teardown-race tolerance — as
// DSL over the zeroed-event factory. The DEL retry passes &ev instead
// of the legacy nullptr; the kernel ignores the payload for DEL, so
// either is correct. (The original reason -- "the DSL has no
// null-pointer spelling" -- is no longer true: `core::ptr::null_mut()`
// lowers to `rusty::ptr::null_mut()`. Passing &ev is kept because it is
// clearer, not because null is unavailable.)
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
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.data); }) { return (__r.data); } else if constexpr (requires { (__r.data_field); }) { return (__r.data_field); } else if constexpr (requires { ((*__r).data); }) { return ((*__r).data); } else { return ((*__r).data_field); } }(ev).fd = std::move(fd);
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.events); }) { return (__r.events); } else if constexpr (requires { (__r.events_field); }) { return (__r.events_field); } else if constexpr (requires { ((*__r).events); }) { return ((*__r).events); } else { return ((*__r).events_field); } }(ev) = (rusty::detail::deref_if_pointer_like(EPOLLET) | rusty::detail::deref_if_pointer_like(EPOLLIN)) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP);
    if (((rusty::detail::deref_if_pointer_like(poll_mode) & PollMode::WRITE)) != static_cast<int32_t>(0)) {
        rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.events); }) { return (__r.events); } else if constexpr (requires { (__r.events_field); }) { return (__r.events_field); } else if constexpr (requires { ((*__r).events); }) { return ((*__r).events); } else { return ((*__r).events_field); } }(ev)) |= EPOLLOUT;
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


// The Linux epoll_ctl(DEL) entry point, authored in the DSL as a
// route-2 unsafe{} libc call over the zeroed-event factory. The
// test-instrumentation bump is simply the first statement — it used to
// need a hand-written C++ wrapper around an `_body` helper, which is
// gone; this fn now IS the interface-declared `epoll_remove_impl`.
#if RUSTYCPP_RUST
fn epoll_remove_impl(poll_fd: i32, fd: i32) -> i32 {
    epoll_bump_remove_count();
    let mut ev = epoll_event_zeroed();
    unsafe { epoll_ctl(poll_fd, EPOLL_CTL_DEL, fd, &mut ev); }
    0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll.remove_body version=1 rust_sha256=bf41f3b7b04a267b150c532eee695bea96704263990944684f309d95ae383a65*/
int32_t epoll_remove_impl(int32_t poll_fd, int32_t fd);

int32_t epoll_remove_impl(int32_t poll_fd, int32_t fd) {
    epoll_bump_remove_count();
    auto ev = epoll_event_zeroed();
    // @unsafe
    {
        epoll_ctl(std::move(poll_fd), EPOLL_CTL_DEL, std::move(fd), &ev);
    }
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=epoll.remove_body*/


// The Linux epoll_ctl(MOD) entry point — interest recompute +
// ENOENT/EBADF tolerance (racing close/remove) — as DSL over the zeroed
// factory. `old_mode` is unused on Linux (EPOLL_CTL_MOD replaces the
// whole interest set) but stays in the signature: the shared interface
// declares it because the kqueue twin really needs it. Carrying it here
// is what deleted the hand-written C++ wrapper that used to drop it (a
// named-but-unused C++ parameter does not warn, verified under -Wall).
#if RUSTYCPP_RUST
fn epoll_update_impl(poll_fd: i32, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
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
/*RUSTYCPP:GEN-BEGIN id=epoll.update_body version=1 rust_sha256=6085e4a717dd73e91409795df2ae858d779595b7358cda614b969a1933b3ad74*/
int32_t epoll_update_impl(int32_t poll_fd, int32_t fd, int32_t new_mode, int32_t old_mode);

int32_t epoll_update_impl(int32_t poll_fd, int32_t fd, int32_t new_mode, int32_t old_mode) {
    auto ev = epoll_event_zeroed();
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.data); }) { return (__r.data); } else if constexpr (requires { (__r.data_field); }) { return (__r.data_field); } else if constexpr (requires { ((*__r).data); }) { return ((*__r).data); } else { return ((*__r).data_field); } }(ev).fd = std::move(fd);
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.events); }) { return (__r.events); } else if constexpr (requires { (__r.events_field); }) { return (__r.events_field); } else if constexpr (requires { ((*__r).events); }) { return ((*__r).events); } else { return ((*__r).events_field); } }(ev) = rusty::detail::deref_if_pointer_like(EPOLLET) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP);
    if (((rusty::detail::deref_if_pointer_like(new_mode) & PollMode::READ)) != static_cast<int32_t>(0)) {
        rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.events); }) { return (__r.events); } else if constexpr (requires { (__r.events_field); }) { return (__r.events_field); } else if constexpr (requires { ((*__r).events); }) { return ((*__r).events); } else { return ((*__r).events_field); } }(ev)) |= EPOLLIN;
    }
    if (((rusty::detail::deref_if_pointer_like(new_mode) & PollMode::WRITE)) != static_cast<int32_t>(0)) {
        rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.events); }) { return (__r.events); } else if constexpr (requires { (__r.events_field); }) { return (__r.events_field); } else if constexpr (requires { ((*__r).events); }) { return ((*__r).events); } else { return ((*__r).events_field); } }(ev)) |= EPOLLOUT;
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


// The remaining interface-declared entry point: allocate the epoll poll
// fd. (`epoll_create`'s size hint has been ignored since Linux 2.6.8 but
// must still be positive.)
#if RUSTYCPP_RUST
fn epoll_open() -> i32 {
    let fd: i32 = unsafe { epoll_create(10) };
    verify(fd != -1);
    fd
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_platform_linux.4 version=1 rust_sha256=b1154281fae5d352306e0bbd69605a4e64c3ca767658d54faeaa69a14154cff0*/
int32_t epoll_open();

int32_t epoll_open() {
    int32_t fd = epoll_create(10);
    verify(rusty::detail::deref_if_pointer_like(fd) != -1);
    return std::move(fd);
}
/*RUSTYCPP:GEN-END id=epoll_platform_linux.4*/

}  // namespace rrr
