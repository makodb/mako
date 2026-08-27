/* srpc_fiber.c — see srpc_fiber.h. All raw-memory fiber machinery in
 * plain C: mmap+guard stacks, the ABI context seed, the thread-local
 * active-fiber slot, and the resume/yield/finish state machine.
 * Invariant violations abort (the C++ side's verify did the same).
 */
#include "srpc_fiber.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static _Thread_local srpc_fiber* g_active_fiber = NULL;

/* Runs on the fiber stack; entered via the seeded context. */
static void srpc_fiber_entry_trampoline(void) {
  srpc_fiber* f = g_active_fiber;
  if (f == NULL) {
    abort();
  }
  f->state = SRPC_FIBER_RUNNING;
  f->entry_fn(f->entry_arg);
  f->state = SRPC_FIBER_FINISHED;
  fiber_swap_context(&f->fiber_ctx, &f->caller_ctx);
  abort(); /* a finished fiber must never be resumed past this point */
}

void srpc_fiber_init(srpc_fiber* f, size_t stack_bytes,
                     void (*entry_fn)(void*), void* entry_arg) {
  long page = sysconf(_SC_PAGESIZE);
  size_t page_sz = page > 0 ? (size_t)page : 4096;

  memset(f, 0, sizeof(*f));
  f->state = SRPC_FIBER_NEW;
  f->entry_fn = entry_fn;
  f->entry_arg = entry_arg;

  f->stack_mapping_bytes = stack_bytes + page_sz;
  void* mapping = mmap(NULL, f->stack_mapping_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    abort();
  }
  if (mprotect(mapping, page_sz, PROT_NONE) != 0) {
    abort();
  }
  f->stack_mapping = mapping;

  uintptr_t stack_top =
      (uintptr_t)((char*)f->stack_mapping + f->stack_mapping_bytes);
  stack_top &= ~(uintptr_t)0xF;

#if defined(__x86_64__)
  /* SysV x86_64 ABI: %rsp % 16 == 8 on function entry (simulates a call
   * having pushed the return address). */
  stack_top -= sizeof(void*);
  *(void**)stack_top = NULL;
  f->fiber_ctx.rsp = (void*)stack_top;
  f->fiber_ctx.rip = (void*)&srpc_fiber_entry_trampoline;
#elif defined(__aarch64__)
  /* AAPCS64: sp 16-byte aligned on entry; trampoline entered via ret
   * from pc (= lr). */
  f->fiber_ctx.sp = (void*)stack_top;
  f->fiber_ctx.pc = (void*)&srpc_fiber_entry_trampoline;
#endif
}

void srpc_fiber_destroy(srpc_fiber* f) {
  if (f->stack_mapping != NULL) {
    if (munmap(f->stack_mapping, f->stack_mapping_bytes) != 0) {
      abort();
    }
    f->stack_mapping = NULL;
    f->stack_mapping_bytes = 0;
  }
}

void srpc_fiber_resume(srpc_fiber* f) {
  if (f->state == SRPC_FIBER_FINISHED) {
    return;
  }
  srpc_fiber* old = g_active_fiber;
  g_active_fiber = f;
  fiber_swap_context(&f->caller_ctx, &f->fiber_ctx);
  g_active_fiber = old;
}

void srpc_fiber_yield(srpc_fiber* f) {
  if (f->state != SRPC_FIBER_RUNNING) {
    abort();
  }
  f->state = SRPC_FIBER_SUSPENDED;
  fiber_swap_context(&f->fiber_ctx, &f->caller_ctx);
  if (f->state != SRPC_FIBER_FINISHED) {
    f->state = SRPC_FIBER_RUNNING;
  }
}

/* ---- reactor platform / build-configuration facade (see header) ---- */

int64_t srpc_reactor_gettid(void) {
  /* <sys/syscall.h> supplies SYS_gettid for the target being compiled.
   * Never hardcode the number: it differs per architecture. */
  return (int64_t)syscall(SYS_gettid);
}

int32_t srpc_reactor_reusing_fiber(void) {
  /* The historical reactor.h predicate, verbatim. This TU is compiled with
   * the library's own flags, so the answer is the library's real
   * configuration rather than a constant frozen into portable source. */
#if defined(REUSE_FIBER) || defined(REUSE_CORO)
  return 1;
#else
  return 0;
#endif
}
