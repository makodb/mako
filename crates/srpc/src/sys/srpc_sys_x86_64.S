/* Thunks over the libc syscall wrappers.
 *
 * ONE source of truth, assembled by both toolchains: Rust via
 * global_asm!(include_str!(...)), the C++ build directly. The same
 * arrangement as runtime/fiber/fiber_x86_64.S.
 *
 * WHY THESE EXIST. The crate cannot declare `connect`, `bind`, `accept`
 * or `recv` itself: <sys/socket.h> IS reachable in the emitted C++, so a
 * self-declaration collides with libc's ("conflicting types for
 * 'connect'"). Renaming on the Rust side alone does not help — the
 * transpiler does not read #[link_name] — so the rename has to be real,
 * and a one-instruction tail call is the cheapest real rename there is.
 *
 * A thunk is a no-op for a symbol that would NOT have collided, so the
 * whole surface goes through one mechanism rather than half of it going
 * through two.
 *
 * `jmp` rather than `call`: a tail call leaves the caller's frame and
 * return address in place, so the callee returns straight to our caller.
 * Nothing to save, nothing to restore, no stack adjustment — which is
 * also why this is correct for the variadic `fcntl` (we always pass
 * integer args, and %al is already 0 from the Rust side).
 */
    .text

.macro THUNK name
    .globl srpc_\name
    .type srpc_\name,@function
srpc_\name:
    jmp \name@PLT
    .size srpc_\name,.-srpc_\name
.endm

    THUNK socket
    THUNK connect
    THUNK bind
    THUNK listen
    THUNK accept
    THUNK shutdown
    THUNK recv
    THUNK send
    THUNK setsockopt
    THUNK getsockopt
    THUNK fcntl
    THUNK poll
    THUNK close
    THUNK mmap
    THUNK mprotect
    THUNK munmap
    THUNK epoll_create
    THUNK epoll_ctl
    THUNK epoll_wait

    .section .note.GNU-stack,"",@progbits
