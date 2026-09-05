/* srpc_fiber.h — the stackful-fiber engine as plain C (Goal-0 fiber-API
 * C demotion). Everything the fiber runtime does with raw memory lives
 * here: the arch-specific callee-saved register bag (its field offsets
 * are the ABI contract with fiber_context_{x86_64,aarch64}.S), the
 * mmap+guard-page stack, the thread-local active-fiber slot, and the
 * resume/yield/finish state machine. The C++ side supplies exactly one
 * callback: entry_fn(entry_arg), invoked on the fiber stack.
 */
#ifndef SRPC_FIBER_H
#define SRPC_FIBER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field order/sizes mirror the .S offsets exactly — do not reorder. */
typedef struct srpc_fiber_ctx {
#if defined(__x86_64__)
  void* rsp;
  void* rip;
  uintptr_t rbx;
  uintptr_t rbp;
  uintptr_t r12;
  uintptr_t r13;
  uintptr_t r14;
  uintptr_t r15;
#elif defined(__aarch64__)
  /* AAPCS64 callee-saved: x19-x28, x29 (fp), x30 (lr used as pc), sp. */
  /* offset 0 */
  void* sp;
  /* offset 8 (lr on entry = resume address) */
  void* pc;
  /* offset 16 */
  uintptr_t x19;
  /* offset 24 */
  uintptr_t x20;
  /* offset 32 */
  uintptr_t x21;
  /* offset 40 */
  uintptr_t x22;
  /* offset 48 */
  uintptr_t x23;
  /* offset 56 */
  uintptr_t x24;
  /* offset 64 */
  uintptr_t x25;
  /* offset 72 */
  uintptr_t x26;
  /* offset 80 */
  uintptr_t x27;
  /* offset 88 */
  uintptr_t x28;
  /* offset 96 (x29) */
  uintptr_t fp;
#endif
} srpc_fiber_ctx;

/* Implemented in fiber_context_<arch>.S. */
void fiber_swap_context(srpc_fiber_ctx* from, srpc_fiber_ctx* to);

enum {
  SRPC_FIBER_NEW = 0,
  SRPC_FIBER_RUNNING = 1,
  SRPC_FIBER_SUSPENDED = 2,
  SRPC_FIBER_FINISHED = 3
};

typedef struct srpc_fiber {
  srpc_fiber_ctx caller_ctx;
  srpc_fiber_ctx fiber_ctx;
  void* stack_mapping;
  size_t stack_mapping_bytes;
  int state;
  void (*entry_fn)(void* arg);
  void* entry_arg;
} srpc_fiber;

/* mmap the stack (+1 guard page), seed the context so the first resume
 * enters the trampoline, store the entry callback. Aborts on mmap /
 * mprotect failure (mirrors the old verify semantics). */
void srpc_fiber_init(srpc_fiber* f, size_t stack_bytes,
                     void (*entry_fn)(void*), void* entry_arg);

/* munmap the stack; safe on an already-destroyed fiber. */
void srpc_fiber_destroy(srpc_fiber* f);

/* Switch onto the fiber (no-op if FINISHED). Saves/restores the
 * thread-local active-fiber slot around the swap. */
void srpc_fiber_resume(srpc_fiber* f);

/* Called ON the fiber: RUNNING -> SUSPENDED, swap back to the caller.
 * On the next resume, execution continues here (back to RUNNING). */
void srpc_fiber_yield(srpc_fiber* f);

/* ---- reactor platform / build-configuration facade -------------------
 *
 * Canonical Rust cannot read a C preprocessor macro: crate mode compiles
 * the same source under rustc, where `SYS_gettid` and `REUSE_FIBER` do
 * not exist. Spelling them as Rust constants bakes one target's syscall
 * table and one build's configuration into the source. Both facts are
 * therefore answered by this C translation unit, which is compiled by the
 * same build, with the same flags, against the same platform headers as
 * the rest of libsrpc.
 */

/* The calling OS thread's kernel thread id: syscall(SYS_gettid) using
 * <sys/syscall.h>'s own number for the target being compiled. That number
 * is arch-specific (186 on x86-64, 178 on aarch64, 224 on i386) and this
 * directory already ships an aarch64 context-switch trampoline, so it must
 * never be written down in portable source. */
int64_t srpc_reactor_gettid(void);

/* Non-zero when this library was compiled with fiber reuse enabled.
 * Exactly the historical `REUSING_FIBER` predicate,
 *     #if defined(REUSE_FIBER) || defined(REUSE_CORO)
 * evaluated in a translation unit that actually sees the build's flags. */
int32_t srpc_reactor_reusing_fiber(void);

#ifdef __cplusplus
}
#endif

#endif /* SRPC_FIBER_H */
