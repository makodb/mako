// srpc.epoll_wrapper — Linux implementation unit (Rust std's sys-module
// pattern). This is now the ONLY implementation unit: the kqueue twin
// and every other macOS branch were removed. No preprocessor splits —
// every body here is inline-Rust DSL over route-2 unsafe{} libc calls,
// plus the zeroed-event factory kernel.
module;

#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>

#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

module srpc.epoll_wrapper;

import srpc.debugging;

namespace srpc {

// @safe - the zeroed-epoll_event factory, in DSL. It used to be a plain-C
// kernel in srpc_epoll.c on the reasoning that "memset-then-fill has no DSL
// spelling"; that was true of memset-then-fill, but the factory only needs
// the ZEROING, and `Default::default()` supplies it. That retired the whole
// srpc_epoll.c translation unit -- the last C file in reactor/ -- and with
// it an UNCONDITIONAL macOS build break: CMakeLists added srpc_epoll.c to
// every platform's source list, and it includes <sys/epoll.h>.
//
// Zeroing equivalence was measured, not assumed: `rusty::default_like<
// epoll_event>()` over poisoned (0xAB) storage leaves 0 non-zero bytes and
// memcmp's equal to memset(&ev,0,sizeof ev) -- padding included, which
// matters because the kernel reads the whole union.
#if RUSTYCPP_RUST
fn epoll_event_zeroed() -> epoll_event {
    Default::default()
}

// Constructing the packed Linux event as an aggregate lets the compiler place
// epoll_data_t at the platform ABI's actual offset without ever forming a
// reference to that potentially unaligned union member.
fn epoll_event_with_fd(fd: i32, events: u32) -> epoll_event {
    let mut data: epoll_data_t = Default::default();
    data.fd = fd;
    epoll_event {
        events,
        data,
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=epoll_platform_linux.1 version=1 rust_sha256=8d4be9e03978fcb39f3cdd6c4d19a05d4ffbb153cc2d68d8d0a33c63be176ab7*/
epoll_event epoll_event_zeroed();
epoll_event epoll_event_with_fd(int32_t fd, uint32_t events);

epoll_event epoll_event_zeroed() {
    return rusty::default_like<epoll_event>();
}

epoll_event epoll_event_with_fd(int32_t fd, uint32_t events) {
    epoll_data_t data = rusty::default_like<epoll_data_t>();
    data.fd = std::move(fd);
    return epoll_event{.events = std::move(events), .data = std::move(data)};
}
/*RUSTYCPP:GEN-END id=epoll_platform_linux.1*/

// The Linux epoll_ctl(ADD) body — registration flags, EEXIST
// del-then-re-add retry, and the EBADF teardown-race tolerance — as
// DSL over the fully initialized event factory. The DEL retry passes &ev
// instead of the legacy nullptr; the kernel ignores the payload for DEL, so
// either is correct. (The original reason -- "the DSL has no
// null-pointer spelling" -- is no longer true: `core::ptr::null_mut()`
// lowers to `rusty::ptr::null_mut()`. Passing &ev is kept because it is
// clearer, not because null is unavailable.)
#if RUSTYCPP_RUST
fn epoll_add_impl(poll_fd: i32, fd: i32, poll_mode: i32) -> i32 {
    let mut events: u32 = EPOLLET | EPOLLIN | EPOLLRDHUP;
    if (poll_mode & PollMode::WRITE) != 0 {
        events |= EPOLLOUT;
    }
    let mut ev = epoll_event_with_fd(fd, events);
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
/*RUSTYCPP:GEN-BEGIN id=epoll.add_impl version=1 rust_sha256=bf553f3a39e820e118c0eb779dd8a5df9637d8e9994ecc97bba5b0a10f15dc9c*/
int32_t epoll_add_impl(int32_t poll_fd, int32_t fd, int32_t poll_mode);

int32_t epoll_add_impl(int32_t poll_fd, int32_t fd, int32_t poll_mode) {
    uint32_t events = (rusty::detail::deref_if_pointer_like(EPOLLET) | rusty::detail::deref_if_pointer_like(EPOLLIN)) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP);
    if (((rusty::detail::deref_if_pointer_like(poll_mode) & PollMode::WRITE)) != static_cast<int32_t>(0)) {
        events |= EPOLLOUT;
    }
    auto ev = epoll_event_with_fd(std::move(fd), std::move(events));
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
// declared it for the since-removed kqueue twin. Carrying it here
// is what deleted the hand-written C++ wrapper that used to drop it (a
// named-but-unused C++ parameter does not warn, verified under -Wall).
#if RUSTYCPP_RUST
fn epoll_update_impl(poll_fd: i32, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
    let mut events: u32 = EPOLLET | EPOLLRDHUP;
    if (new_mode & PollMode::READ) != 0 {
        events |= EPOLLIN;
    }
    if (new_mode & PollMode::WRITE) != 0 {
        events |= EPOLLOUT;
    }
    let mut ev = epoll_event_with_fd(fd, events);
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
/*RUSTYCPP:GEN-BEGIN id=epoll.update_body version=1 rust_sha256=f0508ae10695d4d189d623abfe90bd4294a18bb5af4943a90e6bf60c3c6e2cb9*/
int32_t epoll_update_impl(int32_t poll_fd, int32_t fd, int32_t new_mode, int32_t old_mode);

int32_t epoll_update_impl(int32_t poll_fd, int32_t fd, int32_t new_mode, int32_t old_mode) {
    uint32_t events = rusty::detail::deref_if_pointer_like(EPOLLET) | rusty::detail::deref_if_pointer_like(EPOLLRDHUP);
    if (((rusty::detail::deref_if_pointer_like(new_mode) & PollMode::READ)) != static_cast<int32_t>(0)) {
        events |= EPOLLIN;
    }
    if (((rusty::detail::deref_if_pointer_like(new_mode) & PollMode::WRITE)) != static_cast<int32_t>(0)) {
        events |= EPOLLOUT;
    }
    auto ev = epoll_event_with_fd(std::move(fd), std::move(events));
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

}  // namespace srpc
